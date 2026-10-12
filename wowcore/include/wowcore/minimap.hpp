// Portable local-map display math. The host converts Skyrim coordinates into east/north map units first.
// Positions use doubles so small offsets remain accurate far from the world origin. No engine/GPU ownership.
#pragma once

#include <optional>
#include <istream>
#include <map>
#include <string>
#include <vector>

namespace wowcore {
struct MapPoint {
    double x = 0, y = 0;   // east/right, north/up
};

struct MinimapView {
    MapPoint player;
    double radius = 0;    // map units visible from centre to rim
    double heading = 0;   // radians clockwise from map north
    bool rotate = false;
};

struct MinimapCapture {
    MapPoint origin;      // map position corresponding to uvOrigin
    MapPoint uvOrigin{0.5, 0.5};
    MapPoint eastUv;      // UV change for one map unit east
    MapPoint northUv;     // UV change for one map unit north (includes texture Y convention)
};

// Five steps matching the current widget API. Scale is a proposal, not a measured 1.12 yard table.
std::optional<double> minimapRadius(double baseRadius, int zoom);

// Disc coordinates: centre (0,0), unit rim, +Y up. Rotation is shared by terrain, markers and pings.
std::optional<MapPoint> minimapProject(const MinimapView& view, MapPoint position);
std::optional<MapPoint> minimapUnproject(const MinimapView& view, MapPoint disc);
std::optional<MapPoint> minimapArrow(const MinimapView& view);
bool minimapContains(MapPoint disc);

// Inverse mapping for a textured disc vertex. Rejects outside-mask / outside-source samples; never wraps UVs.
std::optional<MapPoint> minimapSampleUv(const MinimapView& view, const MinimapCapture& capture, MapPoint disc);
// Whether the WHOLE disc fits the captured image, with an optional UV inset for the sampler's texel footprint.
bool minimapCaptureContains(const MinimapView& view, const MinimapCapture& capture, MapPoint uvInset = {});

// Blip centre relative to the widget centre in physical pixels, +Y down. All four glyph corners must fit.
std::optional<MapPoint> minimapBlip(const MinimapView& view, MapPoint position,
                                  double pixelRadius, MapPoint glyphHalfSize);

// Static terrain atlas. Tile keys are Skyrim CELL origins, including worldspaces
// whose four-cell LOD grid is offset from zero. All display positions are yards.
struct MinimapTile {
    MapPoint origin;
    double span = 256;
    std::string texture;
};
struct MinimapWorld {
    MapPoint low, high;
    std::string overviewTexture; // optional atlas-derived full-map image; same world bounds
    std::map<std::pair<int, int>, MinimapTile> tiles;
    const MinimapTile* at(MapPoint position) const;
    std::vector<const MinimapTile*> around(const MinimapView& view) const;
    std::optional<MapPoint> mapPosition(MapPoint position) const; // stable world bounds; Y down
};
class MinimapAtlas {
public:
    static MinimapAtlas read(std::istream& index); // tools/skyrim_minimap.py index.tsv; throws on invalid data
    const MinimapWorld* world(std::string name) const; // case insensitive editor ID, not localized name
private:
    std::map<std::string, MinimapWorld> worlds_;
};
struct MinimapVertex { MapPoint disc, uv; };
struct MinimapPoiPlacement { MapPoint centre; bool arrow; double heading; };
// In-range icons keep their position; distant targets remain on the rim. Inset is in disc units.
std::optional<MinimapPoiPlacement> minimapPoi(const MinimapView& view, MapPoint position, double inset);
// Upright screen-space glyph, clipped to the disc even when its centre is at the rim.
std::vector<MinimapVertex> minimapGlyphTriangles(MapPoint centre, MapPoint half, MapPoint uvLow, MapPoint uvHigh,
                                               double heading = 0);
// Intersect one projected tile with a 64-sided disc. Triangle list; UVs clamped
// to texel centres so both the repeat CPU sampler and clamp D3D sampler agree.
std::vector<MinimapVertex> minimapTileTriangles(const MinimapView& view, const MinimapTile& tile,
                                              const std::vector<const MinimapTile*>& covering = {});
}  // namespace wowcore
