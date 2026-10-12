// api_check [--data DIR] [--fixture FILE] [--evidence FILE] [--report FILE] [--eval LUA]
//   --eval: after loading, run LUA (an expression list) in the idle state, print its values and stop.
//   Load the 1.12 interface over WoWBridge's API, put the fixture into each recorded state and ask every probe the
//   real client answered (evidence/apiref): prints matches per state and the functions that differ, worst first.
#include "addons/lua_engine.hpp"
#include "core/logger.hpp"
#include "wowcore/apiref.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/framexml.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/wowapi.hpp"
#include "ui/interface_fonts.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
// Answers that depend on the moment or the machine, not on the state: never equal between two runs of the client.


bool run(lua_State* L, const std::string& code, const char* what) {
    if (luaL_loadbuffer(L, code.data(), code.size(), what) || lua_pcall(L, 0, 0, 0)) {
        std::fprintf(stderr, "%s: %s\n", what, lua_tostring(L, -1));
        return false;
    }
    return true;
}

bool runFile(lua_State* L, const std::string& path) {
    if (luaL_dofile(L, path.c_str())) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), lua_tostring(L, -1));
        return false;
    }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string root = WOWBRIDGE_ROOT;
    std::string data = root + "/../WoW/client/Data", fixture = root + "/wowcore/tests/fixtures/elunara.lua",
                evidence = root + "/evidence/apiref/WBOracle_api.lua", report = root + "/build/apiref/api_check.txt", eval;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        (a == "--data" ? data : a == "--fixture" ? fixture : a == "--evidence" ? evidence : a == "--eval" ? eval : report) = argv[i + 1];
    }
    setenv("WOWEE_LUA_API_FALLBACK", "0", 1);   // the 1.12 client has no fallback: a missing function is an error
    auto& sink = wowee::core::LogSink::get();
    sink.min = wowee::core::LogLevel::ERROR;
    sink.write = [](wowee::core::LogLevel, const std::string& m) { std::fprintf(stderr, "%s\n", m.c_str()); };

    wowcore::Mpq mpq(data);
    wowcore::ClientDb db(mpq);
    wowee::addons::LuaEngine engine;
    std::string err;
    if (!db.ok() || !engine.initialize() || !wowcore::installWowApi(engine, db, &err)) {
        std::fprintf(stderr, "setup failed: %s\n", err.c_str());
        return 1;
    }
    lua_State* L = engine.getState();
    wowee::ui::setInterfacePixelScale(720.0f / 768.0f);   // the reference client: 1280x720, default interface scale
    if (!runFile(L, fixture) || !run(L, "__WBFixture('idle')", "fixture")) return 1;
    const auto r = wowcore::loadFrameXml(engine, mpq);
    std::printf("FrameXML: %d Lua + %d XML loaded, %zu failed\n", r.luaFiles, r.xmlFiles, r.failures.size());
    for (const auto& [file, why] : r.failures) std::printf("  FAIL %s: %s\n", file.c_str(), why.substr(0, 300).c_str());
    if (!eval.empty()) {
        const int top = lua_gettop(L);
        if (luaL_dostring(L, ("return " + eval).c_str())) { std::printf("error: %s\n", lua_tostring(L, -1)); return 1; }
        for (int i = top + 1; i <= lua_gettop(L); ++i)
            std::printf("%s%s", i > top + 1 ? ", " : "", lua_isstring(L, i) ? lua_tostring(L, i) : luaL_typename(L, i));
        std::printf("\n");
        return 0;
    }
    const char* states[] = {"idle", "target", "self"};
    const auto res = wowcore::checkApiReference(L, evidence, {"idle", "target", "self"});
    if (!res.error.empty()) { std::fprintf(stderr, "check: %s\n", res.error.c_str()); return 1; }
    std::printf("%s\n", res.summary.c_str());
    const auto tips = wowcore::checkTooltipReference(L, {"idle", "target", "self"});
    std::printf("%s\n", tips.error.empty() ? tips.summary.c_str() : tips.error.c_str());
    if (FILE* f = std::fopen((report + ".tips").c_str(), "w")) { std::fputs(tips.details.c_str(), f); std::fclose(f); }
    if (FILE* f = std::fopen(report.c_str(), "w")) { std::fputs(res.details.c_str(), f); std::fclose(f); }
    engine.shutdown();
    return res.matches == res.probes && r.failures.empty() ? 0 : 2;
}
