-- Epic ground mounts are freely available; only the preferred choice persists.
local WB=__WB
WB.mountCatalog=WB.mountCatalog or {}
WB.mountState=WB.mountState or {}
local function installBook()
  local seen={}
  for _,id in ipairs(WB.spellbook or {}) do seen[id]=true end
  for _,m in ipairs(WB.mountCatalog) do
    if m.spell and WB.spellInfo(m.spell) and not seen[m.spell] then
      tinsert(WB.spellbook,m.spell) seen[m.spell]=true
    end
  end
  WB.spellbookDirty=true
  __WBFire("SPELLS_CHANGED") __WBFire("ACTIONBAR_SLOT_CHANGED",0)
end
function __WBMountCatalog(rows)
  WB.mountCatalog=rows installBook()
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
      for _,aura in ipairs(player.buffs or {}) do if aura.spell==active.spell then aura.mount=true aura.name=active.name end end
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
function __WBInstallMounts()
-- Mount choice belongs to the Skyrim character, alongside its action bars.
-- Account preferences and tutorial flags keep their existing shared scope.
function __WBCharacterVariablesText()
  if type(WBPreferredMount)=="string" then return "WBPreferredMount = "..string.format("%q",WBPreferredMount).."\n" end
  return "WBPreferredMount = nil\n"
end
installBook()
SLASH_WBMOUNT1="/mount"
SlashCmdList.WBMOUNT=function(key)
  key=string.gsub(key or "","^%s*(.-)%s*$","%1")
  key=string.lower(string.gsub(key,"%s+","_"))
  if key=="" then
    local preferred="swift_frostsaber"
    for _,m in ipairs(WB.mountCatalog) do if m.key==WBPreferredMount then preferred=m.key end end
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
