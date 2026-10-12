#include "wowcore/player_model.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/effect.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>
namespace {
struct Canvas {
    int size;
    std::vector<uint8_t> rgba;
    std::vector<float> depth;
    explicit Canvas(int n) : size(n), rgba(static_cast<size_t>(n) * n * 4), depth(static_cast<size_t>(n) * n, 1e30f) {

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
            p[3] = static_cast<uint8_t>((t.a + p[3] / 255.0f * (1 - t.a)) * 255.0f + 0.5f);
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
          const float* effectTimeMs = nullptr) {
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
        verts[i].screen = glm::vec3((clip.x / clip.w * 0.5f + 0.5f) * c.size, (0.5f - clip.y / clip.w * 0.5f) * c.size, clip.z / clip.w);
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
            {
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
}
namespace wowcore {
struct PlayerModel::Impl {
    Scene scene;
    Look look;
    std::optional<Model> body;
    std::vector<std::optional<Model>> attached;
    explicit Impl(const Mpq& mpq) : scene{mpq,{}} {}
};
PlayerModel::PlayerModel(const Mpq& mpq) : impl_(std::make_unique<Impl>(mpq)) {}
PlayerModel::~PlayerModel() = default;
void PlayerModel::setLook(const Look& look) {
    impl_->look=look;
    impl_->body=loadModel(impl_->scene.mpq,look.body);
    impl_->attached.clear();
    for (const auto& a : look.attached) impl_->attached.push_back(loadModel(impl_->scene.mpq,a.model));
}
bool PlayerModel::ready() const { return impl_->body.has_value(); }
Image PlayerModel::render(float rotation,int size) {
    if (!ready() || size<1 || !std::isfinite(rotation)) return {};
    auto& s=*impl_;
    const auto& body=*s.body;
    std::vector<glm::mat4> bones;
    poseBones(body,std::max(findSequence(body,0),0),0,0,bones);
    glm::vec3 low(1e30f),high(-1e30f);
    for (const auto& v : body.vertices) {
        // Height is independent of rotation; include only the displayed body's geometry.
        low=glm::min(low,v.position); high=glm::max(high,v.position);
    }
    const glm::vec3 target(0,0,(low.z+high.z)*0.5f);
    const float distance=std::max(1.0f,(high.z-low.z)*0.6f/std::tan(glm::radians(17.5f)));
    const glm::vec3 eye=target+glm::vec3(std::cos(rotation),std::sin(rotation),0.05f)*distance;
    const auto vp=glm::perspective(glm::radians(35.0f),1.0f,0.1f,100.0f)*glm::lookAt(eye,target,glm::vec3(0,0,1));
    Canvas canvas(size);
    std::map<uint32_t,const Image*> textures{{1,&s.look.skin},{6,&s.scene.image(s.look.hair)},{2,&s.scene.image(s.look.cape)}};
    for (const auto& [type,path] : s.look.replaceableTextures) textures[type]=&s.scene.image(path);
    draw(canvas,s.scene,body,bones,glm::mat4(1),vp,&s.look.geosets,textures);
    for (size_t i=0;i<s.look.attached.size();++i) {
        const auto& a=s.look.attached[i];
        const auto at=placeAttached(body,bones,a,false);
        if (!s.attached[i] || !at) continue;
        std::vector<glm::mat4> itemBones;
        poseBones(*s.attached[i],std::max(findSequence(*s.attached[i],0),0),0,0,itemBones);
        draw(canvas,s.scene,*s.attached[i],itemBones,*at,vp,nullptr,{{2,&s.scene.image(a.texture)}});
    }
    Image image(size,size); image.rgba=std::move(canvas.rgba); return image;
}
}
