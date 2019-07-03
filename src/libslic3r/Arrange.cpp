#include "Arrange.hpp"
#include "Geometry.hpp"
#include "SVG.hpp"
#include "MTUtils.hpp"

#include <libnest2d/backends/clipper/geometries.hpp>
#include <libnest2d/optimizers/nlopt/subplex.hpp>
#include <libnest2d/placers/nfpplacer.hpp>
#include <libnest2d/selections/firstfit.hpp>

#include <numeric>
#include <ClipperUtils.hpp>

#include <boost/geometry/index/rtree.hpp>
#include <boost/multiprecision/integer.hpp>
#include <boost/rational.hpp>

namespace libnest2d {
#if !defined(_MSC_VER) && defined(__SIZEOF_INT128__) && !defined(__APPLE__)
using LargeInt = __int128;
#else
using LargeInt = boost::multiprecision::int128_t;
template<> struct _NumTag<LargeInt>
{
    using Type = ScalarTag;
};
#endif

template<class T> struct _NumTag<boost::rational<T>>
{
    using Type = RationalTag;
};

namespace nfp {

template<class S> struct NfpImpl<S, NfpLevel::CONVEX_ONLY>
{
    NfpResult<S> operator()(const S &sh, const S &other)
    {
        return nfpConvexOnly<S, boost::rational<LargeInt>>(sh, other);
    }
};

} // namespace nfp
} // namespace libnest2d

namespace Slic3r {

template<class Tout = double, class = FloatingOnly<Tout>>
inline SLIC3R_CONSTEXPR EigenVec<Tout, 2> unscaled(
    const ClipperLib::IntPoint &v) SLIC3R_NOEXCEPT
{
    return EigenVec<Tout, 2>{unscaled<Tout>(v.X), unscaled<Tout>(v.Y)};
}

namespace arrangement {

using namespace libnest2d;
namespace clppr = ClipperLib;

// Get the libnest2d types for clipper backend
using Item         = _Item<clppr::Polygon>;
using Box          = _Box<clppr::IntPoint>;
using Circle       = _Circle<clppr::IntPoint>;
using Segment      = _Segment<clppr::IntPoint>;
using MultiPolygon = TMultiShape<clppr::Polygon>;

// The return value of nesting, a vector (for each logical bed) of Item
// reference vectors. 
using PackGroup    = _PackGroup<clppr::Polygon>;

// Summon the spatial indexing facilities from boost
namespace bgi = boost::geometry::index;
using SpatElement = std::pair<Box, unsigned>;
using SpatIndex = bgi::rtree< SpatElement, bgi::rstar<16, 4> >;
using ItemGroup = std::vector<std::reference_wrapper<Item>>;

// A coefficient used in separating bigger items and smaller items.
const double BIG_ITEM_TRESHOLD = 0.02;

// Fill in the placer algorithm configuration with values carefully chosen for
// Slic3r.
template<class PConf>
void fillConfig(PConf& pcfg) {

    // Align the arranged pile into the center of the bin
    pcfg.alignment = PConf::Alignment::CENTER;

    // Start placing the items from the center of the print bed
    pcfg.starting_point = PConf::Alignment::CENTER;

    // TODO cannot use rotations until multiple objects of same geometry can
    // handle different rotations.
    pcfg.rotations = { 0.0 };

    // The accuracy of optimization.
    // Goes from 0.0 to 1.0 and scales performance as well
    pcfg.accuracy = 0.65f;
    
    // Allow parallel execution.
    pcfg.parallel = true;
}

// Apply penality to object function result. This is used only when alignment
// after arrange is explicitly disabled (PConfig::Alignment::DONT_ALIGN)
double fixed_overfit(const std::tuple<double, Box>& result, const Box &binbb)
{
    double score = std::get<0>(result);
    Box pilebb  = std::get<1>(result);
    Box fullbb  = sl::boundingBox(pilebb, binbb);
    double diff = fullbb.area() - binbb.area();
    if(diff > 0) score += diff;
    
    return score;
}

// A class encapsulating the libnest2d Nester class and extending it with other
// management and spatial index structures for acceleration.
template<class TBin>
class AutoArranger {
public:
    // Useful type shortcuts...
    using Placer = typename placers::_NofitPolyPlacer<clppr::Polygon, TBin>;
    using Selector = selections::_FirstFitSelection<clppr::Polygon>;
    using Packer   = Nester<Placer, Selector>;
    using PConfig  = typename Packer::PlacementConfig;
    using Distance = TCoord<PointImpl>;

protected:
    Packer    m_pck;
    PConfig   m_pconf; // Placement configuration
    TBin      m_bin;
    double    m_bin_area;
    SpatIndex m_rtree; // spatial index for the normal (bigger) objects
    SpatIndex m_smallsrtree;    // spatial index for only the smaller items
    double    m_norm;           // A coefficient to scale distances
    MultiPolygon m_merged_pile; // The already merged pile (vector of items)
    Box          m_pilebb;      // The bounding box of the merged pile.
    ItemGroup m_remaining; // Remaining items (m_items at the beginning)
    ItemGroup m_items;     // The items to be packed

    // This is "the" object function which is evaluated many times for each
    // vertex (decimated with the accuracy parameter) of each object.
    // Therefore it is upmost crucial for this function to be as efficient
    // as it possibly can be but at the same time, it has to provide
    // reasonable results.
    std::tuple<double /*score*/, Box /*farthest point from bin center*/>
    objfunc(const Item &item, const clppr::IntPoint &bincenter)
    {
        const double bin_area = m_bin_area;
        const SpatIndex& spatindex = m_rtree;
        const SpatIndex& smalls_spatindex = m_smallsrtree;
        const ItemGroup& remaining = m_remaining;
        
        // We will treat big items (compared to the print bed) differently
        auto isBig = [bin_area](double a) {
            return a/bin_area > BIG_ITEM_TRESHOLD ;
        };
        
        // Candidate item bounding box
        auto ibb = item.boundingBox();
        
        // Calculate the full bounding box of the pile with the candidate item
        auto fullbb = sl::boundingBox(m_pilebb, ibb);
        
        // The bounding box of the big items (they will accumulate in the center
        // of the pile
        Box bigbb;
        if(spatindex.empty()) bigbb = fullbb;
        else {
            auto boostbb = spatindex.bounds();
            boost::geometry::convert(boostbb, bigbb);
        }
        
        // Will hold the resulting score
        double score = 0;
        
        // Density is the pack density: how big is the arranged pile
        double density = 0;
        
        const double N = m_norm;
        auto norm = [N](double val) { return val / N; };
        
        // Distinction of cases for the arrangement scene
        enum e_cases {
            // This branch is for big items in a mixed (big and small) scene
            // OR for all items in a small-only scene.
            BIG_ITEM,
            
            // This branch is for the last big item in a mixed scene
            LAST_BIG_ITEM,
            
            // For small items in a mixed scene.
            SMALL_ITEM
        } compute_case;
        
        bool bigitems = isBig(item.area()) || spatindex.empty();
        if(bigitems && !remaining.empty()) compute_case = BIG_ITEM;
        else if (bigitems && remaining.empty()) compute_case = LAST_BIG_ITEM;
        else compute_case = SMALL_ITEM;
        
        switch (compute_case) {
        case BIG_ITEM: {
            const clppr::IntPoint& minc = ibb.minCorner(); // bottom left corner
            const clppr::IntPoint& maxc = ibb.maxCorner(); // top right corner

            // top left and bottom right corners
            clppr::IntPoint top_left{getX(minc), getY(maxc)};
            clppr::IntPoint bottom_right{getX(maxc), getY(minc)};

            // Now the distance of the gravity center will be calculated to the
            // five anchor points and the smallest will be chosen.
            std::array<double, 5> dists;
            auto cc = fullbb.center(); // The gravity center
            dists[0] = pl::distance(minc, cc);
            dists[1] = pl::distance(maxc, cc);
            dists[2] = pl::distance(ibb.center(), cc);
            dists[3] = pl::distance(top_left, cc);
            dists[4] = pl::distance(bottom_right, cc);

            // The smalles distance from the arranged pile center:
            double dist = norm(*(std::min_element(dists.begin(), dists.end())));
            double bindist = norm(pl::distance(ibb.center(), bincenter));
            dist = 0.8 * dist + 0.2*bindist;

            // Prepare a variable for the alignment score.
            // This will indicate: how well is the candidate item
            // aligned with its neighbors. We will check the alignment
            // with all neighbors and return the score for the best
            // alignment. So it is enough for the candidate to be
            // aligned with only one item.
            auto alignment_score = 1.0;

            auto query = bgi::intersects(ibb);
            auto& index = isBig(item.area()) ? spatindex : smalls_spatindex;

            // Query the spatial index for the neighbors
            std::vector<SpatElement> result;
            result.reserve(index.size());

            index.query(query, std::back_inserter(result));

            // now get the score for the best alignment
            for(auto& e : result) { 
                auto idx = e.second;
                Item& p = m_items[idx];
                auto parea = p.area();
                if(std::abs(1.0 - parea/item.area()) < 1e-6) {
                    auto bb = sl::boundingBox(p.boundingBox(), ibb);
                    auto bbarea = bb.area();
                    auto ascore = 1.0 - (item.area() + parea)/bbarea;

                    if(ascore < alignment_score) alignment_score = ascore;
                }
            }

            density = std::sqrt(norm(fullbb.width()) * norm(fullbb.height()));

            // The final mix of the score is the balance between the
            // distance from the full pile center, the pack density and
            // the alignment with the neighbors
            if (result.empty())
                score = 0.5 * dist + 0.5 * density;
            else
                score = 0.40 * dist + 0.40 * density + 0.2 * alignment_score;
            
            break;
        }
        case LAST_BIG_ITEM: {
            auto mp = m_merged_pile;
            mp.emplace_back(item.transformedShape());
            auto chull = sl::convexHull(mp);
    
            placers::EdgeCache<clppr::Polygon> ec(chull);
            
            double circ  = norm(ec.circumference());
            double bcirc = 2.0 * norm(fullbb.width() + fullbb.height());
            score = 0.5 * circ + 0.5 * bcirc;
            break;
        }
        case SMALL_ITEM: {
            // Here there are the small items that should be placed around the
            // already processed bigger items.
            // No need to play around with the anchor points, the center will be
            // just fine for small items
            score = norm(pl::distance(ibb.center(), bigbb.center()));
            break;
        }            
        }
        
        return std::make_tuple(score, fullbb);
    }
    
    std::function<double(const Item&)> get_objfn();
    
public:
    AutoArranger(const TBin &                  bin,
                 Distance                      dist,
                 std::function<void(unsigned)> progressind,
                 std::function<bool(void)>     stopcond)
        : m_pck(bin, dist)
        , m_bin(bin)
        , m_bin_area(sl::area(bin))
        , m_norm(std::sqrt(m_bin_area))
    {
        fillConfig(m_pconf);

        // Set up a callback that is called just before arranging starts
        // This functionality is provided by the Nester class (m_pack).
        m_pconf.before_packing =
        [this](const MultiPolygon& merged_pile,            // merged pile
               const ItemGroup& items,             // packed items
               const ItemGroup& remaining)         // future items to be packed
        {
            m_items = items;
            m_merged_pile = merged_pile;
            m_remaining = remaining;

            m_pilebb = sl::boundingBox(merged_pile);

            m_rtree.clear();
            m_smallsrtree.clear();
            
            // We will treat big items (compared to the print bed) differently
            auto isBig = [this](double a) {
                return a / m_bin_area > BIG_ITEM_TRESHOLD ;
            };

            for(unsigned idx = 0; idx < items.size(); ++idx) {
                Item& itm = items[idx];
                if(isBig(itm.area())) m_rtree.insert({itm.boundingBox(), idx});
                m_smallsrtree.insert({itm.boundingBox(), idx});
            }
        };
        
        m_pconf.object_function = get_objfn();
        
        if (progressind) m_pck.progressIndicator(progressind);
        if (stopcond) m_pck.stopCondition(stopcond);
        
        m_pck.configure(m_pconf);
    }
    
    template<class...Args> inline PackGroup operator()(Args&&...args) {
        m_rtree.clear();
        return m_pck.execute(std::forward<Args>(args)...);
    }
    
    inline void preload(std::vector<Item>& fixeditems) {
        m_pconf.alignment = PConfig::Alignment::DONT_ALIGN;
        auto bb = sl::boundingBox(m_bin);
        auto bbcenter = bb.center();
        m_pconf.object_function = [this, bb, bbcenter](const Item &item) {
            return fixed_overfit(objfunc(item, bbcenter), bb);
        };

        // Build the rtree for queries to work
        
        for(unsigned idx = 0; idx < fixeditems.size(); ++idx) {
            Item& itm = fixeditems[idx];
            itm.markAsFixed();
            m_rtree.insert({itm.boundingBox(), idx});
        }

        m_pck.configure(m_pconf);
    }

    bool is_colliding(const Item& item) {
        if(m_rtree.empty()) return false;
        std::vector<SpatElement> result;
        m_rtree.query(bgi::intersects(item.boundingBox()),
                      std::back_inserter(result));
        return !result.empty();
    }
};

template<> std::function<double(const Item&)> AutoArranger<Box>::get_objfn()
{
    auto bincenter = m_bin.center();

    return [this, bincenter](const Item &itm) {
        auto result = objfunc(itm, bincenter);
        
        double score = std::get<0>(result);
        auto& fullbb = std::get<1>(result);
        
        double miss = Placer::overfit(fullbb, m_bin);
        miss = miss > 0? miss : 0;
        score += miss*miss;
        
        return score;    
    };
}

template<> std::function<double(const Item&)> AutoArranger<Circle>::get_objfn()
{
    auto bincenter = m_bin.center();
    return [this, bincenter](const Item &item) {
        
        auto result = objfunc(item, bincenter);
        
        double score = std::get<0>(result);
        
        auto isBig = [this](const Item& itm) {
            return itm.area() / m_bin_area > BIG_ITEM_TRESHOLD ;
        };
        
        if(isBig(item)) {
            auto mp = m_merged_pile;
            mp.push_back(item.transformedShape());
            auto chull = sl::convexHull(mp);
            double miss = Placer::overfit(chull, m_bin);
            if(miss < 0) miss = 0;
            score += miss*miss;
        }
        
        return score;
    };
}

// Specialization for a generalized polygon.
// Warning: this is unfinished business. It may or may not work.
template<>
std::function<double(const Item &)> AutoArranger<clppr::Polygon>::get_objfn()
{
    auto bincenter = sl::boundingBox(m_bin).center();
    return [this, bincenter](const Item &item) {
        return std::get<0>(objfunc(item, bincenter));
    };
}

inline Circle to_lnCircle(const CircleBed& circ) {
    return Circle({circ.center()(0), circ.center()(1)}, circ.radius());
}

// Get the type of bed geometry from a simple vector of points.
BedShapeHint bedShape(const Polyline &bed) {
    BedShapeHint ret;

    auto x = [](const Point& p) { return p(X); };
    auto y = [](const Point& p) { return p(Y); };

    auto width = [x](const BoundingBox& box) {
        return x(box.max) - x(box.min);
    };

    auto height = [y](const BoundingBox& box) {
        return y(box.max) - y(box.min);
    };

    auto area = [&width, &height](const BoundingBox& box) {
        double w = width(box);
        double h = height(box);
        return w * h;
    };

    auto poly_area = [](Polyline p) {
        Polygon pp; pp.points.reserve(p.points.size() + 1);
        pp.points = std::move(p.points);
        pp.points.emplace_back(pp.points.front());
        return std::abs(pp.area());
    };

    auto distance_to = [x, y](const Point& p1, const Point& p2) {
        double dx = x(p2) - x(p1);
        double dy = y(p2) - y(p1);
        return std::sqrt(dx*dx + dy*dy);
    };

    auto bb = bed.bounding_box();

    auto isCircle = [bb, distance_to](const Polyline& polygon) {
        auto center = bb.center();
        std::vector<double> vertex_distances;
        double avg_dist = 0;
        for (auto pt: polygon.points)
        {
            double distance = distance_to(center, pt);
            vertex_distances.push_back(distance);
            avg_dist += distance;
        }

        avg_dist /= vertex_distances.size();

        CircleBed ret(center, avg_dist);
        for(auto el : vertex_distances)
        {
            if (std::abs(el - avg_dist) > 10 * SCALED_EPSILON) {
                ret = CircleBed();
                break;
            }
        }

        return ret;
    };

    auto parea = poly_area(bed);

    if( (1.0 - parea/area(bb)) < 1e-3 ) {
        ret.type = BedShapeType::BOX;
        ret.shape.box = bb;
    }
    else if(auto c = isCircle(bed)) {
        ret.type = BedShapeType::CIRCLE;
        ret.shape.circ = c;
    } else {
        ret.type = BedShapeType::IRREGULAR;
        ret.shape.polygon = bed;
    }

    // Determine the bed shape by hand
    return ret;
}

template<class BinT> // Arrange for arbitrary bin type
PackGroup _arrange(std::vector<Item> &           shapes,
                   std::vector<Item> &           excludes,
                   const BinT &                  bin,
                   coord_t                       minobjd,
                   std::function<void(unsigned)> prind,
                   std::function<bool()>         stopfn)
{   
    AutoArranger<BinT> arranger{bin, minobjd, prind, stopfn};
    
    auto it = excludes.begin();
    while (it != excludes.end())
        sl::isInside(it->transformedShape(), bin) ?
            ++it : it = excludes.erase(it);

    // If there is something on the plate
    if(!excludes.empty())
        {
            arranger.preload(excludes);
            auto binbb = sl::boundingBox(bin);

            // Try to put the first item to the center, as the arranger
            // will not do this for us.
            for (auto it = shapes.begin(); it != shapes.end(); ++it) {
                Item &itm = *it;
                auto  ibb = itm.boundingBox();
                auto  d   = binbb.center() - ibb.center();
                itm.translate(d);

                if (!arranger.is_colliding(itm)) {
                    itm.markAsFixed();

                    // Write the transformation data into the item. The
                    // callback was set on the instantiation of Item and
                    // calls the Arrangeable interface.
                    it->callApplyFunction(0);

                    // Remove this item, as it is arranged now
                    it = shapes.erase(it);
                    break;
                }
            }
        }

    std::vector<std::reference_wrapper<Item>> inp;
    inp.reserve(shapes.size() + excludes.size());
    for (auto &itm : shapes  ) inp.emplace_back(itm);
    for (auto &itm : excludes) inp.emplace_back(itm);
    
    return arranger(inp.begin(), inp.end());
}

inline SLIC3R_CONSTEXPR coord_t stride_padding(coord_t w)
{
    return w + w / 5;
}

// The final client function for arrangement. A progress indicator and
// a stop predicate can be also be passed to control the process.
bool arrange(ArrangeablePtrs &             arrangables,
             const ArrangeablePtrs &       excludes,
             coord_t                       min_obj_distance,
             const BedShapeHint &          bedhint,
             std::function<void(unsigned)> progressind,
             std::function<bool()>         stopcondition)
{
    bool ret = true;
    namespace clppr = ClipperLib;
    
    std::vector<Item> items, fixeditems;
    items.reserve(arrangables.size());
    coord_t binwidth = 0;

    auto process_arrangeable =
        [](const Arrangeable *                         arrangeable,
           std::vector<Item> &                         outp,
           std::function<void(const Item &, unsigned)> applyfn)
    {
        assert(arrangeable);

        auto arrangeitem = arrangeable->get_arrange_polygon();

        Polygon &      p        = std::get<0>(arrangeitem);
        const Vec2crd &offs     = std::get<1>(arrangeitem);
        double         rotation = std::get<2>(arrangeitem);

        if (p.is_counter_clockwise()) p.reverse();

        clppr::Polygon clpath(Slic3rMultiPoint_to_ClipperPath(p));

        auto firstp = clpath.Contour.front();
        clpath.Contour.emplace_back(firstp);

        outp.emplace_back(applyfn, std::move(clpath));
        outp.back().rotation(rotation);
        outp.back().translation({offs.x(), offs.y()});
    };

    for (Arrangeable *arrangeable : arrangables) {
        process_arrangeable(
            arrangeable,
            items,
            // callback called by arrange to apply the result on the arrangeable
            [arrangeable, &binwidth](const Item &itm, unsigned binidx) {
                clppr::cInt stride = binidx * stride_padding(binwidth);

                clppr::IntPoint offs = itm.translation();
                arrangeable->apply_arrange_result({unscaled(offs.X + stride),
                                                   unscaled(offs.Y)},
                                                  itm.rotation());
            });
    }
    
    for (const Arrangeable * fixed: excludes)
        process_arrangeable(fixed, fixeditems, nullptr);
    
    // Integer ceiling the min distance from the bed perimeters
    coord_t md = min_obj_distance - SCALED_EPSILON;
    md = (md % 2) ? md / 2 + 1 : md / 2;
    
    auto& cfn = stopcondition;
    
    switch (bedhint.type) {
//    case BedShapeType::BOX: {
//        // Create the arranger for the box shaped bed
//        BoundingBox bbb = bedhint.shape.box;
//        bbb.min -= Point{md, md}, bbb.max += Point{md, md};
//        Box binbb{{bbb.min(X), bbb.min(Y)}, {bbb.max(X), bbb.max(Y)}};
//        binwidth = coord_t(binbb.width());
        
//        _arrange(items, fixeditems, binbb, min_obj_distance, progressind, cfn);
//        break;
//    }
//    case BedShapeType::CIRCLE: {
//        auto c  = bedhint.shape.circ;
//        auto cc = to_lnCircle(c);
//        binwidth = scaled(c.radius());
        
//        _arrange(items, fixeditems, cc, min_obj_distance, progressind, cfn);
//        break;
//    }
//    case BedShapeType::IRREGULAR: {
//        auto ctour = Slic3rMultiPoint_to_ClipperPath(bedhint.shape.polygon);
//        auto irrbed = sl::create<clppr::Polygon>(std::move(ctour));
//        BoundingBox polybb(bedhint.shape.polygon);
//        binwidth = (polybb.max(X) - polybb.min(X));
        
//        _arrange(items, fixeditems, irrbed, min_obj_distance, progressind, cfn);
//        break;
//    }
//    case BedShapeType::INFINITE: {
//        const InfiniteBed& nobin = bedhint.shape.infinite;
//        Box infbb{{nobin.center.x(), nobin.center.y()}};
        
//        _arrange(items, fixeditems, infbb, min_obj_distance, progressind, cfn);
//        break;
//    }
//    case BedShapeType::UNKNOWN: {
//        // We know nothing about the bed, let it be infinite and zero centered 
//        _arrange(items, fixeditems, Box{}, min_obj_distance, progressind, cfn);
//        break;
//    }
    default: {
        Box infbb = Box::infinite({bedhint.shape.box.center().x(), bedhint.shape.box.center().y()});

        _arrange(items, fixeditems, infbb, min_obj_distance, progressind, cfn);
        break;
    }
    };
    
    if(stopcondition()) return false;

    return ret;
}

// Arrange, without the fixed items (excludes)
bool arrange(ArrangeablePtrs &                inp,
             coord_t                       min_d,
             const BedShapeHint &          bedhint,
             std::function<void(unsigned)> prfn,
             std::function<bool()>         stopfn)
{
    return arrange(inp, {}, min_d, bedhint, prfn, stopfn);
}

} // namespace arr
} // namespace Slic3r
