// Tests against the real 1.12.1 client data. They skip (not fail) when the client is absent; the data path comes
// from WOWBRIDGE_WOW_DATA or the project default.
#include <catch_amalgamated.hpp>

#include "pipeline/blp_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "wowcore/character.hpp"
#include "wowcore/combat_feedback.hpp"
#include "wowcore/model_layers.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/effect.hpp"
#include "wowcore/item_preview.hpp"
#include "wowcore/mpq.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "ui/interface_fonts.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/soft_raster.hpp"
#include "addons/lua_engine.hpp"
extern "C" {
#include <lauxlib.h>
}

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <memory>
#include <limits>
#include <set>
#include <regex>
#include <string>

namespace {
std::shared_ptr<wowcore::Mpq> client() {
    static std::shared_ptr<wowcore::Mpq> mpq = [] {
        const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
        const std::filesystem::path dir = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
        return std::filesystem::exists(dir / "interface.MPQ") ? std::make_shared<wowcore::Mpq>(dir) : nullptr;
    }();
    return mpq;
}
}  // namespace

TEST_CASE("item previews require an explicit realm entry mapping", "[client][item-preview]") {
    auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    const auto unknown = wowcore::loadItemPreview(*mpq, 873, {});
    CHECK(unknown.status == wowcore::ItemPreviewStatus::MissingItem);
    CHECK(unknown.entry == 873);
    CHECK(unknown.parts.empty());
    const auto absent = wowcore::loadItemPreview(*mpq, 873, {{873, {0xffffffffu, 17}}});
    CHECK(absent.status == wowcore::ItemPreviewStatus::MissingDisplay);
    CHECK(absent.displayId == 0xffffffffu);
    CHECK(absent.parts.empty());
}

TEST_CASE("Tier 1 sets and iconic equipment dress every Classic race and sex", "[client][equipment]") {
    auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    // Golden displays from the pinned ClassicDB item_template/ItemSet fixture, not entry IDs.
    const std::vector<std::vector<wowcore::Worn>> sets{
        {{5, 30583}, {8, 30584}, {0, 31517}, {7, 30587}, {9, 30585}, {6, 30582}, {2, 30586}, {4, 30581}}, // arcanist
        {{5, 31722}, {7, 31724}, {8, 31725}, {4, 31797}, {9, 31726}, {0, 32790}, {6, 31729}, {2, 32016}}, // cenarion
        {{5, 31829}, {7, 31830}, {8, 31831}, {4, 31832}, {2, 31833}, {9, 31834}, {0, 31835}, {6, 31836}}, // earthfury
        {{5, 31969}, {8, 31970}, {9, 31971}, {6, 31972}, {4, 31973}, {2, 31974}, {0, 31987}, {7, 31975}}, // felheart
        {{5, 32019}, {7, 32040}, {8, 32021}, {4, 32022}, {2, 32030}, {9, 32024}, {0, 32028}, {6, 32029}}, // giantstalker
        {{5, 31353}, {7, 31354}, {8, 31509}, {4, 31505}, {9, 31507}, {0, 31506}, {6, 31352}, {2, 31510}}, // lawbringer
        {{5, 31019}, {8, 31020}, {4, 31021}, {9, 31022}, {0, 31260}, {6, 31023}, {2, 31024}, {7, 31025}}, // might
        {{5, 31339}, {7, 31109}, {8, 31106}, {4, 31105}, {0, 31514}, {9, 31503}, {6, 31340}, {2, 31504}}, // nightslayer
        {{7, 31718}, {0, 31371}, {5, 30621}, {9, 30620}, {6, 28198}, {2, 30623}, {4, 31490}, {8, 30617}}, // prophecy
        {{5, 30541}, {7, 30542}, {8, 30548}, {4, 30536}, {0, 33655}, {9, 34016}, {6, 30540}, {2, 30546}}, // stormrage
    };
    const std::vector<wowcore::Worn> weapons{{15, 30606, 1}, {15, 29698, 1}, {15, 32000, 1},
        {15, 34114, 2}, {17, 31338, 0}, {15, 31163, 2}, {15, 31347, 2}, {15, 31346, 2},
        {15, 33615, 3}, {15, 30994, 3}, {16, 30993, 4}};
    for (std::uint32_t race = 1; race <= 8; ++race) {
        for (std::uint32_t sex = 0; sex <= 1; ++sex) {
            const wowcore::Appearance who{race, sex, 0, 0, 0, 0, 0};
            INFO("race " << race << " sex " << sex);
            const auto bare = wowcore::dressCharacter(*mpq, who, {});
            const auto body = wowcore::loadModel(*mpq, bare.body);
            REQUIRE(body);
            std::vector<glm::mat4> bones;
            wowcore::poseBones(*body, wowcore::findSequence(*body, 0), 0, 0, bones);
            for (const auto& set : sets) {
                INFO("set helm display " << set[2].displayId);
                const auto look = wowcore::dressCharacter(*mpq, who, set);
                CHECK(look.skin.rgba != bare.skin.rgba);
                REQUIRE(look.attached.size() == 3); // helm and both shoulders
                for (const auto& item : look.attached) {
                    INFO(item.model);
                    REQUIRE(wowcore::loadModel(*mpq, item.model));
                    CHECK_FALSE(wowcore::loadImage(*mpq, item.texture).rgba.empty());
                    CHECK(wowcore::placeAttached(*body, bones, item, false));
                }
            }
            for (const auto& worn : weapons) {
                INFO("weapon/shield display " << worn.displayId);
                const auto look = wowcore::dressCharacter(*mpq, who, {worn});
                REQUIRE(look.attached.size() == 1);
                const auto& item = look.attached.front();
                REQUIRE(wowcore::loadModel(*mpq, item.model));
                CHECK_FALSE(wowcore::loadImage(*mpq, item.texture).rgba.empty());
                CHECK(wowcore::placeAttached(*body, bones, item, false));
                CHECK(wowcore::placeAttached(*body, bones, item, true));
                if (worn.slot == 17) CHECK(item.point == 26);
                if (worn.sheath == 4) CHECK(item.model.find("\\Shield\\") != std::string::npos);
            }
        }
    }
}

TEST_CASE("1.12 item previews load the staff helm and shoulder assets independently", "[client][item-preview]") {
    auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    // Realm export build/character.json: entry and display ID intentionally differ.
    const wowcore::ItemDisplayCatalog catalog{
        {873, {20298, 17}}, {16900, {33655, 1}}, {16902, {30546, 3}}, {16897, {30536, 5}},
        {22398, {34954, 28}}, {1, {0xffffffffu, 17}}
    };
    const wowcore::Appearance who{4, 1, 5, 8, 5, 3, 0};
    const auto dressed = wowcore::dressCharacter(*mpq, who, {{0, 33655}, {2, 30546}, {15, 20298, 2}});
    for (const auto entry : {873u, 16900u, 16902u}) {
        INFO(entry);
        auto preview = wowcore::loadItemPreview(*mpq, entry, catalog, who);
        INFO(preview.detail);
        REQUIRE(preview.status == wowcore::ItemPreviewStatus::Ready);
        CHECK(preview.entry == entry);
        CHECK(preview.displayId == catalog.at(entry).displayId);
        REQUIRE(preview.parts.size() == (entry == 16902 ? 2u : 1u));
        // A move into a render-thread snapshot keeps batch bindings valid (no interior pointers).
        auto moved = std::move(preview);
        for (const auto& part : moved.parts) {
            REQUIRE_FALSE(part.model.vertices.empty());
            REQUIRE_FALSE(part.model.indices.empty());
            REQUIRE_FALSE(part.images.empty());
            REQUIRE(part.batchTextures.size() == part.model.batches.size());
            for (const auto& texture : part.batchTextures) {
                REQUIRE(part.images.count(texture) == 1);
                const auto& image = part.images.at(texture);
                CHECK(image.w > 0);
                CHECK(image.h > 0);
                CHECK(image.rgba.size() == static_cast<size_t>(image.w) * image.h * 4);
            }
            // The standalone part is the same M2 already used by Avatar's dressing path.
            CHECK(std::any_of(dressed.attached.begin(), dressed.attached.end(), [&](const auto& attached) {
                auto model = wowcore::loadModel(*mpq, attached.model);
                return model && model->name == part.model.name;
            }));
        }
    }
    CHECK(wowcore::loadItemPreview(*mpq, 20298, catalog).status == wowcore::ItemPreviewStatus::MissingItem);
    CHECK(wowcore::loadItemPreview(*mpq, 1, catalog).status == wowcore::ItemPreviewStatus::MissingDisplay);
    CHECK(wowcore::loadItemPreview(*mpq, 16897, catalog).status == wowcore::ItemPreviewStatus::NoStandaloneModel);
    CHECK(wowcore::loadItemPreview(*mpq, 22398, catalog).status == wowcore::ItemPreviewStatus::UnsupportedInventoryType);
    CHECK(wowcore::loadItemPreview(*mpq, 16900, catalog, {0, 1}).status == wowcore::ItemPreviewStatus::InvalidAppearance);
    CHECK(wowcore::loadItemPreview(*mpq, 16900, catalog, {4, 2}).status == wowcore::ItemPreviewStatus::InvalidAppearance);
    // Incorrect type resolves to Shield rather than silently loading a weapon from the wrong folder.
    const auto wrongFolder = wowcore::loadItemPreview(*mpq, 873, {{873, {20298, 14}}});
    CHECK(wrongFolder.status == wowcore::ItemPreviewStatus::MissingModel);
    CHECK(wrongFolder.parts.empty());
}

TEST_CASE("1.12 chestguard preview contains only torso and sleeves with the item's textures", "[client][item-preview]") {
    const auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    const wowcore::ItemDisplayCatalog catalog{{16897, {30536, 5}}, {873, {20298, 17}}, {1, {0xffffffffu, 5}}};
    const wowcore::Appearance who{4, 1, 5, 8, 5, 3, 0};
    auto preview = wowcore::loadArmorItemPreview(*mpq, 16897, catalog, who);
    INFO(preview.detail);
    REQUIRE(preview.status == wowcore::ItemPreviewStatus::Ready);
    REQUIRE(preview.armorOnly);
    REQUIRE(preview.parts.size() == 1);  // No helm, shoulders, weapons or other outfit parts.
    const auto& part = preview.parts.front();
    const auto& body = part.model;
    CHECK(body.vertices.size() < 65536);
    std::set<uint16_t> geosets;
    bool torso = false, sleeve = false;
    for (const auto& batch : body.batches) {
        geosets.insert(batch.submeshId);
        REQUIRE(batch.indexCount > 0);
        CHECK(body.materials.at(batch.materialIndex).blendMode == 1);
        for (uint32_t i = batch.indexStart; i < batch.indexStart + batch.indexCount; ++i) {
            const auto uv = body.vertices.at(body.indices.at(i)).texCoords[0];
            // The chestguard paints upper/lower arms and upper/lower torso: no hand, head, legs or feet.
            const bool arm = uv.x <= 0.5f && uv.y <= 0.5f;
            const bool chest = uv.x >= 0.5f && uv.y <= 0.375f;
            CHECK((arm || chest));
            sleeve = sleeve || uv.x < 0.5f;
            torso = torso || uv.x > 0.5f;
        }
    }
    CHECK(torso);
    CHECK(sleeve);
    CHECK(geosets == std::set<uint16_t>{0, 401});  // Tight torso/upper sleeves in 0, lower sleeves in bare 401.
    const auto& atlas = part.images.at("body:item");
    REQUIRE(atlas.rgba.size() == 256 * 256 * 4);
    // Face/hair, hands, legs and feet never receive skin, underwear or unrelated item pixels.
    for (int y = 0; y < 256; ++y) for (int x = 0; x < 256; ++x) {
        if ((x < 128 && y >= 128) || (x >= 128 && y >= 96))
            CHECK(atlas.rgba[(y * 256 + x) * 4 + 3] == 0);
    }
    // Skin, face and hair choices do not affect a piece; only race/sex supplies its geometry.
    const auto otherLook = wowcore::loadArmorItemPreview(*mpq, 16897, catalog, {4, 1, 999, 999, 999, 999, 999});
    REQUIRE(otherLook.status == wowcore::ItemPreviewStatus::Ready);
    CHECK(otherLook.parts.front().images.at("body:item").rgba == atlas.rgba);
    CHECK(otherLook.parts.front().model.indices == body.indices);
    const auto moved = std::move(preview);
    REQUIRE(moved.parts.front().batchTextures.size() == moved.parts.front().model.batches.size());
    for (const auto& path : moved.parts.front().batchTextures) REQUIRE_FALSE(moved.parts.front().images.at(path).rgba.empty());
    for (const auto& failed : {
        wowcore::loadArmorItemPreview(*mpq, 16897, {}, who),
        wowcore::loadArmorItemPreview(*mpq, 1, catalog, who),
        wowcore::loadArmorItemPreview(*mpq, 873, catalog, who),
        wowcore::loadArmorItemPreview(*mpq, 16897, catalog, {0, 1}),
        wowcore::loadArmorItemPreview(*mpq, 16897, catalog, {4, 2})
    }) {
        CHECK(failed.status != wowcore::ItemPreviewStatus::Ready);
        CHECK(failed.parts.empty());
        CHECK_FALSE(failed.armorOnly);
    }
}

TEST_CASE("1.12 body armour slots keep only their covered geometry", "[client][item-preview]") {
    const auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    const wowcore::ItemDisplayCatalog catalog{{16903, {30541, 6}}, {16901, {30540, 7}}, {16898, {30542, 8}},
                                             {16904, {30548, 9}}, {16899, {34016, 10}}};
    const std::map<uint32_t, std::set<uint16_t>> expectedGeosets{
        {16903, {0}}, {16901, {0, 501, 1302}}, {16898, {0, 502}}, {16904, {401}}, {16899, {0, 402}}
    };
    // Region rectangles independently match the atlas columns each reference item supplies.
    const std::map<uint32_t, std::vector<glm::vec4>> coverage{
        {16903, {{0.5f, 0.375f, 1, 0.625f}}},  // belt: LegUpper (hips), not a whole torso
        {16901, {{0.5f, 0.375f, 1, 0.875f}}},  // legs: LegUpper/Lower, including the kilt
        {16898, {{0.5f, 0.625f, 1, 1}}},       // boots: LegLower/Foot
        {16904, {{0, 0.25f, 0.5f, 0.5f}}},    // wrists: ArmLower
        {16899, {{0, 0.25f, 0.5f, 0.625f}}}   // gloves: ArmLower/Hand
    };
    for (const auto& [entry, item] : catalog) {
        INFO(entry);
        const auto preview = wowcore::loadArmorItemPreview(*mpq, entry, catalog, {});
        INFO(preview.detail);
        REQUIRE(preview.status == wowcore::ItemPreviewStatus::Ready);
        REQUIRE(preview.parts.size() == 1);
        const auto& model = preview.parts.front().model;
        std::set<uint16_t> geosets;
        for (const auto& batch : model.batches) geosets.insert(batch.submeshId);
        CHECK(geosets == expectedGeosets.at(entry));
        glm::vec3 lo = model.vertices.front().position, hi = lo;
        for (const auto& vertex : model.vertices) {
            const auto uv = vertex.texCoords[0];
            CHECK(std::any_of(coverage.at(entry).begin(), coverage.at(entry).end(), [&](const auto& rect) {
                return uv.x >= rect.x && uv.y >= rect.y && uv.x <= rect.z && uv.y <= rect.w;
            }));
            lo = glm::min(lo, vertex.position); hi = glm::max(hi, vertex.position);
        }
        // Framing is based on these compact vertices, not the original figure's unreferenced head/feet.
        CHECK(hi.z - lo.z < 1.5f);
        if (entry == 16903) CHECK(hi.z - lo.z < 0.2f);
        if (entry == 16904) CHECK(hi.z - lo.z < 0.4f);
    }
}

TEST_CASE("body item geosets belong to the selected slot", "[item-preview]") {
    using wowcore::itemBodyGeosets;
    CHECK(itemBodyGeosets(5, {2, 1, 1}) == std::set<uint16_t>{0, 401, 501, 803, 901, 1002, 1302});
    CHECK(itemBodyGeosets(20, {2, 1, 1}) == itemBodyGeosets(5, {2, 1, 1}));
    CHECK(itemBodyGeosets(7, {1, 0, 1}) == std::set<uint16_t>{0, 401, 501, 801, 902, 1001, 1302});
    CHECK(itemBodyGeosets(10, {1, 0, 0}) == std::set<uint16_t>{0, 402, 501, 801, 901, 1001, 1301});
    CHECK(itemBodyGeosets(8, {2, 0, 0}) == std::set<uint16_t>{0, 401, 503, 801, 901, 1001, 1301});
    CHECK(itemBodyGeosets(9, {1, 0, 0}) == std::set<uint16_t>{0, 401, 501, 802, 901, 1001, 1301});
    CHECK(itemBodyGeosets(6, {}) == std::set<uint16_t>{0, 401, 501, 801, 901, 1001, 1301});
    CHECK(itemBodyGeosets(16, {1, 0, 0}) == std::set<uint16_t>{1503});
    CHECK(itemBodyGeosets(19, {1, 0, 0}) == std::set<uint16_t>{1202});
    CHECK(itemBodyGeosets(1, {}).empty());
}

TEST_CASE("body item clipping removes unrelated regions and geosets, including crossing triangles", "[item-preview]") {
    wowcore::Model model{};
    const auto triangle = [&](uint16_t geoset, const std::array<glm::vec2, 3>& uv) {
        wowee::pipeline::M2Batch batch{};
        batch.submeshId = geoset;
        batch.indexStart = static_cast<uint32_t>(model.indices.size());
        batch.indexCount = 3;
        for (const auto& at : uv) {
            wowee::pipeline::M2Vertex vertex{};
            vertex.position = {at.x, at.y, 0};
            vertex.normal = {0, 0, 1};
            vertex.texCoords[0] = at;
            model.indices.push_back(static_cast<uint16_t>(model.vertices.size()));
            model.vertices.push_back(vertex);
        }
        model.batches.push_back(batch);
    };
    triangle(0, {{{0.6f, 0.1f}, {0.8f, 0.1f}, {0.6f, 0.2f}}}); // torso
    triangle(801, {{{0.1f, 0.1f}, {0.3f, 0.1f}, {0.1f, 0.2f}}}); // sleeve
    triangle(0, {{{0.1f, 0.8f}, {0.3f, 0.8f}, {0.1f, 0.9f}}}); // head
    triangle(0, {{{0.6f, 0.6f}, {0.8f, 0.6f}, {0.6f, 0.7f}}}); // leg
    triangle(501, {{{0.6f, 0.9f}, {0.8f, 0.9f}, {0.6f, 0.95f}}}); // foot
    triangle(702, {{{0.6f, 0.1f}, {0.8f, 0.1f}, {0.6f, 0.2f}}}); // ears, even with torso UVs
    triangle(802, {{{0.1f, 0.1f}, {0.3f, 0.1f}, {0.1f, 0.2f}}}); // unselected sleeve variant
    triangle(0, {{{0.6f, 0.3f}, {0.8f, 0.3f}, {0.6f, 0.5f}}}); // crosses torso/leg boundary
    const wowcore::ItemBodySelection selection{{{0, 0, 0.5f, 0.5f}, {0.5f, 0, 1, 0.375f}}, wowcore::itemBodyGeosets(5, {})};
    auto empty = model;
    CHECK_FALSE(wowcore::isolateItemBody(empty, {}));
    CHECK(empty.indices == model.indices);  // Failed isolation never publishes a partial mesh.
    auto invalid = model;
    invalid.indices[0] = 65535;
    CHECK_FALSE(wowcore::isolateItemBody(invalid, selection));
    CHECK(invalid.indices[0] == 65535);
    REQUIRE(wowcore::isolateItemBody(model, selection));
    REQUIRE(model.batches.size() == 3);
    CHECK(model.batches[0].submeshId == 0);
    CHECK(model.batches[1].submeshId == 801);
    CHECK(model.batches[2].indexCount == 6); // Crossing triangle becomes two triangles, cut at the atlas seam.
    for (const auto& vertex : model.vertices) {
        const auto uv = vertex.texCoords[0];
        CHECK((uv.x <= 0.5f ? uv.y <= 0.5f : uv.y <= 0.375f));
        CHECK(vertex.position.x == Catch::Approx(uv.x));
        CHECK(vertex.position.y == Catch::Approx(uv.y));
    }
}

TEST_CASE("character preview policies cover body armour slots and retain standalone previews", "[item-preview]") {
    const std::pair<uint32_t, uint32_t> slots[] = {{4, 3}, {5, 4}, {20, 4}, {6, 5}, {7, 6}, {8, 7}, {9, 8}, {10, 9}, {16, 14}, {19, 18}};
    for (const auto& [type, slot] : slots) CHECK(wowcore::characterPreviewSlot(type) == slot);
    for (const auto type : {0u, 1u, 2u, 3u, 11u, 12u, 13u, 14u, 15u, 17u, 21u, 22u, 23u, 25u, 26u, 28u})
        CHECK_FALSE(wowcore::characterPreviewSlot(type));
}

TEST_CASE("1.12 archive set opens in client priority order", "[client]") {
    auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    REQUIRE(mpq->archiveCount() == 14);
    auto toc = mpq->read("Interface\\FrameXML\\FrameXML.toc");
    REQUIRE(toc);
    const std::string text(toc->begin(), toc->end());
    CHECK(text.find("UIParent.xml") != std::string::npos);
    CHECK(mpq->exists("interface/framexml/uiparent.lua"));          // case and slash insensitive
    CHECK(mpq->exists("Fonts\\FRIZQT__.TTF"));
    CHECK(mpq->list("Interface\\FrameXML\\*.xml").size() > 50);
}

TEST_CASE("slice spell icons decode from BLP", "[client]") {
    auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    for (const char* icon : {"Spell_Nature_AbolishMagic", "Spell_Nature_StarFall", "Spell_Nature_Rejuvenation",
                             "Spell_Nature_HealingTouch", "Spell_Nature_StrangleVines", "Spell_Nature_Cyclone"}) {
        auto blp = mpq->read(std::string("Interface\\Icons\\") + icon + ".blp");
        REQUIRE(blp);
        auto img = wowee::pipeline::BLPLoader::load(*blp);
        INFO(icon);
        REQUIRE(img.isValid());
        CHECK(img.width == 64);
        CHECK(img.height == 64);
        CHECK(img.data.size() == 64u * 64u * 4u);
    }
}

TEST_CASE("every generated Skyrim icon decodes through the engine and loose overlay", "[client][skyrim-icons]") {
    auto base = client();
    const auto directory = std::filesystem::path(WOWBRIDGE_ROOT) / "build/skyrim_icons";
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    std::unique_ptr<wowcore::Mpq> overlay;
    if (base) overlay = std::make_unique<wowcore::Mpq>(env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data", directory);
    std::ifstream manifest(directory / "manifest.json");
    REQUIRE(manifest.good());
    const std::string json((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
    const std::regex icon(R"re("icon"\s*:\s*"(WB_[A-Za-z0-9_]+)")re");
    std::set<std::string> expected;
    for (auto it = std::sregex_iterator(json.begin(), json.end(), icon); it != std::sregex_iterator(); ++it)
        expected.insert((*it)[1].str() + ".blp");
    REQUIRE_FALSE(expected.empty());
    std::set<std::string> actual;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".blp") continue;
        actual.insert(entry.path().filename().string());
        const auto path = "Interface\\Icons\\" + entry.path().filename().string();
        INFO(path);
        std::ifstream input(entry.path(), std::ios::binary);
        REQUIRE(input.good());
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (overlay) {
            REQUIRE(overlay->exists(path));
            REQUIRE(overlay->read(path) == bytes);
        }
        REQUIRE(bytes.size() >= 148 + 1024);
        CHECK(std::string(bytes.begin(), bytes.begin() + 4) == "BLP2");
        const auto image = wowee::pipeline::BLPLoader::load(bytes, false);
        REQUIRE(image.isValid());
        CHECK(image.width == 64);
        CHECK(image.height == 64);
        REQUIRE(image.data.size() == 64u * 64u * 4u);
        for (size_t i = 3; i < image.data.size(); i += 4)
            if (image.data[i] != 255) FAIL("icon art must be opaque and full bleed");
    }
    CHECK(actual == expected);
    if (!overlay) return;  // decoding above still runs without installed client archives
    CHECK(overlay->list("interface/icons/wb_*.blp").size() == expected.size());
    // Overlay does not change the existing FrameXML or MPQ icon lookups.
    CHECK(overlay->read("Interface\\FrameXML\\UIParent.xml") == base->read("Interface\\FrameXML\\UIParent.xml"));
    CHECK(overlay->read("Interface\\Icons\\Spell_Nature_HealingTouch.blp") == base->read("Interface\\Icons\\Spell_Nature_HealingTouch.blp"));
}

TEST_CASE("loose icons precede MPQs with case and slash insensitive paths", "[client][skyrim-icons]") {
    const auto base = client();
    if (!base) SKIP("WoW 1.12 client data not present");
    struct Temporary {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("wowcore-icons-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Temporary() { std::filesystem::create_directory(path); }
        ~Temporary() { std::filesystem::remove_all(path); }
    } temp;
    const auto replacement = base->read("Interface\\Icons\\Spell_Fire_Fire.blp");
    REQUIRE(replacement);
    const auto file = temp.path / "Spell_Nature_HealingTouch.blp";
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(replacement->data()), replacement->size());
    std::ofstream(temp.path / "manifest.json") << "{}";
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const auto data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    wowcore::Mpq overlay(data, temp.path);
    CHECK(overlay.read("INTERFACE/icons/spell_nature_healingtouch.BLP") == replacement);
    CHECK(overlay.exists("interface/ICONS/SPELL_NATURE_HEALINGTOUCH.blp"));
    CHECK(overlay.list("interface/icons/spell_nature_healingtouch.blp").size() == 1);
    CHECK_FALSE(overlay.read("Interface/Icons/../manifest.json"));
    CHECK_FALSE(overlay.read("Interface/Icons/manifest.json"));
    CHECK_FALSE(overlay.read("Interface/Icons/./Spell_Nature_HealingTouch.blp"));
    CHECK(overlay.read("Fonts/FRIZQT__.TTF") == base->read("Fonts/FRIZQT__.TTF"));
    std::filesystem::remove(file);
    CHECK_FALSE(overlay.read("Interface/Icons/Spell_Nature_HealingTouch.blp"));
    CHECK_FALSE(overlay.exists("Interface/Icons/Spell_Nature_HealingTouch.blp"));
    // A missing optional directory preserves the original archive behaviour.
    wowcore::Mpq missing(data, temp.path / "absent");
    CHECK(missing.read("Interface/Icons/Spell_Nature_HealingTouch.blp") == base->read("Interface/Icons/Spell_Nature_HealingTouch.blp"));
}

TEST_CASE("M2: the night elf female loads from the client (vanilla v256, embedded view)", "[client][m2]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "model.MPQ")) SKIP("WoW 1.12 client data not present");
    const wowcore::Mpq mpq(data);
    const auto bytes = mpq.read("Character\\NightElf\\Female\\NightElfFemale.m2");
    REQUIRE(bytes);
    const auto m = wowee::pipeline::M2Loader::load(*bytes);
    std::printf("NightElfFemale: version %u verts %zu indices %zu bones %zu sequences %zu batches %zu textures %zu attachments %zu\n",
                m.version, m.vertices.size(), m.indices.size(), m.bones.size(), m.sequences.size(), m.batches.size(),
                m.textures.size(), m.attachments.size());
    for (size_t i = 0; i < m.textures.size(); ++i) std::printf("  texture %zu type %u '%s'\n", i, m.textures[i].type, m.textures[i].filename.c_str());
    std::printf("  bounds %.3f %.3f %.3f .. %.3f %.3f %.3f\n", m.boundMin.x, m.boundMin.y, m.boundMin.z, m.boundMax.x, m.boundMax.y, m.boundMax.z);
    std::set<uint16_t> geosets;
    for (const auto& b : m.batches) geosets.insert(b.submeshId);
    std::printf("  geosets:");
    for (auto g : geosets) std::printf(" %u", g);
    for (const auto& c : m.cameras)
        std::printf("  camera type %d fov %.3f near %.2f far %.2f pos %.3f %.3f %.3f target %.3f %.3f %.3f\n", c.type, c.fov, c.nearClip, c.farClip,
                    c.positionBase.x, c.positionBase.y, c.positionBase.z, c.targetBase.x, c.targetBase.y, c.targetBase.z);
    for (size_t i = 0; i < m.sequences.size(); ++i) {
        const auto id = m.sequences[i].id;
        if ((id < 51 || id > 56) && id != 125 && id != 126) continue;
        std::printf("  sequence %zu (anim %u, %u ms) events:", i, id, m.sequences[i].duration);
        if (i < m.sequenceEvents.size()) for (const auto& [ev, at] : m.sequenceEvents[i]) std::printf(" %s@%u", ev.c_str(), at);
        std::printf("\n");
    }
    std::printf("\n  attachments:");
    for (const auto& a : m.attachments) std::printf(" %u", a.id);
    std::printf("\n");
    CHECK(m.version == 256);
    CHECK_FALSE(m.vertices.empty());
    CHECK_FALSE(m.batches.empty());
    // Her staff's swing (Attack2HL, its first variation): the whoosh at 667 ms, the blow landing at 734 of 1500.
    const int swing = wowcore::findSequence(m, 19);
    REQUIRE(swing >= 0);
    CHECK(m.sequences[swing].duration == 1500);
    CHECK(wowcore::eventTime(m, swing, "$CSS") == 667u);
    CHECK(wowcore::eventTime(m, swing, "$HIT") == 734u);
    CHECK_FALSE(wowcore::eventTime(m, wowcore::findSequence(m, 0), "$HIT"));
}

TEST_CASE("Character: Elunara dressed as the client dresses her", "[client][m2]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "model.MPQ")) SKIP("WoW 1.12 client data not present");
    const wowcore::Mpq mpq(data);
    // The reference realm's character: night elf female, skin 5, face 8, hair 5 colour 3; Stormrage, Staff of Jordan,
    // and the Idol of Rejuvenation in the relic slot, which the client does not draw.
    const auto look = wowcore::dressCharacter(mpq, { 4, 1, 5, 8, 5, 3, 0 },
        { {0, 33655}, {2, 30546}, {4, 30536}, {5, 30541}, {6, 30540}, {7, 30542}, {8, 30548}, {9, 34016}, {15, 20298, 2},
          {17, 34954} });
    CHECK(look.body == "Character\\NightElf\\Female\\NightElfFemale.m2");
    // The hood hides her hair (HelmetGeosetVisData 305); gloves 402, boots 502, the legguards' kilt 1302, no cape.
    CHECK(look.geosets == std::set<uint16_t>{ 0, 402, 502, 702, 801, 901, 1001, 1302, 1501 });
    CHECK((look.skin.w == 256 && look.skin.h == 256));
    REQUIRE(look.attached.size() == 4);
    CHECK(look.attached[0].model == "Item\\ObjectComponents\\Head\\Helm_Leather_RaidDruid_B_01_NiF.m2");
    CHECK((look.attached[1].point == 6 && look.attached[2].point == 5));
    CHECK((look.attached[3].point == 27 && look.attached[3].sheathed));
    const auto body = wowcore::loadModel(mpq, look.body);
    REQUIRE(body);
    std::vector<glm::mat4> bones;
    wowcore::poseBones(*body, wowcore::findSequence(*body, 0), 0.0f, 0.0f, bones);
    CHECK(wowcore::placeAttached(*body, bones, look.attached[3], false).has_value());
}

TEST_CASE("Character: male Night Elf appearance, Stormrage, animations and item previews", "[client][m2][male]") {
    const auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    const wowcore::Appearance who{4, 0, 5, 0, 3, 3, 1};  // data/character-male.json, reviewed with model_render
    const auto sections = wowcore::Dbc::load(*mpq, "DBFilesClient\\CharSections.dbc");
    REQUIRE(sections);
    // Every layer exists for these male options: skin, face, beard, scalp/hair and underwear.
    for (uint32_t base = 0; base < 5; ++base) {
        const uint32_t variation = base == 1 ? who.face : base == 2 ? who.facialHair : base == 3 ? who.hairStyle : 0;
        const uint32_t color = base == 2 || base == 3 ? who.hairColor : who.skin;
        bool found = false;
        for (uint32_t r = 0; r < sections->rows(); ++r) {
            if (sections->u32(r, 1) != who.race || sections->u32(r, 2) != who.sex ||
                sections->u32(r, 3) != base || sections->u32(r, 4) != variation || sections->u32(r, 5) != color) continue;
            found = true;
            for (uint32_t c = 6; c < 9; ++c) {
                if (*sections->str(r, c)) CHECK_FALSE(wowcore::loadImage(*mpq, sections->str(r, c)).rgba.empty());
            }
        }
        INFO("CharSections base " << base);
        CHECK(found);
    }
    const auto bare = wowcore::dressCharacter(*mpq, who, {});
    const auto look = wowcore::dressCharacter(*mpq, who,
        {{0, 33655}, {2, 30546}, {4, 30536}, {5, 30541}, {6, 30540}, {7, 30542}, {8, 30548}, {9, 34016}, {15, 20298, 2}});
    CHECK(look.body == "Character\\NightElf\\Male\\NightElfMale.m2");
    CHECK(bare.geosets.count(5));  // ponytail; helmet hides it but keeps the selected beard/brows
    CHECK(look.geosets.count(5) == 0);
    CHECK(look.geosets == std::set<uint16_t>{0, 102, 202, 302, 402, 502, 702, 801, 901, 1001, 1302, 1501});
    CHECK(bare.skin.rgba != look.skin.rgba);
    REQUIRE(look.attached.size() == 4);
    CHECK(look.attached.front().model == "Item\\ObjectComponents\\Head\\Helm_Leather_RaidDruid_B_01_NiM.m2");
    for (const auto& item : look.attached) REQUIRE(wowcore::loadModel(*mpq, item.model));
    const auto model = wowcore::loadModel(*mpq, look.body);
    REQUIRE(model);
    CHECK(std::ranges::any_of(model->cameras, [](const auto& c) { return c.type == 0; }));
    for (const uint32_t id : {0u, 4u, 5u, 13u, 16u, 17u, 18u, 19u, 25u, 26u, 27u, 28u,
                            37u, 38u, 39u, 40u, 41u, 42u, 45u, 51u, 52u, 53u, 54u, 55u,
                            66u, 69u, 74u, 99u, 100u, 101u, 103u}) {
        INFO("male animation " << id);
        const int seq = wowcore::findSequence(*model, id);
        REQUIRE(seq >= 0);
        std::vector<glm::mat4> bones;
        wowcore::poseBones(*model, seq, 250.0f, 250.0f, bones);
        REQUIRE(bones.size() == model->bones.size());
        for (const auto& item : look.attached) CHECK(wowcore::placeAttached(*model, bones, item, true));
    }
    const int swing = wowcore::findSequence(*model, 19);
    CHECK(wowcore::eventTime(*model, swing, "$CSS"));
    CHECK(wowcore::eventTime(*model, swing, "$HIT"));
    const wowcore::ItemDisplayCatalog catalog{{16900, {33655, 1}}, {16897, {30536, 5}}};
    const auto helm = wowcore::loadItemPreview(*mpq, 16900, catalog, who);
    REQUIRE(helm.status == wowcore::ItemPreviewStatus::Ready);
    CHECK(helm.parts.front().model.name.find("NiM") != std::string::npos);
    const auto armor = wowcore::loadArmorItemPreview(*mpq, 16897, catalog, who);
    REQUIRE(armor.status == wowcore::ItemPreviewStatus::Ready);
    CHECK(armor.armorOnly);
    REQUIRE(armor.parts.size() == 1);
    CHECK(armor.parts.front().model.name == look.body);
    CHECK_FALSE(armor.parts.front().images.at("body:item").rgba.empty());
}

TEST_CASE("Character: mapped Skyrim torso and hood cover the body and hide hair", "[client][m2][skyrim-gear]") {
    auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    const wowcore::Appearance who{4, 1, 5, 8, 5, 3, 0};
    const auto bare = wowcore::dressCharacter(*mpq, who, {});
    const auto chest = wowcore::dressCharacter(*mpq, who, {{4, 24393}});
    const std::vector<wowcore::Worn> ironItems{{0, 15340}, {4, 24393}, {6, 25833}};
    const auto iron = wowcore::dressCharacter(*mpq, who, ironItems);
    CHECK(iron.skin.rgba != bare.skin.rgba);
    CHECK(iron.skin.rgba != chest.skin.rgba);  // coordinated leggings cover the chest's exposed leg regions
    REQUIRE(iron.attached.size() == 1);
    CHECK(iron.attached.front().point == 11);
    CHECK(wowcore::loadModel(*mpq, iron.attached.front().model).has_value());
    const auto robe = wowcore::dressCharacter(*mpq, who, {{0, 15278}, {4, 27856}});
    CHECK(robe.geosets.count(1302) == 1);
    REQUIRE(robe.attached.size() == 1);
    CHECK(wowcore::loadModel(*mpq, robe.attached.front().model).has_value());
    bool bareHasHair = false;
    for (const auto g : bare.geosets) {
        if (g > 0 && g < 100) {
            bareHasHair = true;
            CHECK(robe.geosets.count(g) == 0);  // HelmetGeosetVisData hides the hairstyle under the hood
        }
    }
    CHECK(bareHasHair);
    const auto restored = wowcore::dressCharacter(*mpq, who, {});
    CHECK(restored.geosets == bare.geosets);
    CHECK(restored.skin.rgba == bare.skin.rgba);
}

TEST_CASE("Creature: druid display models, replaceable skins, geosets and scale", "[client][m2][forms]") {
    const auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    struct Form { uint32_t display; const char* folder; const char* skin; float scale; };
    for (const auto& form : { Form{892, "DruidCat", "DruidCatSkinBlack", 0.9f},
                              Form{2281, "DruidBear", "DruidBearSkin", 1.0f},
                              Form{632, "Tiger", "TigerSkinYellow", 1.15f},
                              Form{2428, "SeaLion", "SeaLionSkin", 1.0f} }) {
        INFO("display " << form.display);
        const auto look = wowcore::dressCreature(*mpq, form.display);
        REQUIRE(look);
        const std::string dir = std::string("Creature\\") + form.folder + "\\";
        CHECK(look->body == dir + form.folder + ".m2");
        CHECK(look->replaceableTextures.at(11) == dir + form.skin + ".blp");
        CHECK(std::abs(look->scale - form.scale) < 1e-6f);
        CHECK(look->attached.empty());
        CHECK(look->skin.rgba.empty());
        CHECK(look->geosets == std::set<uint16_t>{0});
        const auto model = wowcore::loadModel(*mpq, look->body);
        REQUIRE(model);
        CHECK_FALSE(model->vertices.empty());
        for (const auto& texture : model->textures) {
            if (texture.type < 11 || texture.type > 13) continue;
            REQUIRE(look->replaceableTextures.count(texture.type));
            const auto image = wowcore::loadImage(*mpq, look->replaceableTextures.at(texture.type));
            CHECK_FALSE(image.rgba.empty());
        }
        for (const auto& batch : model->batches) CHECK(look->geosets.count(batch.submeshId));
        if (form.display == 892) {
            CHECK(look->replaceableTextures.at(12) == dir + "DruidCatSkinBlack.blp");
            CHECK(look->replaceableTextures.at(13) == dir + "DruidCatEyeGlow.blp");
            // The actual world-renderer fallback must animate the cat's legs.
            CHECK(wowcore::findSequence(*model,119)==-1);
            const int walk=wowcore::sequenceWithFallback(*model,119);
            REQUIRE(walk>=0); CHECK(model->sequences[walk].id==4);
            std::vector<glm::mat4> first,last;
            const float duration=static_cast<float>(model->sequences[walk].duration);
            wowcore::poseBones(*model,walk,duration*.2f,0,first);
            wowcore::poseBones(*model,walk,duration*.65f,0,last);
            REQUIRE(first.size()==last.size());
            float motion=0;
            for (size_t i=0;i<first.size();++i) for (int c=0;c<4;++c) for (int r=0;r<4;++r)
                motion=std::max(motion,std::abs(first[i][c][r]-last[i][c][r]));
            CHECK(motion>.01f);
        }
    }
    CHECK_FALSE(wowcore::dressCreature(*mpq, 0));
    CHECK_FALSE(wowcore::dressCreature(*mpq, 0xffffffff));
}

namespace {
void checkMatrix(const glm::mat4& actual, const glm::mat4& expected, float tolerance = 1e-5f) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) CHECK(std::abs(actual[c][r] - expected[c][r]) <= tolerance);
}
}

TEST_CASE("M2: transitions blend each local bone before composing the NightElfFemale hierarchy", "[client][m2][blend]") {
    const auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    const auto model = wowcore::loadModel(*mpq, "Character\\NightElf\\Female\\NightElfFemale.m2");
    REQUIRE(model);
    const auto& m = *model;
    const int stand = wowcore::findSequence(m, 0), walk = wowcore::findSequence(m, 4);
    const int run = wowcore::findSequence(m, 5), cast = wowcore::findSequence(m, 52);
    REQUIRE(stand >= 0);
    REQUIRE(walk >= 0);
    REQUIRE(run >= 0);
    REQUIRE(cast >= 0);
    for (const auto [from, to] : { std::pair{stand, run}, {run, stand}, {stand, walk}, {cast, stand}, {stand, cast} }) {
        INFO("from " << m.sequences[from].id << " to " << m.sequences[to].id);
        CHECK(wowcore::blendDurationMs(m, from, to) == m.sequences[to].blendTime);
        std::printf("blend anim %u -> %u: %u ms (from %u ms)\n", m.sequences[from].id, m.sequences[to].id,
                    wowcore::blendDurationMs(m, from, to), m.sequences[from].blendTime);
    }
    CHECK(wowcore::blendDurationMs(m, stand, stand) == 0);
    CHECK(wowcore::blendDurationMs(m, -1, run) == 0);
    CHECK(wowcore::blendDurationMs(m, stand, static_cast<int>(m.sequences.size())) == 0);

    constexpr float fromMs = 350.0f, toMs = 120.0f, globalMs = 470.0f;
    std::vector<glm::mat4> from, to, blended;
    wowcore::poseBones(m, stand, fromMs, globalMs, from);
    wowcore::poseBones(m, run, toMs, globalMs, to);
    REQUIRE(from.size() == m.bones.size());
    for (float progress : { -0.5f, 0.0f, 1.0f, 1.5f }) {
        wowcore::poseBonesBlended(m, stand, fromMs, run, toMs, progress, globalMs, blended);
        REQUIRE(blended.size() == from.size());
        for (size_t i = 0; i < from.size(); ++i) checkMatrix(blended[i], progress <= 0 ? from[i] : to[i], 0.0f);
    }
    wowcore::poseBonesBlended(m, stand, fromMs, run, toMs, std::numeric_limits<float>::quiet_NaN(), globalMs, blended);
    for (size_t i = 0; i < from.size(); ++i) checkMatrix(blended[i], from[i], 0.0f);
    size_t movingBones = 0;
    for (float progress : { 0.25f, 0.5f, 0.75f }) {
        const float weight = progress == 0.25f ? 0.15625f : progress == 0.75f ? 0.84375f : 0.5f;
        wowcore::poseBonesBlended(m, stand, fromMs, run, toMs, progress, globalMs, blended);
        for (size_t i = 0; i < m.bones.size(); ++i) {
            INFO("bone " << i << " progress " << progress);
            const auto& b = m.bones[i];
            // Recover the local bone from its parent, so the check detects matrix/palette blending.
            const glm::mat4 local = b.parentBone >= 0 && static_cast<size_t>(b.parentBone) < i
                ? glm::inverse(blended[b.parentBone]) * blended[i] : blended[i];
            namespace track = wowee::rendering::m2_track;
            const auto& globals = m.globalSequenceDurations;
            const auto t0 = track::sampleVec3(b.translation, stand, fromMs, globalMs, globals, glm::vec3(0));
            const auto t1 = track::sampleVec3(b.translation, run, toMs, globalMs, globals, glm::vec3(0));
            const auto s0 = track::sampleVec3(b.scale, stand, fromMs, globalMs, globals, glm::vec3(1));
            const auto s1 = track::sampleVec3(b.scale, run, toMs, globalMs, globals, glm::vec3(1));
            const auto r0 = track::sampleQuat(b.rotation, stand, fromMs, globalMs, globals);
            const auto r1 = track::sampleQuat(b.rotation, run, toMs, globalMs, globals);
            const auto t = t0 * (1.0f - weight) + t1 * weight;
            const auto s = s0 * (1.0f - weight) + s1 * weight;
            CHECK(glm::length(glm::vec3(local * glm::vec4(b.pivot, 1)) - b.pivot - t) < 2e-5f);
            glm::mat3 rotation;
            for (int k = 0; k < 3; ++k) {
                CHECK(std::abs(glm::length(glm::vec3(local[k])) - s[k]) < 2e-5f);
                rotation[k] = glm::vec3(local[k]) / s[k];
            }
            const auto r = glm::normalize(glm::quat_cast(rotation));
            const float angle = std::acos(std::clamp(std::abs(glm::dot(r0, r1)), 0.0f, 1.0f));
            // Shortest-arc rotation is between the two endpoints, with proportional angular distance.
            CHECK(std::abs(std::abs(glm::dot(r0, r)) - std::cos(angle * weight)) < 2e-5f);
            CHECK(std::abs(std::abs(glm::dot(r1, r)) - std::cos(angle * (1.0f - weight))) < 2e-5f);
            if (progress == 0.5f && (angle > 0.01f || glm::length(t1 - t0) > 0.01f)) ++movingBones;
        }
    }
    CHECK(movingBones > 10);
}

TEST_CASE("M2: local TRS blending preserves pivots, global tracks, missing tracks and billboards", "[m2][blend]") {
    wowcore::Model m;
    m.sequences.resize(2);
    m.sequences[0].blendTime = 80;
    m.sequences[1].blendTime = 240;
    CHECK(wowcore::blendDurationMs(m, 0, 1) == 240);
    CHECK(wowcore::blendDurationMs(m, 1, 0) == 80);
    m.sequences[1].blendTime = 0;
    CHECK(wowcore::blendDurationMs(m, 0, 1) == 0);
    m.globalSequenceDurations = { 1000 };
    m.bones.resize(3);
    auto& root = m.bones[0];
    root.parentBone = -1;
    root.pivot = glm::vec3(1, 2, 3);
    root.translation.sequences.resize(2);
    root.translation.sequences[0] = { {0}, {glm::vec3(0)}, {}, {} };
    root.translation.sequences[1] = { {0}, {glm::vec3(4, 2, 0)}, {}, {} };
    root.scale.sequences.resize(2);
    root.scale.sequences[0] = { {0}, {glm::vec3(1)}, {}, {} };
    root.scale.sequences[1] = { {0}, {glm::vec3(3)}, {}, {} };
    root.rotation.sequences.resize(2);
    const auto rotation = glm::angleAxis(glm::radians(120.0f), glm::vec3(0, 0, 1));
    root.rotation.sequences[0] = { {0}, {}, {glm::quat(1, 0, 0, 0)}, {} };
    root.rotation.sequences[1] = { {0}, {}, {-rotation}, {} }; // equivalent sign, shortest arc
    auto& child = m.bones[1];
    child.parentBone = 0;
    child.translation.globalSequence = 0;
    child.translation.interpolationType = 1;
    child.translation.sequences = { { {0, 1000}, {glm::vec3(0), glm::vec3(0, 0, 4)}, {}, {} } };
    m.bones[2].parentBone = 1;
    m.bones[2].flags = 0x8;
    m.bones[2].pivot = glm::vec3(0, 0, 1);

    std::vector<glm::mat4> bones, expected;
    wowcore::poseBonesBlended(m, 0, 10, 1, 900, 0.5f, 250, bones);
    const auto halfRotation = glm::angleAxis(glm::radians(60.0f), glm::vec3(0, 0, 1));
    const glm::mat4 rootExpected = glm::translate(glm::mat4(1), root.pivot + glm::vec3(2, 1, 0)) *
        glm::mat4_cast(halfRotation) * glm::scale(glm::mat4(1), glm::vec3(2)) * glm::translate(glm::mat4(1), -root.pivot);
    checkMatrix(bones[0], rootExpected);
    checkMatrix(bones[1], rootExpected * glm::translate(glm::mat4(1), glm::vec3(0, 0, 1)));
    checkMatrix(bones[2], bones[1]); // empty tracks -> identity local

    const glm::vec3 eye(10, 8, 6), up(0, 0, 1);
    for (float progress : {0.0f, 1.0f}) {
        wowcore::poseBones(m, progress == 0 ? 0 : 1, progress == 0 ? 10 : 900, 250, expected, &eye, &up);
        wowcore::poseBonesBlended(m, 0, 10, 1, 900, progress, 250, bones, &eye, &up);
        for (size_t i = 0; i < bones.size(); ++i) checkMatrix(bones[i], expected[i], 0.0f);
    }
    wowcore::poseBonesBlended(m, 0, 10, 1, 900, 0.5f, 250, bones, &eye, &up);
    const auto at = glm::vec3(bones[2] * glm::vec4(m.bones[2].pivot, 1));
    CHECK(glm::dot(glm::normalize(glm::vec3(bones[2][0])), glm::normalize(eye - at)) > 0.99999f);
    checkMatrix(bones[1], rootExpected * glm::translate(glm::mat4(1), glm::vec3(0, 0, 1)));
}

TEST_CASE("Text: widths as the 1.12 client measures them (evidence/fontref)", "[client][text]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "fonts.MPQ") && !std::filesystem::exists(data / "base.MPQ")) SKIP("no client data");
    const wowcore::Mpq mpq(data);
    static const auto ttf = mpq.read("Fonts\\FRIZQT__.TTF");
    REQUIRE(ttf);
    // Measuring never touches the ImFont itself once its face's metrics are registered: any distinct key will do.
    static int key = 0;
    auto* font = reinterpret_cast<ImFont*>(&key);
    wowee::ui::registerInterfaceFaceMetrics(font, ttf->data(), ttf->size());
    // GameFontNormalSmall at 1280x720: 9 px lines, 10 px glyphs. The client's GetStringWidth * 0.9375:
    CHECK(wowee::ui::interfaceTextPx(font, 10.0f, "[PH[ Combat Tester") == 97.0f);   // 103.47: cut to fit 100
    CHECK(wowee::ui::interfaceTextPx(font, 10.0f, "PH[ Combat Tester") == 93.0f);    // 99.20: fits
    CHECK(wowee::ui::interfaceTextPx(font, 10.0f, "Elunara") == 36.0f);              // 38.40
    CHECK(wowee::ui::interfacePenPx(font, 10.0f, " ") == 3.0f);                      // a space: 3.20
    CHECK(wowee::ui::interfacePenPx(font, 12.0f, "a") == 7.0f);                      // GameFontNormal: 7.47
    // Exhaust the recorded ASCII samples instead of checking only five labels.
    // JSON values are UI units; the capture's physical scale was 720/768.
    for (const auto& filename : {"client_advances.json", "client_single.json"}) {
        std::ifstream input(std::filesystem::path(WOWBRIDGE_ROOT) / "evidence/fontref" / filename);
        REQUIRE(input.good());
        const auto samples = nlohmann::json::parse(input);
        for (const auto& family : {std::pair{"small", 10.0f}, std::pair{"normal", 12.0f}, std::pair{"large", 16.0f}}) {
            for (const auto& [code, measured] : samples.at(family.first).items()) {
                const char text[]{static_cast<char>(std::stoi(code)), '\0'};
                INFO(filename << " " << family.first << " ASCII " << code);
                const float expected = std::round(measured.get<float>() * 0.9375f);
                const float actual = std::string_view(filename) == "client_advances.json" ?
                    wowee::ui::interfacePenPx(font, family.second, text) :
                    wowee::ui::interfaceTextPx(font, family.second, text);
                CHECK(actual == expected);
            }
        }
        if (samples.contains("ssmall")) {
            for (const auto& family : {std::pair{"ssmall", 10.0f}, std::pair{"snormal", 12.0f}}) {
                for (const auto& [text, measured] : samples.at(family.first).items()) {
                    INFO(family.first << " " << text);
                    CHECK(wowee::ui::interfaceTextPx(font, family.second, text.c_str()) ==
                          std::round(measured.get<float>() * 0.9375f));
                }
            }
        }
    }
}

TEST_CASE("ClientDb: the character level-up celebration resolves its archive model and sound", "[client][dbc][level-up]") {
    const auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    const wowcore::ClientDb db(*mpq);
    const auto effect = db.levelUpEffect();
    REQUIRE(effect);
    CHECK(effect->effectId == 21);
    CHECK(effect->model == "Spells\\LevelUp\\LevelUp.mdl");
    CHECK(effect->soundId == 124);
    const auto* sound = db.sound(effect->soundId);
    REQUIRE(sound);
    CHECK(sound->files == std::vector<std::string>{"Sound\\Spells\\LevelUp.wav"});
    CHECK(mpq->exists(sound->files[0]));
    const auto model = wowcore::loadModel(*mpq, effect->model);
    REQUIRE(model);
    CHECK_FALSE(model->vertices.empty());
    CHECK_FALSE(model->particleEmitters.empty());
}

TEST_CASE("Melee: the stance and swing for what is in the main hand", "[character]") {
    const auto staff = wowcore::meleeAnims(2, 10, 17);   // the Staff of Jordan: held loose, as the client holds it
    CHECK((staff.ready == 28 && staff.attack == 19 && staff.swingType == 2));
    CHECK(wowcore::meleeAnims(2, 6, 17).ready == 28);    // a polearm
    CHECK(wowcore::meleeAnims(2, 8, 17).ready == 27);    // a two-handed sword
    CHECK(wowcore::meleeAnims(2, 7, 21).attack == 17);   // a main-hand sword
    const auto bare = wowcore::meleeAnims(0, 0, 0);       // nothing in the hand
    CHECK((bare.ready == 25 && bare.attack == 16));
}

TEST_CASE("ClientDb: melee audio follows the attacking form, bare hands or weapon subclass", "[client][dbc][melee-sounds]") {
    const auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    const wowcore::ClientDb db(*mpq);
    using Source = wowcore::MeleeSoundSource;
    const auto bare = db.meleeSounds(Source::Unarmed);
    REQUIRE(bare);
    CHECK(bare->swing == 233);
    CHECK(bare->impact == 1014);
    CHECK(bare->exertion == 0); // race/gender vocals belong to the player model, not the held item
    CHECK(db.meleeSounds(Source::Unarmed, 0, 0, 5)->impact == 1019);
    CHECK(db.meleeSounds(Source::Unarmed, 0, 0, 6)->impact == 1020);

    struct Weapon { uint32_t subclass, parry, swing, hit, critical; };
    for (const auto& weapon : {Weapon{10, 0, 237, 941, 942}, Weapon{7, 1, 235, 143, 144},
                               Weapon{8, 1, 237, 129, 130}, Weapon{0, 1, 235, 171, 172},
                               Weapon{15, 1, 233, 1151, 1152}, Weapon{4, 1, 235, 969, 970}}) {
        INFO("subclass " << weapon.subclass);
        const auto normal = db.meleeSounds(Source::Weapon, weapon.subclass, weapon.parry);
        const auto crit = db.meleeSounds(Source::Weapon, weapon.subclass, weapon.parry, 0, true);
        REQUIRE(normal);
        REQUIRE(crit);
        CHECK(normal->swing == weapon.swing);
        CHECK(crit->swing == weapon.swing + 1);
        CHECK(normal->impact == weapon.hit);
        CHECK(crit->impact == weapon.critical);
        CHECK(normal->exertion == 0);
    }
    // The old staff answers are preserved for every receiving material, including parries and shields.
    for (uint32_t index = 0; index < 10; ++index) for (bool crit : {false, true}) {
        const auto staff = db.meleeSounds(Source::Weapon, 10, 0, index, crit);
        REQUIRE(staff);
        CHECK(staff->swing == db.weaponSwingSound(2, crit));
        CHECK(staff->impact == db.weaponImpactSound(10, 0, index, crit));
    }
    struct Creature { uint32_t display, normal, critical; const char* model; uint32_t swingAt, impactAt; };
    for (const auto& creature : {Creature{892, 643, 0, "DruidCat", 300, 434}, Creature{2281, 473, 474, "DruidBear", 534, 634},
                                 Creature{632, 643, 0, "Tiger", 300, 434}, Creature{2428, 3403, 0, "SeaLion", 300, 466}}) {
        INFO("display " << creature.display);
        for (bool crit : {false, true}) {
            const auto sounds = db.meleeSounds(Source::Creature, creature.display, 0, 0, crit);
            REQUIRE(sounds);
            CHECK(sounds->swing == (crit ? 234 : 233));
            CHECK(sounds->impact == 1014); // cat and bear both have ordinary flesh, unarmed blows
            CHECK(sounds->exertion == (crit ? creature.critical : creature.normal));
            CHECK(sounds->creatureImpactIndex == 0);
            for (auto sound : {sounds->swing, sounds->impact, sounds->exertion}) {
                if (!sound) continue;
                const auto* entry = db.sound(sound);
                REQUIRE(entry);
                REQUIRE_FALSE(entry->files.empty());
                for (const auto& file : entry->files) CHECK(mpq->exists(file));
            }
            for (auto custom : sounds->customAttack) CHECK(custom == 0);
        }
        const auto model = wowcore::loadModel(*mpq, std::string("Creature\\") + creature.model + "\\" + creature.model + ".m2");
        REQUIRE(model);
        const auto attack = wowcore::findSequence(*model, 16);
        REQUIRE(attack >= 0);
        CHECK(wowcore::eventTime(*model, attack, "$CSS") == creature.swingAt);
        CHECK(wowcore::eventTime(*model, attack, "$CAH") == creature.impactAt);
    }
    // A display's sound override wins over the model: mini infernal (1126), versus normal infernal (169).
    const auto override = db.meleeSounds(Source::Creature, 1126, 0, 0, true);
    const auto stone = db.meleeSounds(Source::Creature, 169);
    REQUIRE(override);
    REQUIRE(stone);
    CHECK(override->exertion == 463);
    CHECK(override->creatureImpactIndex == 0);
    CHECK(stone->creatureImpactIndex == 8);
    CHECK_FALSE(db.meleeSounds(Source::Creature, 0));
    CHECK_FALSE(db.meleeSounds(Source::Creature, 0xffffffff));
    CHECK_FALSE(db.meleeSounds(Source::Weapon, 999));
    CHECK_FALSE(db.meleeSounds(Source::Weapon, 10, 2));
    CHECK_FALSE(db.meleeSounds(Source::Unarmed, 0, 0, 10));
}

TEST_CASE("M2: form impacts keep their authored emission windows and particle lifespans", "[client][m2][effects][morph]") {
    const auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    const wowcore::ClientDb db(*mpq);
    const auto spells = wowcore::Dbc::load(*mpq, "DBFilesClient\\Spell.dbc");
    const auto visuals = wowcore::Dbc::load(*mpq, "DBFilesClient\\SpellVisual.dbc");
    REQUIRE(spells);
    REQUIRE(visuals);
    for (uint32_t spell : {5487, 9634, 768, 783, 1066}) {
        INFO(spell);
        const auto row = spells->rowOf(spell);
        REQUIRE(row);
        const uint32_t visual = spells->u32(*row, 115);
        CHECK(visual == (spell == 1066 ? 775 : spell == 5487 ? 653 : spell == 9634 ? 4227 : 4228));
        const auto vr = visuals->rowOf(visual);
        REQUIRE(vr);
        const auto impact = db.spellVisualKits(spell).impact;
        REQUIRE(impact.size() == 1);
        CHECK(impact[0].slot == 2);
        CHECK(impact[0].scale == 1.0f);
        CHECK(impact[0].model == (spell == 1066 ? "Spells\\DruidMorph_Aqua_Impact_Base.mdx" : "Spells\\DruidMorph_Impact_Base.mdx"));
        std::printf("morph spell %u visual %u impact kit %u model %s\n", spell, visual, visuals->u32(*vr, 3), impact[0].model.c_str());
    }
    for (bool aqua : {false, true}) {
        const char* path = aqua ? "Spells\\DruidMorph_Aqua_Impact_Base.mdx" : "Spells\\DruidMorph_Impact_Base.mdx";
        INFO(path);
        const auto m = wowcore::loadModel(*mpq, path);
        REQUIRE(m);
        CHECK(m->version == 256);
        REQUIRE(m->sequences.size() == 1);
        CHECK(wowcore::findSequence(*m, 0) == 0);
        CHECK(m->sequences[0].duration == 2800);
        CHECK(m->sequences[0].nextAnimation == -1);
        REQUIRE(m->particleEmitters.size() == (aqua ? 4 : 5));
        CHECK(m->ribbonEmitters.size() == (aqua ? 3 : 0));
        const float lifespans[] = {1.32f, 1.0f, 1.5f, aqua ? 0.855f : 0.8f, 1.0f};
        const float rates[] = {5, 37, 37, aqua ? 37.0f : 20.0f, 20};
        for (size_t i = 0; i < m->particleEmitters.size(); ++i) {
            INFO(i);
            const auto& e = m->particleEmitters[i];
            const uint32_t off = i == 0 || (!aqua && i >= 3) ? 1400 : 1700;
            REQUIRE(e.enabledTrack.sequences.size() == (aqua ? 11 : 2));
            CHECK(e.enabledTrack.interpolationType == 0);
            CHECK(e.enabledTrack.globalSequence == -1);
            CHECK(e.enabledTrack.sequences[0].timestamps == std::vector<uint32_t>{0, off});
            CHECK(e.enabledTrack.sequences[0].floatValues == std::vector<float>{1, 0});
            // The additional ranges have minimum == maximum: their one key must also survive the loader.
            for (size_t s = 1; s < e.enabledTrack.sequences.size(); ++s) {
                CHECK(e.enabledTrack.sequences[s].timestamps == std::vector<uint32_t>{0});
                CHECK(e.enabledTrack.sequences[s].floatValues == std::vector<float>{1});
            }
            REQUIRE(e.lifespan.sequences.size() == 1);
            CHECK(e.lifespan.globalSequence == -1);
            REQUIRE(e.lifespan.sequences[0].floatValues.size() == 1);
            CHECK(std::abs(e.lifespan.sequences[0].floatValues[0] - lifespans[i]) < 1e-6f);
            REQUIRE(e.emissionRate.sequences.size() == 1);
            CHECK(e.emissionRate.globalSequence == -1);
            CHECK(e.emissionRate.sequences[0].floatValues == std::vector<float>{rates[i]});
            std::printf("  %s emitter %zu: %.3f s life, %.0f/s, off at %u ms\n", aqua ? "aqua" : "base", i, lifespans[i], rates[i], off);

            // A live emitter must turn off from its own track, without the host stopping it. This failed when the
            // last (off) key was dropped, and would also catch interpolating an on/off track as a gradual fade.
            wowcore::Model single{};
            single.particleEmitters.push_back(e);
            wowcore::Emitters enabled(single);
            enabled.update(0.25f, {}, glm::mat4(1), 0, off - 1.0f, off - 1.0f);
            REQUIRE_FALSE(enabled.idle());
            enabled.update(lifespans[i] + 0.01f, {}, glm::mat4(1), 0, off, off);
            CHECK(enabled.idle());

            // Stopping emission leaves existing particles alive for their authored lifespan (not the model length).
            wowcore::Emitters stopped(single);
            stopped.update(0.25f, {}, glm::mat4(1), 0, 0, 0);
            REQUIRE_FALSE(stopped.idle());
            stopped.update(lifespans[i] - 0.01f, {}, glm::mat4(1), 0, 0, 0, false);
            CHECK_FALSE(stopped.idle());
            stopped.update(0.02f, {}, glm::mat4(1), 0, 0, 0, false);
            CHECK(stopped.idle());
        }

        // Even with the correct windows, particles survive the one-shot model's deadline. The host must remove
        // the instance at 2.8 s, rather than wait for Emitters::idle (roughly 3.2 s after fixing the enabled keys).
        for (float dt : {1.0f / 60, 1.0f / 120, 0.001f}) {
            INFO(dt);
            wowcore::Emitters emitters(*m);
            std::vector<glm::mat4> bones;
            float age = 0, last = 0;
            bool liveAtEnd = false;
            while (age < 4.5f) {
                const float previous = age;
                age += dt;
                const float time = std::min(age, 2.8f) * 1000;
                wowcore::poseBones(*m, 0, time, age * 1000, bones);
                emitters.update(dt, bones, glm::mat4(1), 0, time, age * 1000, age < 2.8f);
                if (previous < 2.8f && age >= 2.8f) liveAtEnd = !emitters.idle();
                if (!emitters.idle()) last = age;
            }
            CHECK(liveAtEnd);
            CHECK(last > 3.15f);
            CHECK(last < 3.25f);
            std::printf("  %s dt %.6f: particle drain %.6f s; model deadline 2.800 s\n", aqua ? "aqua" : "base", dt, last);
        }
    }
}

TEST_CASE("M2: spell effect models (geometry, particle emitters, ribbons)", "[client][m2][.effects]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "model.MPQ")) SKIP("WoW 1.12 client data not present");
    const wowcore::Mpq mpq(data);
    for (const char* path : { "spells\\wrath_precast_hand.m2", "spells\\wrath_missile.m2", "spells\\wrath_impact_chest.m2",
                              "Spells\\Nature_PreCast_Low_Hand.m2", "Spells\\Nature_Cast_Hand.m2", "Spells\\Rejuvenation_Impact_Base.m2", "spells\\moonfire_impact_base.m2",
                              "Spells\\EntanglingRoots_State.m2", "Spells\\CallLightning_Impact.m2", "Spells\\LightningStorm_Cloud_State.m2" }) {
        const auto m = wowcore::loadModel(mpq, path);
        if (!m) { std::printf("%s: missing\n", path); continue; }
        std::printf("%s: verts %zu batches %zu bones %zu seqs %zu particles %zu ribbons %zu textures:", path, m->vertices.size(), m->batches.size(),
                    m->bones.size(), m->sequences.size(), m->particleEmitters.size(), m->ribbonEmitters.size());
        for (const auto& t : m->textures) std::printf(" '%s'", t.filename.c_str());
        std::printf("\n");
        glm::vec3 lo(1e9f), hi(-1e9f);
        for (const auto& v : m->vertices) lo = glm::min(lo, v.position), hi = glm::max(hi, v.position);
        std::printf("    vertex bounds (%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
        for (const auto& b : m->batches) {
            const auto& mat = m->materials[b.materialIndex];
            std::printf("    batch submesh %u tex %u blend %u flags 0x%x color %d transparency %u\n", b.skinSectionIndex, b.textureIndex, mat.blendMode, mat.flags,
                        static_cast<int>(b.colorIndex), b.transparencyIndex);
        }
        for (const auto& e : m->particleEmitters)
            std::printf("    emitter bone %u tex %u blend %u type %u particle %u tiles %ux%u cells %u-%u/%u-%u flags 0x%x spin %.2f twinkle %.2f\n", e.bone, e.texture,
                        e.blendingType, e.emitterType, e.particleType, e.textureRows, e.textureCols, e.headCells[0], e.headCells[1], e.decayCells[0], e.decayCells[1], e.flags, e.spin, e.twinklePercent);
        for (const auto& e : m->particleEmitters) {
            auto first = [](const wowee::pipeline::M2AnimationTrack& t) {
                for (const auto& sq : t.sequences) if (!sq.floatValues.empty()) return sq.floatValues[0];
                return -999.0f;
            };
            std::printf("      speed %.3f var %.3f vrange %.3f hrange %.3f gravity %.3f life %.3f rate %.2f areaL %.3f areaW %.3f decel %.3f colors (%.2f %.2f %.2f %.2f) (%.2f %.2f %.2f %.2f) (%.2f %.2f %.2f %.2f) scales %.3f %.3f %.3f tail %.2f\n",
                        first(e.emissionSpeed), first(e.speedVariation), first(e.verticalRange), first(e.horizontalRange), first(e.gravity), first(e.lifespan),
                        first(e.emissionRate), first(e.emissionAreaLength), first(e.emissionAreaWidth), first(e.deceleration),
                        e.particleColor.vec3Values[0].r, e.particleColor.vec3Values[0].g, e.particleColor.vec3Values[0].b, e.particleAlpha.floatValues[0],
                        e.particleColor.vec3Values[1].r, e.particleColor.vec3Values[1].g, e.particleColor.vec3Values[1].b, e.particleAlpha.floatValues[1],
                        e.particleColor.vec3Values[2].r, e.particleColor.vec3Values[2].g, e.particleColor.vec3Values[2].b, e.particleAlpha.floatValues[2],
                        e.particleScale.floatValues[0], e.particleScale.floatValues[1], e.particleScale.floatValues[2], e.tailLength);
        }
        for (const auto& sq : m->sequences) std::printf("    sequence %u duration %u\n", sq.id, sq.duration);
        for (const auto& e : m->particleEmitters) {
            std::printf("    rate keys:");
            for (const auto& sq : e.emissionRate.sequences)
                for (size_t k = 0; k < sq.timestamps.size(); ++k) std::printf(" %u=%.1f", sq.timestamps[k], sq.floatValues[k]);
            std::printf("  colour times:");
            for (float t : e.particleColor.timestamps) std::printf(" %.2f", t);
            std::printf("  alpha times:");
            for (float t : e.particleAlpha.timestamps) std::printf(" %.2f", t);
            std::printf("  scale times:");
            for (float t : e.particleScale.timestamps) std::printf(" %.2f", t);
            std::printf("  enabled keys:");
            for (const auto& sq : e.enabledTrack.sequences)
                for (size_t k = 0; k < sq.timestamps.size(); ++k) std::printf(" %u=%.1f", sq.timestamps[k], sq.floatValues.size() > k ? sq.floatValues[k] : -1.0f);
            std::printf("\n");
        }
        for (const auto& r : m->ribbonEmitters) {
            std::printf("    ribbon bone %u tex %u material %u edges/s %.1f life %.2f\n", r.bone, r.textureIndex, r.materialIndex, r.edgesPerSecond, r.edgeLifetime);
            const std::pair<const char*, const wowee::pipeline::M2AnimationTrack*> tracks[] = { { "alpha", &r.alphaTrack },
                { "above", &r.heightAboveTrack }, { "below", &r.heightBelowTrack }, { "visible", &r.visibilityTrack } };
            for (const auto& [name, t] : tracks) {
                std::printf("      %s:", name);
                for (const auto& sq : t->sequences)
                    for (size_t k = 0; k < sq.timestamps.size() && k < 12; ++k)
                        std::printf(" %u=%.2f", sq.timestamps[k], k < sq.floatValues.size() ? sq.floatValues[k] : -1.0f);
                std::printf("\n");
            }
            std::printf("      colour:");
            for (const auto& sq : r.colorTrack.sequences)
                for (size_t k = 0; k < sq.timestamps.size() && k < 6; ++k)
                    std::printf(" %u=(%.2f %.2f %.2f)", sq.timestamps[k], sq.vec3Values[k].x, sq.vec3Values[k].y, sq.vec3Values[k].z);
            std::printf("\n");
        }
        std::printf("    colour tracks %zu\n", m->colorRGBTracks.size());
    }
}

TEST_CASE("combat rings survive zero controllers and corpse bounds", "[forms][combat]") {
    CHECK(wowcore::selectionCircleRadius(0, 12) == 16);
    CHECK(wowcore::selectionCircleRadius(0, 42) == 42);
    CHECK(wowcore::selectionCircleRadius(50, 200) == 50);
    CHECK(wowcore::selectionCircleRadius(-1, 0) == 16);
    CHECK(wowcore::selectionCircleRadius(std::numeric_limits<float>::quiet_NaN(), 24) == 24);
    CHECK(wowcore::selectionCircleRadius(0, std::numeric_limits<float>::infinity()) == 16);
    for (const auto* command : {"MOVEFORWARD", "MOVEBACKWARD", "TURNLEFT", "TURNRIGHT", "STRAFELEFT", "STRAFERIGHT"})
        CHECK(wowcore::isMovementCommand(command));
    CHECK_FALSE(wowcore::isMovementCommand("TOGGLESOUND"));
    CHECK_FALSE(wowcore::isMovementCommand("ACTIONBUTTON1"));
}

TEST_CASE("melee torso layers retain locomotion and carry weapon attachments", "[client][m2][forms]") {
    const auto mpq = client();
    if (!mpq) SKIP("WoW 1.12 client data not present");
    for (const auto* path : {"Character\\NightElf\\Female\\NightElfFemale.m2", "Character\\NightElf\\Male\\NightElfMale.m2"}) {
        const auto model = wowcore::loadModel(*mpq, path);
        REQUIRE(model);
        const auto& m = *model;
        const auto run = wowcore::findSequence(m, 5), swing = wowcore::findSequence(m, 19);
        REQUIRE(run >= 0); REQUIRE(swing >= 0);
        std::vector<glm::mat4> base, layered, action;
        wowcore::poseBones(m, run, 220, 500, base);
        layered = base;
        REQUIRE(wowcore::layerUpperBody(m, swing, 250, 500, layered));
        wowcore::poseBones(m, swing, 250, 500, action);
        std::vector<bool> upper(m.bones.size(), false);
        int lower = 0, changed = 0;
        for (size_t i = 0; i < m.bones.size(); ++i) {
            const auto& b = m.bones[i];
            upper[i] = b.keyBoneId == 4 || (b.parentBone >= 0 && static_cast<size_t>(b.parentBone) < i && upper[b.parentBone]);
            if (!upper[i]) { checkMatrix(layered[i], base[i], 0); ++lower; }
            else {
                const auto parent = b.parentBone >= 0 ? layered[b.parentBone] : glm::mat4(1);
                const auto actionParent = b.parentBone >= 0 ? action[b.parentBone] : glm::mat4(1);
                checkMatrix(glm::inverse(parent) * layered[i], glm::inverse(actionParent) * action[i], 0.0002f);
                if (glm::length(glm::vec3(layered[i][0] - base[i][0])) > .001f) ++changed;
            }
        }
        CHECK(lower > 0); CHECK(changed > 0);
        const auto hand = wowcore::attachmentMatrix(m, layered, 1);
        const auto baseHand = wowcore::attachmentMatrix(m, base, 1);
        REQUIRE(hand); REQUIRE(baseHand);
        CHECK(glm::length(glm::vec3((*hand)[3] - (*baseHand)[3])) > .01f);
        const auto saved = layered;
        CHECK_FALSE(wowcore::layerUpperBody(m, -1, 0, 0, layered));
        for (size_t i = 0; i < saved.size(); ++i) checkMatrix(layered[i], saved[i], 0);
    }
}


TEST_CASE("Text: native outline and shadow padding and absolute money digit widths", "[client][text][font-faces]") {
    const auto mpq=client();
    if (!mpq) SKIP("no client data");
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq,db,textures,1280,720);
    std::string err;
    REQUIRE(ui.start(&err));
    ui.engine().widgets().noteScreenSize(1280,720);
    auto* L=ui.lua();
    const float priorScale=wowee::ui::interfacePixelScale();
    struct Restore { float scale; ~Restore() { wowee::ui::setInterfacePixelScale(scale); } } restore{priorScale};
    wowee::ui::setInterfacePixelScale(720.f/768.f);
    REQUIRE(luaL_dostring(L,R"(
      local frame=CreateFrame('Frame')
      local text=frame:CreateFontString(nil,'OVERLAY')
      function __WBMeasureNative(face,height,flags,value,shadowX)
        text:SetFont('Fonts\\'..face,height,flags)
        text:SetShadowOffset(shadowX,-1)
        text:SetText(value)
        assert(math.abs(text:GetWidth()-text:GetStringWidth())<0.01)
        return text:GetStringWidth()
      end
    )")==0);
    auto measure=[&](const std::string& face,int height,const std::string& flags,const std::string& text,float shadowX=0.f) {
        lua_getglobal(L,"__WBMeasureNative");
        lua_pushlstring(L,face.data(),face.size()); lua_pushnumber(L,height);
        lua_pushlstring(L,flags.data(),flags.size()); lua_pushlstring(L,text.data(),text.size()); lua_pushnumber(L,shadowX);
        const int status=lua_pcall(L,5,1,0);
        REQUIRE(status==0);
        const float pixels=lua_tonumber(L,-1)*0.9375f;
        lua_pop(L,1); return pixels;
    };
    auto load=[](const char* name) {
        std::ifstream input(std::filesystem::path(WOWBRIDGE_ROOT)/"evidence/fontref"/name);
        REQUIRE(input.good()); return nlohmann::json::parse(input);
    };
    auto fields=[](std::string key,int separators) {
        std::vector<std::string> out;
        for (int i=0;i<separators;++i) {
            const auto at=key.find('|');
            if (at==std::string::npos) return std::vector<std::string>{};
            out.push_back(key.substr(0,at)); key.erase(0,at+1);
        }
        out.push_back(key); return out;
    };
    // Restyling an already measured automatic label must be observable before
    // the next frame, as native PanelTemplates reads these widths immediately.
    measure("ARIALN.ttf",14,"","0");
    ui.frame(1.f/60.f);
    measure("ARIALN.ttf",14,"OUTLINE","0");
    measure("ARIALN.ttf",14,"OUTLINE","0",3.f);
    const auto samples=load("client_more_faces.json");
    // Base glyph/kerning discrepancies remain outside this style-padding fix.
    // Every recorded outline delta is tested, plus the absolute Arial numeric
    // strings that triggered the native money-frame displacement.
    for (const auto& [key,expected]:samples.items()) {
        const auto f=fields(key,3); if (f.empty()) continue;
        const int height=std::stoi(f[1]);
        const float actual=measure(f[0],height,f[2],f[3]);
        const float native=std::stof(expected.get<std::string>())*0.9375f;
        INFO(key);
        if (!f[2].empty()) {
            const auto plainKey=f[0]+"|"+f[1]+"||"+f[3];
            const float nativePlain=std::stof(samples.at(plainKey).get<std::string>())*0.9375f;
            CHECK(std::abs(actual-measure(f[0],height,"",f[3])-(native-nativePlain))<0.05f);
        }
        if (f[0]=="ARIALN.ttf" && !f[3].empty() && f[3].find_first_not_of("0123456789")==std::string::npos)
            CHECK(std::abs(actual-native)<0.05f);
    }
    for (const auto& [key,expected]:load("client_style_widths.json").items()) {
        const auto f=fields(key,4); if (f.empty()) continue;
        const int height=std::stoi(f[1]); const float shadowX=std::stof(f[3]);
        const float actual=measure(f[0],height,f[2],f[4],shadowX)-measure(f[0],height,"",f[4]);
        const auto plainKey=f[0]+"|"+f[1]+"||"+f[4];
        const float native=(std::stof(expected.get<std::string>())-std::stof(samples.at(plainKey).get<std::string>()))*0.9375f;
        INFO(key); CHECK(std::abs(actual-native)<0.05f);
    }
}

TEST_CASE("Text: captured GetFont values survive repeated round trips at every scale", "[client][text][font-roundtrip]") {
    const auto mpq = client();
    if (!mpq) SKIP("no client data");
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    auto* L = ui.lua();
    const auto capture = std::filesystem::path(WOWBRIDGE_ROOT) /
        "evidence/clientref-20261010/font_preview_native/state.lua";
    REQUIRE(luaL_dofile(L, capture.string().c_str()) == 0);
    REQUIRE(luaL_dostring(L, R"(
      local frame = CreateFrame('Frame')
      local text = frame:CreateFontString(nil, 'OVERLAY')
      function __WBFontRoundTrip(face, height, flags, value)
        text:SetFont(face, height, flags)
        text:SetText(value)
        local file, size, style = text:GetFont()
        local before = text:GetStringWidth()
        for i=1,3 do text:SetFont(text:GetFont()) end
        return file, size, style, before, text:GetStringWidth()
      end
    )") == 0);
    lua_getglobal(L, "WBOracleAPI");
    lua_getfield(L, -1, "frames_font_preview_native");
    REQUIRE(lua_istable(L, -1));
    std::vector<std::vector<std::string>> samples;
    for (size_t i = 1; i <= lua_objlen(L, -1); ++i) {
        lua_rawgeti(L, -1, static_cast<int>(i));
        const std::string row = lua_tostring(L, -1);
        lua_pop(L, 1);
        if (row.find("R|WBFontPreviewText") != 0) continue;
        std::vector<std::string> fields;
        size_t start = 0;
        for (size_t at = row.find('|'); at != std::string::npos; at = row.find('|', start)) {
            fields.push_back(row.substr(start, at-start)); start = at+1;
        }
        fields.push_back(row.substr(start));
        REQUIRE(fields.size() == 12);
        samples.push_back(std::move(fields));
    }
    lua_pop(L, 2);
    REQUIRE(samples.size() == 32);
    const float priorScale = wowee::ui::interfacePixelScale();
    struct Restore { float scale; ~Restore() { wowee::ui::setInterfacePixelScale(scale); } } restore{priorScale};
    for (const float scale : {0.75f, 0.9375f, 1.0f, 1.875f}) {
        wowee::ui::setInterfacePixelScale(scale);
        for (const auto& fields : samples) {
            INFO(fields[1] << " scale=" << scale);
            lua_getglobal(L, "__WBFontRoundTrip");
            lua_pushstring(L, fields[7].c_str()); lua_pushnumber(L, std::stof(fields[8]));
            lua_pushstring(L, fields[9].c_str()); lua_pushstring(L, fields[11].c_str());
            const int status = lua_pcall(L, 4, 5, 0);
            if (status != 0) INFO(lua_tostring(L, -1));
            REQUIRE(status == 0);
            CHECK(std::string(lua_tostring(L, -5)) == fields[7]);
            CHECK(std::abs(lua_tonumber(L, -4) - std::stof(fields[8])) < 0.001);
            CHECK(std::string(lua_tostring(L, -3)) == fields[9]);
            CHECK(std::abs(lua_tonumber(L, -2) - lua_tonumber(L, -1)) < 0.001);
            lua_pop(L, 5);
        }
    }
}
