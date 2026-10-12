// WoWBridge's WoW 1.12 game API (wowcore/lua) installed into a WoWee Lua engine, with the C functions it uses.
#pragma once
#include <functional>
#include <string>

struct lua_State;
namespace wowee::addons { class LuaEngine; }

namespace wowcore {
class ClientDb;

// Receives every command the interface issues (__WBHost(cmd, ...)). Called with the arguments on the Lua stack
// from index 2; returns the number of results it pushed.
using HostHandler = std::function<int(lua_State*, const std::string& command)>;

// Call after LuaEngine::initialize and before loading FrameXML; `db` must outlive the engine.
bool installWowApi(wowee::addons::LuaEngine& engine, const ClientDb& db, std::string* error = nullptr);
void setHostHandler(lua_State* L, HostHandler handler);
}  // namespace wowcore
