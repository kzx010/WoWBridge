#pragma once
#include "wowcore/minimap.hpp"

namespace wowcore {
// A north-up rectangular viewport in yards. Projection is normalized, east
// right and north at Y=0; terrain, client POIs and player use these same bounds.
struct WorldMapView { MapPoint low, high; };
std::optional<MapPoint> worldMapProject(const WorldMapView& view, MapPoint position);
std::optional<MapPoint> worldMapUnproject(const WorldMapView& view, MapPoint position);
bool worldMapContains(MapPoint position);
struct WorldMapVertex { MapPoint position, uv; };
std::vector<WorldMapVertex> worldMapRectTriangles(const WorldMapView& view, const WorldMapView& textureBounds,
                                               MapPoint textureSize,
                                               const std::vector<const MinimapTile*>& covering = {});
// Rectangular clipping, with nearer child tiles subtracted from parent tiles.
std::vector<WorldMapVertex> worldMapTileTriangles(const WorldMapView& view, const MinimapTile& tile,
                                               const std::vector<const MinimapTile*>& covering = {});
}
