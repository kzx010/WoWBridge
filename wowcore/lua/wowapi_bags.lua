-- Live Skyrim stacks in the unchanged 1.12 ContainerFrames. Until __WBInventory arrives every recorded answer
-- (including zero-result empty slots and usage errors) remains the client's answer.
local WB, icons, MT = __WB, __WBBagIcons, __WoweeFrameMT
local recorded, live = {}, {}
for _, name in ipairs({"GetContainerNumSlots", "GetContainerItemInfo", "GetContainerItemLink", "GetBagName",
  "GetContainerItemCooldown", "PickupContainerItem", "UseContainerItem", "GetInventoryItemTexture",
  "PutItemInBackpack", "PutItemInBag", "SplitContainerItem", "ContainerIDToInventoryID", "SetBagPortaitTexture",
  "GetInventoryItemLink", "GetInventoryItemCount", "GetInventoryItemCooldown", "GetItemInfo", "IsUsableItem"}) do recorded[name] = _G[name] end
local oldBagTip, oldInvTip, oldHyperlink, oldCompare, oldClear = MT.SetBagItem, MT.SetInventoryItem, MT.SetHyperlink, MT.SetHyperlinkCompareItem, ClearCursor
local function path(icon) return "Interface\\Icons\\" .. icon end
local function valid(bag, slot) return type(bag)=="number" and bag>=0 and bag<=4 and math.floor(bag)==bag and
  (not slot or type(slot)=="number" and slot>=1 and slot<=16 and math.floor(slot)==slot) end
local function position(bag, slot) return (WB.bagPage-1)*80 + bag*16 + slot end
local function itemAt(bag, slot)
  local i=valid(bag,slot) and slot and WB.bagItems[position(bag,slot)]
  return i and (i.bagCount or i.count)>0 and i or nil
end
function __WBBagIcon(item)
  if item.texture then return item.texture end
  local icon
  if item.weapon then icon = icons.weapon[item.weapon]
  elseif item.armorSlot then
    local row = icons.armor[item.armorSlot] or icons.armor.body
    icon = row[(item.armorType or 0)+1]
  elseif item.potion then icon = icons.potion[item.potion] or icons.potion.default end
  if not icon then
    for _, keyword in ipairs(item.keywords or {}) do
      if icons.keywords[keyword] then icon = icons.keywords[keyword] break end
    end
  end
  icon = icon or (item.form==15 and icons.gold) or (item.form==10 and icons.lockpick) or icons.form[item.kind] or icons.default
  return path(icon)
end
local function rememberClientItems()
  WB.wowBagItems = WB.wowBagItems or {}
  local function remember(i)
    local id = i.link and tonumber(string.match(i.link, "item:(%d+):"))
    if id then WB.wowBagItems[id] = i end
  end
  for _, i in pairs(WB.equipment or {}) do remember(i) end
  for _, bag in pairs(WB.bags or {}) do for slot, i in pairs(bag) do if type(i)=="table" then remember(i) end end end
end
-- Older packaged gear already has the captured client tooltips; new exports also carry item_template data.
-- Build those with the client vocabulary; instance crafting effects follow the base WoW stats.
local function wowTooltip(i)
  local w=i.wow
  if not w then
    if not i.tooltip then return end
    -- Old gear packages carry captured client rows instead of item_template.
    local lines={}
    for _,row in ipairs(i.tooltip) do
      if not string.find(row.left or "","^Requires Level %d+") then tinsert(lines,row) end
    end
    return lines
  end
  local lines={}
  local function line(text,green)
    tinsert(lines,{left=text,r=green and 0 or 1,g=1,b=green and 0 or 1,wrap=green and 1 or nil})
  end
  local c=ITEM_QUALITY_COLORS[i.quality] or ITEM_QUALITY_COLORS[1]
  tinsert(lines,{left=i.name,r=c.r,g=c.g,b=c.b})
  if w.bonding==1 then line(ITEM_SOULBOUND) elseif w.bonding==2 then line(ITEM_BIND_ON_EQUIP) elseif w.bonding==3 then line(ITEM_BIND_ON_USE) end
  if w.maxcount==1 then line(ITEM_UNIQUE) end
  local slots={[1]="Head",[2]="Neck",[3]="Shoulder",[4]="Shirt",[5]="Chest",[6]="Waist",[7]="Legs",[8]="Feet",
    [9]="Wrist",[10]="Hands",[11]="Finger",[12]="Trinket",[13]="One-Hand",[14]="Off Hand",[15]="Ranged",
    [16]="Back",[17]="Two-Hand",[19]="Tabard",[20]="Chest",[21]="Main Hand",[22]="Off Hand",[23]="Held In Off-hand",[28]="Relic"}
  local armor={[1]="Cloth",[2]="Leather",[3]="Mail",[4]="Plate",[6]="Shield",[8]="Idol"}
  local weapons={[0]="Axe",[1]="Axe",[2]="Bow",[3]="Gun",[4]="Mace",[5]="Mace",[6]="Polearm",[7]="Sword",
    [8]="Sword",[10]="Staff",[13]="Fist Weapon",[15]="Dagger",[18]="Crossbow",[19]="Wand"}
  if slots[w.InventoryType] then tinsert(lines,{left=slots[w.InventoryType],right=(w.class==4 and armor or weapons)[w.subclass] or ""}) end
  if w.armor and w.armor>0 then line(w.armor.." Armor") end
  local damageMin,damageMax=0,0
  local schools={[1]="Holy",[2]="Fire",[3]="Nature",[4]="Frost",[5]="Shadow",[6]="Arcane"}
  for n=1,5 do
    local low,high=w["dmg_min"..n] or 0,w["dmg_max"..n] or 0
    if high>0 then
      local school=schools[w["dmg_type"..n]]
      tinsert(lines,{left=string.format("%g - %g %sDamage",low,high,school and school.." " or ""),
        right=n==1 and string.format("Speed %.2f",(w.delay or 0)/1000) or nil})
      damageMin,damageMax=damageMin+low,damageMax+high
    end
  end
  if damageMax>0 and w.delay and w.delay>0 then line(string.format("(%.1f damage per second)",(damageMin+damageMax)*500/w.delay)) end
  local stats={[0]="Mana",[1]="Health",[3]="Agility",[4]="Strength",[5]="Intellect",[6]="Spirit",[7]="Stamina"}
  for n=1,10 do local value=w["stat_value"..n] if value and value~=0 and stats[w["stat_type"..n]] then line(string.format("%+d %s",value,stats[w["stat_type"..n]])) end end
  for _,r in ipairs({{"holy_res","Holy"},{"fire_res","Fire"},{"nature_res","Nature"},{"frost_res","Frost"},{"shadow_res","Shadow"},{"arcane_res","Arcane"}}) do
    if w[r[1]] and w[r[1]]>0 then line("+"..w[r[1]].." "..r[2].." Resistance") end
  end
  if w.MaxDurability and w.MaxDurability>0 then line("Durability "..w.MaxDurability.." / "..w.MaxDurability) end
  if w.AllowableClass and w.AllowableClass>0 and w.AllowableClass<1535 then
    local names={}
    for _,class in ipairs({{1,"Warrior"},{2,"Paladin"},{3,"Hunter"},{4,"Rogue"},{5,"Priest"},{7,"Shaman"},{8,"Mage"},{9,"Warlock"},{11,"Druid"}}) do
      if math.mod(math.floor(w.AllowableClass/2^(class[1]-1)),2)==1 then tinsert(names,class[2]) end
    end
    if table.getn(names)>0 then
      local _,class=UnitClass("player")
      local ids={WARRIOR=1,PALADIN=2,HUNTER=3,ROGUE=4,PRIEST=5,SHAMAN=7,MAGE=8,WARLOCK=9,DRUID=11}
      local id=ids[class]
      local met=id and math.mod(math.floor(w.AllowableClass/2^(id-1)),2)==1
      tinsert(lines,{left="Classes: "..table.concat(names,", "),r=1,g=met and 1 or 32/255,b=met and 1 or 32/255})
    end
  end
  for n=1,5 do
    local spell=w["spellid_"..n]
    if spell and spell>0 then
      local info=WB.spellInfo(spell)
      local prefix=({[0]="Use: ",[1]="Equip: ",[2]="Chance on hit: "})[w["spelltrigger_"..n]]
      if info and prefix then line(prefix..WB.spellText(info,info.description),true) end
    end
  end
  if i.temper then line("Improved: "..i.temper) end
  for _,effect in ipairs(i.effects or {}) do line(effect,true) end
  if i.temper or (i.effects and table.getn(i.effects)>0) then
    if i.armor then line(string.format("Skyrim Armor: %g",i.armor)) end
    if i.damage then line(string.format("Skyrim Damage: %g",i.damage)) end
  end
  if w.description and w.description~="" then line('"'..w.description..'"') end
  if i.set then
    local worn,count={},0
    for _,item in pairs(WB.equipment or {}) do if item.entry then worn[item.entry]=true end end
    for _,item in ipairs(i.set.items) do if worn[item.entry] then count=count+1 end end
    line(" ")
    tinsert(lines,{left=i.set.name.." ("..count.."/"..table.getn(i.set.items)..")",r=1,g=0.82,b=0})
    for _,item in ipairs(i.set.items) do
      local c=worn[item.entry] and 1 or 0.5
      tinsert(lines,{left="  "..item.name,r=c,g=c,b=c})
    end
    for _,bonus in ipairs(i.set.bonuses) do
      local info=WB.spellInfo(bonus.spell)
      if info then
        local active=count>=bonus.threshold
        tinsert(lines,{left="("..bonus.threshold..") Set: "..WB.spellText(info,info.description),
          r=active and 0 or 0.5,g=active and 1 or 0.5,b=active and 0 or 0.5,wrap=1})
      end
    end
  end
  return lines
end
WB.itemTooltip = wowTooltip -- shared client-format lines for native gear validation
function __WBBagPrepareItem(item)
  if not WB.wowBagItems then rememberClientItems() end
  local own = item.entry and WB.wowBagItems[item.entry]
  if own then
    item.texture, item.link = own.texture, own.link
    if not item.wow and not item.tooltip then item.tooltip=own.tooltip end
    if item.instance and (item.temper or item.enchanted) then item.link=nil end
    -- The captured client colour is authoritative when an older gear.json has no quality.
    local colours={ ["9d9d9d"]=0,["ffffff"]=1,["1eff00"]=2,["0070dd"]=3,["a335ee"]=4,["ff8000"]=5 }
    item.quality = item.quality or colours[string.match(own.link or "", "^|cff(%x%x%x%x%x%x)")]
  end
  item.quality=item.quality or (item.enchanted and 2 or 1)
  -- Live imported gear always uses its display's client icon. A remembered
  -- bag texture (or Skyrim fallback) must never override ItemDisplayInfo.
  if item.entry and item.display then
    local icon=__WBItemIcon(item.display)
    if icon and icon~="" then item.texture=icon end
  end
  item.texture=__WBBagIcon(item)
  if not item.link then
    local colour=ITEM_QUALITY_COLORS and ITEM_QUALITY_COLORS[item.quality]
    local hex=colour and colour.hex or (item.quality==2 and "|cff1eff00" or "|cffffffff")
    item.link=hex.."|H"..(item.entry and item.entry>0 and "item:"..item.entry..":"..(item.enchantment or 0)..":0:"..(item.instance or 0) or "skyrim:"..item.form).."|h["..item.name.."]|h|r"
  end
  if item.entry and item.display and item.wow then
    WB.dressItems=WB.dressItems or {}
    WB.dressItems[item.entry]={display=item.display,type=item.wow.InventoryType,sheath=item.wow.sheath or 0}
  end
  if item.entry and item.entry>0 then
    if item.wow then item.wow.RequiredLevel=0 end
    if item.tooltip then item.tooltip=wowTooltip(item) end
    WB.liveItemInfo=WB.liveItemInfo or {}
    WB.liveItemInfo[item.entry]=item
    for _,name in ipairs({"GetItemInfo","IsUsableItem"}) do if live[name] then _G[name]=live[name] end end
  end
  return item
end
local function importedItem(value)
  local id=tonumber(value) or (type(value)=="string" and tonumber(string.match(value,"item:(%d+):")))
  if id and WB.liveItemInfo then return WB.liveItemInfo[id] end
  for _,item in pairs(WB.liveItemInfo or {}) do if item.name==value then return item end end
end
function IsUsableItem(...)
  local item=importedItem(arg[1])
  if item and (not item.wow or item.wow.class==2 or item.wow.class==4) then return 1,nil end -- Skyrim owns gear equip/use.
  if recorded.IsUsableItem then return recorded.IsUsableItem(unpack(arg,1,arg.n)) end
end
function GetItemInfo(...)
  local item=importedItem(arg[1])
  if not item or not item.wow or (item.wow.class~=2 and item.wow.class~=4) then
    if recorded.GetItemInfo then return recorded.GetItemInfo(unpack(arg,1,arg.n)) end
    return
  end
  local w=item.wow
  local slots={[1]="HEAD",[2]="NECK",[3]="SHOULDER",[4]="BODY",[5]="CHEST",[6]="WAIST",[7]="LEGS",[8]="FEET",
    [9]="WRIST",[10]="HAND",[11]="FINGER",[12]="TRINKET",[13]="WEAPON",[14]="SHIELD",[15]="RANGED",
    [16]="CLOAK",[17]="2HWEAPON",[19]="TABARD",[20]="ROBE",[21]="WEAPONMAINHAND",[22]="WEAPONOFFHAND",
    [23]="HOLDABLE",[28]="RELIC"}
  local armor={[1]="Cloth",[2]="Leather",[3]="Mail",[4]="Plate",[6]="Shields",[8]="Idols"}
  local weapon={[0]="One-Handed Axes",[1]="Two-Handed Axes",[2]="Bows",[3]="Guns",[4]="One-Handed Maces",
    [5]="Two-Handed Maces",[6]="Polearms",[7]="One-Handed Swords",[8]="Two-Handed Swords",[10]="Staves",
    [13]="Fist Weapons",[15]="Daggers",[18]="Crossbows",[19]="Wands"}
  return item.name,item.link,item.quality,w.ItemLevel or 0,0,w.class==2 and "Weapon" or "Armor",
    (w.class==2 and weapon or armor)[w.subclass] or "Miscellaneous",w.stackable or 1,
    slots[w.InventoryType] and "INVTYPE_"..slots[w.InventoryType] or "",item.texture
end
local function setHeld(item)
  WB.bagHeld = item
  WB.bagCursor = item and item.key or nil
  WB.setCursor(item and { item = item.key, texture = item.texture } or nil)
end
-- Deterministic projection of Skyrim copies onto the limited paper doll. Keep reserved places
-- in bagItems for save/host identity, but expose only copies not represented by an equipment slot.
local function projectEquipment()
  local list={} for _,i in pairs(WB.bagItems) do tinsert(list,i) end
  if WB.bagHeld and WB.bagHeld.pos==0 then tinsert(list,WB.bagHeld) end
  table.sort(list,function(a,b)
    local ap=a.pos==0 and 1000001 or a.pos local bp=b.pos==0 and 1000001 or b.pos
    return ap~=bp and ap<bp or ap==bp and a.key<b.key
  end)
  WB.equipment={}
  local nativeSlots={head=1,amulet=2,shoulders=3,body=5,waist=6,legs=7,feet=8,wrists=9,hands=10,ring=11,back=15,shield=17}
  local groups,order={},{}
  for _,i in ipairs(list) do
    i.bagCount=i.count i.equippedCount=0 i.bagEquipped=nil
    local id=i.identity or i.key
    if not groups[id] then groups[id]={items={},slots={},seen={}} tinsert(order,id) end
    local g=groups[id] tinsert(g.items,i)
    if i.worn then
      g.worn=true
      local slots=i.inventorySlots
      if not slots then
        local slot=nativeSlots[i.armorSlot] or (i.weapon and (i.weapon==7 or i.weapon==9) and 18) or (i.weapon and 16)
        slots=slot and {slot} or {}
      end
      for _,slot in ipairs(slots) do if not g.seen[slot] then g.seen[slot]=true tinsert(g.slots,slot) end end
    end
  end
  local function assign(i,slot)
    if type(slot)~='number' or slot<0 or slot>19 or math.floor(slot)~=slot or WB.equipment[slot] or i.equippedCount>=i.count then return false end
    WB.equipment[slot]=i i.equippedCount=i.equippedCount+1
    i.bagCount=slot==0 and 0 or i.count-i.equippedCount
    return true
  end
  if WB.bagEquipSlot and WB.bagHeld then assign(WB.bagHeld,WB.bagEquipSlot) end
  for _,id in ipairs(order) do
    local g=groups[id]
    local excess=g.worn and table.getn(g.slots)==0
    for _,slot in ipairs(g.slots) do
      local done=WB.bagHeld and WB.bagEquipSlot==slot and WB.bagHeld.identity==id
      if not done then
        if slot==11 and WB.equipment[11] then slot=12 elseif slot==13 and WB.equipment[13] then slot=14 end
        for _,i in ipairs(g.items) do
          if i~=WB.bagHeld and assign(i,slot) then done=true break end
        end
      end
      if not done then excess=true end
    end
    if excess then
      for _,i in ipairs(g.items) do if i.bagCount>0 then i.bagEquipped=true break end end
    end
  end
end
local function command(action,key,count,to,expected,splitKey)
  WB.bagRevision=(WB.bagRevision or 0)+1
  WB.host("bagCursor",action,key or "",count or 0,to or 0,expected or "",splitKey or "",WB.bagRevision)
end
local function firstFree()
  local pos=1 while WB.bagItems[pos] do pos=pos+1 end return pos
end
local function cancelHeld()
  local i=WB.bagHeld
  if i and i.pos==0 then
    local target=WB.bagItems[WB.bagOrigin]
    if target and target.identity==i.identity then target.count=target.count+i.count
    else
      i.pos=not target and WB.bagOrigin and WB.bagOrigin>0 and WB.bagOrigin or firstFree()
      WB.bagItems[i.pos]=i
    end
  end
  WB.bagOrigin=nil WB.bagEquipSlot=nil setHeld(nil) projectEquipment()
end
function ClearCursor()
  if WB.bagHeld then command("cancel") cancelHeld() oldClear() __WBBagsChanged() else oldClear() end
end
function __WBBagWorldClick(button)
  if WB.merchantCursor then ClearCursor() return 1 end
  if not WB.bagHeld then return 0 end
  if button==1 then ClearCursor()
  else command("drop",WB.bagHeld.key,WB.bagHeld.count) end
  return 1
end
-- Mouse dispatch must leave inventory cursors for the host's world-release routing.
-- Character pickups use this same held pile, with bagEquipSlot reserving the worn copy.
function __WBBagCursorHeld() return (WB.bagHeld or WB.merchantCursor) and 1 or nil end
function __WBResetBags()
  WB.bagRevision=0 WB.bagHeld=nil WB.bagOrigin=nil WB.bagCursor=nil WB.bagEquipSlot=nil oldClear()
  __WBInventory({},1,"",0,0)
end

local function controls(frame)
  if frame.wbPages then return end
  local panel = CreateFrame("Frame", frame:GetName().."Pages", frame)
  panel:SetWidth(112) panel:SetHeight(20) panel:SetPoint("TOPLEFT", frame, "TOPLEFT", 48, -25)
  local label = panel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
  label:SetPoint("CENTER", panel, "CENTER", 0, 0)
  panel.label = label
  for _, direction in ipairs({-1,1}) do
    local button = CreateFrame("Button", nil, panel)
    button:SetWidth(20) button:SetHeight(20)
    button:SetPoint(direction==-1 and "LEFT" or "RIGHT", panel, direction==-1 and "LEFT" or "RIGHT", 0, 0)
    local art = "Interface\\Buttons\\UI-SpellbookIcon-" .. (direction==-1 and "Prev" or "Next") .. "Page-"
    button:SetNormalTexture(art.."Up") button:SetPushedTexture(art.."Down") button:SetDisabledTexture(art.."Disabled")
    button:SetHighlightTexture("Interface\\Buttons\\UI-Common-MouseHilight", "ADD")
    button:SetScript("OnClick", function() __WBBagPage(WB.bagPage+direction) PlaySound("igAbiliityPageTurn") end)
    if direction==-1 then panel.prev=button else panel.next=button end
  end
  frame.wbPages=panel
end
function __WBBagsChanged()
  if not WB.liveInventory then return end
  local high=1 for pos in pairs(WB.bagItems) do high=math.max(high,pos) end
  WB.bagPages=math.floor((high-1)/80)+1
  if WB.bagHeld and WB.bagHeld.pos==0 and firstFree()>WB.bagPages*80 then WB.bagPages=WB.bagPages+1 end
  WB.bagPage=math.min(WB.bagPage,WB.bagPages)
  for i=1, NUM_CONTAINER_FRAMES or 0 do
    local frame=getglobal("ContainerFrame"..i)
    -- The pages turn on the backpack, the first bag, for the whole set; shown once a second page exists.
    if frame and frame:IsShown() and frame:GetID()==0 and WB.bagPages>1 then
      controls(frame) frame.wbPages:Show()
      frame.wbPages.label:SetText(WB.bagPage.." / "..WB.bagPages)
      if WB.bagPage>1 then frame.wbPages.prev:Enable() else frame.wbPages.prev:Disable() end
      if WB.bagPage<WB.bagPages then frame.wbPages.next:Enable() else frame.wbPages.next:Disable() end
    elseif frame and frame.wbPages then frame.wbPages:Hide() end
  end
  for bag=0,4 do __WBFire("BAG_UPDATE",bag) end
  for n=1,NUM_CONTAINER_FRAMES or 0 do
    local f=getglobal("ContainerFrame"..n)
    if f and f:IsShown() then
      for slot=1,16 do
        local b=getglobal(f:GetName().."Item"..slot)
        if b then
          if not b.wbEquipped then
            b.wbEquipped=b:CreateFontString(nil,"OVERLAY","GameFontHighlightSmall")
            b.wbEquipped:SetPoint("BOTTOMLEFT",b,"BOTTOMLEFT",2,2)
          end
          local i=itemAt(f:GetID(),b:GetID())
          b.wbEquipped:SetText(i and i.bagEquipped and "E" or "")
        end
      end
    end
  end
end
function __WBBagPage(page)
  if not WB.liveInventory then return end
  WB.bagPage=math.max(1,math.min(WB.bagPages,math.floor(page)))
  WB.host("bagPage", WB.bagPage)
  __WBBagsChanged()
end
function __WBInventory(list, page, heldKey, origin, revision, equippedSlot)
  if revision and revision<(WB.bagRevision or 0) then return end
  if revision then WB.bagRevision=revision end
  if not WB.liveInventory then
    rememberClientItems()
  end
  for name, fn in pairs(live) do _G[name]=fn end
  WB.liveInventory=true WB.bagItems={}
  local high, held=1, nil
  for _, item in ipairs(list) do
    __WBBagPrepareItem(item)
    item.identity=item.identity or item.key
    if item.pos>0 then WB.bagItems[item.pos]=item high=math.max(high,item.pos) end
    if item.key==(heldKey or WB.bagCursor) then held=item end
  end
  if heldKey~=nil or WB.bagCursor then
    WB.bagOrigin=origin or WB.bagOrigin WB.bagEquipSlot=equippedSlot setHeld(held)
  end
  projectEquipment()
  for _,item in pairs(WB.bagItems) do if item.wow then item.tooltip=wowTooltip(item) end end
  __WBFire("UNIT_INVENTORY_CHANGED","player")
  for slot=1,19 do __WBFire("PLAYER_EQUIPMENT_CHANGED",slot) end
  -- A page more only once every place of hers is full (Bags.cpp places a new stack in the first free place);
  -- existing pages are never packed to fill gaps, and an emptied last page goes.
  WB.bagPages=math.floor((high-1)/80)+1
  if heldKey~=nil or WB.bagCursor then WB.bagOrigin=origin or WB.bagOrigin setHeld(held) end
  if held and held.pos==0 and firstFree()>WB.bagPages*80 then WB.bagPages=WB.bagPages+1 end
  WB.bagPage=math.max(1,math.min(WB.bagPages,page or WB.bagPage or 1))
  if CharacterBag0Slot then
    for bag=0,3 do getglobal("CharacterBag"..bag.."SlotIconTexture"):SetTexture(path(icons.bag)) end
  end
  __WBBagsChanged()
end
function GetContainerNumSlots(...)
  local bag = arg[1]
  if not WB.liveInventory or not valid(bag) then return recorded.GetContainerNumSlots(unpack(arg,1,arg.n)) end
  return 16
end
function GetContainerItemInfo(...)
  local bag,slot = unpack(arg,1,arg.n)
  if not WB.liveInventory then return recorded.GetContainerItemInfo(unpack(arg,1,arg.n)) end
  if slot==nil then error("Usage: GetContainerItemInfo(index, slot)",2) end
  local i=itemAt(bag,slot)
  if i then return i.texture,i.bagCount or i.count,i.key==WB.bagCursor and 1 or nil,i.quality,nil end
end
function GetContainerItemLink(...)
  local bag,slot = unpack(arg,1,arg.n)
  if not WB.liveInventory then return recorded.GetContainerItemLink(unpack(arg,1,arg.n)) end
  if slot==nil then error("Usage: GetContainerItemLink(index, slot)",2) end
  local i=itemAt(bag,slot) if i then return i.link end
end
local function itemCooldown(item)
  local c=item and item.use and WB.cooldowns[item.use.spell]
  if c and c[1]+c[2]>GetTime() then return c[1],c[2],1 end
  return 0,0,item and 1 or 0
end
function GetContainerItemCooldown(...)
  local bag,slot = unpack(arg,1,arg.n)
  if not WB.liveInventory then return recorded.GetContainerItemCooldown(unpack(arg,1,arg.n)) end
  if slot==nil then error("Usage: GetContainerItemCooldown(index, slot)",2) end
  return itemCooldown(itemAt(bag,slot))
end
function GetBagName(...)
  local bag = unpack(arg,1,arg.n)
  if not WB.liveInventory or not valid(bag) then return recorded.GetBagName(unpack(arg,1,arg.n)) end
  return bag==0 and BACKPACK_TOOLTIP or "Traveler's Bag"
end
function ContainerIDToInventoryID(...)
  local bag = unpack(arg,1,arg.n)
  if WB.liveInventory and valid(bag) and bag>0 then return 19+bag end
  return recorded.ContainerIDToInventoryID(unpack(arg,1,arg.n))
end
function GetInventoryItemTexture(...)
  local unit,slot = unpack(arg,1,arg.n)
  if WB.liveInventory and unit=="player" and slot and slot>=20 and slot<=23 then return path(icons.bag) end
  if WB.liveInventory and unit=="player" then local i=WB.equipment[slot] return i and i.texture end
  return recorded.GetInventoryItemTexture(unpack(arg,1,arg.n))
end
function GetInventoryItemLink(unit,slot)
  if WB.liveInventory and unit=="player" then local i=WB.equipment[slot] return i and i.link end
  return recorded.GetInventoryItemLink(unit,slot)
end
function GetInventoryItemCount(unit,slot)
  if WB.liveInventory and unit=="player" then local i=WB.equipment[slot] return i and (slot==0 and i.count or 1) or 0 end
  return recorded.GetInventoryItemCount(unit,slot)
end
function GetInventoryItemCooldown(unit,slot)
  if WB.liveInventory and unit=="player" then return itemCooldown(WB.equipment[slot]) end
  return recorded.GetInventoryItemCooldown(unit,slot)
end
function SetBagPortaitTexture(...)
  local texture,bag = unpack(arg,1,arg.n)
  if not WB.liveInventory or not valid(bag) then return recorded.SetBagPortaitTexture(unpack(arg,1,arg.n)) end
  texture:SetTexture(path(bag==0 and icons.backpack or icons.bag))
end
local function grab(item,count,action,slot)
  if not item or WB.cursor or count<1 or count>item.count or math.floor(count)~=count then return end
  WB.bagOrigin=item.pos
  local splitKey=item.key..":split:"..((WB.bagRevision or 0)+1)
  local function exists(key)
    for _,i in pairs(WB.bagItems) do if i.key==key then return true end end
  end
  while exists(splitKey) do splitKey=splitKey..":pile" end
  command(action or "grab",item.key,count,slot or 0,"",splitKey)
  if count<item.count then
    local partial={} for k,v in pairs(item) do partial[k]=v end
    partial.key=splitKey partial.pos=0 partial.count=count item.count=item.count-count
    setHeld(partial)
  else setHeld(item) end
end
function __WBBagEquipment(action,slot)
  local item=action=="equip" and WB.bagHeld or WB.equipment[slot]
  if not item then return end
  if action=="equip" then
    command(action,item.key,1,slot)
    cancelHeld() oldClear()
  elseif action=="undress" then
    if WB.cursor then return end
    grab(item,1,"undress",slot) WB.bagEquipSlot=slot projectEquipment()
  else command(action,item.key,1,slot) end
  __WBBagsChanged()
end

function SplitContainerItem(...)
  local bag,slot,count=unpack(arg,1,arg.n)
  if not WB.liveInventory then return recorded.SplitContainerItem(unpack(arg,1,arg.n)) end
  if type(count)~="number" then return end
  local i=itemAt(bag,slot)
  if i and count<=(i.bagCount or i.count) then grab(i,count) projectEquipment() __WBBagsChanged() end
end
function PickupContainerItem(...)
  local bag,slot = unpack(arg,1,arg.n)
  if not WB.liveInventory then return recorded.PickupContainerItem(unpack(arg,1,arg.n)) end
  if not valid(bag,slot) or not slot then return end
  if WB.merchantCursor then __WBMerchantPlace() return end
  local target=itemAt(bag,slot)
  if not WB.bagHeld then grab(target,target and (target.bagCount or target.count) or 0) projectEquipment()
  else
    local source=WB.bagHeld
    local to=position(bag,slot)
    if source.pos==to and not WB.bagEquipSlot then ClearCursor() return end
    -- Hidden equipped copies still reserve their native places. Swapping that
    -- reservation must not undress the displaced equipped item.
    target=WB.bagItems[to]
    command("place",source.key,0,to,target and target.key or "")
    WB.bagEquipSlot=nil
    if target==source then WB.bagOrigin=nil setHeld(nil) __WBBagsChanged() return end
    if target and target.identity==source.identity then
      target.count=target.count+source.count
      if source.pos>0 then WB.bagItems[source.pos]=nil end
      WB.bagOrigin=nil setHeld(nil)
    else
      local from=source.pos
      if from>0 then WB.bagItems[from]=target end
      WB.bagItems[to]=source source.pos=to
      if target then target.pos=from if from>0 then WB.bagOrigin=from end else WB.bagOrigin=nil end
      setHeld(target)
      cancelHeld()
    end
    projectEquipment()
  end
  __WBBagsChanged()
end

local function put(bag)
  if WB.merchantCursor then __WBMerchantPlace() return 1 end
  if not WB.bagCursor then return nil end
  for slot=1,16 do if not itemAt(bag,slot) then PickupContainerItem(bag,slot) return 1 end end
  return 1
end
function PutItemInBackpack() if WB.liveInventory then return put(0) end return recorded.PutItemInBackpack() end
function PutItemInBag(slot) if WB.liveInventory and slot and slot>=20 and slot<=23 then return put(slot-19) end return recorded.PutItemInBag(slot) end
function UseContainerItem(...)
  local bag,slot = unpack(arg,1,arg.n)
  if not WB.liveInventory then return recorded.UseContainerItem(unpack(arg,1,arg.n)) end
  if WB.bagHeld then ClearCursor() return end
  local i=itemAt(bag,slot)
  if i and WB.merchant then __WBMerchantSell(i)
  elseif i and WB.lootSession and WB.lootCanDeposit then __WBLootDeposit(i)
  elseif i and not WB.cursor then WB.host(i.use and IsAltKeyDown() and "bagUseEffect" or "bagUse",i.key) end
end
-- What she can do with it, in the client's own hint style (green, as ITEM_OPENABLE "<Right Click to Open>"); the
-- user: "the tooltips for each item in the bag show both the item information and the options and hotkeys".
local function hints(t,i)
  local use
  if i.kind=="Weapon" or i.kind=="Armor" or i.kind=="Ammo" then use=i.bagEquipped and "Unequip" or "Equip"
  elseif i.kind=="Food" then use="Eat"
  elseif i.kind=="Potion" then use=i.potion=="poison" and "Apply to Your Weapon" or "Drink" end
  if WB.merchant then use="Sell"
  elseif WB.lootSession and WB.lootCanDeposit then use="Store" end
  if use then t:AddLine("<Right Click to "..use..">",0,1,0) end
  if i.use and not WB.merchant then
    local spell=WB.spellInfo(i.use.spell)
    if spell then t:AddLine("<Alt Right Click to "..spell.name..">",0,1,0) end
  end
  if i.count>1 then t:AddLine("<Shift Click to Split>",0,1,0) end
  t:AddLine("<Drag to Move>",0,1,0)
  t:AddLine(WB.lootSession and WB.lootCanDeposit and "<Drag to Container to Store>" or "<Drag Out to Drop>",0,1,0)
  if WB.merchant then return end
  -- Reading and scrolls still use Skyrim's own inventory menu.
  local skyrim=(i.kind=="Book" and "Read") or (i.kind=="Scroll" and "Ready") or "Sell"
  t:AddLine("<Tab, then Items: "..skyrim..">",0.5,0.5,0.5)
end
local function tooltip(t,i,showHints)
  t:ClearLines()
  if not i then t:Hide() return nil,0 end
  t.__itemId=i.entry
  local lines=wowTooltip(i)
  if lines then
    for _,l in ipairs(lines) do
      if l.right then t:AddDoubleLine(l.left,l.right,l.r or 1,l.g or 1,l.b or 1,l.rr or 1,l.rg or 1,l.rb or 1)
      else t:AddLine(l.left,l.r or 1,l.g or 1,l.b or 1,l.wrap) end
    end
  else
    local c=ITEM_QUALITY_COLORS[i.quality] or ITEM_QUALITY_COLORS[1]
    t:AddLine(i.name,c.r,c.g,c.b)

    t:AddDoubleLine(i.slotName or i.kind or "Miscellaneous",i.typeName or "",1,1,1,1,1,1)
    if i.armor then t:AddLine(string.format("%g Armor",i.armor),1,1,1) end
    if i.damage then t:AddLine(string.format("%g Damage",i.damage),1,1,1) end
    t:AddLine(string.format("Weight: %g",i.weight or 0),1,1,1)
    t:AddLine(string.format("Value: %d",i.value or 0),1,1,1)
    for _,effect in ipairs(i.effects or {}) do t:AddLine(effect,0,1,0,1) end
  end
  -- Older captured tooltips lack item_template rows, but still show instance modifiers.
  if not i.wow then
    if i.temper then t:AddLine("Improved: "..i.temper,1,1,1) end
    if i.tooltip then for _,effect in ipairs(i.effects or {}) do t:AddLine(effect,0,1,0,1) end end
  end
  if showHints~=false and i.bagEquipped then t:AddLine("Equipped",1,1,1) end
  if showHints~=false then hints(t,i) end
  t:Show() return showHints==false and 1 or nil,0
end
function __WBBagTooltip(t,item) return tooltip(t,item,false) end
function MT:SetBagItem(bag,slot)
  if not WB.liveInventory then return oldBagTip(self,bag,slot) end
  return tooltip(self,itemAt(bag,slot))
end
function MT:SetInventoryItem(unit,slot)
  if WB.liveInventory and unit=="player" and slot and slot>=20 and slot<=23 then
    self:SetText("Traveler's Bag",1,1,1) self:AddLine("16 Slot Bag",1,1,1) self:Show() return 1
  end
  if WB.liveInventory and unit=="player" then return tooltip(self,WB.equipment[slot],false) end
  return oldInvTip(self,unit,slot)
end
function MT:SetHyperlink(link)
  if WB.liveInventory then for _,i in pairs(WB.bagItems) do if i.link==link or link=="skyrim:"..i.form then tooltip(self,i) return 1 end end end
  local item=importedItem(link)
  if item then return tooltip(self,item,false) end
  return oldHyperlink(self,link)
end
function MT:SetHyperlinkCompareItem(link,index,shift,anchor)
  if WB.liveInventory then
    local item=importedItem(link)
    if item then
      local slots=__WoweeCompareSlots and __WoweeCompareSlots[select(9,GetItemInfo(item.entry))]
      local slot=slots and slots[index or 1]
      return tooltip(self,slot and WB.equipment[slot],false)
    end
  end
  if oldCompare then return oldCompare(self,link,index,shift,anchor) end
end
-- FrameXML loads after this API. Install the small header controls only after that load, without replacing its
-- container scripts; both keyboard and bag-bar clicks still run the client's functions.
function __WBInstallBags()
  if WB.bagsInstalled then return end WB.bagsInstalled=true
  -- Keep the recorded client defaults intact for reference renders. In Skyrim,
  -- B uses the client's all-bag toggle, including saved legacy Shift-B bindings.
  local function bagBindings()
    SetBinding("B","OPENALLBAGS")
    if GetBindingAction("SHIFT-B")=="OPENALLBAGS" then SetBinding("SHIFT-B") end
    __WBFire("UPDATE_BINDINGS")
  end
  local loadBindings=LoadBindings
  LoadBindings=function(set) loadBindings(set) bagBindings() end
  bagBindings()
  for n=1,NUM_CONTAINER_FRAMES do
    local money=getglobal("ContainerFrame"..n.."MoneyFrame")
    if money then
      money:EnableMouse(false)
      for _,coin in ipairs({"Gold","Silver","Copper"}) do
        local button=getglobal(money:GetName()..coin.."Button")
        if button then button:EnableMouse(false) button:SetScript("OnClick",nil) end
      end
    end
  end
  local gameMenu=ToggleGameMenu
  ToggleGameMenu=function(clicked)
    if not clicked and WB.bagHeld then ClearCursor() return end
    return gameMenu(clicked)
  end
  local generate=ContainerFrame_GenerateFrame
  ContainerFrame_GenerateFrame=function(...)
    generate(unpack(arg))
    if WB.liveInventory then __WBBagsChanged() end
  end
  if WB.liveInventory then
    local list={} for _,item in pairs(WB.bagItems or {}) do tinsert(list,item) end
    __WBInventory(list,WB.bagPage)
  end
end

-- Leave the original closures untouched until a live snapshot, including their error stack levels.
for name,fn in pairs(recorded) do live[name]=_G[name] _G[name]=fn end
-- __WBLogin: after a ReloadUI her inventory is back (in __WB) before the next publish.
WB.installBags = function() for name, fn in pairs(live) do _G[name] = fn end end
