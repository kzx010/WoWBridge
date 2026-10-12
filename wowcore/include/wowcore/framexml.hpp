// Loads the client's own interface (Interface\FrameXML\FrameXML.toc) from the MPQs into a WoWee LuaEngine, in
// manifest order, the way WoWee's AddonManager::loadFrameXml does from disk.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace wowee::addons { class LuaEngine; }

namespace wowcore {
class Mpq;

struct FrameXmlReport {
    int interfaceVersion = 0;
    int luaFiles = 0, xmlFiles = 0;
    std::vector<std::pair<std::string, std::string>> failures;   // file, reason
    double ms = 0;
};

FrameXmlReport loadFrameXml(wowee::addons::LuaEngine& engine, const Mpq& mpq,
                            const std::string& toc = "Interface\\FrameXML\\FrameXML.toc");
}  // namespace wowcore
