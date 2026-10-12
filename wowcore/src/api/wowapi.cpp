#include "wowcore/wowapi.hpp"

#include "addons/lua_engine.hpp"
#include "addons/lua_api_helpers.hpp"
#include "core/app_clock.hpp"
#include "ui/interface_fonts.hpp"
#include "ui/text_markup.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/embedded.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cmath>
#include <string>
#include <vector>
#include <new>

namespace wowcore {
namespace {
const char* const kDbKey = "wowcore.ClientDb";
const char* const kHostKey = "wowcore.HostHandler";

// Tooltip layout and drawing must break exactly the same words. Font strings already hold
// display text (the Lua tooltip adapter retains the source text for GetText).
int lua_TextExtent(lua_State* L) {
    lua_getfield(L, 1, "__wid");
    const auto id = static_cast<uint32_t>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    auto* tree = wowee::addons::getWidgetTree(L);
    const auto* w = tree ? tree->get(id) : nullptr;
    if (!w) { lua_pushnumber(L, 0); lua_pushinteger(L, 1); return 2; }
    const auto measure = [&](const std::string& text) {
        return wowee::ui::interfaceTextWidth(text, w->fontFace, w->fontHeight);
    };
    const auto rows = wowee::ui::wrapText(wowee::ui::parseMarkup(w->text),
        static_cast<float>(luaL_optnumber(L, 2, 0)), w->nonSpaceWrap, measure);
    float widest = 0;
    for (const auto& row : rows) {
        std::string text;
        for (const auto& run : row) text += run.text;
        // Breaks retain the spaces preceding them for drawing, but the client sizes
        // a tooltip around the visible glyphs, without those trailing spaces.
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
        widest = std::max(widest, measure(text));
    }
    lua_pushnumber(L, widest);
    lua_pushinteger(L, std::max<size_t>(1, rows.size()));
    return 2;
}

const ClientDb* db(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kDbKey);
    auto* p = static_cast<const ClientDb*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return p;
}

// Seconds since the UI clock started, to the millisecond, as the client reports it.
int lua_GetTime(lua_State* L) {
    lua_pushnumber(L, std::floor(wowee::core::appTimeSeconds() * 1000.0 + 0.5) / 1000.0);
    return 1;
}

void field(lua_State* L, const char* k, const std::string& v) { lua_pushlstring(L, v.data(), v.size()); lua_setfield(L, -2, k); }
void field(lua_State* L, const char* k, double v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); }

// __WBSpellInfo(id) -> { name, rank, icon, passive, castTime, rangeMin, rangeMax, cost, powerType, duration,
//                        recovery, categoryRecovery, gcd, dispel, school, description, auraDescription } | nil
int lua_SpellInfo(lua_State* L) {
    const ClientDb* d = db(L);
    const SpellInfo* s = d ? d->spell(static_cast<uint32_t>(luaL_checknumber(L, 1))) : nullptr;
    if (!s) return 0;
    lua_createtable(L, 0, 17);
    field(L, "id", s->id);
    field(L, "name", s->name);
    field(L, "rank", s->rank);
    field(L, "icon", s->icon);
    lua_pushboolean(L, s->passive());
    lua_setfield(L, -2, "passive");
    field(L, "castTime", s->castTimeMs);
    field(L, "rangeMin", s->rangeMin);
    field(L, "rangeMax", s->rangeMax);
    field(L, "cost", s->manaCost);
    field(L, "powerType", s->powerType);
    field(L, "duration", s->durationMs);
    field(L, "recovery", s->recoveryMs);
    field(L, "categoryRecovery", s->categoryRecoveryMs);
    lua_pushboolean(L, s->startRecoveryMs > 0);
    lua_setfield(L, -2, "gcd");
    lua_pushboolean(L, s->targetsUnit);
    lua_setfield(L, -2, "targetsUnit");
    if (const std::string dispel = d->dispelName(s->dispelType); !dispel.empty()) field(L, "dispel", dispel);
    field(L, "school", s->school);
    field(L, "attributes", s->attributes);
    field(L, "attributesEx", s->attributesEx);
    field(L, "attributesExB", s->attributesExB);
    field(L, "spellLevel", s->spellLevel);
    field(L, "baseLevel", s->baseLevel);
    field(L, "maxLevel", s->maxLevel);
    field(L, "costPct", s->manaCostPct);
    field(L, "procCharges", s->procCharges);
    field(L, "procChance", s->procChance);
    field(L, "rangeIndex", s->rangeIndex);
    field(L, "stances", s->shapeshiftMask);   // the forms it is used in (bit form-1: 1 cat, 16 bear, 128 dire bear)
    field(L, "stancesNot", s->shapeshiftExclude);
    lua_createtable(L, 3, 0);
    for (int e = 0; e < 3; ++e) {
        const SpellEffect& f = s->effects[e];
        lua_createtable(L, 0, 10);
        field(L, "effect", f.effect);
        field(L, "aura", f.aura);
        field(L, "base", f.basePoints);
        field(L, "sides", f.dieSides);
        field(L, "dice", f.baseDice);
        field(L, "perLevel", f.pointsPerLevel);
        field(L, "period", f.periodMs);
        field(L, "radius", f.radius);
        field(L, "chain", f.chainTargets);
        field(L, "amplitude", f.chainAmplitude);
        field(L, "misc", f.misc);
        lua_rawseti(L, -2, e + 1);
    }
    lua_setfield(L, -2, "effects");
    lua_createtable(L, static_cast<int>(s->reagents.size()), 0);
    for (size_t k = 0; k < s->reagents.size(); ++k) {
        lua_createtable(L, 0, 2);
        field(L, "item", s->reagents[k].first);
        field(L, "count", s->reagents[k].second);
        lua_rawseti(L, -2, static_cast<int>(k) + 1);
    }
    lua_setfield(L, -2, "reagents");
    lua_createtable(L, 0, 0);   // the forms it needs, by name, when it cannot be cast without one
    if (s->shapeshiftMask && !(s->attributesExB & 0x80000)) {   // SPELL_ATTR_EX2_NOT_NEED_SHAPESHIFT
        int n = 0;
        for (uint32_t form = 1; form <= 32; ++form)
            if (s->shapeshiftMask & (1u << (form - 1))) {
                const std::string name = d->formName(form);
                lua_pushlstring(L, name.data(), name.size());
                lua_rawseti(L, -2, ++n);
            }
    }
    lua_setfield(L, -2, "requiresForms");
    field(L, "description", s->description);
    field(L, "auraDescription", s->auraDescription);
    return 1;
}

int lua_ItemIcon(lua_State* L) {
    const auto* d = db(L);
    const auto icon = d ? d->itemIcon(static_cast<uint32_t>(luaL_checknumber(L, 1))) : std::string();
    if (icon.empty()) return 0;
    lua_pushlstring(L, icon.data(), icon.size());
    return 1;
}

// __WBSpellLines(id) -> { { id, name, category, icon }, ... } (SkillLineAbility)
int lua_SpellLines(lua_State* L) {
    const ClientDb* d = db(L);
    lua_newtable(L);
    if (!d) return 1;
    int n = 0;
    for (uint32_t id : d->skillLinesOf(static_cast<uint32_t>(luaL_checknumber(L, 1)))) {
        const SkillLineInfo* l = d->skillLine(id);
        if (!l) continue;
        lua_createtable(L, 0, 4);
        field(L, "id", l->id);
        field(L, "name", l->name);
        field(L, "category", l->category);
        field(L, "icon", l->icon);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// __WBHost(cmd, ...): to the host's handler; without one, commands are kept in __WB.log for tests to read.
int lua_Host(lua_State* L) {
    const std::string cmd = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, kHostKey);
    auto* handler = static_cast<HostHandler*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (handler && *handler) return (*handler)(L, cmd);
    lua_getglobal(L, "__WB");
    lua_getfield(L, -1, "log");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "log");
    }
    const int top = lua_gettop(L), args = top - 2;
    std::string line = cmd;
    for (int i = 2; i <= args; ++i) {
        line += ' ';
        line += lua_isstring(L, i) ? lua_tostring(L, i) : luaL_typename(L, i);
    }
    lua_pushstring(L, line.c_str());
    lua_rawseti(L, -2, static_cast<int>(lua_objlen(L, -2)) + 1);
    lua_settop(L, args);
    return 0;
}
// __WBFire(event, ...): an event from Lua (the login burst, the host's Lua side), as the engine fires them.
int lua_Fire(lua_State* L) {
    auto* engine = static_cast<wowee::addons::LuaEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
    std::vector<std::string> args;
    for (int i = 2; i <= lua_gettop(L); ++i)
        args.push_back(lua_isnil(L, i) ? std::string(wowee::game::kEventNil) : std::string(luaL_checkstring(L, i)));
    engine->fireEvent(luaL_checkstring(L, 1), args);
    return 0;
}
}  // namespace

bool installWowApi(wowee::addons::LuaEngine& engine, const ClientDb& database, std::string* error) {
    lua_State* L = engine.getState();
    // WoWee's FrameXML bootstrap can install a missing-API __index on _G that
    // returns a callable table for undefined PascalCase names. The 1.12 client
    // needs honest nil globals (including cleared cast errors), just as our
    // API/HUD tools require. Apply this per state, also after ReloadUI, without
    // relying on the host process's WOWEE_LUA_API_FALLBACK environment setting.
    lua_pushnil(L);
    lua_setmetatable(L, LUA_GLOBALSINDEX);
    lua_pushcfunction(L, lua_TextExtent);
    lua_setglobal(L, "__WBTextExtent");
    lua_pushlightuserdata(L, const_cast<ClientDb*>(&database));
    lua_setfield(L, LUA_REGISTRYINDEX, kDbKey);
    lua_register(L, "GetTime", lua_GetTime);
    lua_register(L, "__WBSpellInfo", lua_SpellInfo);
    lua_register(L, "__WBItemIcon", lua_ItemIcon);
    lua_register(L, "__WBSpellLines", lua_SpellLines);
    lua_register(L, "__WBHost", lua_Host);
    lua_pushlightuserdata(L, &engine);
    lua_pushcclosure(L, lua_Fire, 1);
    lua_setglobal(L, "__WBFire");
    for (const EmbeddedFile* f = kWowApiLua; f->name; ++f) {
        const std::string chunk = std::string("@wowcore/lua/") + f->name;
        if (luaL_loadbuffer(L, f->text, f->size, chunk.c_str()) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
            if (error) *error = lua_tostring(L, -1);
            lua_pop(L, 1);
            return false;
        }
    }
    return true;
}

void setHostHandler(lua_State* L, HostHandler handler) {
    // A full userdata the registry holds, so the handler is destroyed with the state or when replaced.
    auto* slot = new (lua_newuserdata(L, sizeof(HostHandler))) HostHandler(std::move(handler));
    (void)slot;
    if (luaL_newmetatable(L, "wowcore.HostHandler.gc")) {
        lua_pushcfunction(L, [](lua_State* s) -> int {
            static_cast<HostHandler*>(lua_touserdata(s, 1))->~HostHandler();
            return 0;
        });
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, kHostKey);
}
}  // namespace wowcore
