// WoWBridge's side of the seams WoWee's Lua engine calls out through: the game-facing WoW API registrations
// (lua_api_registrations.hpp), tooltip methods over game data, the clipboard, and the CVar store.
#include <string>   // lua_api_registrations.hpp uses std::string without including it

#include "addons/lua_api_registrations.hpp"
#include "addons/lua_api_helpers.hpp"

extern "C" {
#include <lua.h>
}

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {
std::string g_clipboard;
std::map<std::string, std::string> g_cvars;
}  // namespace

extern "C" {
char* SDL_GetClipboardText(void) { return strdup(g_clipboard.c_str()); }
bool SDL_SetClipboardText(const char* text) { g_clipboard = text ? text : ""; return true; }
void SDL_free(void* mem) { std::free(mem); }

// GameTooltip:SetAction/SetSpell/SetUnit/... over WoWBridge game state. Until a method is implemented it answers
// false, which FrameXML treats as "nothing to show".
int wowbridge_tooltip_method(lua_State* L, const char* /*method*/) { lua_pushboolean(L, 0); return 1; }
}

// WoWee's per-domain registrations stay empty: WoWBridge's API is Lua over its own state, installed after the engine
// by wowcore::installWowApi (wowcore/lua, src/api/wowapi.cpp).
namespace wowee::addons {
void registerUnitLuaAPI(lua_State*) {}
void registerSpellLuaAPI(lua_State*) {}
void registerInventoryLuaAPI(lua_State*) {}
void registerQuestLuaAPI(lua_State*) {}
void registerLfgLuaAPI(lua_State*) {}
void registerSocketLuaAPI(lua_State*) {}
void registerSocialLuaAPI(lua_State*) {}
void registerSystemLuaAPI(lua_State*) {}
void registerActionLuaAPI(lua_State*) {}
bool clientActsOnBinding(const std::string&) { return false; }
void setStoredCVar(const std::string& key, const std::string& value) { g_cvars[key] = value; }
void invalidateTradeSkillRows() {}
bool cursorItemCameFromInventory() { return false; }
}  // namespace wowee::addons
