-- Real 1.12 client capture metadata. Reads state; never replaces client APIs or FrameXML.
local function value(v)
  if type(v) == "string" then return string.format("%q", v) end
  if type(v) == "number" or type(v) == "boolean" or v == nil then return tostring(v) end
  return type(v)
end
local function values(ok, ...)
  if not ok then return "error: " .. tostring(arg[1]) end
  local out = {}
  for i = 1, arg.n do out[i] = value(arg[i]) end
  return table.concat(out, ", ")
end
local panels = { "SpellBookFrame", "GameMenuFrame", "CharacterFrame", "MerchantFrame", "LootFrame",
                 "ClassTrainerFrame", "TaxiFrame", "QuestLogFrame", "DressUpFrame", "GossipFrame", "ScriptErrors", "StaticPopup1", "StackSplitFrame" }
local function snapshot(label, kind, panel)
  local out = { kind = kind, expectedPanel = panel or "", label = label }
  local function probe(expr)
    local fn, err = loadstring("return " .. expr)
    out[expr] = fn and values(pcall(fn)) or ("error: " .. tostring(err))
  end
  local function call(fn, ...)
    local a = {}
    for i = 1, arg.n do a[i] = value(arg[i]) end
    probe(fn .. "(" .. table.concat(a, ",") .. ")")
  end
  for _, expr in ipairs({ 'GetBuildInfo()', 'GetTime()', 'GetScreenWidth()', 'GetScreenHeight()',
      'UIParent:GetEffectiveScale()', 'GetCVar("useUiScale")', 'GetCVar("uiscale")',
      'GetCVar("statusBarText")', 'NAMEPLATES_ON', 'FRIENDNAMEPLATES_ON',
      'UnitName("player")', 'UnitLevel("player")', 'UnitHealth("player")', 'UnitHealthMax("player")',
      'UnitMana("player")', 'UnitManaMax("player")', 'UnitPowerType("player")', 'GetNumShapeshiftForms()',
      'UnitName("target")', 'UnitName("npc")', 'GetMoney()', 'GetNumPartyMembers()',
      'GetZoneText()', 'GetSubZoneText()', 'GetPlayerMapPosition("player")',
      'GameTooltip:IsVisible()', 'UIParent:IsVisible()', 'GetCursorPosition()', 'UnitRace("player")', 'UnitSex("player")' }) do probe(expr) end
  for _, name in ipairs(panels) do
    local f = getglobal(name)
    out["visible:" .. name] = f and f:IsVisible() and "true" or "false"
  end
  probe('GetMouseFocus() and GetMouseFocus():GetName()')
  if GameTooltip:IsVisible() then
    for i=1,4 do
      for _,side in ipairs({'Left','Right'}) do
        local name='GameTooltipText'..side..i
        local text=getglobal(name)
        if text and text:IsVisible() then probe(name..':GetText()') end
      end
    end
  end
  if StackSplitFrame:IsVisible() then
    for _,expr in ipairs({'StackSplitFrame.split','StackSplitFrame.maxStack',
      'StackSplitFrame.owner:GetName()','StackSplitText:GetText()',
      'StackSplitLeftButton:IsEnabled()','StackSplitRightButton:IsEnabled()'}) do probe(expr) end
  end
  for i = 1, GetNumShapeshiftForms() do call("GetShapeshiftFormInfo", i) end
  for i = 1, 19 do call("GetInventoryItemLink", "player", i) end
  for bag = 0, 4 do
    call("GetContainerNumSlots", bag)
    for slot = 1, GetContainerNumSlots(bag) do
      call("GetContainerItemLink", bag, slot) call("GetContainerItemInfo", bag, slot)
    end
  end
  for i = 0, 31 do call("GetPlayerBuff", i, "HELPFUL") end
  for i = 1, 24 do call("GetActionTexture", i) end
  if kind == "spellbook" then
    probe("GetNumSpellTabs()") probe("SpellBookFrame.selectedSkillLine") probe("SpellBookFrame.bookType")
    for i = 1, GetNumSpellTabs() do call("GetSpellTabInfo", i) end
    local i = 1
    while GetSpellName(i, "spell") do
      call("GetSpellName", i, "spell") call("GetSpellTexture", i, "spell") i = i + 1
    end
  elseif kind == "merchant" or kind == "buyback" then
    probe("GetMerchantNumItems()") probe("GetNumBuybackItems()") probe("MerchantFrame.page") probe("CanMerchantRepair()")
    probe("MerchantFrame.selectedTab")
    for i=1,12 do
      local name='MerchantItem'..i..'ItemButton'
      local button=getglobal(name)
      if button and button:IsVisible() then
        probe(name..':GetButtonState()') probe(name..':IsEnabled()')
      end
    end
    for i = 1, GetMerchantNumItems() do
      call("GetMerchantItemInfo", i) call("GetMerchantItemLink", i) call("GetMerchantItemMaxStack", i)
    end
    for i = 1, GetNumBuybackItems() do call("GetBuybackItemInfo", i) call("GetBuybackItemLink", i) end
  elseif kind == "trainer" then
    probe("GetNumTrainerServices()") probe("GetTrainerSelectionIndex()") probe("GetTrainerGreetingText()")
    probe("ClassTrainerTrainButton:IsEnabled()") probe("ClassTrainerListScrollFrameScrollBar:GetValue()")
    local selection = GetTrainerSelectionIndex()
    if selection > 0 and selection <= GetNumTrainerServices() then
      local name, rank, status = GetTrainerServiceInfo(selection)
      out.selectedServiceName = name or "none"
      out.selectedServiceRank = rank or "none"
      out.selectedServiceType = status or "none"
    else
      out.selectedServiceType = "none"
    end
    for _, filter in ipairs({ "available", "unavailable", "used" }) do call("GetTrainerServiceTypeFilter", filter) end
    for i = 1, GetNumTrainerServices() do
      for _, fn in ipairs({ "GetTrainerServiceInfo", "GetTrainerServiceCost", "GetTrainerServiceLevelReq",
                            "GetTrainerServiceIcon", "GetTrainerServiceDescription", "GetTrainerServiceSkillReq",
                            "GetTrainerServiceStepReq", "GetTrainerServiceNumAbilityReq" }) do call(fn, i) end
      for j = 1, GetTrainerServiceNumAbilityReq(i) do call("GetTrainerServiceAbilityReq", i, j) end
    end
  elseif kind == "taxi" then
    probe("NumTaxiNodes()")
    for i = 1, NumTaxiNodes() do
      for _, fn in ipairs({ "TaxiNodeName", "TaxiNodePosition", "TaxiNodeGetType", "TaxiNodeCost", "GetNumRoutes" }) do call(fn, i) end
      -- Route counts come from native state; bound them before iterating.
      -- A malformed count must not stall the reference-client recorder.
      local routes = GetNumRoutes(i)
      if TaxiNodeGetType(i) == "REACHABLE" and type(routes) == "number" and routes >= 0 and routes <= 64 then
        for j = 1, routes do
          call("TaxiGetSrcX", i, j) call("TaxiGetSrcY", i, j) call("TaxiGetDestX", i, j) call("TaxiGetDestY", i, j)
        end
      elseif type(routes) ~= "number" or routes < 0 or routes > 64 then
        out["skippedRouteCount:" .. i] = tostring(routes)
      end
    end
  elseif kind == "loot" then
    probe("GetNumLootItems()") probe("LootFrame.page")
    for i = 1, GetNumLootItems() do
      call("GetLootSlotInfo", i) call("GetLootSlotLink", i) call("LootSlotIsCoin", i) call("LootSlotIsItem", i)
    end
  elseif kind == "character" then
    -- The native 1.12 model exposes SetRotation, but no GetRotation.
    -- FrameXML owns this field and uses it for Model_RotateLeft/Right.
    probe("CharacterModelFrame.rotation")
    for i = 1, 5 do call("UnitStat", "player", i) end
    for i = 0, 6 do call("UnitResistance", "player", i) end
  elseif kind == "buff" then
    for i = 1, 32 do call("UnitBuff", "player", i) end
    for i = 0, 31 do
      local index = GetPlayerBuff(i, "HELPFUL")
      if index and index >= 0 then
        call("GetPlayerBuffTexture", index) call("GetPlayerBuffTimeLeft", index) call("GetPlayerBuffDispelType", index)
      end
    end
  elseif kind == "party" then
    for i = 1, 4 do
      for _, fn in ipairs({ "UnitName", "UnitHealth", "UnitHealthMax", "UnitMana", "UnitManaMax",
                            "UnitPowerType", "UnitLevel", "UnitClass", "UnitIsDead", "UnitIsConnected" }) do call(fn, "party" .. i) end
    end
  end
  WBOracleAPI = WBOracleAPI or {}
  WBOracleAPI["capture_" .. label] = out
  SlashCmdList.WBORACLE("frames " .. label)
  ChatFrame1:Clear()
end
function WBFidelityCapture(label, kind, panel, delay)
  delay = tonumber(delay) or 0.5
  assert(delay >= 0.5 and delay <= 15, "invalid capture delay")
  -- Run after the slash-command edit box closes; its focus must not contaminate the capture.
  local f, elapsed = CreateFrame("Frame"), 0
  f:SetScript("OnUpdate", function()
    elapsed = elapsed + arg1
    if elapsed < delay then return end
    this:SetScript("OnUpdate", nil)
    local ok, err = pcall(snapshot, label, kind, panel)
    if not ok then
      WBOracleAPI = WBOracleAPI or {}
      WBOracleAPI["capture_" .. label] = { error = tostring(err) }
    end
    Screenshot()
  end)
end

function WBFidelityWorld(label)
  local f, elapsed, taken = CreateFrame("Frame"), 0, false
  f:SetScript("OnUpdate", function()
    elapsed = elapsed + arg1
    if elapsed > 0.3 then UIParent:Hide() end
    if elapsed > 0.8 and not taken then
      WBOracleAPI = WBOracleAPI or {}
      WBOracleAPI["world_" .. label] = { ["UIParent:IsVisible()"] = tostring(UIParent:IsVisible() and true or false) }
      Screenshot()
      taken = true
    end
    if elapsed > 1.8 then UIParent:Show() this:SetScript("OnUpdate", nil) end
  end)
end

-- Bounded native font getter probe; temporary hidden FontStrings only.
-- Does not replace getters, font objects or FrameXML.
function WBFidelityFontMetrics()
  local f=CreateFrame('Frame',nil,UIParent)
  local text=f:CreateFontString(nil,'OVERLAY')
  local out={build=values(pcall(GetBuildInfo)),screen=GetScreenWidth()..'|'..GetScreenHeight(),
             scale=tostring(UIParent:GetEffectiveScale())}
  local samples={}
  for i=32,126 do table.insert(samples,string.char(i)) end
  for _,s in ipairs({'10','39','45','1000','Elunara','Dash','Hello, druid!  Ready for some training?'}) do table.insert(samples,s) end
  for _,face in ipairs({'FRIZQT__.TTF','ARIALN.ttf'}) do
    for _,height in ipairs({10,12,14,16}) do
      for _,flags in ipairs({'','OUTLINE'}) do
        text:SetFont('Fonts\\'..face,height,flags)
        for _,s in ipairs(samples) do
          text:SetText(s)
          out[face..'|'..height..'|'..flags..'|'..s]=tostring(text:GetStringWidth())
        end
      end
    end
  end
  f:Hide()
  WBOracleAPI=WBOracleAPI or {}
  WBOracleAPI.font_metric_faces=out
end
-- The same temporary font-only frame is built in the native reference client.
-- This probes text rendering; it does not replace a client getter or font object.
function WBFidelityFontPreview()
  local f=CreateFrame('Frame','WBFontPreview',UIParent)
  f:SetWidth(640) f:SetHeight(360)
  f:SetPoint('TOPLEFT',UIParent,'TOPLEFT',375,-185)
  f:SetFrameStrata('DIALOG')
  local bg=f:CreateTexture(nil,'BACKGROUND')
  bg:SetAllPoints(f) bg:SetTexture(0,0,0,1)
  local n=0
  for col,style in ipairs({{'FRIZQT__.TTF',''},{'FRIZQT__.TTF','OUTLINE'},{'ARIALN.ttf',''},{'ARIALN.ttf','OUTLINE'}}) do
    local title=f:CreateFontString(nil,'OVERLAY')
    title:SetFont('Fonts\\FRIZQT__.TTF',10,'')
    title:SetPoint('TOPLEFT',f,'TOPLEFT',(col-1)*160+8,-5)
    title:SetText(style[1]..' '..style[2])
    for row,height in ipairs({10,12,14,16}) do
      for line,s in ipairs({'0123456789','Dash Elunara'}) do
        n=n+1
        local text=f:CreateFontString('WBFontPreviewText'..n,'OVERLAY')
        text:SetFont('Fonts\\'..style[1],height,style[2])
        text:SetPoint('TOPLEFT',f,'TOPLEFT',(col-1)*160+8,-20-(row-1)*80-(line-1)*28)
        text:SetText(s)
      end
    end
  end
end
function WBFidelityFontStyleMetrics()
  local f=CreateFrame('Frame',nil,UIParent)
  local text=f:CreateFontString(nil,'OVERLAY')
  local out={build=values(pcall(GetBuildInfo)),screen=GetScreenWidth()..'|'..GetScreenHeight()}
  for _,face in ipairs({'FRIZQT__.TTF','ARIALN.ttf'}) do
    for _,height in ipairs({10,12,14,16}) do
      for _,flags in ipairs({'','OUTLINE','THICKOUTLINE'}) do
        text:SetFont('Fonts\\'..face,height,flags)
        for _,offset in ipairs({{0,0},{1,-1},{-1,-1},{3,0}}) do
          text:SetShadowOffset(offset[1],offset[2]) text:SetShadowColor(0,0,0,1)
          for _,s in ipairs({'0','1','45','1000','Elunara','/','\\','_',' '}) do
            text:SetText(s)
            out[face..'|'..height..'|'..flags..'|'..offset[1]..','..offset[2]..'|'..s]=tostring(text:GetStringWidth())
          end
        end
      end
    end
  end
  f:Hide() WBOracleAPI=WBOracleAPI or {} WBOracleAPI.font_style_widths=out
end
