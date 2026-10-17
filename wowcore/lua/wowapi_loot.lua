-- Skyrim corpses/containers in the client's actual 1.12 LootFrame. Recorded answers stay intact until live loot.
local WB, MT = __WB, __WoweeFrameMT
BINDING_HEADER_WOWBRIDGE = "WoWBridge"
BINDING_NAME_WBLOOTALL = "Loot All"
BINDING_NAME_WBTOGGLEAUTOLOOT = "Toggle Auto Loot"
local recorded, live = {}, {}
for _, name in ipairs({"GetNumLootItems", "GetLootSlotInfo", "GetLootSlotLink", "LootSlotIsCoin", "LootSlotIsItem",
  "LootSlot", "CloseLoot", "IsFishingLoot"}) do recorded[name] = _G[name] end
local oldTip = MT.SetLootItem
local function itemAt(slot) return WB.lootSession and WB.lootItems and WB.lootItems[slot] end
local function redraw()
  if LootFrame and LootFrame:IsShown() then
    LootFrame.numLootItems = WB.lootSlots or 0
    LootFrame.page = math.max(1, math.min(LootFrame.page or 1, math.ceil(math.max(1,WB.lootSlots)/
      (WB.lootSlots>LOOTFRAME_NUMBUTTONS and LOOTFRAME_NUMBUTTONS-1 or LOOTFRAME_NUMBUTTONS))))
    LootFrame_Update()
  end
end

function __WBLoot(session, list, opening, shift, canDeposit)
  __WBInstallLoot()
  for name, fn in pairs(live) do _G[name] = fn end
  local old = WB.lootSession==session and WB.lootItems or {}
  WB.lootCanDeposit=canDeposit==true
  WB.lootDepositPending={}
  WB.liveLoot, WB.lootSession, WB.lootItems, WB.lootSlots = true, session, {}, 0
  for _, item in ipairs(list) do
    __WBBagPrepareItem(item)
    if item.form==15 then item.coin=true end -- septims are gold alone, exactly as in their backpack
    WB.lootItems[item.slot] = item
    WB.lootSlots = math.max(WB.lootSlots,item.slot)
  end
  -- Stable slots until close: queued clicks cannot shift onto another item after a removal.
  if not opening then
    for slot, item in pairs(old) do
      if not WB.lootItems[slot] then __WBFire("LOOT_SLOT_CLEARED",slot) end
    end
  end
  WB.lootPending = {} -- snapshots acknowledge/refuse requested transfers
  if opening then
    WB.lootClosing=nil
    __WBFire("LOOT_OPENED")
    -- Shift inverts the persistent auto-loot setting, as the client's activation gesture does.
    if (GetCVar("autoLootDefault")=="1") ~= (shift==true) then __WBLootAll() end
  else redraw() end
end
function __WBLootClosed(session)
  if WB.lootSession~=session then return end
  WB.lootSession, WB.lootItems, WB.lootSlots, WB.lootPending, WB.lootClosing = nil, {}, 0, {}, nil
  WB.lootCanDeposit,WB.lootDepositPending=nil,{}
  __WBFire("LOOT_CLOSED")
end
function GetNumLootItems() return WB.lootSlots or 0 end
function GetLootSlotInfo(slot)
  if not slot then error("Usage: GetLootSlotInfo(slot)",2) end
  local i=itemAt(slot)
  if not i then return nil,"",0,0 end
  local label=i.coin and (i.count.." Gold") or i.name
  if i.steal then label="|cffff2020Steal|r "..label end
  return i.texture,label,i.coin and 1 or i.count,i.quality
end
function GetLootSlotLink(slot)
  if not slot then error("Usage: GetLootSlotLink(slot)",2) end
  local i=itemAt(slot) return i and not i.coin and i.link or nil
end
function LootSlotIsCoin(slot)
  if not slot then error("Usage: LootSlotIsCoin(slot)",2) end
  local i=itemAt(slot) return i and i.coin and 1 or nil
end
function LootSlotIsItem(slot)
  if not slot then error("Usage: LootSlotIsItem(slot)",2) end
  local i=itemAt(slot) return i and not i.coin and 1 or nil
end
function IsFishingLoot() return nil end
function LootSlot(slot)
  local i=itemAt(slot)
  if not i or WB.lootClosing or WB.lootPending[i.key] then return end
  WB.lootPending[i.key]=true
  WB.host("lootTake",WB.lootSession,i.key,i.count)
end
function __WBLootDeposit(item)
  if not item or not WB.lootSession or not WB.lootCanDeposit or WB.lootClosing or WB.bagEquipSlot then return end
  local count=item.bagCount or item.count
  if not count or count<1 or WB.lootDepositPending[item.key] then return end
  WB.lootDepositPending[item.key]=true
  WB.host("lootDeposit",WB.lootSession,item.key,count)
end
local function receiveBag()
  if WB.bagHeld and WB.lootCanDeposit then __WBLootDeposit(WB.bagHeld) return true end
end
function __WBLootAll()
  for slot=1,WB.lootSlots or 0 do LootSlot(slot) end
end
function __WBToggleAutoLoot()
  local enabled=GetCVar("autoLootDefault")~="1"
  SetCVar("autoLootDefault",enabled and 1 or 0)
  if UIOptionsFrameCheckButton70 then UIOptionsFrameCheckButton70:SetChecked(enabled and 1 or nil) end
  if UIErrorsFrame then
    local color=ChatTypeInfo.SYSTEM
    UIErrorsFrame:AddMessage(enabled and "Auto Loot enabled." or "Auto Loot disabled.",color.r,color.g,color.b)
  end
end
function CloseLoot()
  if not WB.lootSession or WB.lootClosing then return end
  WB.lootClosing=true
  WB.host("lootClose",WB.lootSession)
end
function MT:SetLootItem(slot)
  if not WB.liveLoot then return oldTip(self,slot) end
  local i=itemAt(slot)
  __WBBagTooltip(self,i)
  if i and i.steal then self:AddLine("Steal - taking this item is theft.",1,0.125,0.125,1) self:Show() end
end
function __WBInstallLoot()
  if WB.lootInstalled or not LootFrame then return end
  WB.lootInstalled=true
  __WoweeBindingScripts.WBLOOTALL=function()
    if WB.lootSession and LootFrame:IsShown() then __WBLootAll() end
  end
  __WoweeBindingScripts.WBTOGGLEAUTOLOOT=__WBToggleAutoLoot
  WB.cvarDefaults.autoLootDefault="0"
  RegisterCVar("autoLootDefault",0)
  if BasicOptionsGeneral then
    WB_AUTO_LOOT_TEXT="Auto Loot"
    local option=CreateFrame("CheckButton","UIOptionsFrameCheckButton70",BasicOptionsGeneral,"UIOptionsCheckButtonTemplate")
    option:SetPoint("TOP",UIOptionsFrameCheckButton61,"BOTTOM",0,0)
    getglobal(option:GetName().."Text"):SetText(WB_AUTO_LOOT_TEXT)
    OPTION_TOOLTIP_WB_AUTO_LOOT="Take every item when opening loot. Shift reverses this setting, including when stealing."
    option.tooltipText=OPTION_TOOLTIP_WB_AUTO_LOOT
    UIOptionsFrameCheckButtons.WB_AUTO_LOOT_TEXT={index=70,cvar="autoLootDefault"}
  end
  local click=LootFrameItem_OnClick
  LootFrameItem_OnClick=function(button)
    if not WB.lootSession then return click(button) end
    if receiveBag() then return end
    if IsControlKeyDown() or ChatFrameEditBox:IsVisible() and IsShiftKeyDown() then return click(button) end
    if IsShiftKeyDown() then __WBLootAll() else LootSlot(this.slot) end
  end
  LootFrame:SetScript("OnReceiveDrag",receiveBag)
  local mouseUp=LootFrame:GetScript("OnMouseUp")
  LootFrame:SetScript("OnMouseUp",function()
    if arg1=="LeftButton" and receiveBag() then return end
    if mouseUp then mouseUp() end
  end)
  -- Blizzard's hover uses a three-slot page offset even on a four-item first page; use its assigned slot.
  for n=1,LOOTFRAME_NUMBUTTONS do
    local button=getglobal("LootButton"..n)
    -- The native LootButton method assigns a server loot slot; the portable engine has no server object.
    button.SetSlot=function(self,slot) self.slot=slot end
    button:SetScript("OnReceiveDrag",receiveBag)
    local enter=button:GetScript("OnEnter")
    button:SetScript("OnEnter",function()
      if not WB.lootSession then return enter() end
      if itemAt(this.slot) then
        GameTooltip:SetOwner(this,"ANCHOR_RIGHT") GameTooltip:SetLootItem(this.slot) CursorUpdate()
      end
    end)
  end
  -- ReloadUI rebuilds widgets while the native backend stays open. Restore once, without auto-looting again.
  if WB.lootSession then
    local list={} for _,i in pairs(WB.lootItems or {}) do tinsert(list,i) end
    __WBLoot(WB.lootSession,list,false,false,WB.lootCanDeposit) __WBFire("LOOT_OPENED")
  end
end
for name, fn in pairs(recorded) do live[name]=_G[name] _G[name]=fn end
