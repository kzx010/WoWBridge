// hud_render OUT_DIR [--size WxH] [STATE ...]  -- the WoW HUD as wowcore draws it, one transparent 1280x720 PNG per HUD reference
// state (wowcore/tests/fixtures/hud_states.lua; default: all), for tools/hud_diff.py to compare with evidence/hudref.
// Each state gets a fresh session: idle fixture, FrameXML, the client's recorded login events (evidence/events) with
// frames between them, 30 s of quiet, then the state's own fixture, events, Lua and pointer, and its settle time.
#include "addons/lua_engine.hpp"
#include "core/app_clock.hpp"
#include "core/logger.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/event_log.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/minimap.hpp"
#include "wowcore/soft_raster.hpp"
#include "wowcore/ui_session.hpp"
#include "ui/widget_tree.hpp"

extern "C" {
#include <lauxlib.h>
}

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {
void step(wowcore::UiSession& ui, wowcore::SoftTextures& textures, double& t, double dt, ImDrawData** last = nullptr) {
    t += dt;
    wowee::core::setAppTimeSeconds(t);
    ImDrawData* dd = ui.frame(static_cast<float>(dt));
    textures.sync(dd);
    if (last) *last = dd;
}

int gWidth = 1280, gHeight = 720;   // --size WxH: other resolutions than the reference's

bool render(const std::string& root, const std::string& state, const std::string& out) {
    const char* data = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::string beside = root + "/../WoW/client/Data";
    const auto terrain = std::filesystem::path(root) / "build/skyrim_minimap";
    std::optional<wowcore::MinimapAtlas> atlas;
    std::ifstream index(terrain / "index.tsv");
    if (index) atlas = wowcore::MinimapAtlas::read(index);
    auto mpq = std::make_shared<wowcore::Mpq>(data ? data :
        (std::filesystem::exists(beside) ? beside : WOWBRIDGE_ROOT "/../WoW/client/Data"),
        std::filesystem::path(root) / "build/skyrim_icons",terrain);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, static_cast<float>(gWidth), static_cast<float>(gHeight));
    bool luaFailed = false;
    ui.engine().setLuaErrorCallback([&](const std::string& message) {
        luaFailed = true;
        std::fprintf(stderr, "%s Lua error: %s\n", state.c_str(), message.c_str());
    });
    std::string err;
    if (!ui.start(&err)) { std::fprintf(stderr, "start: %s\n", err.c_str()); return false; }
    lua_State* L = ui.lua();
    const std::string fixtures = root + "/wowcore/tests/fixtures/";
    if (luaL_dofile(L, (fixtures + "elunara.lua").c_str()) || luaL_dofile(L, (fixtures + "hud_states.lua").c_str()) ||
        luaL_dofile(L, (fixtures + "talents_render.lua").c_str()) ||
        luaL_dofile(L, (fixtures + "talent_feedback.lua").c_str()) ||
        luaL_dofile(L, (fixtures + "fidelity_states.lua").c_str()) ||
        luaL_dofile(L, (fixtures + "clientref_states.lua").c_str()) ||
        luaL_dofile(L, (fixtures + "font_preview.lua").c_str()) ||
        luaL_dostring(L, "__WBFixture('idle')")) {
        std::fprintf(stderr, "fixture: %s\n", lua_tostring(L, -1));
        return false;
    }
    lua_getglobal(L, "HUD_STATES");
    lua_getfield(L, -1, state.c_str());
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "user_scale");
        if (lua_isnumber(L, -1)) ui.engine().widgets().setUserScale(static_cast<float>(lua_tonumber(L, -1)));
    }
    lua_settop(L, 0);
    // Font/anchor queries during OnLoad must see this viewport's ruler.
    ui.engine().widgets().noteScreenSize(static_cast<float>(gWidth), static_cast<float>(gHeight));
    if (!ui.loadInterface(&err)) { std::fprintf(stderr, "interface: %s\n", err.c_str()); return false; }

    // The login, as the client raised it: everything up to a minute after PLAYER_ENTERING_WORLD.
    double t = 0, pew = -1;
    for (const auto& e : wowcore::loadEventLog(root + "/evidence/events/login_actions.lua")) {
        if (e.name == "ADDON_LOADED") continue;          // only WBOracle's own; WoWBridge loads no addons
        if (e.name == "TUTORIAL_TRIGGER" && !e.args.empty()) {   // read since this login: the server raises it no more
            lua_getglobal(L, "__WBTutorialFlagged");
            lua_pushstring(L, e.args[0].c_str());
            const bool read = lua_pcall(L, 1, 1, 0) == 0 && lua_toboolean(L, -1);
            lua_pop(L, 1);
            if (read) continue;
        }
        if (pew < 0 && e.name == "PLAYER_ENTERING_WORLD") pew = e.time;
        if (pew >= 0 && e.time > pew + 60) break;
        while (t < e.time) step(ui, textures, t, 0.05);   // 20 fps between events is enough for OnUpdate
        ui.fire(e.name, e.args);
    }
    for (int i = 0; i < 600; ++i) step(ui, textures, t, 0.05);   // the client had been up for minutes

    // The state itself.
    lua_getglobal(L, "HUD_STATES");
    lua_getfield(L, -1, state.c_str());
    if (!lua_istable(L, -1)) { std::fprintf(stderr, "no HUD state %s\n", state.c_str()); return false; }
    const int st = lua_gettop(L);
    lua_getfield(L, st, "fixture");
    const std::string fixture = luaL_optstring(L, -1, "idle");
    lua_pop(L, 1);
    if (luaL_dostring(L, ("__WBFixture('" + fixture + "')").c_str())) { std::fprintf(stderr, "%s\n", lua_tostring(L, -1)); return false; }
    // Read independent client snapshots only for explicit offline reference states.
    lua_getfield(L, st, "client_reference");
    if (const char* label = lua_tostring(L, -1)) {
        const std::string path = root + "/evidence/clientref-20261010/" + label + "/state.lua";
        if (luaL_dofile(L, path.c_str())) {
            std::fprintf(stderr, "client reference: %s\n", lua_tostring(L, -1)); return false;
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, st, "client_item_templates");
    if (const char* path = lua_tostring(L, -1)) {
        if (luaL_dofile(L, (root + "/evidence/" + path).c_str())) {
            std::fprintf(stderr, "client item templates: %s\n", lua_tostring(L, -1)); return false;
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, st, "run");
    if (lua_isstring(L, -1) && luaL_dostring(L, lua_tostring(L, -1))) {   // the chunk's error replaces nothing below
        std::fprintf(stderr, "run: %s\n", lua_tostring(L, -1));
        return false;
    }
    ui.applyLiveMinimap(atlas ? &*atlas : nullptr);
    lua_getfield(L, st, "after_live");
    if (const char* code = lua_tostring(L, -1); code && luaL_dostring(L, code)) {
        std::fprintf(stderr, "after live: %s\n", lua_tostring(L, -1)); return false;
    }
    lua_pop(L, 1);
    lua_settop(L, st);   // state changes first, then the events that announce them
    lua_getfield(L, st, "events");
    for (int i = 1; lua_istable(L, -1); ++i) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); break; }
        std::vector<std::string> args;
        for (int k = 2;; ++k) {
            lua_rawgeti(L, -1, k);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
            args.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_rawgeti(L, -1, 1);
        const std::string name = lua_tostring(L, -1);
        lua_pop(L, 2);
        ui.fire(name, args);
    }
    lua_pop(L, 1);

    // The pointer: one position { x, y }, or a path { { x, y }, ... } with a second of frames at each stop but the last.
    lua_getfield(L, st, "mouse");
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        const bool path = lua_istable(L, -1);
        lua_pop(L, 1);
        const int stops = path ? static_cast<int>(lua_objlen(L, -1)) : 1;
        for (int i = 1; i <= stops; ++i) {
            if (path) lua_rawgeti(L, -1, i);
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            const float k = gHeight / 720.0f;   // states give the pointer at the reference's 1280x720
            ui.engine().dispatchMouse(static_cast<float>(lua_tonumber(L, -2)) * k, static_cast<float>(lua_tonumber(L, -1)) * k,
                                      static_cast<float>(gHeight), {});
            ui.pointer(static_cast<float>(lua_tonumber(L, -2)) * k, static_cast<float>(lua_tonumber(L, -1)) * k);
            lua_pop(L, path ? 3 : 2);
            if (i < stops) for (int f = 0; f < 60; ++f) step(ui, textures, t, 1.0 / 60);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, st, "settle");
    const double settle = luaL_optnumber(L, -1, 1.0);
    lua_getfield(L, st, "print");   // a Lua expression shown after the settle (looking into the interface's state)
    const std::string print = luaL_optstring(L, -1, "");
    lua_settop(L, 0);

    ImDrawData* dd = nullptr;
    for (double s = 0; s < settle; s += 1.0 / 60) step(ui, textures, t, 1.0 / 60, &dd);
    if (!print.empty()) {
        const bool failed = luaL_dostring(L, ("return tostring(" + print + ")").c_str()) != 0;
        std::printf("%s: %s%s\n", state.c_str(), failed ? "error: " : "", lua_tostring(L, -1));
        lua_settop(L, 0);
        if (failed) return false;
    }
    // Our layout, in the client's terms (see /wbo frames): every visible named frame and region in UI units.
    {
        const auto& tree = ui.engine().widgets();
        std::FILE* f = std::fopen((out.substr(0, out.size() - 4) + "_frames.txt").c_str(), "w");
        for (uint32_t id = 1; f && id < tree.size(); ++id) {
            const wowee::ui::Widget* w = tree.get(id);
            if (!w || !w->visible || w->name.empty()) continue;
            const bool region = w->objectType == "Texture" || w->objectType == "FontString";
            std::string looks;
            if (w->objectType == "FontString") {
                std::string text = w->text;
                for (char& c : text) if (c == '|' || c == '\n') c = '~';
                looks = "|FontString|" + w->fontFace + "|" + std::to_string(w->fontHeight) + "||0|" + text;
            } else if (w->objectType == "Texture") {
                looks = "|Texture|" + w->texturePath;
            }
            std::fprintf(f, "%s|%s|%.2f|%.2f|%.2f|%.2f%s\n", region ? "R" : "F", w->name.c_str(), w->left, w->bottom,
                         w->left + w->rectW, w->bottom + w->rectH, looks.c_str());
        }
        if (f) std::fclose(f);
    }
    if (luaFailed) return false;
    wowcore::Image img(gWidth, gHeight);
    wowcore::rasterize(dd, textures, img);
    if (!wowcore::writePng(out, img)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return false; }
    std::printf("%s -> %s\n", state.c_str(), out.c_str());
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: hud_render OUT_DIR [STATE ...]\n"); return 1; }
    const std::string root = WOWBRIDGE_ROOT, dir = argv[1];
    setenv("WOWEE_LUA_API_FALLBACK", "0", 1);
    auto& sink = wowee::core::LogSink::get();
    sink.min = wowee::core::LogLevel::ERROR;
    sink.write = [](wowee::core::LogLevel, const std::string& m) { std::fprintf(stderr, "%s\n", m.c_str()); };
    std::vector<std::string> states;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &gWidth, &gHeight);
        else states.emplace_back(argv[i]);
    }
    if (states.empty())
        states = {"hud_idle", "hud_target_debuffs", "hud_casting", "hud_tooltip", "spellbook", "game_menu", "key_bindings"};
    bool ok = true;
    for (const auto& s : states) ok &= render(root, s, dir + "/" + s + ".png");
    return ok ? 0 : 1;
}
