#include "wowcore/worldmap.hpp"
#include <algorithm>
#include <cmath>

namespace wowcore {
namespace {
bool finite(MapPoint p) { return std::isfinite(p.x) && std::isfinite(p.y); }
bool valid(const WorldMapView& v) {
    return finite(v.low) && finite(v.high) && v.high.x > v.low.x && v.high.y > v.low.y &&
        std::isfinite(v.high.x - v.low.x) && std::isfinite(v.high.y - v.low.y);
}
struct Rect { MapPoint low, high; };
bool nonempty(const Rect& r) { return r.low.x < r.high.x && r.low.y < r.high.y; }
Rect intersection(Rect a, Rect b) {
    return {{std::max(a.low.x,b.low.x),std::max(a.low.y,b.low.y)},
            {std::min(a.high.x,b.high.x),std::min(a.high.y,b.high.y)}};
}
}
std::optional<MapPoint> worldMapProject(const WorldMapView& v, MapPoint p) {
    if (!valid(v) || !finite(p)) return std::nullopt;
    const MapPoint result{(p.x-v.low.x)/(v.high.x-v.low.x), (v.high.y-p.y)/(v.high.y-v.low.y)};
    return finite(result) ? std::optional{result} : std::nullopt;
}
std::optional<MapPoint> worldMapUnproject(const WorldMapView& v, MapPoint p) {
    if (!valid(v) || !finite(p)) return std::nullopt;
    const MapPoint result{v.low.x+p.x*(v.high.x-v.low.x), v.high.y-p.y*(v.high.y-v.low.y)};
    return finite(result) ? std::optional{result} : std::nullopt;
}
bool worldMapContains(MapPoint p) { return finite(p) && p.x>=0 && p.y>=0 && p.x<=1 && p.y<=1; }

std::vector<WorldMapVertex> worldMapRectTriangles(const WorldMapView& v, const WorldMapView& textureBounds,
                                               MapPoint textureSize, const std::vector<const MinimapTile*>& covering) {
    std::vector<WorldMapVertex> result;
    if (!valid(v) || !valid(textureBounds) || !finite(textureSize) || textureSize.x<1 || textureSize.y<1) return result;
    const Rect bounds{textureBounds.low,textureBounds.high};
    std::vector<Rect> pieces{intersection(bounds,{v.low,v.high})};
    for (const auto* cover : covering) {
        if (!cover || !finite(cover->origin) || !std::isfinite(cover->span) || cover->span<=0) continue;
        const Rect covered{cover->origin,{cover->origin.x+cover->span,cover->origin.y+cover->span}};
        std::vector<Rect> remaining;
        for (const auto& piece : pieces) {
            const auto cut=intersection(piece,covered);
            if (!nonempty(cut)) { remaining.push_back(piece); continue; }
            for (const auto& r : {Rect{piece.low,{cut.low.x,piece.high.y}},
                Rect{{cut.high.x,piece.low.y},piece.high},
                Rect{{cut.low.x,piece.low.y},{cut.high.x,cut.low.y}},
                Rect{{cut.low.x,cut.high.y},{cut.high.x,piece.high.y}}})
                if (nonempty(r)) remaining.push_back(r);
        }
        pieces=std::move(remaining);
    }
    const auto vertex=[&](MapPoint p) {
        return WorldMapVertex{*worldMapProject(v,p),
            {std::clamp((p.x-bounds.low.x)/(bounds.high.x-bounds.low.x),0.5/textureSize.x,1-0.5/textureSize.x),
             std::clamp((bounds.high.y-p.y)/(bounds.high.y-bounds.low.y),0.5/textureSize.y,1-0.5/textureSize.y)}};
    };
    for (const auto& r : pieces) if (nonempty(r)) {
        const MapPoint p[]={{r.low.x,r.high.y},r.high,{r.high.x,r.low.y},r.low};
        for (const int i : {0,1,2,0,2,3}) result.push_back(vertex(p[i]));
    }
    return result;
}
std::vector<WorldMapVertex> worldMapTileTriangles(const WorldMapView& v, const MinimapTile& tile,
                                               const std::vector<const MinimapTile*>& covering) {
    return worldMapRectTriangles(v,{tile.origin,{tile.origin.x+tile.span,tile.origin.y+tile.span}}, {256,256},covering);
}
}
