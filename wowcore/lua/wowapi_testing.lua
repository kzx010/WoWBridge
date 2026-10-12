-- User-requested gameplay testing tools. Opening/browsing is read-only; explicit
-- buttons send validated wb operations to the main-thread console dispatcher.
local WB=__WB
local pages={"Character","Skills","Gear","Items","Spells","Travel","Spawns","Mounts"}
local names={onehanded="One-handed",twohanded="Two-handed",marksman="Archery",heavyarmor="Heavy armor",lightarmor="Light armor",speechcraft="Speech"}
local selectedSkill="destruction"
local page,offset,rows,refresh="Character",0,{},nil
local frame,fields,buttons,list,tabs
local rowCount=11
WB.testPanelPending=nil
local function label(s) return names[s] or string.upper(string.sub(s,1,1))..string.sub(s,2) end
local function send(command,arg,value)
  if WB.testPanelPending then return end
  WB.testPanelPending=true
  if frame then frame.status:SetText("Working...") frame.status:SetTextColor(1,.8,.2) end
  WB.host("testCommand",command,arg or "",value or "")
end
local function row(text,fn,hint) return {text=text,run=fn,hint=hint} end
local function action(text,command,arg,value)
  return row(text,function() send(command,type(arg)=="function" and arg() or arg,type(value)=="function" and value() or value) end)
end
local function a() return fields[1]:GetText() end
local function b() return fields[2]:GetText() end
local function menu(name) return function() frame:Hide() WB.host("menu",name) end end
local function setup()
  local data=WB.testPanelCatalog or {}
  local controls,choices={},{}
  local description,left,right,defaultA,defaultB
  if page=="Character" then
    description="Change appearance, resources and progression. Skill XP earns Skyrim's normal level-up choices."
    left,right,defaultA,defaultB="Level","Base magicka",tostring(UnitLevel("player") or 1),"1500"
    controls={action("Set level","level",a),action("Set magicka","magicka",b),action("Refill resources","heal"),
      action("Male","sex","male"),action("Female","sex","female"),action("Reset WoW effects","reset"),
      action("Refund talents","talents","reset"),row("Open Skyrim skills",menu("StatsMenu")),row("Open inventory",menu("InventoryMenu"))}
    tinsert(choices,action("Return to humanoid form","form","none"))
    local formNames={[768]="cat",[5487]="bear",[9634]="bear",[783]="travel",[1066]="aquatic"}
    for _,known in ipairs(WB.forms or {}) do
      local form=formNames[known.spell]
      if form then tinsert(choices,action("Preview learned "..label(form).." form","form",form)) end
    end
  elseif page=="Skills" then
    description="Select a skill below. Set changes its value directly; XP advances skills and enables normal level-up choices."
    left,right,defaultA,defaultB="Skill value (0-100)","Skill XP (0-1000000)","30","1000"
    controls={action("Set "..label(selectedSkill),"skill",function() return selectedSkill end,a),
      action("Add skill XP","xp",function() return selectedSkill end,b),action("Set all skills","skills",a),
      action("+100 XP","xp",function() return selectedSkill end,"100"),action("+1000 XP","xp",function() return selectedSkill end,"1000"),
      action("+10000 XP","xp",function() return selectedSkill end,"10000")}
    for _,s in ipairs(data.skills or {}) do
      local key=s.key
      tinsert(choices,row(label(key).."  ("..math.floor(s.value or 0)..")",function() selectedSkill=key refresh(false) end))
    end
  elseif page=="Gear" then
    description="Select equipment, give a piece or complete set, then equip it. Native multipliers affect Skyrim combat/movement."
    left,right,defaultA,defaultB="WoW item entry","Native multiplier % (10-300)","873","100"
    controls={action("Give item","give",a),action("Give and equip","equip",a),row("Open inventory",menu("InventoryMenu")),
      action("Weapon damage %","damage",b),action("Movement speed %","speed",b),action("Reset multipliers","multipliers")}
    for _,s in ipairs(data.sets or {}) do tinsert(choices,action("Give set: "..s.name.." ("..s.count.." pieces)","give","set",s.key)) end
    for _,item in ipairs(data.gear or {}) do
      local entry=tostring(item.entry)
      tinsert(choices,row(item.name.."  ["..entry.."]",function() fields[1]:SetText(entry) end,"Select this entry, then Give or Give and equip."))
    end
  elseif page=="Items" then
    description="Give Skyrim inventory items by hexadecimal form ID. Select a preset or enter any installed item ID."
    left,right,defaultA,defaultB="Skyrim form ID (hex)","Quantity (1-100)","0000000F","1"
    controls={action("Give Skyrim item","nativeitem",a,b),row("Open inventory",menu("InventoryMenu"))}
    for _,item in ipairs({{"Gold","0000000F"},{"Lockpick","0000000A"},{"Iron ingot","0005ACE4"},
      {"Leather strips","000800E4"},{"Petty soul gem","0002E4E2"},{"Health potion","0003EADD"}}) do
      local id=item[2] tinsert(choices,row(item[1].."  ["..id.."]",function() fields[1]:SetText(id) end))
    end
  elseif page=="Spells" then
    description="Learn individual class/native spells or the complete class book. Learning leaves current skills unchanged."
    left,right,defaultA,defaultB="WoW spell ID (decimal)","Skyrim spell ID (hex)","5176","00012FCD"
    controls={action("Learn WoW spell","learnwow",a),action("Learn Skyrim spell","learnskyrim",b),action("Learn class book","learnall"),
      action("Rebuild from skills","unlearn"),row("Open spellbook",function() frame:Hide() ToggleSpellBook("spell") end),row("Open Skyrim magic",menu("MagicMenu")),action("Refresh cooldowns","cooldowns")}
    for id,s in pairs(WB.classRows or {}) do if not s.passive then
      local info=WB.spellInfo(id) local key=tostring(id)
      if info then tinsert(choices,row("WoW: "..info.name.." "..(info.rank or "").." ["..key.."]",function() fields[1]:SetText(key) end)) end
    end end
    for id,s in pairs(WB.skyrimSpells or {}) do
      local key=string.format("%08X",math.abs(id))
      tinsert(choices,row("Skyrim: "..s.name.." ["..key.."]",function() fields[2]:SetText(key) end))
    end
    table.sort(choices,function(x,y) return x.text<y.text end)
  elseif page=="Travel" then
    description="Travel immediately to a town or testing area. QASmoke is Skyrim's indoor item-testing room."
    left,right,defaultA,defaultB="","","",""
    controls={}
    for _,p in ipairs(data.places or {}) do tinsert(choices,action("Travel to "..label(p.key),"travel",p.key)) end
  elseif page=="Mounts" then
    description="Choose an epic ground mount outdoors in humanoid form. Stand still for 3 seconds; movement cancels preparation."
    left,right,defaultA,defaultB="","","",""
    controls={action("Dismount","mount","none")}
    for _,m in ipairs(WB.mountCatalog or {}) do
      local key=m.key
      tinsert(choices,action(m.name.."  (+100% ground speed)","mount",key))
    end
  else
    description="Spawn nearby actors. The dummy wolf has 10000 HP and disabled AI; Clear spawned affects only actors created here."
    left,right,defaultA,defaultB="Count (1-5)","","1",""
    controls={action("Spawn dummy","spawn","dummy",a),action("Spawn wolf","spawn","wolf",a),action("Spawn chicken","spawn","chicken",a),
      action("Clear spawned","clearspawn"),action("Refill resources","heal")}
  end
  return description,left,right,defaultA,defaultB,controls,choices
end
local function create()
  frame=CreateFrame("Frame","WBTestPanel",UIParent)
  frame:SetWidth(760) frame:SetHeight(590) frame:SetPoint("CENTER",UIParent,"CENTER",0,0)
  frame:SetFrameStrata("DIALOG") frame:EnableMouse(true) frame:SetClampedToScreen(true)
  frame:SetBackdrop({bgFile="Interface\\DialogFrame\\UI-DialogBox-Background",edgeFile="Interface\\DialogFrame\\UI-DialogBox-Border",tile=true,tileSize=32,edgeSize=32,insets={left=10,right=10,top=10,bottom=10}})
  frame:Hide()
  local function text(name,x,y,w,font)
    local f=frame:CreateFontString(name,"OVERLAY",font or "GameFontHighlightSmall") f:SetPoint("TOPLEFT",frame,"TOPLEFT",x,y)
    f:SetWidth(w) f:SetJustifyH("LEFT") return f
  end
  local function button(name,x,y,w,h)
    local f=CreateFrame("Button",name,frame,"UIPanelButtonTemplate") f:SetPoint("TOPLEFT",frame,"TOPLEFT",x,y) f:SetWidth(w) f:SetHeight(h) return f
  end
  frame.title=text("WBTestPanelTitle",22,-20,650,"GameFontNormalLarge") frame.title:SetText("WoWBridge testing tools")
  local close=CreateFrame("Button","WBTestPanelClose",frame,"UIPanelCloseButton") close:SetPoint("TOPRIGHT",frame,"TOPRIGHT",-6,-6) close:SetScript("OnClick",function() frame:Hide() end)
  frame.description=text("WBTestPanelDescription",146,-52,586) frame.description:SetHeight(38)
  fields={} buttons={} list={} tabs={}
  for i=1,2 do
    local x=146+(i-1)*292
    local f=CreateFrame("EditBox","WBTestPanelValue"..i,frame,"InputBoxTemplate") f:SetPoint("TOPLEFT",frame,"TOPLEFT",x,-109) f:SetWidth(262) f:SetHeight(22) f:SetAutoFocus(false) f:SetMaxLetters(16)
    f.caption=text("WBTestPanelValue"..i.."Label",x,-91,270) f:SetScript("OnEscapePressed",function() this:ClearFocus() end) f:SetScript("OnEnterPressed",function() this:ClearFocus() end)
    fields[i]=f
  end
  for i=1,9 do local f=button("WBTestPanelAction"..i,146+((i-1)%3)*194,-140-math.floor((i-1)/3)*25,188,22)
    f:SetScript("OnClick",function() if this.action then this.action.run() end end) buttons[i]=f end
  frame.search=CreateFrame("EditBox","WBTestPanelSearch",frame,"InputBoxTemplate") frame.search:SetPoint("TOPLEFT",frame,"TOPLEFT",146,-244) frame.search:SetWidth(460) frame.search:SetHeight(22) frame.search:SetAutoFocus(false) frame.search:SetMaxLetters(80)
  frame.search:SetScript("OnTextChanged",function() offset=0 refresh(false) end) frame.search:SetScript("OnEscapePressed",function() this:ClearFocus() end)
  frame.searchLabel=text("WBTestPanelSearchLabel",146,-226,400) frame.searchLabel:SetText("Filter this page")
  for i=1,rowCount do local f=button("WBTestPanelRow"..i,146,-278-(i-1)*22,580,21)
    f:SetScript("OnClick",function() if this.row then this.row.run() end end)
    f:SetScript("OnEnter",function() if this.row and this.row.hint then GameTooltip:SetOwner(this,"ANCHOR_RIGHT") GameTooltip:SetText(this.row.hint,1,1,1) GameTooltip:Show() end end)
    f:SetScript("OnLeave",function() GameTooltip:Hide() end) list[i]=f end
  for i,name in ipairs(pages) do local f=button("WBTestPanelTab"..i,18,-60-(i-1)*32,115,28)
    f:SetText(name) f.page=name f:SetScript("OnClick",function() page=this.page offset=0 frame.search:SetText("") refresh(true) end) tabs[i]=f end
  local prev=button("WBTestPanelPrev",146,-532,80,22) prev:SetText("Previous") prev:SetScript("OnClick",function() offset=math.max(0,offset-rowCount) refresh(false) end)
  local next=button("WBTestPanelNext",646,-532,80,22) next:SetText("Next") next:SetScript("OnClick",function() if offset+rowCount<table.getn(rows) then offset=offset+rowCount end refresh(false) end)
  frame.count=text("WBTestPanelCount",240,-536,400)
  local reload=button("WBTestPanelRefresh",18,-334,115,24) reload:SetText("Refresh lists") reload:SetScript("OnClick",function() send("catalog") end)
  frame.status=text("WBTestPanelStatus",22,-558,706) frame.status:SetHeight(30) frame.status:SetText("Choose a page. Changes apply when you click an action.")
  frame:SetScript("OnShow",function() refresh(true) send("catalog") end)
  tinsert(UISpecialFrames,"WBTestPanel")
  if __WBPanelLocks then __WBPanelLocks() end
end
refresh=function(reset)
  if not frame then return end
  local description,left,right,defaultA,defaultB,controls,choices=setup()
  frame.description:SetText(description)
  for i,title in ipairs({left,right}) do fields[i].caption:SetText(title) if title=="" then fields[i]:Hide() else fields[i]:Show() end end
  if reset then fields[1]:SetText(defaultA) fields[2]:SetText(defaultB) end
  for i,button in ipairs(buttons) do local op=controls[i] button.action=op if op then button:SetText(op.text) button:Show() else button:Hide() end end
  local filter=string.lower(frame.search:GetText() or "") rows={}
  for _,r in ipairs(choices) do if filter=="" or string.find(string.lower(r.text),filter,1,true) then tinsert(rows,r) end end
  offset=math.min(offset,math.max(0,math.floor((table.getn(rows)-1)/rowCount)*rowCount))
  for i,button in ipairs(list) do local r=rows[offset+i] button.row=r if r then button:SetText(r.text) button:Show() else button:Hide() end end
  frame.count:SetText(table.getn(rows)==0 and "No matching entries" or string.format("%d-%d of %d",offset+1,math.min(offset+rowCount,table.getn(rows)),table.getn(rows)))
  for _,tab in ipairs(tabs) do if tab.page==page then tab:LockHighlight() else tab:UnlockHighlight() end end
end
function __WBTestPanelToggle() if not frame then create() end if frame:IsShown() then frame:Hide() else frame:Show() end end
function __WBTestSkills(skills)
  WB.testPanelCatalog=WB.testPanelCatalog or {} WB.testPanelCatalog.skills=skills refresh(false)
end
function __WBTestCatalog(data) WB.testPanelCatalog=data refresh(false) end
function __WBTestResult(result)
  WB.testPanelPending=result.pending or nil WB.testPanelResult=result
  if frame then frame.status:SetText(result.message or "") frame.status:SetTextColor(result.ok and .3 or 1,result.ok and 1 or .3,.3) end
end
