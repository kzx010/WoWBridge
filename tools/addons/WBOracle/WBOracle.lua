-- WoWBridge reference recorder: combat-log chat events + probes into WBOracleDB (written on /reload or logout).
WBOracleDB = WBOracleDB or {}
local events = {
  "CHAT_MSG_SPELL_SELF_DAMAGE", "CHAT_MSG_SPELL_PERIODIC_CREATURE_DAMAGE", "CHAT_MSG_SPELL_SELF_BUFF",
  "CHAT_MSG_SPELL_PERIODIC_SELF_BUFFS", "CHAT_MSG_SPELL_FAILED_LOCALPLAYER", "CHAT_MSG_SPELL_AURA_GONE_OTHER",
  "CHAT_MSG_SPELL_PERIODIC_CREATURE_BUFFS", "CHAT_MSG_SPELL_BREAK_AURA", "UI_ERROR_MESSAGE",
}
local function log(kind, text)
  table.insert(WBOracleDB, string.format("%.3f|%s|%s", GetTime(), kind, tostring(text)))
end
local f = CreateFrame("Frame")
for _, ev in ipairs(events) do f:RegisterEvent(ev) end
f:SetScript("OnEvent", function() log(event, arg1) end)

-- API reference: every probe in WBORACLE_PROBES (tools/apiref.py) evaluated under pcall, answers serialized.
local function ser(v)
  local t = type(v)
  if t == "string" then return string.format("%q", v) elseif t == "number" or t == "boolean" or t == "nil" then return tostring(v) end
  return t
end
local function answers(ok, ...)
  if not ok then return "error: " .. tostring(arg[1]) end
  local out = {}
  for i = 1, arg.n do out[i] = ser(arg[i]) end
  return table.concat(out, ", ")
end
local function api(label)
  WBOracleAPI = WBOracleAPI or {}
  local res = {}
  for _, p in ipairs(WBORACLE_PROBES or {}) do
    local fn, err = loadstring("return " .. p)
    res[p] = fn and answers(pcall(fn)) or ("error: " .. err)
  end
  WBOracleAPI[label] = res
  return table.getn(WBORACLE_PROBES or {})
end

-- Event log, for replaying the client's own event sequence in the headless UI: every event FrameXML registers for
-- (WBORACLE_EVENTS), with arg1..arg9, from load until 60 s after entering the world, and while `/wbo events on`.
-- Kept in a local until our SavedVariables arrive (they would overwrite the global), then published.
local evlog, evStart, evUntil, evOn = {}, GetTime(), nil, false
local rec = CreateFrame("Frame")
for _, ev in ipairs(WBORACLE_EVENTS or {}) do rec:RegisterEvent(ev) end
rec:RegisterEvent("ADDON_LOADED")
rec:SetScript("OnEvent", function()
  if event == "ADDON_LOADED" and arg1 == "WBOracle" then WBOracleEvents = evlog end
  if event == "PLAYER_ENTERING_WORLD" and not evUntil then evUntil = GetTime() + 60 end
  if not evOn and evUntil and GetTime() > evUntil then return end
  local t, last = { string.format("%.3f", GetTime() - evStart), event }, 2
  local args = { arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9 }
  for i = 1, 9 do
    local a = args[i]
    t[i + 2] = type(a) == "string" and string.format("%q", a) or tostring(a)
    if a ~= nil then last = i + 2 end
  end
  table.insert(evlog, table.concat(t, "|", 1, last))
end)

-- Tooltip reference: GameTooltip filled by each WBORACLE_TIPS method call, read back line by line with colours.
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

-- Layout reference: every visible frame and region with its rectangle in UI units (left|bottom|right|top),
-- strata, level and alpha; unnamed ones as <parent>:<index><type>. The chat box typing the command is left out.
local function rectOf(o)
  local l, b, r, t = o:GetLeft(), o:GetBottom(), o:GetRight(), o:GetTop()
  if not l or not b or not r or not t then return nil end
  return string.format("%.2f|%.2f|%.2f|%.2f", l, b, r, t)
end
-- What a region shows: a font string's font file, height, flags, string width and text; a texture's file.
local function looks(rg)
  local kind = rg:GetObjectType()
  if kind == "FontString" then
    local font, height, flags = rg:GetFont()
    return string.format("%s|%s|%.2f|%s|%.2f|%s", kind, font or "", height or 0, flags or "", rg:GetStringWidth() or 0,
                         string.gsub(rg:GetText() or "", "[|\n]", "~"))
  elseif kind == "Texture" then
    return kind .. "|" .. (rg:GetTexture() or "")
  end
  return kind
end
local function frames(label)
  local out = { string.format("screen|%.2f|%.2f|%.4f", GetScreenWidth(), GetScreenHeight(), UIParent:GetEffectiveScale()) }
  local f = EnumerateFrames()
  while f do
    local name = f:GetName()
    if f:IsVisible() and name ~= "ChatFrameEditBox" then
      local r = rectOf(f)
      if r then
        table.insert(out, string.format("F|%s|%s|%s|%d|%.2f", name or "?", r, f:GetFrameStrata(), f:GetFrameLevel(), f:GetAlpha()))
      end
      local regions = { f:GetRegions() }
      for i, rg in ipairs(regions) do
        local rr = rg:IsVisible() and rectOf(rg)
        if rr then
          table.insert(out, string.format("R|%s|%s|%s", rg:GetName() or ((name or "?") .. ":" .. i .. rg:GetObjectType()), rr,
                                          looks(rg)))
        end
      end
    end
    f = EnumerateFrames(f)
  end
  WBOracleAPI = WBOracleAPI or {}
  WBOracleAPI["frames_" .. label] = out
  return table.getn(out)
end

local function say(msg) DEFAULT_CHAT_FRAME:AddMessage("WBO: " .. msg) end

local function stats()
  local s = ""
  for i = 1, 5 do local _, t = UnitStat("player", i) s = s .. t .. " " end
  s = s .. "mana=" .. UnitManaMax("player") .. " hp=" .. UnitHealthMax("player")
  for i = 1, 19 do
    local l = GetInventoryItemLink("player", i)
    if l then local _, _, id = string.find(l, "item:(%d+)") s = s .. " " .. i .. "=" .. id end
  end
  return s
end

SLASH_WBORACLE1 = "/wbo"
SlashCmdList["WBORACLE"] = function(msg)
  local _, _, cmd, rest = string.find(msg or "", "^(%S*)%s*(.-)$")
  if cmd == "clear" then
    WBOracleDB = {}
  elseif cmd == "target" then          -- /wbo target <name>
    TargetByName(rest, true)
    say("target=" .. (UnitName("target") or "none") .. " lvl=" .. (UnitLevel("target") or 0) ..
        " react=" .. (UnitReaction("player", "target") or -1) .. " attackable=" .. tostring(UnitCanAttack("player", "target")))
  elseif cmd == "shoot" then           -- /wbo shoot <spell> [shots] [seconds apart]: the interface hidden, the spell
    -- cast, the client's own screenshots from that moment (Screenshots\), then the interface back
    local _, _, spell, count, step = string.find(rest, "^(.-)%s+(%d+)%s+([%d%.]+)$")
    spell, count, step = spell or rest, tonumber(count) or 16, tonumber(step) or 0.15
    local f, t, n = CreateFrame("Frame"), 0, 0
    UIParent:Hide()
    f:SetScript("OnUpdate", function()
      t = t + arg1
      if t < 0.5 then return end
      if n == 0 and t - arg1 < 0.5 then CastSpellByName(spell) end
      if n < count and t >= 0.5 + n * step then Screenshot() n = n + 1 end
      if n >= count then UIParent:Show() this:SetScript("OnUpdate", nil) end
    end)
  elseif cmd == "stats" then
    local s = stats() log("STATS", s) say(s)
  elseif cmd == "bar" then            -- slice layout: slots 1-6 = highest ranks, 7-12 empty
    local slice = { "Wrath", "Moonfire", "Rejuvenation", "Healing Touch", "Entangling Roots", "Hurricane" }
    local best, i = {}, 1
    while GetSpellName(i, "spell") do best[GetSpellName(i, "spell")] = i i = i + 1 end
    for slot = 1, 12 do
      PickupAction(slot) ClearCursor()
      local idx = slice[slot] and best[slice[slot]]
      if idx then PickupSpell(idx, "spell") PlaceAction(slot) ClearCursor() end
    end
    say("bar set")
  elseif cmd == "api" then            -- /wbo api <state>: record every probe's answer under <state>
    say("api " .. rest .. ": " .. api(rest) .. " probes")
  elseif cmd == "events" then          -- /wbo events on|off|clear
    if rest == "clear" then evlog = {} WBOracleEvents = evlog evStart = GetTime() else evOn = rest == "on" end
    say("events " .. rest .. " (" .. table.getn(evlog) .. ")")
  elseif cmd == "tips" then            -- /wbo tips <state>: every WBORACLE_TIPS tooltip under "tooltip_<state>"
    WBOracleAPI = WBOracleAPI or {}
    local res = {}
    for _, p in ipairs(WBORACLE_TIPS or {}) do res[p] = tip(p) end
    WBOracleAPI["tooltip_" .. rest] = res
    say("tips " .. rest .. ": " .. table.getn(WBORACLE_TIPS or {}))
  elseif cmd == "frames" then          -- /wbo frames <state>: visible frame and region rectangles
    local n = frames(rest)
    say("frames " .. rest .. ": " .. n)
  elseif cmd == "apiclear" then
    WBOracleAPI = {}
  elseif cmd == "mark" then            -- /wbo mark <label>: separators between experiments
    log("MARK", rest) say("mark " .. rest)
  end
  say(table.getn(WBOracleDB) .. " events")
end
