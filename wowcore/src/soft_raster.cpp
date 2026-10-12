#include "wowcore/soft_raster.hpp"

#include "ui/widget_renderer.hpp"   // the additive-blend callbacks

#include <imgui.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace wowcore {
namespace {
VkDescriptorSet handle(size_t index) { return reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(index + 1)); }

void copyTexture(ImTextureData* t, Image& img) {
    img = Image(t->Width, t->Height);
    const auto* src = static_cast<const uint8_t*>(t->GetPixels());
    if (t->Format == ImTextureFormat_RGBA32) {
        std::memcpy(img.rgba.data(), src, img.rgba.size());
    } else {   // Alpha8: white with coverage as alpha
        for (size_t i = 0, n = static_cast<size_t>(t->Width) * t->Height; i < n; ++i) {
            img.rgba[i * 4 + 0] = img.rgba[i * 4 + 1] = img.rgba[i * 4 + 2] = 255;
            img.rgba[i * 4 + 3] = src[i];
        }
    }
}

// Box-filtered mip chain down to 1x1, as a GPU's generated mips.
std::vector<Image> mips(Image base) {
    std::vector<Image> chain;
    chain.push_back(std::move(base));
    while (chain.back().w > 1 || chain.back().h > 1) {
        const Image& a = chain.back();
        Image b(std::max(1, a.w / 2), std::max(1, a.h / 2));
        for (int y = 0; y < b.h; ++y)
            for (int x = 0; x < b.w; ++x)
                for (int k = 0; k < 4; ++k) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx)
                            sum += a.rgba[(static_cast<size_t>(std::min(a.h - 1, y * 2 + dy)) * a.w + std::min(a.w - 1, x * 2 + dx)) * 4 + k];
                    b.rgba[(static_cast<size_t>(y) * b.w + x) * 4 + k] = static_cast<uint8_t>((sum + 2) / 4);
                }
        chain.push_back(std::move(b));
    }
    return chain;
}

struct Texel { float r, g, b, a; };

// Bilinear, wrap addressing, texel centres at half-integers - what a linear/repeat sampler returns.
Texel sample(const Image& img, float u, float v) {
    const float x = u * img.w - 0.5f, y = v * img.h - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float ax = x - fx, ay = y - fy;
    auto wrap = [](int i, int n) { i %= n; return i < 0 ? i + n : i; };
    const int x0 = wrap(static_cast<int>(fx), img.w), x1 = wrap(static_cast<int>(fx) + 1, img.w);
    const int y0 = wrap(static_cast<int>(fy), img.h), y1 = wrap(static_cast<int>(fy) + 1, img.h);
    auto at = [&](int px, int py) { return &img.rgba[(static_cast<size_t>(py) * img.w + px) * 4]; };
    const uint8_t *p00 = at(x0, y0), *p10 = at(x1, y0), *p01 = at(x0, y1), *p11 = at(x1, y1);
    float c[4];
    for (int k = 0; k < 4; ++k) {
        const float top = p00[k] + (p10[k] - p00[k]) * ax, bottom = p01[k] + (p11[k] - p01[k]) * ax;
        c[k] = (top + (bottom - top) * ay) / 255.0f;
    }
    return {c[0], c[1], c[2], c[3]};
}

Texel colour(ImU32 c) {
    return {((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f, ((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
            ((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f, ((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f};
}

Texel sampleChain(const std::vector<Image>& chain, float u, float v, float lod) {
    if (lod <= 0 || chain.size() == 1) return sample(chain[0], u, v);
    lod = std::min(lod, static_cast<float>(chain.size() - 1));
    const int l0 = static_cast<int>(lod);
    const float f = lod - l0;
    const Texel a = sample(chain[l0], u, v);
    if (f == 0 || l0 + 1 >= static_cast<int>(chain.size())) return a;
    const Texel b = sample(chain[l0 + 1], u, v);
    return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, a.a + (b.a - a.a) * f};
}

void triangle(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const std::vector<Image>* tex,
              const ImVec4& clip, Image& out, Filter filter, bool additive) {
    float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
    if (area == 0.0f) return;
    const ImDrawVert* v[3] = {&a, &b, &c};
    if (area < 0) { std::swap(v[1], v[2]); area = -area; }   // counter-clockwise in y-down space
    const int x0 = std::max(0, static_cast<int>(std::floor(std::max(clip.x, std::min({a.pos.x, b.pos.x, c.pos.x})))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::max(clip.y, std::min({a.pos.y, b.pos.y, c.pos.y})))));
    const int x1 = std::min(out.w, static_cast<int>(std::ceil(std::min(clip.z, std::max({a.pos.x, b.pos.x, c.pos.x})))));
    const int y1 = std::min(out.h, static_cast<int>(std::ceil(std::min(clip.w, std::max({a.pos.y, b.pos.y, c.pos.y})))));
    // Edge i is opposite vertex i. A pixel centre exactly on an edge goes to one side only: a shared edge runs in
    // opposite directions in its two triangles, so `owns` holds for exactly one of them.
    struct Edge { float A, B, C; bool owns; };
    Edge e[3];
    for (int i = 0; i < 3; ++i) {
        const ImVec2 p = v[(i + 1) % 3]->pos, q = v[(i + 2) % 3]->pos;
        e[i].A = p.y - q.y;
        e[i].B = q.x - p.x;
        e[i].C = p.x * q.y - p.y * q.x;
        e[i].owns = (e[i].A == 0 && e[i].B < 0) || e[i].A < 0;
    }
    // The level of detail is constant over an affine triangle: texels per pixel along the steeper screen axis.
    float lod = 0;
    if (tex && filter == Filter::Trilinear) {
        const float ex1 = v[1]->pos.x - v[0]->pos.x, ey1 = v[1]->pos.y - v[0]->pos.y;
        const float ex2 = v[2]->pos.x - v[0]->pos.x, ey2 = v[2]->pos.y - v[0]->pos.y;
        const float det = ex1 * ey2 - ex2 * ey1;
        const float W = static_cast<float>((*tex)[0].w), H = static_cast<float>((*tex)[0].h);
        const float du1 = (v[1]->uv.x - v[0]->uv.x) * W, dv1 = (v[1]->uv.y - v[0]->uv.y) * H;
        const float du2 = (v[2]->uv.x - v[0]->uv.x) * W, dv2 = (v[2]->uv.y - v[0]->uv.y) * H;
        const float dudx = (du1 * ey2 - du2 * ey1) / det, dvdx = (dv1 * ey2 - dv2 * ey1) / det;
        const float dudy = (du2 * ex1 - du1 * ex2) / det, dvdy = (dv2 * ex1 - dv1 * ex2) / det;
        const float rho = std::max(std::sqrt(dudx * dudx + dvdx * dvdx), std::sqrt(dudy * dudy + dvdy * dvdy));
        lod = rho > 1 ? std::log2(rho) : 0;
    }
    const Texel ca = colour(v[0]->col), cb = colour(v[1]->col), cc = colour(v[2]->col);
    const bool flat = v[0]->col == v[1]->col && v[1]->col == v[2]->col;
    for (int y = y0; y < y1; ++y) {
        const float py = y + 0.5f;
        if (py < clip.y || py >= clip.w) continue;
        for (int x = x0; x < x1; ++x) {
            const float px = x + 0.5f;
            if (px < clip.x || px >= clip.z) continue;
            float w[3];
            bool in = true;
            for (int i = 0; i < 3 && in; ++i) {
                w[i] = e[i].A * px + e[i].B * py + e[i].C;   // twice the area of (p, edge): positive inside
                in = w[i] > 0 || (w[i] == 0 && e[i].owns);
            }
            if (!in) continue;
            const float l0 = w[0] / area, l1 = w[1] / area, l2 = w[2] / area;
            Texel s = flat ? ca : Texel{ca.r * l0 + cb.r * l1 + cc.r * l2, ca.g * l0 + cb.g * l1 + cc.g * l2,
                                        ca.b * l0 + cb.b * l1 + cc.b * l2, ca.a * l0 + cb.a * l1 + cc.a * l2};
            if (tex) {
                const float u = v[0]->uv.x * l0 + v[1]->uv.x * l1 + v[2]->uv.x * l2;
                const float t = v[0]->uv.y * l0 + v[1]->uv.y * l1 + v[2]->uv.y * l2;
                const Texel k = sampleChain(*tex, u, t, lod);
                s = {s.r * k.r, s.g * k.g, s.b * k.b, s.a * k.a};
            }
            if (s.a <= 0.0f) continue;
            uint8_t* d = &out.rgba[(static_cast<size_t>(y) * out.w + x) * 4];
            if (additive) {   // light added (src * alpha + dst), as the in-game backend blends a model's additive batches
                const float add[3] = {s.r, s.g, s.b};
                for (int k = 0; k < 3; ++k)
                    d[k] = static_cast<uint8_t>(std::clamp(std::lround(d[k] + add[k] * s.a * 255.0f), 0L, 255L));
                d[3] = std::max(d[3], static_cast<uint8_t>(std::clamp(std::lround(s.a * std::max({s.r, s.g, s.b}) * 255.0f), 0L, 255L)));
                continue;
            }
            const float inv = 1.0f - s.a;
            auto mix = [](float src, uint8_t dst, float sa, float keep) {
                return static_cast<uint8_t>(std::clamp(std::lround(src * sa * 255.0f + dst * keep), 0L, 255L));
            };
            d[0] = mix(s.r, d[0], s.a, inv);
            d[1] = mix(s.g, d[1], s.a, inv);
            d[2] = mix(s.b, d[2], s.a, inv);
            d[3] = static_cast<uint8_t>(std::clamp(std::lround(s.a * 255.0f + d[3] * inv), 0L, 255L));
        }
    }
}
}  // namespace

bool SoftTextures::updateImGuiTexture(VkDescriptorSet texture, const uint8_t* rgba, int width, int height) {
    const auto id=reinterpret_cast<uintptr_t>(texture);
    if (!id || id>images_.size()) return false;
    Image image(width,height);
    std::memcpy(image.rgba.data(),rgba,image.rgba.size());
    images_[id-1]=mips(std::move(image));
    return true;
}

VkDescriptorSet SoftTextures::uploadImGuiTexture(const uint8_t* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) return VK_NULL_HANDLE;
    Image img(width, height);
    std::memcpy(img.rgba.data(), rgba, img.rgba.size());
    images_.push_back(mips(std::move(img)));
    return handle(images_.size() - 1);
}

void SoftTextures::sync(ImDrawData* dd) {
    if (!dd || !dd->Textures) return;
    for (ImTextureData* t : *dd->Textures) {
        if (getenv("WOWCORE_TEXLOG") && t->Status != ImTextureStatus_OK)
            fprintf(stderr, "tex #%d %dx%d status %d updates %d\n", t->UniqueID, t->Width, t->Height, (int)t->Status, t->Updates.Size);
        if (t->Status == ImTextureStatus_WantCreate) {   // ImGui's own textures are drawn 1:1: no mips
            images_.emplace_back(1);
            copyTexture(t, images_.back()[0]);
            t->SetTexID(static_cast<ImTextureID>(images_.size()));
            t->SetStatus(ImTextureStatus_OK);
        } else if (t->Status == ImTextureStatus_WantUpdates) {
            copyTexture(t, images_[static_cast<size_t>(t->GetTexID()) - 1][0]);
            t->SetStatus(ImTextureStatus_OK);
        } else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0) {
            images_[static_cast<size_t>(t->GetTexID()) - 1].clear();
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

const std::vector<Image>* SoftTextures::get(uint64_t id) const {
    if (id == 0 || id > images_.size() || images_[id - 1].empty()) return nullptr;
    return &images_[id - 1];
}

void rasterize(const ImDrawData* dd, const SoftTextures& textures, Image& target, Filter filter) {
    if (!dd) return;
    const ImVec2 off = dd->DisplayPos;
    bool additive = false;
    for (const ImDrawList* list : dd->CmdLists) {
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {   // the interface's only render-state callbacks: additive and back
                if (cmd.UserCallback == wowee::ui::HostAdditiveBlend) additive = true;
                else if (cmd.UserCallback == wowee::ui::HostAlphaBlend) additive = false;
                continue;
            }
            const ImVec4 clip(cmd.ClipRect.x - off.x, cmd.ClipRect.y - off.y, cmd.ClipRect.z - off.x, cmd.ClipRect.w - off.y);
            const std::vector<Image>* tex = textures.get(static_cast<uint64_t>(cmd.GetTexID()));
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawIdx* idx = &list->IdxBuffer[static_cast<int>(cmd.IdxOffset + i)];
                ImDrawVert a = list->VtxBuffer[static_cast<int>(cmd.VtxOffset + idx[0])];
                ImDrawVert b = list->VtxBuffer[static_cast<int>(cmd.VtxOffset + idx[1])];
                ImDrawVert c = list->VtxBuffer[static_cast<int>(cmd.VtxOffset + idx[2])];
                for (ImDrawVert* p : {&a, &b, &c}) { p->pos.x -= off.x; p->pos.y -= off.y; }
                triangle(a, b, c, tex, clip, target, filter, additive);
            }
        }
    }
}

bool writePng(const std::string& path, const Image& img) {
    auto be32 = [](std::string& s, uint32_t v) { for (int k = 3; k >= 0; --k) s += static_cast<char>((v >> (k * 8)) & 0xFF); };
    auto chunk = [&](std::string& out, const char* type, const std::string& data) {
        be32(out, static_cast<uint32_t>(data.size()));
        const std::string body = std::string(type, 4) + data;
        out += body;
        be32(out, static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(body.data()), static_cast<uInt>(body.size()))));
    };
    std::string raw;
    raw.reserve(static_cast<size_t>(img.h) * (img.w * 4 + 1));
    for (int y = 0; y < img.h; ++y) {
        raw += '\0';
        raw.append(reinterpret_cast<const char*>(&img.rgba[static_cast<size_t>(y) * img.w * 4]), static_cast<size_t>(img.w) * 4);
    }
    uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
    std::string z(zlen, '\0');
    if (compress2(reinterpret_cast<Bytef*>(z.data()), &zlen, reinterpret_cast<const Bytef*>(raw.data()),
                  static_cast<uLong>(raw.size()), 6) != Z_OK) return false;
    z.resize(zlen);
    std::string ihdr;
    be32(ihdr, static_cast<uint32_t>(img.w));
    be32(ihdr, static_cast<uint32_t>(img.h));
    ihdr += std::string("\x08\x06\x00\x00\x00", 5);   // 8-bit RGBA
    std::string png("\x89PNG\r\n\x1a\n", 8);
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    std::ofstream f(path, std::ios::binary);
    f.write(png.data(), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(f);
}
}  // namespace wowcore
