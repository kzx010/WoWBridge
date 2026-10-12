#include "wowcore/framexml.hpp"

#include "addons/lua_engine.hpp"
#include "addons/lua_handler_globals.hpp"
#include "core/logger.hpp"
#include "ui/framexml_emitter.hpp"
#include "ui/xml_parser.hpp"
#include "wowcore/mpq.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

namespace wowcore {
namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool endsWith(const std::string& s, const char* ext) { return lower(s).ends_with(ext); }
std::string dirOf(const std::string& p) {
    const auto at = p.find_last_of("\\/");
    return at == std::string::npos ? std::string() : p.substr(0, at);
}

class Loader {
public:
    Loader(wowee::addons::LuaEngine& e, const Mpq& m, std::string root) : engine_(e), mpq_(m), root_(std::move(root)) {}
    std::string error;

    bool lua(const std::string& path) {
        auto data = mpq_.read(path);
        if (!data) { error = "not in the archives"; return false; }
        if (!engine_.executeString(std::string(data->begin(), data->end()))) { error = engine_.lastError(); return false; }
        return true;
    }

    // Includes, then <Script file>, then the frames the XML declares (AddonManager::loadXmlFile order).
    bool xml(const std::string& path, int depth) {
        if (depth > 16) { error = "include nesting too deep"; return false; }
        auto data = mpq_.read(path);
        if (!data) { error = "not in the archives"; return false; }
        wowee::ui::XmlNode root;
        std::string perr;
        if (!wowee::ui::parseXml(std::string(data->begin(), data->end()), root, perr)) { error = "XML parse: " + perr; return false; }
        auto emitted = wowee::ui::emitFrameXml(root);
        // 1.12's default action template gates both drag handlers on LOCK_ACTIONBAR. Shift is the client's
        // temporary unlock; apply it to that template, including the bonus/multibar buttons inheriting it.
        if (lower(path).ends_with("\\actionbarframe.xml")) {
            const std::string gate = "LOCK_ACTIONBAR ~= \"1\"";
            size_t at = 0;
            while ((at = emitted.lua.find(gate, at)) != std::string::npos) {
                const std::string unlocked = "(LOCK_ACTIONBAR ~= \"1\" or IsShiftKeyDown())";
                emitted.lua.replace(at, gate.size(), unlocked);
                at += unlocked.size();
            }
        }
        for (const auto& w : emitted.warnings) LOG_WARNING("FrameXML ", path, ": ", w);
        bool ok = true;
        auto child = [&](const std::string& name) {
            std::string n = name;
            std::replace(n.begin(), n.end(), '/', '\\');
            const std::string beside = dirOf(path).empty() ? n : dirOf(path) + "\\" + n;
            return mpq_.exists(beside) ? beside : root_ + "\\" + n;
        };
        auto run = [&](const std::string& name, const char* what) {
            const std::string p = child(name);
            const bool loaded = endsWith(p, ".lua") ? lua(p) : xml(p, depth + 1);
            if (!loaded && ok) error = std::string(what) + " " + name + ": " + error;
            ok &= loaded;
        };
        for (const auto& inc : emitted.includeFiles) run(inc, "include");
        for (const auto& s : emitted.scriptFiles) run(s, "script");
        if (!emitted.lua.empty() && !engine_.executeString(emitted.lua)) {
            if (ok) error = "frames: " + engine_.lastError();
            ok = false;
        }
        return ok;
    }

private:
    wowee::addons::LuaEngine& engine_;
    const Mpq& mpq_;
    std::string root_;
};
}  // namespace

FrameXmlReport loadFrameXml(wowee::addons::LuaEngine& engine, const Mpq& mpq, const std::string& toc) {
    FrameXmlReport r;
    const auto start = std::chrono::steady_clock::now();
    auto data = mpq.read(toc);
    if (!data) { r.failures.emplace_back(toc, "manifest not in the archives"); return r; }
    std::vector<std::string> files;
    std::istringstream in(std::string(data->begin(), data->end()));
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        if (line.rfind("## Interface:", 0) == 0) r.interfaceVersion = std::atoi(line.c_str() + 13);
        else if (!line.empty() && line[0] != '#') files.push_back(line);
    }
    // Pre-3.0 interfaces read this/event/arg1..argN as globals in their handlers.
    wowee::addons::setLegacyHandlerGlobals(r.interfaceVersion == 0 || r.interfaceVersion < 30000);
    engine.executeString("__WoweeInterfaceVersion = " + std::to_string(r.interfaceVersion));
    engine.setChunkTimeoutMs(5000);

    const std::string root = dirOf(toc);
    Loader loader(engine, mpq, root);
    if (const std::string bindings = root + "\\Bindings.xml"; mpq.exists(bindings) && !loader.xml(bindings, 0))
        r.failures.emplace_back("Bindings.xml", loader.error);
    for (const auto& f : files) {
        const std::string path = root + "\\" + f;
        loader.error.clear();
        const bool isLua = endsWith(f, ".lua");
        if (isLua ? loader.lua(path) : loader.xml(path, 0)) (isLua ? r.luaFiles : r.xmlFiles)++;
        else r.failures.emplace_back(f, loader.error);
    }
    engine.setChunkTimeoutMs(0);
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return r;
}

}  // namespace wowcore
