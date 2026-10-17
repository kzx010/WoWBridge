#include "wowcore/apiref.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace wowcore {
namespace {
const char* const kChecker = R"LUA(
local VOLATILE = { GetTime = 1, GetGameTime = 1, GetNetStats = 1, GetFramerate = 1, GetPlayerBuffTimeLeft = 1,
                   GetCursorPosition = 1, GetMacroIconInfo = 1, GetScreenWidth = 1, GetScreenHeight = 1,
                   GetMouseFocus = 1 }
local function ser(v)
  local t = type(v)
  if t == "string" then return string.format("%q", v) elseif t == "number" or t == "boolean" or t == "nil" then return tostring(v) end
  return t
end
local function answers(ok, ...)
  if not ok then return "error: " .. tostring((...)) end
  local out = {}
  for i = 1, select("#", ...) do out[i] = ser((select(i, ...))) end
  return table.concat(out, ", ")
end
-- Lua 5.0 quotes names in its messages as `name'; 5.1 as 'name'.
function __WBSame(...) return ... end
-- GameTooltip after one tooltip method, as WBOracle's tip() writes it: size, then each shown line with its colour.
local function tip(probe)
  GameTooltip:SetOwner(UIParent, "ANCHOR_NONE")
  local fn, err = loadstring("GameTooltip:" .. probe)
  local ok
  if fn then ok, err = pcall(fn) end
  local out = { string.format("n=%d w=%.1f h=%.1f", GameTooltip:NumLines(), GameTooltip:GetWidth(), GameTooltip:GetHeight()) }
  for i = 1, GameTooltip:NumLines() do
    for _, side in ipairs({ "Left", "Right" }) do
      local fs = getglobal("GameTooltipText" .. side .. i)
      if fs and fs:IsShown() and fs:GetText() then
        local r, g, b = fs:GetTextColor()
        table.insert(out, string.format("%s%d=%s|%.3f,%.3f,%.3f", strsub(side, 1, 1), i, fs:GetText(), r, g, b))
      end
    end
  end
  GameTooltip:Hide()
  if not ok then return "error: " .. tostring(err) end
  return table.concat(out, "\n")
end
-- Tooltip lines must match exactly; the size line is reported apart (layout, not content).
function __WBCheckTips(states)
  local out, total, same, sized = {}, 0, 0, 0
  for _, state in ipairs(states) do
    __WBFixture(state)
    for p, want in pairs(WBOracleAPI["tooltip_" .. state] or {}) do
      local got = tip(p)
      local wl, gl = string.gsub(want, "^[^\n]*\n?", ""), string.gsub(got, "^[^\n]*\n?", "")
      -- The client leaves out a reagent line until the reagent's name is in its item cache, which it was not yet
      -- for the first states recorded; WoWBridge always knows the names. Compared without ours then. (Item set
      -- pieces are the same; their lines come from the host, so a cold recording is compared with the warm one.)
      local _, _, cut = string.find(gl, "L(%d+)=Reagents: ")
      if cut and not string.find(wl, "Reagents: ", 1, true) then
        cut = tonumber(cut)
        gl = string.gsub(gl, "L%d+=Reagents: [^\n]*\n?", "")
        gl = string.gsub(gl, "([LR])(%d+)=", function(side, i)
          i = tonumber(i)
          return side .. (i > cut and i - 1 or i) .. "="
        end)
      end
      if string.find(p, "^SetInventoryItem") then
        for _, other in ipairs(states) do
          local o = WBOracleAPI["tooltip_" .. other] and WBOracleAPI["tooltip_" .. other][p]
          if o and string.len(o) > string.len(want) then want = o end
        end
        wl = string.gsub(want, "^[^\n]*\n?", "")
      end
      total = total + 1
      if wl == gl then same = same + 1 else table.insert(out, state .. "\t" .. p .. "\n\twant " .. want .. "\n\tgot  " .. got) end
      if string.match(want, "^[^\n]*") == string.match(got, "^[^\n]*") then sized = sized + 1
      else table.insert(out, "size\t" .. state .. "\t" .. p .. "\t" .. string.match(want, "^[^\n]*") .. "\t" .. string.match(got, "^[^\n]*")) end
    end
  end
  table.sort(out)
  return string.format("tooltips: %d / %d with the client's lines, %d / %d with its size", same, total, sized, total), table.concat(out, "\n"), total, same, sized
end
local function norm(s) return (string.gsub(s, "`", "'")) end
-- WoWBridge's deliberate departures from the client (the user's choices; wowapi.lua UNBOUND_BY_DEFAULT): probes about
-- these commands, and General carrying the combat log, answer otherwise on purpose and are not compared.
local DEPARTED = { "ATTACKTARGET", "TARGETPREVIOUSENEMY", "GetChatWindowMessages(1)" }
local function departed(p, want)
  -- The bridge adds three binding rows after the generated vanilla list. Loot tests
  -- check those rows; keep comparing every vanilla row and all other API answers here.
  if string.find(p, "^GetNumBindings%(") then return true end
  local row = tonumber(string.match(p, "^GetBinding%((%d+)%)$"))
  local vanillaRows = table.getn(__WB.defaultBindings)
  if row and row > vanillaRows and row <= vanillaRows + 3 then return true end
  for _, c in ipairs(DEPARTED) do
    if string.find(p, c, 1, true) or string.find(want, c, 1, true) then return true end
  end
end
function __WBCheck(states)
  local total, same, byFn, lines, out = 0, 0, {}, {}, {}
  local function print(s) table.insert(out, s) end
  for _, state in ipairs(states) do
    local probes = WBOracleAPI[state]
    __WBFixture(state)
    local n, ok = 0, 0
    for p, want in pairs(probes) do
      local fn = string.match(p, "^([%w_]+)")
      if not VOLATILE[fn] and not departed(p, want) then
        -- Called as an argument, not tail-called, so a usage error names this chunk the way the client's did.
        local f, err = loadstring("return __WBSame(" .. p .. ")", "return " .. p)
        local got = f and answers(pcall(f)) or ("error: " .. err)
        n = n + 1
        if norm(got) == norm(want) then ok = ok + 1
        else
          local e = byFn[fn] or { n = 0 }
          byFn[fn] = e
          e.n = e.n + 1
          if not e.p then e.p, e.want, e.got, e.state = p, want, got, state end
          table.insert(lines, state .. "\t" .. p .. "\n\twant " .. want .. "\n\tgot  " .. got)
        end
      end
    end
    print(string.format("%-7s %5d / %5d", state, ok, n))
    total, same = total + n, same + ok
  end
  local fns = {}
  for fn, e in pairs(byFn) do table.insert(fns, fn) end
  table.sort(fns, function(a, b) if byFn[a].n ~= byFn[b].n then return byFn[a].n > byFn[b].n end return a < b end)
  print(string.format("total   %5d / %5d match; %d functions differ", same, total, table.getn(fns)))
  for _, fn in ipairs(fns) do
    local e = byFn[fn]
    print(string.format("%4d %-30s %s@%s\n       want %s\n       got  %s", e.n, fn, e.p, e.state,
          string.sub(e.want, 1, 160), string.sub(e.got, 1, 160)))
  end
  table.sort(lines)
  return table.concat(out, "\n"), table.concat(lines, "\n"), total, same
end
)LUA";
}  // namespace

ApiRefResult checkApiReference(lua_State* L, const std::string& evidencePath, const std::vector<std::string>& states) {
    ApiRefResult r;
    if (luaL_dofile(L, evidencePath.c_str()) || luaL_dostring(L, kChecker)) {
        r.error = lua_tostring(L, -1);
        lua_pop(L, 1);
        return r;
    }
    lua_getglobal(L, "__WBCheck");
    lua_createtable(L, static_cast<int>(states.size()), 0);
    for (size_t i = 0; i < states.size(); ++i) {
        lua_pushstring(L, states[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    if (lua_pcall(L, 1, 4, 0)) {
        r.error = lua_tostring(L, -1);
        lua_pop(L, 1);
        return r;
    }
    r.summary = lua_tostring(L, -4);
    r.details = lua_tostring(L, -3);
    r.probes = static_cast<int>(lua_tonumber(L, -2));
    r.matches = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 4);
    return r;
}
TooltipRefResult checkTooltipReference(lua_State* L, const std::vector<std::string>& states) {
    TooltipRefResult r;
    lua_getglobal(L, "__WBCheckTips");
    lua_createtable(L, static_cast<int>(states.size()), 0);
    for (size_t i = 0; i < states.size(); ++i) {
        lua_pushstring(L, states[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    if (lua_pcall(L, 1, 5, 0)) {
        r.error = lua_tostring(L, -1);
        lua_pop(L, 1);
        return r;
    }
    r.summary = lua_tostring(L, -5);
    r.details = lua_tostring(L, -4);
    r.tooltips = static_cast<int>(lua_tonumber(L, -3));
    r.sameLines = static_cast<int>(lua_tonumber(L, -2));
    r.sameSize = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 5);
    return r;
}
}  // namespace wowcore
