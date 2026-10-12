-- Live CharacterFrame adapters. Blizzard owns the panel, tabs, empty-slot art and rotation buttons.
local WB = __WB
local skillNames = {"One-Handed","Two-Handed","Archery","Block","Smithing","Heavy Armor","Light Armor",
  "Pickpocket","Lockpicking","Sneak","Alchemy","Speech","Alteration","Conjuration","Destruction",
  "Illusion","Restoration","Enchanting"}
local recorded,live = {},{}
for _,name in ipairs({"GetNumSkillLines","GetSkillLineInfo","GetSelectedSkill","SetSelectedSkill",
  "ExpandSkillHeader","CollapseSkillHeader","GetNumFactions","GetFactionInfo","GetSelectedFaction",
  "GetWatchedFactionInfo","PickupInventoryItem","UseInventoryItem","CursorCanGoInSlot","AutoEquipCursorItem",
  "IsInventoryItemLocked","UnitHasRelicSlot"}) do recorded[name]=_G[name] end
local function skills() return WB.characterInstalled and WB.units[WB.tokens.player].skyrim end
local installed=false
function __WBInstallCharacter()
  for name,fn in pairs(live) do _G[name]=fn end
  if not installed then
    installed=true
    -- Remove unsupported tabs and their toggle destinations; preserve Skills' client ID.
    CharacterFrameTab3:Hide() CharacterFrameTab5:Hide()
    CharacterFrameTab4:ClearAllPoints()
    CharacterFrameTab4:SetPoint("LEFT",CharacterFrameTab1,"RIGHT",-16,0)
    local showSub=CharacterFrame_ShowSubFrame
    CharacterFrame_ShowSubFrame=function(name)
      if name=="HonorFrame" or name=="ReputationFrame" then name="PaperDollFrame" end
      showSub(name)
      CharacterFrameTab3:Hide() CharacterFrameTab5:Hide()
    end
    for _,name in ipairs({"PaperDollFrame_SetStats","PaperDollFrame_SetArmor","PaperDollFrame_SetResistances",
        "PaperDollFrame_SetDamage","PaperDollFrame_SetAttackBothHands","PaperDollFrame_SetAttackPower",
        "PaperDollFrame_SetRangedAttack","PaperDollFrame_SetRangedDamage","PaperDollFrame_SetRangedAttackPower"}) do
      local old=_G[name]
      _G[name]=function(...) if not __WBCharacterStats() then return old(unpack(arg,1,arg.n)) end end
    end
    -- DressUpModel's XML scale doubles the inherited button size/offsets. Keep its model
    -- in normal interface units so its 35px buttons and frame-relative anchors match 1.12.
    DressUpModel:SetScale(1)
    DressUpModel:SetFrameLevel(DressUpFrame:GetFrameLevel()+2)
    local toggle=ToggleCharacter
    ToggleCharacter=function(tab)
      if tab=="HonorFrame" or tab=="ReputationFrame" then tab="PaperDollFrame" end
      toggle(tab)
      -- The host does not dispatch a child OnShow when only its parent became visible.
      if CharacterFrame:IsVisible() and PaperDollFrame:IsVisible() then PaperDollFrame_OnShow() end
    end
  end
  WB.characterInstalled=true
  __WBFire("SKILL_LINES_CHANGED") __WBFire("UPDATE_FACTION")
end
-- The paper doll presents Skyrim actor values. Combat service values remain available
-- to the spell/combat APIs; no WoW attack rating or attribute conversions leak into this pane.
function __WBCharacterStats()
  local p=WB.units[WB.tokens.player]
  local s=WB.characterInstalled and p and p.nativeSheet
  if not s then return false end
  local function number(n) return string.format("%.0f",n or 0) end
  local function pool(current,maximum) return number(current).." / "..number(maximum) end
  local function row(name,label,text,description)
    local f=getglobal(name)
    f:SetWidth((string.find(name,"CharacterStatFrame",1,true) or name=="CharacterArmorFrame") and 110 or 99)
    getglobal(name.."Label"):SetText(label..":")
    getglobal(name.."StatText"):SetText(text)
    f.tooltip=label.." "..text f.tooltipSubtext=description
    f:SetScript("OnEnter",function()
      GameTooltip:SetOwner(this,"ANCHOR_RIGHT") GameTooltip:SetText(this.tooltip)
      GameTooltip:AddLine(this.tooltipSubtext,1,1,1,1) GameTooltip:Show()
    end)
  end
  row("CharacterStatFrame1","Health",number(s.healthMax),"Current / maximum Health: "..pool(s.health,s.healthMax))
  row("CharacterStatFrame2","Magicka",number(s.magickaMax),"Current / maximum Magicka: "..pool(s.magicka,s.magickaMax).." (also while shapeshifted)")
  row("CharacterStatFrame3","Stamina",number(s.staminaMax),"Current / maximum Stamina: "..pool(s.stamina,s.staminaMax))
  row("CharacterStatFrame4","Weight",pool(s.weight,s.carry),"Skyrim's carried weight and carrying capacity.")
  row("CharacterStatFrame5","Disease",number(s.disease).."%","Skyrim's Resist Disease actor value.")
  row("CharacterArmorFrame","Armor",number(s.armor),"Skyrim's armor rating, including active effects.")
  row("CharacterAttackFrame","Damage",number(s.damage),"Skyrim's inventory damage for the right-hand weapon, including skill and tempering; zero without a weapon.")
  row("CharacterAttackPowerFrame","Off Hand",number(s.offDamage),"Skyrim's inventory damage for the left-hand weapon; zero for a spell, shield or empty hand.")
  row("CharacterDamageFrame","Magic Resist",number(s.magic).."%","Skyrim's Resist Magic actor value.")
  row("CharacterRangedAttackFrame","Poison Resist",number(s.poison).."%","Skyrim's Poison Resist actor value.")
  CharacterRangedAttackPowerFrame:Hide() CharacterRangedDamageFrame:Hide()
  local res={{"fire","Fire","Spell_Fire_Fire"},{"frost","Frost","Spell_Frost_Frost"},
    {"shock","Shock","Spell_Nature_Lightning"},{"magic","Magic","Spell_Holy_MagicalSentry"},
    {"poison","Poison","Spell_Nature_CorrosiveBreath"}}
  for n,r in ipairs(res) do
    local f=getglobal("MagicResFrame"..n)
    if not f.wbIcon then
      f.wbIcon=f:CreateTexture(nil,"BACKGROUND") f.wbIcon:SetAllPoints(f)
      getglobal("MagicResText"..n):SetDrawLayer("OVERLAY")
    end
    f.wbIcon:SetTexture("Interface\\Icons\\"..r[3])
    getglobal("MagicResText"..n):SetText(number(s[r[1]]).."%")
    f.tooltip=r[2].." Resistance "..number(s[r[1]]).."%"
    f.tooltipSubtext="Skyrim's raw resistance actor value; combat applies its own caps."
  end
  return true
end
function GetNumSkillLines()
  if not skills() then return recorded.GetNumSkillLines() end
  return WB.skillCollapsed and 1 or table.getn(skillNames)+1
end
function GetSkillLineInfo(...)
  if not skills() then return recorded.GetSkillLineInfo(unpack(arg,1,arg.n)) end
  local index=arg[1]
  if index==nil then error("Usage: GetSkillLineInfo(index)",2) end
  if index==1 then return "Skyrim",1,not WB.skillCollapsed and 1 or nil,0,0,0,0,nil,nil,nil,0,0,"" end
  local name = not WB.skillCollapsed and skillNames[index-1]
  if not name then return "",nil,nil,0,0,0,0,nil,nil,nil,0,0,"" end
  return name,nil,nil,skills()[name] or 0,0,0,100,nil,nil,nil,0,0,""
end
function GetSelectedSkill() if skills() then return WB.selectedSkill or 0 end return recorded.GetSelectedSkill() end
function SetSelectedSkill(index)
  if skills() then WB.selectedSkill=index and index>1 and index<=GetNumSkillLines() and index or 0
  else recorded.SetSelectedSkill(index) end
end
function ExpandSkillHeader(index)
  if not skills() then return recorded.ExpandSkillHeader(index) end
  if index==0 or index==1 then WB.skillCollapsed=nil __WBFire("SKILL_LINES_CHANGED") end
end
function CollapseSkillHeader(index)
  if not skills() then return recorded.CollapseSkillHeader(index) end
  if index==0 or index==1 then WB.skillCollapsed=true WB.selectedSkill=0 __WBFire("SKILL_LINES_CHANGED") end
end
function GetNumFactions() if WB.characterInstalled then return 0 end return recorded.GetNumFactions() end
function GetFactionInfo(...) if not WB.characterInstalled then return recorded.GetFactionInfo(unpack(arg,1,arg.n)) end end
function GetSelectedFaction() if WB.characterInstalled then return 0 end return recorded.GetSelectedFaction() end
function GetWatchedFactionInfo() if not WB.characterInstalled then return recorded.GetWatchedFactionInfo() end end
local nativeSlots={head=1,amulet=2,shoulders=3,body=5,waist=6,legs=7,feet=8,wrists=9,hands=10,ring=11,back=15,shield=17}
local function fits(item,slot)
  if not item then return false end
  local w=item.wow
  if w then
    local t=w.InventoryType
    if t==13 then return slot==16 or slot==17 end
    local slots={[1]=1,[2]=2,[3]=3,[4]=4,[5]=5,[6]=6,[7]=7,[8]=8,[9]=9,[10]=10,
      [11]=11,[12]=13,[14]=17,[15]=18,[16]=15,[17]=16,[19]=19,[20]=5,[21]=16,[22]=17,[23]=17,[28]=18}
    return slots[t]==slot or t==11 and slot==12 or t==12 and slot==14
  end
  if item.armorSlot then return nativeSlots[item.armorSlot]==slot or item.armorSlot=="ring" and slot==12 end
  if item.kind=="Ammo" then return slot==0 end
  if item.weapon then
    if item.weapon==7 or item.weapon==9 then return slot==18 end
    return slot==16 or slot==17 and (item.weapon<5 or item.weapon==8)
  end
  return false
end
function CursorCanGoInSlot(slot)
  if WB.liveInventory then return fits(WB.bagHeld,slot) and 1 or nil end
  return recorded.CursorCanGoInSlot(slot)
end
local function equip(slot)
  if not fits(WB.bagHeld,slot) then return end
  __WBBagEquipment("equip",slot)
end
function PickupInventoryItem(slot)
  if not WB.liveInventory then return recorded.PickupInventoryItem(slot) end
  if WB.bagHeld then equip(slot) else __WBBagEquipment("undress",slot) end
end
function UseInventoryItem(slot)
  if not WB.liveInventory then return recorded.UseInventoryItem(slot) end
  if WB.bagHeld then ClearCursor() else __WBBagEquipment("unequip",slot) end
end
function AutoEquipCursorItem()
  if not WB.liveInventory then return recorded.AutoEquipCursorItem() end
  for slot=0,19 do if fits(WB.bagHeld,slot) then equip(slot) return end end
end
function IsInventoryItemLocked(...)
  local slot=arg[1]
  if WB.liveInventory then return WB.bagHeld and WB.equipment[slot]==WB.bagHeld and 1 or nil end
  return recorded.IsInventoryItemLocked(unpack(arg,1,arg.n))
end
-- Rendering inputs for native hosts; the plugin supplies its already assembled Avatar look instead.
function __WBCharacterLook(appearance,worn)
  if __WBPlayerLook then __WBPlayerLook(appearance,worn) end
  if CharacterModelFrame then CharacterModelFrame:SetUnit("player") end
end

function UnitHasRelicSlot(unit)
  if WB.liveInventory and unit=="player" then
    local i=WB.equipment[18]
    if i and (i.weapon==7 or i.weapon==9) then return nil end
  end
  return recorded.UnitHasRelicSlot(unit)
end

-- Keep the recorded closures themselves until live mode, including exact usage-error stack locations.
for name in pairs(recorded) do live[name]=_G[name] _G[name]=recorded[name] end
