-- The tooltip audit (test_tooltip_audit.cpp, [tooltips][audit]): renders every druid spell rank, talent rank, book
-- entry and item the game can show in the real GameTooltip, with the service's answers (tooltip_audit.json, from
-- service/tests/test_tooltip_audit.py) supplied as the live game supplies them, and lists each defect found as
-- {class=, where=, text=}. The C++ side applies the allowlist and fails on the rest.
--   __tooltipAudit        the JSON fixture: casters (costs, spell_tooltips, resolve), talents, book
--   __tooltipAuditItems   imported gear and Armory items, as Bags.cpp / Armory.cpp hand them over

local WB = __WB
local issues = {}
local stats = { tooltips = 0, lines = 0, spells = 0, compared = 0, talents = 0, book = 0, items = 0, auras = 0 }
local function issue(class, where, text) tinsert(issues, { class = class, where = where, text = text or "" }) end

-- Widest a tooltip may be: 1.12 wraps every long line (descriptions, item spell lines, flavour text) at about 240
-- units, so only a long name or an unwrapped line could take a tooltip far past it.
local MAX_WIDTH = 330

-- Defects any one line of text can show by itself.
local function scanText(t, where)
  if type(t) ~= "string" then issue("nil-text", where, tostring(t)) return end
  if strfind(t, "$", 1, true) then issue("token", where, t) end
  if strfind(t, "%%[%-+#0]*%d*%.?%d*[dsfgixXcuq]") or strfind(t, "%%%%") then issue("format", where, t) end
  local opened, closed = 0, 0
  for _ in string.gmatch(t, "|c%x%x%x%x%x%x%x%x") do opened = opened + 1 end
  for _ in string.gmatch(t, "|r") do closed = closed + 1 end
  if opened > closed then issue("markup-unclosed", where, t) end
  local stray = string.gsub(string.gsub(string.gsub(t, "|c%x%x%x%x%x%x%x%x", ""), "|[rnHhTt|]", ""), "||", "")
  if strfind(stray, "|", 1, true) then issue("markup-stray", where, t) end
  -- Scaleform/HTML tags (the client's own "<Right Click to Open>" hints are prose, not tags).
  for name in string.gmatch(t, "<%s*/?%s*(%a+)[%s/>]") do
    local tag = string.lower(name)
    if tag == "img" or tag == "font" or tag == "br" or tag == "p" or tag == "b" or tag == "i" or tag == "u" or
       tag == "a" and name == "a" or tag == "span" or tag == "div" or tag == "li" or tag == "textformat" then
      issue("markup-html", where, t) break
    end
  end
  if strfind(t, "<mag>", 1, true) or strfind(t, "<dur>", 1, true) or strfind(t, "<area>", 1, true) or
     strfind(t, "[SURV=", 1, true) or strfind(t, "<global=", 1, true) then issue("markup-native", where, t) end
  if strfind(t, "&%a+;") or strfind(t, "&#%d+;") then issue("entity", where, t) end
  if strfind(t, "[%z\1-\9\11\12\14-\31]") or strfind(t, "\\n", 1, true) then issue("control", where, t) end
  local lower = string.lower(t)
  if strfind(lower, "%f[%a]nil%f[%A]") or strfind(lower, "%f[%a]nan%f[%A]") or strfind(lower, "%f[%a]inf%f[%A]") or
     strfind(lower, "#ind", 1, true) or strfind(lower, "#inf", 1, true) then issue("nil-nan", where, t) end
  -- Ranges whose low end is above the high end: "X to Y", "X - Y Damage", "X-Y damage".
  for a, b in string.gmatch(t, "(%d+%.?%d*) to (%d+%.?%d*)") do
    if tonumber(a) > tonumber(b) then issue("range-order", where, t) end
  end
  for a, b in string.gmatch(t, "(%d+%.?%d*) ?%- ?(%d+%.?%d*)") do
    if tonumber(a) > tonumber(b) then issue("range-order", where, t) end
  end
  -- The client shows armour and weapon damage in whole points.
  if strfind(t, "^%d+%.%d+ Armor$") or strfind(t, "^%d+%.%d+ Damage$") or strfind(t, "^%d+%.%d+ %- %d") then
    issue("fraction", where, t)
  end
  -- Negative or zero health amounts where the client's amounts are positive.
  for _, p in ipairs({ "%f[%w%-]%-%d+%.?%d* to ", " to %-%d", "for %-%d", "heals %-%d", "absorbs? %-%d",
                       "%f[%w%-]%-%d+%.?%d* [%a ]-[Dd]amage", "%f[%w%-]%-%d+%.?%d* Armor", "%f[%w%-]%-%d+ health" }) do
    if strfind(t, p) then issue("negative", where, t) break end
  end
  for _, p in ipairs({ "%f[%w%.]0 to 0%f[%D]", "%f[%w%.]0 %- 0 ", "%f[%w%.]0 [%a ]-[Dd]amage", "^0 Armor$",
                       "for 0%f[^%d%.]", "heals 0%f[^%d%.]", "absorbs? 0%f[^%d%.]", "%f[%w%.]0 health" }) do
    if strfind(t, p) then issue("zero", where, t) break end
  end
end

-- Every line of a shown tooltip: its own text, then empty, repeated or overflowing lines.
local PAD, GAP = 10, 40
local function scan(tip, where)
  stats.tooltips = stats.tooltips + 1
  local n = tip:NumLines()
  if n == 0 or not tip:IsShown() then issue("empty-tooltip", where, "") return end
  local previous
  local width = tip:GetWidth()
  for i = 1, n do
    local l = getglobal(tip:GetName() .. "TextLeft" .. i)
    local r = getglobal(tip:GetName() .. "TextRight" .. i)
    local left, right = l:GetText(), r:IsShown() and r:GetText() or nil
    local at = where .. " #" .. i
    stats.lines = stats.lines + 1
    scanText(left, at)
    if right then scanText(right, at .. "R") end
    if left == "" and (not right or right == "") then issue("empty-line", at, "") end
    local key = tostring(left) .. "\0" .. tostring(right)
    if key == previous and left ~= " " then issue("duplicate-line", at, left) end
    previous = key
    -- Each row inside the frame's insets; a double line's two sides apart.
    -- (a wrapped line's widest row, at the limit the tooltip broke it at)
    local used = l.__wbWrap and __WBTextExtent(l, 240) or __WBTextExtent(l, 0)
    if right then used = used + GAP + r:GetStringWidth() end
    if used > width - 2 * PAD + 1 then issue("overflow", at, string.format("%s (%.0f in %.0f)", left, used, width)) end
  end
  -- A name is never wrapped by the client (its first line may be as wide as it is); any other line this wide
  -- should have been a wrapped one.
  for i = 2, n do
    local l = getglobal(tip:GetName() .. "TextLeft" .. i)
    local r = getglobal(tip:GetName() .. "TextRight" .. i)
    local w = (l.__wbWrap and __WBTextExtent(l, 240) or __WBTextExtent(l, 0)) + (r:IsShown() and GAP + r:GetStringWidth() or 0)
    if w + 2 * PAD > MAX_WIDTH then issue("too-wide", where .. " #" .. i, string.format("%.0f units: %s", w, l:GetText())) end
  end
end

local function lines(tip)
  local out = {}
  for i = 1, tip:NumLines() do
    local r = getglobal(tip:GetName() .. "TextRight" .. i)
    tinsert(out, { left = getglobal(tip:GetName() .. "TextLeft" .. i):GetText(), right = r:IsShown() and r:GetText() or nil })
  end
  return out
end

------------------------------------------------------------------------------------------------- casters and spells
local fixture = __tooltipAudit
local p = WB.resolve("player")
local function useCaster(c, native, profiles)
  __WBPlayerLevel(c.level)
  p = WB.resolve("player")
  p.level = c.level
  p.skyrim = native and c.skyrim or nil
  WB.skyrimCosts = {}
  for id, cost in pairs(c.costs) do WB.skyrimCosts[tonumber(id)] = cost end
  __WBSpellTooltips(profiles == nil and c.spell_tooltips or profiles)
end

-- "lo to hi" or "v" from one token's text.
local function range(text)
  local a, b = string.match(text, "^(%-?%d+%.?%d*) to (%-?%d+%.?%d*)$")
  if a then return tonumber(a), tonumber(b) end
  local v = tonumber(text)
  return v, v
end

-- The tooltip's numbers against what the service deals, heals and charges for the same caster.
local function compare(id, info, r, where, tip)
  stats.compared = stats.compared + 1
  local function check(ok, what, shown)
    if not ok then issue("service-mismatch", where, what .. ": tooltip " .. shown .. ", service " .. r.__desc) end
  end
  if r.direct then
    local e = r.direct_effect
    local text = WB.spellText(info, "$s" .. e)
    local lo, hi = range(text)
    r.__desc = r.direct[1] .. " to " .. r.direct[2]
    check(lo and lo <= r.direct[1] and hi >= r.direct[2], "$s" .. e, text)
    -- The shown range should be the service's, not merely contain it (one point either side for unseen rolls).
    if lo and lo <= r.direct[1] and hi >= r.direct[2] then
      local slack = 1 + math.floor((hi - lo) / 10)
      if r.direct[1] - lo > slack or hi - r.direct[2] > slack then
        issue("service-loose", where, "$s" .. e .. ": tooltip " .. text .. ", service " .. r.__desc)
      end
    end
  end
  for e, v in pairs(r.periodic or {}) do
    local low, high, ticks = v[1], v[2], v[3]
    local total = WB.spellText(info, "$o" .. e)
    r.__desc = low .. (high ~= low and ("-" .. high) or "") .. " x " .. ticks
    check(tonumber(total) and tonumber(total) >= low * ticks and tonumber(total) <= high * ticks, "$o" .. e, total)
    local tick = WB.spellText(info, "$s" .. e)
    local lo, hi = range(tick)
    check(lo and lo <= low and hi >= high, "$s" .. e .. " per tick", tick)
  end
  for e, amount in pairs(r.shield or {}) do
    local text = WB.spellText(info, "$s" .. e)
    r.__desc = tostring(amount)
    check(tonumber(text) == amount, "$s" .. e .. " shield", text)
  end
  if r.cost and (info.powerType or 0) == 0 then
    local shown
    for _, line in ipairs(lines(tip)) do
      shown = shown or string.match(line.left or "", "^(%d+) Mana$")
    end
    r.__desc = tostring(r.cost)
    if r.cost > 0 then check(tonumber(shown) == r.cost, "cost", tostring(shown)) end
  end
end

local function spellIds(c)
  local ids = {}
  for id, profile in pairs(c.spell_tooltips) do
    if not profile.item_use then tinsert(ids, tonumber(id)) end
  end
  table.sort(ids)
  return ids
end

local function auditSpells(name, c)
  for _, native in ipairs({ true, false }) do
    useCaster(c, native)
    local mode = name .. (native and "" or " (WoW units)")
    for _, id in ipairs(spellIds(c)) do
      local info = WB.spellInfo(id)
      local where = mode .. " " .. (info and (info.name .. " " .. (info.rank or "")) or "?") .. " [" .. id .. "]"
      if not info then issue("missing-spell", where, "") else
        stats.spells = stats.spells + 1
        WB.actions[1] = { spell = id }
        GameTooltip:SetOwner(UIParent, "ANCHOR_NONE")
        GameTooltip:SetAction(1)
        scan(GameTooltip, where)
        local profile = WB.spellTooltips[id]
        if native then
          -- Their Skyrim skill: one grey line, on every spell the service names one for.
          if profile and profile.skill then
            local found
            for _, line in ipairs(lines(GameTooltip)) do
              if line.left == (({ OneHanded = "One-Handed", TwoHanded = "Two-Handed", Marksman = "Archery" })[profile.skill] or profile.skill) then found = true end
            end
            if not found and not info.passive then issue("missing-skyrim-line", where, "skill " .. profile.skill) end
          end
          local r = c.resolve[tostring(id)]
          if r then compare(id, info, r, where, GameTooltip) end
        end
        -- Its buff or debuff as the target or the player sees it, with the snapshot the cast carries.
        if info.auraDescription and info.auraDescription ~= "" then
          stats.auras = stats.auras + 1
          p.buffs = { { spell = id, tooltip = profile, count = 1, untilCancelled = true } }
          GameTooltip:SetOwner(UIParent, "ANCHOR_NONE")
          GameTooltip:SetPlayerBuff(0)
          scan(GameTooltip, where .. " aura")
          p.buffs = {}
        end
      end
    end
  end
  WB.actions[1] = nil
end

-- Without the service's profiles (before its first answer, or wherever none is sent) a spell must not show numbers
-- the service would not deal: those spells' rows are compared too.
local function auditFallback(name, c)
  useCaster(c, true, {})
  for _, id in ipairs(spellIds(c)) do
    local info = WB.spellInfo(id)
    local r = info and c.resolve[tostring(id)]
    if r then
      WB.actions[1] = { spell = id }
      GameTooltip:SetOwner(UIParent, "ANCHOR_NONE")
      GameTooltip:SetAction(1)
      local before = table.getn(issues)
      compare(id, info, r, name .. " without profile " .. info.name .. " " .. (info.rank or "") .. " [" .. id .. "]", GameTooltip)
      for i = before + 1, table.getn(issues) do issues[i].class = "fallback-" .. issues[i].class end
    end
  end
  WB.actions[1] = nil
end

------------------------------------------------------------------------------------------------- talents
local function auditTalents(name, c)
  useCaster(c, true)
  local reply = fixture.talents
  __WBTalents(reply)
  for tab, tree in ipairs(WB.talents.trees) do
    for index, t in ipairs(tree.talents) do
      for rank = 0, t.max_rank do
        t.rank = rank
        t.learnable = rank < t.max_rank
        local where = name .. " talent " .. t.name .. " " .. rank .. "/" .. t.max_rank
        stats.talents = stats.talents + 1
        GameTooltip:SetOwner(UIParent, "ANCHOR_NONE")
        GameTooltip:SetTalent(tab, index)
        scan(GameTooltip, where)
        -- The description of each rank shown is that rank's.
        local shown = lines(GameTooltip)
        local expect = WB.spellText(WB.spellInfo(t.rank_spells[math.max(1, rank)]), t.descriptions[math.max(1, rank)])
        local found
        for _, line in ipairs(shown) do if line.left == expect then found = true end end
        if not found then issue("talent-description", where, expect) end
      end
      t.rank = 0
    end
  end
end

------------------------------------------------------------------------------------------------- the book
-- As the live game does it: the costs answer first, then the book's own answer (with its profiles), then every
-- entry's tooltip. No profile the costs answer gave may be lost, and the book's numbers are still the service's.
local function auditBook(name, c)
  useCaster(c, true)
  local before = {}
  for id in pairs(WB.spellTooltips) do before[id] = true end
  local reply = {}
  for k, v in pairs(fixture.book) do reply[k] = v end
  reply.spell_tooltips = {}
  for _, id in ipairs(fixture.book.spell_tooltips) do reply.spell_tooltips[tostring(id)] = c.spell_tooltips[tostring(id)] end
  __WBClassSpells(reply)
  for id in pairs(before) do
    if not WB.spellTooltips[id] then
      local info = WB.spellInfo(id)
      issue("profile-dropped", name .. " book answer " .. (info and info.name or "?") .. " [" .. id .. "]", "")
    end
  end
  for tab = 1, GetNumSpellTabs() do
    local _, _, offset, count = GetSpellTabInfo(tab)
    for index = offset + 1, offset + count do
      stats.book = stats.book + 1
      local id = WB.bookSpell(index, "spell")
      local info = id and WB.spellInfo(id)
      local where = name .. " book " .. index .. " " .. (info and (info.name .. " " .. (info.rank or "")) or tostring(id))
      GameTooltip:SetOwner(UIParent, "ANCHOR_NONE")
      if GameTooltip:SetSpell(index, "spell") then
        scan(GameTooltip, where)
        local r = info and c.resolve[tostring(id)]
        if r then compare(id, info, r, where, GameTooltip) end
      else issue("empty-tooltip", where, "") end
    end
  end
end

------------------------------------------------------------------------------------------------- items
-- The bag tooltip of each item (the lines SetBagItem shows), in Skyrim and in WoW units.
local function auditItems(name, c, list, label, viaBag)
  for _, native in ipairs({ true, false }) do
    useCaster(c, native)
    -- Their swing with each weapon the player carries, as Cast.cpp hands it over (__WB.skyrimWeapons).
    WB.skyrimWeapons = {}
    for entry, w in pairs(native and c.weapons or {}) do WB.skyrimWeapons[tonumber(entry)] = { min = w[1], max = w[2] } end
    for _, item in ipairs(list) do
      stats.items = stats.items + 1
      local where = name .. (native and "" or " (WoW units)") .. " " .. label .. " " .. item.name .. " [" .. tostring(item.entry) .. "]"
      GameTooltip:SetOwner(UIParent, "ANCHOR_NONE")
      if viaBag then
        __WBInventory({ item }, 1)
        GameTooltip:SetBagItem(0, 1)
      else
        __WBBagPrepareItem(item)
        GameTooltip:ClearLines()
        for _, l in ipairs(WB.itemTooltip(item) or {}) do
          if l.right then GameTooltip:AddDoubleLine(l.left, l.right, l.r or 1, l.g or 1, l.b or 1, l.rr or 1, l.rg or 1, l.rb or 1)
          else GameTooltip:AddLine(l.left, l.r or 1, l.g or 1, l.b or 1, l.wrap) end
        end
        GameTooltip:Show()
      end
      scan(GameTooltip, where)
      if native then
        -- Skyrim's own numbers: its armour, and its damage for a weapon.
        local w, s = item.wow or {}, item.skyrim or {}
        local shown = lines(GameTooltip)
        local armor, damage
        for _, line in ipairs(shown) do
          armor = armor or string.match(line.left or "", "^(%d+) Armor$")
          damage = damage or (string.find(line.left or "", "^%d+ %- %d+ .*Damage$") and line.left)
        end
        if (w.armor or 0) > 0 and s.armor and s.armor > 0 and tonumber(armor) ~= math.floor(s.armor + 0.5) then
          issue("missing-skyrim-line", where, "armor " .. tostring(armor) .. " for Skyrim's " .. s.armor)
        end
        if w.class == 2 and (w.dmg_max1 or 0) > 0 and not (item.damage or s.damage) then
          issue("missing-skyrim-line", where, "no Skyrim damage for its WoW " .. (w.dmg_min1 or 0) .. " - " .. w.dmg_max1)
        end
        if w.class == 2 and (w.dmg_max1 or 0) > 0 and not damage then issue("missing-skyrim-line", where, "no damage line") end
        -- A weapon the player could swing reads as the service's swing deals it.
        local swing = WB.skyrimWeapons[item.entry]
        if swing then
          stats.swings = (stats.swings or 0) + 1
          local expect = string.format("%d - %d ", math.floor(swing.min + 0.5), math.floor(swing.max + 0.5))
          if not (damage and strfind(damage, expect, 1, true) == 1) then
            issue("service-mismatch", where, "swing " .. expect .. "but " .. tostring(damage))
          end
        end
      end
    end
  end
end

------------------------------------------------------------------------------------------------- every client spell
-- Every description and aura description in Spell.dbc, through the same formatter (its tokens and arithmetic).
local function auditClientSpells(name, c)
  useCaster(c, true, {})
  for id = 1, 30000 do
    local info = WB.spellInfo(id)
    if info then
      for _, field in ipairs({ "description", "auraDescription" }) do
        local text = info[field]
        if text and text ~= "" then
          stats.clientSpells = (stats.clientSpells or 0) + 1
          local out = WB.spellText(info, text)
          local where = "client " .. field .. " " .. info.name .. " " .. (info.rank or "") .. " [" .. id .. "]"
          if strfind(out, "$", 1, true) then issue("token", where, out) end
          if strfind(out, "%f[%a]nil%f[%A]") or strfind(string.lower(out), "%f[%a]nan%f[%A]") then issue("nil-nan", where, out) end
        end
      end
    end
  end
end

------------------------------------------------------------------------------------------------- Skyrim's own items
-- Skyrim items in their bags, as Bags.cpp Describe hands them over: GetDamage/GetArmorValue are floats (skill, perks,
-- tempering), effects already through NativeText::ItemDescription.
local NATIVE = {
  { key = "n1", form = 0x13989, name = "Steel Sword", count = 1, kind = "Weapon", weapon = 1, typeName = "Sword",
    slotName = "One-Hand", damage = 9.6000003814697, plainDamage = 8.6, baseDamage = 8, weight = 10, value = 45,
    temper = "Fine", temperFactor = 1.1, effects = {}, keywords = {} },
  { key = "n2", form = 0x13961, name = "Elven Gauntlets", count = 1, kind = "Armor", armorSlot = "hands", armorType = 0,
    typeName = "Light Armor", slotName = "Hands", armor = 9.3599996566772, baseArmor = 8, weight = 1, value = 110,
    effects = { "Increases your carrying capacity by 15 points." }, keywords = {} },
  { key = "n3", form = 0x3EADE, name = "Potion of Minor Healing", count = 3, kind = "Potion", potion = "health",
    typeName = "Potion", weight = 0.5, value = 17, effects = { "Restore 25 points of Health." }, keywords = {} },
  { key = "n4", form = 0x64B3F, name = "Sweet Roll", count = 2, kind = "Food", typeName = "Food", weight = 0.1,
    value = 2, effects = { "Restores 5 Health." }, keywords = { "VendorItemFood" } },
  { key = "n5", form = 0x1397D, name = "Iron Arrow", count = 24, kind = "Ammo", weight = 0, value = 1, effects = {},
    keywords = {} },
  { key = "n6", form = 0xF, name = "Gold", count = 120, kind = "Miscellaneous", weight = 0, value = 1, effects = {},
    keywords = {} },
}
local function auditNative(name, c)
  useCaster(c, true)
  for n, item in ipairs(NATIVE) do
    item.pos = 1
    stats.items = stats.items + 1
    __WBInventory({ item }, 1)
    GameTooltip:SetOwner(UIParent, "ANCHOR_NONE")
    GameTooltip:SetBagItem(0, 1)
    scan(GameTooltip, name .. " native " .. item.name)
  end
end

------------------------------------------------------------------------------------------------- run
function __WBTooltipAudit(parts)
  issues = {}
  local saved = WB.spellTooltips
  for _, name in ipairs({ "level60", "level20", "talented60" }) do
    local c = fixture.casters[name]
    if parts.spells then auditSpells(name, c) auditFallback(name, c) end
  end
  if parts.talents then auditTalents("level60", fixture.casters.level60) auditTalents("talented60", fixture.casters.talented60) end
  if parts.client then auditClientSpells("level60", fixture.casters.level60) end
  if parts.book then auditBook("level20", fixture.casters.level20) end
  if parts.native then auditNative("level60", fixture.casters.level60) end
  if parts.gear then auditItems("level60", fixture.casters.level60, __tooltipAuditItems.gear, "gear", true) end
  if parts.armory then auditItems("level60", fixture.casters.level60, __tooltipAuditItems.armory, "armory", false) end
  WB.spellTooltips = saved
  p.skyrim = nil
  return issues, stats
end
