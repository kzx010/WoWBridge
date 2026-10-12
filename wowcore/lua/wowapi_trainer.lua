-- Skyrim's native training offer and her class's next automatic spell ranks,
-- over Blizzard's unchanged 1.12 ClassTrainerFrame. Until the first snapshot,
-- leave the recorded trainer closures (including error locations) untouched.
local WB = __WB
local live = {}
local installed, installing = false, false
local oldUnitName = UnitName
local function fire(...) __WBFire(...) end
local function offer()
  local t = WB.trainer
  return t and not WB.trainerPending and not WB.trainerClosing and t.current >= 0 and t.current < t.maximum and
    t.trained < t.limit and t.cost >= 0 and t.gold >= t.cost
end
local function row(index)
  return WB.trainerEntries and WB.trainerEntries[index or 0]
end
local function required(index, name)
  if not index then error("Usage: "..name.."(index)", 3) end
  return row(index)
end
local function rebuild()
  local t, entries = WB.trainer, {}
  if t then
    local service = {key="train", name=t.skillName, sub="Level "..(t.current+1),
      kind=offer() and "available" or "unavailable", cost=math.max(0,t.cost)*10000,
      description="Increase "..t.skillName.." from "..t.current.." to "..(t.current+1)..".\n"..
        "Trainings this level: "..t.trained.."/"..t.limit..". Trainer maximum: "..t.maximum.."."}
    if t.current >= t.maximum then service.description=service.description.."\nYou have reached this trainer's maximum."
    elseif t.trained >= t.limit then service.description=service.description.."\nYou must gain a level before training again."
    elseif t.gold < t.cost then service.description=service.description.."\nYou do not have enough gold." end
    local nextRanks = {}
    -- Progression already applies data/progression.json's class, source and
    -- prerequisite gates. Show the first pending rank in each chain for this
    -- skill, including ranks beyond this trainer's maximum (with their threshold).
    for _, spell in ipairs(WB.classReply and WB.classReply.unlearned_spells or {}) do
      if spell.skill == t.skill then
        local previous = nextRanks[spell.chain or spell.spell]
        if not previous or (spell.rank or 0) < (previous.rank or 0) then nextRanks[spell.chain or spell.spell] = spell end
      end
    end
    local spells = {}
    for _, spell in pairs(nextRanks) do tinsert(spells, spell) end
    table.sort(spells, function(a,b)
      if a.learn_skill_value ~= b.learn_skill_value then return a.learn_skill_value < b.learn_skill_value end
      if a.name ~= b.name then return a.name < b.name end
      return a.spell < b.spell
    end)
    WB.trainerFilters = WB.trainerFilters or {available=true, unavailable=true, used=true}
    WB.trainerCollapsed = WB.trainerCollapsed or {}
    local function group(key, name, list)
      local visible = {}
      for _, item in ipairs(list) do if WB.trainerFilters[item.kind] then tinsert(visible,item) end end
      if table.getn(visible)==0 then return end
      tinsert(entries,{key=key,name=name,kind="header",expanded=not WB.trainerCollapsed[key]})
      if not WB.trainerCollapsed[key] then for _, item in ipairs(visible) do tinsert(entries,item) end end
    end
    group("training", t.skillName, {service})
    local previews = {}
    for _, spell in ipairs(spells) do
      local info = WB.spellInfo(spell.spell)
      if info then
        -- 'used' is the client's grey, non-purchasable state. These are previews;
        -- earning their skill threshold teaches them through Progression, then
        -- they leave this list and the next rank takes their place.
        local description = WB.spellText(info).."\nLearned at "..spell.learn_skill.." "..spell.learn_skill_value.."."
        for _, requirement in ipairs(spell.learn_requirements or {}) do description=description.."\n"..requirement end
        if spell.learn_skill_value > t.maximum then description=description.."\nBeyond this trainer's maximum ("..t.maximum..")." end
        tinsert(previews,{key="spell:"..spell.spell, name=info.name, sub=spell.rank_name or info.rank or "", kind="used",
          spell=spell.spell, cost=0, description=description, threshold=spell.learn_skill_value, skill=spell.learn_skill})
      end
    end
    group("spells", "Next spells", previews)
  end
  local selected = row(WB.trainerSelection)
  WB.trainerEntries, WB.trainerSelection = entries, 0
  if selected then for i, item in ipairs(entries) do if item.key==selected.key then WB.trainerSelection=i break end end end
end
local function update()
  -- The client's event handler selects service 2 when nothing is selected.
  -- With two collapsed groups, 2 is another header; selecting it would expand
  -- it and recursively trigger the same event. Redraw that purely UI state.
  if ClassTrainerFrame and ClassTrainerFrame:IsVisible() and (WB.trainerSelection or 0)==0 and
    row(2) and row(2).kind=="header" then
    ClassTrainerFrame.selectedService=nil
    ClassTrainerTrainButton:Disable()
    ClassTrainerFrame_Update()
  else fire("TRAINER_UPDATE") end
end
local function refresh()
  rebuild()
  if WB.trainer and not installing then update() end
end
function __WBTrainer(snapshot, revision)
  if revision and revision < (WB.trainerRevision or 0) then return end
  if not snapshot and not WB.liveTrainer then WB.trainerRevision=revision or 0 return end
  local previous, pending = WB.trainer, WB.trainerPending
  WB.liveTrainer, WB.trainer, WB.trainerRevision = true, snapshot, revision or 0
  WB.trainerPending, WB.trainerClosing = nil, nil
  WB.installTrainer()
  if not snapshot then
    rebuild()
    if previous then fire("TRAINER_CLOSED") end
    return
  end
  local opening = not previous or previous.session ~= snapshot.session
  if opening then WB.trainerCollapsed,WB.trainerSelection={},0 end
  if pending then WB.trainerSelection=0 end -- the client's next learnable service restores detail/Train after a purchase
  if WB.world.money ~= snapshot.gold*10000 then WB.world.money=snapshot.gold*10000 fire("PLAYER_MONEY") end
  rebuild()
  if opening then fire("TRAINER_SHOW") else update() end
end
function live.GetNumTrainerServices() return table.getn(WB.trainerEntries or {}) end
function live.GetTrainerGreetingText()
  local t=WB.trainer
  return t and (t.skillName.." "..t.current.."/"..t.maximum..". Trainings this level: "..t.trained.."/"..t.limit..".") or ""
end
function live.GetTrainerSelectionIndex() return WB.trainerSelection or 0 end
function live.SelectTrainerService(index) WB.trainerSelection=row(index) and index or 0 end
function live.GetTrainerServiceInfo(index)
  local r=required(index,"GetTrainerServiceInfo")
  if not r then return nil,nil,nil,1 end
  return r.name,r.sub,r.kind,r.expanded and 1 or nil
end
function live.GetTrainerServiceCost(index) local r=required(index,"GetTrainerServiceCost") return r and r.cost or 0,0,0 end
function live.GetTrainerServiceDescription(index) local r=required(index,"GetTrainerServiceDescription") return r and r.description end
function live.GetTrainerServiceIcon(index)
  local r=required(index,"GetTrainerServiceIcon") local info=r and r.spell and WB.spellInfo(r.spell)
  return r and (info and info.icon or "Interface\\Icons\\INV_Misc_Book_09")
end
function live.GetTrainerServiceLevelReq(index) required(index,"GetTrainerServiceLevelReq") return 0 end
function live.GetTrainerServiceSkillLine(index) local r=required(index,"GetTrainerServiceSkillLine") return r and r.skill end
function live.GetTrainerServiceSkillReq(index)
  local r=required(index,"GetTrainerServiceSkillReq")
  return r and r.skill,r and r.threshold or 0,r and r.threshold and WB.trainer.current>=r.threshold and 1 or nil
end
function live.GetTrainerServiceNumAbilityReq(index) required(index,"GetTrainerServiceAbilityReq") return 0 end
function live.GetTrainerServiceAbilityReq(index, ability)
  required(index,"GetTrainerServiceAbilityReq")
  if not ability then error("Usage: GetTrainerServiceAbilityReq(index, abilityIndex)",2) end
  return nil,nil
end
function live.GetTrainerServiceStepReq(index) required(index,"GetTrainerServiceStepReq") return nil,1 end
function live.IsTrainerServiceLearnSpell(index) local r=required(index,"IsTrainerServiceLearnSpell") return r and r.spell and 1 or nil,nil end
function live.IsTradeskillTrainer() return nil end
function live.IsTalentTrainer() return nil end
function live.IsTalentTrainerTabSelected() return nil end
local function filter(kind)
  if kind~="available" and kind~="unavailable" and kind~="used" then error("Bad service type in GetTrainerServiceTypeFilter",3) end
end
function live.GetTrainerServiceTypeFilter(kind)
  if not kind then error('Usage: GetTrainerServiceTypeFilter("type")',2) end
  filter(kind) return WB.trainerFilters and WB.trainerFilters[kind] and 1 or nil
end
function live.SetTrainerServiceTypeFilter(kind, enabled)
  filter(kind)
  local value=enabled and enabled~=0 or false
  local changed=WB.trainerFilters[kind]~=value
  WB.trainerFilters[kind]=value
  _G["TRAINER_FILTER_"..string.upper(kind)]=value and 1 or 0
  if changed then refresh() end
end
local function collapse(index, collapsed)
  if index==0 then
    WB.trainerCollapsed.training,WB.trainerCollapsed.spells=collapsed,collapsed
  else local r=row(index) if r and r.kind=="header" then WB.trainerCollapsed[r.key]=collapsed end end
  rebuild()
  if ClassTrainerFrame and ClassTrainerFrame:IsVisible() then
    ClassTrainerFrame.selectedService=WB.trainerSelection>0 and WB.trainerSelection or nil
    if WB.trainerSelection>0 then ClassTrainer_SetSelection(WB.trainerSelection)
    else ClassTrainerTrainButton:Disable() end
    ClassTrainerFrame_Update()
  end
end
function live.CollapseTrainerSkillLine(index) collapse(index,true) end
function live.ExpandTrainerSkillLine(index) collapse(index,nil) end
function live.BuyTrainerService(index)
  local r,t=row(index),WB.trainer
  if not t or not r or r.key~="train" or not offer() then return end
  WB.trainerPending=true
  WB.host("trainer","train",t.session,t.current,t.cost,t.trained)
  refresh() -- disable repeat clicks until the native acknowledgement
end
function live.CloseTrainer()
  local t=WB.trainer
  if not t or WB.trainerClosing then return end
  WB.trainerClosing=true
  WB.host("trainer","close",t.session,t.current,t.cost,t.trained)
end
function live.UnitName(unit)
  if type(unit)=="string" and string.lower(unit)=="npc" and WB.trainer then return WB.trainer.name,nil end
  return oldUnitName(unit)
end
WB.installTrainer=function()
  for name,fn in pairs(live) do _G[name]=fn end
  if installed or not WB.trainer then return end
  installed,installing=true,true
  -- Blizzard_TrainerUI defaults 'used' off at ADDON_LOADED. Grey previews
  -- should be visible initially; retain the player's filters across ReloadUI.
  local filters={available=true,unavailable=true,used=true}
  for kind,enabled in pairs(WB.trainerFilters or {}) do filters[kind]=enabled end
  WB.trainerFilters={}
  for kind,enabled in pairs(filters) do WB.trainerFilters[kind]=enabled end
  if ClassTrainerFrame_LoadUI then ClassTrainerFrame_LoadUI() end
  WB.trainerFilters=filters
  for kind,enabled in pairs(filters) do _G["TRAINER_FILTER_"..string.upper(kind)]=enabled and 1 or 0 end
  installing=false
  rebuild()
end
-- A fresh progression answer refreshes the pending list while training is open.
local classSpells = __WBClassSpells
function __WBClassSpells(reply)
  classSpells(reply)
  if WB.trainer then refresh() end
end
