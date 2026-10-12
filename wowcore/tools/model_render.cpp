// A dressed character, posed and drawn on the CPU to a PNG - to look at the model, its textures, geosets and
// attachments before (and beside) the D3D11 renderer in the game.
//
// usage: model_render OUT.png [--yaw DEG] [--anim ID] [--time MS] [--size N] [--zoom F] [--atlas ATLAS.png]
//                            [--attach 0|1] [--drawn 0|1] [--naked 0|1]
//                            [--display ID] (CreatureDisplayInfo, e.g. Cat 892, Bear 2281)
//                            [--from-anim ID --from-time MS --blend-progress F] [--global-time MS]
//                            [--item ENTRY --item-display ID --item-type InventoryType --item-part N]
//                            [--race ID --sex 0|1] (helm variant; defaults to Elunara)
//                            [--skin N --face N --hair-style N --hair-color N --facial-hair N]
//                            [--worn SLOT:DISPLAY[:SHEATH]] (repeat; replaces reference outfit)
// --anim/--time are the entering pose; --blend-progress is elapsed/duration (0..1), eased by wowcore.
#include "wowcore/character.hpp"
#include "wowcore/effect.hpp"
#include "wowcore/item_preview.hpp"
#include "wowcore/model_layers.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/soft_raster.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
struct Canvas {
    int size;
    std::vector<uint8_t> rgba;
    std::vector<float> depth;
    explicit Canvas(int n) : size(n), rgba(static_cast<size_t>(n) * n * 4), depth(static_cast<size_t>(n) * n, 1e30f) {
        for (int i = 0; i < n * n; ++i) rgba[i * 4 + 0] = 40, rgba[i * 4 + 1] = 44, rgba[i * 4 + 2] = 52, rgba[i * 4 + 3] = 255;
    }
};

glm::vec4 sample(const wowcore::Image& img, glm::vec2 uv) {
    if (img.rgba.empty()) return glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
    float u = uv.x - std::floor(uv.x), v = uv.y - std::floor(uv.y);
    const int x = std::min(img.w - 1, static_cast<int>(u * img.w));
    const int y = std::min(img.h - 1, static_cast<int>(v * img.h));
    const uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.w + x) * 4];
    return glm::vec4(p[0], p[1], p[2], p[3]) / 255.0f;
}

struct Vert { glm::vec3 screen; glm::vec2 uv; float shade; float normalShade; };

// M2 blend modes (wowdev M2 materials): 0 opaque, 1 alpha key, 2 alpha, 3 add, 4 add by alpha, 5 modulate,
// 6 modulate 2x, 7 blend add.
void triangle(Canvas& c, const Vert& a, const Vert& b, const Vert& d, const wowcore::Image& tex, int blend, bool depthWrite,
              const glm::vec4& tint = glm::vec4(1.0f), const glm::mat3& uvm = glm::mat3(1.0f)) {
    const float edge = static_cast<float>(c.size - 1);
    const float minx = std::max(0.0f, std::floor(std::min({ a.screen.x, b.screen.x, d.screen.x })));
    const float maxx = std::min(edge, std::ceil(std::max({ a.screen.x, b.screen.x, d.screen.x })));
    const float miny = std::max(0.0f, std::floor(std::min({ a.screen.y, b.screen.y, d.screen.y })));
    const float maxy = std::min(edge, std::ceil(std::max({ a.screen.y, b.screen.y, d.screen.y })));
    const float area = (b.screen.x - a.screen.x) * (d.screen.y - a.screen.y) - (b.screen.y - a.screen.y) * (d.screen.x - a.screen.x);
    if (std::abs(area) < 1e-6f) return;
    for (int y = static_cast<int>(miny); y <= static_cast<int>(maxy); ++y) {
        for (int x = static_cast<int>(minx); x <= static_cast<int>(maxx); ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float w0 = ((b.screen.x - px) * (d.screen.y - py) - (b.screen.y - py) * (d.screen.x - px)) / area;
            const float w1 = ((d.screen.x - px) * (a.screen.y - py) - (d.screen.y - py) * (a.screen.x - px)) / area;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            const float z = w0 * a.screen.z + w1 * b.screen.z + w2 * d.screen.z;
            float& depth = c.depth[static_cast<size_t>(y) * c.size + x];
            if (z >= depth) continue;
            const glm::vec2 uv = glm::vec2(uvm * glm::vec3(a.uv * w0 + b.uv * w1 + d.uv * w2, 1.0f));
            const glm::vec4 t = sample(tex, uv) * tint;
            if (blend == 1 && t.a < 0.878f) continue;   // alpha key
            const float shade = w0 * a.shade + w1 * b.shade + w2 * d.shade;
            uint8_t* p = &c.rgba[(static_cast<size_t>(y) * c.size + x) * 4];
            const glm::vec3 lit = glm::vec3(t) * shade;
            for (int k = 0; k < 3; ++k) {
                const float dst = p[k] / 255.0f, src = blend >= 5 ? t[k] : lit[k];
                float out = src;
                switch (blend) {
                case 2: out = src * t.a + dst * (1.0f - t.a); break;
                case 3: out = dst + src; break;
                case 4: out = dst + src * t.a; break;
                case 5: out = dst * src; break;
                case 6: out = dst * src * 2.0f; break;
                case 7: out = src + dst * (1.0f - t.a); break;
                default: break;
                }
                p[k] = static_cast<uint8_t>(std::clamp(out, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            if (depthWrite) depth = z;
        }
    }
}

struct Scene {
    const wowcore::Mpq& mpq;
    std::map<std::string, wowcore::Image> images;
    const wowcore::Image& image(const std::string& path) {
        auto it = images.find(path);
        if (it == images.end()) it = images.emplace(path, wowcore::loadImage(mpq, path)).first;
        return it->second;
    }
};

// The light: ambient + diffuse * N.L (--light A,D; the defaults are this tool's own).
float g_ambient = 0.55f, g_diffuse = 0.45f;
glm::vec3 g_light = glm::normalize(glm::vec3(-0.6f, 0.4f, 0.7f));

// Draws a model's batches (those whose geoset is shown) with a world transform, posed.
void draw(Canvas& c, Scene& scene, const wowcore::Model& m, const std::vector<glm::mat4>& bones, const glm::mat4& world,
          const glm::mat4& viewProj, const std::set<uint16_t>* geosets, const std::map<uint32_t, const wowcore::Image*>& byType,
          const float* effectTimeMs = nullptr, const wowcore::ItemPreviewPart* itemPart = nullptr) {
    std::vector<Vert> verts(m.vertices.size());
    const glm::vec3 light = g_light;
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        const auto& v = m.vertices[i];
        glm::vec4 p(0.0f), n(0.0f);
        float total = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const float w = v.boneWeights[k] / 255.0f;
            if (w <= 0.0f || v.boneIndices[k] >= bones.size()) continue;
            p += w * (bones[v.boneIndices[k]] * glm::vec4(v.position, 1.0f));
            n += w * (bones[v.boneIndices[k]] * glm::vec4(v.normal, 0.0f));
            total += w;
        }
        if (total <= 0.0f) p = glm::vec4(v.position, 1.0f), n = glm::vec4(v.normal, 0.0f);
        const glm::vec4 wp = world * glm::vec4(glm::vec3(p), 1.0f);
        const glm::vec3 wn = glm::normalize(glm::vec3(world * glm::vec4(glm::vec3(n), 0.0f)));
        const glm::vec4 clip = viewProj * wp;
        verts[i].screen = glm::vec3((clip.x / clip.w * 0.5f + 0.5f) * c.size, (0.5f - clip.y / clip.w * 0.5f) * c.size, clip.w);
        verts[i].uv = v.texCoords[0];
        verts[i].shade = g_ambient + g_diffuse * std::max(0.0f, glm::dot(wn, light));
        verts[i].normalShade = verts[i].shade;
    }
    // Opaque first, then the blended ones.
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t batchIndex = 0; batchIndex < m.batches.size(); ++batchIndex) {
            const auto& b = m.batches[batchIndex];
            if (geosets && !geosets->count(b.submeshId)) continue;
            const int  blend = b.materialIndex < m.materials.size() ? m.materials[b.materialIndex].blendMode : 0;
            const auto flags = b.materialIndex < m.materials.size() ? m.materials[b.materialIndex].flags : 0;
            const bool unlit = flags & 0x1, depthWrite = blend <= 1 && !(flags & 0x10);
            if ((blend >= 2) != (pass == 1)) continue;
            const wowcore::Image* img = nullptr;
            if (itemPart) img = &itemPart->images.at(itemPart->batchTextures.at(batchIndex));
            else {
                const auto& tex = m.textures[b.textureIndex < m.textureLookup.size() ? m.textureLookup[b.textureIndex] : 0];
                if (auto it = byType.find(tex.type); it != byType.end()) img = it->second;
                else img = &scene.image(tex.filename);
            }
            for (uint32_t i = 0; i + 2 < b.indexCount; i += 3) {
                const auto i0 = m.indices[b.indexStart + i], i1 = m.indices[b.indexStart + i + 1], i2 = m.indices[b.indexStart + i + 2];
                if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) continue;
                Vert v0 = verts[i0], v1 = verts[i1], v2 = verts[i2];
                if (unlit) v0.shade = v1.shade = v2.shade = 1.0f;
                wowcore::BatchLook look;
                if (effectTimeMs) look = wowcore::batchLook(m, b, 0, *effectTimeMs, *effectTimeMs);
                triangle(c, v0, v1, v2, *img, blend, depthWrite, look.color, look.uv);
            }
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: model_render OUT.png [--yaw DEG] [--anim ID] [--time MS] [--size N] [--atlas ATLAS.png]\n"
                             "       [--from-anim ID --from-time MS --blend-progress F] [--global-time MS] [--display ID]\n"
                             "       [--item ENTRY --item-display ID --item-type InventoryType --item-part N] [--race ID --sex 0|1]\n"
                             "       [--skin N --face N --hair-style N --hair-color N --facial-hair N]\n"
                             "       [--worn SLOT:DISPLAY[:SHEATH]] (repeat; replaces reference outfit)\n");
        return 2;
    }
    if ((argc - 2) % 2 != 0) {
        std::fprintf(stderr, "every option requires a value\n");
        return 2;
    }
    float yaw = 180.0f, timeMs = 0.0f;
    uint32_t anim = 0;
    uint32_t displayId = 0, mountDisplay = 0;
    std::optional<uint32_t> itemEntry, itemDisplay, itemType;
    std::vector<wowcore::Worn> selectedWorn;
    uint32_t itemPartIndex = 0;
    wowcore::Appearance who{ 4, 1, 5, 8, 5, 3, 0 };
    uint32_t fromAnim = 0;
    float fromTimeMs = 0.0f, blendProgress = 1.0f, globalMs = 0.0f;
    bool transition = false, explicitGlobalTime = false;
    int upperAnim = -1; float upperTimeMs = 0;
    int size = 512;
    std::string atlasOut;
    bool attachments = true;
    bool naked = false, portrait = false;
    bool drawn = false;   // weapons in hand
    std::string effect;   // an effect model on both spell hands (21, 22)
    float zoom = 1.0f;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--yaw")) yaw = std::stof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--upper-anim")) upperAnim = std::stoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--upper-time")) upperTimeMs = std::stof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--anim")) anim = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--mount")) mountDisplay = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--display")) displayId = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--item")) itemEntry = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--item-display")) itemDisplay = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--item-type")) itemType = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--item-part")) itemPartIndex = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--worn")) {
            wowcore::Worn gear;
            int consumed = 0;
            const char* value = argv[i + 1];
            if (std::sscanf(value, "%u:%u:%u%n", &gear.slot, &gear.displayId, &gear.sheath, &consumed) != 3) {
                gear.sheath = 0;
                consumed = 0;
                std::sscanf(value, "%u:%u%n", &gear.slot, &gear.displayId, &consumed);
            }
            if (!consumed || value[consumed] || gear.slot > 18 || !gear.displayId) {
                std::fprintf(stderr, "--worn requires SLOT:DISPLAY[:SHEATH], slot 0..18 and a nonzero display\n");
                return 2;
            }
            selectedWorn.push_back(gear);
        }
        else if (!std::strcmp(argv[i], "--race")) who.race = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--sex")) who.sex = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--skin")) who.skin = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--face")) who.face = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--hair-style")) who.hairStyle = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--hair-color")) who.hairColor = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--facial-hair")) who.facialHair = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--time")) timeMs = std::stof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--from-anim")) fromAnim = static_cast<uint32_t>(std::stoul(argv[i + 1])), transition = true;
        else if (!std::strcmp(argv[i], "--from-time")) fromTimeMs = std::stof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--blend-progress")) blendProgress = std::stof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--global-time")) globalMs = std::stof(argv[i + 1]), explicitGlobalTime = true;
        else if (!std::strcmp(argv[i], "--size")) size = std::stoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--atlas")) atlasOut = argv[i + 1];
        else if (!std::strcmp(argv[i], "--attach")) attachments = std::stoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--zoom")) zoom = std::stof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--drawn")) drawn = std::stoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--effect")) effect = argv[i + 1];
        else if (!std::strcmp(argv[i], "--naked")) naked = std::stoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--portrait")) portrait = std::stoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--light")) std::sscanf(argv[i + 1], "%f,%f", &g_ambient, &g_diffuse);
    }
    if ((itemEntry || itemDisplay || itemType) && (!itemEntry || !itemDisplay || !itemType || displayId || portrait)) {
        std::fprintf(stderr, "item preview requires --item, --item-display and --item-type together, without --display/--portrait\n");
        return 2;
    }
    if (!selectedWorn.empty() && (itemEntry || displayId || naked)) {
        std::fprintf(stderr, "--worn requires character rendering without --item/--display/--naked\n");
        return 2;
    }
    if (size <= 0 || !std::isfinite(zoom) || zoom <= 0.0f) {
        std::fprintf(stderr, "--size and --zoom must be positive\n");
        return 2;
    }
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const wowcore::Mpq mpq(env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data");
    // Elunara (the reference realm's character): night elf female, skin 5, face 8, hair 5 colour 3, markings 0; the
    // Stormrage set, the Staff of Jordan.
    const std::vector<wowcore::Worn> worn = { {0, 33655}, {2, 30546}, {4, 30536}, {5, 30541}, {6, 30540}, {7, 30542},
                                              {8, 30548}, {9, 34016}, {15, 20298, 2} };
    const auto dressed = itemEntry ? std::optional<wowcore::Look>(wowcore::Look{}) : displayId ? wowcore::dressCreature(mpq, displayId) :
        std::optional<wowcore::Look>(wowcore::dressCharacter(mpq, who, naked ? std::vector<wowcore::Worn>{} :
                                                        selectedWorn.empty() ? worn : selectedWorn));
    if (!dressed) {
        std::fprintf(stderr, "cannot dress creature display %u\n", displayId);
        return 1;
    }
    const auto& look = *dressed;
    if (!atlasOut.empty()) wowcore::writePng(atlasOut, look.skin);
    wowcore::ItemPreview item;
    const wowcore::ItemPreviewPart* itemPart = nullptr;
    std::optional<wowcore::Model> loadedBody;
    const wowcore::Model* body = nullptr;
    if (itemEntry) {
        item = wowcore::loadItemPreview(mpq, *itemEntry, { {*itemEntry, {*itemDisplay, *itemType}} }, who);
        if (item.status != wowcore::ItemPreviewStatus::Ready || itemPartIndex >= item.parts.size()) {
            std::fprintf(stderr, "item %u: %s (%s); part %u of %zu\n", *itemEntry,
                         wowcore::itemPreviewStatusName(item.status), item.detail.c_str(), itemPartIndex, item.parts.size());
            return 1;
        }
        itemPart = &item.parts[itemPartIndex];
        body = &itemPart->model;
        std::printf("item %u display %u part %u: %s, %zu batches, %zu images\n", *itemEntry, item.displayId,
                    itemPartIndex, body->name.c_str(), body->batches.size(), itemPart->images.size());
    } else {
        loadedBody = wowcore::loadModel(mpq, look.body);
        if (loadedBody) body = &*loadedBody;
    }
    if (!body) {
        std::fprintf(stderr, "no model %s\n", look.body.c_str());
        return 1;
    }
    if (!itemEntry) {
        std::printf("body %s, geosets:", look.body.c_str());
        for (auto g : look.geosets) std::printf(" %u", g);
        std::printf("; scale %.3f\n", look.scale);
    }
    for (const auto& [type, path] : look.replaceableTextures) std::printf("texture type %u: %s\n", type, path.c_str());

    int seq = wowcore::findSequence(*body, anim);
    const int fromSeq = transition ? wowcore::findSequence(*body, fromAnim) : -1;
    if (transition && (seq < 0 || fromSeq < 0 || !std::isfinite(blendProgress) || blendProgress < 0.0f || blendProgress > 1.0f)) {
        std::fprintf(stderr, "transition requires existing animation ids and --blend-progress in [0,1]\n");
        return 2;
    }
    if (seq < 0) seq = 0;
    if (!explicitGlobalTime) globalMs = timeMs;
    std::vector<glm::mat4> bones;
    if (transition) {
        std::printf("transition %u -> %u: %u ms, progress %.3f (smoothstep)\n", fromAnim, anim,
                    wowcore::blendDurationMs(*body, fromSeq, seq), blendProgress);
        wowcore::poseBonesBlended(*body, fromSeq, fromTimeMs, seq, timeMs, blendProgress, globalMs, bones);
    } else {
        wowcore::poseBones(*body, seq, timeMs, globalMs, bones);
    }

    if (upperAnim >= 0 && !wowcore::layerUpperBody(*body, wowcore::findSequence(*body, upperAnim), upperTimeMs, globalMs, bones)) {
        std::fprintf(stderr, "upper-body action requires a valid sequence and SpineLow bone\n");
        return 2;
    }

    // A camera 6 yards out at chest height, turned yaw degrees round the character (180: from behind).
    const float r = glm::radians(yaw);
    glm::vec3 target(0.0f, 0.0f, zoom > 1.5f ? 2.0f : 1.2f);
    float distance = 4.5f / zoom;
    if (displayId || itemEntry) {
        // Creatures and standalone items: frame the scaled model instead of using character chest height.
        // The M2 header bounds are collision bounds, so measure the posed vertices (including mane and tail).
        glm::vec3 low(1e30f), high(-1e30f);
        for (const auto& vertex : body->vertices) {
            glm::vec4 p(0.0f);
            float total = 0.0f;
            for (int k = 0; k < 4; ++k) {
                if (!vertex.boneWeights[k] || vertex.boneIndices[k] >= bones.size()) continue;
                const float weight = vertex.boneWeights[k] / 255.0f;
                p += weight * (bones[vertex.boneIndices[k]] * glm::vec4(vertex.position, 1.0f));
                total += weight;
            }
            const glm::vec3 position = (total > 0.0f ? glm::vec3(p) : vertex.position) * look.scale;
            low = glm::min(low, position);
            high = glm::max(high, position);
        }
        target = (low + high) * 0.5f;
        distance = glm::length(high - low) * 0.5f / std::sin(glm::radians(17.5f)) * 1.05f / zoom;
    }
    const glm::vec3 direction(std::cos(r), std::sin(r), 0.15f);
    const glm::vec3 eye = target + (displayId || itemEntry ? glm::normalize(direction) : direction) * distance;
    glm::mat4 viewProj = glm::perspective(glm::radians(35.0f), 1.0f, 0.1f, 100.0f) * glm::lookAt(eye, target, glm::vec3(0, 0, 1));
    // The unit portrait: the model's own camera 0, its diagonal field of view made vertical for a square.
    const auto portraitCam = std::find_if(body->cameras.begin(), body->cameras.end(), [](const auto& c) { return c.type == 0; });
    if (portrait && portraitCam != body->cameras.end()) {
        const float vertical = 2.0f * std::atan(std::tan(portraitCam->fov * 0.5f) / std::sqrt(2.0f));
        g_light = glm::normalize(portraitCam->positionBase - portraitCam->targetBase);
        viewProj = glm::perspective(vertical, 1.0f, portraitCam->nearClip, portraitCam->farClip) *
                   glm::lookAt(portraitCam->positionBase, portraitCam->targetBase, glm::vec3(0, 0, 1));
    }

    const auto mountLook = mountDisplay ? wowcore::dressCreature(mpq, mountDisplay) : std::nullopt;
    const auto mount = mountLook ? wowcore::loadModel(mpq, mountLook->body) : std::nullopt;
    glm::mat4 rider(1.0f);
    std::vector<glm::mat4> mountBones;
    if (mountDisplay) {
        if (!mount) return 1;
        const int mountSeq = wowcore::sequenceWithFallback(*mount, 5);
        wowcore::poseBones(*mount, mountSeq, timeMs, globalMs, mountBones);
        const auto saddle = wowcore::mountedRiderMatrix(*mount, mountBones, mountLook->scale);
        if (!saddle) return 1;
        rider = *saddle;
        target = glm::vec3(0, 0, 1.8f);
        const glm::vec3 mountEye = target + direction * 9.0f / zoom;
        viewProj = glm::perspective(glm::radians(35.0f), 1.0f, 0.1f, 100.0f) * glm::lookAt(mountEye, target, glm::vec3(0, 0, 1));
        std::printf("saddle %.3f %.3f %.3f scale %.3f\n", rider[3].x,rider[3].y,rider[3].z,mountLook->scale);
    }
    Canvas canvas(size);
    Scene scene{ mpq, {} };
    const wowcore::Image hair = wowcore::loadImage(mpq, look.hair);
    const wowcore::Image cape = wowcore::loadImage(mpq, look.cape);
    std::map<uint32_t, const wowcore::Image*> byType{ {1, &look.skin}, {6, &hair}, {2, &cape} };
    for (const auto& [type, path] : look.replaceableTextures) byType[type] = &scene.image(path);
    if (mount) {
        std::map<uint32_t, const wowcore::Image*> skin;
        for (const auto& [type, path] : mountLook->replaceableTextures) skin[type] = &scene.image(path);
        draw(canvas, scene, *mount, mountBones, glm::scale(glm::mat4(1.0f),glm::vec3(mountLook->scale)),viewProj,&mountLook->geosets,skin);
    }
    draw(canvas, scene, *body, bones, glm::scale(rider, glm::vec3(look.scale)), viewProj,
         itemEntry ? nullptr : &look.geosets, byType, itemEntry ? &timeMs : nullptr, itemPart);
    for (const auto& a : attachments ? look.attached : std::vector<wowcore::Attached>{}) {
        const auto model = wowcore::loadModel(mpq, a.model);
        const auto at = wowcore::placeAttached(*body, bones, a, drawn);
        if (!model || !at) {
            std::printf("attachment %u: %s %s\n", a.point, a.model.c_str(), model ? "(no point)" : "(no model)");
            continue;
        }
        std::vector<glm::mat4> itemBones;
        wowcore::poseBones(*model, 0, timeMs, timeMs, itemBones);
        const wowcore::Image& tex = scene.image(a.texture);
        draw(canvas, scene, *model, itemBones, rider * *at, viewProj, nullptr, { {2, &tex} });
        std::printf("attachment %u: %s (%s)\n", a.point, a.model.c_str(), tex.rgba.empty() ? "no texture" : "textured");
    }
    if (!effect.empty()) {
        // The effect on each spell hand, run from 0 to the time asked for (its particles and ribbons at 30 steps a
        // second), billboards turned to the camera.
        const auto fx = wowcore::loadModel(mpq, effect);
        if (!fx) {
            std::fprintf(stderr, "no effect %s\n", effect.c_str());
            return 1;
        }
        const glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0, 0, 1));
        const glm::vec3 right = glm::vec3(glm::inverse(view)[0]), up = glm::vec3(glm::inverse(view)[1]);
        std::map<uint32_t, const wowcore::Image*> none;
        for (const uint32_t point : { 21u, 22u }) {
            const auto at = wowcore::attachmentMatrix(*body, bones, point);
            if (!at) continue;
            wowcore::Emitters emitters(*fx, point);
            std::vector<glm::mat4> fxBones;
            const float step = 1000.0f / 30.0f;
            for (float t = 0.0f; t <= timeMs; t += step) {
                const glm::mat4 inv = glm::inverse(*at);
                const glm::vec3 eyeLocal = glm::vec3(inv * glm::vec4(eye, 1.0f));
                const glm::vec3 upLocal = glm::normalize(glm::vec3(inv * glm::vec4(0, 0, 1, 0)));
                wowcore::poseBones(*fx, 0, t, t, fxBones, &eyeLocal, &upLocal);
                emitters.update(step / 1000.0f, fxBones, *at, 0, t, t);
            }
            draw(canvas, scene, *fx, fxBones, *at, viewProj, nullptr, none, &timeMs);
            std::vector<wowcore::EffectQuad> quads;
            emitters.quads(right, up, quads);
            for (const auto& q : quads) {
                const wowcore::Image& tex = scene.image(q.texture < fx->textures.size() ? fx->textures[q.texture].filename : std::string());
                Vert v[4];
                for (int k = 0; k < 4; ++k) {
                    const glm::vec4 clip = viewProj * glm::vec4(q.corner[k], 1.0f);
                    v[k].screen = glm::vec3((clip.x / clip.w * 0.5f + 0.5f) * size, (0.5f - clip.y / clip.w * 0.5f) * size, clip.w);
                    v[k].uv = q.uv[k];
                    v[k].shade = v[k].normalShade = 1.0f;
                }
                triangle(canvas, v[0], v[1], v[2], tex, q.blend, false, q.color);
                triangle(canvas, v[0], v[2], v[3], tex, q.blend, false, q.color);
            }
            std::printf("effect at %u: %zu quads\n", point, quads.size());
        }
    }
    wowcore::Image out(size, size);
    out.rgba = canvas.rgba;
    return wowcore::writePng(argv[1], out) ? 0 : 1;
}
