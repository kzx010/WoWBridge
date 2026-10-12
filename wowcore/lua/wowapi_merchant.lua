-- Skyrim barter snapshots over the unchanged 1.12 MerchantFrame. Prices are
-- Skyrim septims, represented as gold (10000 copper); native code owns trades.
local WB, MT = __WB, __WoweeFrameMT
local recorded,live = {},{}
for _,name in ipairs({"GetMerchantNumItems","GetMerchantItemInfo","GetMerchantItemLink","GetMerchantItemMaxStack",
  "GetNumBuybackItems","GetBuybackItemInfo","GetBuybackItemLink","BuyMerchantItem","BuybackItem",
  "PickupMerchantItem","CloseMerchant","CanMerchantRepair"}) do recorded[name]=_G[name] end
local oldClear,oldUnitName = ClearCursor,UnitName
local oldMerchantTip,oldBuybackTip = MT.SetMerchantItem,MT.SetBuybackItem
local function row(index,back)
  local m=WB.merchant
  return m and (back and m.buyback or m.items)[index or 0]
end
local function host(action,key,count)
  local m=WB.merchant
  if m then WB.host("merchant",action,key or "",count or 0,m.session,WB.merchantRevision) end
end
local function clearPurchase()
  if WB.merchantCursor then WB.merchantCursor=nil WB.setCursor(nil) end
end
function ClearCursor() clearPurchase() return oldClear() end
function UnitName(unit)
  if type(unit)=="string" and string.lower(unit)=="npc" and WB.merchant then return WB.merchant.name,nil end
  return oldUnitName(unit)
end
function __WBMerchant(snapshot,revision)
  if snapshot and WB.merchant and snapshot.session==WB.merchant.session and revision<(WB.merchantRevision or 0) then return end
  for name,fn in pairs(live) do _G[name]=fn end
  local previous=WB.merchant
  WB.liveMerchant=true WB.merchant=snapshot WB.merchantRevision=revision or 0
  if not snapshot then
    clearPurchase()
    if StaticPopup_Hide then StaticPopup_Hide("WB_MERCHANT_GOLD_LIMIT") end
    if previous then __WBFire("MERCHANT_CLOSED") end
    return
  end
  local changed=not previous or previous.session~=snapshot.session
  if changed then clearPurchase() end
  for _,list in ipairs({snapshot.items,snapshot.buyback}) do
    for _,item in ipairs(list) do __WBBagPrepareItem(item) end
  end
  if snapshot.money~=nil and WB.world.money~=snapshot.money then
    WB.world.money=snapshot.money __WBFire("PLAYER_MONEY")
  end
  if WB.merchantCursor then
    local found
    for _,item in ipairs(snapshot.items) do if item.key==WB.merchantCursor.key then found=item end end
    if not found then clearPurchase() end
  end
  if MerchantFrame and not MerchantFrame.wbVendorGold then
    local text=MerchantFrame:CreateFontString(nil,"OVERLAY","GameFontHighlightSmall")
    text:SetPoint("BOTTOMLEFT",MerchantFrame,"BOTTOMLEFT",26,70)
    MerchantFrame.wbVendorGold=text
  end
  if MerchantFrame then
    MerchantFrame.wbVendorGold:SetText("Merchant gold: "..snapshot.gold)
    MerchantFrame.page=math.min(MerchantFrame.page or 1,math.max(1,math.ceil(table.getn(snapshot.items)/10)))
  end
  __WBFire(changed and "MERCHANT_SHOW" or "MERCHANT_UPDATE")
end
function GetMerchantNumItems(...)
  if not WB.liveMerchant then return recorded.GetMerchantNumItems(unpack(arg,1,arg.n)) end
  return WB.merchant and table.getn(WB.merchant.items) or 0
end
local function info(item,back)
  if not item then return nil,nil,0,1,0,1 end
  return item.name,item.texture,item.price*10000*(back and item.count or 1),back and item.count or 1,item.count,1
end
function GetMerchantItemInfo(...)
  if not WB.liveMerchant then return recorded.GetMerchantItemInfo(unpack(arg,1,arg.n)) end
  if not arg[1] then error("Usage: GetMerchantItemInfo(index)",2) end
  return info(row(arg[1]))
end
function GetMerchantItemLink(...)
  if not WB.liveMerchant then return recorded.GetMerchantItemLink(unpack(arg,1,arg.n)) end
  if not arg[1] then error("Usage: GetMerchantItemLink(index)",2) end
  local i=row(arg[1]) return i and i.link
end
function GetMerchantItemMaxStack(...)
  if not WB.liveMerchant then return recorded.GetMerchantItemMaxStack(unpack(arg,1,arg.n)) end
  if not arg[1] then error("Usage: GetMerchantItemMaxStack(index)",2) end
  local i=row(arg[1]) return i and i.count or 1
end
function GetNumBuybackItems(...)
  if not WB.liveMerchant then return recorded.GetNumBuybackItems(unpack(arg,1,arg.n)) end
  return WB.merchant and table.getn(WB.merchant.buyback) or 0
end
function GetBuybackItemInfo(...)
  if not WB.liveMerchant then return recorded.GetBuybackItemInfo(unpack(arg,1,arg.n)) end
  return info(row(arg[1],true),true)
end
function GetBuybackItemLink(...)
  if not WB.liveMerchant then return recorded.GetBuybackItemLink(unpack(arg,1,arg.n)) end
  local i=row(arg[1],true) return i and i.link
end
function CanMerchantRepair(...)
  if not WB.liveMerchant then return recorded.CanMerchantRepair(unpack(arg,1,arg.n)) end
  return nil
end
function BuyMerchantItem(index,count)
  if not WB.liveMerchant then return recorded.BuyMerchantItem(index,count) end
  local i=row(index) count=count or 1
  if i and type(count)=="number" and count>0 and count==math.floor(count) then host("buy",i.key,count) end
end
function BuybackItem(index)
  if not WB.liveMerchant then return recorded.BuybackItem(index) end
  if WB.bagHeld then __WBMerchantSell(WB.bagHeld) return end
  local i=row(index,true) if i then host("buyback",i.key,i.count) end
end
function __WBMerchantSell(item)
  if not WB.merchant then return end
  local m=WB.merchant
  local offer=m.sales[item.identity or item.key]
  if not offer then __WBFire("UI_ERROR_MESSAGE","This merchant will not buy that item.") return end
  if offer.price*item.count>m.gold then
    -- Skyrim asks before accepting a sale for less than its quoted value.
    StaticPopupDialogs.WB_MERCHANT_GOLD_LIMIT={
      text="The merchant can only pay %s gold. Sell this quantity anyway?",
      button1=ACCEPT,button2=CANCEL,timeout=0,whileDead=1,hideOnEscape=1,
      OnAccept=function(request)
        if request then WB.host("merchant","sell",request.key,request.count,request.session,request.revision) end
      end,
    }
    local dialog=StaticPopup_Show("WB_MERCHANT_GOLD_LIMIT",m.gold)
    if dialog then dialog.data={key=item.key,count=item.count,session=m.session,revision=WB.merchantRevision} end
    return
  end
  host("sell",item.key,item.count)
end
function __WBMerchantPlace()
  local i=WB.merchantCursor
  if i and WB.merchant then host("buy",i.key,1) end
  clearPurchase()
end
function PickupMerchantItem(index)
  if not WB.liveMerchant then return recorded.PickupMerchantItem(index) end
  if WB.bagHeld then __WBMerchantSell(WB.bagHeld) return end
  if WB.merchantCursor then ClearCursor() return end
  local i=row(index)
  if i then WB.merchantCursor=i WB.setCursor({item=i.key,texture=i.texture}) end
end
function CloseMerchant()
  if not WB.liveMerchant then return recorded.CloseMerchant() end
  if WB.merchant then host("close") __WBMerchant(nil) end
end
function MT:SetMerchantItem(index)
  if not WB.liveMerchant then if oldMerchantTip then return oldMerchantTip(self,index) end return end
  return __WBBagTooltip(self,row(index))
end
-- Classic MerchantFrame calls this method from its normal OnEnter handler.
-- Reuse the equipped-item comparison path, including imported gear adaptations.
function MT:SetMerchantCompareItem(index, compareIndex)
  self:ClearLines()
  local link
  if WB.liveMerchant then
    local item=row(index)
    link=item and item.link
  else
    link=GetMerchantItemLink(index)
  end
  local shown=link and self:SetHyperlinkCompareItem(link,compareIndex)
  if not shown then self:Hide() end
  return shown
end
function MT:SetBuybackItem(index)
  if not WB.liveMerchant then if oldBuybackTip then return oldBuybackTip(self,index) end return end
  return __WBBagTooltip(self,row(index,true))
end
function __WBInstallMerchant()
  if WB.merchantInstalled then return end WB.merchantInstalled=true
  if WB.merchant then
    local snapshot,revision=WB.merchant,WB.merchantRevision
    WB.merchant=nil __WBMerchant(snapshot,revision)
  end
end

-- Keep captured native closures themselves until the first live session, including error stack locations.
for name,fn in pairs(recorded) do live[name]=_G[name] _G[name]=fn end
