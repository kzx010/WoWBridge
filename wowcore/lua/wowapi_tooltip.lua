-- WoWBridge: GameTooltip's 1.12 contents (SetSpell, SetAction, SetUnit, buffs, items), replacing WoWee's later-client
-- versions on the frame metatable. Lines, colours and wording follow the client's own tooltips as recorded in
-- evidence/apiref (tooltip_* states; /wbo tips). Spell text is Spell.dbc's description with its $-tokens resolved the
-- client's way; item lines come from the host (WB.equipment[slot].tooltip, WB.bags[bag][slot].tooltip), which builds
-- them from the item data it owns.

local WB = __WB
local MT = __WoweeFrameMT
-- The client's colours are bytes: its gold reads back as 210/255, its grey as 128/255.
local WHITE, GOLD, GRAY, RED = { 1, 1, 1 }, { 1, 210 / 255, 0 }, { 128 / 255, 128 / 255, 128 / 255 }, { 1, 32 / 255, 32 / 255 }
local POWER = { [0] = "Mana", [1] = "Rage", [2] = "Focus", [3] = "Energy" }

------------------------------------------------------------------------------------------------------- tooltip frame
-- 1.12's tooltip is a frame of numbered font strings (<name>TextLeftN / TextRightN, GameTooltipTemplate), which
-- FrameXML reads and recolours; Show() sizes it around them. WoWee draws its own list instead, so on tooltips these
-- methods are replaced and its list stays empty.
-- Insets; between lines; left to right text; the widest a wrapped row may be. WRAP: the client's row breaks in the
-- recorded tooltips need a limit of 220-229 px at 0.9375 (235-244 units); this is its middle. Sizes then match the
-- client's heights for most tooltips, and widths to within the +-2 px per string that WoW's own glyph advances
-- still differ from FreeType's (STATUS.md, M5).
local PAD, SPACING, GAP, WRAP = 10, 2, 40, 240
local base = { AddLine = MT.AddLine, AddDoubleLine = MT.AddDoubleLine, ClearLines = MT.ClearLines,
               NumLines = MT.NumLines, SetText = MT.SetText, Show = MT.Show, Hide = MT.Hide, SetOwner = MT.SetOwner }
local function isTip(f) return f.GetObjectType and f:GetObjectType() == "GameTooltip" end

-- 1.12's native GameTooltip timer holds for a second, then fades for a second.
-- Keep this separate from the tooltip's OnUpdate (addons may replace that script).
local fading = {}
local fadeDriver = CreateFrame("Frame")
fadeDriver:Hide()
local function cancelFade(tip)
  fading[tip] = nil
  tip:SetAlpha(1)
end
function MT:SetOwner(owner, anchor, ...)
  if isTip(self) then
    cancelFade(self)
    if anchor ~= "ANCHOR_PRESERVE" then self.__wbAnchor = anchor end
  end
  return base.SetOwner(self, owner, anchor, unpack(arg, 1, arg.n))
end
function MT:Hide()
  if isTip(self) then cancelFade(self) end
  return base.Hide(self)
end
function MT:FadeOut()
  if not isTip(self) or not self:IsShown() then return end
  -- Cursor-anchored tooltips are hidden immediately by the 1.12 client.
  if self.__wbAnchor == "ANCHOR_CURSOR" then self:Hide() return end
  fading[self] = 2
  fadeDriver:Show()
end
fadeDriver:SetScript("OnUpdate", function()
  for tip, remaining in pairs(fading) do
    remaining = remaining - arg1
    if not tip:IsShown() or remaining <= 0 then
      tip:Hide()
    else
      fading[tip] = remaining
      -- Native alpha is a byte; the client restores full alpha before hiding.
      tip:SetAlpha(math.floor(math.min(remaining, 1) * 255) / 255)
    end
  end
  if not next(fading) then fadeDriver:Hide() end
end)

local function text(tip, i, side)
  local name = tip:GetName() .. "Text" .. side .. i
  local fs = getglobal(name)
  if not fs then
    fs = tip:CreateFontString(name, "ARTWORK", i == 1 and "GameTooltipHeaderText" or "GameTooltipText")
    if side == "Left" then fs:SetPoint("TOPLEFT", getglobal(tip:GetName() .. "TextLeft" .. (i - 1)), "BOTTOMLEFT", 0, -SPACING)
    else fs:SetPoint("RIGHT", getglobal(tip:GetName() .. "TextLeft" .. i), "LEFT", GAP, 0) end
  end
  if not fs.__wbSetText then
    -- DBC paragraphs use CRLF. A carriage return is a break delimiter, not a font glyph.
    -- Keep GetText's original bytes (FrameXML and the reference probes read them back).
    fs.__wbSetText = fs.SetText
    fs.SetText = function(self, value)
      self.__wbText = value
      local shown = value and string.gsub(string.gsub(value, "\r\n", "\n"), "\r", "\n")
      self.__wbSetText(self, shown)
    end
    fs.GetText = function(self) return self.__wbText end
  end
  return fs
end

local function add(tip, left, right, lr, lg, lb, rr, rg, rb, wrap)
  local n = (tip.__wbLines or 0) + 1
  tip.__wbLines = n
  local l = text(tip, n, "Left")
  l:SetText(left)
  l:SetTextColor(lr or 1, lg or 210 / 255, lb or 0)
  l:Show()
  l.__wbWrap = wrap and wrap ~= 0
  local r = text(tip, n, "Right")
  if right then
    r:SetText(right)
    r:SetTextColor(rr or 1, rg or 210 / 255, rb or 0)
    r:Show()
  else
    r:Hide()
  end
end

function MT:AddLine(t, r, g, b, wrap)
  if not isTip(self) then return base.AddLine(self, t, r, g, b, wrap) end
  -- Stock 1.12 TaxiFrame (and other legacy callers) supplies an empty
  -- right-column placeholder before its RGB values.
  if r == "" then r, g, b, wrap = g, b, wrap, nil end
  add(self, t or "", nil, r, g, b, nil, nil, nil, wrap)
end
function MT:AddDoubleLine(left, right, lr, lg, lb, rr, rg, rb)
  if not isTip(self) then return base.AddDoubleLine(self, left, right, lr, lg, lb, rr, rg, rb) end
  add(self, left or "", right or "", lr, lg, lb, rr, rg, rb)
end
function MT:ClearLines()
  if not isTip(self) then return base.ClearLines(self) end
  for i = 1, self.__wbLines or 0 do
    text(self, i, "Left"):Hide()
    text(self, i, "Right"):Hide()
  end
  self.__wbLines = 0
end
function MT:NumLines()
  if not isTip(self) then return base.NumLines(self) end
  return self.__wbLines or 0
end
function MT:SetText(t, r, g, b, a, wrap)
  if not isTip(self) then return base.SetText(self, t, r, g, b, a, wrap) end
  self:ClearLines()
  add(self, t or "", nil, r, g, b, nil, nil, nil, wrap)
  self:Show()
end

-- Greedy word wrap, as the client breaks a wrapped line: rows no wider than `limit`; the widest row's width and
-- the number of rows (a CR LF in the text starts a new row).
local function wrapRows(fs, limit)
  -- Use the renderer's word breaks, including trailing spaces and inline markup.
  -- Counting with collapsed whitespace could reserve fewer rows than were drawn.
  return __WBTextExtent(fs, limit)
end

-- Sized around its lines: the widest unwrapped line (left, gap, right) or wrapped row, insets around, and the
-- client's extra EXTRA units on each axis (over 150 recorded tooltips both sizes come out exactly that much
-- larger than content plus insets).
local EXTRA = 15 / 32
function MT:Show()
  if isTip(self) then cancelFade(self) end
  if isTip(self) and (self.__wbLines or 0) > 0 then
    local limit = WRAP
    local width, rowsOf = 0, {}
    for i = 1, self.__wbLines do
      local l, r = text(self, i, "Left"), text(self, i, "Right")
      l:SetWidth(0)
      local w
      if l.__wbWrap then w, rowsOf[i] = wrapRows(l, limit)
      else w, rowsOf[i] = wrapRows(l, 0) end
      if r:IsShown() then w = w + GAP + r:GetStringWidth() end
      if w > width then width = w end
    end
    local height = 0
    for i = 1, self.__wbLines do
      local l, r = text(self, i, "Left"), text(self, i, "Right")

      if r:IsShown() then
        r:ClearAllPoints()
        r:SetPoint("RIGHT", self, "TOPRIGHT", -PAD, 0)
        r:SetPoint("TOP", l, "TOP", 0, 0)
      end
      local t = l:GetText() or ""
      l:SetWidth(0)
      l:SetText("A")
      local lineH = l:GetStringHeight()                     -- one row of this line's font, in whole pixels
      l:SetText(t)
      -- A line that fits on one row is not wrapped at all (a width of its own text would let rounding break it); a
      -- longer one breaks at the same limit its rows were counted at, so the renderer's breaks are ours.
      l:SetWidth((l.__wbWrap and rowsOf[i] > 1) and limit or 0)
      -- Reused font strings may still have the previous line's height. Give wrapped rows their full height so
      -- the renderer treats them as paragraphs rather than fixed-height labels to shorten with an ellipsis.
      l:SetHeight(rowsOf[i] * lineH)
      height = height + rowsOf[i] * lineH + (i > 1 and SPACING or 0)
    end
    self:SetWidth(math.max(width + 2 * PAD + EXTRA, self:GetMinimumWidth()))
    self:SetHeight(height + 2 * PAD + EXTRA)
  end
  return base.Show(self)
end

local function num(x) return (string.gsub(string.format("%.2f", x), "%.?0+$", "")) end   -- 2 -> "2", 1.5 -> "1.5"
local function span(ms)                                                     -- "15 sec", "10 min", "1 hour"
  if ms >= 3600000 then local h = ms / 3600000 return num(h) .. (h == 1 and " hour" or " hours") end
  if ms >= 60000 then return num(ms / 60000) .. " min" end
  return num(ms / 1000) .. " sec"
end

------------------------------------------------------------------------------------------------------- spell text
local function level() local p = WB.resolve("player") return p and p.level or 60 end
local function mods(info) return WB.spellMods and WB.spellMods[info.id] or {} end
local function profile(info) return WB.spellTooltips and WB.spellTooltips[info.id] or {} end
local function duration(info, values)
  values = values or profile(info)
  return values.duration_ms or (info.duration or 0) + (mods(info).duration or 0)
end

-- The client works these out in single precision: 1.8 per level over 5 levels is exactly 9 there, not 8.99999.
local function f32(x)
  if x == 0 then return 0 end
  local m, e = math.frexp(x)
  return math.ldexp(math.floor(m * 16777216 + 0.5) / 16777216, e)
end

-- An effect's value at the caster's level: lowest and highest roll, as the client prints them.
local function effectRange(info, e, values)
  values = values or profile(info)
  if values.effects and values.effects[e] then return unpack(values.effects[e]) end
  local f = info.effects[e]
  if not f then return 0, 0 end
  local lv = level()
  if info.maxLevel > 0 and lv > info.maxLevel then lv = info.maxLevel end
  lv = math.max(lv - info.spellLevel, 0)
  local v = f32(f.base + f32(f.perLevel * lv))
  local lo, hi = math.floor(f32(v + f.dice)), math.ceil(f32(v + math.max(f.sides, f.dice)))
  if lo > hi then lo, hi = hi, lo end
  return math.abs(lo), math.abs(hi)
end
local function ticks(info, e, values)
  local f = info.effects[e]
  if not f or f.period == 0 then return 0 end
  return math.floor(duration(info, values) / f.period)
end

-- Match service/skyrim.py: roots take priority over heals, then damage.
local function spellSkill(info)
  local heal, damage
  for _, f in ipairs(info.effects) do
    if f.effect and f.effect ~= 0 then
      if f.aura == 26 then return "Alteration" end
      if f.effect == 10 or f.aura == 8 then heal = true end
      if f.effect == 2 or f.aura == 3 then damage = true end
    end
  end
  return heal and "Restoration" or damage and "Destruction" or "Alteration"
end
local function healthScale(info, f, op, by, values)
  local p = WB.resolve("player")
  if not (p and p.skyrim and f.effect and f.effect ~= 0) then return nil end
  if f.effect ~= 2 and f.effect ~= 10 and f.aura ~= 3 and f.aura ~= 8 then return nil end
  values=values or profile(info)
  local factor=values.health_factor or (0.25 * (0.5 + (p.skyrim[spellSkill(info)] or 50) / 100))
  return function(amount)
    -- Token arithmetic is part of the WoW amount, before conversion.
    if op == "/" then amount = amount / tonumber(by)
    elseif op == "*" then amount = amount * tonumber(by) end
    local value = amount * factor
    local lower = math.floor(value)
    -- Python round: exactly halfway goes to the even integer.
    if value - lower > 0.5 or (value - lower == 0.5 and lower % 2 ~= 0) then lower=lower+1 end
    if values.minimum_health and factor>0 and amount>0 then lower=math.max(1,lower) end
    return lower
  end
end

-- One $-token's value as text, and the number it stands for (for $l plurals).
local function token(info, code, e, op, by, values)
  e = tonumber(e) or 1
  local f = info.effects[e] or {}
  local scale = healthScale(info, f, op, by, values)
  if code == "s" or code == "m" or code == "M" then
    local lo, hi = effectRange(info, e, values)
    if scale then lo, hi = scale(lo), scale(hi) end
    if code == "m" then return tostring(lo), lo, scale end
    if code == "M" then return tostring(hi), hi, scale end
    if lo == hi then return tostring(lo), lo, scale end
    return lo .. " to " .. hi, hi, scale
  elseif code == "o" then
    local lo = effectRange(info, e, values)
    local total = (scale and scale(lo) or lo) * ticks(info, e, values)   -- the service converts each tick
    return tostring(total), total, scale
  elseif code == "d" then
    return span(duration(info, values)), duration(info, values) / 1000
  elseif code == "t" then
    return num((f.period or 0) / 1000), (f.period or 0) / 1000
  elseif code == "a" then
    return num(f.radius or 0), f.radius or 0
  elseif code == "x" then
    return tostring(f.chain or 0), f.chain or 0
  elseif code == "f" then
    return num(f.amplitude or 1), f.amplitude or 1
  elseif code == "n" then
    return tostring(info.procCharges), info.procCharges
  elseif code == "h" then
    local chance = (values or profile(info)).proc_chance or info.procChance
    return tostring(chance), chance
  elseif code == "q" then
    return tostring(f.misc or 0), f.misc or 0
  elseif code == "r" then
    local range = (values or profile(info)).range_max or info.rangeMax or 0
    return num(range), range
  end
end

function WB.spellText(info, text, values)
  text = text or info.description or ""
  local out, last = {}, 0
  local i, n = 1, string.len(text)
  while i <= n do
    local c = string.sub(text, i, i)
    if c ~= "$" then
      table.insert(out, c)
      i = i + 1
    else
      local rest = string.sub(text, i + 1)
      -- $lsingular:plural;  and  $gmale:female;
      local _, stop, which, a, b = string.find(rest, "^([lLgG])([^:;]*):([^;]*);")
      if which then
        if which == "l" or which == "L" then table.insert(out, last == 1 and a or b)
        else table.insert(out, ((WB.resolve("player") or {}).sex == 3) and b or a) end
        i = i + 1 + stop
      else
        -- $/1000;s3  $*2;s1  $12345d  $s1
        local _, stop2, op, by, id, code, e = string.find(rest, "^([/*]?)(%d*);?(%d*)([sSmMoOdtaxfnhqr])(%d?)")
        local value, number, scaled
        if stop2 then
          local target = info
          if op == "" and by ~= "" then id = by end                 -- $12345s1: another spell's
          if id ~= "" then target = WB.spellInfo(tonumber(id)) or info end
          value, number, scaled = token(target, (code == "O" or code == "S") and string.lower(code) or code, e, op, by, target == info and values or nil)
          if value and op ~= "" and not scaled then
            number = op == "/" and number / tonumber(by) or number * tonumber(by)
            value = num(number)
          end
        end
        if value then
          table.insert(out, value)
          last = number or 0
          i = i + 1 + stop2
        else
          table.insert(out, "$")   -- not a token the 1.12 texts use here: kept as written
          i = i + 1
        end
      end
    end
  end
  return table.concat(out)   -- line breaks stay as Spell.dbc has them (CR LF), as the client keeps them
end

------------------------------------------------------------------------------------------------------- spell lines
local function cost(info)
  local p = WB.resolve("player")
  local amount = WB.spellCost(p, info)
  if amount <= 0 then return nil end
  return amount .. " " .. POWER[info.powerType or 0]
end
local function castText(info)
  local a, ex = info.attributes or 0, info.attributesEx or 0
  local function has(v, f) return math.floor(v / f) % 2 == 1 end
  if has(ex, 4) or has(ex, 64) then return "Channeled" end
  if has(a, 1024) then return "Next melee" end
  local cast = profile(info).cast_ms or (info.castTime or 0) + (mods(info).castTime or 0)
  if cast > 0 then return num(cast / 1000) .. " sec cast" end
  if has(a, 16) then return "Instant" end
  return "Instant cast"
end

-- Stat lines some spells show in place of the usual ones (their numbers are the host's: WB player stats).
local SPECIAL = {
  [6603] = function(t, p) t:AddLine(string.format("%.2f%% chance to crit", p and p.critChance or 0), 1, 1, 1) end,
  [81] = function(t, p) t:AddLine(string.format("%.2f%% chance to dodge", p and p.dodgeChance or 0), 1, 1, 1) end,
}

local function abilityTime(seconds)
  if seconds >= 86400 then local days = seconds / 86400 return num(days) .. (days == 1 and " day" or " days") end
  return span(seconds * 1000)
end

-- The rank sits on the right of the name for an action; the spellbook prints it under the name instead.
local function addSpell(t, id, showRank, shapeshift)
  local info = WB.spellInfo(id)
  if not info then return nil end
  local rank = info.rank or ""
  if shapeshift then
    t:AddDoubleLine(info.name, "Shapeshift", WHITE[1], WHITE[2], WHITE[3], GRAY[1], GRAY[2], GRAY[3])
  elseif showRank and strfind(rank, "^Rank ") then
    t:AddDoubleLine(info.name, rank, WHITE[1], WHITE[2], WHITE[3], GRAY[1], GRAY[2], GRAY[3])
  else
    t:AddLine(info.name, WHITE[1], WHITE[2], WHITE[3])
  end
  local special = SPECIAL[id]
  if special then
    special(t, WB.resolve("player"))
  elseif info.skyrimKind then
    if info.skyrimKind == "shout" then
      t:AddLine("Shout", 1, 1, 1)
      local words = {}
      for _, w in ipairs(info.words) do
        if w.learned then
          tinsert(words, w.name .. (w.unlocked and "" or " (locked)") .. " (" .. abilityTime(w.recovery) .. " recovery)")
        end
      end
      t:AddLine("Words learned: " .. table.concat(words, ", "), 1, 1, 1, 1)
      t:AddDoubleLine(castText(info), abilityTime(info.recovery / 1000) .. " recovery", 1, 1, 1, 1, 1, 1)
    else
      t:AddLine(info.skyrimKind == "greater" and "Greater Power - Once per Skyrim day" or "Lesser Power - At will", 1, 1, 1)
      local cd = info.cooldownSeconds
      if cd > 0 then t:AddDoubleLine(castText(info), abilityTime(cd) .. " cooldown", 1, 1, 1, 1, 1, 1)
      else t:AddLine(castText(info), 1, 1, 1) end
    end
    local start, seconds = WB.cooldown(id)
    local remaining = start + seconds - GetTime()
    if seconds > 0 and remaining > 0 then t:AddLine("Cooldown remaining: " .. abilityTime(remaining), RED[1], RED[2], RED[3]) end
  elseif not info.passive then
    local c = cost(info)
    local nextMelee = math.floor((info.attributes or 0) / 1024) % 2 == 1   -- on-next-swing: no range of its own
    local rangeMax = profile(info).range_max or info.rangeMax or 0
    local range = rangeMax > 0 and info.rangeIndex ~= 1 and not nextMelee and (num(rangeMax) .. " yd range") or nil
    if c and range then t:AddDoubleLine(c, range, 1, 1, 1, 1, 1, 1)
    elseif c or range then t:AddLine(c or range, 1, 1, 1) end
    local cd = profile(info).cooldown_ms or math.max(info.recovery or 0, info.categoryRecovery or 0)
    local cast = shapeshift and (info.castTime or 0) == 0 and "Instant cast" or castText(info)
    if cd > 0 then t:AddDoubleLine(cast, span(cd) .. " cooldown", 1, 1, 1, 1, 1, 1)
    else t:AddLine(cast, 1, 1, 1) end
  elseif WB.classRows and WB.classRows[id] then
    t:AddLine(PASSIVE or "Passive", GRAY[1], GRAY[2], GRAY[3])
  end
  if info.requiresForms and table.getn(info.requiresForms) > 0 and WB.spellFormError(info) == "SPELL_FAILED_ONLY_SHAPESHIFT" then
    t:AddLine("Requires " .. table.concat(info.requiresForms, ", "), RED[1], RED[2], RED[3])
  end
  -- Reagents, each one the player is short of in red (names and counts are the host's: WB.itemNames, WB.itemCounts).
  if info.reagents and table.getn(info.reagents) > 0 then
    local names = {}
    for _, r in ipairs(info.reagents) do
      local name = WB.itemNames and WB.itemNames[r.item] or ("item " .. r.item)
      if r.count > 1 then name = name .. " (" .. r.count .. ")" end
      if (WB.itemCounts and WB.itemCounts[r.item] or 0) < r.count then name = "|cffff2020" .. name .. "|r" end
      table.insert(names, name)
    end
    t:AddLine("Reagents: " .. table.concat(names, ", "), 1, 1, 1, 1)
  end
  local desc = WB.spellText(info)
  if desc ~= "" then t:AddLine(desc, GOLD[1], GOLD[2], GOLD[3], 1) end
  t:Show()
  return 1
end

------------------------------------------------------------------------------------------------------- methods
local function filled(t, ok) if not ok then t:Hide() end return ok end

function MT:SetSpell(index, bookType)
  self:ClearLines()
  local id = WB.bookSpell(index, bookType)
  local ok = addSpell(self, id)
  local pending = id and WB.unlearned and WB.unlearned[id]
  if ok and pending then
    self:AddLine("Learned at " .. pending.learn_skill .. " " .. math.ceil(pending.learn_skill_value), RED[1], RED[2], RED[3], 1)
    for _, text in ipairs(pending.learn_requirements or {}) do self:AddLine(text, RED[1], RED[2], RED[3], 1) end
    self:Show()
  end
  return filled(self, ok)
end
function MT:SetTalent(tab, index)
  self:ClearLines()
  local tree = WB.talents and WB.talents.trees[tab]
  local t = tree and tree.talents[index]
  if not t then return filled(self, nil) end
  self:AddLine(t.name, 1, 1, 1)
  self:AddLine(string.format(TOOLTIP_TALENT_RANK, t.rank, t.max_rank), 1, 1, 1)
  if t.rank < t.max_rank then
    if tree.points_spent < t.tier * 5 then
      self:AddLine(string.format(TOOLTIP_TALENT_TIER_POINTS, t.tier * 5, tree.name), RED[1], RED[2], RED[3], 1)
    end
    for _, p in ipairs(t.prerequisites or {}) do
      for _, parent in ipairs(tree.talents) do
        if parent.id == p.talent and parent.rank < p.rank then
          self:AddLine(string.format(p.rank == 1 and TOOLTIP_TALENT_PREREQ or TOOLTIP_TALENT_PREREQ_P1,
            p.rank, parent.name), RED[1], RED[2], RED[3], 1)
        end
      end
    end
    if t.required_spell and t.required_spell ~= 0 and not t.meets_prerequisites then
      local required = WB.spellInfo(t.required_spell)
      local known
      for _, id in ipairs(WB.spellbook) do if id == t.required_spell then known = true break end end
      if required and not known then
        self:AddLine("Requires " .. required.name, RED[1], RED[2], RED[3], 1)
      end
    end
  end
  local function description(rank)
    local info = WB.spellInfo(t.rank_spells[rank])
    if info then self:AddLine(WB.spellText(info, t.descriptions[rank]), GOLD[1], GOLD[2], GOLD[3], 1) end
  end
  description(math.max(1, t.rank))
  if t.rank > 0 and t.rank < t.max_rank then
    self:AddLine(" ")
    self:AddLine(TOOLTIP_TALENT_NEXT_RANK, 1, 1, 1)
    description(t.rank + 1)
  end
  if t.learnable then self:AddLine(TOOLTIP_TALENT_LEARN, 0, 1, 0) end
  self:Show()
  return 1
end
function MT:SetAction(slot)
  self:ClearLines()
  local a = type(slot) == "number" and WB.actions[slot]
  return filled(self, a and a.spell and addSpell(self, a.spell, true))
end
function MT:SetShapeshift(index)
  self:ClearLines()
  local f = WB.forms and WB.forms[index]
  return filled(self, f and addSpell(self, f.spell, false, true))
end

local function addLines(t, lines)
  if not lines then return nil end
  for _, l in ipairs(lines) do
    if l.right then t:AddDoubleLine(l.left, l.right, l.r or 1, l.g or 1, l.b or 1, l.rr or 1, l.rg or 1, l.rb or 1)
    else t:AddLine(l.left, l.r or 1, l.g or 1, l.b or 1, l.wrap) end
  end
  t:Show()
  return 1
end
function MT:SetInventoryItem(unit, slot)
  self:ClearLines()
  local e = UnitIsUnit(unit or "", "player") and WB.equipment and WB.equipment[slot]
  local lines=e and (WB.liveInventory and WB.itemTooltip and WB.itemTooltip(e) or e.tooltip)
  return filled(self, e and addLines(self, lines))
end
function MT:SetBagItem(bag, slot)
  self:ClearLines()
  local b = WB.bags and WB.bags[bag]
  local item = b and b[slot]
  local lines=item and (WB.liveInventory and WB.itemTooltip and WB.itemTooltip(item) or item.tooltip)
  return filled(self, item and addLines(self, lines))
end

-- "Level 60 Night Elf Druid (Player)" for players, "Level 60 Humanoid" for creatures.
function MT:SetUnit(unit)
  self:ClearLines()
  local u = WB.resolve(unit)
  if not u then return filled(self, nil) end
  self:AddLine(u.name, GOLD[1], GOLD[2], GOLD[3])
  local lvl = (u.level and u.level > 0) and u.level or "??"
  if u.isPlayer then
    self:AddLine(string.format("Level %s %s %s (Player)", lvl, u.race or "", u.class or ""), 1, 1, 1)
  else
    local kind = u.creatureType or ""
    if u.classification == "elite" or u.classification == "rareelite" then kind = "Elite " .. kind end
    self:AddLine(string.format("Level %s %s", lvl, kind), 1, 1, 1)
  end
  self:Show()
  return 1
end

local function auraTime(seconds)
  local unit, amount
  if seconds >= 86400 then unit, amount = "DAYS", math.ceil(seconds / 86400)
  elseif seconds >= 3600 then unit, amount = "HOURS", math.ceil(seconds / 3600)
  elseif seconds >= 60 then unit, amount = "MIN", math.ceil(seconds / 60)
  else unit, amount = "SEC", math.ceil(seconds) end
  local format = getglobal("SPELL_TIME_REMAINING_" .. unit .. (amount == 1 and "" or "_P1"))
  return string.format(format, amount)
end

local function addAura(t, a, playerBuff)
  if not a then return nil end
  local name, description
  if a.mount then
    name,description=a.name,"Increases ground movement speed by 100%. Right-click to dismount."
  elseif a.skyrim then   -- Skyrim's own magic: its name and words
    name, description = a.name, a.text
  else
    local info = a.spell and WB.spellInfo(a.spell)
    if not info then return nil end
    name, description = info.name, WB.spellText(info, info.auraDescription, a.tooltip or {})
  end
  if a.dispel and a.dispel ~= "" then
    t:AddDoubleLine(name, a.dispel, GOLD[1], GOLD[2], GOLD[3], GOLD[1], GOLD[2], GOLD[3])
  else t:AddLine(name, GOLD[1], GOLD[2], GOLD[3]) end
  if description and description ~= "" then t:AddLine(description, 1, 1, 1, 1) end
  -- Only the player's buff API exposes a duration in 1.12; unit aura tooltips have no time-left row.
  if playerBuff and a.expires and not a.untilCancelled then
    local seconds = math.max(a.expires - GetTime(), 0)
    if seconds > 0 then t:AddLine(auraTime(seconds), GOLD[1], GOLD[2], GOLD[3]) end
  end
  t:Show()
  return 1
end
function MT:SetPlayerBuff(id)
  self:ClearLines()
  local p = WB.resolve("player")
  local a
  if p and type(id) == "number" and id >= 0 then
    if id < 16 then a = p.buffs and p.buffs[id + 1]
    else a = p.debuffs and p.debuffs[id - 15] end
  end
  return filled(self, addAura(self, a, true))
end
function MT:SetTrackingSpell()
  self:ClearLines()
  return filled(self, addAura(self, WB.trackingAura and WB.trackingAura()))
end
function MT:SetUnitBuff(unit, index)
  self:ClearLines()
  local u = WB.resolve(unit)
  return filled(self, addAura(self, u and u.buffs and u.buffs[index]))
end
function MT:SetUnitDebuff(unit, index)
  self:ClearLines()
  local u = WB.resolve(unit)
  return filled(self, addAura(self, u and u.debuffs and u.debuffs[index]))
end
