-- Chat bubbles as the 1.12 client draws them (CGChatBubbleFrame, Source\Ui\ChatBubbleFrame.cpp in WoW.exe 5875),
-- over the player and over the people who speak near the player. Read from the client's code:
--   a unit that says something within 20 yards of the player, with ChatBubbles on (PARTY: ChatBubblesParty), gets one
--   bubble, which replaces the one it had (CGUnit's chat bubble);
--   its text is NAMEPLATE_FONT at 0.01 DDC (the screen's diagonal is 1), no outline or shadow, in the chat type's
--   colour, a link reduced to its name; as wide as it is, at least 32 and at most 0.2 DDC, where it wraps;
--   the bubble is 0.01 DDC round the text: ChatBubble-Background/-Backdrop, tiles, edges and insets 16; the tail is
--   ChatBubble-Tail, 16 square, its TOPRIGHT at the bubble's BOTTOM, 4 up;
--   the text's BOTTOM is on the point 0.7 yards over the unit's model (Bridge.cpp: its nameplate row's bx/by; theirs:
--   WB.playerHead, as before);
--   it lasts 2 s and 0.75 s a word (theirs: 1 s and 0.5 s a word), fades in over 0.25 s and out over 0.25 s from 0.25 s
--   past its time; while the unit is beyond 20 yards of the camera or not drawn it fades out and waits (here: no row -
--   off the screen, out of sight or beyond 20 yards); nearer bubbles are drawn over farther ones.
local WB = __WB
local FADE = 0.25
local bubbles, free, frames, npcs = {}, {}, 0, 0 -- by guid; "player" is theirs
local rows = {}
local driver
local function finite(v) return type(v) == "number" and v == v and math.abs(v) < math.huge end
local function ddc(d) local w, h = __WBViewportSize() return d * math.sqrt(w * w + h * h) end

local function create(name)
  local f = CreateFrame("Frame", name, WorldFrame)
  f:SetFrameStrata("LOW")
  f:SetBackdrop({ bgFile = "Interface\\Tooltips\\ChatBubble-Background", edgeFile = "Interface\\Tooltips\\ChatBubble-Backdrop",
    tile = true, tileSize = 16, edgeSize = 16, insets = { left = 16, right = 16, top = 16, bottom = 16 } })
  f.text = f:CreateFontString(name .. "Text", "ARTWORK")
  f.text:SetPoint("CENTER", f, "CENTER", 0, 0) f.text:SetJustifyH("CENTER")
  f.tail = f:CreateTexture(name .. "Tail", "ARTWORK")
  f.tail:SetTexture("Interface\\Tooltips\\ChatBubble-Tail") f.tail:SetWidth(16) f.tail:SetHeight(16)
  f.tail:SetPoint("TOPRIGHT", f, "BOTTOM", 0, 4)
  f:Hide()
  return f
end

local function remove(guid)
  local b = bubbles[guid]
  if not b then return end
  b:Hide() bubbles[guid] = nil
  if not b.own then npcs = npcs - 1 tinsert(free, b) end
end
local function fadeIn(b, now)
  if b.fading == "in" or b.dying then return end
  b.fading, b.fadeAt = "in", now - b.alpha * FADE   -- from where it is: a one-frame dropout must not blink
  b:Show()
end
local function fadeOut(b, now, dying)
  if b.dying then return end
  b.dying = dying
  if b.fading ~= "out" then b.fading, b.fadeAt = "out", now - (1 - b.alpha) * FADE end
end
-- Where its text stands (screen fractions from the top left) and how far it is, or nothing while it is not drawn.
local function anchor(b)
  if b.own then
    local head = WB.playerHead
    if head and finite(head.x) and finite(head.y) then return head.x, head.y, 0 end
    return
  end
  local u = rows[b.guid]
  if u and not u.dead and finite(u.bx) and finite(u.by) then return u.bx, u.by, u.distance or 0 end
end

local function index()
  rows = {}
  for _, u in ipairs(WB.nameplates and WB.nameplates.units or {}) do rows[u.guid] = u end
end

local function update()
  if not next(bubbles) then return end
  local now = GetTime()
  local w, h = __WBViewportSize()
  if npcs > 0 then index() end
  local order = {}
  for guid, b in pairs(bubbles) do
    local x, y, yards = anchor(b)
    if x then if b.alpha < 1 or b.fading == "out" then fadeIn(b, now) end
    elseif b.alpha > 0 or b.fading == "in" then fadeOut(b, now) end
    if b.fading ~= "out" and now - b.start > b.life + FADE then fadeOut(b, now, true) end
    local e = now - b.fadeAt
    if b.fading == "in" then
      if e > FADE then b.alpha, b.fading = 1, nil else b.alpha = e / FADE end
    elseif b.fading == "out" then
      if e <= FADE then b.alpha = 1 - e / FADE
      elseif b.dying then remove(guid)
      else b.alpha, b.fading = 0, nil b:Hide() end
    end
    if bubbles[guid] then
      b:SetAlpha(b.alpha)
      if x then
        b:ClearAllPoints()
        -- Theirs keeps its frame 12 above WB.playerHead (head + 24 units), as before; others' text stands on their point.
        b:SetPoint("BOTTOM", WorldFrame, "BOTTOMLEFT", x * w, (1 - y) * h + (b.own and 12 or -b.pad))
        b.yards = yards
      end
      tinsert(order, b)
    end
  end
  table.sort(order, function(p, q) return (p.yards or 0) > (q.yards or 0) end)
  for i, b in ipairs(order) do
    if b.level ~= i + 1 then b.level = i + 1 b:SetFrameLevel(i + 1) end
  end
end

-- A unit's line over its head, as CGUnit's chat bubble: guid is the host's (an actor's form ID) or "player" for theirs;
-- chatType the chat event's (SAY, YELL, MONSTER_SAY, ...); yards from the player when said.
function __WBChatBubble(guid, text, chatType, yards)
  if guid == nil or type(text) ~= "string" or text == "" or not WorldFrame then return end
  if GetCVar(chatType == "PARTY" and "ChatBubblesParty" or "ChatBubbles") == "0" then return end
  if not finite(yards) or yards > 20 then return end
  local own = guid == "player"
  remove(guid)
  local b
  if own then
    b = WBChatBubble or create("WBChatBubble")
  else
    b = table.remove(free)
    if not b then frames = frames + 1 b = create("WBChatBubble" .. frames) end
    npcs = npcs + 1
  end
  if not driver then
    driver = CreateFrame("Frame", nil, WorldFrame)
    driver:SetScript("OnUpdate", update)
  end
  -- The client copies the line without link codes: "[Linen Cloth]" as a link shows as Linen Cloth.
  text = string.gsub(text, "|c%x%x%x%x%x%x%x%x(|H[^|]*|h%[[^%]]*%]|h)|r", "%1")
  text = string.gsub(text, "|H[^|]*|h%[([^%]]*)%]|h", "%1")
  local color = ChatTypeInfo and ChatTypeInfo[chatType] or {}
  b.pad = ddc(0.01)
  b.text:SetFont(NAMEPLATE_FONT or "Fonts\\FRIZQT__.TTF", b.pad)
  b.text:SetTextColor(color.r or 1, color.g or 1, color.b or 1)
  b.text:SetText(text)
  local natural = b.text:GetStringWidth()
  local width = math.min(math.max(natural, 32), ddc(0.2))
  -- Wrapped only past the widest; a line that fits keeps its own width (one given its exact width can wrap here,
  -- where the drawn glyphs are measured at the screen's pixel size).
  if natural > width then b.text:SetWidth(width) b.text:SetHeight(b.text:GetStringHeight())
  else b.text:SetWidth(0) b.text:SetHeight(0) end
  b:SetWidth(width + 2 * b.pad) b:SetHeight(b.text:GetStringHeight() + 2 * b.pad)
  -- A word is each run of spaces or tabs after something said.
  local _, words = string.gsub(text, "[^ \t][ \t]", "")
  local word = own and 0.5 or 0.75
  b.guid, b.own, b.start, b.life = guid, own, GetTime(), (own and 1 or 2) + word * (words + 1)
  b.alpha, b.fading, b.dying, b.fadeAt, b.level, b.yards = 0, nil, nil, b.start, nil, yards
  b:SetAlpha(0)
  bubbles[guid] = b
  if not own then index() end
  local x, y = anchor(b)
  if x then
    local w, h = __WBViewportSize()
    b:ClearAllPoints() b:SetPoint("BOTTOM", WorldFrame, "BOTTOMLEFT", x * w, (1 - y) * h - b.pad)
    fadeIn(b, b.start)
  end
end
-- The bubble now over a unit (probes and tests), or nil.
function __WBChatBubbleFrame(guid) return bubbles[guid] end

-- Their own SAY and YELL, as the client raises them.
function __WBInstallChatBubble()
  if WBChatBubbleEvents then return end
  local events = CreateFrame("Frame", "WBChatBubbleEvents")
  events:RegisterEvent("CHAT_MSG_SAY") events:RegisterEvent("CHAT_MSG_YELL")
  events:SetScript("OnEvent", function()
    if arg2 == UnitName("player") then __WBChatBubble("player", arg1, event == "CHAT_MSG_YELL" and "YELL" or "SAY", 0) end
  end)
end

-- A load of the same character keeps this session and its clock: bubbles of the world left behind go with it.
function __WBClearChatBubbles()
  for guid in pairs(bubbles) do remove(guid) end
end
