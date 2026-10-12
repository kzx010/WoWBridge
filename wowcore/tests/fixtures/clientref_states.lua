-- Offline rendering fixtures from independently captured 1.12 client answers.
-- Never loaded by the gameplay DLL or installed in the reference client.
local getters = {
  GetMoney=true, UnitName=true,
  GetMerchantNumItems=true, GetMerchantItemInfo=true, GetMerchantItemLink=true,
  GetMerchantItemMaxStack=true, GetNumBuybackItems=true, GetBuybackItemInfo=true,
  GetBuybackItemLink=true, CanMerchantRepair=true,
  GetContainerNumSlots=true, GetContainerItemInfo=true, GetContainerItemLink=true,
  GetNumTrainerServices=true, GetTrainerSelectionIndex=true, GetTrainerGreetingText=true,
  GetTrainerServiceTypeFilter=true, GetTrainerServiceInfo=true, GetTrainerServiceCost=true,
  GetTrainerServiceLevelReq=true, GetTrainerServiceIcon=true, GetTrainerServiceDescription=true,
  GetTrainerServiceSkillReq=true, GetTrainerServiceStepReq=true, GetTrainerServiceNumAbilityReq=true,
  GetTrainerServiceAbilityReq=true,
  NumTaxiNodes=true, TaxiNodeName=true, TaxiNodePosition=true, TaxiNodeGetType=true,
  TaxiNodeCost=true, GetNumRoutes=true, TaxiGetSrcX=true, TaxiGetSrcY=true,
  TaxiGetDestX=true, TaxiGetDestY=true,
  GetNumLootItems=true, GetLootSlotInfo=true, GetLootSlotLink=true, LootSlotIsCoin=true, LootSlotIsItem=true,
}
local function key(name, ...)
  local args={}
  for i=1,arg.n do
    local v=arg[i]
    if name=='UnitName' and type(v)=='string' then v=string.lower(v) end
    args[i]=type(v)=='string' and string.format('%q',v) or tostring(v)
  end
  return name..'('..table.concat(args,',')..')'
end
local function install(data)
  local groups={}
  for expression,answer in pairs(data) do
    local name=string.match(expression,'^(%w+)%(')
    if getters[name] then
      groups[name]=groups[name] or {}
      local fn,err=loadstring('return '..answer)
      if not fn then
        -- A recorded native usage error is not a fabricated successful response.
        fn=function() error(answer) end
      end
      groups[name][expression]=fn
    end
  end
  for name,answers in pairs(groups) do
    local previous=_G[name]
    _G[name]=function(...)
      local expression=key(name,unpack(arg,1,arg.n))
      local fn=answers[expression]
      if fn then return fn() end
      -- Only unrelated unit tokens may fall back to the baseline HUD fixture.
      if name=='UnitName' then return previous(unpack(arg,1,arg.n)) end
      error('missing native client answer: '..expression)
    end
  end
end
local function nativeValue(data, expression)
  local answer=assert(data[expression], 'missing independent value: '..expression)
  return assert(loadstring('return '..answer))()
end
function __WBClientRefPanel(label)
  __WBClientRefHud()
  local data=assert(WBOracleAPI['capture_'..label],'missing independent capture')
  if data.kind=='loot' then __WBInstallLoot() end
  install(data)
  local kind=data.kind
  if kind=='merchant' or kind=='buyback' then
    -- Early captures predate the CanMerchantRepair probe. Its visible native
    -- repair button independently establishes the truthy branch of this API.
    if data['CanMerchantRepair()']==nil then
      local repair=false
      for _,row in ipairs(WBOracleAPI['frames_'..label]) do
        if string.match(row,'^F|MerchantRepairItemButton|') then repair=true end
      end
      CanMerchantRepair=function() return repair end
    end
    __WBFire('MERCHANT_SHOW')
    MerchantFrame.page=assert(tonumber(data['MerchantFrame.page']))
    if kind=='buyback' then MerchantFrameTab2:Click('LeftButton') end
    MerchantFrame_Update()
    if label=='merchant_hover_verified' or label=='merchant_pressed_verified' then
      -- Real realm data feeds the normal host item-tooltip path. No native
      -- caption is injected, and approved gear-level adaptations stay active.
      local raw=assert(WBClientRefMerchantItemTemplates[792])
      local name,texture=GetMerchantItemInfo(1)
      assert(name==raw.name, 'wrong canonical merchant item')
      __WB.liveItemInfo=__WB.liveItemInfo or {}
      __WB.liveItemInfo[792]={entry=raw.entry,name=raw.name,quality=raw.Quality,
        texture=texture,link=GetMerchantItemLink(1),wow=raw}
      local state=nativeValue(data,'MerchantItem1ItemButton:GetButtonState()')
      MerchantItem1ItemButton:SetButtonState(state, state=='PUSHED')
      assert(MerchantItem1ItemButton:GetButtonState()==state,'wrong recorded button state')
    end
    if data['visible:StackSplitFrame']=='true' then
      local owner=_G[nativeValue(data,'StackSplitFrame.owner:GetName()')]
      local maximum=nativeValue(data,'StackSplitFrame.maxStack')
      local split=nativeValue(data,'StackSplitFrame.split')
      assert(owner and maximum>=2 and split>=1 and split<=maximum, 'invalid native quantity state')
      OpenStackSplitFrame(maximum,owner,'BOTTOMLEFT','TOPLEFT')
      for n=2,split do StackSplitFrameRight_Click() end
      assert(StackSplitFrame:IsVisible() and StackSplitFrame.split==split,'wrong quantity selection')
      assert(tostring(StackSplitLeftButton:IsEnabled())==data['StackSplitLeftButton:IsEnabled()'], 'wrong decrement enable state')
      assert(tostring(StackSplitRightButton:IsEnabled())==data['StackSplitRightButton:IsEnabled()'], 'wrong increment enable state')
      assert(StackSplitText:GetText()==nativeValue(data,'StackSplitText:GetText()'),'wrong quantity text')
    end
  elseif kind=='loot' then
    __WBFire('LOOT_OPENED')
    local page=assert(tonumber(data['LootFrame.page']))
    for i=2,page do LootFrame_PageDown() end
  elseif kind=='trainer' then
    __WBFire('TRAINER_SHOW')
    local selection=assert(tonumber(data['GetTrainerSelectionIndex()']))
    -- Native selection commands have no host in this offline snapshot.
    SelectTrainerService=function() end
    ClassTrainerFrame.showSkillDetails=1
    ClassTrainer_SetSelection(selection)
    ClassTrainerListScrollFrameScrollBar:SetValue(tonumber(data['ClassTrainerListScrollFrameScrollBar:GetValue()']) or 0)
    ClassTrainerFrame_Update()
  elseif kind=='taxi' then
    local map
    for _,row in ipairs(WBOracleAPI['frames_'..label]) do
      map=map or string.match(row,'^R|TaxiMap|[^|]+|[^|]+|[^|]+|[^|]+|Texture|(.+)$')
    end
    assert(map,'missing recorded taxi map texture')
    SetTaxiMap=function(texture) texture:SetTexture(map) end
    __WBFire('TAXIMAP_OPENED')
  else error('unsupported native panel kind: '..tostring(kind)) end
  GameTooltip:Hide()
  local panel=data.expectedPanel
  assert(_G[panel] and _G[panel]:IsVisible(),'recorded panel did not open: '..tostring(panel))
  if kind=='trainer' then
    assert(ClassTrainerSkillName:GetText()==data.selectedServiceName or data.selectedServiceName==nil,'wrong trainer detail')
    local enabled=data['ClassTrainerTrainButton:IsEnabled()']
    assert(not enabled or tostring(ClassTrainerTrainButton:IsEnabled())==enabled,'wrong Train enable state')
  end
  print('CLIENT_REFERENCE '..label..' kind='..kind)
end
function __WBVerifyMerchantReference(label)
  local data=assert(WBOracleAPI['capture_'..label])
  assert(MerchantFrame:IsVisible(),'missing merchant panel')
  assert(MerchantItem1ItemButton:GetButtonState()==nativeValue(data,'MerchantItem1ItemButton:GetButtonState()'),'wrong final button state')
  if data['visible:StackSplitFrame']=='true' then
    assert(StackSplitFrame:IsVisible(),'missing quantity dialog')
    assert(StackSplitText:GetText()==nativeValue(data,'StackSplitText:GetText()'),'wrong final quantity')
  else
    assert(GameTooltip:IsVisible(),'missing merchant tooltip')
    for _,name in ipairs({'GameTooltipTextLeft1','GameTooltipTextLeft2','GameTooltipTextRight2','GameTooltipTextLeft3'}) do
      assert(_G[name]:GetText()==nativeValue(data,name..':GetText()'),'wrong native tooltip caption: '..name)
    end
  end
  return 'native merchant interaction verified'
end
for _,label in ipairs({'merchant_page1','merchant_page2','merchant_buyback',
  'merchant_hover_verified','merchant_pressed_verified',
  'merchant_quantity_min_verified','merchant_quantity_middle_verified','merchant_quantity_max_verified',
  'loot_page1','loot_page2','trainer_populated','trainer_known_verified',
  'trainer_available_verified','taxi_map_populated'}) do
  HUD_STATES['clientref_'..label]={fixture='idle',settle=2,client_reference=label,
    client_item_templates=(label=='merchant_hover_verified' or label=='merchant_pressed_verified') and 'clientref-20261010/merchant_item_templates.lua' or nil,
    print=string.match(label,'^merchant_.*_verified$') and '__WBVerifyMerchantReference("'..label..'")' or nil,
    mouse=(label=='merchant_hover_verified' or label=='merchant_pressed_verified') and {{500,550},{40,190}} or nil,
    run='__WBClientRefPanel("'..label..'")'}
end

HUD_STATES.clientref_font_preview_native={fixture='idle',settle=2,
  client_reference='font_preview_native',run='__WBClientRefHud() WBFidelityFontPreview()'}

function __WBClientRefCharacter(label)
  __WBClientRefHud()
  local data=assert(WBOracleAPI['capture_'..label])
  assert(data.kind=='character' and data.expectedPanel=='CharacterFrame')
  __WBFidelityCharacter()
  -- Preserve the host's adapted statistics. Only title, equipment, model and
  -- rotation controls are compared with the native paper doll.
  for slot=1,19 do
    local expression='GetInventoryItemLink("player",'..slot..')'
    assert(GetInventoryItemLink('player',slot)==nativeValue(data,expression),
      'reference equipment differs at slot '..slot)
  end
  local rotation=nativeValue(data,'CharacterModelFrame.rotation')
  assert(type(rotation)=='number','missing native FrameXML rotation')
  CharacterModelFrame.rotation=rotation
  CharacterModelFrame:SetRotation(rotation)
  SetCVar('statusBarText',0)
  TextStatusBar_UpdateTextString(PlayerFrameHealthBar)
  TextStatusBar_UpdateTextString(PlayerFrameManaBar)
  GameTooltip:Hide()
  assert(CharacterFrame:IsVisible(),'native character panel did not open')
  print('CLIENT_REFERENCE '..label..' rotation='..rotation)
end
function __WBClientRefBuff(label)
  __WBClientRefHud()
  local data=assert(WBOracleAPI['capture_'..label])
  assert(data.kind=='buff' and data.expectedPanel=='none')
  __WBApplyLive({player={
    health=nativeValue(data,'UnitHealth("player")'),
    healthMax=nativeValue(data,'UnitHealthMax("player")'),
    power=nativeValue(data,'UnitMana("player")'),
    powerMax=nativeValue(data,'UnitManaMax("player")'),powerType=0}})
  -- Build the actual recorded spell through the normal aura/tooltip path;
  -- native getters and tooltip text are never replaced in the reference client.
  __WBAuraAdd('player',9885,false,nativeValue(data,'GetPlayerBuffTimeLeft(0)'))
  local index=GetPlayerBuff(0,'HELPFUL')
  assert(GetPlayerBuffTexture(index)==nativeValue(data,'GetPlayerBuffTexture(0)'), 'wrong native buff texture')
  assert(GetPlayerBuffDispelType(index)==nativeValue(data,'GetPlayerBuffDispelType(0)'), 'wrong native dispel type')
  TextStatusBar_UpdateTextString(PlayerFrameHealthBar)
  TextStatusBar_UpdateTextString(PlayerFrameManaBar)
  GameTooltip:Hide()
  print('CLIENT_REFERENCE '..label..' spell=9885')
end
for _,side in ipairs({'front','back','left','right'}) do
  local label='character_'..side..'_verified'
  HUD_STATES['clientref_'..label]={fixture='idle',settle=2,client_reference=label,
    run='__WBClientRefCharacter("'..label..'")'}
end
for _,label in ipairs({'buff_motw_native','buff_motw_tooltip_native'}) do
  HUD_STATES['clientref_'..label]={fixture='idle',settle=1,client_reference=label,
    mouse=label=='buff_motw_tooltip_native' and {{0,360},{1096,23}} or nil,
    run='__WBClientRefBuff("'..label..'")'}
end
