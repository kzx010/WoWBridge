#include "wowcore/minimap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>

namespace wowcore {
namespace {
bool finite(MapPoint p) { return std::isfinite(p.x) && std::isfinite(p.y); }
bool valid(const MinimapView& v) {
    return finite(v.player) && std::isfinite(v.radius) && v.radius > 0 && std::isfinite(v.heading);
}
bool valid(const MinimapCapture& c) {
    // Reject a collapsed projection; there must be two independent map axes.
    const double determinant = c.eastUv.x * c.northUv.y - c.eastUv.y * c.northUv.x;
    return finite(c.origin) && finite(c.uvOrigin) && finite(c.eastUv) && finite(c.northUv) &&
           std::isfinite(determinant) && determinant != 0;
}
MapPoint sample(const MinimapCapture& c, MapPoint p) {
    const double x = p.x - c.origin.x, y = p.y - c.origin.y;
    return {c.uvOrigin.x + x * c.eastUv.x + y * c.northUv.x,
            c.uvOrigin.y + x * c.eastUv.y + y * c.northUv.y};
}
}  // namespace

std::optional<double> minimapRadius(double baseRadius, int zoom) {
    if (!std::isfinite(baseRadius) || baseRadius <= 0) return std::nullopt;
    const double radius = baseRadius * std::pow(0.5, std::clamp(zoom, 0, 4) * 0.5);
    return radius > 0 ? std::optional<double>(radius) : std::nullopt;
}

std::optional<MapPoint> minimapProject(const MinimapView& v, MapPoint p) {
    if (!valid(v) || !finite(p)) return std::nullopt;
    const double x = (p.x - v.player.x) / v.radius, y = (p.y - v.player.y) / v.radius;
    const double angle = v.rotate ? v.heading : 0, cs = std::cos(angle), sn = std::sin(angle);
    const MapPoint disc{cs * x - sn * y, sn * x + cs * y};
    return finite(disc) ? std::optional<MapPoint>(disc) : std::nullopt;
}

std::optional<MapPoint> minimapUnproject(const MinimapView& v, MapPoint d) {
    if (!valid(v) || !finite(d)) return std::nullopt;
    const double angle = v.rotate ? v.heading : 0, cs = std::cos(angle), sn = std::sin(angle);
    const MapPoint p{v.player.x + v.radius * (cs * d.x + sn * d.y),
                     v.player.y + v.radius * (-sn * d.x + cs * d.y)};
    return finite(p) ? std::optional<MapPoint>(p) : std::nullopt;
}

std::optional<MapPoint> minimapArrow(const MinimapView& v) {
    if (!valid(v)) return std::nullopt;
    const double angle = v.rotate ? 0 : v.heading;
    return MapPoint{std::sin(angle), std::cos(angle)};
}

bool minimapContains(MapPoint d) { return finite(d) && std::hypot(d.x, d.y) <= 1; }

std::optional<MapPoint> minimapSampleUv(const MinimapView& v, const MinimapCapture& c, MapPoint d) {
    if (!valid(c) || !minimapContains(d)) return std::nullopt;
    const auto p = minimapUnproject(v, d);
    if (!p) return std::nullopt;
    const auto uv = sample(c, *p);
    if (!finite(uv) || uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) return std::nullopt;
    return uv;
}

bool minimapCaptureContains(const MinimapView& v, const MinimapCapture& c, MapPoint inset) {
    if (!valid(v) || !valid(c) || !finite(inset) || inset.x < 0 || inset.y < 0 ||
        inset.x >= 0.5 || inset.y >= 0.5) return false;
    const auto uv = sample(c, v.player);
    // Extremum of a linear form over a circle: radius * length of its coefficients.
    // This works for native cameras whose UV axes are rotated or have different scales.
    const double rx = v.radius * std::hypot(c.eastUv.x, c.northUv.x);
    const double ry = v.radius * std::hypot(c.eastUv.y, c.northUv.y);
    return finite(uv) && std::isfinite(rx) && std::isfinite(ry) &&
           uv.x - rx >= inset.x && uv.x + rx <= 1 - inset.x &&
           uv.y - ry >= inset.y && uv.y + ry <= 1 - inset.y;
}

std::optional<MapPoint> minimapBlip(const MinimapView& v, MapPoint p,
                                  double pixelRadius, MapPoint half) {
    if (!std::isfinite(pixelRadius) || pixelRadius <= 0 || !finite(half) || half.x < 0 || half.y < 0)
        return std::nullopt;
    const auto d = minimapProject(v, p);
    if (!d) return std::nullopt;
    const MapPoint pixels{d->x * pixelRadius, -d->y * pixelRadius};
    if (!finite(pixels) || std::hypot(std::abs(pixels.x) + half.x, std::abs(pixels.y) + half.y) > pixelRadius)
        return std::nullopt;
    return pixels;
}

namespace {
std::string lower(std::string name) {
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}
bool identifier(const std::string& s) {
    return !s.empty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}
}  // namespace

MinimapAtlas MinimapAtlas::read(std::istream& index) {
    std::string line;
    if (!std::getline(index, line) || line != "WOWBRIDGE_MINIMAP 1 64 16384")
        throw std::runtime_error("unsupported minimap index");
    MinimapAtlas atlas;
    while (std::getline(index, line)) {
        std::istringstream row(line);
        std::string name, file, extra;
        int x = 0, y = 0;
        if (!(row >> name >> x >> y >> file) || row >> extra || !identifier(name) ||
            std::abs(static_cast<double>(x)) > 100000 || std::abs(static_cast<double>(y)) > 100000 ||
            file != "WB_" + name + "_" + std::to_string(x) + "_" + std::to_string(y) + ".blp")
            throw std::runtime_error("invalid minimap tile row: " + line);
        auto& w = atlas.worlds_[name];
        const MapPoint origin{x * 64.0, y * 64.0};
        if (w.tiles.empty()) {
            w.low = origin;
            w.high = {origin.x + 256, origin.y + 256};
        } else {
            const auto first = w.tiles.begin()->first;
            if ((x - first.first) % 4 || (y - first.second) % 4)
                throw std::runtime_error("inconsistent minimap tile grid: " + name);
            w.low = {std::min(w.low.x, origin.x), std::min(w.low.y, origin.y)};
            w.high = {std::max(w.high.x, origin.x + 256), std::max(w.high.y, origin.y + 256)};
        }
        w.overviewTexture = "Interface\\Minimap\\WB_" + name + "_overview.blp";
        if (!w.tiles.emplace(std::pair{x, y}, MinimapTile{origin, 256, "Interface\\Minimap\\" + file}).second)
            throw std::runtime_error("duplicate minimap tile: " + line);
    }
    if (index.bad() || atlas.worlds_.empty()) throw std::runtime_error("empty/unreadable minimap index");
    return atlas;
}

const MinimapWorld* MinimapAtlas::world(std::string name) const {
    const auto it = worlds_.find(lower(std::move(name)));
    return it == worlds_.end() ? nullptr : &it->second;
}

const MinimapTile* MinimapWorld::at(MapPoint p) const {
    if (!mapPosition(p)) return nullptr;
    const int x = static_cast<int>(low.x / 64) + static_cast<int>(std::floor((p.x - low.x) / 256)) * 4;
    const int y = static_cast<int>(low.y / 64) + static_cast<int>(std::floor((p.y - low.y) / 256)) * 4;
    const auto it = tiles.find({x, y});
    return it == tiles.end() ? nullptr : &it->second;
}

std::optional<MapPoint> MinimapWorld::mapPosition(MapPoint p) const {
    if (!finite(p) || tiles.empty() || p.x < low.x || p.y < low.y || p.x >= high.x || p.y >= high.y)
        return std::nullopt;
    return MapPoint{(p.x - low.x) / (high.x - low.x), (high.y - p.y) / (high.y - low.y)};
}

std::vector<const MinimapTile*> MinimapWorld::around(const MinimapView& v) const {
    std::vector<const MinimapTile*> result;
    if (!valid(v) || v.radius > 4096 || tiles.empty()) return result;
    // A circle has the same world-space bounding box at every heading. Iterate
    // only that small neighbourhood, including a tile just touching the rim.
    const double left = std::max(low.x, v.player.x - v.radius), right = std::min(high.x, v.player.x + v.radius);
    const double bottom = std::max(low.y, v.player.y - v.radius), top = std::min(high.y, v.player.y + v.radius);
    if (left >= right || bottom >= top) return result;
    const int firstX = static_cast<int>(low.x / 64) + static_cast<int>(std::floor((left - low.x) / 256)) * 4;
    const int firstY = static_cast<int>(low.y / 64) + static_cast<int>(std::floor((bottom - low.y) / 256)) * 4;
    const int lastX = static_cast<int>(low.x / 64) + static_cast<int>(std::floor((right - low.x) / 256)) * 4;
    const int lastY = static_cast<int>(low.y / 64) + static_cast<int>(std::floor((top - low.y) / 256)) * 4;
    for (int x = firstX; x <= lastX; x += 4) for (int y = firstY; y <= lastY; y += 4) {
        const auto it = tiles.find({x, y});
        if (it != tiles.end()) result.push_back(&it->second);
    }
    return result;
}

std::optional<MinimapPoiPlacement> minimapPoi(const MinimapView& view, MapPoint position, double inset) {
    const auto d = minimapProject(view, position);
    if (!d || !std::isfinite(inset) || inset < 0 || inset >= 1) return std::nullopt;
    const double distance = std::hypot(d->x, d->y);
    if (distance <= 1) return MinimapPoiPlacement{*d, false, 0};
    return MinimapPoiPlacement{{d->x / distance * (1 - inset), d->y / distance * (1 - inset)},
                               true, std::atan2(d->x, d->y)};
}

namespace {
// The inscribed 64-sided mask's corners, once.
const std::array<MapPoint, 64>& rim() {
    static const auto points = [] {
        std::array<MapPoint, 64> p;
        for (int i = 0; i < 64; ++i) p[i] = {std::cos(i * 2 * std::numbers::pi / 64), std::sin(i * 2 * std::numbers::pi / 64)};
        return p;
    }();
    return points;
}

// A convex polygon clipped to the inscribed 64-sided mask; untouched when it lies inside the mask's inner circle.
std::vector<MapPoint> clipToDisc(std::vector<MapPoint> polygon) {
    static const double inner = std::cos(std::numbers::pi / 64);
    if (std::ranges::all_of(polygon, [](MapPoint p) { return p.x * p.x + p.y * p.y <= inner * inner; })) return polygon;
    for (int edge = 0; edge < 64 && !polygon.empty(); ++edge) {
        const MapPoint from = rim()[edge], to = rim()[(edge + 1) % 64];
        const auto distance = [&](MapPoint p) { return (to.x - from.x) * (p.y - from.y) - (to.y - from.y) * (p.x - from.x); };
        std::vector<MapPoint> clipped;
        for (size_t i = 0; i < polygon.size(); ++i) {
            const auto p = polygon[i], q = polygon[(i + 1) % polygon.size()];
            const double dp = distance(p), dq = distance(q);
            if (dp >= 0) clipped.push_back(p);
            if ((dp < 0 && dq > 0) || (dp > 0 && dq < 0)) {
                const double t = dp / (dp - dq);
                clipped.push_back({p.x + t * (q.x - p.x), p.y + t * (q.y - p.y)});
            }
        }
        polygon = std::move(clipped);
    }
    return polygon;
}

void fan(const std::vector<MapPoint>& polygon, std::vector<MapPoint>& out) {
    for (size_t i = 1; i + 1 < polygon.size(); ++i) {
        out.push_back(polygon[0]); out.push_back(polygon[i]); out.push_back(polygon[i + 1]);
    }
}
}

std::vector<MapPoint> minimapDiscTriangles() {
    std::vector<MapPoint> out;
    fan({rim().begin(), rim().end()}, out);
    return out;
}

MinimapFloorMesh minimapFloorMesh(const MinimapView& v, const std::vector<MinimapFloorTriangle>& floor, double z,
                                  double wallHalfWidth) {
    MinimapFloorMesh mesh;
    if (!valid(v) || !std::isfinite(z) || !std::isfinite(wallHalfWidth) || wallHalfWidth < 0) return mesh;
    for (const auto& t : floor) {
        if (!std::isfinite(t.z) || std::abs(t.z - z) > 3 + std::max(t.rise, 0.0)) continue;   // another storey
        const auto a = minimapProject(v, t.a), b = minimapProject(v, t.b), c = minimapProject(v, t.c);
        if (!a || !b || !c) continue;
        if (std::min({a->x, b->x, c->x}) > 1 || std::max({a->x, b->x, c->x}) < -1 ||
            std::min({a->y, b->y, c->y}) > 1 || std::max({a->y, b->y, c->y}) < -1) continue;
        // Counter-clockwise, as the clipper keeps the inside of each rim edge.
        const bool ccw = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x) >= 0;
        fan(clipToDisc(ccw ? std::vector<MapPoint>{*a, *b, *c} : std::vector<MapPoint>{*a, *c, *b}), mesh.floor);
        const std::array<MapPoint, 3> corner = {*a, *b, *c};
        for (int e = 0; e < 3; ++e) {
            if (!(t.walls & (1u << e)) || wallHalfWidth <= 0) continue;
            const auto p = corner[e], q = corner[(e + 1) % 3];
            const double length = std::hypot(q.x - p.x, q.y - p.y);
            if (length <= 0) continue;
            const MapPoint n{-(q.y - p.y) / length * wallHalfWidth, (q.x - p.x) / length * wallHalfWidth};
            fan(clipToDisc({{p.x - n.x, p.y - n.y}, {q.x - n.x, q.y - n.y}, {q.x + n.x, q.y + n.y}, {p.x + n.x, p.y + n.y}}),
                mesh.walls);
        }
    }
    return mesh;
}

std::vector<MinimapVertex> minimapGlyphTriangles(MapPoint centre, MapPoint half, MapPoint uvLow, MapPoint uvHigh,
                                               double heading) {
    if (!minimapContains(centre) || !finite(half) || half.x <= 0 || half.y <= 0 ||
        !finite(uvLow) || !finite(uvHigh) || !std::isfinite(heading)) return {};
    const double cs = std::cos(heading), sn = std::sin(heading);
    std::vector<MapPoint> polygon = {{centre.x - half.x, centre.y + half.y},
        {centre.x + half.x, centre.y + half.y}, {centre.x + half.x, centre.y - half.y},
        {centre.x - half.x, centre.y - half.y}};
    for (auto& p : polygon) {
        const double x = p.x - centre.x, y = p.y - centre.y;
        p = {centre.x + cs * x + sn * y, centre.y - sn * x + cs * y};
    }
    // Same inscribed 64-sided mask as terrain. Clip the glyph, rather than
    // hiding in-range units whose glyph touches the rim.
    polygon = clipToDisc(std::move(polygon));
    std::vector<MinimapVertex> result;
    const auto vertex = [&](MapPoint p) {
        const double x = p.x - centre.x, y = p.y - centre.y;
        return MinimapVertex{p, {uvLow.x + std::clamp((cs * x - sn * y + half.x) / (2 * half.x), 0.0, 1.0) * (uvHigh.x - uvLow.x),
            uvLow.y + std::clamp((half.y - sn * x - cs * y) / (2 * half.y), 0.0, 1.0) * (uvHigh.y - uvLow.y)}};
    };
    for (size_t i = 1; i + 1 < polygon.size(); ++i) {
        result.push_back(vertex(polygon[0])); result.push_back(vertex(polygon[i])); result.push_back(vertex(polygon[i + 1]));
    }
    return result;
}

std::vector<MinimapVertex> minimapTileTriangles(const MinimapView& v, const MinimapTile& tile,
                                              const std::vector<const MinimapTile*>& covering) {
    std::vector<MinimapVertex> result;
    if (!valid(v) || !finite(tile.origin) || !std::isfinite(tile.span) || tile.span <= 0 ||
        !finite({tile.origin.x + tile.span, tile.origin.y + tile.span})) return result;
    // Begin with the disc, then clip in world space against all four tile edges.
    std::vector<MapPoint> polygon;
    for (int i = 0; i < 64; ++i) {
        const double a = i * 2 * std::numbers::pi / 64;
        const auto point = minimapUnproject(v, {std::cos(a), std::sin(a)});
        if (!point) return result;
        polygon.push_back(*point);
    }
    for (int edge = 0; edge < 4 && !polygon.empty(); ++edge) {
        const int axis = edge / 2;
        const double at = (axis ? tile.origin.y : tile.origin.x) + (edge % 2 ? tile.span : 0);
        const auto distance = [&](MapPoint p) { return ((axis ? p.y : p.x) - at) * (edge % 2 ? -1 : 1); };
        std::vector<MapPoint> clipped;
        for (size_t i = 0; i < polygon.size(); ++i) {
            const MapPoint a = polygon[i], b = polygon[(i + 1) % polygon.size()];
            const double da = distance(a), db = distance(b);
            if (da >= 0) clipped.push_back(a);
            if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
                const double t = da / (da - db);
                clipped.push_back({a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)});
            }
        }
        polygon = std::move(clipped);
    }
    // Subtract closer layers in world space. Painting two opaque tiles on top
    // of one another would darken/mix them when the widget inherits a faded alpha.
    const auto clip = [](const std::vector<MapPoint>& source, int axis, double at, double sign) {
        std::vector<MapPoint> clipped;
        for (size_t i = 0; i < source.size(); ++i) {
            const auto a = source[i], b = source[(i + 1) % source.size()];
            const double da = ((axis ? a.y : a.x) - at) * sign, db = ((axis ? b.y : b.x) - at) * sign;
            if (da >= 0) clipped.push_back(a);
            if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
                const double t = da / (da - db);
                clipped.push_back({a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)});
            }
        }
        return clipped;
    };
    std::vector<std::vector<MapPoint>> pieces{std::move(polygon)};
    for (const auto* cover : covering) {
        if (!cover || cover->origin.x + cover->span <= tile.origin.x || cover->origin.y + cover->span <= tile.origin.y ||
            cover->origin.x >= tile.origin.x + tile.span || cover->origin.y >= tile.origin.y + tile.span) continue;
        std::vector<std::vector<MapPoint>> remaining;
        for (auto piece : pieces) {
            for (int edge = 0; edge < 4 && !piece.empty(); ++edge) {
                const int axis = edge / 2;
                const double at = (axis ? cover->origin.y : cover->origin.x) + (edge % 2 ? cover->span : 0);
                const double sign = edge % 2 ? -1 : 1;
                auto outside = clip(piece, axis, at, -sign);
                if (outside.size() >= 3) remaining.push_back(std::move(outside));
                piece = clip(piece, axis, at, sign);
            } // The remaining interior is covered; each exterior piece is disjoint.
        }
        pieces = std::move(remaining);
    }
    auto vertex = [&](MapPoint p) {
        constexpr double inset = 0.5 / 256;
        return MinimapVertex{*minimapProject(v, p),
            {std::clamp((p.x - tile.origin.x) / tile.span, inset, 1 - inset),
             std::clamp(1 - (p.y - tile.origin.y) / tile.span, inset, 1 - inset)}};
    };
    for (const auto& piece : pieces) for (size_t i = 1; i + 1 < piece.size(); ++i)
        for (const auto p : {piece[0], piece[i], piece[i + 1]}) result.push_back(vertex(p));
    return result;
}
}  // namespace wowcore
