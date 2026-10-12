#include "wowcore/character.hpp"
#include "wowcore/item_preview.hpp"

#include "pipeline/blp_loader.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <optional>

namespace wowcore {
namespace {
// Race folder names and the suffix helms carry for each race (1.12: eight races).
struct Race { const char* folder; const char* helm; };
Race raceOf(uint32_t race) {
    static const Race kRaces[] = { {"", ""}, {"Human", "Hu"}, {"Orc", "Or"}, {"Dwarf", "Dw"}, {"NightElf", "Ni"},
                                   {"Scourge", "Sc"}, {"Tauren", "Ta"}, {"Gnome", "Gn"}, {"Troll", "Tr"} };
    return race < std::size(kRaces) ? kRaces[race] : kRaces[0];
}

// The body texture's components on the 256x256 atlas: the eight item regions in ItemDisplayInfo's column order,
// then the two face regions.
enum Region { ArmUpper, ArmLower, Hand, TorsoUpper, TorsoLower, LegUpper, LegLower, Foot, FaceUpper, FaceLower };
struct Rect { int x, y, w, h; };
constexpr Rect kRegions[] = { {0, 0, 128, 64}, {0, 64, 128, 64}, {0, 128, 128, 32}, {128, 0, 128, 64},
                              {128, 64, 128, 32}, {128, 96, 128, 64}, {128, 160, 128, 64}, {128, 224, 128, 32},
                              {0, 160, 128, 32}, {0, 192, 128, 64} };
const char* const kComponentDirs[] = { "ArmUpperTexture", "ArmLowerTexture", "HandTexture", "TorsoUpperTexture",
                                       "TorsoLowerTexture", "LegUpperTexture", "LegLowerTexture", "FootTexture" };

// Lays an image into a region of the atlas, over what is there by its alpha (nearest sampling when its size is not
// the region's at the atlas's scale).
void paint(Image& atlas, const Image& layer, Region region) {
    if (layer.rgba.empty() || atlas.rgba.empty()) return;
    const int scale = atlas.w / 256;
    const Rect r = kRegions[region];
    const int rw = r.w * scale, rh = r.h * scale;
    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            const int sx = x * layer.w / rw, sy = y * layer.h / rh;
            const uint8_t* s = &layer.rgba[(static_cast<size_t>(sy) * layer.w + sx) * 4];
            uint8_t* d = &atlas.rgba[(static_cast<size_t>(r.y * scale + y) * atlas.w + r.x * scale + x) * 4];
            const int a = s[3];
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((s[c] * a + d[c] * (255 - a) + 127) / 255);
            d[3] = std::max<uint8_t>(d[3], s[3]);
        }
    }
}

std::optional<uint32_t> rowWhere(const Dbc& t, std::initializer_list<std::pair<uint32_t, uint32_t>> match) {
    for (uint32_t r = 0; r < t.rows(); ++r) {
        bool ok = true;
        for (const auto& [col, value] : match) ok = ok && t.u32(r, col) == value;
        if (ok) return r;
    }
    return std::nullopt;
}

// CharSections: ID, RaceID, SexID, BaseSection (0 skin, 1 face, 2 facial hair / markings, 3 hair, 4 underwear),
// VariationIndex, ColorIndex, TextureName[3], Flags.
std::array<std::string, 3> section(const Dbc& t, const Appearance& who, uint32_t base, uint32_t variation, uint32_t color) {
    std::array<std::string, 3> out;
    if (const auto r = rowWhere(t, { {1, who.race}, {2, who.sex}, {3, base}, {4, variation}, {5, color} }))
        for (uint32_t i = 0; i < 3; ++i) out[i] = t.str(*r, 6 + i);
    return out;
}

// An item's texture component for the wearer's sex: _F/_M first, then the unisex _U, then the bare name.
std::string componentPath(const Mpq& mpq, int region, const std::string& name, bool female) {
    const std::string base = std::string("Item\\TextureComponents\\") + kComponentDirs[region] + "\\" + name;
    for (const char* suffix : { female ? "_F.blp" : "_M.blp", "_U.blp", ".blp" })
        if (mpq.exists(base + suffix)) return base + suffix;
    return {};
}
}  // namespace

std::optional<glm::mat4> placeAttached(const Model& body, const std::vector<glm::mat4>& bones, const Attached& item,
                                       bool weaponsDrawn) {
    const bool held = item.sheathed && weaponsDrawn;
    const uint32_t point = held ? (item.slot == 16 || item.slot == 17 ? 2u : 1u) : item.point;
    auto at = attachmentMatrix(body, bones, point);
    // On the back a weapon hangs the other way round: the sheath points' frame turned half a turn about its up axis
    // (matched to the 1.12 client: a staff's head over the left shoulder, its foot at the right hip).
    if (at && item.sheathed && !held) *at = *at * glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    return at;
}

MeleeAnims meleeAnims(uint32_t itemClass, uint32_t subclass, uint32_t inventoryType) {
    constexpr uint32_t kWeapon = 2, kPolearm = 6, kStaff = 10, kTwoHand = 17;
    if (itemClass != kWeapon) return {};
    if (inventoryType == kTwoHand) {
        if (subclass == kPolearm || subclass == kStaff) return { 28, 19, 2 };
        return { 27, 18, 2 };
    }
    return { 26, 17, 1 };
}

Image loadImage(const Mpq& mpq, const std::string& path) {
    Image out;
    const auto bytes = path.empty() ? std::nullopt : mpq.read(path);
    if (!bytes) return out;
    auto blp = wowee::pipeline::BLPLoader::load(*bytes);
    if (blp.data.empty()) return out;
    out.w = blp.width, out.h = blp.height, out.rgba = std::move(blp.data);
    return out;
}

const char* itemPreviewStatusName(ItemPreviewStatus status) {
    switch (status) {
    case ItemPreviewStatus::Ready: return "ready";
    case ItemPreviewStatus::MissingItem: return "missing item catalog entry";
    case ItemPreviewStatus::MissingDisplay: return "missing ItemDisplayInfo";
    case ItemPreviewStatus::NoStandaloneModel: return "no standalone model (use body item geometry)";
    case ItemPreviewStatus::UnsupportedInventoryType: return "unsupported inventory type";
    case ItemPreviewStatus::InvalidAppearance: return "invalid character race/sex";
    case ItemPreviewStatus::MissingModel: return "missing or empty model";
    case ItemPreviewStatus::MissingTexture: return "missing or unsupported batch texture";
    }
    return "unknown";
}

ItemPreview loadItemPreview(const Mpq& mpq, uint32_t entry, const ItemDisplayCatalog& catalog, const Appearance& who) {
    ItemPreview out;
    out.entry = entry;
    const auto item = catalog.find(entry);
    if (item == catalog.end()) return out;
    out.displayId = item->second.displayId;
    const auto display = Dbc::load(mpq, "DBFilesClient\\ItemDisplayInfo.dbc");
    const auto row = display ? display->rowOf(out.displayId) : std::nullopt;
    if (!row) {
        out.status = ItemPreviewStatus::MissingDisplay;
        return out;
    }
    const uint32_t type = item->second.inventoryType;
    const char* directory = nullptr;
    switch (type) {
    case 1: directory = "Head"; break;
    case 3: directory = "Shoulder"; break;
    case 14: directory = "Shield"; break;
    case 13: case 15: case 17: case 21: case 22: case 23: case 25: case 26:
        directory = "Weapon"; break;
    case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 16: case 19: case 20:
        out.status = ItemPreviewStatus::NoStandaloneModel;
        return out;
    default:
        out.status = ItemPreviewStatus::UnsupportedInventoryType;
        out.detail = std::to_string(type);
        return out;
    }
    if (type == 1 && (who.race < 1 || who.race > 8 || who.sex > 1)) {
        out.status = ItemPreviewStatus::InvalidAppearance;
        return out;
    }
    const std::string dir = std::string("Item\\ObjectComponents\\") + directory + "\\";
    const auto fail = [&](ItemPreviewStatus status, const std::string& detail) {
        out.status = status;
        out.detail = detail;
        out.parts.clear();
        return std::move(out);
    };
    for (uint32_t side = 0; side < (type == 3 ? 2u : 1u); ++side) {
        std::string name = display->str(*row, 1 + side);
        if (name.empty()) continue;
        if (const auto dot = name.find_last_of('.'); dot != std::string::npos) name.resize(dot);
        if (type == 1) name += std::string("_") + raceOf(who.race).helm + (who.sex ? "F" : "M");
        const std::string path = dir + name + ".m2";
        auto model = loadModel(mpq, path);
        if (!model || model->vertices.empty() || model->indices.empty() || model->batches.empty())
            return fail(ItemPreviewStatus::MissingModel, path);
        ItemPreviewPart part;
        part.model = std::move(*model);
        std::string skin = display->str(*row, 3 + side);
        if (!skin.empty()) skin = dir + skin + ".blp";
        for (const auto& batch : part.model.batches) {
            if (batch.textureIndex >= part.model.textureLookup.size())
                return fail(ItemPreviewStatus::MissingTexture, path + ": texture lookup");
            const auto index = part.model.textureLookup[batch.textureIndex];
            if (index >= part.model.textures.size())
                return fail(ItemPreviewStatus::MissingTexture, path + ": texture index");
            const auto& tex = part.model.textures[index];
            const std::string texture = tex.type == 0 ? tex.filename : tex.type == 2 ? skin : std::string();
            if (texture.empty()) return fail(ItemPreviewStatus::MissingTexture, path + ": texture type " + std::to_string(tex.type));
            if (!part.images.count(texture)) {
                auto image = loadImage(mpq, texture);
                if (image.rgba.empty()) return fail(ItemPreviewStatus::MissingTexture, texture);
                part.images.emplace(texture, std::move(image));
            }
            part.batchTextures.push_back(texture);
        }
        out.parts.push_back(std::move(part));
    }
    out.status = out.parts.empty() ? ItemPreviewStatus::NoStandaloneModel : ItemPreviewStatus::Ready;
    return out;
}

std::optional<uint32_t> characterPreviewSlot(uint32_t inventoryType) {
    switch (inventoryType) {
    case 4: return 3;   // shirt
    case 5: case 20: return 4;   // chest / robe
    case 6: return 5;   // waist
    case 7: return 6;   // legs
    case 8: return 7;   // feet
    case 9: return 8;   // wrists
    case 10: return 9;  // hands
    case 16: return 14; // cape
    case 19: return 18; // tabard
    default: return std::nullopt;
    }
}

std::set<uint16_t> itemBodyGeosets(uint32_t inventoryType, const std::array<uint32_t, 3>& groups) {
    if (!characterPreviewSlot(inventoryType)) return {};
    if (inventoryType == 16) return {static_cast<uint16_t>(1502 + groups[0])};
    if (inventoryType == 19) return {static_cast<uint16_t>(1201 + groups[0])};
    // Bare variants supply painted geometry too: 401 carries the lower forearm for wristbands/tight sleeves.
    // Keep each undressed body variant, then replace only this item's groups. UV coverage removes unrelated parts.
    std::set<uint16_t> geosets{0, 401, 501, 801, 901, 1001, 1301};
    const auto replace = [&](uint16_t base, uint32_t group) {
        geosets.erase(base);
        geosets.insert(static_cast<uint16_t>(base + group));
    };
    switch (inventoryType) {
    case 4: case 5: case 20:
        replace(801, groups[0]); replace(1001, groups[1]); replace(1301, groups[2]); break;
    case 7: replace(901, groups[0]); replace(1301, groups[2]); break;
    case 8: replace(501, groups[0]); break;
    case 9: replace(801, groups[0]); break;
    case 10: replace(401, groups[0]); break;
    // Vanilla belts are painted (sometimes in LegUpper); no dedicated belt geoset is selected by ItemDisplayInfo.
    }
    return geosets;
}

bool isolateItemBody(Model& model, const ItemBodySelection& selection) {
    using Vertex = wowee::pipeline::M2Vertex;
    std::vector<Vertex> posed = model.vertices;
    std::vector<glm::mat4> bones;
    poseBones(model, std::max(findSequence(model, 0), 0), 0, 0, bones);
    for (auto& v : posed) {
        glm::vec4 p(0.0f);
        glm::vec3 n(0.0f);
        float total = 0;
        for (int k = 0; k < 4; ++k) {
            if (!v.boneWeights[k]) continue;
            if (v.boneIndices[k] >= bones.size()) return false;
            const float w = v.boneWeights[k] / 255.0f;
            p += w * bones[v.boneIndices[k]] * glm::vec4(v.position, 1.0f);
            n += w * glm::mat3(bones[v.boneIndices[k]]) * v.normal;
            total += w;
        }
        if (total > 0) { v.position = glm::vec3(p); v.normal = n; }
        std::fill(std::begin(v.boneWeights), std::end(v.boneWeights), 0);
    }
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<wowee::pipeline::M2Batch> batches;
    const auto interpolate = [](const Vertex& a, const Vertex& b, float t) {
        Vertex v{};
        v.position = glm::mix(a.position, b.position, t);
        v.normal = glm::mix(a.normal, b.normal, t);
        for (int k = 0; k < 2; ++k) v.texCoords[k] = glm::mix(a.texCoords[k], b.texCoords[k], t);
        return v;
    };
    for (auto batch : model.batches) {
        if (!selection.geosets.count(batch.submeshId)) continue;
        if (batch.indexStart > model.indices.size() || batch.indexCount > model.indices.size() - batch.indexStart ||
            batch.indexCount % 3) return false;
        const auto end = batch.indexStart + batch.indexCount;
        const auto start = batch.indexStart;
        batch.indexStart = static_cast<uint32_t>(indices.size());
        batch.vertexStart = static_cast<uint32_t>(vertices.size());
        for (uint32_t i = start; i < end; i += 3) {
            for (int k = 0; k < 3; ++k) if (model.indices[i + k] >= posed.size()) return false;
            for (const auto& rect : selection.regions) {
                if (!(rect.z > rect.x && rect.w > rect.y)) return false;
                std::vector<Vertex> polygon{posed[model.indices[i]], posed[model.indices[i + 1]], posed[model.indices[i + 2]]};
                // Clip crossing triangles, rather than retaining a neck/leg sliver by its UV centroid.
                for (int edge = 0; edge < 4 && !polygon.empty(); ++edge) {
                    const int axis = edge % 2;
                    const bool lower = edge < 2;
                    const float bound = rect[edge];
                    std::vector<Vertex> clipped;
                    auto previous = polygon.back();
                    float pd = lower ? previous.texCoords[0][axis] - bound : bound - previous.texCoords[0][axis];
                    for (const auto& current : polygon) {
                        const float cd = lower ? current.texCoords[0][axis] - bound : bound - current.texCoords[0][axis];
                        if ((pd >= 0) != (cd >= 0)) clipped.push_back(interpolate(previous, current, pd / (pd - cd)));
                        if (cd >= 0) clipped.push_back(current);
                        previous = current; pd = cd;
                    }
                    polygon = std::move(clipped);
                }
                for (size_t k = 1; k + 1 < polygon.size(); ++k) {
                    const auto& a = polygon[0]; const auto& b = polygon[k]; const auto& c = polygon[k + 1];
                    const glm::vec2 ab = b.texCoords[0] - a.texCoords[0], ac = c.texCoords[0] - a.texCoords[0];
                    if (std::abs(ab.x * ac.y - ab.y * ac.x) < 1e-10f) continue;
                    if (vertices.size() + 3 > 65536) return false;  // M2 uses 16-bit indices.
                    for (const auto* v : {&a, &b, &c}) {
                        indices.push_back(static_cast<uint16_t>(vertices.size()));
                        vertices.push_back(*v);
                    }
                }
            }
        }
        batch.indexCount = static_cast<uint32_t>(indices.size()) - batch.indexStart;
        batch.vertexCount = static_cast<uint32_t>(vertices.size()) - batch.vertexStart;
        if (batch.indexCount) batches.push_back(batch);
    }
    if (batches.empty()) return false;
    model.vertices = std::move(vertices);
    model.indices = std::move(indices);
    model.batches = std::move(batches);
    return true;
}

ItemPreview loadArmorItemPreview(const Mpq& mpq, uint32_t entry, const ItemDisplayCatalog& catalog,
                                 const Appearance& who) {
    ItemPreview out;
    out.entry = entry;
    const auto item = catalog.find(entry);
    if (item == catalog.end()) return out;
    out.displayId = item->second.displayId;
    const auto fail = [&](ItemPreviewStatus status, const std::string& detail) {
        out.status = status;
        out.detail = detail;
        out.parts.clear();
        return std::move(out);
    };
    if (!characterPreviewSlot(item->second.inventoryType))
        return fail(ItemPreviewStatus::UnsupportedInventoryType, std::to_string(item->second.inventoryType));
    if (who.race < 1 || who.race > 8 || who.sex > 1)
        return fail(ItemPreviewStatus::InvalidAppearance, "race/sex");
    const auto display = Dbc::load(mpq, "DBFilesClient\\ItemDisplayInfo.dbc");
    const auto row = display ? display->rowOf(out.displayId) : std::nullopt;
    if (!row) return fail(ItemPreviewStatus::MissingDisplay, std::to_string(out.displayId));
    const auto race = raceOf(who.race);
    const std::string gender = who.sex ? "Female" : "Male";
    const std::string path = std::string("Character\\") + race.folder + "\\" + gender + "\\" + race.folder + gender + ".m2";
    auto body = loadModel(mpq, path);
    if (!body || body->vertices.empty() || body->indices.empty() || body->batches.empty())
        return fail(ItemPreviewStatus::MissingModel, path);
    if (findSequence(*body, 0) < 0) return fail(ItemPreviewStatus::MissingModel, path + ": no Stand sequence");
    ItemBodySelection selection;
    selection.geosets = itemBodyGeosets(item->second.inventoryType,
        {display->u32(*row, 6), display->u32(*row, 7), display->u32(*row, 8)});
    Image atlas;
    atlas.w = atlas.h = 256;
    atlas.rgba.assign(256 * 256 * 4, 0);
    for (int region = 0; region < 8; ++region) {
        const std::string name = display->str(*row, 14 + region);
        if (name.empty()) continue;
        const auto component = componentPath(mpq, region, name, who.sex == 1);
        const auto image = loadImage(mpq, component);
        if (image.rgba.empty()) return fail(ItemPreviewStatus::MissingTexture, component.empty() ? name : component);
        // Each component is the only layer here; keep straight RGB and its coverage, without skin underneath.
        const Rect r = kRegions[region];
        int loX = r.w, loY = r.h, hiX = -1, hiY = -1;
        for (int y = 0; y < r.h; ++y) for (int x = 0; x < r.w; ++x) {
            const auto src = (static_cast<size_t>(y * image.h / r.h) * image.w + x * image.w / r.w) * 4;
            const auto dst = (static_cast<size_t>(r.y + y) * atlas.w + r.x + x) * 4;
            std::copy_n(image.rgba.data() + src, 4, atlas.rgba.data() + dst);
            if (image.rgba[src + 3] >= 224) {
                loX = std::min(loX, x); loY = std::min(loY, y);
                hiX = std::max(hiX, x); hiY = std::max(hiY, y);
            }
        }
        if (hiX >= loX && hiY >= loY)
            selection.regions.emplace_back((r.x + loX) / 256.0f, (r.y + loY) / 256.0f,
                                            (r.x + hiX + 1) / 256.0f, (r.y + hiY + 1) / 256.0f);
    }
    const bool cape = item->second.inventoryType == 16;
    std::string capePath;
    if (cape) {
        const std::string name = display->str(*row, 3);
        for (const char* dir : {"Item\\ObjectComponents\\Cape\\", "Item\\TextureComponents\\Cape\\"})
            if (!name.empty() && capePath.empty() && mpq.exists(dir + name + ".blp")) capePath = dir + name + ".blp";
        selection.regions = {{0, 0, 1, 1}};  // Cape UVs use their own texture, not the body atlas.
    }
    // Some base-geoset batches use hair/eye textures with unrelated UVs. Only the selected texture domain applies.
    for (const auto& batch : body->batches) {
        if (batch.textureIndex >= body->textureLookup.size() || body->textureLookup[batch.textureIndex] >= body->textures.size())
            return fail(ItemPreviewStatus::MissingTexture, path + ": texture lookup");
    }
    std::erase_if(body->batches, [&](const auto& batch) {
        return body->textures[body->textureLookup[batch.textureIndex]].type != (cape ? 2u : 1u);
    });
    if (!isolateItemBody(*body, selection)) return fail(ItemPreviewStatus::MissingModel, path + ": no item geometry");
    ItemPreviewPart part;
    part.model = std::move(*body);
    if (!cape) part.images.emplace("body:item", std::move(atlas));
    // A transparent atlas has real coverage; opaque body materials must alpha-test its cutouts.
    for (auto& material : part.model.materials) if (!cape && material.blendMode == 0) material.blendMode = 1;
    for (const auto& batch : part.model.batches) {
        if (batch.textureIndex >= part.model.textureLookup.size()) return fail(ItemPreviewStatus::MissingTexture, path + ": texture lookup");
        const auto index = part.model.textureLookup[batch.textureIndex];
        if (index >= part.model.textures.size()) return fail(ItemPreviewStatus::MissingTexture, path + ": texture index");
        const auto& texture = part.model.textures[index];
        const std::string texturePath = texture.type == 1 && !cape ? "body:item" : texture.type == 2 && cape ? capePath : std::string();
        if (texturePath.empty()) return fail(ItemPreviewStatus::MissingTexture, path + ": texture type " + std::to_string(texture.type));
        if (!part.images.count(texturePath)) part.images.emplace(texturePath, loadImage(mpq, texturePath));
        if (part.images.at(texturePath).rgba.empty()) return fail(ItemPreviewStatus::MissingTexture, texturePath);
        part.batchTextures.push_back(texturePath);
    }
    out.parts.push_back(std::move(part));
    out.armorOnly = true;
    out.status = ItemPreviewStatus::Ready;
    return out;
}

std::optional<Look> dressCreature(const Mpq& mpq, uint32_t displayId) {
    const auto displays = Dbc::load(mpq, "DBFilesClient\\CreatureDisplayInfo.dbc");
    const auto models = Dbc::load(mpq, "DBFilesClient\\CreatureModelData.dbc");
    if (!displays || !models) return std::nullopt;
    const auto display = displays->rowOf(displayId);
    if (!display || displays->u32(*display, 3)) return std::nullopt;
    const auto modelRow = models->rowOf(displays->u32(*display, 1));
    if (!modelRow) return std::nullopt;
    Look look;
    look.body = models->str(*modelRow, 2);
    // CreatureModelData's vanilla filenames end in .mdx; the archive contains the corresponding .m2.
    if (const auto dot = look.body.find_last_of('.'); dot != std::string::npos) look.body.replace(dot, std::string::npos, ".m2");
    const auto model = loadModel(mpq, look.body);
    if (!model) return std::nullopt;
    look.scale = displays->f32(*display, 4) * models->f32(*modelRow, 4);
    const auto slash = look.body.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? "" : look.body.substr(0, slash + 1);
    for (uint32_t i = 0; i < 3; ++i) {
        std::string skin = displays->str(*display, 6 + i);
        if (skin.empty()) continue;
        if (skin.find_first_of("\\/") == std::string::npos) skin = dir + skin;
        look.replaceableTextures.emplace(11 + i, skin + ".blp");
    }
    // Vanilla creature displays have no geoset override field: use the creature model's meshes, including eyes.
    for (const auto& batch : model->batches) look.geosets.insert(batch.submeshId);
    return look;
}

Look dressCharacter(const Mpq& mpq, const Appearance& who, const std::vector<Worn>& worn) {
    Look look;
    const Race race = raceOf(who.race);
    const bool female = who.sex == 1;
    const std::string gender = female ? "Female" : "Male";
    look.body = std::string("Character\\") + race.folder + "\\" + gender + "\\" + race.folder + gender + ".m2";

    const auto load = [&](const char* name) { return Dbc::load(mpq, std::string("DBFilesClient\\") + name); };
    const auto sections = load("CharSections.dbc");
    const auto hairGeosets = load("CharHairGeosets.dbc");
    const auto facial = load("CharacterFacialHairStyles.dbc");
    const auto display = load("ItemDisplayInfo.dbc");
    const auto helmVis = load("HelmetGeosetVisData.dbc");
    if (!sections || !display) return look;

    // ItemDisplayInfo: ID, ModelName[2], ModelTexture[2], InventoryIcon, GeosetGroup[3], Flags, SpellVisualID,
    // GroupSoundIndex, HelmetGeosetVisID[2], Texture[8], ItemVisual.
    struct Item { const Worn* worn; uint32_t row; };
    std::vector<Item> items;
    for (const auto& w : worn)
        if (const auto r = display->rowOf(w.displayId)) items.push_back({ &w, *r });
    const auto bySlot = [&](uint32_t slot) -> const Item* {
        for (const auto& i : items) if (i.worn->slot == slot) return &i;
        return nullptr;
    };
    const auto group = [&](uint32_t slot, uint32_t g) -> uint32_t {
        const Item* i = bySlot(slot);
        return i ? display->u32(i->row, 6 + g) : 0;
    };

    // The body texture: skin, face, markings, scalp, underwear, then the items' components in the client's order.
    const auto skin = section(*sections, who, 0, 0, who.skin);
    look.skin = loadImage(mpq, skin[0]);
    if (look.skin.rgba.empty()) {
        look.skin.w = look.skin.h = 256;
        look.skin.rgba.assign(256 * 256 * 4, 255);
    }
    const auto face = section(*sections, who, 1, who.face, who.skin);
    paint(look.skin, loadImage(mpq, face[0]), FaceLower);
    paint(look.skin, loadImage(mpq, face[1]), FaceUpper);
    const auto markings = section(*sections, who, 2, who.facialHair, who.hairColor);
    paint(look.skin, loadImage(mpq, markings[0]), FaceLower);
    paint(look.skin, loadImage(mpq, markings[1]), FaceUpper);
    const auto hair = section(*sections, who, 3, who.hairStyle, who.hairColor);
    look.hair = hair[0];
    paint(look.skin, loadImage(mpq, hair[1]), FaceLower);
    paint(look.skin, loadImage(mpq, hair[2]), FaceUpper);
    const auto underwear = section(*sections, who, 4, 0, who.skin);
    paint(look.skin, loadImage(mpq, underwear[0]), LegUpper);
    paint(look.skin, loadImage(mpq, underwear[1]), TorsoUpper);
    // Shirt, head, neck, shoulders, legs, feet, chest, tabard, waist, wrists, hands, back: what lies over what. A robe or
    // a kilt (geoset 1302 and up) hangs over the boots, so the boots go under the legs and chest instead.
    const bool robe = std::max(group(4, 2), group(6, 2)) > 0;
    const auto order = robe ? std::initializer_list<uint32_t>{ 3u, 0u, 1u, 2u, 7u, 6u, 4u, 18u, 5u, 8u, 9u, 14u }
                            : std::initializer_list<uint32_t>{ 3u, 0u, 1u, 2u, 6u, 7u, 4u, 18u, 5u, 8u, 9u, 14u };
    for (const uint32_t slot : order) {
        const Item* item = bySlot(slot);
        if (!item) continue;
        for (int region = 0; region < 8; ++region) {
            const std::string name = display->str(item->row, 14 + region);
            if (!name.empty()) paint(look.skin, loadImage(mpq, componentPath(mpq, region, name, female)), static_cast<Region>(region));
        }
    }

    // Geosets: the body, the hair style, facial hair (none for most styles), and each group's bare or worn variant.
    look.geosets.insert(0);
    bool showHair = true, showFacial[3] = { true, true, true }, showEars = true;
    if (const Item* helm = bySlot(0); helm && helmVis) {
        // HelmetGeosetVisData: ID, then race masks that hide the hair, facial hair 1xx/2xx/3xx, and the ears.
        if (const auto r = helmVis->rowOf(display->u32(helm->row, 12 + (female ? 1 : 0)))) {
            const uint32_t bit = 1u << (who.race - 1);
            showHair = !(helmVis->u32(*r, 1) & bit);
            for (int f = 0; f < 3; ++f) showFacial[f] = !(helmVis->u32(*r, 2 + f) & bit);
            showEars = !(helmVis->u32(*r, 5) & bit);
        }
    }
    if (hairGeosets && showHair)
        if (const auto r = rowWhere(*hairGeosets, { {1, who.race}, {2, who.sex}, {3, who.hairStyle} }))
            if (const uint32_t g = hairGeosets->u32(*r, 4)) look.geosets.insert(static_cast<uint16_t>(g));
    if (facial)
        // CharacterFacialHairStyles: RaceID, SexID, VariationID, Geoset[6] - the first three are the 100s, 300s and 200s.
        if (const auto r = rowWhere(*facial, { {0, who.race}, {1, who.sex}, {2, who.facialHair} })) {
            const uint32_t groups[3] = { 100, 300, 200 };
            const int show[3] = { 0, 2, 1 };
            for (int i = 0; i < 3; ++i) {
                const uint32_t v = facial->u32(*r, 3 + i);
                if (v > 0 && v < 100 && showFacial[show[i]]) look.geosets.insert(static_cast<uint16_t>(groups[i] + v));
            }
        }
    if (showEars) look.geosets.insert(702);
    look.geosets.insert(static_cast<uint16_t>(401 + group(9, 0)));                   // gloves
    look.geosets.insert(static_cast<uint16_t>(501 + group(7, 0)));                   // boots
    look.geosets.insert(static_cast<uint16_t>(801 + std::max(group(4, 0), group(8, 0))));   // sleeves
    look.geosets.insert(static_cast<uint16_t>(1001 + group(4, 1)));                  // the chest's lower part
    look.geosets.insert(static_cast<uint16_t>(1301 + std::max(group(4, 2), group(6, 2))));  // robe or kilt
    look.geosets.insert(static_cast<uint16_t>(901 + group(6, 0)));                   // kneepads
    if (bySlot(18)) look.geosets.insert(static_cast<uint16_t>(1201 + group(18, 0))); // tabard
    if (const Item* cape = bySlot(14)) {
        look.geosets.insert(static_cast<uint16_t>(1501 + group(14, 0) + 1));
        const std::string name = display->str(cape->row, 3);
        for (const char* dir : { "Item\\ObjectComponents\\Cape\\", "Item\\TextureComponents\\Cape\\" })
            if (look.cape.empty() && mpq.exists(dir + name + ".blp")) look.cape = dir + name + ".blp";
    } else {
        look.geosets.insert(1501);
    }

    // Models: the helm (made for each race and sex), shoulders, weapons (on the back until drawn).
    if (const Item* helm = bySlot(0)) {
        std::string model = display->str(helm->row, 1);
        if (const auto dot = model.rfind('.'); dot != std::string::npos) model.resize(dot);
        if (!model.empty())
            look.attached.push_back({ 11, "Item\\ObjectComponents\\Head\\" + model + "_" + race.helm + (female ? "F" : "M") + ".m2",
                                      "Item\\ObjectComponents\\Head\\" + std::string(display->str(helm->row, 3)) + ".blp", 0, false });
    }
    if (const Item* shoulders = bySlot(2)) {
        for (int side = 0; side < 2; ++side) {   // left (6), right (5)
            const std::string model = display->str(shoulders->row, 1 + side);
            if (!model.empty())
                look.attached.push_back({ side == 0 ? 6u : 5u, "Item\\ObjectComponents\\Shoulder\\" + model,
                                          "Item\\ObjectComponents\\Shoulder\\" + std::string(display->str(shoulders->row, 3 + side)) + ".blp", 2, false });
        }
    }
    for (const uint32_t slot : { 15u, 16u, 17u }) {
        const Item* weapon = bySlot(slot);
        if (!weapon) continue;
        const std::string model = display->str(weapon->row, 1);
        if (model.empty()) continue;
        // Held in the hand; sheathed on the back (two-handers and staves: item_template.sheath 1 and 2).
        const uint32_t sheath = weapon->worn->sheath;
        // Relics also use slot 17, but have no standalone model. A ranged model uses the left hand/back.
        const uint32_t back = sheath == 1 || slot == 17 ? 26u : sheath == 2 ? 27u : sheath == 4 ? 28u : slot == 15 ? 32u : 33u;
        const std::string directory = sheath == 4 ? "Item\\ObjectComponents\\Shield\\" : "Item\\ObjectComponents\\Weapon\\";
        look.attached.push_back({ back, directory + model,
                                  directory + std::string(display->str(weapon->row, 3)) + ".blp", slot, true });
    }
    return look;
}
}  // namespace wowcore
