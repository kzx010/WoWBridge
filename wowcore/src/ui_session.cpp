#include "wowcore/ui_session.hpp"

#include "addons/lua_engine.hpp"
#include "addons/lua_api_helpers.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/vk_context.hpp"
#include "ui/interface_fonts.hpp"
#include "ui/widget_renderer.hpp"
#include "wowcore/effect.hpp"
#include "wowcore/framexml.hpp"
#include "wowcore/model.hpp"
#include "wowcore/player_model.hpp"
#include "wowcore/minimap.hpp"
#include "wowcore/worldmap.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/wowapi.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <limits>
#include <unordered_map>

namespace wowcore {
struct UiSession::Impl {
    ImVec2 pointer{ -FLT_MAX, -FLT_MAX };   // none yet
    std::shared_ptr<Mpq> mpq;
    const ClientDb& db;
    wowee::rendering::VkContext& textures;
    wowee::pipeline::AssetManager assets;
    wowee::addons::LuaEngine engine;
    wowee::ui::WidgetRenderer renderer;
    ImGuiContext* imgui = nullptr;

    Impl(std::shared_ptr<Mpq> m, const ClientDb& d, wowee::rendering::VkContext& t)
        : mpq(std::move(m)), db(d), textures(t), assets(mpq) {}

    // The interface's faces, as WoWee's client sets them up (ui_manager.cpp): FRIZQT first, as ImGui's default; then
    // each face FrameXML names, registered under its file name. Heights mean an em, as in the client.
    int fonts = 0;   // faces registered for the interface
    void addFonts() {
        ImGuiIO& io = ImGui::GetIO();
        auto add = [&](const char* file) -> ImFont* {
            auto data = mpq->read(std::string("Fonts\\") + file);
            if (!data) return nullptr;
            ImFontConfig cfg;
            // Preserve FreeType coverage. Boosting it thickened tooltip and HUD glyph edges.
            cfg.RasterizerMultiply = 1.0f;
            cfg.ExtraSizeScale = wowee::ui::fontEmSizeScale(data->data(), data->size());
            cfg.PixelSnapH = true;   // whole-pixel advances, as the client's (patch_wowee.py patch_fonts)
            void* owned = IM_ALLOC(data->size());
            std::memcpy(owned, data->data(), data->size());
            ImFont* font = io.Fonts->AddFontFromMemoryTTF(owned, static_cast<int>(data->size()), 18.0f, &cfg);
            wowee::ui::registerInterfaceFaceMetrics(font, owned, data->size());   // the client's advances and widths
            return font;
        };
        add("FRIZQT__.TTF");
        for (const char* face : {"FRIZQT__.TTF", "MORPHEUS.TTF", "SKURRI.TTF", "ARIALN.TTF", "FRIENDS.TTF"}) {
            if (ImFont* f = add(face)) {
                ++fonts;
                std::string lower(face);
                for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                wowee::ui::registerInterfaceFace(lower, f);
            }
        }
    }

    // A convex polygon in pixels with its texture coordinates, cut where the texture's edges fall: on an axis the
    // texture repeats along, each piece is moved into the texture; on one it is clamped along, a piece outside it takes
    // the edge texel.
    struct PolyVertex { glm::vec2 pos, uv; glm::vec4 color{1.0f}; };
    using Polygon = std::vector<PolyVertex>;
    static void cut(const Polygon& in, int axis, float at, Polygon& below, Polygon& above) {
        for (size_t i = 0; i < in.size(); ++i) {
            const PolyVertex& a = in[i];
            const PolyVertex& b = in[(i + 1) % in.size()];
            const float da = a.uv[axis] - at, db = b.uv[axis] - at;
            if (da <= 0.0f) below.push_back(a);
            if (da >= 0.0f) above.push_back(a);
            if ((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) {
                const float t = da / (da - db);
                PolyVertex mid{glm::mix(a.pos, b.pos, t), glm::mix(a.uv, b.uv, t), glm::mix(a.color, b.color, t)};
                mid.uv[axis] = at;
                below.push_back(mid);
                above.push_back(mid);
            }
        }
    }
    static void address(const Polygon& poly, int axis, bool repeats, float halfTexel, std::vector<Polygon>& out) {
        float lo = poly[0].uv[axis], hi = lo;
        for (const auto& v : poly) lo = std::min(lo, v.uv[axis]), hi = std::max(hi, v.uv[axis]);
        std::vector<float> cuts;
        if (repeats) for (float k = std::floor(lo) + 1.0f; k < hi; k += 1.0f) cuts.push_back(k);
        else for (float k : {0.0f, 1.0f}) if (k > lo && k < hi) cuts.push_back(k);
        Polygon rest = poly;
        const auto emit = [&](Polygon piece) {
            if (piece.size() < 3) return;
            float centre = 0.0f;
            for (const auto& v : piece) centre += v.uv[axis];
            centre /= float(piece.size());
            for (auto& v : piece) {
                // Cutting many tiles can accumulate a few ulps at a boundary.
                if (repeats) v.uv[axis] = std::clamp(v.uv[axis] - std::floor(centre), 0.0f, 1.0f);
                else if (centre < 0.0f) v.uv[axis] = halfTexel;
                else if (centre > 1.0f) v.uv[axis] = 1.0f - halfTexel;
            }
            out.push_back(std::move(piece));
        };
        for (float at : cuts) {
            Polygon below, above;
            cut(rest, axis, at, below, above);
            emit(std::move(below));
            rest = std::move(above);
        }
        emit(std::move(rest));
    }

    // FrameXML's SetTexCoord can repeat art (the shapeshift middle uses 0..numForms-2).
    // Split those triangles at tile edges so the host's clamp sampler and the native
    // repeat sampler draw the same picture. Leave ordinary commands, including the
    // font atlas, intact. Appended vertices use VtxOffset, including on 16-bit ImGui.
    static void tileTextures(ImDrawData* data) {
        data->TotalVtxCount = data->TotalIdxCount = 0;
        for (ImDrawList* list : data->CmdLists) {
            ImVector<ImDrawCmd> commands;
            ImVector<ImDrawIdx> indices;
            for (const ImDrawCmd& source : list->CmdBuffer) {
                auto vertex = [&](unsigned int i) -> ImDrawVert {
                    return list->VtxBuffer[source.VtxOffset + list->IdxBuffer[source.IdxOffset + i]];
                };
                bool repeats = false;
                if (!source.UserCallback) {
                    for (unsigned int i = 0; i < source.ElemCount && !repeats; ++i) {
                        const ImVec2 uv = vertex(i).uv;
                        repeats = uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1;
                    }
                }
                ImDrawCmd cmd = source;
                cmd.IdxOffset = indices.Size;
                if (!repeats) {
                    for (unsigned int i = 0; i < source.ElemCount; ++i)
                        indices.push_back(list->IdxBuffer[source.IdxOffset + i]);
                    commands.push_back(cmd);
                    continue;
                }
                cmd.VtxOffset = list->VtxBuffer.Size;
                cmd.ElemCount = 0;
                auto emit = [&](const PolyVertex& v) {
                    const unsigned int local = list->VtxBuffer.Size - cmd.VtxOffset;
                    const glm::vec4 c = glm::clamp(v.color, 0.0f, 1.0f);
                    list->VtxBuffer.push_back({{v.pos.x, v.pos.y}, {v.uv.x, v.uv.y},
                        IM_COL32(int(c.r * 255 + 0.5f), int(c.g * 255 + 0.5f),
                                 int(c.b * 255 + 0.5f), int(c.a * 255 + 0.5f))});
                    indices.push_back(static_cast<ImDrawIdx>(local));
                    ++cmd.ElemCount;
                };
                for (unsigned int i = 0; i + 2 < source.ElemCount; i += 3) {
                    Polygon tri;
                    bool across = false, down = false;
                    for (unsigned int k = 0; k < 3; ++k) {
                        const ImDrawVert v = vertex(i + k);
                        tri.push_back({{v.pos.x, v.pos.y}, {v.uv.x, v.uv.y},
                            {((v.col >> IM_COL32_R_SHIFT) & 255) / 255.0f,
                             ((v.col >> IM_COL32_G_SHIFT) & 255) / 255.0f,
                             ((v.col >> IM_COL32_B_SHIFT) & 255) / 255.0f,
                             ((v.col >> IM_COL32_A_SHIFT) & 255) / 255.0f}});
                        across |= v.uv.x < 0 || v.uv.x > 1;
                        down |= v.uv.y < 0 || v.uv.y > 1;
                    }
                    std::vector<Polygon> horizontal, pieces;
                    if (across) address(tri, 0, true, 0, horizontal);
                    else horizontal.push_back(std::move(tri));
                    for (const Polygon& p : horizontal) {
                        if (down) address(p, 1, true, 0, pieces);
                        else pieces.push_back(p);
                    }
                    for (const Polygon& p : pieces) {
                        for (size_t k = 1; k + 1 < p.size(); ++k) {
                            if (list->VtxBuffer.Size - cmd.VtxOffset + 3 > std::numeric_limits<ImDrawIdx>::max()) {
                                commands.push_back(cmd);
                                cmd.VtxOffset = list->VtxBuffer.Size;
                                cmd.IdxOffset = indices.Size;
                                cmd.ElemCount = 0;
                            }
                            emit(p[0]); emit(p[k]); emit(p[k + 1]);
                        }
                    }
                }
                commands.push_back(cmd);
            }
            list->CmdBuffer.swap(commands);
            list->IdxBuffer.swap(indices);
            data->TotalVtxCount += list->VtxBuffer.Size;
            data->TotalIdxCount += list->IdxBuffer.Size;
        }
    }

    // Model frames the interface runs. 1.12's cooldown is one: CooldownFrameTemplate is a <Model> playing
    // Interface\Cooldown\UI-Cooldown-Indicator.mdx - sequence 0 the sweep, its time set from the cooldown's progress,
    // then sequence 1 the flash as it ends (Cooldown.lua). The interface keeps each frame's model, animation and time
    // (wowapi_model.lua -> __WBModelState); this runs the M2 as the effect models are run (bones; each batch's colour,
    // transparency and texture animation: batchLook) and hands the renderer its triangles. Drawn face on, x across and
    // y up, the extent of its blended (not additive) geometry filling the frame: where the client draws the
    // cooldown's square (evidence/hudref/gcd).
    struct ModelFrame { std::string file; uint32_t anim = 0; float ms = 0.0f; float rotation = 0.61f; };
    std::unique_ptr<PlayerModel> playerModel, dressModel;
    Appearance playerAppearance;
    std::vector<Worn> playerWorn, dressWorn;
    VkDescriptorSet dressTexture = VK_NULL_HANDLE;
    float dressRotation = std::numeric_limits<float>::quiet_NaN();
    bool dressDirty = true;
    uint32_t dressTextureGeneration = 0;
    VkDescriptorSet playerTexture = VK_NULL_HANDLE;
    float playerRotation = std::numeric_limits<float>::quiet_NaN();
    bool playerDirty = true;
    uint32_t playerTextureGeneration = 0;
    struct UiModel { std::optional<Model> model; glm::vec2 lo{0.0f}, hi{0.0f}; };
    std::unordered_map<uint32_t, ModelFrame> modelFrames;   // by widget id
    std::unordered_map<std::string, glm::vec2> textureSizes; // texels, from each BLP's header

    glm::vec2 textureSize(const std::string& path) {
        auto it = textureSizes.find(path);
        if (it == textureSizes.end()) {
            glm::vec2 size(1.0f);
            if (const auto blp = mpq->read(path); blp && blp->size() >= 20) {   // BLP2: magic, type, flags, width, height
                uint32_t w = 0, h = 0;
                std::memcpy(&w, blp->data() + 12, 4);
                std::memcpy(&h, blp->data() + 16, 4);
                if (w && h) size = glm::vec2(float(w), float(h));
            }
            it = textureSizes.emplace(path, size).first;
        }
        return it->second;
    }
    std::unordered_map<std::string, UiModel> uiModels;      // by file

    const MinimapWorld* minimapWorld = nullptr; // owned by the host's atlas
    std::vector<const MinimapWorld*> minimapLayers; // child first, nearest parent next
    MinimapView minimapView;
    bool minimapPose = false;
    double minimapTrackingMask = 0;
    struct TrackedBlip { MapPoint position; int reaction; std::string name; };
    std::vector<TrackedBlip> minimapBlips;
    struct Poi { MapPoint position; std::string name, kind; int icon; bool discovered; int nativeFrame=0; double opacity=1, scale=1; };
    bool minimapNative=false;
    static constexpr const char* compassAtlas="Interface\\Icons\\WB_Compass.blp";
    std::vector<Poi> minimapPois;
    std::vector<const MinimapWorld*> worldMapLayers;
    std::vector<const MinimapWorld*> taxiMapLayers;
    MapPoint worldMapPlayer;
    double worldMapHeading = 0;
    static constexpr double poiRimInset = 10.0 / 70; // keep the visible arrow inside FrameXML's border art

    MinimapView currentMinimapView(const wowee::ui::Widget& w) {
        auto view = minimapView;
        view.radius = *minimapRadius(200, w.zoomLevel);
        auto* L = engine.getState();
        lua_getglobal(L, "GetCVar");
        lua_pushliteral(L, "rotateMinimap");
        if (lua_pcall(L, 1, 1, 0) == 0) {
            const char* value = lua_tostring(L, -1);
            view.rotate = value && std::strcmp(value, "1") == 0;
        }
        lua_pop(L, 1);
        return view;
    }

    void minimapTooltip(float height) {
        auto& tree = engine.widgets();
        const auto* w = tree.findByName("Minimap");
        const std::string* hover = nullptr;
        if (minimapPose && w && w->visible && w->rectW > 0 && w->rectH > 0 &&
            tree.hitTest(pointer.x / tree.uiScale(), (height - pointer.y) / tree.uiScale()) == w->id) {
            const float scale = tree.uiScale();
            const double radius = std::min(w->rectW, w->rectH) * scale / 2;
            const MapPoint cursor{(pointer.x - (w->left + w->rectW / 2) * scale) / radius,
                (height - pointer.y - (w->bottom + w->rectH / 2) * scale) / radius};
            if (minimapContains(cursor)) {
                const auto view = currentMinimapView(*w);
                double nearest = w->rectW * scale * (7.0 / 140) / radius;
                for (const auto& poi : minimapPois) {
                    if (poi.opacity<=0 || poi.scale<=0) continue;
                    const auto p = minimapPoi(view, poi.position, poiRimInset);
                    if (!p) continue;
                    const double distance = std::hypot(p->centre.x - cursor.x, p->centre.y - cursor.y);
                    if (distance <= nearest) { nearest = distance; hover = &poi.name; }
                }
                auto* L = engine.getState();
                lua_getglobal(L, "__WB");
                lua_getfield(L, -1, "trackingCreatureMask");
                const bool tracking = lua_pcall(L, 0, 1, 0) == 0 && lua_tonumber(L, -1) != 0 &&
                    static_cast<uint32_t>(lua_tonumber(L, -1)) == minimapTrackingMask;
                lua_pop(L, 2);
                if (tracking) for (const auto& blip : minimapBlips) {
                    const auto at = minimapProject(view, blip.position);
                    if (!at || blip.name.empty()) continue;
                    const double distance = std::hypot(at->x - cursor.x, at->y - cursor.y);
                    if (distance <= nearest) { nearest = distance; hover = &blip.name; }
                }
            }
        }
        auto* L = engine.getState();
        lua_getglobal(L, "__WBMinimapTooltip");
        if (hover) lua_pushlstring(L, hover->data(), hover->size());
        else lua_pushnil(L);
        if (lua_pcall(L, 1, 0, 0) != 0) lua_pop(L, 1);
    }

    void drawMinimap(const wowee::ui::Widget& w, float x0, float y0, float x1, float y1,
                     std::vector<wowee::ui::HostModelBatch>& out) {
        if (!minimapPose) return;
        lua_State* L = engine.getState();
        lua_getglobal(L, "__WB");
        lua_getfield(L, -1, "trackingCreatureMask");
        lua_remove(L, -2);
        const bool tracking = lua_pcall(L, 0, 1, 0) == 0 && lua_tonumber(L, -1) != 0 &&
            lua_tonumber(L, -1) == minimapTrackingMask;
        lua_pop(L, 1);
        if (minimapLayers.empty() && minimapPois.empty() && !minimapNative && (!tracking || minimapBlips.empty())) return;
        const auto view = currentMinimapView(w);
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
        const float radius = std::min(x1 - x0, y1 - y0) * 0.5f;
        if (radius <= 0) return;
        // Child coverage cuts holes in parent geometry. Every terrain pixel is
        // drawn once, including at inherited alpha < 1 and offset tile grids.
        std::vector<const MinimapTile*> covering;
        for (const auto* layer : minimapLayers) {
            const auto tiles = layer->around(view);
            for (const auto* tile : tiles) {
                if (!mpq->exists(tile->texture)) continue;
                wowee::ui::HostModelBatch batch;
                batch.texture = tile->texture;
                for (const auto& vertex : minimapTileTriangles(view, *tile, covering))
                    batch.triangles.push_back({cx + float(vertex.disc.x) * radius, cy - float(vertex.disc.y) * radius,
                                               float(vertex.uv.x), float(vertex.uv.y), IM_COL32_WHITE});
                if (!batch.triangles.empty()) out.push_back(std::move(batch));
            }
            for (const auto* tile : tiles) if (mpq->exists(tile->texture)) covering.push_back(tile);
        }
        if (tracking) {
            wowee::ui::HostModelBatch blips;
            blips.texture = "Interface\\Minimap\\ObjectIcons.blp";
            const double half = (x1 - x0) * (4.0 / 140.0) / radius; // 8 interface units, independent of zoom
            for (const auto& blip : minimapBlips) {
                const auto d = minimapProject(view, blip.position);
                if (!d) continue;
                const int cell = blip.reaction <= 3 ? 1 : blip.reaction == 4 ? 0 : 2;
                // 32x32 cells in the shipped 128x128 atlas: yellow, red, green.
                for (const auto& v : minimapGlyphTriangles(*d, {half, half},
                        {(cell * 32 + 0.5) / 128, 0.5 / 128}, {(cell * 32 + 31.5) / 128, 31.5 / 128}))
                    blips.triangles.push_back({cx + float(v.disc.x) * radius, cy - float(v.disc.y) * radius,
                        float(v.uv.x), float(v.uv.y), IM_COL32_WHITE});
            }
            if (!blips.triangles.empty()) out.push_back(std::move(blips));
        }
        if (minimapNative) { wowee::ui::HostModelBatch warm; warm.texture=compassAtlas; out.push_back(std::move(warm)); }
        for (const auto& poi : minimapPois) {
            if (poi.opacity<=0 || poi.scale<=0) continue;
            const auto p = minimapPoi(view, poi.position, poiRimInset);
            if (!p) continue;
            wowee::ui::HostModelBatch glyph;
            MapPoint lo{}, hi{1, 1};
            const double half = (x1 - x0) * ((poi.nativeFrame ? 9.0*poi.scale : p->arrow ? 12.0 : 6.0) / 140) / radius;
            if (poi.nativeFrame) {
                glyph.texture=compassAtlas; const int index=poi.nativeFrame-1;
                lo={((index%16)*32+.5)/512,((index/16)*32+.5)/512};
                hi={((index%16)*32+31.5)/512,((index/16)*32+31.5)/512};
            } else if (p->arrow) {
                glyph.texture = poi.kind == "quest" ? "Interface\\Minimap\\Rotating-MinimapCorpseArrow.blp" :
                                                     "Interface\\Minimap\\Rotating-MinimapGuideArrow.blp";
            } else if (poi.kind == "custom") {
                glyph.texture = "Interface\\Minimap\\ObjectIcons.blp";
                lo = {0.5 / 128, 32.5 / 128}; hi = {31.5 / 128, 63.5 / 128};
            } else {
                glyph.texture = "Interface\\Minimap\\POIIcons.blp";
                const int icon = poi.kind == "quest" ? 6 : poi.icon;
                lo = {((icon % 8) * 16 + 0.5) / 128, ((icon / 8) * 16 + 0.5) / 128};
                hi = {((icon % 8) * 16 + 15.5) / 128, ((icon / 8) * 16 + 15.5) / 128};
            }
            const auto colour = poi.nativeFrame ? IM_COL32(255,255,255,int(std::clamp(poi.opacity,0.0,1.0)*255)) :
                poi.kind == "location" && !poi.discovered ? IM_COL32(150,150,150,255) : IM_COL32_WHITE;
            for (const auto& v : minimapGlyphTriangles(p->centre, {half, half}, lo, hi, poi.nativeFrame ? 0 : p->heading))
                glyph.triangles.push_back({cx + float(v.disc.x) * radius, cy - float(v.disc.y) * radius,
                    float(v.uv.x), float(v.uv.y), colour});
            if (!glyph.triangles.empty()) out.push_back(std::move(glyph));
        }
        // MinimapArrow.m2 is a planar textured quad. Draw its own client BLP,
        // upright at heading zero, in a fixed 16-interface-unit square.
        const auto arrow = minimapArrow(view);
        if (!arrow) return;
        const double angle = view.rotate ? 0 : view.heading;
        const double cs = std::cos(angle), sn = std::sin(angle);
        const float half = std::min(radius * 0.5f, (x1 - x0) * (8.0f / 140.0f));
        wowee::ui::HostModelBatch batch;
        batch.texture = "Interface\\Minimap\\MinimapArrow.blp";
        const MapPoint corners[] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
        const MapPoint uvs[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (const int i : {0, 1, 2, 0, 2, 3}) {
            const auto p = corners[i];
            batch.triangles.push_back({cx + float(cs * p.x + sn * p.y) * half,
                                      cy - float(-sn * p.x + cs * p.y) * half,
                                      float(uvs[i].x), float(uvs[i].y), IM_COL32_WHITE});
        }
        out.push_back(std::move(batch));
    }

    void drawWorldMap(float x0, float y0, float x1, float y1,
                      std::vector<wowee::ui::HostModelBatch>& out, bool taxi = false) {
        auto* L = engine.getState();
        const int top = lua_gettop(L);
        lua_getglobal(L, "__WB"); lua_getfield(L, -1, taxi ? "taxiView" : "worldMapView");
        const auto number = [&](const char* key) {
            lua_getfield(L, -1, key);
            const double n = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : std::numeric_limits<double>::quiet_NaN();
            lua_pop(L, 1); return n;
        };
        WorldMapView view;
        if (lua_istable(L, -1)) view = {{number("lowX"),number("lowY")},{number("highX"),number("highY")}};
        lua_settop(L, top);
        const auto& layers = taxi ? taxiMapLayers : worldMapLayers;
        if (layers.empty() || !worldMapProject(view, view.low)) return;
        // Rectangular tile clipping preserves child/parent coverage and never
        // stretches the continent: Lua fits its bounds to the client's aspect.
        std::vector<const MinimapTile*> covering;
        for (const auto* layer : layers) {
            std::vector<const MinimapTile*> tiles;
            const bool overview=std::max(view.high.x-view.low.x,view.high.y-view.low.y)>1600 &&
                mpq->exists(layer->overviewTexture);
            for (const auto& [key,tile] : layer->tiles)
                if (tile.origin.x<view.high.x && tile.origin.y<view.high.y && tile.origin.x+tile.span>view.low.x &&
                    tile.origin.y+tile.span>view.low.y && (overview || mpq->exists(tile.texture))) tiles.push_back(&tile);
            const auto batch=[&](const std::string& texture,const std::vector<WorldMapVertex>& vertices) {
                wowee::ui::HostModelBatch b; b.texture=texture;
                for (const auto& v : vertices)
                    b.triangles.push_back({x0+float(v.position.x)*(x1-x0),y0+float(v.position.y)*(y1-y0),
                        float(v.uv.x),float(v.uv.y),IM_COL32_WHITE});
                if (!b.triangles.empty()) out.push_back(std::move(b));
            };
            if (overview) {
                // tools/skyrim_minimap.py uses 32 pixels/tile, capped at 1024.
                const double width=(layer->high.x-layer->low.x)/8,height=(layer->high.y-layer->low.y)/8;
                const double ratio=std::min(1.0,1024/std::max(width,height));
                batch(layer->overviewTexture,worldMapRectTriangles(view,{layer->low,layer->high},
                    {std::max(1.0,std::floor(width*ratio)),std::max(1.0,std::floor(height*ratio))},covering));
            } else {
                for (const auto* tile : tiles)
                    batch(tile->texture,worldMapTileTriangles(view,*tile,covering));
            }
            covering.insert(covering.end(),tiles.begin(),tiles.end());
        }
        if (taxi) return; // TaxiFrame's CURRENT button is its position marker.
        const auto at = worldMapProject(view,worldMapPlayer);
        if (!at || !worldMapContains(*at)) return;
        const float cx=x0+float(at->x)*(x1-x0), cy=y0+float(at->y)*(y1-y0);
        const float half=(x1-x0)*8.0f/1002.0f;
        const double cs=std::cos(worldMapHeading),sn=std::sin(worldMapHeading);
        const MapPoint corners[]={{-1,-1},{1,-1},{1,1},{-1,1}},uvs[]={{0,0},{1,0},{1,1},{0,1}};
        wowee::ui::HostModelBatch arrow;
        arrow.texture="Interface\\Minimap\\MinimapArrow.blp";
        for (int i : {0,1,2,0,2,3}) {
            const auto p=corners[i];
            arrow.triangles.push_back({cx+float(cs*p.x-sn*p.y)*half,cy+float(sn*p.x+cs*p.y)*half,
                float(uvs[i].x),float(uvs[i].y),IM_COL32_WHITE});
        }
        out.push_back(std::move(arrow));
    }

    const Model* uiModel(const std::string& file, glm::vec2* lo = nullptr, glm::vec2* hi = nullptr) {
        auto it = uiModels.find(file);
        if (it == uiModels.end()) {
            UiModel u;
            u.model = loadModel(*mpq, file);
            if (u.model) {
                bool any = false;
                for (bool blendedOnly : {true, false}) {
                    for (const auto& b : u.model->batches) {
                        const auto blend = b.materialIndex < u.model->materials.size() ? u.model->materials[b.materialIndex].blendMode : 0;
                        if (blendedOnly && blend >= 3) continue;
                        for (uint32_t i = b.indexStart; i < b.indexStart + b.indexCount && i < u.model->indices.size(); ++i) {
                            const glm::vec2 p(u.model->vertices[u.model->indices[i]].position);
                            u.lo = any ? glm::min(u.lo, p) : p;
                            u.hi = any ? glm::max(u.hi, p) : p;
                            any = true;
                        }
                    }
                    if (any) break;
                }
            }
            it = uiModels.emplace(file, std::move(u)).first;
        }
        if (lo) *lo = it->second.lo;
        if (hi) *hi = it->second.hi;
        return it->second.model ? &*it->second.model : nullptr;
    }

    void drawModel(const wowee::ui::Widget& w, float x0, float y0, float x1, float y1,
                   std::vector<wowee::ui::HostModelBatch>& out) {
        if (w.name == "WorldMapDetailFrame") {
            drawWorldMap(x0,y0,x1,y1,out);
            return;
        }
        if (w.name == "TaxiRouteMap") {
            drawWorldMap(x0,y0,x1,y1,out,true);
            return;
        }
        if (w.objectType == "Minimap") {
            drawMinimap(w, x0, y0, x1, y1, out);
            return;
        }
        const auto f = modelFrames.find(w.id);
        if (f == modelFrames.end()) return;
        if (f->second.file == "__player" || f->second.file == "__dress") {
            const bool dressing=f->second.file == "__dress";
            auto& model=dressing ? dressModel : playerModel;
            auto& texture=dressing ? dressTexture : playerTexture;
            auto& generation=dressing ? dressTextureGeneration : playerTextureGeneration;
            auto& dirty=dressing ? dressDirty : playerDirty;
            auto& rotation=dressing ? dressRotation : playerRotation;
            if (!model || !model->ready()) return;
            if (generation != textures.uiTextureGeneration()) {
                texture=VK_NULL_HANDLE; dirty=true;
                generation=textures.uiTextureGeneration();
            }
            if (dirty || rotation != f->second.rotation) {
                const auto image=model->render(f->second.rotation);
                if (image.rgba.empty()) return;
                if (!texture) texture=textures.uploadImGuiTexture(image.rgba.data(),image.w,image.h);
                else if (!textures.updateImGuiTexture(texture,image.rgba.data(),image.w,image.h)) return;
                if (!texture) return;
                dirty=false; rotation=f->second.rotation;
            }
            wowee::ui::HostModelBatch batch;
            batch.externalTexture=reinterpret_cast<uintptr_t>(texture);
            for (const auto& v : {wowee::ui::HostModelVertex{x0,y0,0,0,IM_COL32_WHITE},
                    {x1,y0,1,0,IM_COL32_WHITE},{x1,y1,1,1,IM_COL32_WHITE},
                    {x0,y0,0,0,IM_COL32_WHITE},{x1,y1,1,1,IM_COL32_WHITE},{x0,y1,0,1,IM_COL32_WHITE}})
                batch.triangles.push_back(v);
            out.push_back(std::move(batch)); return;
        }
        glm::vec2 lo, hi;
        const Model* m = uiModel(f->second.file, &lo, &hi);
        const int seq = m ? findSequence(*m, f->second.anim) : -1;
        if (seq < 0) return;
        std::vector<glm::mat4> bones;
        poseBones(*m, seq, f->second.ms, 0.0f, bones);
        const glm::vec2 span = glm::max(hi - lo, glm::vec2(1e-6f));
        for (const auto& b : m->batches) {
            const BatchLook look = batchLook(*m, b, seq, f->second.ms, 0.0f);
            if (look.color.a <= 0.002f) continue;
            const auto lookup = b.textureIndex < m->textureLookup.size() ? m->textureLookup[b.textureIndex] : 0;
            if (lookup >= m->textures.size() || m->textures[lookup].filename.empty()) continue;
            wowee::ui::HostModelBatch batch;
            batch.texture = m->textures[lookup].filename;
            batch.add = b.materialIndex < m->materials.size() && m->materials[b.materialIndex].blendMode >= 3;
            const glm::vec4 c = glm::clamp(look.color, 0.0f, 1.0f);
            const uint32_t col = IM_COL32(int(c.r * 255.0f + 0.5f), int(c.g * 255.0f + 0.5f), int(c.b * 255.0f + 0.5f), int(c.a * 255.0f + 0.5f));
            const uint32_t flags = m->textures[lookup].flags;   // 1: repeats across, 2: repeats down; else clamped
            const glm::vec2 halfTexel = 0.5f / textureSize(batch.texture);
            Polygon tri;
            for (uint32_t i = b.indexStart; i < b.indexStart + b.indexCount && i < m->indices.size(); ++i) {
                const auto& v = m->vertices[m->indices[i]];
                glm::vec4 p(0.0f);
                for (int k = 0; k < 4; ++k)
                    if (v.boneWeights[k] && v.boneIndices[k] < bones.size())
                        p += (v.boneWeights[k] / 255.0f) * (bones[v.boneIndices[k]] * glm::vec4(v.position, 1.0f));
                tri.push_back({{x0 + (p.x - lo.x) / span.x * (x1 - x0), y1 - (p.y - lo.y) / span.y * (y1 - y0)},
                               glm::vec2(look.uv * glm::vec3(v.texCoords[0], 1.0f))});
                if (tri.size() < 3) continue;
                // The texture's own addressing, so neither sampler's (the D3D11 one clamps, the CPU one repeats)
                // decides what a turned or scrolled texture shows.
                std::vector<Polygon> across, pieces;
                address(tri, 0, flags & 1, halfTexel.x, across);
                for (const Polygon& a : across) address(a, 1, flags & 2, halfTexel.y, pieces);
                for (const Polygon& piece : pieces)
                    for (size_t k = 1; k + 1 < piece.size(); ++k)
                        for (const PolyVertex* pv : {&piece[0], &piece[k], &piece[k + 1]})
                            batch.triangles.push_back({pv->pos.x, pv->pos.y, pv->uv.x, pv->uv.y, col});
                tri.clear();
            }
            out.push_back(std::move(batch));
        }
    }
};

namespace {
const char* const kSessionKey = "wowcore.UiSession";

// LoadAddOn(name): the client's load-on-demand addons (Blizzard_BindingUI, ...) from the archives, then ADDON_LOADED.
int lua_LoadAddOn(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kSessionKey);
    auto* s = static_cast<UiSession*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    const std::string name = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "wowcore.addons");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "wowcore.addons");
    }
    lua_getfield(L, -1, name.c_str());
    const bool loaded = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (loaded) { lua_pop(L, 1); lua_pushnumber(L, 1); return 1; }
    std::string err;
    if (!s || !s->loadAddOn(name, &err)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, err.empty() ? "MISSING" : "CORRUPT");
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, name.c_str());
    lua_pop(L, 1);
    lua_pushnumber(L, 1);
    return 1;
}

UiSession::Impl* sessionImpl(lua_State* L);

// __WBModelState(frame, file, animation, ms): what a Model frame shows now ("" file: nothing).
int lua_ModelState(lua_State* L) {
    auto* impl = sessionImpl(L);
    if (!impl || !lua_istable(L, 1)) return 0;
    lua_getfield(L, 1, "__wid");
    const auto id = static_cast<uint32_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    const std::string file = luaL_optstring(L, 2, "");
    if (file.empty()) impl->modelFrames.erase(id);
    else impl->modelFrames[id] = {file, static_cast<uint32_t>(luaL_optnumber(L, 3, 0)), static_cast<float>(luaL_optnumber(L, 4, 0)), static_cast<float>(luaL_optnumber(L, 5, 0.61))};
    if (auto* w = impl->engine.widgets().get(id)) w->hostModel = !file.empty();   // drawn though it has nothing else
    return 0;
}

int lua_PlayerModelReady(lua_State* L) {
    auto* impl=sessionImpl(L);
    lua_pushnumber(L,impl && impl->playerModel && impl->playerModel->ready() && impl->playerTexture ? 1 : 0);
    return 1;
}

// Native render fixtures provide the same appearance/displays used by the plugin's assembled look.
int lua_DressLook(lua_State* L) {
    auto* impl=sessionImpl(L);
    if (!impl) return 0;
    impl->dressWorn=lua_toboolean(L,1) ? impl->playerWorn : std::vector<Worn>{};
    if (!impl->dressModel) impl->dressModel=std::make_unique<PlayerModel>(*impl->mpq);
    impl->dressModel->setLook(dressCharacter(*impl->mpq,impl->playerAppearance,impl->dressWorn));
    impl->dressDirty=true;
    return 0;
}
int lua_DressTryOn(lua_State* L) {
    auto* impl=sessionImpl(L);
    if (!impl || !impl->dressModel) return 0;
    const auto slot=static_cast<uint32_t>(luaL_checkinteger(L,1));
    const auto display=static_cast<uint32_t>(luaL_checkinteger(L,2));
    const auto sheath=static_cast<uint32_t>(luaL_optinteger(L,3,0));
    if (slot>18 || !display) return 0;
    std::erase_if(impl->dressWorn,[&](const Worn& i) { return i.slot==slot || (lua_toboolean(L,4) && i.slot==16); });
    impl->dressWorn.push_back({slot,display,sheath});
    impl->dressModel->setLook(dressCharacter(*impl->mpq,impl->playerAppearance,impl->dressWorn));
    impl->dressDirty=true;
    lua_pushboolean(L,impl->dressModel->ready());
    return 1;
}

int lua_PlayerLook(lua_State* L) {
    auto* impl=sessionImpl(L);
    if (!impl || !lua_istable(L,1) || !lua_istable(L,2)) return 0;
    const auto field=[&](int index,const char* key,uint32_t fallback=0) {
        lua_getfield(L,index,key);
        const auto value=lua_isnumber(L,-1) ? static_cast<uint32_t>(lua_tonumber(L,-1)) : fallback;
        lua_pop(L,1); return value;
    };
    Appearance who{field(1,"race",4),field(1,"sex",1),field(1,"skin"),field(1,"face"),
        field(1,"hairStyle"),field(1,"hairColor"),field(1,"facialHair")};
    std::vector<Worn> worn;
    for (int i=1;;++i) {
        lua_rawgeti(L,2,i);
        if (!lua_istable(L,-1)) { lua_pop(L,1); break; }
        const int row=lua_gettop(L);
        worn.push_back({field(row,"slot"),field(row,"displayId"),field(row,"sheath")});
        lua_pop(L,1);
    }
    if (!impl->playerModel) impl->playerModel=std::make_unique<PlayerModel>(*impl->mpq);
    impl->playerAppearance=who; impl->playerWorn=worn;
    const auto look=dressCharacter(*impl->mpq,who,worn);
    impl->playerModel->setLook(look); impl->playerDirty=true;
    if (!impl->dressModel) impl->dressModel=std::make_unique<PlayerModel>(*impl->mpq);
    impl->dressWorn=worn; impl->dressModel->setLook(look); impl->dressDirty=true;
    return 0;
}

// __WBPortraitUnit(texture, unit): the texture shows that unit's portrait; the host fills each such one with the unit's
// live render every frame (her face; the target's).
int lua_PortraitUnit(lua_State* L) {
    auto* impl = sessionImpl(L);
    if (!impl || !lua_istable(L, 1)) return 0;
    lua_getfield(L, 1, "__wid");
    const auto id = static_cast<uint32_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    if (id) impl->engine.widgets().setPortraitUnit(id, luaL_checkstring(L, 2));
    return 0;
}

// __WBModelDuration(file, animation): how long that animation of the model runs (ms), or nil.
int lua_ModelDuration(lua_State* L) {
    auto* impl = sessionImpl(L);
    const Model* m = impl ? impl->uiModel(luaL_checkstring(L, 1)) : nullptr;
    const int seq = m ? findSequence(*m, static_cast<uint32_t>(luaL_optnumber(L, 2, 0))) : -1;
    if (seq < 0) return 0;
    lua_pushnumber(L, m->sequences[seq].duration);
    return 1;
}

int lua_IsAddOnLoaded(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowcore.addons");
    if (lua_isnil(L, -1)) return 1;
    lua_getfield(L, -1, luaL_checkstring(L, 1));
    if (!lua_toboolean(L, -1)) lua_pushnil(L); else lua_pushnumber(L, 1);
    return 1;
}
}  // namespace

namespace {
UiSession::Impl* sessionImpl(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kSessionKey);
    auto* s = static_cast<UiSession*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s ? s->impl() : nullptr;
}
// Screen anchors for host projections must be valid before the first layout, including after ReloadUI.
int lua_ViewportSize(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kSessionKey);
    auto* s=static_cast<UiSession*>(lua_touserdata(L,-1));
    lua_pop(L,1);
    if (!s) return 0;
    const float scale=s->height()/wowee::ui::WidgetTree::kInterfaceHeight*s->engine().widgets().userScale();
    lua_pushnumber(L,s->width()/scale);
    lua_pushnumber(L,s->height()/scale);
    return 2;
}
}  // namespace

UiSession::UiSession(std::shared_ptr<Mpq> mpq, const ClientDb& db, wowee::rendering::VkContext& textures, float width,
                     float height)
    : impl_(std::make_unique<Impl>(std::move(mpq), db, textures)), width_(width), height_(height) {}

UiSession::~UiSession() {
    wowee::ui::setHostModelHook({});
    impl_->engine.shutdown();
    if (impl_->imgui) ImGui::DestroyContext(impl_->imgui);
}

bool UiSession::start(std::string* error) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    impl_->imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(impl_->imgui);   // CreateContext leaves an existing context current (the host's own)
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.DisplaySize = ImVec2(width_, height_);
    impl_->addFonts();
    ImGui::SetCurrentContext(previous);

    wowee::ui::setInterfacePixelScale(height_ / 768.0f);   // known before the first layout: text measured at load
    impl_->renderer.initialize(&impl_->assets, &impl_->textures);
    if (!impl_->engine.initialize()) {
        if (error) *error = "Lua engine did not start";
        return false;
    }
    if (!installWowApi(impl_->engine, impl_->db, error)) return false;
    lua_State* L = impl_->engine.getState();
    lua_pushlightuserdata(L, this);
    lua_setfield(L, LUA_REGISTRYINDEX, kSessionKey);
    lua_register(L, "__WBViewportSize", lua_ViewportSize);
    lua_register(L, "LoadAddOn", lua_LoadAddOn);
    lua_register(L, "IsAddOnLoaded", lua_IsAddOnLoaded);
    lua_register(L, "__WBModelState", lua_ModelState);
    lua_register(L, "__WBPlayerLook", lua_PlayerLook);
    lua_register(L, "__WBDressLook", lua_DressLook);
    lua_register(L, "__WBDressTryOn", lua_DressTryOn);
    lua_register(L, "__WBPlayerModelReady", lua_PlayerModelReady);
    lua_register(L, "__WBModelDuration", lua_ModelDuration);
    lua_register(L, "__WBPortraitUnit", lua_PortraitUnit);
    wowee::ui::setHostModelHook([impl = impl_.get()](const wowee::ui::Widget& w, float x0, float y0, float x1, float y1,
                                                     std::vector<wowee::ui::HostModelBatch>& out) {
        impl->drawModel(w, x0, y0, x1, y1, out);
    });
    return true;
}

bool UiSession::loadAddOn(const std::string& name, std::string* error) {
    const std::string toc = "Interface\\AddOns\\" + name + "\\" + name + ".toc";
    if (!impl_->mpq->exists(toc)) { if (error) error->clear(); return false; }
    const FrameXmlReport r = loadFrameXml(impl_->engine, *impl_->mpq, toc);
    if (!r.failures.empty()) {
        if (error) *error = r.failures.front().first + ": " + r.failures.front().second;
        return false;
    }
    impl_->engine.fireEvent("ADDON_LOADED", {name});
    // Restore after the addon's defaults; its ADDON_LOADED handler can set anchors.
    lua_State* L = lua();
    lua_getglobal(L, "__WBRestoreLayout");
    if (lua_pcall(L, 0, 0, 0) != 0) {
        if (error) *error = lua_tostring(L, -1);
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool UiSession::loadInterface(std::string* error) {
    // Unit frames run OnLoad before their later siblings exist. Publish the
    // retained target after all FrameXML widgets are ready, as on world entry.
    lua_State* L = lua();
    lua_getglobal(L, "__WB");
    lua_getfield(L, -1, "tokens");
    lua_getfield(L, -1, "target");
    const int target = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushnil(L); lua_setfield(L, -2, "target");
    lua_pop(L, 2);
    const FrameXmlReport r = loadFrameXml(impl_->engine, *impl_->mpq);
    lua_getglobal(L, "__WB");
    lua_getfield(L, -1, "tokens");
    if (target == LUA_REFNIL) lua_pushnil(L);
    else lua_rawgeti(L, LUA_REGISTRYINDEX, target);
    lua_setfield(L, -2, "target");
    lua_pop(L, 2);
    luaL_unref(L, LUA_REGISTRYINDEX, target);
    if (!r.failures.empty()) {
        if (error) *error = r.failures.front().first + ": " + r.failures.front().second;
        return false;
    }
    impl_->engine.fireEvent("PLAYER_TARGET_CHANGED");
    return true;
}

void UiSession::setPlayerLook(const Look& look, const Appearance& appearance, const std::vector<Worn>& worn) {
    if (!impl_->playerModel) impl_->playerModel=std::make_unique<PlayerModel>(*impl_->mpq);
    impl_->playerModel->setLook(look); impl_->playerDirty=true;
    impl_->playerAppearance=appearance; impl_->playerWorn=worn; impl_->dressWorn=worn;
    if (!impl_->dressModel) impl_->dressModel=std::make_unique<PlayerModel>(*impl_->mpq);
    impl_->dressModel->setLook(dressCharacter(*impl_->mpq,appearance,worn)); impl_->dressDirty=true;
}

void UiSession::fire(const std::string& event, const std::vector<std::string>& args) { impl_->engine.fireEvent(event, args); }

void UiSession::setMinimap(const MinimapAtlas* atlas, const std::string& worldspace, MapPoint player, double heading) {
    impl_->minimapView = {player, 200, heading, false};
    impl_->minimapPose = !worldspace.empty() && minimapProject(impl_->minimapView, player).has_value();
    impl_->minimapBlips.clear();
    impl_->minimapPois.clear(); impl_->minimapNative=false;
    impl_->minimapLayers.clear();
    impl_->minimapTrackingMask = 0;
    impl_->minimapWorld = atlas ? atlas->world(worldspace) : nullptr;
    if (!impl_->minimapPose) impl_->minimapWorld = nullptr;
    if (impl_->minimapWorld) impl_->minimapLayers.push_back(impl_->minimapWorld);
    std::optional<MapPoint> position;
    lua_State* L = lua();
    const int top = lua_gettop(L);
    lua_getglobal(L, "__WB");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "minimap");
        if (impl_->minimapPose && lua_istable(L, -1)) {
            lua_getfield(L, -1, "parents");
            for (size_t i = 1; atlas && lua_istable(L, -1) && i <= lua_objlen(L, -1); ++i) {
                lua_rawgeti(L, -1, i);
                const auto* name = lua_tostring(L, -1);
                const auto* layer = name ? atlas->world(name) : nullptr;
                if (layer && std::find(impl_->minimapLayers.begin(), impl_->minimapLayers.end(), layer) == impl_->minimapLayers.end())
                    impl_->minimapLayers.push_back(layer);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            lua_getfield(L,-1,"compass"); impl_->minimapNative=lua_toboolean(L,-1); lua_pop(L,1);
            lua_getfield(L, -1, "pois");
            for (size_t i = 1; lua_istable(L, -1) && i <= lua_objlen(L, -1); ++i) {
                lua_rawgeti(L, -1, i);
                if (lua_istable(L, -1)) {
                    const auto number = [&](const char* key) {
                        lua_getfield(L, -1, key);
                        const double value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : std::numeric_limits<double>::quiet_NaN();
                        lua_pop(L, 1); return value;
                    };
                    const auto string = [&](const char* key) {
                        lua_getfield(L, -1, key);
                        const char* value = lua_tostring(L, -1);
                        const std::string s = value ? value : "";
                        lua_pop(L, 1); return s;
                    };
                    const MapPoint at{number("x"), number("y")};
                    auto name = string("name"), kind = string("kind");
                    const double icon = number("icon"), native=number("native"), opacity=number("opacity"), size=number("scale");
                    const bool nativeValid=std::isfinite(native) && native>=1 && native<=152 && std::floor(native)==native;
                    if (impl_->minimapNative && !nativeValid) impl_->minimapNative=false;
                    lua_getfield(L, -1, "discovered");
                    const bool discovered = lua_toboolean(L, -1); lua_pop(L, 1);
                    if (minimapProject(impl_->minimapView, at) && !name.empty() &&
                        (kind == "quest" || kind == "custom" || kind == "location" || (kind == "enemy" && nativeValid)))
                        impl_->minimapPois.push_back({at, std::move(name), std::move(kind),
                            std::isfinite(icon) && icon >= 0 && icon <= 42 ? static_cast<int>(icon) : 5, discovered,nativeValid ? static_cast<int>(native) : 0,
                            std::isfinite(opacity) ? std::clamp(opacity,0.0,1.0) : 1,std::isfinite(size) ? std::clamp(size,0.0,3.0) : 1});
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            lua_getfield(L, -1, "trackingMask");
            impl_->minimapTrackingMask = lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "blips");
            for (size_t i = 1; lua_istable(L, -1) && i <= lua_objlen(L, -1); ++i) {
                lua_rawgeti(L, -1, i);
                if (lua_istable(L, -1)) {
                    const auto number = [&](const char* key) {
                        lua_getfield(L, -1, key);
                        const double value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : std::numeric_limits<double>::quiet_NaN();
                        lua_pop(L, 1); return value;
                    };
                    const MapPoint at{number("x"), number("y")};
                    const double reaction = number("reaction");
                    lua_getfield(L, -1, "name");
                    const char* name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
                    if (minimapProject(impl_->minimapView, at) && reaction >= 1 && reaction <= 8)
                        impl_->minimapBlips.push_back({at, static_cast<int>(reaction), name ? name : ""});
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        // Normalize against stable combined bounds, never against whichever tile
        // happens to be underneath her (which would jump at the child's edge).
        size_t tiles = 0;
        const MinimapTile* under = nullptr;
        MapPoint low{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()}, high{-low.x, -low.y};
        for (const auto* layer : impl_->minimapLayers) {
            low = {std::min(low.x, layer->low.x), std::min(low.y, layer->low.y)};
            high = {std::max(high.x, layer->high.x), std::max(high.y, layer->high.y)};
            tiles += layer->around(impl_->minimapView).size();
            if (!under) {
                const auto* candidate = layer->at(player);
                if (candidate && impl_->mpq->exists(candidate->texture)) under = candidate;
            }
        }
        if (!impl_->minimapLayers.empty() && player.x >= low.x && player.y >= low.y && player.x < high.x && player.y < high.y)
            position = MapPoint{(player.x - low.x) / (high.x - low.x), (high.y - player.y) / (high.y - low.y)};
        lua_pushnumber(L, tiles);
        lua_setfield(L, -2, "minimapTiles");
        // The tour needs file existence as well as index coverage: a partial
        // package can have an index row without the actual BLP.
        lua_pushboolean(L, under && impl_->mpq->exists(under->texture));
        lua_setfield(L, -2, "minimapTileExists");
        if (under) lua_pushlstring(L, under->texture.data(), under->texture.size()); else lua_pushnil(L);
        lua_setfield(L, -2, "minimapTile");
        lua_getfield(L, -1, "units");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -2, "tokens");
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "player");
                lua_remove(L, -2); // tokens; retain the numeric or string unit key
                lua_gettable(L, -2);
                if (lua_istable(L, -1)) {
                    lua_pushnumber(L, position ? position->x : 0);
                    lua_setfield(L, -2, "mapX");
                    lua_pushnumber(L, position ? position->y : 0);
                    lua_setfield(L, -2, "mapY");
                }
            }
        }
    }
    lua_settop(L, top);
    if (auto* w = impl_->engine.widgets().findByName("Minimap"))
        w->hostModel = !impl_->minimapLayers.empty() || (impl_->minimapPose && (!impl_->minimapBlips.empty() || !impl_->minimapPois.empty()));
}

void UiSession::applyLiveMinimap(const MinimapAtlas* atlas) {
    auto* L = lua();
    const int top = lua_gettop(L);
    std::string world;
    MapPoint player;
    double heading = 0;
    lua_getglobal(L, "__WB");
    lua_getfield(L, -1, "minimap");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "worldspace");
        if (const auto* name = lua_tostring(L, -1)) world = name;
        lua_pop(L, 1);
        const auto number = [&](const char* key) {
            lua_getfield(L, -1, key);
            const double value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : std::numeric_limits<double>::quiet_NaN();
            lua_pop(L, 1); return value;
        };
        player = {number("x"), number("y")}; heading = number("heading");
    }
    lua_settop(L, top);
    setMinimap(atlas, world, player, heading);
    applyLiveWorldMap(atlas);
}

void UiSession::applyLiveWorldMap(const MinimapAtlas* atlas) {
    auto* L=lua();
    const int top=lua_gettop(L);
    impl_->worldMapLayers.clear();
    MapPoint low{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity()},high{-low.x,-low.y};
    lua_getglobal(L,"__WB"); lua_getfield(L,-1,"worldmap");
    if (lua_istable(L,-1)) {
        const auto number=[&](const char* key) {
            lua_getfield(L,-1,key);
            const double n=lua_isnumber(L,-1)?lua_tonumber(L,-1):std::numeric_limits<double>::quiet_NaN();
            lua_pop(L,1); return n;
        };
        impl_->worldMapPlayer={number("x"),number("y")}; impl_->worldMapHeading=number("heading");
        const auto add=[&] {
            const char* name=lua_tostring(L,-1);
            const auto* layer=name && atlas?atlas->world(name):nullptr;
            if (layer && std::find(impl_->worldMapLayers.begin(),impl_->worldMapLayers.end(),layer)==impl_->worldMapLayers.end()) {
                impl_->worldMapLayers.push_back(layer);
                low={std::min(low.x,layer->low.x),std::min(low.y,layer->low.y)};
                high={std::max(high.x,layer->high.x),std::max(high.y,layer->high.y)};
            }
        };
        if (std::isfinite(impl_->worldMapPlayer.x) && std::isfinite(impl_->worldMapPlayer.y) && std::isfinite(impl_->worldMapHeading)) {
            lua_getfield(L,-1,"worldspace"); add(); lua_pop(L,1);
            lua_getfield(L,-1,"parents");
            for (size_t i=1;lua_istable(L,-1) && i<=lua_objlen(L,-1);++i) {
                lua_rawgeti(L,-1,i); add(); lua_pop(L,1);
            }
            lua_pop(L,1);
        }
    }
    lua_settop(L,top);
    lua_getglobal(L,"__WBWorldMapAtlas");
    for (double n : {low.x,low.y,high.x,high.y}) {
        if (std::isfinite(n)) lua_pushnumber(L,n); else lua_pushnil(L);
    }
    if (lua_pcall(L,4,0,0)!=0) lua_pop(L,1);
    if (auto* w=impl_->engine.widgets().findByName("WorldMapDetailFrame")) w->hostModel=!impl_->worldMapLayers.empty();
    // A carriage session owns its terrain identity, independent of a player's
    // current WorldMap selection. Rendering walks copied data only.
    impl_->taxiMapLayers.clear();
    lua_getglobal(L,"__WB"); lua_getfield(L,-1,"taxi");
    if (lua_istable(L,-1)) {
        lua_getfield(L,-1,"worldspace");
        const char* name=lua_tostring(L,-1);
        if (const auto* layer=name && atlas ? atlas->world(name) : nullptr) impl_->taxiMapLayers.push_back(layer);
    }
    lua_settop(L,top);
    if (auto* w=impl_->engine.widgets().findByName("TaxiRouteMap")) w->hostModel=!impl_->taxiMapLayers.empty();
    lua_getglobal(L,"__WBTaxiTerrain"); lua_pushboolean(L,!impl_->taxiMapLayers.empty());
    if (lua_pcall(L,1,0,0)!=0) lua_pop(L,1);
}

void UiSession::pointer(float x, float y) { impl_->pointer = ImVec2(x, y); }

ImDrawData* UiSession::frame(float elapsed, const std::function<void(ImDrawList*)>& overlay) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(impl_->imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width_, height_);
    io.DeltaTime = elapsed > 0 ? elapsed : 1.0f / 60.0f;
    io.MousePos = impl_->pointer;   // GetCursorPosition reads it (the host does not drive ImGui's input)
    auto& tree = impl_->engine.widgets();
    impl_->engine.dispatchOnUpdate(elapsed);
    impl_->renderer.layout(tree, width_, height_);
    impl_->minimapTooltip(height_);
    ImGui::NewFrame();
    impl_->renderer.draw(tree, width_, height_);
    lua_getglobal(lua(),"__WB");
    if (lua_istable(lua(),-1)) { lua_pushboolean(lua(),impl_->minimapPose && impl_->minimapNative && impl_->renderer.artResident(Impl::compassAtlas)); lua_setfield(lua(),-2,"nativeCompassReady"); }
    lua_pop(lua(),1);
    if (overlay) overlay(ImGui::GetForegroundDrawList());
    ImGui::Render();
    impl_->engine.updateVisibility();     // after drawing, as WoWee's client does (application.cpp)
    impl_->engine.updateSizeChanges();
    impl_->engine.updateScrollRanges();
    ImDrawData* dd = ImGui::GetDrawData();
    Impl::tileTextures(dd);
    if (previous) ImGui::SetCurrentContext(previous);
    return dd;
}

lua_State* UiSession::lua() { return impl_->engine.getState(); }
ImGuiContext* UiSession::imguiContext() { return impl_->imgui; }
int UiSession::fontsLoaded() const { return impl_->fonts; }
wowee::addons::LuaEngine& UiSession::engine() { return impl_->engine; }
}  // namespace wowcore
