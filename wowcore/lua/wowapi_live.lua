-- WoWBridge: the host's live state (from Skyrim and the gameplay service) into __WB, raising the events the 1.12
-- client raises for each change, so FrameXML redraws exactly what changed.
--
-- __WBApplyLive(s) takes a snapshot table; any part left out is left as it was:
--   s.player = { level, levelUp = {level, gains}, xp, xpMax, health, healthMax, power, powerMax, powerType, combat, skyrim } (skyrim = { Destruction, Restoration, Alteration }:
--              their skills; with it their mana is magicka and costs are a fifth)
--   s.target = { guid, name, level, health, healthMax, reaction, attackable, isPlayer, classification,
--                creatureType, distance, dead, sex, class, classToken, powerType } | "player" (yourself) | false (none)
--   s.mouseover = a unit as s.target | { object, locked, steal } (a thing in the world) | false (nothing)
--   s.party = ordered list of up to four units, with power/powerMax/connected and known Skyrim auras
--   s.nameplates = { epoch, units = { { guid, name, level, health, healthMax, reaction, classification,
--                    attackable, x/y = normalized screen position from top left, distance = yards,
--                    bx/by = the same for its chat bubble's point, 0.7 yards over it }, ... } }
--   s.world  = { zone, subzone, hour, minute }
--   s.minimap = { worldspace = terrain editor ID, x/y = yards east/north, heading = radians clockwise, trackingMask = creature mask,
--                 blips = { {guid, x/y = yards east/north, reaction = 1..8}, ... } } | false
--   s.worldmap = { worldspace, name, parents, x/y = exterior yards east/north, heading, interior, pois } | false
--   s.form = shapeshift bar index (0 to leave); s.prowling = boolean; s.comboPoints = 0..5 on s.target
-- Rage and energy are client units 0..100 (no Skyrim conversion, no server rage tenths).

local WB = __WB
local targetNumbersInstalled = false -- widgets/closures are recreated by ReloadUI

local function fire(...) __WBFire(...) end
local function set(t, k, v)
  if v ~= nil and t[k] ~= v then t[k] = v return true end
end

local POWER_EVENT = { [0] = "MANA", "RAGE", "FOCUS", "ENERGY", "HAPPINESS" }
local function playerLevel(level)
  assert(type(level) == "number" and level > 0 and level < math.huge and level == math.floor(level), "expected positive integer player level")
  return level
end
-- Keep Blizzard's bar, hover text and newbie tooltip. Skyrim has no rested XP or overall level-60 cap.
-- These hooks are installed only for a live XP snapshot, after FrameXML has defined the client functions.
local experienceInstalled = false
function __WBInstallExperience()
  if experienceInstalled or not ReputationWatchBar_Update then return end
  experienceInstalled = true
  MainMenuExpBar:SetScript("OnMouseUp", function()
    if arg1 == "LeftButton" then WB.host("menu", "StatsMenu") end
  end)
  local enter = MainMenuExpBar:GetScript("OnEnter")
  MainMenuExpBar:SetScript("OnEnter", function()
    enter()
    local p = WB.resolve("player")
    if p and p.skyrimXP and p.xp >= p.xpMax then
      GameTooltip_SetDefaultAnchor(GameTooltip, this)
      GameTooltip:SetText("Level up available", 1, 1, 1)
      GameTooltip:AddLine("Click to open Skyrim's Skills menu and choose Health, Magicka or Stamina.", 1, 1, 1, 1)
      GameTooltip:Show()
    end
  end)
  local reputation, exhaustion, newbie = ReputationWatchBar_Update, ExhaustionToolTipText, GameTooltip_AddNewbieTip
  ReputationWatchBar_Update = function(level)
    local p = WB.resolve("player")
    reputation(p and p.skyrimXP and 1 or level) -- use the client's below-cap layout, including watched reputation
  end
  ExhaustionToolTipText = function()
    local p = WB.resolve("player")
    if not (p and p.skyrimXP) then exhaustion() end
  end
  GameTooltip_AddNewbieTip = function(label, r, g, b, text, noNormalText)
    local p = WB.resolve("player")
    if p and p.skyrimXP and label == XPBAR_LABEL then
      -- The client definition is the first sentence; the rest describes WoW's rested XP system.
      text = (string.match(text, "^.-%.") or text) .. "\nClick to open Skyrim's Skills menu."
    end
    newbie(label, r, g, b, text, noNormalText)
  end
end
local function applyPlayer(s)
  local p = WB.units[WB.tokens.player]
  if not p then return end
  local oldMana, oldMaxMana = WB.availablePower(p, 0)
  local firstXP = s.xpMax ~= nil and not p.skyrimXP
  if s.xpMax ~= nil then p.skyrimXP = true __WBInstallExperience() end
  local xp = set(p, "xp", s.xp)
  local xpMax = set(p, "xpMax", s.xpMax)
  -- Celebration and synchronization travel together even when the host coalesces pending snapshots.
  if s.level and s.level < p.level then WB.levelUpNotified = nil end
  if s.levelUp and s.levelUp.level == s.level then __WBLevelUp(s.levelUp.level, s.levelUp.gains, true) end
  local maxHp = set(p, "healthMax", s.healthMax)
  local hp = set(p, "health", s.health)
  local self = WB.tokens.target == WB.tokens.player   -- the target frame shows you too
  local function both(event) fire(event, "player") if self then fire(event, "target") end end
  if s.name ~= nil and set(p, "name", s.name ~= "" and s.name or "Dragonborn") then
    p.pvpName = nil
    both("UNIT_NAME_UPDATE")
  end
  if set(p, "sex", s.sex) then both("UNIT_PORTRAIT_UPDATE") end
  if s.level ~= nil and set(p, "level", playerLevel(s.level)) then
    both("UNIT_LEVEL")
    if WB.talents then fire("CHARACTER_POINTS_CHANGED", 0, 0) end
  end
  if firstXP then
    if ReputationWatchBar_Update then ReputationWatchBar_Update() end
    fire("UPDATE_EXHAUSTION")
  end
  if xp or xpMax or firstXP then fire("PLAYER_XP_UPDATE", "player") end
  -- A threshold-only change leaves SetValue unchanged, so no OnValueChanged refreshes the hover text.
  if xpMax and MainMenuExpBar then TextStatusBar_UpdateTextString(MainMenuExpBar) end
  if maxHp then both("UNIT_MAXHEALTH") end
  if hp or maxHp then both("UNIT_HEALTH") end
  local powerType = set(p, "powerType", s.powerType)
  local maximum, value = s.powerMax, s.power
  if p.powerType == 1 or p.powerType == 3 then
    maximum = 100
    if value ~= nil then value = math.floor(math.max(0, math.min(100, value))) end
  end
  local maxPower = set(p, "powerMax", maximum)
  local power = set(p, "power", value)
  local resource = POWER_EVENT[p.powerType or 0] or "MANA"
  if powerType then both("UNIT_DISPLAYPOWER") end
  if maxPower or powerType then both("UNIT_MAX" .. resource) end
  if power or maxPower or powerType then both("UNIT_" .. resource) end
  local function same(a,b)
    if type(a)~="table" or type(b)~="table" then return a==b end
    for k,v in pairs(a) do if not same(v,b[k]) then return false end end
    for k in pairs(b) do if a[k]==nil then return false end end
    return true
  end
  for _,field in ipairs({{"armor","UNIT_RESISTANCES"},{"damage","UNIT_DAMAGE"},
      {"stats","UNIT_STATS"},{"resistances","UNIT_RESISTANCES"},
      {"attackPower","UNIT_ATTACK_POWER"},{"attackSpeed","UNIT_ATTACK_SPEED"},
      {"attackBothHands","UNIT_ATTACK"},{"defense","UNIT_DEFENSE"},
      {"rangedAttack","UNIT_RANGED_ATTACK_POWER"},{"rangedDamage","UNIT_RANGEDDAMAGE"},
      {"rangedAttackPower","UNIT_RANGED_ATTACK_POWER"}}) do
    if s[field[1]] and not same(p[field[1]],s[field[1]]) then
      p[field[1]]=s[field[1]] both(field[2])
    end
  end
  if s.nativeSheet and not same(p.nativeSheet,s.nativeSheet) then
    p.nativeSheet=s.nativeSheet
    if __WBCharacterStats then __WBCharacterStats() end
  end
  if s.skyrim ~= nil then
    local was = p.skyrim
    p.skyrim = s.skyrim
    if not was then fire("ACTIONBAR_UPDATE_USABLE") fire("SPELL_UPDATE_USABLE") end
    if not same(was,s.skyrim) then fire("SKILL_LINES_CHANGED") end
  end
  if s.combat ~= nil and (p.combat or false) ~= s.combat then
    p.combat = s.combat
    fire(s.combat and "PLAYER_REGEN_DISABLED" or "PLAYER_REGEN_ENABLED")
    fire("UNIT_FLAGS", "player")
  end
  local hiddenManaChanged = false
  if (p.powerType or 0) ~= 0 then
    local mana, maximum = WB.availablePower(p, 0)
    if mana ~= nil then
      WB.manaSnapshot = {mana, maximum}
      hiddenManaChanged = mana ~= oldMana or maximum ~= oldMaxMana
    end
  end
  -- Publish availability after the native sheet, so form buttons see fresh
  -- magicka even if energy/rage did not change in this snapshot.
  if power or maxPower or powerType or hiddenManaChanged then
    fire("ACTIONBAR_UPDATE_USABLE")
    fire("SPELL_UPDATE_USABLE")
  end
end

-- Opt in to their learned forms. The bar keeps the original Bear slot when Dire Bear replaces it at level 40:
-- Bear/Dire Bear, Aquatic, Cat, Travel. Names and icons come from Spell/SpellIcon, not a second icon table.
-- count: the forms the player knows, in the order a druid learns them (Bear 10, Aquatic 16, Cat 20, Travel 30); default all.
function __WBDruidForms(bearSpell, count, moonkin)
  if WB.classId and WB.classId ~= 11 then WB.forms = {} WB.formIndex = 0 return end
  bearSpell = bearSpell or 9634
  assert(bearSpell == 5487 or bearSpell == 9634, "expected Bear or Dire Bear spell")
  local active = WB.forms and WB.forms[WB.formIndex or 0]
  local activeSpell = WB.activeFormSpell or (active and active.spell)
  WB.forms = {}
  for i, spell in ipairs({ bearSpell, 1066, 768, 783 }) do
    if i > (count or 4) then break end
    local info = assert(WB.spellInfo(spell), "missing druid form spell data")
    table.insert(WB.forms, { spell = spell, icon = info.icon, name = info.name, castable = true, active = false })
  end
  if moonkin then
    local info = assert(WB.spellInfo(24858), "missing Moonkin Form spell data")
    table.insert(WB.forms, { spell = 24858, icon = info.icon, name = info.name, castable = true, active = false })
  end
  WB.formIndex = 0
  if activeSpell then
    for i, f in ipairs(WB.forms) do
      if f.spell == activeSpell or activeSpell == 5487 and f.spell == 9634 then WB.formIndex = i break end
    end
  end
  if WB.formIndex and WB.forms[WB.formIndex] then WB.forms[WB.formIndex].active = true end
  fire("UPDATE_SHAPESHIFT_FORMS")
  -- Forms can arrive after login's layout pass. Apply FrameXML's bar appearance
  -- now too (background hidden and 50-unit borders above the bottom-left bar).
  if UIParent_ManageFramePositions then UIParent_ManageFramePositions() end
end

-- Call only while constructing a new action layout, never after loading the player's
-- saved actions. A partially populated realm page is also a player-owned layout.
function __WBDefaultFormActions()
  if WB.classId and WB.classId ~= 11 then return end
  WB.actions = WB.actions or {}
  local pages = {
    { 73, 9850, 9830, 9904, 9896, 31018, 9846, 9821, 9892, 17392, 9913 },
    { 85, 9867, 9827, 9830, 9904 },
    { 97, 9881, 9908, 6795, 9898, 8983, 5229, 22896, 17392 },
  }
  for _, page in ipairs(pages) do
    local first, empty = page[1], true
    for slot = first, first + 11 do if WB.actions[slot] then empty = false break end end
    if empty then
      WB.actions[first] = { spell = 6603 }
      for i = 2, table.getn(page) do WB.actions[first + i - 1] = { spell = page[i] } end
    end
  end
  fire("ACTIONBAR_SLOT_CHANGED", 0)
end

function __WBForm(index, powerType, prowling, activeSpell)
  if WB.classId and WB.classId ~= 11 then index, powerType, prowling, activeSpell = 0, 0, false, 0 end
  if not WB.forms then __WBDruidForms() end
  assert(index == 0 or WB.forms[index], "invalid shapeshift bar index")
  local form = WB.forms[index]
  local spell = activeSpell or (form and form.spell) or 0
  local expectedPower = spell == 768 and 3 or (spell == 5487 or spell == 9634) and 1 or 0
  powerType = powerType or expectedPower
  local changed = (WB.formIndex or 0) ~= index or (WB.activeFormSpell or 0) ~= spell
  local p = WB.units[WB.tokens.player]
  if changed and p then
    if (p.powerType or 0) == 0 and powerType ~= 0 then WB.manaSnapshot = { p.power, p.powerMax } end
    if powerType == 0 and WB.manaSnapshot then
      local mana, maximum = WB.availablePower(p, 0)
      applyPlayer({ powerType = 0, power = mana, powerMax = maximum })
      WB.manaSnapshot = nil
    elseif powerType ~= 0 then
      applyPlayer({ powerType = powerType, power = 0, powerMax = 100 })
    else
      applyPlayer({ powerType = powerType })
    end
  end
  WB.formIndex, WB.activeFormSpell = index, spell
  if changed then WB.prowling = false end
  if prowling ~= nil then WB.prowling = prowling end
  for i, f in ipairs(WB.forms) do f.active = i == index end
  local offset = spell == 768 and (WB.prowling and 2 or 1) or (spell == 5487 or spell == 9634) and 3 or 0
  local pageChanged = set(WB, "bonusBarOffset", offset)
  if changed then fire("UPDATE_SHAPESHIFT_FORMS") fire("PLAYER_AURAS_CHANGED") end
  if pageChanged then fire("UPDATE_BONUS_ACTIONBAR") fire("ACTIONBAR_PAGE_CHANGED") end
  if changed or pageChanged then fire("ACTIONBAR_UPDATE_STATE") fire("ACTIONBAR_UPDATE_USABLE") end
  if WB.classId == 2 then __WBPaladinAuras() end   -- their bar holds their auras, not forms
end

-- A Paladin's auras ride the stance bar: the 1.12 client lists spells with AttributesEx2's
-- SPELL_ATTR_EX2_USE_SHAPESHIFT_BAR (0x10) there, the highest rank of each in their book's order; the aura on the player is
-- the active button, and clicking one casts it (the host's shapeshift command casts any spell the player knows).
function __WBPaladinAuras()
  if WB.classId ~= 2 then return end
  local forms, byName = {}, {}
  for _, id in ipairs(WB.spellbook or {}) do
    local info = type(id) == "number" and id > 0 and WB.spellInfo(id)
    if info and math.floor((info.attributesExB or 0) / 16) - math.floor((info.attributesExB or 0) / 32) * 2 == 1 then
      local f = byName[info.name]
      if not f then
        f = { spell = id, icon = info.icon, name = info.name, castable = true, active = false }
        byName[info.name] = f
        table.insert(forms, f)
      elseif (info.spellLevel or 0) > ((WB.spellInfo(f.spell) or {}).spellLevel or 0) then
        f.spell, f.icon = id, info.icon
      end
    end
  end
  local player = WB.units and WB.units[WB.tokens.player]
  local index = 0
  for i, f in ipairs(forms) do
    for _, a in ipairs(player and player.buffs or {}) do
      local info = WB.spellInfo(a.spell)
      if info and info.name == f.name then f.active = true index = i end
    end
  end
  local before = WB.forms
  local changed = not before or table.getn(before) ~= table.getn(forms) or (WB.formIndex or 0) ~= index
  if not changed then
    for i, f in ipairs(forms) do if before[i].spell ~= f.spell then changed = true end end
  end
  WB.forms, WB.formIndex = forms, index
  if changed then
    fire("UPDATE_SHAPESHIFT_FORMS")
    if UIParent_ManageFramePositions then UIParent_ManageFramePositions() end
  end
end

-- Active form is independent of learned slots: console previews can enter an
-- unlearned form without inventing a castable button or losing power/action state.
function __WBFormSpell(spell, powerType, prowling)
  local index = 0
  for i, f in ipairs(WB.forms or {}) do
    if f.spell == spell or spell == 5487 and f.spell == 9634 then index = i break end
  end
  __WBForm(index, powerType, prowling, spell)
end

function __WBComboPoints(n)
  n = WB.tokens.target and math.floor(math.max(0, math.min(5, n))) or 0
  local changed = (WB.comboPoints or 0) ~= n or WB.comboTarget ~= WB.tokens.target
  WB.comboPoints, WB.comboTarget = n, WB.tokens.target
  if changed then fire("PLAYER_COMBO_POINTS") end
end

local function targetChanged(guid)
  WB.tokens.target = guid
  __WBComboPoints(0)
  fire("PLAYER_TARGET_CHANGED")
end

local TARGET_FIELDS = { "name", "level", "reaction", "attackable", "isPlayer", "classification", "creatureType",
                        "distance", "dead", "sex", "class", "classToken", "powerType" }

local function applyTarget(t)
  if t == "player" then   -- yourself (F1)
    if WB.tokens.target ~= WB.tokens.player then
      targetChanged(WB.tokens.player)
    end
    return
  end
  if t == false then
    if WB.tokens.target then
      targetChanged(nil)
    end
    return
  end
  local guid = t.guid
  local u = WB.units[guid]
  if not u then
    u = { power = 0, powerMax = 0, powerType = 1, connected = true, classification = "normal" }
    WB.units[guid] = u
  end
  local powerType=t.powerType~=nil and t.powerType~=u.powerType
  for _, k in ipairs(TARGET_FIELDS) do if t[k] ~= nil then u[k] = t[k] end end
  local maxHp = set(u, "healthMax", t.healthMax)
  local hp = set(u, "health", t.health)
  local maxPower = set(u, "powerMax", t.powerMax)
  local power = set(u, "power", t.power)
  if WB.tokens.target ~= guid then
    targetChanged(guid)
  elseif hp or maxHp then
    if maxHp then fire("UNIT_MAXHEALTH", "target") end
    fire("UNIT_HEALTH", "target")
  end
  local resource=POWER_EVENT[u.powerType or 0] or "MANA"
  if powerType then fire("UNIT_DISPLAYPOWER","target") end
  if maxPower then fire("UNIT_MAX"..resource,"target") end
  if power or maxPower then fire("UNIT_"..resource,"target") end
end

-- What the pointer is over in the world, as the client shows it: a unit's tooltip at the default place (SetUnit
-- "mouseover", its name coloured on UPDATE_MOUSEOVER_UNIT), a thing's name in white with "Locked" in red; gone when
-- the pointer leaves it. Only a tooltip the world put up is taken down (a frame's own is left alone).
local function applyMouseover(m)
  if m and m.guid then
    local u = WB.units[m.guid] or { power = 0, powerMax = 0, powerType = 1, connected = true }
    WB.units[m.guid] = u
    for _, k in ipairs(TARGET_FIELDS) do if m[k] ~= nil then u[k] = m[k] end end
    u.health, u.healthMax = m.health, m.healthMax
  end
  local key = m and (m.guid or m.object .. (m.action or "") .. (m.locked and "|locked" or "") .. (m.steal and "|steal" or "")) or nil
  if key == WB.hoverKey then return end
  WB.hoverKey = key
  WB.tokens.mouseover = m and m.guid or nil
  if not key then
    if GameTooltip:IsOwned(UIParent) then GameTooltip:FadeOut() end
    return
  end
  GameTooltip_SetDefaultAnchor(GameTooltip, UIParent)
  if m.guid then
    GameTooltip:SetUnit("mouseover")
    fire("UPDATE_MOUSEOVER_UNIT")
    return
  end
  GameTooltip:SetText(m.object, 1, 1, 1)
  if m.action and m.action ~= "" then GameTooltip:AddLine(m.action, 1, 1, 1, 1) end
  if m.locked then GameTooltip:AddLine(LOCKED, RED_FONT_COLOR.r, RED_FONT_COLOR.g, RED_FONT_COLOR.b) end
  if m.steal then GameTooltip:AddLine("Steal", RED_FONT_COLOR.r, RED_FONT_COLOR.g, RED_FONT_COLOR.b) end   -- Skyrim's word
  GameTooltip:Show()
end

local function applyWorld(w)
  local W = WB.world
  local zone = set(W, "zone", w.zone)
  local sub = set(W, "subzone", w.subzone)
  if w.subzone then W.minimapZone = w.subzone ~= "" and w.subzone or W.zone end
  W.hour, W.minute = w.hour or W.hour, w.minute or W.minute
  -- The client's clock art is an atlas. GameTime.xml updates it on load,
  -- before the host's first time snapshot, and has no minute-update script.
  -- Refresh through its own Lua when live time arrives, including midnight
  -- (the XML's initial timeOfDay=0 would otherwise leave the whole atlas).
  if (w.hour ~= nil or w.minute ~= nil) and type(rawget(_G,'GameTimeFrame')) == 'table' and
     type(rawget(_G,'GameTimeFrame_Update')) == 'function' then
    local previous = this
    this = GameTimeFrame
    this.timeOfDay = nil
    GameTimeFrame_Update()
    this = previous
  end
  if w.money and set(W, "money", w.money) then fire("PLAYER_MONEY") end   -- their septims (Bridge.cpp)
  if zone then fire("ZONE_CHANGED_NEW_AREA") elseif sub then fire("ZONE_CHANGED") end
  if zone or sub then fire("MINIMAP_ZONE_CHANGED") end
end

function __WBApplyLive(s)
  if s.form ~= nil then __WBForm(s.form, nil, s.prowling)
  elseif s.prowling ~= nil then __WBFormSpell(WB.activeFormSpell or 0, nil, s.prowling) end
  if s.player then applyPlayer(s.player) end
  if s.party ~= nil then __WBParty(s.party) end
  if s.target ~= nil then applyTarget(s.target) end
  if s.nameplates ~= nil then __WBNameplates(s.nameplates) end
  if s.comboPoints ~= nil then __WBComboPoints(s.comboPoints) end
  if s.mouseover ~= nil then applyMouseover(s.mouseover) end
  if s.world then applyWorld(s.world) end
  if s.minimap ~= nil then
    local old = WB.minimap and WB.minimap.worldspace
    WB.minimap = s.minimap or nil
    if old ~= (WB.minimap and WB.minimap.worldspace) then fire("MINIMAP_UPDATE_ZOOM") end
  end
  if s.worldmap ~= nil then WB.worldmap = s.worldmap or nil end
  if s.head ~= nil then WB.playerHead = s.head or nil end
end

------------------------------------------------------------------------------------------------- casting and combat
-- The host's casting, in the events and wording the 1.12 client uses: SPELLCAST_START/STOP/FAILED/INTERRUPTED,
-- the global cooldown, auras on units, UI errors and combat log lines through FrameXML's own strings
-- (GlobalStrings: SPELL_FAILED_*, SPELLLOGSCHOOLSELFOTHER, ...). Times are the interface's clock (GetTime).

local SCHOOL = { [0] = "Physical", "Holy", "Fire", "Nature", "Frost", "Shadow", "Arcane" }
WB.SCHOOL = SCHOOL

function __WBUiError(key)
  -- Native client failures include audio outside FrameXML's message handler.
  -- Feral finishers use the archived nature fizzle; text/font/colour/lifetime
  -- remain the unchanged UIErrorsFrame and SPELL_FAILED_NO_COMBO_POINTS.
  if key == "SPELL_FAILED_NO_COMBO_POINTS" then PlaySound("SpellFizzleNature") end
  -- This failure has no reagent-name argument; the named template contains %s.
  if key=="SPELL_FAILED_REAGENTS" then key="ERR_SPELL_FAILED_REAGENTS_GENERIC" end
  __WBFire("UI_ERROR_MESSAGE", getglobal(key) or key)
end

-- What a person or creature says, as the client shows a creature's speech in chat ("Faendal says: ..."; General
-- carries CREATURE) and, with its unit (the actor's form ID, yards from the player), over its head (wowapi_bubbles.lua).
-- The host passes Skyrim's subtitles and the people's answers to their emotes.
function __WBSay(speaker, text, guid, yards)
  __WBFire("CHAT_MSG_MONSTER_SAY", text, speaker, "", "", "", "", 0, 0, "")
  if guid then __WBChatBubble(guid, text, "MONSTER_SAY", yards) end
end

-- A combat log line: the chat event and the GlobalStrings format it is written with. The client gives every chat
-- event all nine arguments - the line, then author, language, channel, target, flags as "", the channel ids as 0 -
-- and ChatFrame_OnEvent reads the channel's length before anything else (with only the line, it raised and no
-- combat line ever reached the Combat Log window).
function __WBLog(event, fmtKey, ...)
  __WBFire(event, string.format(getglobal(fmtKey) or fmtKey, unpack(arg)), "", "", "", "", "", 0, 0, "")
end

-- Nearby combat supplied by the host (which owns radius filtering and duplicate suppression).
-- Units are ephemeral {name, category}; they need not be a target or exist in WB.units.
-- See docs/combat-world.md for the category mapping and report schema.
local COMBAT_CATEGORY = { self = "SELF", party = "PARTY", friendlyPlayer = "FRIENDLYPLAYER",
                          hostilePlayer = "HOSTILEPLAYER", friendlyCreature = true, hostileCreature = true }
local function combatUnit(u)
  assert(type(u) == "table" and COMBAT_CATEGORY[u.category], "expected combat unit category")
  assert(type(u.name) == "string" and u.name ~= "", "expected combat unit name")
  return u
end
local function combatNumber(n)
  assert(type(n) == "number" and n >= 0 and n < math.huge and n == math.floor(n), "expected nonnegative combat amount")
  return n
end
local function combatGroup(source, target)
  local group = COMBAT_CATEGORY[source.category]
  if type(group) == "string" then return group end
  return "CREATURE_VS_" .. (target.category == "self" and "SELF" or target.category == "party" and "PARTY" or "CREATURE")
end
local function combatLine(key, values)
  return string.format(assert(getglobal(key), "missing client combat string: " .. key), unpack(values))
end
local function combatChat(group, line)
  fire("CHAT_MSG_" .. group, line, "", "", "", "", "", 0, 0, "")
end

function __WBCombatEvent(e)
  assert(type(e) == "table", "expected combat report")
  local target = combatUnit(e.target)
  if e.kind == "death" then
    local friendly = target.category == "self" or target.category == "party" or
                     target.category == "friendlyPlayer" or target.category == "friendlyCreature"
    combatChat("COMBAT_" .. (friendly and "FRIENDLY_DEATH" or "HOSTILE_DEATH"),
               combatLine(target.category == "self" and "UNITDIESSELF" or "UNITDIESOTHER", {target.name}))
    return
  elseif e.kind == "kill" then
    -- The killing-blow notification is separate from the unit-death notification, like the client's packets.
    assert(target.category ~= "self", "expected another unit for your killing blow")
    combatChat("COMBAT_HOSTILE_DEATH", combatLine("SELFKILLOTHER", {target.name}))
    return
  end
  local source = combatUnit(e.source)
  local group = combatGroup(source, target)
  local suffix = (source.category == "self" and "SELF" or "OTHER") ..
                 (target.category == "self" and "SELF" or "OTHER")
  local values, key, channel = {}, nil, nil
  local function add(v) table.insert(values, v) end
  local school = e.school or 0
  assert(SCHOOL[school], "expected combat school 0..6")
  if e.kind == "swing" or (e.kind == "damage" and not e.spell) then
    assert(suffix ~= "SELFSELF", "expected another unit for melee")
    local outcome = e.outcome or "hit"
    local miss = ({miss="MISSED", dodge="VSDODGE", parry="VSPARRY", block="VSBLOCK",
                   immune="VSIMMUNE", absorb="VSABSORB"})[outcome]
    assert(miss or outcome == "hit" or outcome == "crit" or outcome == "glance", "expected melee outcome")
    key = miss or ("COMBATHIT" .. (outcome == "crit" and "CRIT" or "") .. (school ~= 0 and "SCHOOL" or ""))
    if source.category ~= "self" then add(source.name) end
    if target.category ~= "self" then add(target.name) end
    if not miss then
      add(combatNumber(e.amount))
      if school ~= 0 then add(SCHOOL[school]) end
    end
    channel = "COMBAT_" .. group .. (miss and "_MISSES" or "_HITS")
  elseif e.kind == "damage" or e.kind == "heal" then
    assert(type(e.spell) == "string" and e.spell ~= "", "expected combat spell name")
    if e.periodic then
      if target.category ~= "self" then add(target.name) end
      add(combatNumber(e.amount))
      if e.kind == "damage" then add(SCHOOL[school]) end
      if source.category ~= "self" then add(source.name) end
      add(e.spell)
      key = e.kind == "heal" and "PERIODICAURAHEAL" or "PERIODICAURADAMAGE"
      local recipient = COMBAT_CATEGORY[target.category]
      channel = "SPELL_PERIODIC_" .. (type(recipient) == "string" and recipient or "CREATURE") ..
                (e.kind == "heal" and "_BUFFS" or "_DAMAGE")
    else
      if source.category ~= "self" then add(source.name) end
      add(e.spell)
      if target.category ~= "self" then add(target.name) end
      add(combatNumber(e.amount))
      if e.kind == "heal" then
        key = "HEALED" .. (e.crit and "CRIT" or "")
      else
        key = "SPELLLOG" .. (e.crit and "CRIT" or "") .. (school ~= 0 and "SCHOOL" or "")
        if school ~= 0 then add(SCHOOL[school]) end
      end
      channel = "SPELL_" .. group .. (e.kind == "heal" and "_BUFF" or "_DAMAGE")
    end
  else
    error("expected swing, damage, heal, death or kill report")
  end
  local line = combatLine(key .. suffix, values)
  if e.outcome == "glance" then line = line .. GLANCING_TRAILER end
  for _, trailer in ipairs({{"blocked", "BLOCK_TRAILER"}, {"absorbed", "ABSORB_TRAILER"}, {"resisted", "RESIST_TRAILER"}}) do
    if e[trailer[1]] ~= nil and combatNumber(e[trailer[1]]) > 0 then
      line = line .. combatLine(trailer[2], {e[trailer[1]]})
    end
  end
  combatChat(channel, line)
end

------------------------------------------------------------------------------------------------- class progression
-- Installed on the first host answer after the interface loads (a ReloadUI runs the recorded defaults again while
-- __WB, and its talents, survive), after recorded API defaults have loaded. Reference HUDs keep their recorded
-- answers; in game the service owns the tree order, allocation and point budget.
local function installTalentApi()
  __WBTalentApi = true   -- a global of this interface: gone with a ReloadUI, unlike __WB
  local characterPoints = UnitCharacterPoints
  GetNumTalentTabs = function() return table.getn(WB.talents.trees) end
  GetTalentTabInfo = function(tab)
    local tree = WB.talents.trees[tab]
    if tree then return tree.name, tree.icon, tree.points_spent, tree.background end
  end
  GetNumTalents = function(tab)
    local tree = WB.talents.trees[tab]
    return tree and table.getn(tree.talents) or 0
  end
  GetTalentInfo = function(tab, index)
    local tree = WB.talents.trees[tab]
    local t = tree and tree.talents[index]
    if t then return t.name, t.icon, t.tier + 1, t.column + 1, t.rank, t.max_rank,
      t.exceptional and 1 or nil, t.meets_prerequisites and 1 or nil end
  end
  GetTalentPrereqs = function(tab, index)
    local tree = WB.talents.trees[tab]
    local t, args = tree and tree.talents[index], { n = 0 }
    for _, p in ipairs(t and t.prerequisites or {}) do
      for _, parent in ipairs(tree.talents) do
        if parent.id == p.talent then
          args[args.n + 1], args[args.n + 2] = parent.tier + 1, parent.column + 1
          args[args.n + 3] = parent.rank >= p.rank and 1 or nil
          args.n = args.n + 3
          break
        end
      end
    end
    return unpack(args, 1, args.n)
  end
  UnitCharacterPoints = function(unit)
    -- Another class has no points; a Paladin's come with their talent snapshot (none before it arrives).
    if WB.classId and WB.classId ~= 11 and UnitIsUnit(unit,"player") and
       (WB.classId ~= 2 or table.getn((WB.talents or {}).trees or {}) == 0) then return 0,0 end
    if UnitIsUnit(unit, "player") then
      local spent = WB.talents.points_spent or (math.max(0, WB.talents.level - 9) - WB.talents.points_available)
      return math.max(0, UnitLevel("player") - 9 - spent), 0
    end
    return characterPoints(unit)
  end
  LearnTalent = function(tab, index)
    local tree = WB.talents.trees[tab]
    local t = tree and tree.talents[index]
    if t and t.rank<t.max_rank then
      if UnitCharacterPoints("player")==0 then fire("UI_ERROR_MESSAGE",WB_ERR_NO_TALENT_POINTS) return end
      WB.host("learnTalent",t.id,t.rank+1)
    end
  end
end
WB.installTalentApi = installTalentApi   -- __WBLogin: after a ReloadUI their talents are back before any reply

-- P1 has identity and a body; no class kit until its progression is implemented.
function __WBEmptyClass()
  -- Another class's kit goes (a Druid's forms, spells, bars, talents). Their own class's book and talents, which
  -- Progression published for their class (a Paladin's), stay through later identity publications.
  local own = WB.classReply and WB.classReply.class_id == WB.classId
  local ownRows = own and WB.classRows or {}
  WB.forms, WB.formIndex, WB.activeFormSpell = {}, 0, 0
  WB.comboPoints, WB.prowling, WB.manaSnapshot = 0, false, nil
  if not own then WB.classRows, WB.classLines, WB.classTabs, WB.classReply = {}, {}, {}, nil end
  local keep, book = {[6603]=true}, {6603}
  for id in pairs(ownRows) do keep[id] = true end
  for _, mount in ipairs(WB.mountCatalog or {}) do
    if mount.spell then keep[mount.spell] = mount.learned and true or false end
  end
  local function retained(id)
    if id < 0 then return true end -- native Skyrim spells, powers and shouts are not class spells
    if keep[id] ~= nil then return keep[id] end
    local info = WB.spellInfo(id)
    -- Start restores actions before the mount catalog (including its synthetic spells) arrives.
    if not info or info.mountKey then return true end
    for _, e in ipairs(info.effects or {}) do if e.aura == 78 then return true end end -- SPELL_AURA_MOUNTED
    return false
  end
  local seen = {[6603]=true}
  for _, id in ipairs(WB.spellbook or {}) do
    if retained(id) and not seen[id] then tinsert(book,id) seen[id]=true end
  end
  for _, mount in ipairs(WB.mountCatalog or {}) do
    if mount.spell and mount.learned and not seen[mount.spell] then tinsert(book,mount.spell) seen[mount.spell]=true end
  end
  WB.unlearned, WB.spellbook, WB.spellbookDirty = own and WB.unlearned or {}, book, true
  for slot, action in pairs(WB.actions or {}) do
    if action.spell and not retained(action.spell) then WB.actions[slot] = nil end
  end
  if not (WB.talents and WB.talents.class_id and WB.talents.class_id == WB.classId) then
    WB.talents = {trees={},talents={},points_available=0,points_spent=0}
  end
  if not __WBTalentApi then installTalentApi() end
  local p = WB.units and WB.units[WB.tokens.player]
  if p then p.powerType = 0 end
  fire("UPDATE_SHAPESHIFT_FORMS") fire("CHARACTER_POINTS_CHANGED",0,0)
  if WB.classId == 2 then __WBPaladinAuras() end
end

function __WBInstallTalents()
  -- Older caches omit this command. Restore its default only when neither
  -- the command nor N has a user-assigned binding.
  if not GetBindingKey("TOGGLETALENTS") and GetBindingAction("N") == "" then
    SetBinding("N", "TOGGLETALENTS")
    fire("UPDATE_BINDINGS")
  end
  if UpdateTalentButton then UpdateTalentButton() end
  WB.host("requestTalents")
end

function __WBSpellTooltips(profiles)
  WB.spellTooltips = {}
  for id, profile in pairs(profiles or {}) do WB.spellTooltips[tonumber(id)] = profile end
  if WB.classReply then WB.classReply.spell_tooltips = profiles end
  fire("SPELLS_CHANGED")
  fire("ACTIONBAR_UPDATE_USABLE")
end

-- Bridge wording for the pinned enUS client; locales may override this string.
WB_ERR_NO_TALENT_POINTS=WB_ERR_NO_TALENT_POINTS or "Not enough talent points."
function __WBTalents(reply)
  if WB.classId and WB.classId ~= 11 and WB.classId ~= 2 then return false end   -- the Druid's and the Paladin's trees
  if reply.ok == false then
    local message=reply.error or "Talent could not be learned"
    if string.find(message,"no_points",1,true) then message=WB_ERR_NO_TALENT_POINTS end
    fire("UI_ERROR_MESSAGE", message)
    return false
  end
  assert(reply.trees and reply.talents and reply.points_available, "missing talent snapshot")
  if not __WBTalentApi then installTalentApi() end
  local old = WB.talents
  WB.talents = reply
  -- 1.12 supplies both talent and skill point deltas. ChatFrame reads arg2 numerically.
  fire("CHARACTER_POINTS_CHANGED", reply.points_available - (old and old.points_available or 0), 0)
  return true
end

-- A Shrine of Kynareth is their talent master (docs/talents.md): the host fires the client's own CONFIRM_TALENT_WIPE with
-- the wipe's price in copper and the unchanged StaticPopup asks. Accept sends the offer's session and the price shown;
-- the host pays and unlearns, or answers ERR_NOT_ENOUGH_MONEY. CheckTalentMasterDist holds while the player is within reach of
-- the shrine the player prayed at (the host's nil ends it), so the popup's OnUpdate hides it as the player walks away. The recorded
-- answers remain until the first offer.
function __WBTalentWipe(session, cost)
  WB.talentWipe = session and { session = session, cost = cost } or nil
  CheckTalentMasterDist = function() return WB.talentWipe and 1 or nil end
  ConfirmTalentWipe = function()
    local w = WB.talentWipe
    if w then WB.host("talentWipe", w.session, w.cost) end
  end
  if session then fire("CONFIRM_TALENT_WIPE", cost * 10000) end
end

-- Opt-in book refresh from the service's known_spells response. This is deliberately quiet on login/load;
-- the host announces only newly_learned after it has successfully applied and persisted the snapshot.
local classBookHooks
local function hookClassBook()
  if classBookHooks then return end
  assert(SpellButton_UpdateButton and SpellButton_OnClick, "load FrameXML before publishing the class book")
  classBookHooks = true
  local update, click = SpellButton_UpdateButton, SpellButton_OnClick
  SpellButton_UpdateButton = function()
    update()
    local id = WB.bookSpell(SpellBook_GetSpellID(this:GetID()), SpellBookFrame.bookType)
    if id and WB.unlearned and WB.unlearned[id] then
      local name = this:GetName()
      getglobal(name .. "SpellName"):SetTextColor(GRAY_FONT_COLOR.r, GRAY_FONT_COLOR.g, GRAY_FONT_COLOR.b)
      this:RegisterForDrag() -- remain hoverable so the acquisition tooltip is available
    else
      this:RegisterForDrag("LeftButton")
    end
  end
  SpellButton_OnClick = function(drag)
    local id = WB.bookSpell(SpellBook_GetSpellID(this:GetID()), SpellBookFrame.bookType)
    if id and WB.unlearned and WB.unlearned[id] then this:SetChecked(false) return end
    click(drag)
  end
end

function __WBClassSpells(reply)
  if WB.classId and WB.classId ~= 11 and WB.classId ~= 2 then return end   -- the Druid's and the Paladin's books
  local previousRows = WB.classRows or {}
  local rows, unlearned, tabs, lines, ids, seen = {}, {}, {}, {}, {}, {}
  for key, line in pairs(reply.lines) do
    local tab = { name = line.name, tab = line.tab, icon = "Interface\\Icons\\INV_Misc_QuestionMark" }
    lines[tostring(key)] = tab
    if line.tab > 0 then tinsert(tabs, tab) end
  end
  local function entry(row, pending)
    local id = row.spell
    assert(type(id) == "number" and id > 0 and id == math.floor(id) and not seen[id], "invalid or duplicate class spell id")
    assert(WB.spellInfo(id), "missing class spell client data")
    assert(row.tab == 0 or lines[tostring(row.skill_line)], "missing class spell line")
    if pending then
      assert(type(row.learn_skill) == "string" and type(row.learn_skill_value) == "number" and
             row.learn_skill_value >= 0 and row.learn_skill_value < math.huge, "missing class learning threshold")
      unlearned[id] = row
    end
    rows[id], seen[id] = row, true
    local line = lines[tostring(row.skill_line)]
    if line then
      for _, l in ipairs(__WBSpellLines(id)) do if l.id == row.skill_line then line.icon = l.icon end end
    end
  end
  for _, row in ipairs(reply.spells) do entry(row, false) end
  for _, row in ipairs(reply.unlearned_spells) do entry(row, true) end
  for _, id in ipairs(reply.known_spells) do
    assert(rows[id] and not unlearned[id], "missing known class spell metadata")
    tinsert(ids, id)
  end
  -- Reconcile every page on every authoritative book publication, including quiet
  -- reloads and unlearning. Excluded item/talent ranks may be absent from the new
  -- rows entirely, so use old metadata or the client's spell family name too.
  local known, byName, replacements = {}, {}, {}
  for _, id in ipairs(reply.known_spells) do known[id] = true end
  for _, row in pairs(rows) do
    if row.chain then byName[WB.spellInfo(row.spell).name] = row.chain end
  end
  for _, row in ipairs(reply.replacements or {}) do replacements[row.old_spell] = row.new_spell end
  local function reconcile(id)
    if not id or id < 0 then return id end
    local replacement = replacements[id]
    if replacement and known[replacement] then id = replacement end
    if known[id] then return id end
    local row, class = rows[id] or previousRows[id], false
    for _, line in ipairs(__WBSpellLines(id) or {}) do if line.category == 7 then class = true end end
    if not row and not class then return id end -- General/Attack remain independent
    local info = WB.spellInfo(id)
    local chain = row and row.chain or info and byName[info.name]
    local best = chain and reply.highest_ranks and reply.highest_ranks[tostring(chain)]
    return best and known[best] and best or nil
  end
  for slot, action in pairs(WB.actions or {}) do
    if not action.item then
      local id = reconcile(action.spell)
      if id then action.spell = id else WB.actions[slot] = nil end
    end
  end
  if WB.cursor and WB.cursor.spell then
    local id = reconcile(WB.cursor.spell)
    WB.setCursor(id and {spell=id} or nil)
  end
  -- Preserve General (racials, weapon skills, Attack) and all Skyrim categories.
  for _, id in ipairs(WB.spellbook) do
    local class = WB.classRows and WB.classRows[id]
    for _, line in ipairs(id > 0 and __WBSpellLines(id) or {}) do if line.category == 7 then class = true end end
    if not class and not seen[id] then tinsert(ids, id) end
  end
  table.sort(tabs, function(a, b) return a.tab < b.tab end)
  hookClassBook()
  WB.classRows, WB.classLines, WB.classTabs = rows, lines, tabs
  WB.classReply = reply   -- reinstall the class book's FrameXML hooks after ReloadUI
  WB.unlearned, WB.unlearnedSpells = unlearned, reply.unlearned_spells
  WB.spellbook, WB.spellbookDirty = ids, true
  -- The book's answer carries the profiles of the spells it lists; the costs answer (op skyrim_costs) carries every
  -- class spell's and item use's. Add these to those: replacing them left the rest (an unlearned rank, a Use: line)
  -- on the client's own guess at Skyrim's numbers until the next costs answer.
  if reply.spell_tooltips then
    local merged = {}
    for id, profile in pairs(WB.spellTooltips or {}) do merged[id] = profile end
    for id, profile in pairs(reply.spell_tooltips) do merged[tonumber(id)] = profile end
    __WBSpellTooltips(merged)
  end
  fire("SPELLS_CHANGED")
  fire("ACTIONBAR_SLOT_CHANGED", 0)
end

function __WBLearnedSpell(id, kind)
  local info = assert(WB.spellInfo(id), "missing learned spell client data")
  local tab
  for index, spell in ipairs(WB.bookLayout().slots) do
    if spell == id and not (WB.unlearned and WB.unlearned[id]) then
      for i, line in ipairs(WB.bookLayout().tabs) do
        if index > line.offset and index <= line.offset + line.count then tab = i break end
      end
      break
    end
  end
  assert(tab, "publish the learned spell in the book before announcing it")
  -- Spell.dbc attribute 0x10 is IS_ABILITY (the client's spell/ability wording).
  kind = kind or (math.floor((info.attributes or 0) / 16) % 2 == 1 and "ability" or "spell")
  local key = ({ spell = "ERR_LEARN_SPELL_S", ability = "ERR_LEARN_ABILITY_S", recipe = "ERR_LEARN_RECIPE_S" })[kind]
  assert(key, "expected spell, ability or recipe announcement")
  local name = info.name
  if info.rank and info.rank ~= "" then name = name .. " (" .. info.rank .. ")" end
  __WBLog("CHAT_MSG_SYSTEM", key, name)
  fire("LEARNED_SPELL_IN_TAB", tab)
end

-- A silent load/new-game synchronization; level is Skyrim's actual overall level, without a level-60 cap.
function __WBPlayerLevel(level) applyPlayer({ level = level }) end

function __WBLevelUp(level, gains, delayed)
  playerLevel(level)
  local p = assert(WB.resolve("player"), "missing player")
  if level < p.level or WB.levelUpNotified == level or (level == p.level and not delayed) then return false end
  WB.levelUpNotified = level -- host may publish the exact level before measuring attribute gains
  gains = gains or {}
  local function gain(key)
    local n = gains[key] or 0
    assert(type(n) == "number" and n >= 0 and n < math.huge and n == math.floor(n), "expected nonnegative integer level gain")
    return n
  end
  local health, magicka = gain("health"), gain("magicka")
  __WBPlayerLevel(level)
  -- 1.12: level, health, mana, talent points, strength, agility, stamina, intellect, spirit.
  -- ChatFrame formats LEVEL_UP, LEVEL_UP_HEALTH[_MANA], and LEVEL_UP_STAT itself.
  -- Blizzard's handler always prints a health row (even zero). Skyrim may
  -- grant only magicka, stamina, or nothing via setlevel. Filter that one row
  -- during this synchronous event, retaining the unchanged client handler.
  local original = ChatFrame_OnEvent
  if original then
    ChatFrame_OnEvent = function(e)
      local frame, add = this, this.AddMessage
      if e == "PLAYER_LEVEL_UP" and health == 0 then
        frame.AddMessage = function(self, message, ...)
          if message == format(LEVEL_UP_HEALTH, 0) then return end
          if message == format(LEVEL_UP_HEALTH_MANA, 0, magicka) then
            message = format(LEVEL_UP_STAT, MANA, magicka)
          end
          return add(self, message, unpack(arg, 1, arg.n))
        end
      end
      original(e)
      frame.AddMessage = add
    end
  end
  -- Skyrim Stamina is an expendable resource, not WoW's health stat.
  fire("PLAYER_LEVEL_UP", level, health, magicka, 0, gain("strength"), gain("agility"), gain("wow_stamina"), gain("intellect"), gain("spirit"))
  ChatFrame_OnEvent = original
  if gain("stamina") > 0 then __WBLog("CHAT_MSG_SYSTEM", format(LEVEL_UP_STAT, SPELL_STAT2_NAME, gain("stamina"))) end
  WB.host("levelUp", level) -- host draws ClientDb::levelUpEffect() at the player's feet
  PlaySound("LEVELUPSOUND")
  return true
end

local function castState()
  fire("CURRENT_SPELL_CAST_CHANGED")
  fire("ACTIONBAR_UPDATE_STATE")
end

function __WBCastStart(spell, ms)
  local info = WB.spellInfo(spell)
  WB.casting = { spell = spell }
  fire("SPELLCAST_START", info and info.name or "", ms)
  castState()
end

function __WBCastStop()
  WB.casting = nil
  fire("SPELLCAST_STOP")
  castState()
end

function __WBCastFailed(key)
  WB.casting = nil
  fire("SPELLCAST_FAILED")
  castState()
  __WBUiError(key)
end

function __WBCastInterrupted()
  WB.casting = nil
  fire("SPELLCAST_INTERRUPTED")
  castState()
  __WBUiError("SPELL_FAILED_INTERRUPTED")
end

-- Auto-attack started or stopped (the Attack action lights while it runs).
function __WBAttacking(on)
  WB.attacking = on
  fire(on and "PLAYER_ENTER_COMBAT" or "PLAYER_LEAVE_COMBAT")
  fire("ACTIONBAR_UPDATE_STATE")
end

-- A channel (Hurricane): the bar drains over its duration. The 1.12 client names no spell on it: SPELLCAST_CHANNEL_START
-- carries "Channeling" (its CHANNELING string), which is what its bar shows.
function __WBChannelStart(spell, ms)
  WB.casting = { spell = spell, channel = true }
  fire("SPELLCAST_CHANNEL_START", ms, CHANNELING or "Channeling")
  castState()
end

-- Pushback: the cast bar is set back by ms (SPELLCAST_DELAYED); a channel's remaining time (SPELLCAST_CHANNEL_UPDATE).
function __WBCastDelayed(ms) fire("SPELLCAST_DELAYED", ms) end
function __WBChannelUpdate(ms) fire("SPELLCAST_CHANNEL_UPDATE", ms) end

function __WBChannelStop()
  WB.casting = nil
  fire("SPELLCAST_CHANNEL_STOP")
  castState()
end

-- Placing a ground spell: SpellIsTargeting answers 1 (and the action is lit) until it is placed or cancelled.
function __WBTargeting(spell)
  WB.spellTargeting = spell
  castState()
end

function __WBGcd(seconds)
  WB.gcd = { GetTime(), seconds }
  fire("BAG_UPDATE_COOLDOWN")   -- a use spell's GCD sweeps bag and character slots too
  fire("ACTIONBAR_UPDATE_COOLDOWN")
  fire("SPELL_UPDATE_COOLDOWN")
end

function __WBRefreshCooldowns()
  WB.gcd=nil
  for id in pairs(WB.cooldowns) do if id>0 then WB.cooldowns[id]=nil end end
  fire("BAG_UPDATE_COOLDOWN") fire("ACTIONBAR_UPDATE_COOLDOWN") fire("SPELL_UPDATE_COOLDOWN")
end
function __WBCooldown(spell, seconds, held)   -- held: waits for an aura to fade (SPELL_ATTR_COOLDOWN_ON_EVENT)
  WB.cooldowns[spell] = { GetTime(), seconds, held = held and true or nil }
  fire("BAG_UPDATE_COOLDOWN")
  fire("ACTIONBAR_UPDATE_COOLDOWN")
  fire("SPELL_UPDATE_COOLDOWN")
end

-- Auras the host applies (its aura table is the truth; this is what the frames show). unit: "player" or a guid.
local function auraUnit(who)
  return WB.units[who == "player" and WB.tokens.player or who]
end

function __WBAuraAdd(who, spell, harmful, seconds, tooltip)
  local u = auraUnit(who)
  if not u then return end
  local info = WB.spellInfo(spell) or {}
  local list = harmful and "debuffs" or "buffs"
  u[list] = u[list] or {}
  for i, a in ipairs(u[list]) do
    if a.spell == spell then table.remove(u[list], i) break end      -- a refresh replaces
  end
  table.insert(u[list], { spell = spell, texture = info.icon, count = 1, dispel = info.dispel,
                          tooltip = tooltip,
                          expires = seconds and seconds > 0 and GetTime() + seconds or nil,
                          untilCancelled = not seconds or seconds <= 0 })
  __WBUnitAuraChanged(who)
  if who == "player" and WB.classId == 2 then __WBPaladinAuras() end
end

-- The Skyrim spells the player knows (SkyrimSpells), as spells of their book: each by -(its Skyrim form id), with what the
-- interface asks of a spell (name, icon, cost in magicka, cast time, range, targetsUnit, harmful, description).
local function skyrimBookChanged()
  WB.skyrimSpells = {}
  local book = {}
  for _, id in ipairs(WB.spellbook) do if id > 0 then tinsert(book, id) end end
  for _, group in ipairs({ WB.skyrimSpellList or {}, WB.skyrimShouts or {}, WB.skyrimPowers or {} }) do
    for _, s in pairs(group) do
      if s.skyrimKind ~= "shout" or s.wordCount > 0 then
        assert(not WB.skyrimSpells[s.id], "duplicate Skyrim ability id")
        WB.skyrimSpells[s.id] = s
        tinsert(book, s.id)
      end
    end
  end
  WB.spellbook, WB.spellbookDirty = book, true
  -- Repair older unsigned action caches only when a published native ability
  -- identifies the wrapped ID and no real WoW spell owns the positive ID.
  for slot, action in pairs(WB.actions or {}) do
    local id=action.spell
    if type(id)=="number" and id>0 and WB.skyrimSpells[id-4294967296] and not WB.spellInfo(id) then
      action.spell=id-4294967296
      WB.host("actionChanged",slot)
    end
  end
  fire("SPELLS_CHANGED")
  if next(WB.skyrimSpells) then fire("LEARNED_SPELL_IN_TAB", GetNumSpellTabs()) end
  fire("ACTIONBAR_SLOT_CHANGED", 0)
end

local function skyrimEntry(s, kind)
  assert(type(s.id) == "number" and s.id < 0 and s.id == math.floor(s.id), "expected negative Skyrim form id")
  assert(type(s.name) == "string", "expected Skyrim ability name")
  local entry = {}
  for k, v in pairs(s) do entry[k] = v end
  entry.skyrim, entry.skyrimKind, entry.passive = true, kind, false
  entry.rank, entry.powerType, entry.gcd = s.rank or "", 0, kind == nil
  entry.icon = s.icon or "Interface\\Icons\\INV_Misc_QuestionMark"
  entry.cost = s.cost or 0
  return entry
end

function __WBSkyrimSpells(list)
  WB.skyrimSpellList = {}
  for _, s in ipairs(list) do tinsert(WB.skyrimSpellList, skyrimEntry(s)) end
  skyrimBookChanged()
end

-- One stable SHOU id, upgraded to its highest contiguous unlocked word. Learned but locked words are tooltip
-- information only; a shout with no usable word is kept out of the book. Word recovery is in seconds, like SKSE.
local function shoutWords(s, words)
  s.words, s.wordCount = {}, 0
  local contiguous = true
  for i, word in ipairs(words) do
    local w = {}
    for k, v in pairs(word) do w[k] = v end
    assert(type(w.name) == "string" and type(w.recovery) == "number" and w.recovery >= 0, "invalid shout word")
    assert(not w.unlocked or w.learned, "an unlocked shout word must be learned")
    tinsert(s.words, w)
    contiguous = contiguous and w.unlocked == true
    if contiguous then s.wordCount = i end
  end
  local word = s.words[s.wordCount]
  s.wordSpell = word and word.spell
  s.recovery = word and word.recovery * 1000 or 0 -- spellInfo uses WoW's milliseconds
  s.description = word and word.description or s.shoutDescription
  s.rank = s.wordCount .. (s.wordCount == 1 and " Word" or " Words")
end

function __WBSkyrimShouts(list)
  local shouts = {}
  for _, s in ipairs(list) do
    local entry = skyrimEntry(s, "shout")
    assert(not shouts[s.id], "duplicate shout id")
    entry.shoutDescription = s.description
    shoutWords(entry, s.words or {})
    shouts[s.id] = entry
  end
  WB.skyrimShouts = shouts
  skyrimBookChanged()
end

function __WBSkyrimShoutWords(id, words)
  local shout = assert(WB.skyrimShouts and WB.skyrimShouts[id], "unknown shout id")
  shoutWords(shout, words)
  skyrimBookChanged()
end

function __WBSkyrimPowers(list)
  local powers = {}
  for _, s in ipairs(list) do
    assert(s.kind == "greater" or s.kind == "lesser", "expected greater or lesser power")
    local entry = skyrimEntry(s, s.kind)
    assert(not powers[s.id], "duplicate power id")
    entry.rank = s.kind == "greater" and "Greater Power" or "Lesser Power"
    entry.cooldownSeconds = s.cooldownSeconds or (s.kind == "greater" and 86400 or 0)
    assert(entry.cooldownSeconds >= 0, "invalid power cooldown")
    entry.recovery = entry.cooldownSeconds * 1000
    powers[s.id] = entry
  end
  for id in pairs(WB.skyrimPowers or {}) do
    if not powers[id] then WB.cooldowns[id] = nil end
  end
  WB.skyrimPowers = powers
  skyrimBookChanged()
end

-- A successful use, or resynchronization after load/wait/time-scale changes. All shouts share recovery; powers
-- have independent timers. Seconds are on GetTime's clock, never Skyrim calendar hours. A sync restarts the sweep
-- over the remaining time, avoiding a start before the UI clock's origin (1.12 hides nonpositive starts).
local function skyrimTimer(seconds)
  assert(type(seconds) == "number" and seconds >= 0, "invalid Skyrim cooldown")
  return { GetTime(), seconds }
end
local function skyrimCooldownChanged()
  fire("ACTIONBAR_UPDATE_COOLDOWN")
  fire("SPELL_UPDATE_COOLDOWN")
end
function __WBSkyrimShoutCooldown(seconds)
  WB.skyrimShoutCooldown = skyrimTimer(seconds)
  skyrimCooldownChanged()
end
function __WBSkyrimShoutUsed(id, seconds)
  local s = assert(WB.skyrimSpells and WB.skyrimSpells[id], "unknown usable shout id")
  assert(s.skyrimKind == "shout", "expected shout id")
  __WBSkyrimShoutCooldown(seconds or s.recovery / 1000)
end
function __WBSkyrimPowerCooldown(id, seconds)
  assert(WB.skyrimPowers and WB.skyrimPowers[id], "unknown power id")
  WB.cooldowns[id] = skyrimTimer(seconds)
  skyrimCooldownChanged()
end
function __WBSkyrimPowerUsed(id, seconds)
  local s = assert(WB.skyrimPowers and WB.skyrimPowers[id], "unknown power id")
  __WBSkyrimPowerCooldown(id, seconds or s.cooldownSeconds)
end

-- Skyrim's own magic on the player (SkyrimAuras): a potion, a blessing, a disease, a Skyrim spell - an aura with Skyrim's name
-- and words. key: its Skyrim spell's form id; seconds 0: until cured or cancelled.
function __WBSkyrimAura(key, name, icon, harmful, seconds, text, dispel)
  local u = WB.units[WB.tokens.player]
  if not u then return end
  __WBSkyrimAuraRemove(key, true)
  local list = harmful and "debuffs" or "buffs"
  u[list] = u[list] or {}
  table.insert(u[list], { skyrim = key, name = name, text = text, texture = icon, count = 1, dispel = dispel,
                          expires = seconds and seconds > 0 and GetTime() + seconds or nil,
                          untilCancelled = not seconds or seconds <= 0 })
  fire("UNIT_AURA", "player")
  fire("PLAYER_AURAS_CHANGED")
end

function __WBSkyrimAuraRemove(key, quiet)
  local u = WB.units[WB.tokens.player]
  if not u then return end
  for _, list in ipairs({ "buffs", "debuffs" }) do
    for i, a in ipairs(u[list] or {}) do
      if a.skyrim == key then table.remove(u[list], i) break end
    end
  end
  if not quiet then
    fire("UNIT_AURA", "player")
    fire("PLAYER_AURAS_CHANGED")
  end
end

function __WBAuraRemove(who, spell)
  local u = auraUnit(who)
  if not u then return end
  for _, list in ipairs({ "buffs", "debuffs" }) do
    for i, a in ipairs(u[list] or {}) do
      if a.spell == spell then table.remove(u[list], i) break end
    end
  end
  __WBUnitAuraChanged(who)
  if who == "player" and WB.classId == 2 then __WBPaladinAuras() end
end

-- A save loaded or a new game: nothing cast, placed or applied carries over (the host's aura table is cleared too).
function __WBResetCombat()
  local stopped = WB.casting
  -- Native Melee::Reset drops auto-attack without publishing a stop. Notify
  -- FrameXML too, so its checked Attack action and flashing texture are reset.
  if WB.attacking then __WBAttacking(false) end
  if WB.forms then __WBForm(0) end
  WB.manaSnapshot = nil
  __WBComboPoints(0)
  WB.casting, WB.spellTargeting, WB.gcd, WB.cooldowns = nil, nil, nil, {}
  WB.skyrimShoutCooldown = nil
  for _, u in pairs(WB.units) do u.buffs, u.debuffs, u.partyAuraKey = {}, {}, nil end
  if stopped then fire(stopped.channel and "SPELLCAST_CHANNEL_STOP" or "SPELLCAST_STOP") end
  castState()
  fire("UNIT_AURA", "player")
  fire("PLAYER_AURAS_CHANGED")
  if WB.tokens.target then fire("UNIT_AURA", "target") end
  for i=1,4 do if WB.tokens["party"..i] then fire("UNIT_AURA", "party"..i) end end
  fire("ACTIONBAR_UPDATE_COOLDOWN")
  fire("SPELL_UPDATE_COOLDOWN")
end

------------------------------------------------------------------------------------------------------ Skyrim's menus
-- Interface Options as the client lays them out, less what has nothing behind it in single-player Skyrim: the mouse
-- and camera are Skyrim's; no click-to-move, assist, AFK, trades, profanity or spam filter, unit names, quest dialog
-- or tracker, loading screen tips, guild, pets, raid frames or floating combat text; Auto Self Cast and Sticky
-- Targeting are not honoured; WoW's tutorials (the user: remove the toggle) are cleared, Show Tutorials and Reset
-- Tutorials gone. Whole boxes go (Camera, Raid & Party, Combat Text); in the rest the options left close up in the
-- client's columns and each box is as tall as its rows. Hidden ones leave the panel's table, so Okay and Defaults
-- leave their settings alone. Added: Area Spells Hit Friendly and Skyrim Minimap Icons (Controls).
local HIDDEN_OPTIONS = { 1, 3, 5, 6, 13, 14, 16, 21, 22, 23, 28, 30, 38, 42, 44, 45, 46, 51, 61, 62, 66, 67, 68 }

local function minimapNativeArtOption()
  if UIOptionsFrameCheckButton72 or not BasicOptionsGeneral then return end
  local option = CreateFrame("CheckButton", "UIOptionsFrameCheckButton72", BasicOptionsGeneral, "UIOptionsCheckButtonTemplate")
  WB_MINIMAP_NATIVE_ART_TEXT = "Skyrim Minimap Icons"
  getglobal(option:GetName() .. "Text"):SetText(WB_MINIMAP_NATIVE_ART_TEXT)
  OPTION_TOOLTIP_WB_MINIMAP_NATIVE_ART = "Show Skyrim's compass icons on the minimap rim. Uncheck to show WoW icons at known positions; unmatched markers keep their Skyrim icons on the rim."
  option.tooltipText = OPTION_TOOLTIP_WB_MINIMAP_NATIVE_ART
  UIOptionsFrameCheckButtons.WB_MINIMAP_NATIVE_ART_TEXT = { index = 72, cvar = "wbMinimapNativeArt" }
end

-- WoW's area spells spare friendly people; ticked, theirs strike everyone caught in them, as Skyrim's do (each struck
-- friend an assault). The host hears the setting whenever it changes and when their interface starts.
function __WBAreaFriendly() WB.host("areaFriendly", GetCVar("wbAreaFriendly") == "1" and 1 or 0) end
local function areaFriendlyOption()
  if UIOptionsFrameCheckButton71 or not BasicOptionsGeneral then return end
  WB.cvarDefaults.wbAreaFriendly = "0"
  RegisterCVar("wbAreaFriendly", 0)
  local setCVar = SetCVar
  SetCVar = function(name, ...)
    local result = setCVar(name, ...)
    if name == "wbAreaFriendly" then __WBAreaFriendly() end
    return result
  end
  local option = CreateFrame("CheckButton", "UIOptionsFrameCheckButton71", BasicOptionsGeneral, "UIOptionsCheckButtonTemplate")
  WB_AREA_FRIENDLY_TEXT = "Area Spells Hit Friendly"
  getglobal(option:GetName() .. "Text"):SetText(WB_AREA_FRIENDLY_TEXT)
  OPTION_TOOLTIP_WB_AREA_FRIENDLY = "Your area spells also strike friendly people caught in them, as Skyrim's spells do. Each one struck is an assault."
  option.tooltipText = OPTION_TOOLTIP_WB_AREA_FRIENDLY
  UIOptionsFrameCheckButtons.WB_AREA_FRIENDLY_TEXT = { index = 71, cvar = "wbAreaFriendly" }
  __WBAreaFriendly()
end
local function interfaceOptions(keepHidden)
  local boxes = { BasicOptionsCamera, AdvancedOptionsRaid, AdvancedOptionsCombatText }
  for _, box in ipairs(boxes) do keepHidden(box) end
  areaFriendlyOption()
  minimapNativeArtOption()
  keepHidden(UIOptionsFrameResetTutorials)
  if ClearTutorials then ClearTutorials() end
  keepHidden(UIOptionsFrameSlider1) keepHidden(UIOptionsFrameClickCameraDropDown)
  local hidden = {}
  for _, n in ipairs(HIDDEN_OPTIONS) do hidden[n] = true keepHidden(getglobal("UIOptionsFrameCheckButton" .. n)) end
  for key, info in pairs(UIOptionsFrameCheckButtons) do
    local parent = getglobal("UIOptionsFrameCheckButton" .. info.index):GetParent()
    if hidden[info.index] or parent == boxes[1] or parent == boxes[2] or parent == boxes[3] then
      UIOptionsFrameCheckButtons[key] = nil
    end
  end
  UIOptionsFrameSliders = {}   -- the mouse's and the camera's
  local function rows(box, n) box:SetHeight(16 + 26 * n) end
  local function place(n, point, to, toPoint, x, y)
    local b = getglobal("UIOptionsFrameCheckButton" .. n)
    if b then b:ClearAllPoints() b:SetPoint(point, to, toPoint, x, y) end
  end
  -- Controls: Auto Loot (wowapi_loot.lua). Display: tooltips and status text | cloak, helm | buffs, party background.
  place(70, "TOPLEFT", BasicOptionsGeneral, "TOPLEFT", 10, -8)
  place(71, "TOPLEFT", BasicOptionsGeneral, "TOP", -80, -8)
  place(72, "TOPLEFT", BasicOptionsGeneral, "TOPRIGHT", -220, -8) rows(BasicOptionsGeneral, 1)
  place(31, "TOPLEFT", BasicOptionsDisplay, "TOP", -80, -8)
  place(39, "TOPLEFT", BasicOptionsDisplay, "TOPRIGHT", -220, -8)
  place(43, "TOP", UIOptionsFrameCheckButton39, "BOTTOM", 0, 0) rows(BasicOptionsDisplay, 2)
  BasicOptionsHelp:ClearAllPoints()
  BasicOptionsHelp:SetPoint("TOPLEFT", BasicOptionsDisplay, "BOTTOMLEFT", 0, -20)
  BasicOptionsHelp:SetPoint("TOPRIGHT", BasicOptionsDisplay, "BOTTOMRIGHT", 0, -20) rows(BasicOptionsHelp, 1)
  place(29, "TOPLEFT", BasicOptionsHelp, "TOPLEFT", 10, -8)   -- Show Detailed Tooltips, alone now
  -- Chat, the width of Action Bars above it, in its columns: simple chat, lock | hover delay, bubbles.
  AdvancedOptionsChat:ClearAllPoints()
  AdvancedOptionsChat:SetPoint("TOPLEFT", AdvancedOptionsActionBars, "BOTTOMLEFT", 0, -20)
  AdvancedOptionsChat:SetPoint("TOPRIGHT", AdvancedOptionsActionBars, "BOTTOMRIGHT", 0, -20)
  place(18, "TOP", AdvancedOptionsChat, "TOP", -80, -8) rows(AdvancedOptionsChat, 2)
end

-- In Skyrim the panels with nothing behind them here open Skyrim's own menu for the same purpose (docs: the plan's
-- micro menu mapping): Character and Skills stay in the client; world map -> map. The main menu keeps the client's
-- look with the rows that do something here: Skyrim Options (in place of video and sound options) and Exit Game open
-- Skyrim's system page, where its settings and Quit are; Macros (nothing runs them) and Logout (Skyrim has none) go.
-- Social opens the testing tools; Help, with nothing behind it, is hidden. The spellbook, key bindings (less the
-- rows Skyrim has no use for: __WBSkyrimKeys) and interface options stay WoW's. Run once the interface is loaded,
-- in game only: the reference renders keep the client's own panels.
function __WBSkyrimMenus()
  local function open(menu) WB.host("menu", menu) end
  local function system() HideUIPanel(GameMenuFrame) open("System") end   -- Skyrim's Esc menu
  local function keepHidden(f) if f then f:Hide() f.Show = function() end end end
  __WBSkyrimKeys() -- before the bags' own adjustment to the bindings
  __WBInstallCharacter() -- C and the micro button use the client's CharacterFrame
  __WBInstallTalents()
  __WBInstallBags() __WBInstallMerchant() -- B and the bag buttons keep the client's ContainerFrames
  __WBInstallWorldMap() -- M and the world-map micro button open Skyrim's MapMenu
  __WBInstallLoot() -- native corpses/containers keep Skyrim's transfer and crime lifecycle
  __WBInstallChatBubble() -- their own SAY/YELL over their head
  -- Skyrim has one coin: their septims show as gold alone (1 septim = 1 gold; the user: "You don't need to show
  -- copper or silver in the bags since there's only one denomination in Skyrim"), none as "0" gold.
  local player = MoneyTypeInfo and MoneyTypeInfo["PLAYER"]
  if player and not player.wbGoldOnly then
    player.wbGoldOnly, player.showSmallerCoins, player.truncateSmallCoins = true, nil, 1
    local update = MoneyFrame_Update
    MoneyFrame_Update = function(frameName, money)
      local frame = getglobal(frameName)
      if frame and frame.info == player and money == 0 then
        update(frameName, COPPER_PER_SILVER * SILVER_PER_GOLD)   -- laid out as one gold coin
        getglobal(frameName.."GoldButton"):SetText(0)
        return
      end
      update(frameName, money)
    end
  end
  -- The quest log is the client's (their Skyrim quests, Quests.cpp); Skyrim's journal stays on its own key.
  __WBInstallQuestLog() -- also restore the count format after ReloadUI
  QuestLogFrameAbandonButton:Disable()   -- Skyrim's quests cannot be abandoned: its button stays disabled
  QuestLogFrameAbandonButton.Enable = function() end
  ToggleFriendsFrame = function() __WBTestPanelToggle() end
  -- Its tooltip names it as its binding does, with the key as the client shows one (and again when bindings change).
  BINDING_NAME_TOGGLESOCIAL = "Testing Tools"
  local function socials() SocialsMicroButton.tooltipText = MicroButtonTooltipText(BINDING_NAME_TOGGLESOCIAL, "TOGGLESOCIAL") end
  socials() SocialsMicroButton:SetScript("OnEvent", socials)
  SocialsMicroButton.newbieText = "Open WoWBridge character, skills, gear, spells, travel and spawn tools."
  ToggleHelpFrame = function() end
  keepHidden(HelpMicroButton)
  Logout = function() WB.host("logout") system() end
  Quit = function() WB.host("quit") system() end
  GameMenuButtonOptions:SetText("Skyrim Options")
  GameMenuButtonOptions:SetScript("OnClick", function() PlaySound("igMainMenuOption") system() end)
  keepHidden(GameMenuButtonSoundOptions) keepHidden(GameMenuButtonMacros) keepHidden(GameMenuButtonLogout)
  GameMenuButtonUIOptions:ClearAllPoints() GameMenuButtonUIOptions:SetPoint("TOP", GameMenuButtonOptions, "BOTTOM", 0, -1)
  GameMenuButtonQuit:ClearAllPoints() GameMenuButtonQuit:SetPoint("TOP", GameMenuButtonKeybindings, "BOTTOM", 0, -1)
  GameMenuFrame:SetHeight(180)   -- the client's 246, three 22-pixel rows fewer
  interfaceOptions(keepHidden)
  __WBShowGear()   -- their Show Helm and Show Cloak, for their model
  -- Chat is shown (the user's choice: the combat log lives there), less its menu button (the speech bubble: the
  -- user's choice). WB.hideChat = true before this runs keeps its frames, tabs and buttons from showing too.
  keepHidden(ChatFrameMenuButton)
  if WB.hideChat then
    for i = 1, NUM_CHAT_WINDOWS or 7 do
      keepHidden(getglobal("ChatFrame" .. i))
      keepHidden(getglobal("ChatFrame" .. i .. "Tab"))
    end
    keepHidden(ChatFrameEditBox)
  end
  __WBFrameLocks()
  -- The requested Cat resource display follows the player frame, using the
  -- client's combo artwork, target ownership and highlight/fade handlers.
  ComboFrame:SetParent(PlayerFrame) ComboFrame:ClearAllPoints()
  ComboFrame:SetWidth(80) ComboFrame:SetHeight(16)
  ComboFrame:SetPoint("TOPLEFT",PlayerFrameManaBar,"BOTTOMLEFT",0,-3)
  for n=1,MAX_COMBO_POINTS do
    local point=getglobal("ComboPoint"..n)
    point:ClearAllPoints() point:SetWidth(12) point:SetHeight(12)
    point:SetPoint("TOPLEFT",ComboFrame,"TOPLEFT",(n-1)*16,0)
  end
  if not targetNumbersInstalled then
    targetNumbersInstalled=true
    for _,bar in ipairs({TargetFrameHealthBar,TargetFrameManaBar}) do
      local text=bar:CreateFontString(bar:GetName().."Text","OVERLAY","TextStatusBarText")
      text:SetPoint("CENTER",bar,"CENTER",0,0) text:Hide()
      SetTextStatusBarText(bar,text) bar.textLockable=1
    end
    local update=TextStatusBar_UpdateTextString
    TextStatusBar_UpdateTextString=function(bar)
      bar=bar or this
      update(bar)
      local u=WB.resolve("target")
      if u and bar and bar.TextString then
        local value,maximum
        if bar==TargetFrameHealthBar then value,maximum=u.health,u.healthMax
        elseif bar==TargetFrameManaBar then value,maximum=u.power,u.powerMax end
        if value and maximum and maximum>0 then
          bar.TextString:SetText(math.floor(value).." / "..math.floor(maximum))
        end
      end
    end
    local enter,leave=TargetFrame:GetScript("OnEnter"),TargetFrame:GetScript("OnLeave")
    TargetFrame:SetScript("OnEnter",function()
      if enter then enter() end
      for _,bar in ipairs({TargetFrameHealthBar,TargetFrameManaBar}) do
        TextStatusBar_UpdateTextString(bar) ShowTextStatusBarText(bar)
      end
    end)
    TargetFrame:SetScript("OnLeave",function()
      if leave then leave() end
      HideTextStatusBarText(TargetFrameHealthBar) HideTextStatusBarText(TargetFrameManaBar)
    end)
  end
end

-- The player's and their target's frames, locked or unlocked from their right-click menu (the user's request, as later clients
-- offer it: 1.12's unit frames never move, WoWee lets these two drag). Locked to begin with; the unlocked ones are
-- kept between sessions (WBUnlockedFrames: their names).
WBUnlockedFrames = ""
RegisterForSave("WBUnlockedFrames")
local LOCKABLE = { PlayerFrameDropDown = "PlayerFrame", TargetFrameDropDown = "TargetFrame" }
local PANEL_LOCKS={"CharacterFrame","DressUpFrame","QuestLogFrame","TalentFrame","MerchantFrame",
  "WorldMapFrame","LootFrame","ClassTrainerFrame","TaxiFrame","SpellBookFrame",
  "MinimapCluster","BuffFrame","GameMenuFrame","KeyBindingFrame","StackSplitFrame","WBTestPanel"}
for n=1,4 do
  LOCKABLE["PartyMemberFrame"..n.."DropDown"]="PartyMemberFrame"..n
  tinsert(PANEL_LOCKS,"PartyMemberFrame"..n)
end
for n=1,NUM_CONTAINER_FRAMES or 13 do tinsert(PANEL_LOCKS,"ContainerFrame"..n) end
for n=1,NUM_CHAT_WINDOWS or 7 do tinsert(PANEL_LOCKS,"ChatFrame"..n) end
local locksInstalled=false
local function unlocked(name) return string.find(" " .. WBUnlockedFrames .. " ", " " .. name .. " ", 1, true) ~= nil end
local function applyLock(name)
  local f = getglobal(name)
  if not f then return end
  f:SetMovable(unlocked(name)) f:SetClampedToScreen(true)
  if string.find(name,"^ChatFrame%d+$") then FCF_SetLocked(f,not unlocked(name) and 1 or nil) end
  if unlocked(name) then f:RegisterForDrag("LeftButton") else f:RegisterForDrag() end
  local header=rawget(f,"wbLockHeader")
  if header then if unlocked(name) then header:RegisterForDrag("LeftButton") else header:RegisterForDrag() end end
end
function __WBSetFrameUnlocked(name,value)
  local others=string.gsub(" "..WBUnlockedFrames.." "," "..name.." "," ")
  WBUnlockedFrames=string.gsub(others..(value and name or ""),"^%s*(.-)%s*$","%1")
  local f=getglobal(name) if f and not value then f:StopMovingOrSizing() end
  applyLock(name)
end
function __WBPanelLocks()
  for _,name in ipairs(PANEL_LOCKS) do
    local f=rawget(_G,name)
    if f then
      if not f.wbPanelLock then
        f.wbPanelLock=true
        f:SetScript("OnDragStart",function() if unlocked(name) then this:StartMoving() end end)
        f:SetScript("OnDragStop",function() this:StopMovingOrSizing() if unlocked(name) then this:SetUserPlaced(true) end end)
        local mouse=f:GetScript("OnMouseUp")
        f:SetScript("OnMouseUp",function()
          if arg1=="RightButton" then __WBFrameLockMenu(name)
          elseif mouse then mouse() end
        end)
        -- A transparent title region also reaches frames whose child content fills the background.
        local header=CreateFrame("Frame",name.."WBLockHeader",f)
        header:SetWidth(120) header:SetHeight(18) header:EnableMouse(true)
        -- The zero-height aura container anchors its buttons below TOP.
        -- Keep its drag/menu region below the icons so pointer hover reaches them.
        header:SetPoint("TOP",f,"TOP",0,name=="BuffFrame" and -42 or -8)
        f.wbLockHeader=header
        header:SetScript("OnDragStart",function() if unlocked(name) then f:StartMoving() end end)
        header:SetScript("OnDragStop",function() f:StopMovingOrSizing() if unlocked(name) then f:SetUserPlaced(true) end end)
        header:SetScript("OnMouseUp",function() if arg1=="RightButton" then __WBFrameLockMenu(name) end end)
      end
      applyLock(name)
    end
  end
end
function __WBFrameLockMenu(name)
  local menu=rawget(_G,"WBFrameLockDropDown")
  if not menu then menu=CreateFrame("Frame","WBFrameLockDropDown",UIParent,"UIDropDownMenuTemplate") end
  UIDropDownMenu_Initialize(menu,function()
    local info={text=unlocked(name) and "Lock Frame" or "Unlock Frame",notCheckable=1}
    info.func=function() __WBSetFrameUnlocked(name,not unlocked(name)) CloseDropDownMenus() end
    UIDropDownMenu_AddButton(info)
  end,"MENU")
  ToggleDropDownMenu(1,nil,menu,"cursor",0,0)
end
function __WBFrameLocks()
  __WBPanelLocks()
  if locksInstalled then for _,name in pairs(LOCKABLE) do applyLock(name) end return end
  locksInstalled=true
  -- Chat shares the same persisted lock as other panels; its right-click menu
  -- contains only that action, while left-click tab selection stays native.
  local tabClick=FCF_Tab_OnClick
  FCF_Tab_OnClick=function(button)
    if button=="RightButton" then __WBFrameLockMenu("ChatFrame"..this:GetID())
    else tabClick(button) end
  end
  -- Keep explicitly placed panels out of Blizzard's automatic left/center/bag layout.
  local function positions()
    local out={}
    for _,name in ipairs(PANEL_LOCKS) do
      local f=rawget(_G,name)
      if f and f:IsUserPlaced() then out[name]={f:GetLeft(),f:GetBottom()} end
    end
    return out
  end
  local function restore(saved)
    for name,p in pairs(saved) do
      local f=getglobal(name) f:ClearAllPoints()
      f:SetPoint("BOTTOMLEFT",UIParent,"BOTTOMLEFT",p[1],p[2]) f:SetUserPlaced(true)
    end
  end
  for _,name in ipairs({"SetLeftFrame","SetCenterFrame","SetDoublewideFrame","SetFullScreenFrame","updateContainerFrameAnchors"}) do
    local old=_G[name]
    _G[name]=function(...) local saved=positions() old(unpack(arg,1,arg.n)) restore(saved) end
  end
  -- Load-on-demand panels exist after LoadAddOn returns, including its ADDON_LOADED burst.
  local load=LoadAddOn
  LoadAddOn=function(...) local ok,reason=load(unpack(arg,1,arg.n)) __WBPanelLocks() return ok,reason end
  UnitPopupButtons["WB_UNLOCK_FRAME"] = { text = "Unlock Frame", dist = 0 }
  UnitPopupButtons["WB_LOCK_FRAME"] = { text = "Lock Frame", dist = 0 }
  for _, which in ipairs({ "SELF", "PET", "PARTY", "PLAYER", "RAID_TARGET_ICON" }) do
    local menu = UnitPopupMenus[which]
    local at = table.getn(menu) + (menu[table.getn(menu)] == "CANCEL" and 0 or 1)   -- above Cancel
    table.insert(menu, at, "WB_LOCK_FRAME")
    table.insert(menu, at, "WB_UNLOCK_FRAME")
  end
  local hideButtons, onClick = UnitPopup_HideButtons, UnitPopup_OnClick
  UnitPopup_HideButtons = function()
    hideButtons()
    local frame = LOCKABLE[UIDROPDOWNMENU_INIT_MENU]
    for index, value in ipairs(UnitPopupMenus[getglobal(UIDROPDOWNMENU_INIT_MENU).which]) do
      if value == "WB_UNLOCK_FRAME" then UnitPopupShown[index] = (frame and not unlocked(frame)) and 1 or 0
      elseif value == "WB_LOCK_FRAME" then UnitPopupShown[index] = (frame and unlocked(frame)) and 1 or 0 end
    end
  end
  UnitPopup_OnClick = function()
    local frame = LOCKABLE[UIDROPDOWNMENU_INIT_MENU]
    if this.value ~= "WB_UNLOCK_FRAME" and this.value ~= "WB_LOCK_FRAME" then return onClick() end
    if frame then
      __WBSetFrameUnlocked(frame,this.value=="WB_UNLOCK_FRAME")
    end
  end
  -- A creature or person targeted out of a group: 1.12 offers them only the raid icons ("Raid Target Icon", shown in a
  -- group); here their menu is the frame's lock, under their name.
  UnitPopupMenus["WB_TARGET"] = { "WB_UNLOCK_FRAME", "WB_LOCK_FRAME", "CANCEL" }
  local targetMenu = TargetFrameDropDown_Initialize
  TargetFrameDropDown_Initialize = function()
    if UnitIsPlayer("target") or GetNumPartyMembers() > 0 or GetNumRaidMembers() > 0 then return targetMenu() end
    UnitPopup_ShowMenu(TargetFrameDropDown, "WB_TARGET", "target", UnitName("target"))
  end
  TargetFrameDropDown.initialize = TargetFrameDropDown_Initialize   -- the dropdown kept the one it loaded with
  for _, name in pairs(LOCKABLE) do applyLock(name) end
end

-- Whether Esc has something of WoW's to do - UIParent.lua's ToggleGameMenu, in its order, without doing it: a popup,
-- the options or the game menu, a dropdown, a cast, spell targeting, an open window or bag, a target. Without, Esc is
-- Skyrim's: its own pause menu rather than WoW's game menu (the user's choice; the game menu itself is kept).
function __WBEscapeHasWork()
  if WB.bagHeld then return 1 end
  for i = 1, STATICPOPUP_NUMDIALOGS or 4 do
    local f = getglobal("StaticPopup" .. i)
    if f and f:IsVisible() then return 1 end
  end
  if (OptionsFrame and OptionsFrame:IsVisible()) or (GameMenuFrame and GameMenuFrame:IsVisible()) then return 1 end
  if WBCustomizeFrame and WBCustomizeFrame:IsVisible() then return 1 end   -- Escape is its Cancel
  for i = 1, UIDROPDOWNMENU_MAXLEVELS or 2 do
    local f = getglobal("DropDownList" .. i)
    if f and f:IsVisible() then return 1 end
  end
  if WB.casting or SpellIsTargeting() then return 1 end
  if GetLeftFrame() or GetCenterFrame() or GetDoublewideFrame() or GetFullScreenFrame() then return 1 end
  for i = 1, NUM_CONTAINER_FRAMES or 0 do
    local f = getglobal("ContainerFrame" .. i)
    if f and f:IsShown() then return 1 end
  end
  if UnitExists("target") then return 1 end
  return nil
end

-- The action slot holding a spell (the first), for the host's tests and anything that asks by spell rather than slot.
function __WBSlotOf(spell)
  for slot = 1, 120 do
    local a = WB.actions and WB.actions[slot]
    if a and a.spell == spell then return slot end
  end
  return nil
end

-- Stances is a bit mask: Moonkin's high bit does not make a caster spell a bear ability.
function __WBClassActionSlots(info)
  local stances = info and info.stances or 0
  local function allows(bit) return math.floor(stances / bit) % 2 == 1 end
  if allows(1) then return {74,75,76,77,78,79,80,81,82,83,84} end
  if allows(16) or allows(128) then return {98,99,100,101,102,103,104,105,106,107,108} end
  return {1,2,3,4,5,6,7,8,9,10,11,12,61,62,63,64,65,66,67,68,69,70,71,72}
end
