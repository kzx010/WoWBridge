-- User-requested gameplay testing tools. Opening/browsing is read-only; explicit
-- buttons send validated wb operations to the main-thread console dispatcher.
local WB=__WB
local pages={"Character","Skills","Gear","Items","Spells","Travel","Spawns","Mounts","World / Target","Class Spells","Features","Emotes","Level Up","Armory","Paladin"}
local qualities={[0]="Poor","Common","Uncommon","Rare","Epic","Legendary"}
local names={one_handed="One-handed",two_handed="Two-handed",onehanded="One-handed",twohanded="Two-handed",marksman="Archery",heavyarmor="Heavy armor",lightarmor="Light armor",speechcraft="Speech"}
local selectedSkill="destruction"
local selectedRank=nil
local page,offset,rows,refresh="Character",0,{},nil
local frame,fields,buttons,list,tabs
local updating=false
local lastClassReply=nil
local rowCount=11
WB.testPanelPending=nil
local function label(s) return names[s] or string.upper(string.sub(s,1,1))..string.sub(s,2) end
local function send(command,arg,value)
  -- A reply that never comes (service offline, load mid-request) must not lock the panel.
  if WB.testPanelPending and GetTime()-WB.testPanelPending<15 then return end
  WB.testPanelPending=GetTime()
  if frame then frame.status:SetText("Working...") frame.status:SetTextColor(1,.8,.2) end
  WB.host("testCommand",command,arg or "",value or "")
end
local function row(text,fn,hint,selected) return {text=text,run=fn,hint=hint,selected=selected} end
local function selectEntry(text,index,key,hint)
  return row(text,function() fields[index]:SetText(key) refresh(false) end,hint,function() return string.upper(fields[index]:GetText())==string.upper(key) end)
end
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
    controls={action("Set level (skip choices)","level",a),action("Set magicka","magicka",b),action("Refill resources","heal"),
      action("Male","sex","male"),action("Female","sex","female"),action("Reset WoW effects","reset"),
      action("Refund talents","talents","reset"),row("Open Skyrim skills",menu("StatsMenu")),row("Open inventory",menu("InventoryMenu"))}
    tinsert(choices,action("Customize appearance (WoW character creation)","appearance"))
    tinsert(choices,action("Choose class and race","appearance"))
    for _,level in ipairs({10,30,60}) do tinsert(choices,action("Set level "..level.." (skips attribute choices)","level",tostring(level))) end
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
      action("+10000 XP","xp",function() return selectedSkill end,"10000"),
      row("Value: 15",function() fields[1]:SetText("15") end),row("Value: 50",function() fields[1]:SetText("50") end),
      row("Value: 100",function() fields[1]:SetText("100") end)}
    for _,s in ipairs(data.skills or {}) do
      local key=s.key
      tinsert(choices,row(label(key).."  ("..math.floor(s.value or 0)..")",function() selectedSkill=key refresh(false) end,nil,function() return selectedSkill==key end))
    end
  elseif page=="Gear" then
    description="Select any WoW item (WoWBridge.esp's or the Armory's) or a set, give it, then equip it. Native multipliers affect Skyrim combat/movement."
    left,right,defaultA,defaultB="WoW item entry or name","Native multiplier % (10-300)","873","100"
    controls={action("Give item","give",a),action("Give and equip","equip",a),row("Open inventory",menu("InventoryMenu")),
      action("Weapon damage %","damage",b),action("Movement speed %","speed",b),action("Reset multipliers","multipliers"),
      action("Toggle Show Helm","showhelm","toggle"),action("Toggle Show Cloak","showcloak","toggle")}
    for _,s in ipairs(data.sets or {}) do
      tinsert(choices,action("Give set: "..s.name.." ("..s.count.." pieces"..(s.level and ", level "..s.level or "")..")","give","set",s.key))
    end
    for _,item in ipairs(data.gear or {}) do
      local entry=tostring(item.entry)
      tinsert(choices,selectEntry(item.name.."  ["..entry.."]",1,entry,"Select this entry, then Give or Give and equip."))
    end
    -- The Armory's 8,500 items (sent once a session): only those the filter can show become rows.
    local filter=string.lower(frame.search:GetText() or "")
    for _,item in ipairs(WB.testPanelArmory or {}) do
      local entry=tostring(item[1])
      local text=item[2].."  ["..entry.."]  "..(qualities[item[4]] or "").." "..item[3]
      if filter=="" or string.find(string.lower(text),filter,1,true) then
        tinsert(choices,selectEntry(text,1,entry,"WoWBridgeArmory.esp: "..(qualities[item[4]] or "").." quality, Skyrim level "..item[3]..". Select, then Give or Give and equip."))
      end
    end
  elseif page=="Armory" then
    description="WoWBridgeArmory.esp: give a random item of a Skyrim level and quality, or a random set. Roll the treasure lists it joined at your level."
    left,right,defaultA,defaultB="Skyrim level (1-52)","Quality (0 poor - 5 legendary)",tostring(math.min(52,UnitLevel("player") or 1)),"2"
    controls={action("Give random item","random",a,b),action("Give random and equip","randomequip",a,b),
      action("Give random set","randomset",a),row("Open inventory",menu("InventoryMenu")),action("Roll treasure lists","lootroll","100"),
      action("Equip two use trinkets","usetrinkets"),action("NPC shoulders on","npcshoulders","on"),
      action("NPC shoulders off","npcshoulders","off"),action("NPC shoulders status","npcshoulders","status")}
    for _,op in ipairs({"on","off","status"}) do tinsert(choices,action("NPC weapons "..op,"npcweapons",op)) end
    for _,op in ipairs({"on","off","status"}) do tinsert(choices,action("Player Skyrim weapons "..op,"playerweapons",op)) end
    for _,op in ipairs({"on","off","status"}) do tinsert(choices,action("NPC helms "..op,"npchelms",op)) end
    for q=0,5 do tinsert(choices,selectEntry("Quality: "..qualities[q],2,tostring(q))) end
    for _,level in ipairs({1,10,20,30,40,48,52}) do tinsert(choices,selectEntry("Skyrim level: "..level,1,tostring(level))) end
  elseif page=="Items" then
    description="Give Skyrim inventory items by hexadecimal form ID. Select a preset or enter any installed item ID."
    left,right,defaultA,defaultB="Skyrim form ID (hex)","Quantity (1-100)","0000000F","1"
    controls={action("Give Skyrim item","nativeitem",a,b),row("Open inventory",menu("InventoryMenu"))}
    for _,item in ipairs({{"Gold","0000000F"},{"Lockpick","0000000A"},{"Iron ingot","0005ACE4"},
      {"Leather strips","000800E4"},{"Petty soul gem","0002E4E2"},{"Health potion","0003EADD"}}) do
      local id=item[2] tinsert(choices,selectEntry(item[1].."  ["..id.."]",1,id))
    end
  elseif page=="Spells" then
    description="Learn individual class/native spells or the complete class book. Learning leaves current skills unchanged."
    left,right,defaultA,defaultB="WoW spell ID (decimal)","Skyrim spell ID (hex)","5176","00012FCD"
    controls={action("Learn WoW spell","learnwow",a),action("Learn Skyrim spell","learnskyrim",b),action("Learn class book","learnall"),
      action("Rebuild from skills","unlearn"),row("Open spellbook",function() frame:Hide() ToggleSpellBook("spell") end),row("Open Skyrim magic",menu("MagicMenu")),action("Refresh cooldowns","cooldowns"),
      action("Give Skyrim debuffs","afflict")}
    for id,s in pairs(WB.classRows or {}) do if not s.passive then
      local info=WB.spellInfo(id) local key=tostring(id)
      if info then tinsert(choices,selectEntry("WoW: "..info.name.." "..(info.rank or "").." ["..key.."]",1,key)) end
    end end
    for id,s in pairs(WB.skyrimSpells or {}) do
      -- Lua 5.1 formats %X through a C long (32-bit on Windows): split so ESL/light-plugin IDs (FE...) print.
      local formId=math.abs(id) local high=math.floor(formId/65536)
      local key=string.format("%04X%04X",high,formId-high*65536)
      tinsert(choices,selectEntry("Skyrim: "..s.name.." ["..key.."]",2,key))
    end
    table.sort(choices,function(x,y) return x.text<y.text end)
  elseif page=="Class Spells" then
    description="Browse shipped classes, tabs and ranks. Known state is live for your class. Talent ranks require talent allocation."
    left,right,defaultA,defaultB="Class key (browse only for other classes)","Tab number (0 = all)",data.activeClass or "druid","0"
    local classes=data.classes or {}
    local definition=nil
    for _,c in ipairs(classes) do if c.key==string.lower(a()) then definition=c end end
    local function cycleClass(delta)
      local at=1 for i,c in ipairs(classes) do if c.key==string.lower(a()) then at=i end end
      if table.getn(classes)>0 then selectedRank=nil fields[1]:SetText(classes[math.mod(at-1+delta+table.getn(classes),table.getn(classes))+1].key) refresh(false) end
    end
    local function cycleTab(delta)
      local maxTab=0 for _,s in ipairs(definition and definition.spells or {}) do maxTab=math.max(maxTab,s.tab or 0) end
      selectedRank=nil fields[2]:SetText(tostring(math.mod((tonumber(b()) or 0)+delta,maxTab+1))) refresh(false)
    end
    local function selected() return tostring(selectedRank or 0) end
    local function chain()
      for _,s in ipairs(definition and definition.spells or {}) do if s.spell==selectedRank then return tostring(s.chain) end end return "0"
    end
    -- Their spell books (druid ranks a trainer, quest or item teaches): the selected rank's, its chain's, every one.
    controls={action("Learn selected rank","learnwow",selected,a),action("Learn selected chain","learnchain",chain,a),
      action("Learn whole tab","learntab",b,a),action("Give rank's book","givebook",selected),
      action("Give chain's books","givebook","chain",chain),action("Give all books","givebook","all"),
      row("Previous class",function() cycleClass(-1) end),row("Next class",function() cycleClass(1) end),
      row("Next spell tab",function() cycleTab(1) end)}
    local known={}
    if WB.classReply and (not definition or definition.key==data.activeClass) then
      for _,id in ipairs(WB.classReply.known_spells or {}) do known[id]=true end
    end
    local spells={}
    for _,s in ipairs(definition and definition.spells or {}) do
      if tonumber(b())==0 or tonumber(b())==s.tab then tinsert(spells,s) end
    end
    table.sort(spells,function(x,y)
      if x.tab~=y.tab then return x.tab<y.tab end
      if x.name~=y.name then return x.name<y.name end
      if x.rank~=y.rank then return x.rank<y.rank end return x.spell<y.spell end)
    for _,s in ipairs(spells) do
      local id=s.spell
      local learned=known[id] or (not WB.classReply and s.known)
      local gate=label(s.skill or "").." "..tostring(s.learn_skill_value or "?")
      if s.source=="talent" or s.source=="item" or s.source=="racial" or s.source=="pet" then gate=label(s.source).."; "..gate end
      local text=(s.tab_name or "General").." / "..s.name.." "..(s.rank_name or ("Rank "..s.rank)).." - "..(learned and "Known" or "Unknown").."; "..gate
      local book=data.books and data.books[tostring(id)]
      tinsert(choices,row(text,function() selectedRank=id refresh(false) end,
        "Class: "..(definition.name or definition.key).."; spell "..id.."; chain "..s.chain.."; source "..(s.source or "")..
        ". "..(s.learn_requirements or "")..(book and (" Its book: "..book..".") or "")..
        " Select a rank, then learn rank, chain or tab (current class only), or give its book.",
        function() return selectedRank==id end))
    end
  elseif page=="Level Up" then
    description="Native level advancement, +10 chosen attribute and a perk. Stamina adds 5 carry weight. WoW feedback and progression follow normally."
    left,right,defaultA,defaultB="Attribute (health / magicka / stamina)","Number of levels (1-20)","health","1"
    controls={action("Level up: Health","levelup","health","1"),action("Level up: Magicka","levelup","magicka","1"),
      action("Level up: Stamina","levelup","stamina","1"),action("Level up N times","levelup",a,b),
      row("Open Skyrim skills",menu("StatsMenu")),action("Cancel scripted leveling","levelup","cancel")}
    for _,attr in ipairs({"health","magicka","stamina"}) do tinsert(choices,selectEntry("Attribute: "..label(attr),1,attr)) end
    for _,count in ipairs({1,5,10,20}) do tinsert(choices,selectEntry("Levels: "..count,2,tostring(count))) end
  elseif page=="Paladin" then
    description="Paladin spells use normal learning, targets, resources and cooldowns. Hit rolls a five-health attack from your selected fixture. Party hit/block hits the first blessed follower for ten health; select the attacker. Status prints evidence. Racials need your race (Character: Choose class and race)."
    left,right,defaultA,defaultB="Incoming roll seed","","1",""
    controls={action("Paladin status","paladin","status"),action("Toggle auto attack","paladin","attack"),
      action("Incoming defense roll","paladin","hit",a),action("Refill resources","heal"),action("Refresh cooldowns","cooldowns"),
      action("Reset WoW effects","reset"),row("Open talents",function() frame:Hide() ToggleTalentFrame() end),
      action("Spawn still wolf","spawn","dummy","1"),action("Clear spawned actors","clearspawn")}
    for _,spell in ipairs({{"Holy Light","holy_light"},{"Flash of Light","flash"},{"Lay on Hands","lay_hands"},
      {"Seal: Righteousness","righteousness"},{"Seal: Command","command"},{"Seal: Light","light"},{"Seal: Wisdom","wisdom"},
      {"Seal: Justice","justice"},{"Seal: Crusader","crusader"},{"Judgement","judgement"},
      {"Divine Shield","divine_shield"},{"Divine Protection","divine_protection"},{"Blessing of Protection","protection"},
      {"Hammer of Justice","stun"},{"Hammer of Wrath","wrath"},{"Exorcism","exorcism"},{"Holy Wrath","holy_wrath"},
      {"Turn Undead","turn"},{"Consecration","consecration"},{"Righteous Fury","fury"},{"Holy Shield","holy_shield"},
      {"Devotion Aura","devotion"},{"Retribution Aura","retribution"},{"Concentration Aura","concentration"},
      {"Fire Resistance Aura","fire_aura"},{"Frost Resistance Aura","frost_aura"},{"Shadow Resistance Aura","shadow_aura"},
      {"Sanctity Aura","sanctity"},{"Blessing of Sanctuary","sanctuary"},{"Blessing of Sacrifice","sacrifice"},
      {"Blessing of Freedom","freedom"},{"Holy Shock: heal or strike","holy_shock"},
      {"Party hit: first follower","party_hit"},{"Party block: first follower","party_block"},
      {"Freedom: native snare probe","snare"},{"Vindication / Vengeance swings","attack"},
      {"Divine Intervention (kills you)","intervention"},{"Divine Favor","divine_favor"},
      {"Blessing of Might","might"},{"Greater Blessing of Might","greater_might"},
      {"Greater Blessing: party","greater_might"},
      {"Blessing of Salvation","salvation"},{"Greater Blessing of Salvation","greater_salvation"},
      {"Native spell hit on you","spell_hit"},{"Follower weapon hit","follower_hit"},{"Follower spell hit","follower_spell"},
      {"Racial: Perception (Human)","perception"},{"Racial: Stoneform (Dwarf)","stoneform"},
      {"Racial: Find Treasure (Dwarf)","find_treasure"},{"Bleeding probe (Stoneform)","bleed"},
      {"Native fear resistance probe","fear_probe"},{"Native paralysis (stun) probe","paralysis_probe"},
      {"Native stagger resist probe","stagger_probe"},{"Hostile interruption probe","interrupt_probe"}}) do
      tinsert(choices,action(spell[1],"paladin",spell[2]))
    end
    tinsert(choices,action("Skyrim debuffs (Stoneform)","afflict"))
  elseif page=="Features" then
    description="Local fixtures for trade, loot, stealth, talents and item tests. Remove helpers when finished; use a disposable save."
    left,right,defaultA,defaultB="Talent points (0-51, sets level; skips choices)","","0",""
    controls={action("Spawn + open merchant","merchant","spawn"),action("Open helper barter","merchant","open"),action("Remove merchant","merchant","remove"),
      action("Give 100 gold","gold","100"),action("Give 1,000 gold","gold","1000"),action("Give 10,000 gold","gold","10000"),
      action("Spawn + open loot chest","lootchest","spawn"),action("Open loot chest","lootchest","open"),action("Remove loot chest","lootchest","remove")}
    choices={action("NPC scan cost (10 s)","npcscan"),
      row("Minimap icons: "..(GetCVar("wbMinimapNativeArt")=="1" and "Skyrim" or "WoW"),function()
        SetCVar("wbMinimapNativeArt",GetCVar("wbMinimapNativeArt")=="1" and 0 or 1) refresh(false)
      end,"Toggle Skyrim compass art and WoW minimap art. Unmatched markers stay on the rim."),
      row("Ping the minimap",function() Minimap:PingLocation(20,20) end),
      action("Soothe / Hibernate: passive beast nearby","scenario","beast"),
      action("Prowl / aggression: guard and townsperson","scenario","observers"),
      action("Kynareth: spawn shrine beside you","scenario","shrine"),
      action("Reset Kynareth talent wipe price to 1 gold","talents","price"),
      action("Refund all allocated talents","talents","reset"),action("Set total talent points (skips choices)","talentpoints",a),
      action("Bag / tooltip items: varied items in inventory","scenario","inventory"),
      action("Reagents: seeds and resurrection setup","scenario","reagents"),
      action("Crafting: improve / enchant WoW gear and native materials","scenario","crafting"),
      action("Clear scenario actors and shrine","clearspawn"),action("Combat / numbers: 10000 HP dummy","spawn","dummy","1"),
      action("Rebirth / dead resources: wolf corpse","scenario","corpse"),
      action("Threat: wolves on guard follower","scenario","threat"),
      action("Snare + root on you (forms)","scenario","snare"),
      action("CC breaks: two still wolves","scenario","cc"),
      action("Refill health / magicka / stamina","heal"),action("Clear cooldowns (preserve auras)","cooldowns"),
      action("Reset forms / auras / combat","reset"),
      row("Bag replacement: open inventory",menu("InventoryMenu")),
      row("Talent allocation / no-points error: open talents",function() frame:Hide() ToggleTalentFrame() end),
      row("Maps / compass parity: open world map",function() frame:Hide() ToggleWorldMap() end),
      row("Chat / macros / bars: open macros",function() frame:Hide() MacroFrame_LoadUI() ShowUIPanel(MacroFrame) end)}
  elseif page=="Emotes" then
    description="Browse all client emote tokens. Select a token or type one, then Play. Stand ends held dance, sit, sleep and kneel states."
    left,right,defaultA,defaultB="Emote token","","DANCE",""
    controls={action("Play selected emote","emote",a),action("Stand / stop emote","emote","stand"),
      action("Last NPC reaction","react","status"),action("NPC says a line (bubble)","say"),
      action("NPC says a long line","say","long")}
    for _,token in ipairs(data.emotes or {}) do tinsert(choices,selectEntry(token,1,token)) end
  elseif page=="Travel" then
    description="Travel immediately. Feature destinations include water, flight, dungeon rooms, services and combat. NPCs follow their usual schedules."
    left,right,defaultA,defaultB="","","",""
    controls={action("Measure NPC scan (10 s)","npcscan"),action("Print last NPC scan result","npcscan","status")}
    for _,p in ipairs(data.places or {}) do
      local r=action("Travel to "..(p.name or label(p.key))..(p.hint and (" - "..p.hint) or ""),"travel",p.key)
      r.hint=p.hint tinsert(choices,r)
    end
  elseif page=="Mounts" then
    description="Summon a learned mount outdoors in humanoid form (stand still 3 seconds), or give the reins that teach one; forget all to test a new game."
    left,right,defaultA,defaultB="","","",""
    controls={action("Dismount","mount","none"),action("Forget all mounts","mounts","forget"),
      action("Restore all mounts","mounts","restore"),action("Give all reins","givereins","all")}
    for _,m in ipairs(WB.mountCatalog or {}) do
      local key=m.key
      local speed=m.flying and ("+"..m.flight_speed_percent.."% flight speed") or "+100% ground speed"
      tinsert(choices,action(m.name.."  ("..speed..")"..(m.learned and "" or " - not learned"),"mount",key))
    end
    for _,r in ipairs(data.reins or {}) do tinsert(choices,action("Give "..r.name,"givereins",r.key)) end
  elseif page=="World / Target" then
    description="Target actions use the selected WoW target. World controls use native Skyrim commands; god mode toggles each click."
    left,right,defaultA,defaultB="Hour (0-23)","","12",""
    controls={action("Set time","time",a),action("Clear weather","weather","clear"),action("Clear hold bounties","bounty","clear"),
      action("Toggle god mode","god"),action("Full-heal target","target","heal"),action("Kill target","target","kill"),
      action("Revive target","target","revive"),action("Print live defenses","defenses")}
    for _,preset in ipairs({{"Dawn",6},{"Noon",12},{"Dusk",18},{"Midnight",0}}) do
      tinsert(choices,action(preset[1].." ("..preset[2]..":00)","time",tostring(preset[2])))
    end
  else
    description="Spawn 1-5 actors nearby. Dummy: 10000 HP, AI off. Clear spawned disables these actors; it does not delete them."
    left,right,defaultA,defaultB="Count (1-5)","","1",""
    controls={action("Spawn dummy","spawn","dummy",a),action("Spawn wolf","spawn","wolf",a),action("Spawn chicken","spawn","chicken",a),
      action("Clear spawned","clearspawn"),action("Refill resources","heal"),
      action("Spawn bandit","spawn","bandit",a),action("Spawn bear","spawn","bear",a),
      action("Spawn skeever","spawn","skeever",a),action("Spawn mudcrab","spawn","mudcrab",a)}
    tinsert(choices,action("Spawn townsperson - friendly farmer for aggression checks","spawn","townsperson",a))
  end
  return description,left,right,defaultA,defaultB,controls,choices
end
local function create()
  frame=CreateFrame("Frame","WBTestPanel",UIParent)
  frame:SetWidth(800) frame:SetHeight(680) frame:SetPoint("CENTER",UIParent,"CENTER",0,0)
  frame:SetFrameStrata("DIALOG") frame:EnableMouse(true) frame:SetClampedToScreen(true)
  frame:SetBackdrop({bgFile="Interface\\DialogFrame\\UI-DialogBox-Background",edgeFile="Interface\\DialogFrame\\UI-DialogBox-Border",tile=true,tileSize=32,edgeSize=32,insets={left=10,right=10,top=10,bottom=10}})
  frame:Hide()
  local function text(name,x,y,w,font)
    local f=frame:CreateFontString(name,"OVERLAY",font or "GameFontHighlightSmall") f:SetPoint("TOPLEFT",frame,"TOPLEFT",x,y)
    f:SetWidth(w) f:SetJustifyH("LEFT") f:SetJustifyV("TOP") return f
  end
  local function button(name,x,y,w,h)
    local f=CreateFrame("Button",name,frame,"UIPanelButtonTemplate") f:SetPoint("TOPLEFT",frame,"TOPLEFT",x,y) f:SetWidth(w) f:SetHeight(h) return f
  end
  frame.title=text("WBTestPanelTitle",22,-20,700,"GameFontNormalLarge") frame.title:SetText("WoWBridge testing tools")
  local close=CreateFrame("Button","WBTestPanelClose",frame,"UIPanelCloseButton") close:SetPoint("TOPRIGHT",frame,"TOPRIGHT",-6,-6) close:SetScript("OnClick",function() frame:Hide() end)
  frame.description=text("WBTestPanelDescription",166,-54,610) frame.description:SetHeight(48)
  fields={} buttons={} list={} tabs={}
  for i=1,2 do
    local x=166+(i-1)*308
    local f=CreateFrame("EditBox","WBTestPanelValue"..i,frame,"InputBoxTemplate") f:SetPoint("TOPLEFT",frame,"TOPLEFT",x,-137) f:SetWidth(284) f:SetHeight(24) f:SetAutoFocus(false) f:SetMaxLetters(48)
    f.caption=text("WBTestPanelValue"..i.."Label",x,-115,292) f:SetScript("OnEscapePressed",function() this:ClearFocus() end) f:SetScript("OnEnterPressed",function() this:ClearFocus() end)
    f:SetScript("OnTextChanged",function() if not updating and frame.search then refresh(false) end end)
    fields[i]=f
  end
  for i=1,9 do local f=button("WBTestPanelAction"..i,166+((i-1)%3)*204,-173-math.floor((i-1)/3)*28,198,24)
    f:SetScript("OnClick",function() if this.action then this.action.run() end end) buttons[i]=f end
  frame.search=CreateFrame("EditBox","WBTestPanelSearch",frame,"InputBoxTemplate") frame.search:SetWidth(596) frame.search:SetHeight(24) frame.search:SetAutoFocus(false) frame.search:SetMaxLetters(80)
  frame.search:SetScript("OnTextChanged",function() offset=0 if not updating then refresh(false) end end) frame.search:SetScript("OnEscapePressed",function() this:ClearFocus() end)
  frame.search:SetScript("OnEnterPressed",function() this:ClearFocus() end)
  frame.searchLabel=text("WBTestPanelSearchLabel",166,-267,600) frame.searchLabel:SetText("Filter entries (mouse wheel or Previous / Next)")
  for i=1,rowCount do local f=button("WBTestPanelRow"..i,166,-320-(i-1)*24,610,23)
    f:SetNormalFontObject(GameFontHighlightSmall) f:SetHighlightFontObject(GameFontHighlightSmall) f:SetDisabledFontObject(GameFontDisableSmall)
    local fs=f:GetFontString() fs:ClearAllPoints() fs:SetPoint("LEFT",f,"LEFT",24,0) fs:SetWidth(574) fs:SetHeight(18) fs:SetJustifyH("LEFT") fs:SetJustifyV("CENTER") fs:SetFontObject(GameFontHighlightSmall)
    f.marker=f:CreateFontString("WBTestPanelRow"..i.."Selected","OVERLAY","GameFontNormal") f.marker:SetPoint("LEFT",f,"LEFT",8,0) f.marker:SetWidth(12) f.marker:SetText(">") f.marker:Hide()
    f:SetScript("OnClick",function() if this.row then this.row.run() end end)
    f:SetScript("OnEnter",function() if this.row then GameTooltip:SetOwner(this,"ANCHOR_RIGHT") GameTooltip:SetText(this.row.text,1,1,1) if this.row.hint then GameTooltip:AddLine(this.row.hint,1,.82,0,1) end GameTooltip:Show() end end)
    f:SetScript("OnLeave",function() GameTooltip:Hide() end) list[i]=f end
  for i,name in ipairs(pages) do local f=button("WBTestPanelTab"..i,18,-60-(i-1)*30,134,28)
    f:SetText(name) f.page=name f:SetScript("OnClick",function() page=this.page offset=0 updating=true frame.search:SetText("") updating=false refresh(true) end) tabs[i]=f end
  frame.prev=button("WBTestPanelPrev",166,-596,90,24) frame.prev:SetText("Previous") frame.prev:SetScript("OnClick",function() offset=math.max(0,offset-rowCount) refresh(false) end)
  frame.next=button("WBTestPanelNext",686,-596,90,24) frame.next:SetText("Next") frame.next:SetScript("OnClick",function() if offset+rowCount<table.getn(rows) then offset=offset+rowCount end refresh(false) end)
  frame:EnableMouseWheel(true) frame:SetScript("OnMouseWheel",function() if arg1>0 then frame.prev:Click() else frame.next:Click() end end)
  frame.count=text("WBTestPanelCount",272,-601,398)
  local reload=button("WBTestPanelRefresh",18,-60-table.getn(pages)*30-6,134,24) reload:SetText("Refresh lists") reload:SetScript("OnClick",function() send("catalog") end)
  frame.status=text("WBTestPanelStatus",22,-634,754) frame.status:SetHeight(32) frame.status:SetText("Choose a page. Changes apply when you click an action.")
  frame:SetScript("OnShow",function() refresh(true) send("catalog") end)
  frame:SetScript("OnUpdate",function()
    if page=="Class Spells" and WB.classReply~=lastClassReply then lastClassReply=WB.classReply refresh(false) end
  end)
  tinsert(UISpecialFrames,"WBTestPanel")
  if __WBPanelLocks then __WBPanelLocks() end
end
refresh=function(reset)
  if not frame then return end
  updating=true
  local description,left,right,defaultA,defaultB,controls,choices=setup()
  frame.description:SetText(description)
  for i,title in ipairs({left,right}) do fields[i].caption:SetText(title) if title=="" then fields[i]:Hide() fields[i].caption:Hide() else fields[i]:Show() fields[i].caption:Show() end end
  if reset then
    fields[1]:SetText(defaultA) fields[2]:SetText(defaultB)
    -- Pages that derive their list from fields must use the new page's defaults.
    description,left,right,defaultA,defaultB,controls,choices=setup()
  end
  local top=left=="" and 118 or 173
  for i,button in ipairs(buttons) do local op=controls[i] button.action=op if op then
    button:SetPoint("TOPLEFT",frame,"TOPLEFT",166+((i-1)%3)*204,-top-math.floor((i-1)/3)*28) button:SetText(op.text) button:Show()
  else button:Hide() end end
  local searchTop=top+math.ceil(table.getn(controls)/3)*28+12
  frame.searchLabel:SetPoint("TOPLEFT",frame,"TOPLEFT",166,-searchTop)
  frame.search:SetPoint("TOPLEFT",frame,"TOPLEFT",166,-searchTop-19)
  local filter=string.lower(frame.search:GetText() or "") rows={}
  for _,r in ipairs(choices) do if filter=="" or string.find(string.lower(r.text),filter,1,true) then tinsert(rows,r) end end
  offset=math.min(offset,math.max(0,math.floor((table.getn(rows)-1)/rowCount)*rowCount))
  for i,button in ipairs(list) do local r=rows[offset+i] button.row=r if r then
    button:SetPoint("TOPLEFT",frame,"TOPLEFT",166,-searchTop-53-(i-1)*24)
    button:SetText(r.text)
    -- Keep one readable line; the tooltip always carries the full entry.
    local fs=button:GetFontString() local text=r.text
    while (fs:GetStringWidth()>574 or fs:GetStringHeight()>18) and string.len(text)>0 do text=string.sub(text,1,-2) button:SetText(text.."...") end
    if r.selected and r.selected() then button:LockHighlight() button.marker:Show() else button:UnlockHighlight() button.marker:Hide() end
    button:Show()
  else button:Hide() end end
  frame.count:SetText(table.getn(rows)==0 and "No matching entries" or string.format("%d-%d of %d",offset+1,math.min(offset+rowCount,table.getn(rows)),table.getn(rows)))
  if offset>0 then frame.prev:Enable() else frame.prev:Disable() end
  if offset+rowCount<table.getn(rows) then frame.next:Enable() else frame.next:Disable() end
  for _,tab in ipairs(tabs) do if tab.page==page then tab:LockHighlight() else tab:UnlockHighlight() end end
  updating=false
end
function __WBTestPanelToggle() if not frame then create() end if frame:IsShown() then frame:Hide() else frame:Show() end end
function __WBTestPanelRefresh() if frame and frame:IsShown() then refresh(false) end end   -- what the host changed (mounts learned)
function __WBTestSkills(skills)
  WB.testPanelCatalog=WB.testPanelCatalog or {} WB.testPanelCatalog.skills=skills refresh(false)
end
function __WBTestCatalog(data)
  if data.armory then WB.testPanelArmory=data.armory data.armory=nil end   -- {entry,name,level,quality}, kept for the session
  if not data.emotes then
    data.emotes={} local seen={}
    for i=1,600 do local token=getglobal("EMOTE"..i.."_TOKEN")
      if token and not seen[token] then seen[token]=true tinsert(data.emotes,token) end
    end
    table.sort(data.emotes)
  end
  WB.testPanelCatalog=data refresh(false)
end
function __WBTestResult(result)
  WB.testPanelPending=result.pending and GetTime() or nil WB.testPanelResult=result
  if frame then frame.status:SetText(result.message or "") frame.status:SetTextColor(result.ok and .3 or 1,result.ok and 1 or .3,.3) end
end
