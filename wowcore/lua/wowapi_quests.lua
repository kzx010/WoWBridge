-- WoWBridge: their Skyrim quests in the client's Quest Log and quest watch (Quests.cpp). Loaded after the recorded
-- answers (wowapi_recorded.lua), whose empty quest log these replace.
local WB = __WB
local function fire(...) __WBFire(...) end

------------------------------------------------------------------------------------------------------ their quests
-- Their Skyrim quests in the client's Quest Log and quest watch (Quests.cpp): __WBQuests(list), each quest
-- { id, title, header, text, objectives = { { text, done, failed } }, failed }, grouped under its header (Skyrim's
-- kind of quest, as the client groups by zone) in the order given. Skyrim's quests have no level: theirs is used (the
-- client's yellow). The log's selection, collapsed headers and watches are the interface's, kept by quest id.
WB.quests, WB.questEntries, WB.questCollapsed, WB.questWatched = {}, {}, {}, {}
local function questEntries()
  local entries, seen, order = {}, {}, {}
  for _, q in ipairs(WB.quests) do
    if not seen[q.header] then seen[q.header] = {} tinsert(order, q.header) end
    tinsert(seen[q.header], q)
  end
  for _, h in ipairs(order) do
    tinsert(entries, { header = h })
    if not WB.questCollapsed[h] then
      for _, q in ipairs(seen[h]) do tinsert(entries, { quest = q }) end
    end
  end
  WB.questEntries = entries
end
local function questAt(index)
  local e = WB.questEntries[index or WB.questSelected or 0]
  return e and e.quest
end
local function complete(q)
  if q.failed then return -1 end
  for _, o in ipairs(q.objectives) do if o.failed then return -1 end end
  local n = 0
  for _, o in ipairs(q.objectives) do if not o.done then return nil end n = n + 1 end
  return n > 0 and 1 or nil
end

function __WBInstallQuestLog()
  QUEST_LOG_COUNT_TEMPLATE = "%d"
end
function __WBQuests(list)
  -- Install for live quests, including explicit render fixtures.
  __WBInstallQuestLog()
  WB.quests = list
  local live = {}
  for _, q in ipairs(list) do live[q.id] = true end
  for id in pairs(WB.questWatched) do if not live[id] then WB.questWatched[id] = nil end end
  local selected = questAt()
  questEntries()
  WB.questSelected = nil
  for i, e in ipairs(WB.questEntries) do if selected and e.quest and e.quest.id == selected.id then WB.questSelected = i end end
  fire("QUEST_LOG_UPDATE")
end

function GetNumQuestLogEntries()
  local quests = 0
  for _, e in ipairs(WB.questEntries) do if e.quest then quests = quests + 1 end end
  return table.getn(WB.questEntries), table.getn(WB.quests)
end
function GetQuestLogTitle(index)
  if not index then error("Usage: GetQuestLogTitle(index)", 2) end
  local e = WB.questEntries[index]
  if not e then return nil, 0, nil, nil, nil, nil end   -- the client's answer past the log's end
  if e.header then return e.header, 0, nil, 1, WB.questCollapsed[e.header] and 1 or nil, nil end
  return e.quest.title, UnitLevel("player"), nil, nil, nil, complete(e.quest)
end
function SelectQuestLogEntry(index) WB.questSelected = WB.questEntries[index] and index or nil end
function GetQuestLogSelection() return WB.questSelected or 0 end
function ExpandQuestHeader(index)
  local e = WB.questEntries[index]
  if index == 0 then WB.questCollapsed = {} elseif e and e.header then WB.questCollapsed[e.header] = nil end
  questEntries() fire("QUEST_LOG_UPDATE")
end
function CollapseQuestHeader(index)
  local e = WB.questEntries[index]
  if index == 0 then for _, x in ipairs(WB.questEntries) do if x.header then WB.questCollapsed[x.header] = true end end
  elseif e and e.header then WB.questCollapsed[e.header] = true end
  questEntries() fire("QUEST_LOG_UPDATE")
end
function GetQuestLogQuestText()
  local q = questAt()
  if not q then return nil, nil end
  local lines = {}
  for _, o in ipairs(q.objectives) do if not o.done then tinsert(lines, o.text) end end
  return q.text or "", table.concat(lines, "\n")
end
function GetNumQuestLeaderBoards(index)
  local q = questAt(index)
  return q and table.getn(q.objectives) or 0
end
function GetQuestLogLeaderBoard(i, index)
  if not i then error("Usage: GetQuestLogLeaderBoard(index)", 2) end
  local q = questAt(index)
  local o = q and q.objectives[i]
  if not o then return nil, nil, nil end
  return (o.failed and "[Failed] " or "") .. o.text, "event", o.done and 1 or nil
end
function IsCurrentQuestFailed() local q = questAt() return q and complete(q) == -1 and 1 or nil end
function GetQuestLogTimeLeft() return nil end
function GetQuestLogPushable() return nil end
function GetQuestLogRequiredMoney() return 0 end
function GetQuestLogRewardMoney() return 0 end
function GetQuestLogRewardSpell() return nil, nil, nil end
function GetNumQuestLogRewards() return 0 end
function GetNumQuestLogChoices() return 0 end
function GetQuestLogGroupNum() return 0 end
function GetQuestGreenRange() return 12 end   -- the client's answer at their level
function GetAbandonQuestName() local q = questAt() return q and q.title end
function SetAbandonQuest() end
function AbandonQuest() end   -- Skyrim's quests cannot be abandoned: the log's Abandon button is disabled for them
function QuestLogPushQuest() end
function IsQuestWatched(index)
  if not index then error("Usage: IsQuestWatched(index)", 2) end
  local q = questAt(index)
  return q and WB.questWatched[q.id] and 1 or nil
end
function AddQuestWatch(index)
  local q = questAt(index)
  if q and GetNumQuestWatches() < MAX_WATCHABLE_QUESTS then WB.questWatched[q.id] = true WB.host("questWatch", q.id, 1) end
end
function RemoveQuestWatch(index)
  local q = questAt(index)
  if q then WB.questWatched[q.id] = nil WB.host("questWatch", q.id, 0) end
end
function GetNumQuestWatches()
  local n = 0
  for _, e in ipairs(WB.questEntries) do if e.quest and WB.questWatched[e.quest.id] then n = n + 1 end end
  return n
end
function GetQuestIndexForWatch(watch)
  if not watch then error("Usage: GetQuestIndexForWatch(index)", 2) end
  local n = 0
  for i, e in ipairs(WB.questEntries) do
    if e.quest and WB.questWatched[e.quest.id] then n = n + 1 if n == watch then return i end end
  end
  return nil
end
