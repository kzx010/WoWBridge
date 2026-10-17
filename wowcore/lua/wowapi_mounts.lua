-- Epic ground and flying mounts, learned from their reins (Mounts.cpp marks the catalog's learned rows and keeps
-- them with their save); only the preferred choice persists here.
local WB=__WB
WB.mountCatalog=WB.mountCatalog or {}
WB.mountState=WB.mountState or {}
WB.syntheticSpells=WB.syntheticSpells or {}
-- A catalog mount the 1.12 client has no spell for (the TBC-style flyers) gets the spell its swift ground
-- mounts are: a costless, self-only General spell with their cast and flags (Spell.dbc 23221's attributes,
-- 0x8000 outdoors only and 0x10000 not while shapeshifted among them), named, iconed and described by its row.
local function mountSpell(m)
  return {id=m.spell,name=m.name,rank="",icon=m.icon,passive=false,castTime=m.cast_ms or 0,rangeMin=0,rangeMax=0,
    rangeIndex=1,cost=0,costPct=0,powerType=0,duration=-1,recovery=0,categoryRecovery=0,gcd=false,targetsUnit=false,
    school=0,attributes=0x10118110,attributesEx=0,attributesExB=0,spellLevel=1,baseLevel=0,maxLevel=0,procCharges=0,
    procChance=101,stances=0,stancesNot=0,effects={},reagents={},requiresForms={},description=m.description or "",
    auraDescription="",mountKey=m.key}
end
-- Their General tab lists the mounts the player has learned, and no other. A class mount (the Paladin's Warhorse and
-- Charger) is a class spell their class book already places.
local function installBook()
  local mounts,seen,book={},{},{}
  for _,m in ipairs(WB.mountCatalog) do if m.spell and not m.class then mounts[m.spell]=m.learned and true or false end end
  for _,id in ipairs(WB.spellbook or {}) do
    if mounts[id]~=false and not seen[id] then tinsert(book,id) seen[id]=true end
  end
  for _,m in ipairs(WB.mountCatalog) do
    if m.spell and m.learned and not m.class and WB.spellInfo(m.spell) and not seen[m.spell] then
      tinsert(book,m.spell) seen[m.spell]=true
    end
  end
  WB.spellbook=book
  WB.spellbookDirty=true
  __WBFire("SPELLS_CHANGED") __WBFire("ACTIONBAR_SLOT_CHANGED",0)
end
function __WBMountCatalog(rows)
  for id,s in pairs(WB.syntheticSpells) do if s.mountKey then WB.syntheticSpells[id]=nil end end
  for _,m in ipairs(rows) do
    if m.spell and not WB.spellInfo(m.spell) then WB.syntheticSpells[m.spell]=mountSpell(m) end
  end
  WB.mountCatalog=rows installBook()
  if __WBTestPanelRefresh then __WBTestPanelRefresh() end
end
function __WBMountState(state)
  local before=WB.mountState.active or WB.mountState.pending
  WB.mountState=state or {}
  local after=WB.mountState.active or WB.mountState.pending
  local player=WB.resolve("player")
  if player then
    local active=WB.mountState.active
    local found=false
    for n=table.getn(player.buffs or {}),1,-1 do
      local aura=player.buffs[n]
      if aura.mount then
        if active and aura.spell==active.spell then found=true
        else table.remove(player.buffs,n) __WBUnitAuraChanged("player") end
      end
    end
    if active and active.spell and not found then
      __WBAuraAdd("player",active.spell,false,0)
      for _,aura in ipairs(player.buffs or {}) do
        if aura.spell==active.spell then aura.mount=true aura.name=active.name aura.flight=active.flying and active.flight_speed_percent end
      end
    end
  end
  if (before and before.spell)~=(after and after.spell) then
    __WBFire("ACTIONBAR_UPDATE_STATE") __WBFire("ACTIONBAR_UPDATE_USABLE")
  end
  if WB.mountState.active then WBPreferredMount=WB.mountState.active.key end
  if WBTestPanelStatus and WB.testPanelResult and WB.testPanelResult.command=="mount" then
    local m=WB.mountState
    WBTestPanelStatus:SetText(m.pending and ("Preparing "..m.pending.name.."...") or
      m.active and ("Mounted: "..m.active.name) or "Dismounted.")
  end
end
function IsMounted() return WB.mountState.active and 1 or nil end
function Dismount() WB.host("testCommand","mount","none","") end
-- Mount choice belongs to the Skyrim character, alongside its action bars.
-- Account preferences and tutorial flags keep their existing shared scope.
WB.saveForCharacter("WBPreferredMount")
function __WBInstallMounts()
installBook()
SLASH_WBMOUNT1="/mount"
SlashCmdList.WBMOUNT=function(key)
  key=string.gsub(key or "","^%s*(.-)%s*$","%1")
  key=string.lower(string.gsub(key,"%s+","_"))
  if key=="" then   -- their preferred mount, else the first the player has learned
    local preferred
    for _,m in ipairs(WB.mountCatalog) do
      if m.learned and (m.key==WBPreferredMount or not preferred) then preferred=m.key end
    end
    preferred=preferred or "swift_frostsaber"
    key=IsMounted() and "none" or preferred
  end
  WB.host("testCommand","mount",key,"")
end
end
local originalCurrent=IsCurrentAction
function IsCurrentAction(slot)
  local a=WB.actions and WB.actions[slot]
  local m=WB.mountState.active or WB.mountState.pending
  if a and m and a.spell==m.spell then return 1 end
  return originalCurrent(slot)
end
