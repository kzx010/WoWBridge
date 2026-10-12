-- WoWBridge: the WoW 1.12.1 game API, answered over WoWBridge state (the __WB table).
--
-- Every answer follows what the real 1.12.1 client returns, including its oddities (1/nil booleans, percent health
-- for units outside the party, a missing unit counting as the party leader). evidence/apiref holds the client's
-- recorded answers and wowcore's api check compares against them. The host (the SKSE plugin, or a test fixture)
-- writes __WB and fires the events; C++ supplies GetTime, client data lookups (__WBSpellInfo, __WBSpellLines) and
-- __WBHost, which receives every command the interface issues (casts, targeting, movement, bindings saved ...).

__WB = __WB or {}
local WB = __WB
WB.units = WB.units or {}            -- guid -> unit record (fields: see wowcore/tests/fixtures/elunara.lua)
WB.tokens = WB.tokens or {}          -- "player" | "target" | "mouseover" | "pet" | "party1" ... -> guid
WB.actions = WB.actions or {}        -- slot (1..120) -> { spell = id }
WB.spellbook = WB.spellbook or {}    -- known spell ids (the client sorts them into tabs)
WB.cooldowns = WB.cooldowns or {}    -- spell id -> { start, duration }
WB.gcd = WB.gcd or { 0, 0 }
WB.cvars = WB.cvars or {}
WB.bindings = WB.bindings or {}      -- { {command, key1, key2}, ... } in KeyBindingFrame order, headers included
WB.world = WB.world or {}            -- zone, subzone, minimapZone, pvpType, hour, minute, money, ...

local function host(cmd, ...)
  if __WBHost then return __WBHost(cmd, ...) end
end
WB.host = host

local function flag(v) if v then return 1 end return nil end
local function usage(text) error("Usage: " .. text, 3) end

------------------------------------------------------------------------------------------------------------ units
local function resolve(token)
  if type(token) ~= "string" then return nil end
  token = strlower(token)
  local hops = 0
  while strlen(token) > 6 and strsub(token, -6) == "target" do token = strsub(token, 1, -7); hops = hops + 1 end
  local guid = WB.tokens[token]
  for _ = 1, hops do
    local u = guid and WB.units[guid]
    guid = u and u.target
  end
  return guid and WB.units[guid], guid
end
WB.resolve = resolve

local UNIT_BASES = { player = 1, target = 1, pet = 1, mouseover = 1, npc = 1 }
local function validUnit(token)
  if type(token) ~= "string" then return false end
  token = strlower(token)
  while strlen(token) > 6 and strsub(token, -6) == "target" do token = strsub(token, 1, -7) end
  return UNIT_BASES[token] or strfind(token, "^party[1-4]$") or strfind(token, "^partypet[1-4]$")
         or strfind(token, "^raid%d+$") or strfind(token, "^raidpet%d+$")
end
local function checkUnit(unit, sig)
  if unit == nil then error("Usage: " .. sig, 3) end   -- level 3: the caller of the API function
  if not validUnit(unit) then error("Unknown unit name: " .. tostring(unit), 3) end
end

local function player() return WB.units[WB.tokens.player] end
local function inParty(u) return u and (u == player() or u.inParty) end

function UnitExists(unit) return flag(resolve(unit)) end
function UnitName(unit) local u = resolve(unit) return u and u.name, nil end
function UnitPVPName(unit) local u = resolve(unit) return u and (u.pvpName or u.name) or unit end
function UnitLevel(unit) local u = resolve(unit) return u and u.level or 0 end
function UnitSex(unit) local u = resolve(unit) return u and u.sex or 2 end
function UnitClass(unit) local u = resolve(unit) if not u then return nil, nil end return u.class, u.classToken end
function UnitRace(unit) local u = resolve(unit) if not u then return nil, nil end return u.race, u.raceToken end
function UnitFactionGroup(unit) local u = resolve(unit) return u and u.factionGroup, u and u.factionGroup end
function UnitClassification(unit) local u = resolve(unit) return u and u.classification or "normal" end
function UnitCreatureFamily(unit) local u = resolve(unit) return u and u.creatureFamily end
function UnitCreatureType(unit) local u = resolve(unit) return u and u.creatureType end
function UnitIsPlayer(unit) local u = resolve(unit) return flag(u and u.isPlayer) end
function UnitPlayerControlled(unit) local u = resolve(unit) return flag(u and (u.isPlayer or u.controlled)) end
function UnitIsConnected(unit) local u = resolve(unit) return flag(u and (u.connected ~= false)) end
function UnitIsVisible(unit) return flag(resolve(unit)) end
function UnitInParty(unit) return flag(inParty(resolve(unit))) end
function UnitPlayerOrPetInParty(unit) local u = resolve(unit) return flag(u and u.inParty) end
function UnitPlayerOrPetInRaid(unit) local u = resolve(unit) return flag(u and u.inRaid) end
function UnitInRaid(unit) local u = resolve(unit) return flag(u and u.inRaid) end
-- The client compares the party leader's guid with the unit's, and both are 0 when there is no party and no unit.
function UnitIsPartyLeader(unit) local _, guid = resolve(unit) return flag((WB.partyLeader or 0) == (guid or 0)) end
function UnitAffectingCombat(unit) local u = resolve(unit) return flag(u and u.combat) end
function UnitIsDead(unit) local u = resolve(unit) return flag(u and u.dead) end
function UnitIsGhost(unit) local u = resolve(unit) return flag(u and u.ghost) end
function UnitIsDeadOrGhost(unit) local u = resolve(unit) return flag(u and (u.dead or u.ghost)) end
function UnitIsCorpse(unit) local u = resolve(unit) return flag(u and u.corpse) end
function UnitIsTapped(unit) local u = resolve(unit) return flag(u and u.tapped) end
function UnitIsTappedByPlayer(unit) local u = resolve(unit) return flag(u and u.tappedByPlayer) end
function UnitIsPVP(unit) local u = resolve(unit) return flag(u and u.pvp) end
function UnitIsPVPFreeForAll(unit) local u = resolve(unit) return flag(u and u.pvpFreeForAll) end
function UnitIsCivilian(unit) local u = resolve(unit) return flag(u and u.civilian) end
function UnitIsCharmed(unit) local u = resolve(unit) return flag(u and u.charmed) end
function UnitIsTrivial(unit) local u = resolve(unit) return flag(u and u.trivial) end
function UnitOnTaxi(unit) return nil end
function UnitPVPRank(unit) local u = resolve(unit) return u and u.pvpRank or 0 end
function UnitHasRelicSlot(unit) local u = resolve(unit) return flag(u and u.relicSlot) end
-- A missing unit answers 1, like UnitIsPartyLeader: the client compares the icon holder's guid with 0.
function GetRaidTargetIndex(unit)
  checkUnit(unit, "GetRaidTargetIndex(unit)")
  local u = resolve(unit)
  if not u then return 1 end
  return u.raidTarget
end
function CanInspect(unit)
  checkUnit(unit, "CanInspect(unit)")
  local u = resolve(unit)
  return flag(u and u.isPlayer)
end
-- 1 Inspect, 2 Trade, 3 Duel, 4 Follow (yards).
local INTERACT = { 10, 11.11, 9.9, 28 }
function CheckInteractDistance(unit, index)
  if unit == nil or index == nil then usage('CheckInteractDistance("unit", distIndex)') end
  local u = resolve(unit)
  if not u then return nil end
  local d = u == player() and 0 or u.distance
  return flag(d and d <= (INTERACT[index] or 0))
end
function GetPlayerMapPosition(unit)
  local u = resolve(unit)
  if not u then return 0, 0 end
  return u.mapX or 0, u.mapY or 0
end
function UnitIsUnit(a, b) local ua, ub = resolve(a), resolve(b) return flag(ua and ua == ub) end

-- Health and power are exact for the player, the pet and the party; for anyone else 1.12 answers in percent.
local function percent(u) return not inParty(u) and u ~= WB.units[WB.tokens.pet] end
function UnitHealth(unit)
  local u = resolve(unit)
  if not u then return 0 end
  if percent(u) and (u.healthMax or 0) > 0 then return ceil(u.health * 100 / u.healthMax) end
  return u.health or 0
end
function UnitHealthMax(unit)
  local u = resolve(unit)
  if not u then return 0 end
  if percent(u) and (u.healthMax or 0) > 0 then return 100 end
  return u.healthMax or 0
end
function UnitMana(unit) local u = resolve(unit) return u and u.power or 0 end
function UnitManaMax(unit) local u = resolve(unit) return u and u.powerMax or 0 end
function UnitPowerType(unit) local u = resolve(unit) return u and u.powerType or 0 end
function UnitXP(unit) local u = resolve(unit) return u and u.xp or 0 end
function UnitXPMax(unit) local u = resolve(unit) return u and u.xpMax or 0 end
function GetXPExhaustion() local p = player() if p and not p.skyrimXP then return p.restXP end end
function GetRestState() local p = player() if p and not p.skyrimXP and p.rested then return 1, "Rested", 2 end return 2, "Normal", 1 end

-- Reaction of `u` toward the player (1 hated .. 8 exalted); the player toward itself is 5 (friendly).
local function reaction(a, b)
  if not a or not b then return nil end
  if a == b then return 5 end
  local other = a == player() and b or a
  return other.reaction or 5
end
function UnitReaction(a, b) return reaction(resolve(a), resolve(b)) end
function UnitIsFriend(a, b) local r = reaction(resolve(a), resolve(b)) return flag(r and r >= 5) end
function UnitIsEnemy(a, b) local r = reaction(resolve(a), resolve(b)) return flag(r and r <= 2) end
function UnitCanAttack(a, b)
  local ua, ub = resolve(a), resolve(b)
  if not ua or not ub or ua == ub then return nil end
  local other = ua == player() and ub or ua
  return flag(other.attackable)
end
function UnitCanCooperate(a, b)
  local ua, ub = resolve(a), resolve(b)
  return flag(ua and ub and ua.isPlayer and ub.isPlayer and reaction(ua, ub) >= 5)
end

-- Combat statistics: the host gives each unit's answers as tuples (the gameplay service computes the player's).
local function tuple(unit, key, absent, present)
  local u = resolve(unit)
  local t = u and u[key] or (u and present) or absent
  return unpack(t)
end
local Z5, Z7 = { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 }
function UnitArmor(unit) return tuple(unit, "armor", Z5, Z5) end
function UnitDamage(unit) return tuple(unit, "damage", Z7, { 0, 0, 0, 0, 0, 0, 1 }) end
function UnitRangedDamage(unit) return tuple(unit, "rangedDamage", { 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 1 }) end
function UnitAttackSpeed(unit) local u = resolve(unit) return u and u.attackSpeed or 0, u and u.offhandSpeed end
function UnitAttackPower(unit) return tuple(unit, "attackPower", { 0, 0, 0 }, { 0, 0, 0 }) end
function UnitRangedAttackPower(unit) return tuple(unit, "rangedAttackPower", { 0, 0, 0 }, { 0, 0, 0 }) end
function UnitRangedAttack(unit) return tuple(unit, "rangedAttack", { 0, 0 }, { 0, 0 }) end
function UnitAttackBothHands(unit) return tuple(unit, "attackBothHands", { 0, 0, 0, 0 }, { 0, 0, 0, 0 }) end
function UnitDefense(unit) return tuple(unit, "defense", { 0, 0 }, { 0, 0 }) end
function UnitCharacterPoints(unit) return tuple(unit, "characterPoints", { 0, 0 }, { 0, 0 }) end
function UnitStat(unit, index)
  if not index then usage('UnitStat("unit", statIndex)') end
  local u = resolve(unit)
  local s = u and u.stats and u.stats[index]
  if s then return unpack(s) end
  return 0, 0, 0, 0
end
function UnitResistance(unit, index)
  if not index then usage('UnitResistance("unit", resistanceIndex)') end
  local u = resolve(unit)
  local r = u and u.resistances and u.resistances[index]
  if r then return unpack(r) end
  return 0, 0, 0, 0
end

------------------------------------------------------------------------------------------------------------ auras
-- unit.buffs / unit.debuffs: { {spell, texture, count, dispel, expires, untilCancelled, castable}, ... }
-- `castable` on UnitBuff keeps buffs the player can cast; on UnitDebuff it keeps debuffs the player can dispel.
-- Names of the spells in the player's book: a buff is "castable" when the player can cast it, any rank.
local function knownNames()
  if WB.knownNames and not WB.spellbookDirty then return WB.knownNames end
  local names = {}
  for _, id in ipairs(WB.spellbook) do local i = WB.spellInfo(id) if i then names[i.name] = true end end
  WB.knownNames = names
  return names
end
local function aura(list, index, castable, dispels)
  if not list then return nil end
  local n = 0
  for _, a in ipairs(list) do
    local info = not dispels and castable and WB.spellInfo(a.spell)
    if not castable or (dispels and dispels[a.dispel or ""]) or (info and knownNames()[info.name]) then
      n = n + 1
      if n == index then return a end
    end
  end
end
function UnitBuff(unit, index, castable)
  local u = resolve(unit)
  local a = u and aura(u.buffs, index, castable)
  if a then return a.texture, a.count or 1 end
  return nil
end
function UnitDebuff(unit, index, castable)
  local u = resolve(unit)
  local p = player()
  local a = u and aura(u.debuffs, index, castable, castable and (p and p.dispels or {}))
  if a then return a.texture, a.count or 1, a.dispel end
  return nil
end

-- The player's own auras by buff index: helpful ones from 0, harmful ones from 16 (the client's aura slots).
local function playerAura(id)
  local p = player()
  if not p or type(id) ~= "number" then return nil end
  if id < 16 then return p.buffs and p.buffs[id + 1] end
  return p.debuffs and p.debuffs[id - 15]
end
function GetPlayerBuff(index, filter)
  filter = filter or "HELPFUL"
  local helpful = strfind(filter, "HELPFUL") ~= nil
  local p = player()
  local list = p and (helpful and p.buffs or p.debuffs) or {}
  local a = list[(index or 0) + 1]
  if not a then return -1, 0 end
  return index + (helpful and 0 or 16), a.untilCancelled and 1 or 0
end
function GetPlayerBuffTexture(id) local a = playerAura(id) return a and a.texture end
function GetPlayerBuffApplications(id) local a = playerAura(id) return a and (a.count or 1) or 0 end
function GetPlayerBuffDispelType(id) local a = playerAura(id) return a and a.dispel end
function GetPlayerBuffTimeLeft(id)
  local a = playerAura(id)
  if not a or not a.expires then return 0 end
  return max(a.expires - GetTime(), 0)
end
function CancelPlayerBuff(id)
  local a=playerAura(id)
  if a then
    if a.mount then Dismount()
    else host("cancelAura",a.spell or a.skyrim,a.skyrim and 1 or nil) end
  end
end
function GetWeaponEnchantInfo() return nil, nil, nil, nil, nil, nil end

------------------------------------------------------------------------------------------------------------ spells
local infoCache = {}
local function spellInfo(id)     -- name, rank, icon, passive, castTime, range, cost, ... (wowcore/src/api/wowapi.cpp)
  if type(id) == "number" and id < 0 then return WB.skyrimSpells and WB.skyrimSpells[id] end   -- a Skyrim spell of hers
  local i = infoCache[id]
  if i == nil then
    i = id and __WBSpellInfo and __WBSpellInfo(id) or false
    infoCache[id] = i
  end
  return i or nil
end
WB.spellInfo = spellInfo
local function hasBit(mask, bit) return math.floor((mask or 0) / bit) % 2 == 1 end
function WB.spellFormError(info, mask)
  if not info then return end
  local form = WB.forms and WB.forms[WB.formIndex or 0]
  local activeSpell = WB.activeFormSpell or (form and form.spell)
  mask = mask or (({[768]=1,[783]=4,[1066]=8,[5487]=16,[9634]=128,[24858]=1073741824})[activeSpell] or 0)
  if info.skyrim then return mask ~= 0 and "SPELL_FAILED_NOT_SHAPESHIFT" or nil end
  if info.id == 5487 or info.id == 9634 or info.id == 768 or info.id == 783 or info.id == 1066 or info.id == 24858 then return end
  if mask ~= 0 and hasBit(info.stancesNot, mask) then return "SPELL_FAILED_NOT_SHAPESHIFT" end
  if mask ~= 0 and hasBit(info.stances, mask) then return end
  if mask ~= 0 and hasBit(info.attributes, 65536) then return "SPELL_FAILED_NOT_SHAPESHIFT" end
  if (info.stances or 0) ~= 0 and (mask ~= 0 or not hasBit(info.attributesExB, 524288)) then return "SPELL_FAILED_ONLY_SHAPESHIFT" end
end
-- A convenience action: enter a learned Cat/Bear form, then use the ability after
-- the shift's GCD. The service still validates each cast against its real form.
-- Shared feral abilities prefer Cat when a new form is needed, otherwise the
-- strongest learned Bear. An already compatible form is always retained.
function WB.autoFormSpell(info, mask)
  if WB.spellFormError(info, mask) ~= "SPELL_FAILED_ONLY_SHAPESHIFT" then return end
  local stances = info.stances or 0
  if stances ~= 1 and stances ~= 16 and stances ~= 17 and stances ~= 128 and
     stances ~= 129 and stances ~= 144 and stances ~= 145 then return end
  local wanted
  for _, f in ipairs(WB.forms or {}) do
    if f.spell == 768 and hasBit(stances, 1) then wanted = 768 end
  end
  if not wanted then
    for _, f in ipairs(WB.forms or {}) do
      if f.spell == 9634 and hasBit(stances, 128) then wanted = 9634 end
    end
    if not wanted and hasBit(stances, 16) then wanted = 5487 end
  end
  for _, f in ipairs(WB.forms or {}) do
    if wanted and f.spell == wanted and not (WB.unlearned and WB.unlearned[wanted]) then return wanted end
  end
end
-- Attack and Shoot show the weapon they use.
local WEAPON_ICON = { [6603] = 16, [5019] = 18, [75] = 18 }
local function spellIcon(id)
  local slot = WEAPON_ICON[id]
  local e = slot and WB.equipment and WB.equipment[slot]
  if e and e.texture and e.texture ~= "" then return e.texture end
  -- Attack's DBC icon is Temp (a face). Use a glove for unarmed attacks;
  -- an equipped weapon continues to supply its own icon.
  if id == 6603 then return "Interface\\Icons\\INV_Gauntlets_04" end
  local i = spellInfo(id)
  return i and i.icon
end

-- The book as the client lays it out: General first, then each class skill line alphabetically, spells sorted by
-- name and rank inside a tab. Rebuilt when WB.spellbook changes (the host sets WB.spellbookDirty).
local function rankNumber(rank) local _, _, n = strfind(rank or "", "(%d+)") return tonumber(n) or 0 end
local function book()
  if WB.book and not WB.spellbookDirty then return WB.book end
  WB.spellbookDirty = nil
  local general, byLine, lineNames, skyrim = {}, {}, {}, {}
  for _, tab in ipairs(WB.classTabs or {}) do
    byLine[tab.name] = { icon = tab.icon, tab = tab.tab }
    tinsert(lineNames, tab.name)
  end
  local ids = {}
  for _, id in ipairs(WB.spellbook) do tinsert(ids, id) end
  for _, row in ipairs(WB.unlearnedSpells or {}) do tinsert(ids, row.spell) end
  for _, id in ipairs(ids) do
    local line
    for _, l in ipairs(id > 0 and __WBSpellLines and __WBSpellLines(id) or {}) do
      if l.category == 7 then line = l end         -- SkillLine category 7: class skills, one tab each
    end
    local row = WB.classRows and WB.classRows[id]
    if row then line = row.tab > 0 and WB.classLines[tostring(row.skill_line)] or nil end
    if id < 0 then
      tinsert(skyrim, id)
    elseif line then
      if not byLine[line.name] then byLine[line.name] = { icon = line.icon }; tinsert(lineNames, line.name) end
      tinsert(byLine[line.name], id)
    else
      tinsert(general, id)
    end
  end
  local function order(a, b)
    local ua, ub = WB.unlearned and WB.unlearned[a], WB.unlearned and WB.unlearned[b]
    if not ua ~= not ub then return not ua end  -- every learned rank precedes every unlearned rank
    local ia, ib = spellInfo(a), spellInfo(b)
    if ia.name ~= ib.name then return ia.name < ib.name end
    local ra, rb = rankNumber(ia.rank), rankNumber(ib.rank)
    if ra ~= rb then return ra < rb end
    return a < b
  end
  table.sort(lineNames, function(a, b)
    local ta, tb = byLine[a].tab, byLine[b].tab
    if ta and tb and ta ~= tb then return ta < tb end
    return a < b
  end)
  local tabs, slots = {}, {}
  local function add(name, icon, ids)
    table.sort(ids, order)
    tinsert(tabs, { name = name, icon = icon, offset = table.getn(slots), count = table.getn(ids) })
    for _, id in ipairs(ids) do tinsert(slots, id) end
  end
  add(GENERAL or "General", "Interface\\Icons\\Ability_Kick", general)
  for _, name in ipairs(lineNames) do add(name, byLine[name].icon, byLine[name]) end
  -- The Skyrim spells she has learned (tomes, trainers): a tab of their own after her class's (SkyrimSpells).
  if table.getn(skyrim) > 0 then add("Skyrim", "Interface\\Icons\\Spell_Arcane_StarFire", skyrim) end
  WB.book = { tabs = tabs, slots = slots }
  return WB.book
end
WB.bookLayout = book

local function bookSpell(index, bookType)
  if bookType == "pet" then return nil end
  return book().slots[index]
end
WB.bookSpell = bookSpell

function GetNumSpellTabs() return table.getn(book().tabs) end
function GetSpellTabInfo(tab)
  local t = book().tabs[tab]
  if not t then return nil, nil, 0, 0 end
  return t.name, t.icon, t.offset, t.count
end
function GetSpellName(index, bookType)
  local id = bookSpell(index, bookType)
  if not id then return nil, nil end
  local i = spellInfo(id)
  return i.name, WB.classRows and WB.classRows[id] and i.passive and (PASSIVE_PARENS or "(Passive)") or i.rank
end
function GetSpellTexture(index, bookType) local id = bookSpell(index, bookType) return id and spellIcon(id) end
function IsSpellPassive(index, bookType) local id = bookSpell(index, bookType) return flag(id and spellInfo(id).passive) end

-- start, duration, enable: the spell's own cooldown or the global one, whichever ends later.
local function cooldown(id)
  local info = spellInfo(id)
  local c = info and info.skyrimKind == "shout" and WB.skyrimShoutCooldown or WB.cooldowns[id]
  local g, now = WB.gcd, GetTime()
  local start, dur = 0, 0
  -- Resource updates also refresh FrameXML's timers. Reporting an expired
  -- timer restarts its completion flash on every magicka/energy tick.
  if c and c[2] > 0 and c[1] + c[2] > now then start, dur = c[1], c[2] end
  if info and info.gcd and g and g[2] > 0 and g[1] + g[2] > math.max(now, start + dur) then
    start, dur = g[1], g[2]
  end
  return start, dur
end
WB.cooldown = cooldown
function GetSpellCooldown(index, bookType)
  local id = bookSpell(index, bookType)
  if not id then return 0, 0, 1 end
  if WB.unlearned and WB.unlearned[id] then return 0, 0, 0 end
  local s, d = cooldown(id)
  return s, d, 1
end
function GetSpellAutocast() return nil, nil end
function CastSpell(index, bookType)
  local id = bookSpell(index, bookType)
  if id and not (WB.classRows and WB.classRows[id] and WB.classRows[id].passive) and
     not (WB.unlearned and WB.unlearned[id]) then host("castSpell", id) end
end
function CastSpellByName(text, onSelf) host("castSpellByName", text, onSelf) end
function SpellIsTargeting() return flag(WB.spellTargeting) end
-- 1 when there was something to stop (ToggleGameMenu: Escape stops a cast, then a placement, then clears the target).
function SpellStopCasting() if WB.casting then host("stopCasting") return 1 end end
function SpellStopTargeting() if WB.spellTargeting then host("stopTargeting") return 1 end end
function SpellTargetUnit(unit) host("spellTargetUnit", unit) end
function SpellCanTargetUnit(unit)
  if unit == nil then usage('SpellCanTargetUnit("unit")') end
  if not validUnit(unit) then error("Unknown unit name: " .. tostring(unit), 2) end
  return flag(WB.spellTargeting and resolve(unit))
end
function IsCurrentCast(index, bookType)
  local id = (bookType == "spell" or bookType == "pet") and bookSpell(index, bookType)
  if not id then error("Invalid spell slot in IsCurrentCast", 2) end
  return flag(WB.casting and WB.casting.spell == id)
end
function PickupSpell(index, bookType)
  local id = bookSpell(index, bookType)
  if id and WB.classRows and WB.classRows[id] and WB.classRows[id].passive then
    __WBFire("UI_ERROR_MESSAGE", ERR_PASSIVE_ABILITY)
  elseif id and not (WB.unlearned and WB.unlearned[id]) then WB.setCursor({ spell = id }) end
end
-- FrameXML calls this after choosing a skill-line tab or page; refresh the spell buttons for that selection.
function UpdateSpells() if __WBFire then __WBFire("SPELLS_CHANGED") end end
function ToggleSpellAutocast() end
function GetNumShapeshiftForms() return table.getn(WB.forms or {}) end
function GetShapeshiftFormInfo(i)
  if not i then usage("GetShapeshiftFormInfo(index)") end
  local f = WB.forms and WB.forms[i]
  if not f then return nil, nil, nil, nil end
  local castable = f.castable
  if castable and not f.active then
    local p, info = player(), f.spell and spellInfo(f.spell)
    if p and info then
      local mana = WB.availablePower(p, 0)
      if mana and mana < WB.spellCost(p, info) then castable = false end
    end
  end
  return f.icon, f.name, flag(f.active), flag(castable)
end
function GetShapeshiftFormCooldown(i)
  if not i then usage("GetShapeshiftFormCooldown(index)") end
  local f = WB.forms and WB.forms[i]
  local s, d = 0, 0
  if f and f.spell then s, d = cooldown(f.spell) end
  return s, d, 1
end
function CastShapeshiftForm(i)
  local f = WB.forms and WB.forms[i]
  host("shapeshift", i, f and f.spell or 0)
end

------------------------------------------------------------------------------------------------------------ actions
local function action(slot) return type(slot) == "number" and WB.actions[slot] end
function HasAction(slot) return flag(action(slot)) end
function GetActionTexture(slot) local a = action(slot) return a and a.spell and spellIcon(a.spell) end
function GetActionText(slot) local a = action(slot) return a and a.text end
function GetActionCount(slot) local a = action(slot) return a and a.count or 0 end
function GetActionCooldown(slot)
  local a = action(slot)
  if not a then return 0, 0, 0 end
  local s, d = cooldown(a.spell)
  return s, d, 1
end
-- In Skyrim her mana is magicka and a spell costs a fifth (service/skyrim.py); the recorded client paid WoW's.
-- In Skyrim each spell's magicka is the service's (op skyrim_costs, service/skyrim_balance.py), told by the host.
function WB.manaCost(p, amount, powerType, spell)
  -- Host costs already use client rage/energy units and include talent masks.
  if p and p.skyrim and spell and WB.skyrimCosts and WB.skyrimCosts[spell] ~= nil then return WB.skyrimCosts[spell] end
  if powerType == 1 then return amount / 10 end   -- DBC rage costs are tenths; UnitMana and host snapshots are 0..100
  if p and p.skyrim and (powerType or 0) == 0 and amount > 0 then return math.max(1, math.floor(amount * 0.2 + 0.5)) end
  return amount
end
-- Native sheets include magicka even while UnitMana represents rage/energy.
-- Reference clients without a native sheet keep the pre-shift mana snapshot.
function WB.availablePower(p, powerType)
  if not p then return end
  powerType = powerType or 0
  if (p.powerType or 0) == powerType then return p.power, p.powerMax end
  if powerType == 0 then
    local sheet = p.skyrim and p.nativeSheet
    if sheet and sheet.magicka ~= nil then return math.max(0, sheet.magicka), sheet.magickaMax end
    if WB.manaSnapshot then return WB.manaSnapshot[1], WB.manaSnapshot[2] end
  end
end
function WB.spellCost(p, info)
  if not info then return 0 end
  if info.skyrim then return info.cost or 0 end
  local amount = (info.cost or 0) + math.floor((info.costPct or 0) * (p and p.baseMana or 0) / 100)
  return WB.manaCost(p, amount, info.powerType, info.id)
end
function IsUsableAction(slot)
  local a = action(slot)
  if not a then return nil, nil end
  if WB.classRows and WB.classRows[a.spell] and WB.classRows[a.spell].passive then return nil, nil end
  if WB.unlearned and WB.unlearned[a.spell] then return nil, nil end
  local p, info = player(), spellInfo(a.spell)
  if WB.spellFormError(info) then
    local shift = WB.autoFormSpell(info)
    if not shift then return nil, nil end
    local form = spellInfo(shift)
    local mana = WB.availablePower(p, 0)
    if mana and mana < WB.spellCost(p, form) then return nil, 1 end
    return 1, nil
  end
  local form = WB.forms and WB.forms[WB.formIndex or 0]
  local active = WB.activeFormSpell or (form and form.spell)
  if active and active == a.spell then return 1, nil end -- leaving the active form is free
  local available = WB.availablePower(p, info and info.powerType)
  if available and available < WB.spellCost(p, info) then return nil, 1 end
  return 1, nil
end
-- 1 in range, 0 out of range, nil when the action has no range or there is no target.
function IsActionInRange(slot)
  local a = action(slot)
  local t = resolve("target")
  local distance = t == player() and 0 or t and t.distance
  if not a or not distance then return nil end
  local info = spellInfo(a.spell)
  if not info or not info.targetsUnit or (info.rangeMax or 0) == 0 then return nil end
  local range = WB.spellTooltips and WB.spellTooltips[info.id]
  if distance < (info.rangeMin or 0) or distance > (range and range.range_max or info.rangeMax) then return 0 end
  return 1
end
function ActionHasRange(slot) local a = action(slot) local i = a and spellInfo(a.spell) return flag(i and (i.rangeMax or 0) > 0) end
function IsCurrentAction(slot)
  local a = action(slot)
  return flag(a and ((WB.casting and WB.casting.spell == a.spell) or WB.spellTargeting == a.spell or (a.spell == 6603 and WB.attacking)))
end
function IsAutoRepeatAction() return nil end
function IsAttackAction(slot) local a = action(slot) return flag(a and a.spell == 6603) end
function IsConsumableAction() return nil end
function IsEquippedAction() return nil end
function UseAction(slot, checkCursor, onSelf)
  if checkCursor == 1 and WB.cursor then PlaceAction(slot) return end
  local a = action(slot)
  if a and not (WB.classRows and WB.classRows[a.spell] and WB.classRows[a.spell].passive) and
     not (WB.unlearned and WB.unlearned[a.spell]) then host("useAction", slot, a.spell, onSelf) end
end
function PickupAction(slot)
  local a = action(slot)
  if not a then return end
  WB.actions[slot] = nil
  WB.setCursor({ spell = a.spell })
  __WBFire("ACTIONBAR_SLOT_CHANGED", slot)
  host("actionChanged", slot)
end
function PlaceAction(slot)
  local c = WB.cursor
  if not c or not c.spell then return end
  local old = WB.actions[slot]
  WB.actions[slot] = { spell = c.spell }
  WB.setCursor(old and { spell = old.spell } or nil)
  __WBFire("ACTIONBAR_SLOT_CHANGED", slot)
  host("actionChanged", slot)
end
-- 1.12's takes no argument: the caller (the page bindings, ActionBar_PageUp/Down) sets CURRENT_ACTIONBAR_PAGE first,
-- and the client then raises ACTIONBAR_PAGE_CHANGED so every button shows that page's action. Without the event the
-- buttons kept page 1's icons while their keys used page 2's empty slots: greyed, silent (the user's playtest).
function ChangeActionBarPage(page)
  if page then CURRENT_ACTIONBAR_PAGE = page end
  WB.actionPage = CURRENT_ACTIONBAR_PAGE
  host("actionPage", WB.actionPage)
  __WBFire("ACTIONBAR_PAGE_CHANGED")
end
function GetActionBarToggles() local t = WB.barToggles or {} return t[1], t[2], t[3], t[4] end
function SetActionBarToggles(a, b, c, d) WB.barToggles = { a, b, c, d } host("barToggles", a, b, c, d) end
function GetBonusBarOffset() return WB.bonusBarOffset or 0 end

------------------------------------------------------------------------------------------------------------ cursor
function WB.cursorTexture() return WB.cursor and (WB.cursor.texture or WB.cursor.spell and spellIcon(WB.cursor.spell)) end
function WB.setCursor(cursor)
  if (not cursor or not cursor.item) and WB.bagHeld then ClearCursor() end
  local held = WB.cursor ~= nil
  WB.cursor = cursor
  if not cursor or not cursor.item then WB.bagCursor = nil end
  -- The client exposes empty action buttons while carrying a spell, making them drop targets.
  if cursor and not held then __WBFire("ACTIONBAR_SHOWGRID") end
  if not cursor and held then __WBFire("ACTIONBAR_HIDEGRID") end
  host("cursor", cursor and (cursor.item and "item" or "spell"), cursor and (cursor.item or cursor.spell), WB.cursorTexture())
end
function ClearCursor() WB.setCursor(nil) end
function CursorHasItem() return flag(WB.cursor and WB.cursor.item) end
function CursorHasSpell() return flag(WB.cursor and WB.cursor.spell) end
function CursorHasMoney() return nil end
function GetCursorMoney() return 0 end
function ResetCursor() host("setCursor", nil) end
function SetCursor(name) host("setCursor", name) end

------------------------------------------------------------------------------------------------------------ bindings
-- WB.bindings are the rows KeyBindingFrame lists; WB.hiddenBindings hold keys for commands it does not list.
local function allBindings()
  local i, list, hidden = 0, WB.bindings, WB.hiddenBindings or {}
  return function()
    i = i + 1
    if i > table.getn(list) then return hidden[i - table.getn(list)] end
    return list[i]
  end
end
local function bindingIndex()
  if WB.bindingKeys then return WB.bindingKeys end
  local keys = {}
  for b in allBindings() do
    if b[2] then keys[b[2]] = b end
    if b[3] then keys[b[3]] = b end
  end
  WB.bindingKeys = keys
  return keys
end
function GetNumBindings() return table.getn(WB.bindings) end
-- Only the keys a binding has are returned: a command with one key answers one value, not a trailing nil.
local function keysOf(b) if b[3] then return b[2], b[3] elseif b[2] then return b[2] end end
function GetBinding(i)
  if not i then usage("GetBinding(index)") end
  local b = WB.bindings[i]
  if not b then return "" end
  return b[1], keysOf(b)
end
function GetBindingKey(command)
  for b in allBindings() do
    if b[1] == command then return keysOf(b) end
  end
end
function GetBindingAction(key)
  local b = bindingIndex()[tostring(key)]
  return b and b[1] or ""
end
-- SetBinding(key) clears the key; SetBinding(key, command) moves it to command (first free of its two slots).
function SetBinding(key, command)
  if not key then usage('SetBinding("KEY"[, "COMMAND"])') end
  for b in allBindings() do
    if b[3] == key then b[3] = nil end
    if b[2] == key then b[2], b[3] = b[3], nil end
  end
  WB.bindingKeys = nil
  if not command then return 1 end
  for b in allBindings() do
    if b[1] == command then
      if not b[2] then b[2] = key elseif not b[3] then b[3] = key else b[2], b[3] = b[3], key end
      return 1
    end
  end
  return nil
end
-- Saved key bindings as the client keeps them: bindings-cache.wtf, one "bind KEY COMMAND" line per key. SaveBindings
-- hands the host the text to write for the set (1 the account's, 2 the character's) and the host gives it back at
-- start (__WBLoadBindings); LoadBindings reverts to what was saved (the panel's Cancel), or to the defaults (0).
local function bindingsText()
  local out = {}
  for b in allBindings() do
    if b[2] then table.insert(out, "bind " .. b[2] .. " " .. b[1]) end
    if b[3] then table.insert(out, "bind " .. b[3] .. " " .. b[1]) end
  end
  return table.concat(out, "\n") .. "\n"
end
-- The client's default keys, less WoWBridge's departures (the user's choices): T (ATTACKTARGET) stays Skyrim's -
-- she attacks with the Attack action in slot 1, a right click or Alt+right click; Shift+Tab (TARGETPREVIOUSENEMY)
-- opens Skyrim's Tab menu.
local UNBOUND_BY_DEFAULT = { ATTACKTARGET = true, TARGETPREVIOUSENEMY = true }
function __WBDefaultBindings()
  WB.bindings, WB.hiddenBindings = {}, {}
  for i, b in ipairs(WB.defaultBindings) do
    if UNBOUND_BY_DEFAULT[b[1]] then WB.bindings[i] = { b[1] } else WB.bindings[i] = { b[1], b[2], b[3] } end
  end
  for i, b in ipairs(WB.defaultHiddenBindings) do WB.hiddenBindings[i] = { b[1], b[2], b[3] } end
  WB.bindingKeys = nil
end
local defaultBindings = __WBDefaultBindings
local function applyBindings(text)
  for b in allBindings() do b[2], b[3] = nil, nil end
  for key, command in string.gmatch(text, "bind (%S+) (%S+)") do
    for b in allBindings() do
      if b[1] == command then
        if not b[2] then b[2] = key elseif not b[3] then b[3] = key end
        break
      end
    end
  end
  WB.bindingKeys = nil
end
function __WBLoadBindings(set, text)
  WB.bindingSet, WB.savedBindings = set, text
  applyBindings(text)
end
function SaveBindings(set)
  WB.bindingSet = set
  WB.savedBindings = bindingsText()
  host("saveBindings", set, WB.savedBindings)
  __WBFire("UPDATE_BINDINGS")
end
function LoadBindings(set)
  if set == 0 or not WB.savedBindings then defaultBindings() else applyBindings(WB.savedBindings) end
  __WBFire("UPDATE_BINDINGS")
end
function GetCurrentBindingSet() return WB.bindingSet or 1 end
-- A bound key pressed or released in play: the command's FrameXML script, as the client runs it - the release only
-- for the bindings that ask for it (runOnUp: the action buttons cast on release).
function __WBBinding(command, down)
  local script = __WoweeBindingScripts and __WoweeBindingScripts[command]
  if script and (down or __WoweeBindingRunOnUp[command]) then script(down and "down" or "up") end
end
function RunBinding(command, state) __WBBinding(command, state ~= "up") end

------------------------------------------------------------------------------------------------------------ cvars
function GetCVar(name)
  if not name then usage('GetCVar("cvar")') end
  local v = WB.cvars[name]
  if v == nil and WB.cvarDefaults then v = WB.cvarDefaults[name] end   -- a cvar never set has its default
  return v
end
function GetCVarDefault(name)
  if not name then usage('GetCVarDefault("cvar")') end
  return WB.cvarDefaults and WB.cvarDefaults[name]
end
function SetCVar(name, value)
  if not name then usage('SetCVar("cvar", value)') end
  WB.cvars[name] = tostring(value)
  host("cvar", name, tostring(value))
end
function RegisterCVar(name, default) if WB.cvars[name] == nil then WB.cvars[name] = default and tostring(default) end end
-- The cvars the player changed, as config-cache.wtf keeps them ('SET name "value"'), from the host at start.
function __WBLoadCVars(text)
  for name, value in string.gmatch(text, 'SET (%S+) "([^"\n]*)"') do WB.cvars[name] = value end
end

------------------------------------------------------------------------------------------------------------ world
local function W() return WB.world end
function GetZoneText() return W().zone or "" end
function GetRealZoneText() return W().zone or "" end
function GetSubZoneText() return W().subzone or "" end
function GetMinimapZoneText() return W().minimapZone or W().subzone or W().zone or "" end
function GetZonePVPInfo() return W().pvpType, W().pvpFaction, W().pvpCapture end
function GetBindLocation() return W().bindLocation or "" end
function IsInInstance() return flag(W().instance), W().instanceType or "none" end
function IsResting() return flag(W().resting) end
function GetGameTime() return W().hour or 0, W().minute or 0 end
function GetMoney() return W().money or 0 end
function HasFullControl() return flag(W().fullControl ~= false) end
function InCinematic() return nil end
function IsShiftKeyDown() return flag(WB.modifiers and WB.modifiers.shift) end
function IsControlKeyDown() return flag(WB.modifiers and WB.modifiers.ctrl) end
function IsAltKeyDown() return flag(WB.modifiers and WB.modifiers.alt) end
function GetLocale() return "enUS" end
function GetDefaultLanguage() return W().defaultLanguage or "Common" end
function IsMacClient() return nil end
function GetNetStats() return W().netIn or 0, W().netOut or 0, W().latency or 0 end
function GetComboPoints()
  if WB.tokens.target and WB.comboTarget == WB.tokens.target then return WB.comboPoints or 0 end
  return 0
end
function PlaySound(name) host("playSound", name) end
function PlaySoundFile(path) host("playSoundFile", path) end
function PlayMusic(path) host("playMusic", path) end
function StopMusic() host("stopMusic") end
function Screenshot() host("screenshot") end
function ConsoleExec(cmd) host("consoleExec", cmd) end
function ReloadUI() host("reloadUI") end
-- Globals the interface asks to keep between sessions (the client writes them to WTF\Account\...\SavedVariables).
function RegisterForSave(name) WB.saved = WB.saved or {} WB.saved[name] = true end
-- The interface's saved variables (RegisterForSave: its options), written as Lua the host runs again after the
-- interface has loaded and before VARIABLES_LOADED, as the client does.
function __WBSavedVariablesText()
  local out, names = {}, {}
  for name in pairs(WB.saved or {}) do table.insert(names, name) end
  table.sort(names)
  for _, name in ipairs(names) do
    local v = getglobal(name)
    local kind = type(v)
    if kind == "string" then table.insert(out, name .. " = " .. string.format("%q", v))
    elseif kind == "number" or kind == "boolean" then table.insert(out, name .. " = " .. tostring(v)) end
  end
  return table.concat(out, "\n") .. "\n"
end
-- ReloadUI rebuilds widgets/Lua, while the host's game state (book, forms, actions,
-- cooldowns, live units) stays. This text is held in memory, never written to WTF.
function __WBReloadStateText()
  if WB.bagHeld or WB.merchantCursor then ClearCursor() end
  local visiting = {}
  local function value(v)
    local kind = type(v)
    if kind == "string" then return string.format("%q", v) end
    if kind == "boolean" then return tostring(v) end
    if kind == "number" and v == v and math.abs(v) < math.huge then return string.format("%.17g", v) end
    if kind ~= "table" or visiting[v] then return nil end
    visiting[v] = true
    local fields = {}
    for k, x in pairs(v) do
      local key, text = value(k), value(x)
      if key and text then table.insert(fields, "[" .. key .. "]=" .. text) end
    end
    visiting[v] = nil
    return "{" .. table.concat(fields, ",") .. "}"
  end
  local state = {}
  for key, v in pairs(WB) do
    if type(v) ~= "function" and key ~= "saved" and key ~= "book" and key ~= "knownNames" and
       key ~= "hoverKey" and key ~= "cursor" and key ~= "spellTargeting" and
       key ~= "merchantInstalled" and key ~= "merchantCursor" and key ~= "bagsInstalled" and
       key ~= "bagCursor" and key ~= "bagHeld" and key ~= "bagOrigin" and
       key ~= "lootInstalled" and key ~= "lootPending" and key ~= "lootDepositPending" and key ~= "lootClosing" and
       key ~= "trainerClosing" then state[key] = v end
  end
  return "local state = " .. value(state) .. " for k,v in pairs(state) do __WB[k] = v end " ..
         "__WB.spellbookDirty = true if __WB.classReply then __WBClassSpells(__WB.classReply) end"
end
-- The account's tutorial flags, kept by the server (CMaNGOS character_tutorial, sent as SMSG_TUTORIAL_FLAGS): eight
-- words, tutorial n is bit n % 32 of word n / 32. The client raises TUTORIAL_TRIGGER only for a tutorial not flagged;
-- reading one flags it (FlagTutorial), unticking "show tutorials" flags them all (ClearTutorials), ResetTutorials
-- shows them again. Kept with the saved variables; the reference account's to begin with (GAMEMASTER: tut0 4194310,
-- tut1 512 - tutorials 1, 2, 22 and 41 read).
WBTutorialFlags = "4194310 512 0 0 0 0 0 0"
RegisterForSave("WBTutorialFlags")
local function tutorialWords()
  local w = {}
  for v in string.gmatch(WBTutorialFlags or "", "%d+") do table.insert(w, tonumber(v)) end
  for i = table.getn(w) + 1, 8 do w[i] = 0 end
  return w
end
local function tutorialBit(id)
  id = tonumber(id)
  if id and id >= 0 and id < 256 then return math.floor(id / 32) + 1, 2 ^ (id % 32) end
end
function __WBTutorialFlagged(id)
  local word, bit = tutorialBit(id)
  return word ~= nil and math.floor(tutorialWords()[word] / bit) % 2 == 1
end
function FlagTutorial(id)
  local word, bit = tutorialBit(id)
  if not word or __WBTutorialFlagged(id) then return end
  local w = tutorialWords()
  w[word] = w[word] + bit
  WBTutorialFlags = table.concat(w, " ")
end
function ClearTutorials() WBTutorialFlags = string.rep("4294967295 ", 7) .. "4294967295" end
function ResetTutorials() WBTutorialFlags = "0 0 0 0 0 0 0 0" end
function PartialPlayTime() return nil end
function NoPlayTime() return nil end
function Quit() host("quit") end
function ForceQuit() host("quit") end
function Logout() host("logout") end
function CancelLogout() host("cancelLogout") end
function SendChatMessage(msg, chatType, language, channel) host("chat", msg, chatType, language, channel) end
function DoEmote(token, unit) host("emote", token, unit) end
function RandomRoll(lo, hi) host("roll", lo, hi) end

------------------------------------------------------------------------------------------------------------ commands
-- Targeting, movement and camera: all owned by Skyrim, so they go to the host as they are.
for _, name in ipairs({
  "TargetUnit", "ClearTarget", "TargetNearestEnemy", "TargetNearestFriend", "TargetLastEnemy", "TargetByName",
  "TargetNearestPartyMember", "TargetNearestRaidMember", "AssistUnit", "AssistByName", "AttackTarget", "FollowUnit",
  "FollowByName", "InteractUnit", "Jump", "SitOrStand", "ToggleSheath", "ToggleRun", "ToggleAutoRun",
  "MoveForwardStart", "MoveForwardStop", "MoveBackwardStart", "MoveBackwardStop", "TurnLeftStart", "TurnLeftStop",
  "TurnRightStart", "TurnRightStop", "StrafeLeftStart", "StrafeLeftStop", "StrafeRightStart", "StrafeRightStop",
  "PitchUpStart", "PitchUpStop", "PitchDownStart", "PitchDownStop", "TurnOrActionStart", "TurnOrActionStop",
  "CameraOrSelectOrMoveStart", "CameraOrSelectOrMoveStop", "CameraZoomIn", "CameraZoomOut", "MoveViewInStart",
  "MoveViewInStop", "MoveViewOutStart", "MoveViewOutStop", "MoveViewLeftStart", "MoveViewLeftStop",
  "MoveViewRightStart", "MoveViewRightStop", "MoveViewUpStart", "MoveViewUpStop", "MoveViewDownStart",
  "MoveViewDownStop", "NextView", "PrevView", "ResetView", "SaveView", "SetView", "FlipCameraYaw",
  "ToggleCollision", "ToggleCollisionDisplay", "TogglePlayerBounds", "TogglePortals", "ToggleTris",
  "TogglePerformanceDisplay", "TogglePerformanceValues", "ResetPerformanceValues", "Stuck", "RepopMe",
  "RetrieveCorpse", "ShowNameplates", "HideNameplates", "ShowFriendNameplates", "HideFriendNameplates",
}) do
  local cmd = name
  _G[name] = function(...) return host("command", cmd, ...) end
end
-- Clearing the target is seen at once (and answers 1 when there was one): the host's next snapshot agrees.
function ClearTarget()
  if not WB.tokens.target then return end
  WB.tokens.target = nil
  host("command", "ClearTarget")
  __WBFire("PLAYER_TARGET_CHANGED")
  return 1
end

------------------------------------------------------------------------------------------------------------ widgets
-- 1.12's widget predicates answer 1 or nil, like the rest of its API; WoWee's answer true or false. FrameXML code
-- that compares with 1, or keeps the answer, sees the client's values.
do
  local function flagged(fn)
    return function(...)
      local r = { fn(...) }
      for i = 1, table.maxn(r) do
        if r[i] == true then r[i] = 1 elseif r[i] == false then r[i] = nil end
      end
      return unpack(r, 1, table.maxn(r))
    end
  end
  local function predicate(name)
    return strfind(name, "^Is%u") or strfind(name, "^Has%u") or strfind(name, "^Can%u") or name == "GetChecked"
  end
  local MT = __WoweeFrameMT
  for _, t in ipairs({ MT, type(MT.__index) == "table" and MT.__index or {} }) do
    for name, fn in pairs(t) do
      if type(fn) == "function" and predicate(name) then t[name] = flagged(fn) end
    end
  end
end

-- Button:Click() runs the button's scripts as a press does, with the 1.12 handler globals set: this (the button) and
-- arg1 (which mouse button). WoWee's passes them only as arguments, and 1.12's handlers read the globals - a dropdown
-- entry clicked from code raised "attempt to index global 'this'".
do
  local MT = __WoweeFrameMT
  local click = MT.Click
  function MT:Click(button, down)
    local was, wasArg = rawget(_G, "this"), rawget(_G, "arg1")
    this, arg1 = self, button or "LeftButton"
    local ok, err = pcall(click, self, button, down)
    this, arg1 = was, wasArg
    if not ok then error(err, 0) end
  end
end

-- The reference account's chat windows (the client's answers): General shown and docked first, the Combat Log docked
-- second and hidden until its tab is chosen, windows 3 to 7 unused. Answered here because the recorded stand-in gave
-- every window it had not recorded the Combat Log's answers - five more docked combat logs. General also carries the
-- Combat Log's groups, as a player sets it in the window's Messages settings (the user: the fights should show while
-- playing, not only under a tab).
local CHAT_WINDOWS = {
  [1] = { info = { n = 9, "", 0, 0, 0, 0, 0, 1, 1, 1 }, messages = { n = 10, "SYSTEM", "SAY", "YELL", "WHISPER", "PARTY", "GUILD", "CREATURE", "CHANNEL", "SKILL", "LOOT" } },
  [2] = { info = { n = 9, "", 0, 0, 0, 0, 0, nil, 1, 2 }, messages = { n = 34, "COMBAT_MISC_INFO", "COMBAT_SELF_HITS", "COMBAT_SELF_MISSES", "COMBAT_PET_HITS", "COMBAT_PET_MISSES", "COMBAT_HOSTILEPLAYER_HITS", "COMBAT_HOSTILEPLAYER_MISSES", "COMBAT_CREATURE_VS_SELF_HITS", "COMBAT_CREATURE_VS_SELF_MISSES", "COMBAT_FRIENDLY_DEATH", "COMBAT_HOSTILE_DEATH", "COMBAT_XP_GAIN", "SPELL_SELF_DAMAGE", "SPELL_SELF_BUFF", "SPELL_PET_DAMAGE", "SPELL_PET_BUFF", "SPELL_HOSTILEPLAYER_DAMAGE", "SPELL_HOSTILEPLAYER_BUFF", "SPELL_CREATURE_VS_SELF_DAMAGE", "SPELL_CREATURE_VS_SELF_BUFF", "SPELL_TRADESKILLS", "SPELL_DAMAGESHIELDS_ON_SELF", "SPELL_AURA_GONE_SELF", "SPELL_ITEM_ENCHANTMENTS", "SPELL_BREAK_AURA", "SPELL_PERIODIC_SELF_DAMAGE", "SPELL_PERIODIC_SELF_BUFFS", "SPELL_PERIODIC_HOSTILEPLAYER_DAMAGE", "SPELL_PERIODIC_HOSTILEPLAYER_BUFFS", "SPELL_PERIODIC_CREATURE_DAMAGE", "SPELL_PERIODIC_CREATURE_BUFFS", "COMBAT_HONOR_GAIN", "COMBAT_FACTION_CHANGE", "MONEY" } },
}
function GetChatWindowInfo(id)
  if id == nil then error("Usage: GetChatWindowInfo(index)", 2) end
  local w = CHAT_WINDOWS[tonumber(id)]
  if w then return unpack(w.info, 1, w.info.n) end
  return "", 0, 0, 0, 0, 0, nil, nil, nil
end
for i = 1, CHAT_WINDOWS[2].messages.n do
  table.insert(CHAT_WINDOWS[1].messages, CHAT_WINDOWS[2].messages[i])
end
CHAT_WINDOWS[1].messages.n = table.getn(CHAT_WINDOWS[1].messages)
function GetChatWindowMessages(id)
  if id == nil then error("Usage: GetChatWindowMessages(index)", 2) end
  local w = CHAT_WINDOWS[tonumber(id)]
  if w then return unpack(w.messages, 1, w.messages.n) end
end

-- BasicControls calls this optional client diagnostic from its error handler.
-- Keep the original message visible when a late-loaded addon reports an error.
function debuginfo() end
