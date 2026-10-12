// WoWBridge compat for WoWee's addons/lua_api_helpers.hpp: only the game-independent helpers lua_engine.cpp uses,
// copied from WoWee (MIT, see third_party/wowee/LICENSE). Game-state helpers are replaced by WoWBridge's API shim.
#pragma once
#include "addons/lua_services.hpp"
#include "core/app_clock.hpp"
#include "core/logger.hpp"
#include "ui/widget_tree.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cctype>
#include <string>

namespace wowee::game {
inline constexpr const char* kEventNil = "\x01";   // an event argument that arrives as nil (handler_types.hpp)
}

namespace wowee::addons {

inline void toLowerInPlace(std::string& s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Real WoW's C functions convert what they can and treat the rest as unset.
inline double luaOptNumberText(lua_State* L, int idx, double def) {
    switch (lua_type(L, idx)) {
        case LUA_TNUMBER:  return lua_tonumber(L, idx);
        case LUA_TSTRING:  return lua_isnumber(L, idx) ? lua_tonumber(L, idx) : def;
        case LUA_TBOOLEAN: return lua_toboolean(L, idx) ? 1.0 : 0.0;
        default:           return def;
    }
}

inline constexpr const char* kLuaClasses[] = {"", "Warrior", "Paladin", "Hunter", "Rogue", "Priest",
                                              "Death Knight", "Shaman", "Mage", "Warlock", "", "Druid"};
inline constexpr const char* kLuaClassTokens[] = {"", "WARRIOR", "PALADIN", "HUNTER", "ROGUE", "PRIEST",
                                                  "DEATHKNIGHT", "SHAMAN", "MAGE", "WARLOCK", "", "DRUID"};
inline constexpr const char* kLuaRaces[] = {"", "Human", "Orc", "Dwarf", "Night Elf", "Undead",
                                            "Tauren", "Gnome", "Troll", "", "Blood Elf", "Draenei"};

// A class id's token, or nullptr: FrameXML guards these, and "" or "UNKNOWN" would pass the guard and break later.
inline const char* luaClassToken(unsigned classId) {
    if (classId >= sizeof(kLuaClassTokens) / sizeof(kLuaClassTokens[0])) return nullptr;
    const char* token = kLuaClassTokens[classId];
    return (token && *token) ? token : nullptr;
}
inline void luaPushClassToken(lua_State* L, unsigned classId) {
    if (const char* token = luaClassToken(classId)) lua_pushstring(L, token);
    else lua_pushnil(L);
}

struct CursorItemSlot { int bag = -1; int slot = 0; bool equipped = false; };

// "LeftButton"/"RightButton" for the click being dispatched, empty between clicks.
inline std::string& currentClickButton() { static std::string button; return button; }
inline CursorItemSlot& cursorItemSlot() { static CursorItemSlot held; return held; }

inline wowee::ui::WidgetTree* getWidgetTree(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_widget_tree");
    auto* t = static_cast<wowee::ui::WidgetTree*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return t;
}

inline LuaServices* getLuaServices(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_services");
    auto* svc = static_cast<LuaServices*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return svc;
}

// Game-side hooks the engine calls; WoWBridge defines them in its API shim.
void invalidateTradeSkillRows();
bool cursorItemCameFromInventory();

}  // namespace wowee::addons
