-- Skyrim teammates in Blizzard's four party frames. Loaded after the recorded solo API.
local WB = __WB
local recordedCount, recordedMember = GetNumPartyMembers, GetPartyMember
local recordedLeader, recordedIsLeader = GetPartyLeaderIndex, IsPartyLeader
local powerEvents = {[0]="MANA", "RAGE", "FOCUS", "ENERGY", "HAPPINESS"}

function GetNumPartyMembers()
  if not WB.liveParty then return recordedCount() end
  local n = 0
  for i=1,4 do if WB.tokens["party"..i] then n=n+1 end end
  return n
end
function GetPartyMember(...)
  if not WB.liveParty then
    -- Calling the recorded closure through a tail call loses its usage-error source location.
    if select("#",...)==0 then error("Usage: GetPartyMember(1-4)",2) end
    return recordedMember(...)
  end
  local index = ...
  if index == nil then error("Usage: GetPartyMember(1-4)", 2) end
  return WB.tokens["party"..tostring(index)] and 1 or nil
end
function GetPartyLeaderIndex()
  if not WB.liveParty then return recordedLeader() end
  return 0 -- she leads her Skyrim followers
end
function IsPartyLeader()
  if not WB.liveParty then return recordedIsLeader() end
  return GetNumPartyMembers()>0 and 1 or nil
end

local targetUnit = TargetUnit
function TargetUnit(unit)
  local token = type(unit)=="string" and strlower(unit)
  if WB.liveParty and token and strfind(token, "^party[1-4]$") then
    -- Carry identity through the main-thread queue so a departed follower's slot cannot target its replacement.
    if WB.tokens[token] then WB.host("command", "TargetUnit", token, WB.tokens[token]) end
    return
  end
  return targetUnit(unit)
end

local sendChat = SendChatMessage
function SendChatMessage(message, chatType, language, channel)
  if type(chatType)=="string" and strupper(chatType)=="PARTY" then return end
  return sendChat(message,chatType,language,channel)
end

local function unitEvent(guid, event)
  for token, id in pairs(WB.tokens) do if id==guid then __WBFire(event, token) end end
end
function __WBUnitAuraChanged(who)
  local guid = who=="player" and WB.tokens.player or who
  unitEvent(guid, "UNIT_AURA")
  if guid==WB.tokens.player then __WBFire("PLAYER_AURAS_CHANGED") end
end

-- 1.12's party art has no level label. Add one on the portrait only for the live Skyrim roster.
local installed = false
function __WBInstallParty()
  if installed or not PartyMemberFrame1 then return end
  installed = true
  for i=1,4 do
    local frame = getglobal("PartyMemberFrame"..i)
    local level = frame:CreateFontString("PartyMemberFrame"..i.."Level", "OVERLAY", "GameFontNormalSmall")
    level:SetFontObject(GameFontNormalSmall)
    level:SetTextColor(1,0.82,0)
    level:SetPoint("BOTTOM", frame.portrait, "BOTTOM", 0, 3)
    level:SetText(UnitExists("party"..i) and tostring(UnitLevel("party"..i)) or "")
  end
  local driver=CreateFrame("Frame")
  driver:RegisterEvent("UNIT_LEVEL")
  driver:RegisterEvent("UNIT_HEALTH")
  driver:RegisterEvent("PARTY_MEMBERS_CHANGED")
  driver:SetScript("OnEvent", function()
    if event=="UNIT_HEALTH" and arg1 and strfind(arg1,"^party[1-4]$") then
      -- Connection/death can change without SetValue changing. The client normally calls this from OnValueChanged.
      local frame=getglobal("PartyMemberFrame"..strsub(arg1,6))
      local was=this this=frame.healthbar PartyMemberHealthCheck() this=was
      return
    end
    for i=1,4 do
      getglobal("PartyMemberFrame"..i.."Level"):SetText(UnitExists("party"..i) and tostring(UnitLevel("party"..i)) or "")
    end
  end)
end
WB.installParty = __WBInstallParty

local fields = {name="UNIT_NAME_UPDATE", level="UNIT_LEVEL", health="UNIT_HEALTH", healthMax="UNIT_MAXHEALTH",
  powerType="UNIT_DISPLAYPOWER", connected="UNIT_HEALTH", dead="UNIT_HEALTH", sex="UNIT_PORTRAIT_UPDATE"}
local function auras(u, rows)
  if rows==nil then return false end -- unknown is left alone; an empty list means none
  local signature={}
  for _,a in ipairs(rows) do
    tinsert(signature, table.concat({tostring(a.skyrim),a.name or "",a.texture or "",a.text or "",
      tostring(a.harmful),a.dispel or "",tostring(a.count or 1)}, "\031"))
  end
  local key=table.concat(signature,"\030")
  if u.partyAuraKey==key then return false end
  u.partyAuraKey=key
  for _,list in ipairs({"buffs","debuffs"}) do
    local kept={}
    for _,a in ipairs(u[list] or {}) do if not a.partySkyrim then tinsert(kept,a) end end
    u[list]=kept
  end
  for _,a in ipairs(rows) do
    local copy={} for k,v in pairs(a) do copy[k]=v end
    copy.partySkyrim=true
    tinsert(u[a.harmful and "debuffs" or "buffs"],copy)
  end
  return true
end

function __WBParty(list)
  local seen, pending, changed = {}, {}, false
  for i,row in ipairs(list) do
    if i>4 then break end
    assert(row.guid and not seen[row.guid], "expected distinct party actor IDs")
    seen[row.guid]=true
  end
  WB.liveParty=true
  __WBInstallParty()
  -- All tokens/membership and values are replaced before any event can inspect the roster.
  for i=1,4 do
    local token="party"..i
    local old=WB.tokens[token]
    if old and WB.units[old] then WB.units[old].inParty=false end
    WB.tokens[token]=nil
  end
  for i,row in ipairs(list) do
    if i>4 then break end
    local token="party"..i
    local u=WB.units[row.guid] or {power=0,powerMax=0,powerType=0,connected=true,classification="normal"}
    WB.units[row.guid]=u
    WB.tokens[token]=row.guid
    u.inParty=true
    local events={}
    for k,e in pairs(fields) do if row[k]~=nil and u[k]~=row[k] then events[e]=true end end
    local connection=row.connected~=nil and u.connected~=row.connected
    local power=row.power~=nil and u.power~=row.power
    local maxPower=row.powerMax~=nil and u.powerMax~=row.powerMax
    for k,v in pairs(row) do if k~="guid" and k~="auras" then u[k]=v end end
    local resource=powerEvents[u.powerType or 0] or "MANA"
    if power or connection or events.UNIT_DISPLAYPOWER then events["UNIT_"..resource]=true end
    if maxPower or events.UNIT_DISPLAYPOWER then events["UNIT_MAX"..resource]=true end
    if auras(u,row.auras) then events.UNIT_AURA=true end
    pending[row.guid]=events
  end
  -- Compare IDs, including order. Value-only updates do not announce a join/leave.
  local roster={}
  for i=1,4 do roster[i]=WB.tokens["party"..i] or false end
  for i=1,4 do if not WB.partyRoster or WB.partyRoster[i]~=roster[i] then changed=true end end
  WB.partyRoster=roster
  WB.partyLeader=GetNumPartyMembers()>0 and WB.tokens.player or nil
  if changed then
    __WBFire("PARTY_MEMBERS_CHANGED")
    __WBFire("PARTY_LEADER_CHANGED")
    -- A target alias also switches between exact and percentage bars when membership changes.
    if WB.tokens.target then __WBFire("UNIT_HEALTH","target") __WBFire("UNIT_MAXHEALTH","target") end
  end
  for guid,events in pairs(pending) do for event in pairs(events) do unitEvent(guid,event) end end
end
