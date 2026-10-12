// framexml_probe [--data <client>/Data] [--dump DIR] [--globals FILE]
//   Load the 1.12 interface headless and report what fails and which API is missing.
//   --dump DIR      also write the interface sources (FrameXML + Blizzard addons) from the archives to DIR
//   --globals FILE  also write the globals the engine defines before FrameXML loads (WoWee + WoWBridge API)
//   Without a fixture the API answers for an empty world (no player), which FrameXML must survive too.
#include "addons/lua_engine.hpp"
#include "core/logger.hpp"
#include "wowcore/framexml.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/wowapi.hpp"

extern "C" {
#include <lua.h>
}

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::string data = WOWBRIDGE_ROOT "/../WoW/client/Data", dump, globals;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        (a == "--data" ? data : a == "--dump" ? dump : a == "--globals" ? globals : data) = argv[i + 1];
    }
    auto& sink = wowee::core::LogSink::get();
    sink.min = wowee::core::LogLevel::WARNING;
    sink.write = [](wowee::core::LogLevel, const std::string& m) { std::fprintf(stderr, "%s\n", m.c_str()); };
    wowcore::Mpq mpq(data);

    if (!dump.empty()) {
        for (const char* mask : {"Interface\\FrameXML\\*", "Interface\\AddOns\\Blizzard_*"})
            for (const auto& name : mpq.list(mask)) {
                std::string rel = name;
                for (char& c : rel) if (c == '\\') c = '/';
                if (!(rel.ends_with(".lua") || rel.ends_with(".xml") || rel.ends_with(".toc"))) continue;
                const auto bytes = mpq.read(name);
                fs::create_directories((fs::path(dump) / rel).parent_path());
                std::ofstream(fs::path(dump) / rel, std::ios::binary).write(reinterpret_cast<const char*>(bytes->data()),
                                                                           static_cast<std::streamsize>(bytes->size()));
            }
    }

    const wowcore::ClientDb db(mpq);
    wowee::addons::LuaEngine engine;
    std::string err;
    if (!engine.initialize()) return 1;
    if (!globals.empty()) {   // WoWee's own globals, before WoWBridge's API is added
        lua_State* L = engine.getState();
        std::set<std::string> names;
        lua_pushnil(L);
        while (lua_next(L, LUA_GLOBALSINDEX)) {
            if (lua_type(L, -2) == LUA_TSTRING) names.insert(lua_tostring(L, -2));
            lua_pop(L, 1);
        }
        std::ofstream out(globals);
        for (const auto& n : names) out << n << '\n';
    }
    if (!wowcore::installWowApi(engine, db, &err)) {
        std::fprintf(stderr, "setup failed: %s\n", err.c_str());
        return 1;
    }
    const auto r = wowcore::loadFrameXml(engine, mpq);
    std::printf("FrameXML %d: %d Lua + %d XML files loaded, %zu failed, %.0f ms\n", r.interfaceVersion, r.luaFiles,
                r.xmlFiles, r.failures.size(), r.ms);
    for (const auto& [file, why] : r.failures) std::printf("  FAIL %s: %s\n", file.c_str(), why.c_str());
    engine.shutdown();   // lists every global FrameXML asked for that no one defined
    return r.failures.empty() ? 0 : 2;
}
