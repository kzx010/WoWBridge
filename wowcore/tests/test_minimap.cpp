#include <catch_amalgamated.hpp>

#include "wowcore/minimap.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/soft_raster.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/wowapi.hpp"
#include "addons/lua_engine.hpp"
#include "pipeline/blp_loader.hpp"
#include "ui/widget_tree.hpp"
#include <imgui.h>

extern "C" {
#include <lauxlib.h>
}

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <numbers>
#include <fstream>
#include <sstream>
#include <chrono>

namespace {
constexpr double pi = std::numbers::pi;
void pointNear(const std::optional<wowcore::MapPoint>& point, double x, double y, double tolerance = 1e-9) {
    REQUIRE(point.has_value());
    CHECK(std::abs(point->x - x) < tolerance);
    CHECK(std::abs(point->y - y) < tolerance);
}
}  // namespace

TEST_CASE("minimap keeps north-up terrain and turns the player arrow", "[minimap]") {
    const wowcore::MinimapView view{{1200, -3400}, 100, pi / 2, false};
    pointNear(wowcore::minimapProject(view, {1200, -3400}), 0, 0);
    pointNear(wowcore::minimapProject(view, {1200, -3350}), 0, 0.5);
    pointNear(wowcore::minimapProject(view, {1250, -3400}), 0.5, 0);
    pointNear(wowcore::minimapArrow(view), 1, 0);
    pointNear(wowcore::minimapArrow({{}, 100, pi, false}), 0, -1);
    pointNear(wowcore::minimapArrow({{}, 100, -pi / 2, false}), -1, 0);
}

TEST_CASE("rotating minimap moves east ahead and north left with one invertible transform", "[minimap]") {
    const wowcore::MinimapView view{{1200, -3400}, 100, pi / 2, true};
    pointNear(wowcore::minimapProject(view, {1250, -3400}), 0, 0.5);
    pointNear(wowcore::minimapProject(view, {1200, -3350}), -0.5, 0);
    pointNear(wowcore::minimapArrow(view), 0, 1);
    pointNear(wowcore::minimapUnproject(view, {0, 0.5}), 1250, -3400);
    // Non-cardinal headings, both rotation modes, large world coordinates and points beyond the rim.
    for (const bool rotate : {false, true}) {
        for (int i = -16; i <= 16; ++i) {
            const wowcore::MinimapView v{{10000000, -20000000}, 321, i * pi / 11, rotate};
            for (const auto p : {wowcore::MapPoint{10000037, -19999943}, wowcore::MapPoint{9999567, -20000213}}) {
                const auto d = wowcore::minimapProject(v, p);
                REQUIRE(d);
                pointNear(wowcore::minimapUnproject(v, *d), p.x, p.y, 1e-7);
            }
        }
    }
}

TEST_CASE("minimap zoom changes terrain and marker scale together", "[minimap]") {
    const double expected[] = {400, 400 / std::sqrt(2.0), 200, 200 / std::sqrt(2.0), 100};
    for (int zoom = 0; zoom < 5; ++zoom) {
        const auto radius = wowcore::minimapRadius(400, zoom);
        REQUIRE(radius);
        CHECK(std::abs(*radius - expected[zoom]) < 1e-9);
        const wowcore::MinimapView v{{}, *radius, 0, false};
        pointNear(wowcore::minimapProject(v, {50, 0}), 50 / *radius, 0);
        pointNear(wowcore::minimapBlip(v, {50, 0}, 70, {4, 4}), 3500 / *radius, 0);
    }
    CHECK(wowcore::minimapRadius(400, -3) == 400);
    CHECK(wowcore::minimapRadius(400, 99) == 100);
}

TEST_CASE("minimap inverse UV sampling respects native capture orientation and crop drift", "[minimap]") {
    const wowcore::MinimapCapture northUp{{1000, 2000}, {0.5, 0.5}, {1.0 / 400, 0}, {0, -1.0 / 400}};
    const wowcore::MinimapView view{{1000, 2000}, 100, pi / 2, true};
    pointNear(wowcore::minimapSampleUv(view, northUp, {0, 1}), 0.75, 0.5);
    pointNear(wowcore::minimapSampleUv(view, northUp, {-1, 0}), 0.5, 0.25);
    // Terrain under a marker must sample exactly the same source coordinate for every heading and zoom.
    for (int zoom = 0; zoom < 5; ++zoom) for (int i = 0; i < 16; ++i) {
        const wowcore::MinimapView v{{1030, 1980}, *wowcore::minimapRadius(100, zoom), i * pi / 8, true};
        const auto d = wowcore::minimapProject(v, {1040, 1990});
        REQUIRE(d);
        pointNear(wowcore::minimapSampleUv(v, northUp, *d), 0.6, 0.525);
    }
    // Native camera U follows north and V follows east: no hand-written Skyrim UV flip is needed.
    const wowcore::MinimapCapture rotated{{1000, 2000}, {0.5, 0.5}, {0, 1.0 / 400}, {1.0 / 600, 0}};
    pointNear(wowcore::minimapSampleUv(view, rotated, {0, 1}), 0.5, 0.75);
    CHECK(wowcore::minimapCaptureContains(view, northUp, {0.5 / 256, 0.5 / 256}));
    CHECK(wowcore::minimapCaptureContains(view, rotated));
    auto drift = view;
    drift.player.x += 100;
    CHECK(wowcore::minimapCaptureContains(drift, northUp));
    CHECK_FALSE(wowcore::minimapCaptureContains(drift, northUp, {0.5 / 256, 0.5 / 256}));
    drift.player.x += 1;
    CHECK_FALSE(wowcore::minimapCaptureContains(drift, northUp));
    CHECK_FALSE(wowcore::minimapSampleUv(drift, northUp, {0, 1}));
    // Circle coverage is independent of heading, even when the containing square would leave the source.
    const wowcore::MinimapCapture tight{{}, {0.5, 0.5}, {1.0 / 200, 0}, {0, -1.0 / 200}};
    CHECK(wowcore::minimapCaptureContains({{}, 100, pi / 4, true}, tight));
    CHECK_FALSE(wowcore::minimapSampleUv(view, northUp, {1, 1}));
}

TEST_CASE("minimap mask rejects out-of-range points and blips whose glyph crosses the rim", "[minimap]") {
    const wowcore::MinimapView view{{}, 100, 0, false};
    CHECK(wowcore::minimapContains({0, 0}));
    CHECK(wowcore::minimapContains({0.6, 0.8}));
    CHECK(wowcore::minimapContains({1, 0}));
    CHECK_FALSE(wowcore::minimapContains({0.8, 0.8}));
    CHECK_FALSE(wowcore::minimapContains({1.00001, 0}));
    pointNear(wowcore::minimapBlip(view, {0, 50}, 70, {4, 4}), 0, -35);
    CHECK(wowcore::minimapBlip(view, {100, 0}, 70, {0, 0}));
    CHECK_FALSE(wowcore::minimapBlip(view, {100, 0}, 70, {4, 4}));
    CHECK_FALSE(wowcore::minimapBlip(view, {95, 0}, 70, {4, 4}));
    CHECK(wowcore::minimapBlip(view, {90, 0}, 70, {4, 4}));
    CHECK(wowcore::minimapBlip(view, {}, 5, {3, 4}));
    CHECK_FALSE(wowcore::minimapBlip(view, {}, 4.99, {3, 4}));
    // This diagonal centre is inside the mask; its upper-right glyph corner is not.
    CHECK_FALSE(wowcore::minimapBlip(view, {70, 70}, 70, {4, 4}));
}

TEST_CASE("minimap fails closed for invalid camera and capture data", "[minimap]") {
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    const wowcore::MinimapView view{{}, 100, 0, false};
    const wowcore::MinimapCapture capture{{}, {0.5, 0.5}, {0.001, 0}, {0, -0.001}};
    for (const double radius : {0.0, -1.0, nan, inf}) {
        CHECK_FALSE(wowcore::minimapRadius(radius, 0));
        auto v = view;
        v.radius = radius;
        CHECK_FALSE(wowcore::minimapProject(v, {}));
        CHECK_FALSE(wowcore::minimapUnproject(v, {}));
        CHECK_FALSE(wowcore::minimapArrow(v));
        CHECK_FALSE(wowcore::minimapCaptureContains(v, capture));
    }
    CHECK_FALSE(wowcore::minimapProject(view, {nan, 0}));
    CHECK_FALSE(wowcore::minimapProject({{}, 100, inf, true}, {}));
    CHECK_FALSE(wowcore::minimapContains({inf, 0}));
    CHECK_FALSE(wowcore::minimapSampleUv(view, {}, {}));
    CHECK_FALSE(wowcore::minimapSampleUv(view, {{}, {0.5, 0.5}, {1, 1}, {2, 2}}, {}));
    CHECK_FALSE(wowcore::minimapCaptureContains(view, capture, {-1, 0}));
    CHECK_FALSE(wowcore::minimapCaptureContains(view, capture, {0.5, 0}));
    CHECK_FALSE(wowcore::minimapBlip(view, {}, nan, {4, 4}));
    CHECK_FALSE(wowcore::minimapBlip(view, {}, 70, {-1, 4}));
}

TEST_CASE("1.12 minimap buttons use widget zoom and player map answers remain map coordinates", "[client][api][minimap]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    auto* L = ui.lua();
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dostring(L, "__WBFixture('idle')") == 0);
    REQUIRE(ui.loadInterface(&err));
    const int rc = luaL_dostring(L, R"(
        __WBLogin()
        assert(Minimap:GetObjectType() == 'Minimap')
        assert(Minimap:GetZoomLevels() == 5)
        Minimap:SetZoom(0)
        MinimapZoomIn:Click()
        assert(Minimap:GetZoom() == 1)
        for i=1,3 do MinimapZoomIn:Click() end
        assert(Minimap:GetZoom() == 4 and MinimapZoomIn:IsEnabled() == 0)
        MinimapZoomOut:Click()
        assert(Minimap:GetZoom() == 3 and MinimapZoomIn:IsEnabled() == 1)
        for i=1,3 do MinimapZoomOut:Click() end
        assert(Minimap:GetZoom() == 0 and MinimapZoomOut:IsEnabled() == 0)
        Minimap:SetZoom(100)
        assert(Minimap:GetZoom() == 4)
        Minimap:SetZoom(-1)
        assert(Minimap:GetZoom() == 0)
        local p = __WB.units[__WB.tokens.player]
        p.mapX, p.mapY = 0.25, 0.75
        local x, y = GetPlayerMapPosition('player')
        assert(x == 0.25 and y == 0.75)
        Minimap:SetZoom(4)
        SetCVar('rotateMinimap', 1)
        x, y = GetPlayerMapPosition('player')
        assert(x == 0.25 and y == 0.75 and GetCVar('rotateMinimap') == '1')
        x, y = GetPlayerMapPosition('absent')
        assert(x == 0 and y == 0)
        p.mapX, p.mapY = nil, nil
        x, y = GetPlayerMapPosition('player')
        assert(x == 0 and y == 0)
        SetCVar('rotateMinimap', 0)
        Minimap:SetZoom(0)
    )");
    INFO((rc ? lua_tostring(L, -1) : ""));
    REQUIRE(rc == 0);
    const auto* minimap = ui.engine().widgets().findByName("Minimap");
    REQUIRE(minimap);
    CHECK(minimap->zoomLevel == 0);
}

TEST_CASE("static minimap finds negative and offset terrain cells without wrapping holes", "[minimap]") {
    std::istringstream index("WOWBRIDGE_MINIMAP 1 64 16384\n"
        "testworld -3 -1 WB_testworld_-3_-1.blp\n"
        "testworld 1 -1 WB_testworld_1_-1.blp\n"
        "testworld -3 3 WB_testworld_-3_3.blp\n");
    const auto atlas = wowcore::MinimapAtlas::read(index);
    const auto* world = atlas.world("TestWorld");
    REQUIRE(world);
    CHECK_FALSE(atlas.world("interior"));
    CHECK(world->at({-192, -64})->origin.x == -192);
    CHECK(world->at({63.999, -64})->origin.x == -192);
    CHECK(world->at({64, -64})->origin.x == 64);
    CHECK_FALSE(world->at({-192.001, -64}));
    CHECK_FALSE(world->at({320, -64}));
    CHECK_FALSE(world->at({128, 256})); // sparse missing tile
    pointNear(world->mapPosition({64, 192}), 0.5, 0.5);
    CHECK(world->around({{64, 192}, 100, 0, false}).size() == 3);
    CHECK(world->around({{64, 192}, 100, pi / 2, true}).size() == 3);
    CHECK(world->around({{-300, -300}, 10, 0, false}).empty());
    CHECK_FALSE(world->mapPosition({std::numeric_limits<double>::infinity(), 0}));
    for (const auto text : {"wrong header\n", "WOWBRIDGE_MINIMAP 1 64 16384\n",
         "WOWBRIDGE_MINIMAP 1 64 16384\nw 0 0 ../tile.blp\n",
         "WOWBRIDGE_MINIMAP 1 64 16384\nw 0 0 WB_w_0_0.blp\nw 1 0 WB_w_1_0.blp\n",
         "WOWBRIDGE_MINIMAP 1 64 16384\nw 0 0 WB_w_0_0.blp\nw 0 0 WB_w_0_0.blp\n"}) {
        std::istringstream bad(text);
        CHECK_THROWS(wowcore::MinimapAtlas::read(bad));
    }
}

TEST_CASE("static minimap tile triangles cover a masked disc at boundaries and every zoom", "[minimap]") {
    for (const bool rotate : {false, true}) for (int zoom = 0; zoom < 5; ++zoom) {
        const wowcore::MinimapView view{{0, 0}, *wowcore::minimapRadius(200, zoom), pi / 3, rotate};
        double area = 0;
        for (const double x : {-256.0, 0.0}) for (const double y : {-256.0, 0.0}) {
            const wowcore::MinimapTile tile{{x, y}, 256, "test"};
            const auto vertices = wowcore::minimapTileTriangles(view, tile);
            REQUIRE(vertices.size() % 3 == 0);
            for (const auto& v : vertices) {
                CHECK(std::hypot(v.disc.x, v.disc.y) <= 1 + 1e-9);
                CHECK(v.uv.x >= 0.5 / 256);
                CHECK(v.uv.x <= 1 - 0.5 / 256);
                CHECK(v.uv.y >= 0.5 / 256);
                CHECK(v.uv.y <= 1 - 0.5 / 256);
                const auto p = wowcore::minimapUnproject(view, v.disc);
                REQUIRE(p);
                CHECK(p->x >= x - 1e-8);
                CHECK(p->x <= x + 256 + 1e-8);
                CHECK(p->y >= y - 1e-8);
                CHECK(p->y <= y + 256 + 1e-8);
            }
            for (size_t i = 0; i < vertices.size(); i += 3) {
                const auto a = vertices[i].disc, b = vertices[i + 1].disc, c = vertices[i + 2].disc;
                area += std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * 0.5;
            }
        }
        CHECK(std::abs(area - 32 * std::sin(2 * pi / 64)) < 1e-9); // no gaps/overlap at four-way seam
    }
    CHECK(wowcore::minimapTileTriangles({{}, 100, 0, false}, {{1000, 1000}, 256, "test"}).empty());
    CHECK(wowcore::minimapTileTriangles({{}, 0, 0, false}, {{}, 256, "test"}).empty());
}

TEST_CASE("generated Skyrim atlas decodes with wowcore and retains its world origins", "[minimap][minimap-assets]") {
    const auto root = std::filesystem::path(WOWBRIDGE_ROOT) / "build/skyrim_minimap";
    if (!std::filesystem::exists(root / "index.tsv")) SKIP("generate terrain with tools/skyrim_minimap.py first");
    std::ifstream index(root / "index.tsv");
    const auto atlas = wowcore::MinimapAtlas::read(index);
    REQUIRE(atlas.world("Tamriel"));
    REQUIRE(atlas.world("DLC2SolstheimWorld"));
    index.clear(); index.seekg(0);
    std::string line;
    std::getline(index, line);
    size_t count = 0;
    while (std::getline(index, line)) {
        std::istringstream row(line);
        std::string world, file;
        int x = 0, y = 0;
        REQUIRE(static_cast<bool>(row >> world >> x >> y >> file));
        INFO(file);
        std::ifstream input(root / file, std::ios::binary);
        const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
        const auto image = wowee::pipeline::BLPLoader::load(bytes);
        REQUIRE(image.width == 256);
        REQUIRE(image.height == 256);
        REQUIRE(image.data.size() == 256 * 256 * 4);
        // The decoder reports a 16-slot capacity for uncompressed BLP2s;
        // inspect the serialized offsets for the actual number of levels.
        size_t levels = 0;
        for (size_t mip = 0; mip < 16; ++mip) {
            const size_t at = 20 + mip * 4;
            if (bytes[at] || bytes[at + 1] || bytes[at + 2] || bytes[at + 3]) ++levels;
        }
        CHECK(levels == 9);
        const auto* tile = atlas.world(world)->at({x * 64.0 + 128, y * 64.0 + 128});
        REQUIRE(tile);
        CHECK(tile->origin.x == x * 64.0);
        CHECK(tile->origin.y == y * 64.0);
        ++count;
    }
    CHECK(count >= 4608); // supplied Tamriel + Solstheim; mods can add other worlds
}

TEST_CASE("static minimap widget draws terrain, client arrow, zoom, alpha and empty interiors", "[client][minimap]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    const auto root = std::filesystem::temp_directory_path() / ("wowcore-minimap-widget-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{root};
    std::ostringstream rows;
    rows << "WOWBRIDGE_MINIMAP 1 64 16384\n";
    for (const int x : {-4, 0}) for (const int y : {-4, 0}) {
        const auto file = "WB_testworld_" + std::to_string(x) + "_" + std::to_string(y) + ".blp";
        rows << "testworld " << x << ' ' << y << ' ' << file << '\n';
        std::vector<uint8_t> bytes(1172 + 256 * 256 * 4, 0);
        const auto u32 = [&](size_t at, uint32_t value) {
            for (int i = 0; i < 4; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (i * 8));
        };
        std::copy_n("BLP2", 4, bytes.begin());
        u32(4, 1); bytes[8] = 3; bytes[9] = 8;
        u32(12, 256); u32(16, 256); u32(20, 1172); u32(84, 256 * 256 * 4);
        for (int py = 0; py < 256; ++py) for (int px = 0; px < 256; ++px) {
            const size_t at = 1172 + (py * 256 + px) * 4;
            bytes[at] = 40;
            bytes[at + 1] = static_cast<uint8_t>(40 + (y * 64 + 255 - py + 256) * 160 / 512);
            bytes[at + 2] = static_cast<uint8_t>(20 + (x * 64 + px + 256) * 200 / 512);
            bytes[at + 3] = 255;
        }
        std::ofstream out(root / file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    std::istringstream index(rows.str());
    const auto atlas = wowcore::MinimapAtlas::read(index);
    auto mpq = std::make_shared<wowcore::Mpq>(data, std::filesystem::path{}, root);
    REQUIRE(mpq->exists("interface/minimap/wb_TESTWORLD_-4_-4.blp"));
    REQUIRE_FALSE(mpq->exists("Interface\\Minimap\\..\\WB_testworld_-4_-4.blp"));
    CHECK(mpq->list("Interface\\Minimap\\WB_*.blp").size() == 4);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 768, 768);
    std::string error;
    REQUIRE(ui.start(&error));
    auto* L = ui.lua();
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dostring(L, "__WBFixture('idle'); Minimap = CreateFrame('Minimap', 'Minimap', UIParent); "
        "Minimap:SetWidth(140); Minimap:SetHeight(140); Minimap:SetPoint('CENTER', UIParent, 'CENTER', 0, 0)") == 0);
    const auto render = [&](wowcore::MapPoint player, double heading, int zoom, const std::string& world = "testworld") {
        ui.setMinimap(&atlas, world, player, heading);
        ui.engine().widgets().findByName("Minimap")->zoomLevel = zoom;
        ImDrawData* draw = nullptr;
        for (int i = 0; i < 8; ++i) { draw = ui.frame(0.016f); textures.sync(draw); }
        wowcore::Image image(768, 768);
        wowcore::rasterize(draw, textures, image);
        return image;
    };
    const auto channel = [](const wowcore::Image& image, int x, int y, int c) { return image.rgba[(y * image.w + x) * 4 + c]; };
    const auto north = render({}, 0, 0), east = render({}, pi / 2, 0);
    for (const int x : {315, 452}) for (const int y : {315, 452}) CHECK(channel(north, x, y, 3) == 0);
    CHECK(channel(north, 384, 340, 3) == 255);
    CHECK(channel(north, 384, 340, 1) > channel(north, 384, 428, 1)); // north is up
    CHECK(channel(north, 428, 384, 0) > channel(north, 340, 384, 0)); // east is right
    for (int c = 0; c < 4; ++c) CHECK(channel(north, 420, 350, c) == channel(east, 420, 350, c)); // heading only turns arrow
    int arrowChanged = 0;
    for (int y = 375; y < 393; ++y) for (int x = 375; x < 393; ++x)
        arrowChanged += channel(north, x, y, 0) != channel(east, x, y, 0);
    CHECK(arrowChanged > 10);
    const auto zoomed = render({}, 0, 4), moved = render({64, 0}, 0, 0);
    CHECK(channel(zoomed, 428, 384, 0) < channel(north, 428, 384, 0));
    CHECK(channel(moved, 428, 384, 0) > channel(north, 428, 384, 0));
    REQUIRE(luaL_dostring(L, "local x,y=GetPlayerMapPosition('player'); assert(x==0.625 and y==0.5)") == 0);
    REQUIRE(luaL_dostring(L, "SetCVar('rotateMinimap', 1)") == 0);
    const auto rotated = render({}, pi / 2, 0);
    CHECK(channel(rotated, 384, 340, 0) > channel(rotated, 384, 428, 0)); // east ahead/up
    REQUIRE(luaL_dostring(L, "SetCVar('rotateMinimap', 0); UIParent:SetAlpha(0.5)") == 0);
    const auto faded = render({}, 0, 0);
    CHECK(channel(faded, 384, 340, 3) >= 126);
    CHECK(channel(faded, 384, 340, 3) <= 129);
    const auto interior = render({}, 0, 0, "");
    CHECK(channel(interior, 384, 340, 3) == 0);
    CHECK(channel(interior, 384, 384, 3) == 0);
    REQUIRE(luaL_dostring(L, "local x,y=GetPlayerMapPosition('player'); assert(x==0 and y==0)") == 0);
    CHECK_FALSE(ui.engine().widgets().findByName("Minimap")->hostModel);
}

TEST_CASE("tracking glyphs clip at the rim without dropping in-range units", "[minimap][tracking]") {
    for (const auto centre : {wowcore::MapPoint{}, wowcore::MapPoint{1, 0}, wowcore::MapPoint{0.6, 0.8}}) {
        const auto glyph = wowcore::minimapGlyphTriangles(centre, {4.0/70, 4.0/70}, {0.25, 0}, {0.5, 0.25});
        REQUIRE_FALSE(glyph.empty());
        for (const auto& v : glyph) {
            CHECK(std::hypot(v.disc.x, v.disc.y) <= 1.000000001);
            CHECK(v.uv.x >= 0.25); CHECK(v.uv.x <= 0.5);
            CHECK(v.uv.y >= 0); CHECK(v.uv.y <= 0.25);
        }
    }
    CHECK(wowcore::minimapGlyphTriangles({1.001,0}, {0.1,0.1}, {}, {1,1}).empty());
    CHECK(wowcore::minimapGlyphTriangles({}, {}, {}, {1,1}).empty());
}

TEST_CASE("client tracking icon tooltip buff and right click follow the authoritative aura", "[client][api][minimap][tracking]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    REQUIRE(std::filesystem::exists(data / "interface.MPQ"));
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    const auto* spell = db.spell(5225);
    REQUIRE(spell);
    CHECK(spell->effects[0].aura == 44); CHECK(spell->effects[0].misc == 7);
    CHECK(spell->durationMs == -1); CHECK(spell->shapeshiftMask == 1);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string error;
    REQUIRE(ui.start(&error));
    auto* L = ui.lua();
    const auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : "")); REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&error));
    run("__WBLogin(); assert(GetTrackingTexture()==nil and GetTrackingTexture(1)==nil and GetTrackingTexture(2)==nil)");
    run(R"(
        __WBAuraAdd('player', 5225, false, 0)
        assert(GetTrackingTexture() == __WB.spellInfo(5225).icon)
        assert(MiniMapTrackingFrame:IsShown())
        assert(MiniMapTrackingIcon:GetTexture() == GetTrackingTexture())
        assert(UnitBuff('player', 1) == GetTrackingTexture())
        local index, untilCancelled = GetPlayerBuff(0, 'HELPFUL')
        assert(index==0 and untilCancelled and GetPlayerBuffTimeLeft(index)==0)
        GameTooltip:SetOwner(MiniMapTrackingFrame, 'ANCHOR_BOTTOMLEFT')
        GameTooltip:SetTrackingSpell()
        assert(GameTooltipTextLeft1:GetText()=='Track Humanoids')
        assert(string.find(string.lower(GameTooltipTextLeft2:GetText()), 'humanoids'))
    )");
    int cancelled = 0;
    wowcore::setHostHandler(L, [&](lua_State* state, const std::string& command) {
        if (command == "cancelAura") cancelled = static_cast<int>(lua_tonumber(state, 2));
        return 0;
    });
    // Exercise the actual XML OnMouseUp handler, with 1.12's global this/arg1.
    run("this=MiniMapTrackingFrame; arg1='LeftButton'; this:GetScript('OnMouseUp')(); this=nil; arg1=nil");
    CHECK(cancelled == 0);
    run("this=MiniMapTrackingFrame; arg1='RightButton'; this:GetScript('OnMouseUp')(); this=nil; arg1=nil");
    CHECK(cancelled == 5225);
    run("assert(GetTrackingTexture()); __WBAuraRemove('player',5225); assert(GetTrackingTexture()==nil and not MiniMapTrackingFrame:IsShown())");
    run("__WBAuraAdd('player',5225,false,0); __WBResetCombat(); assert(GetTrackingTexture()==nil and not MiniMapTrackingFrame:IsShown())");
}

TEST_CASE("minimap draws actual tracked glyphs with zoom rotation movement alpha and clearing", "[client][minimap][tracking]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    REQUIRE(std::filesystem::exists(data / "interface.MPQ"));
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    const auto bytes = mpq->read("Interface\\Minimap\\ObjectIcons.blp");
    REQUIRE(bytes);
    const auto art = wowee::pipeline::BLPLoader::load(*bytes);
    REQUIRE(art.width == 128); REQUIRE(art.height == 128);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 768, 768);
    std::string error;
    REQUIRE(ui.start(&error));
    auto* L = ui.lua();
    const auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : "")); REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run(R"(
        __WBFixture('idle'); __WBAuraAdd('player',5225,false,0)
        Minimap=CreateFrame('Minimap','Minimap',UIParent)
        Minimap:SetWidth(140); Minimap:SetHeight(140); Minimap:SetPoint('CENTER',UIParent,'CENTER',0,0)
        Minimap:EnableMouse(true)
        GameTooltip=CreateFrame('GameTooltip','GameTooltip',UIParent)
        GameTooltip:Hide()
        __WBApplyLive({minimap={worldspace='Test',x=1000,y=2000,heading=0,trackingMask=64,blips={
          {guid=1,name='Bandit',x=1100,y=2000,reaction=2}, {guid=2,name='Guard',x=1000,y=2080,reaction=5},
          {guid=3,x=920,y=2000,reaction=4}, {guid=4,x=1199,y=2000,reaction=2},
          {guid=5,x=1250,y=2000,reaction=2}, {x=0/0,y=2000,reaction=2}}}})
    )");
    const auto render = [&]() {
        ui.applyLiveMinimap(nullptr);
        ImDrawData* draw = nullptr;
        for (int i=0; i<8; ++i) { draw=ui.frame(0.016f); textures.sync(draw); }
        wowcore::Image image(768,768); wowcore::rasterize(draw,textures,image); return image;
    };
    const auto pixel = [](const wowcore::Image& im,int x,int y,int c) {return im.rgba[(y*im.w+x)*4+c];};
    const auto normal=render();
    CHECK(pixel(normal,419,384,0)>pixel(normal,419,384,1)); // hostile red
    CHECK(pixel(normal,384,356,1)>pixel(normal,384,356,0)); // friendly green
    CHECK(pixel(normal,356,384,0)>pixel(normal,356,384,2)); // neutral yellow
    CHECK(pixel(normal,452,384,3)>0); // glyph centred at the rim is clipped, still drawn
    CHECK(pixel(normal,455,384,3)==0);
    ui.pointer(419,384); render();
    run("assert(__WB.minimapHover=='Bandit' and GameTooltipTextLeft1:GetText()=='Bandit')");
    ui.pointer(384,356); render();
    run("assert(__WB.minimapHover=='Guard')");
    run("__WBAuraRemove('player',5225)"); render();
    run("assert(__WB.minimapHover==nil and not GameTooltip:IsShown())");
    run("__WBAuraAdd('player',5225,false,0)"); ui.pointer(-100,-100);
    run("Minimap:SetZoom(4)");
    const auto zoomed=render(); CHECK(pixel(zoomed,419,384,3)==0); // now outside the 50-yard radius
    run("Minimap:SetZoom(0); __WB.minimap.x=1050");
    const auto moved=render(); CHECK(pixel(moved,401,384,3)>0); CHECK(pixel(moved,419,384,3)==0);
    run("__WB.minimap.x=1000; __WB.minimap.heading=math.pi/2; SetCVar('rotateMinimap',1)");
    const auto rotated=render(); CHECK(pixel(rotated,384,349,0)>pixel(rotated,384,349,1));
    run("SetCVar('rotateMinimap',0); UIParent:SetAlpha(0.5)");
    const auto faded=render(); CHECK(pixel(faded,419,384,3)>=120); CHECK(pixel(faded,419,384,3)<=129);
    run("__WBAuraRemove('player',5225)");
    const auto cancelled=render(); CHECK(pixel(cancelled,419,384,3)==0); // even with a stale positions snapshot
    run("__WBAuraAdd('player',5225,false,0); __WB.minimap.trackingMask=1");
    const auto stale=render(); CHECK(pixel(stale,419,384,3)==0); // a different creature tracker's old snapshot
    run("__WBApplyLive({minimap=false})");
    const auto interior=render(); CHECK(pixel(interior,419,384,3)==0);
    CHECK_FALSE(ui.engine().widgets().findByName("Minimap")->hostModel);
}

TEST_CASE("compass POIs retain positions or point along the rim at every zoom and heading", "[minimap][poi]") {
    for (int zoom = 0; zoom < 5; ++zoom) for (bool rotate : {false, true}) for (int h = 0; h < 8; ++h) {
        const wowcore::MinimapView view{{1000, -2000}, *wowcore::minimapRadius(200, zoom), h * pi / 4, rotate};
        for (auto at : {wowcore::MapPoint{1020, -1980}, wowcore::MapPoint{1800, -1500}}) {
            const auto projected = wowcore::minimapProject(view, at);
            const auto p = wowcore::minimapPoi(view, at, 0.08);
            REQUIRE(projected); REQUIRE(p);
            if (std::hypot(projected->x, projected->y) <= 1) {
                CHECK_FALSE(p->arrow);
                CHECK(p->centre.x == projected->x); CHECK(p->centre.y == projected->y);
            } else {
                CHECK(p->arrow);
                CHECK(std::abs(std::hypot(p->centre.x, p->centre.y) - 0.92) < 1e-9);
                CHECK(std::abs(std::sin(p->heading) - projected->x / std::hypot(projected->x, projected->y)) < 1e-9);
                CHECK(std::abs(std::cos(p->heading) - projected->y / std::hypot(projected->x, projected->y)) < 1e-9);
                const auto glyph = wowcore::minimapGlyphTriangles(p->centre, {0.17, 0.17}, {}, {1,1}, p->heading);
                REQUIRE_FALSE(glyph.empty());
                for (const auto& v : glyph) {
                    CHECK(std::hypot(v.disc.x, v.disc.y) <= 1.000001);
                    CHECK(v.uv.x >= 0); CHECK(v.uv.x <= 1); CHECK(v.uv.y >= 0); CHECK(v.uv.y <= 1);
                }
            }
        }
    }
    CHECK_FALSE(wowcore::minimapPoi({{}, 0, 0, false}, {}, 0.1));
    CHECK_FALSE(wowcore::minimapPoi({{}, 100, 0, false}, {}, 1));
}

TEST_CASE("minimap compass art and client tooltip follow pose zoom rotation and clearing", "[client][minimap][poi]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    for (const auto* name : {"POIIcons", "ObjectIcons", "Rotating-MinimapCorpseArrow", "Rotating-MinimapGuideArrow"}) {
        const auto bytes = mpq->read(std::string("Interface\\Minimap\\") + name + ".blp");
        REQUIRE(bytes); REQUIRE(wowee::pipeline::BLPLoader::load(*bytes).width > 0);
    }
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 768, 768);
    std::string error;
    REQUIRE(ui.start(&error));
    auto* L = ui.lua();
    const auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : "")); REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run(R"(
        __WBFixture('idle')
        Minimap=CreateFrame('Minimap','Minimap',UIParent)
        Minimap:SetWidth(140); Minimap:SetHeight(140); Minimap:SetPoint('CENTER',UIParent,'CENTER',0,0)
        Minimap:EnableMouse(true)
        GameTooltip=CreateFrame('GameTooltip','GameTooltip',UIParent)
        GameTooltip:Hide()
        __WBApplyLive({minimap={worldspace='Test',x=1000,y=2000,heading=0,pois={
          {guid=1,kind='quest',name='Talk to Alvor',x=1100,y=2000},
          {guid=2,kind='location',name='Riverwood',icon=4,discovered=true,x=1000,y=2080},
          {guid=3,kind='custom',name='Custom map marker',x=920,y=2000},
          {guid=4,kind='quest',name='A distant objective',x=1600,y=2400},
          {guid=5,kind='location',name='Undiscovered cave',icon=16,x=600,y=1500},
          {guid=6,kind='quest',name='Bad position',x=0/0,y=2000}}}})
    )");
    const auto render = [&]() {
        ui.applyLiveMinimap(nullptr);
        ImDrawData* draw = nullptr;
        for (int i=0; i<8; ++i) { draw=ui.frame(0.016f); textures.sync(draw); }
        wowcore::Image image(768,768); wowcore::rasterize(draw,textures,image); return image;
    };
    const auto pixel = [](const wowcore::Image& im,int x,int y,int c) {return im.rgba[(y*im.w+x)*4+c];};
    const auto normal=render();
    CHECK(pixel(normal,419,384,0)>pixel(normal,419,384,1)); // actual red quest !
    CHECK(pixel(normal,356,384,2)>pixel(normal,356,384,0)); // actual blue custom marker
    CHECK(pixel(normal,384,356,3)>0); // location house
    int rimPixels=0;
    for (int y=342;y<357;++y) for(int x=431;x<444;++x) rimPixels+=pixel(normal,x,y,3)>0;
    CHECK(rimPixels>10); // distant objective's corpse arrow
    ui.pointer(419,384); render();
    run("assert(__WB.minimapHover=='Talk to Alvor' and GameTooltip:IsOwned(Minimap) and GameTooltip:IsShown()); "
        "assert(GameTooltipTextLeft1:GetText()=='Talk to Alvor')");
    ui.pointer(438,348); render();
    run("assert(__WB.minimapHover=='A distant objective')");
    ui.pointer(-100,-100); render();
    run("assert(__WB.minimapHover==nil and not GameTooltip:IsShown())");
    run("Minimap:SetZoom(4)");
    const auto zoomed=render();
    CHECK(pixel(zoomed,419,384,3)==0); // quest is now a rim arrow, no old icon
    ui.pointer(449,384); render(); run("assert(__WB.minimapHover=='Talk to Alvor')");
    run("Minimap:SetZoom(0); __WB.minimap.heading=math.pi/2; SetCVar('rotateMinimap',1)");
    ui.pointer(-100,-100);
    const auto rotated=render();
    CHECK(pixel(rotated,384,349,0)>pixel(rotated,384,349,1));
    ui.pointer(384,349); render(); run("assert(__WB.minimapHover=='Talk to Alvor')");
    run("SetCVar('rotateMinimap',0); __WB.minimap.x=1050");
    ui.pointer(401,384); render(); run("assert(__WB.minimapHover=='Talk to Alvor')");
    run("__WB.minimap.x=1000; UIParent:SetAlpha(0.5)"); ui.pointer(-100,-100);
    const auto faded=render(); CHECK(pixel(faded,419,384,3)>0); CHECK(pixel(faded,419,384,3)<=129);
    run("__WB.minimap.pois={}"); render();
    CHECK_FALSE(ui.engine().widgets().findByName("Minimap")->hostModel);
    run("__WBApplyLive({minimap=false})");
    const auto interior=render(); CHECK(pixel(interior,419,384,3)==0);
    run("assert(__WB.minimapHover==nil and not __WB.minimapTileExists)");
}

TEST_CASE("child terrain takes precedence and sparse holes and outer coverage use parent tiles", "[client][minimap][poi]") {
    const auto root = std::filesystem::temp_directory_path() / ("wowcore-minimap-parent-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{root};
    std::ostringstream rows;
    rows << "WOWBRIDGE_MINIMAP 1 64 16384\n";
    const auto tile = [&](const std::string& world, int x, int y, bool child) {
        const auto file = "WB_" + world + "_" + std::to_string(x) + "_" + std::to_string(y) + ".blp";
        rows << world << ' ' << x << ' ' << y << ' ' << file << '\n';
        std::vector<uint8_t> bytes(1172 + 256 * 256 * 4, 0);
        const auto u32 = [&](size_t at, uint32_t value) { for (int i=0;i<4;++i) bytes[at+i]=static_cast<uint8_t>(value>>(i*8)); };
        std::copy_n("BLP2",4,bytes.begin()); u32(4,1); bytes[8]=3; bytes[9]=8;
        u32(12,256); u32(16,256); u32(20,1172); u32(84,256*256*4);
        for(size_t at=1172;at<bytes.size();at+=4) { bytes[at]=child?20:200; bytes[at+2]=child?200:20; bytes[at+3]=255; }
        std::ofstream(root/file,std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    };
    for(int x : {-4,0,4}) for(int y : {-4,0,4}) tile("parent",x,y,false);
    tile("child",1,1,true); tile("child",9,1,true); // offset grid, hole at (5,1)
    std::istringstream index(rows.str()); const auto atlas=wowcore::MinimapAtlas::read(index);
    auto mpq=std::make_shared<wowcore::Mpq>(WOWBRIDGE_ROOT "/../WoW/client/Data",std::filesystem::path{},root);
    const wowcore::ClientDb db(*mpq); wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq,db,textures,768,768); std::string error; REQUIRE(ui.start(&error));
    auto* L=ui.lua();
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    const auto run=[&](const char* code) { int rc=luaL_dostring(L,code); INFO((rc?lua_tostring(L,-1):"")); REQUIRE(rc==0); };
    run("__WBFixture('idle'); Minimap=CreateFrame('Minimap','Minimap',UIParent); Minimap:SetWidth(140); Minimap:SetHeight(140); "
        "Minimap:SetPoint('CENTER',UIParent,'CENTER',0,0); __WBApplyLive({minimap={worldspace='Child',parents={'Parent'},x=64,y=164,heading=0}})");
    const auto render=[&]() {
        ui.applyLiveMinimap(&atlas); ImDrawData* draw=nullptr;
        for(int i=0;i<8;++i) { draw=ui.frame(0.016f); textures.sync(draw); }
        wowcore::Image im(768,768); wowcore::rasterize(draw,textures,im); return im;
    };
    const auto channel=[](const wowcore::Image& im,int x,int y,int c) { return im.rgba[(y*im.w+x)*4+c]; };
    for (int zoom=0;zoom<5;++zoom) for(bool rotate : {false,true}) {
        ui.engine().widgets().findByName("Minimap")->zoomLevel=zoom;
        run(rotate?"__WB.minimap.heading=math.pi/2; SetCVar('rotateMinimap',1)":"SetCVar('rotateMinimap',0)");
        const auto im=render();
        CHECK(channel(im,rotate?384:414,rotate?354:384,0)==200); // child is red, over parent
        CHECK(channel(im,rotate?384:354,rotate?414:384,2)==200); // parent blue across edge
    }
    run("UIParent:SetAlpha(0.5)");
    const auto faded = render();
    CHECK(channel(faded,384,354,3)>=126); CHECK(channel(faded,384,354,3)<=129); // no overlapping parent alpha
    CHECK(channel(faded,384,414,3)>=126); CHECK(channel(faded,384,414,3)<=129);
    run("assert(__WB.minimapTileExists and string.find(__WB.minimapTile,'WB_child_1_1',1,true)); __WB.minimap.x=400"); render();
    run("assert(__WB.minimapTileExists and string.find(__WB.minimapTile,'WB_parent_4_0',1,true)); __WB.minimap.x=-100"); render();
    run("assert(__WB.minimapTileExists and string.find(__WB.minimapTile,'WB_parent_-4_0',1,true)); __WB.minimap.worldspace='Absent'"); render();
    run("assert(__WB.minimapTileExists)"); // absent child still has parent's terrain
    run("__WBApplyLive({minimap=false})"); render(); run("assert(not __WB.minimapTileExists and __WB.minimapTile==nil)");
}

TEST_CASE("Native compass badges retain bearings across zoom and use native opacity", "[client][minimap][poi][native-compass]") {
    const auto root=std::filesystem::path(WOWBRIDGE_ROOT)/"build/compass";
    if (!std::filesystem::exists(root/"WB_Compass.blp")) SKIP("Generate local Skyrim compass atlas first");
    auto mpq=std::make_shared<wowcore::Mpq>(WOWBRIDGE_ROOT "/../WoW/client/Data",root);
    const wowcore::ClientDb db(*mpq); wowcore::SoftTextures textures; wowcore::UiSession ui(mpq,db,textures,768,768);
    std::string error; REQUIRE(ui.start(&error)); auto* L=ui.lua();
    const auto run=[&](const char* code) { const int rc=luaL_dostring(L,code); INFO((rc ? lua_tostring(L,-1) : code)); REQUIRE(rc==0); };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run(R"(
      __WBFixture('idle')
      Minimap=CreateFrame('Minimap','Minimap',UIParent) Minimap:SetWidth(140) Minimap:SetHeight(140)
      Minimap:SetPoint('CENTER',UIParent,'CENTER',0,0) Minimap:EnableMouse(true)
      GameTooltip=CreateFrame('GameTooltip','GameTooltip',UIParent) GameTooltip:Hide()
      __WBApplyLive({minimap={worldspace='Test',x=0,y=0,heading=0,compass=true,pois={
        {guid=1,kind='quest',name='Native quest',x=0,y=1000000,native=1,opacity=1,scale=1},
        {guid=2,kind='custom',name='Native custom',x=1000000,y=0,native=3,opacity=.5,scale=1},
        {guid=3,kind='enemy',name='Hidden enemy',x=0,y=-1000000,native=4,opacity=0,scale=1}}}})
    )");
    const auto render=[&] { ui.applyLiveMinimap(nullptr); ImDrawData* draw=nullptr;
        for (int i=0;i<12;++i) { draw=ui.frame(.016f); textures.sync(draw); }
        wowcore::Image im(768,768); wowcore::rasterize(draw,textures,im); return im; };
    const auto alpha=[](const wowcore::Image& im,int x,int y) { return im.rgba[(y*im.w+x)*4+3]; };
    const auto first=render(); run("assert(__WB.nativeCompassReady)");
    CHECK(alpha(first,384,324)>0); CHECK(alpha(first,444,384)>0); CHECK(alpha(first,444,384)<=128);
    CHECK(alpha(first,384,444)==0);
    ui.pointer(384,324); render(); run("assert(__WB.minimapHover=='Native quest')");
    ui.pointer(-100,-100); run("Minimap:SetZoom(4)"); const auto zoomed=render();
    CHECK(first.rgba==zoomed.rgba); // native distance/visibility never follows terrain zoom
    run("__WB.minimap.heading=math.pi/2 SetCVar('rotateMinimap',1)"); const auto rotated=render();
    CHECK(alpha(rotated,324,384)>0); CHECK(alpha(rotated,384,324)>0);
    ui.pointer(324,384); render(); run("assert(__WB.minimapHover=='Native quest')");
    run("__WBApplyLive({minimap=false})"); render(); run("assert(not __WB.nativeCompassReady and not __WB.minimapHover)");
    CHECK(ui.engine().lastError().empty());
}
