-- WoWBridge: their appearance, chosen as the 1.12 client's character creation chooses it (GlueXML CharacterCreate.xml and
-- .lua), in a window built of that screen's own parts from the archives (__WBGlueTemplates, __WBGlueStrings): its
-- configuration column under the logo, the gender buttons, the customization rows with their arrows, Randomize, and
-- Accept over Back (here Cancel), in the glue fonts and GlueStrings' words. Their figure stands in a glue text panel for
-- the screen's scene, turned by its rotation buttons or by dragging. The choices are 1.12's for their race and sex
-- (__WBCustomization: CharSections, CharacterFacialHairStyles, ChrRaces) and wrap as CycleCharCustomization does; the
-- figure shows each at once (__WBCustomizeLook). Accept tells the host the look (appearance: race, sex, skin, face, hair
-- style, hair colour, facial hair), which keeps it with the save; Cancel or Escape keeps theirs (appearance, nothing).
-- __WBCustomizeAppearance() opens it; the host opens it after WoWBridge is chosen, and for wb appearance.
local WB = __WB
local OPTIONS = { "skin", "face", "hairStyle", "hairColor", "facialHair" }   -- CharacterCustomizationButtonFrame1..5
local frame, strings, ranges
local look, saved, picked, owed   -- the look shown, theirs, each sex's while the window is open; an answer due
local selectedClass, savedClass = 11, 11
local facing = -15   -- degrees, as SetCharacterCreateFacing: CharacterCreate_OnShow's

-- Each race's General tab (service/racials.py RACE_ACTIVES then RACE_PASSIVES): the Druid's Night Elf and Tauren, the
-- Paladin's Human and Dwarf.
local RACIALS = { [4] = {20580,20582,20583,20585}, [6] = {20549,20550,20551,20552},
  [1] = {20600,20597,20598,20599,20864}, [3] = {20594,2481,20595,20596} }
-- The host: their look now, and each sex's packaged look ([0] male, [1] female), where a change of sex starts.
function __WBAppearance(current, defaults, classId, classes, chooseClass)
  WB.appearance = { current = current, defaults = defaults, classId = classId or 11, classes = classes, chooseClass = chooseClass }
  WB.classId, WB.identityClasses = classId or 11, classes
  local class = classes and classes[WB.classId]
  local c = __WBCustomization(current.race, current.sex)
  if WB.units and WB.units[WB.tokens.player] then
    WB.units[WB.tokens.player].race, WB.units[WB.tokens.player].raceToken = c.raceName, c.raceToken
    WB.units[WB.tokens.player].sex = current.sex + 2
    if class then
      WB.units[WB.tokens.player].class, WB.units[WB.tokens.player].classToken = class.name, class.token
    end
  end
  if WB.classId ~= 11 then __WBEmptyClass() end   -- another class's kit goes; a Paladin's own book stays
  -- The native look publication owns the General racials; class-book refreshes preserve General.
  local allowed, all = {}, {}
  for _, ids in pairs(RACIALS) do for _, id in ipairs(ids) do all[id] = true end end
  for _, id in ipairs(RACIALS[current.race] or {}) do allowed[id] = true end
  local book = {}
  for _, id in ipairs(WB.spellbook or {}) do if not all[id] then tinsert(book,id) end end
  for _, id in ipairs(RACIALS[current.race] or {}) do tinsert(book,id) end
  WB.spellbook, WB.spellbookDirty = book, true
  for slot, action in pairs(WB.actions or {}) do if all[action.spell] and not allowed[action.spell] then WB.actions[slot] = nil end end
  __WBFire("SPELLS_CHANGED") __WBFire("ACTIONBAR_SLOT_CHANGED",0)
  __WBFire("UNIT_PORTRAIT_UPDATE","player")
  if class then __WBFire("UNIT_LEVEL","player") end
end

local function copy(t) local c = {} for k, v in pairs(t) do c[k] = v end return c end
local function count(option)
  local n = ranges[option]
  if option == "face" then n = n[look.skin] elseif option == "hairColor" then n = n[look.hairStyle] end
  return n or 0
end
-- A face or hair colour the new skin colour or style lacks comes round into its range; then their figure shows it.
local function fit()
  for _, o in ipairs(OPTIONS) do
    local n = count(o)
    look[o] = n > 0 and math.mod(look[o] or 0, n) or 0
  end
  if __WBCustomizeLook then __WBCustomizeLook(look) end
end
-- SetCharacterGender's display: the chosen sex's button checked with its name, and the race's words for that sex's hair
-- and facial hair (CharacterCreate_UpdateHairCustomization, _UpdateFacialHairCustomization; NONE hides the row).
local function eligible(id)
  local c = WB.identityClasses and WB.identityClasses[selectedClass]
  return c and c.races[id] or not c and selectedClass == 11 and (id == 4 or id == 6)
end
local function show()
  for _, id in ipairs({11,2}) do
    local b = getglobal("WBCustomizeClass" .. id)
    local c = WB.identityClasses and WB.identityClasses[id]
    if c then
      if WB.appearance and WB.appearance.chooseClass then b:Enable() else b:Disable() end
      b.tooltip = c.name
      b:GetNormalTexture():SetTexCoord(unpack(strings.CLASS_ICON_TCOORDS[c.token]))
    else b:Disable() end
    local mine = id == selectedClass
    b:SetChecked(mine and 1 or nil)
    if mine then b:LockHighlight() else b:UnlockHighlight() end
    getglobal(b:GetName() .. "HighlightText"):SetText(mine and c and c.name or "")
  end
  for _, race in ipairs({1,3,7,4,2,5,8,6}) do
    local b = getglobal("WBCustomizeRace" .. race)
    local c = __WBCustomization(race, look.sex)
    local coords = strings.RACE_ICON_TCOORDS[strupper(c.raceToken) .. (look.sex == 1 and "_FEMALE" or "_MALE")]
    getglobal(b:GetName() .. "NormalTexture"):SetTexCoord(unpack(coords))
    if b:GetDisabledTexture() then b:GetDisabledTexture():SetTexCoord(unpack(coords)) end
    b.tooltip = c.raceName
    if eligible(race) then b:Enable() else b:Disable() end
    local mine = race == look.race
    b:SetChecked(mine and 1 or nil)
    if mine then b:LockHighlight() else b:UnlockHighlight() end
    getglobal(b:GetName() .. "HighlightText"):SetText(mine and c.raceName or "")
  end
  local colors = strings.FACTION_BACKDROP_COLOR_TABLE[look.race == 6 and "Horde" or "Alliance"]
  WBCustomizeScene:SetBackdropBorderColor(colors[1], colors[2], colors[3])
  WBCustomizeScene:SetBackdropColor(colors[4], colors[5], colors[6])
  for sex, button in pairs({ [0] = WBCustomizeGenderMale, [1] = WBCustomizeGenderFemale }) do
    local mine = sex == look.sex
    button:SetChecked(mine and 1 or nil)
    if mine then button:LockHighlight() else button:UnlockHighlight() end
    getglobal(button:GetName() .. "HighlightText"):SetText(mine and (sex == 1 and strings.FEMALE or strings.MALE) or "")
  end
  WBCustomizeOption3Text:SetText(strings["HAIR_" .. ranges.hairCustomization .. "_STYLE"])
  WBCustomizeOption4Text:SetText(strings["HAIR_" .. ranges.hairCustomization .. "_COLOR"])
  if ranges.facialHairCustomization == "NONE" then
    WBCustomizeOption5:Hide()
  else
    WBCustomizeOption5Text:SetText(strings["FACIAL_HAIR_" .. ranges.facialHairCustomization])
    WBCustomizeOption5:Show()
  end
end
local function gender(sex)
  picked[look.sex] = look
  local defaults = WB.appearance and WB.appearance.defaults
  defaults = defaults and (defaults[look.race] or defaults)
  look = picked[sex] or copy(sex == saved.sex and look.race == saved.race and saved or defaults and defaults[sex] or { race = look.race })
  look.sex = sex
  ranges = __WBCustomization(look.race, sex)
  fit() show()
end
local function race(id)
  if not eligible(id) then return end
  if id == look.race then show() return end
  local defaults = WB.appearance and WB.appearance.defaults
  defaults = defaults and defaults[id]
  if not defaults or not defaults[look.sex] then return end
  look = copy(defaults[look.sex])
  picked = {}   -- changing race always starts at that race's packaged default, including a return
  ranges = __WBCustomization(look.race, look.sex)
  fit() show()
end
local function class(id)
  if not WB.appearance or not WB.appearance.chooseClass then return end
  local c = WB.identityClasses and WB.identityClasses[id]
  if not c then return end
  selectedClass = id
  if not eligible(look.race) then
    for _, r in ipairs({1,3,4,6}) do if eligible(r) then race(r) break end end
  end
  show()
end
local function turn(degrees)
  facing = facing + degrees
  WBCustomizeModel:SetRotation(math.rad(facing))
end
-- CycleCharCustomization(id, delta): the row's choice, round from the last to the first and back.
local function cycle(id, delta)
  local o = OPTIONS[id]
  local n = count(o)
  if n > 0 then look[o] = math.mod(look[o] + delta + n, n) end
  fit()
end
function CharacterCustomization_Left(id) PlaySound("gsCharacterCreationLook") cycle(id, -1) end
function CharacterCustomization_Right(id) PlaySound("gsCharacterCreationLook") cycle(id, 1) end
local function randomize()   -- RandomizeCharCustomization: each row, the sex kept
  PlaySound("gsCharacterCreationLook")
  for _, o in ipairs(OPTIONS) do
    local n = count(o)
    look[o] = n > 0 and math.random(n) - 1 or 0
  end
  fit()
end

local function create()
  assert(__WBGlueTemplates())
  strings = __WBGlueStrings() or {}
  frame = CreateFrame("Frame", "WBCustomizeFrame", UIParent)
  frame:SetWidth(720) frame:SetHeight(660) frame:SetPoint("CENTER", UIParent, "CENTER", 0, 20)
  frame:SetFrameStrata("DIALOG") frame:EnableMouse(true) frame:Hide()
  -- CharacterCreateConfigurationFrame: its background and outer border, as long as the rows it keeps, under the logo.
  local column = CreateFrame("Frame", "WBCustomizeConfiguration", frame)
  column:SetWidth(206) column:SetHeight(590) column:SetPoint("TOPLEFT", frame, "TOPLEFT", 25, -67)
  local art = "Interface\\Glues\\CharacterCreate\\"
  local top = column:CreateTexture("WBCustomizeOuterBorder1", "ARTWORK")
  top:SetTexture(art .. "UI-CharacterCreate-OuterBorder") top:SetWidth(224) top:SetHeight(140)
  top:SetPoint("TOP", column, "TOP", 0, 0) top:SetTexCoord(0, 0.875, 0, 140 / 256)
  local bottom = column:CreateTexture("WBCustomizeOuterBorder3", "ARTWORK")
  bottom:SetTexture(art .. "UI-CharacterCreate-OuterBorder") bottom:SetWidth(224) bottom:SetHeight(450)
  bottom:SetPoint("TOP", top, "BOTTOM", 0, 0) bottom:SetTexCoord(0, 0.875, 0.1796875, 1)
  local back = column:CreateTexture("WBCustomizeBackground", "BACKGROUND")
  back:SetTexture(art .. "UI-CharacterCreate-Background") back:SetWidth(218)
  back:SetPoint("TOPLEFT", top, "TOPLEFT", 6, 0) back:SetPoint("BOTTOMLEFT", bottom, "BOTTOMLEFT", 0, 6)
  local logo = CreateFrame("Frame", "WBCustomizeLogo", column)
  logo:SetWidth(256) logo:SetHeight(128) logo:SetPoint("TOPLEFT", frame, "TOPLEFT", 0, 0)
  local wow = logo:CreateTexture("WBCustomizeLogoTexture", "ARTWORK")
  wow:SetTexture("Interface\\Glues\\Common\\Glues-WoW-Logo") wow:SetAllPoints(logo)
  -- GlueXML's two columns of four race icons; the selected class owns eligibility.
  for i, id in ipairs({1,3,7,4,2,5,8,6}) do
    local b = CreateFrame("CheckButton", "WBCustomizeRace" .. id, column, "CharacterCreateRaceButtonTemplate")
    b:SetPoint("TOPLEFT", column, "TOPLEFT", i <= 4 and 33 or 127, -68 - math.mod(i - 1, 4) * 53)
    do
      b:SetDisabledTexture(art .. "UI-CharacterCreate-Races")
      b:GetDisabledTexture():SetVertexColor(.4,.4,.4)
      b:Disable()
    end
    b:SetScript("OnClick", function() if eligible(id) then race(id) PlaySound("gsCharacterCreationClass") else show() end GameTooltip:Hide() end)
    b:SetScript("OnEnter", function()
      if look.race == id then return end
      GameTooltip:SetOwner(b, "ANCHOR_RIGHT") GameTooltip:SetText(b.tooltip,1,1,1)
    end)
    b:SetScript("OnLeave", function() GameTooltip:Hide() end)
  end
  for sex, name in pairs({ [0] = "Male", [1] = "Female" }) do
    local b = CreateFrame("CheckButton", "WBCustomizeGender" .. name, column, "CharacterCreateGenderButtonTemplate")
    getglobal(b:GetName() .. "NormalTexture"):SetTexCoord(sex * 0.5, sex * 0.5 + 0.5, 0, 1)
    b:SetScript("OnClick", function() gender(sex) PlaySound("gsCharacterCreationClass") end)
    b:SetScript("OnEnter", function()
      if look.sex == sex then return end
      GameTooltip:SetOwner(b, "ANCHOR_RIGHT") GameTooltip:SetText(sex == 1 and strings.FEMALE or strings.MALE, 1, 1, 1)
    end)
    b:SetScript("OnLeave", function() GameTooltip:Hide() end)
  end
  WBCustomizeGenderMale:SetPoint("TOPRIGHT", column, "TOP", -3, -303)
  WBCustomizeGenderFemale:SetPoint("LEFT", WBCustomizeGenderMale, "RIGHT", 5, 0)
  for i = 1, 5 do
    local row = CreateFrame("Frame", "WBCustomizeOption" .. i, column, "CharacterCustomizationFrameTemplate")
    row:SetID(i)
    row:SetPoint("TOP", i == 1 and column or getglobal("WBCustomizeOption" .. (i - 1)), i == 1 and "TOP" or "BOTTOM", 0, i == 1 and -369 or 0)
    getglobal(row:GetName() .. "Text"):SetText(strings["CHAR_CUSTOMIZATION" .. i .. "_DESC"])
  end
  local dice = CreateFrame("Button", "WBCustomizeRandomize", column, "GlueButtonSmallTemplate")
  dice:SetText(strings.RANDOMIZE) dice:SetHeight(30) dice:SetWidth(dice:GetTextWidth() + 50)
  dice:SetPoint("TOP", WBCustomizeOption5, "BOTTOM", 0, -5)
  dice:SetScript("OnClick", randomize)
  -- The scene: the race and class panels' glue backdrop in the Alliance colour, their figure, the screen's rotation buttons
  -- at its foot and Accept over Back at its corner, where the screen has them.
  local scene = CreateFrame("Frame", "WBCustomizeScene", frame)
  scene:SetWidth(460) scene:SetHeight(627) scene:SetPoint("TOPLEFT", frame, "TOPLEFT", 258, -30)
  scene:SetBackdrop({ bgFile = "Interface\\Tooltips\\UI-Tooltip-Background", edgeFile = "Interface\\Glues\\Common\\TextPanel-Border",
    tile = true, tileSize = 32, edgeSize = 32, insets = { left = 8, right = 4, top = 4, bottom = 8 } })
  scene:SetBackdropColor(0.09, 0.09, 0.19)
  -- CharacterCreateClassButtonTemplate's base and class art (the host imports its icon base).
  for i, id in ipairs({11,2}) do
    local b = CreateFrame("CheckButton", "WBCustomizeClass" .. id, scene, "CharacterCreateIconButtonTemplate")
    b:SetPoint("TOPLEFT", scene, "TOPLEFT", 145 + (i-1)*94, -12)
    b:SetNormalTexture(art .. "UI-CharacterCreate-Classes")
    local shadow = b:CreateTexture(nil,"BACKGROUND")
    shadow:SetTexture(art .. "UI-CharacterCreate-IconShadow") shadow:SetWidth(64) shadow:SetHeight(64)
    shadow:SetPoint("CENTER",b,"CENTER",2,-2)
    b:SetScript("OnClick",function() class(id) PlaySound("gsCharacterCreationClass") end)
    b:SetScript("OnEnter",function()
      if b.tooltip then GameTooltip:SetOwner(b,"ANCHOR_RIGHT") GameTooltip:SetText(b.tooltip,1,1,1) end
    end)
    b:SetScript("OnLeave",function() GameTooltip:Hide() end)
  end
  local model = CreateFrame("PlayerModel", "WBCustomizeModel", scene)
  model:SetWidth(372) model:SetHeight(500) model:SetPoint("TOP", scene, "TOP", 0, -66)
  model:SetModel("__customize") model:EnableMouse(true)
  -- CharacterCreateFrame_OnMouseDown/Up/OnUpdate: dragging turns the player, CHARACTER_ROTATION_CONSTANT degrees a pixel.
  model:SetScript("OnMouseDown", function() if arg1 == "LeftButton" then model.dragX = GetCursorPosition() end end)
  model:SetScript("OnMouseUp", function() if arg1 == "LeftButton" then model.dragX = nil end end)
  model:SetScript("OnUpdate", function()
    if not model.dragX then return end
    local x = GetCursorPosition()
    turn((x - model.dragX) * 0.6) model.dragX = x
  end)
  local function rotation(name, step)   -- CharacterCreateRotateLeft/Right: CHARACTER_FACING_INCREMENT a frame while held
    local b = CreateFrame("Button", name, scene)
    b:SetWidth(50) b:SetHeight(50) b:SetHitRectInsets(13, 13, 10, 13) b:SetFrameLevel(model:GetFrameLevel() + 1)
    b:SetNormalTexture(art .. "UI-RotationRight-Big-Up") b:SetPushedTexture(art .. "UI-RotationRight-Big-Down")
    if step < 0 then b:GetNormalTexture():SetTexCoord(1, 0, 0, 1) b:GetPushedTexture():SetTexCoord(1, 0, 0, 1) end
    b:SetHighlightTexture("Interface\\Buttons\\UI-Common-MouseHilight")
    local glow = b:GetHighlightTexture()
    glow:SetBlendMode("ADD") glow:ClearAllPoints() glow:SetWidth(30) glow:SetHeight(30) glow:SetPoint("CENTER", b, "CENTER", 0, 0)
    b:SetScript("OnUpdate", function() if b:GetButtonState() == "PUSHED" then turn(step) end end)
    return b
  end
  rotation("WBCustomizeRotateLeft", -2):SetPoint("BOTTOMLEFT", scene, "BOTTOMLEFT", 12, 8)
  rotation("WBCustomizeRotateRight", 2):SetPoint("LEFT", WBCustomizeRotateLeft, "RIGHT", -19, 0)
  local cancel = CreateFrame("Button", "WBCustomizeCancel", scene, "GlueButtonSmallTemplate")
  cancel:SetWidth(120) cancel:SetHeight(30) cancel:SetText(strings.CANCEL) cancel:SetFrameLevel(model:GetFrameLevel() + 1)
  cancel:SetPoint("BOTTOMRIGHT", scene, "BOTTOMRIGHT", -34, 16)
  cancel:SetScript("OnClick", function()
    PlaySound("gsCharacterCreationCancel")
    owed = nil WB.host("appearance") frame:Hide()
  end)
  local accept = CreateFrame("Button", "WBCustomizeAccept", scene, "GlueButtonTemplate")
  accept:SetWidth(160) accept:SetHeight(35) accept:SetText(strings.CHARACTER_CREATE_ACCEPT) accept:SetFrameLevel(model:GetFrameLevel() + 1)
  accept:SetPoint("BOTTOM", cancel, "TOP", 0, 5)
  accept:SetScript("OnClick", function()
    PlaySound("gsCharacterCreationCreateChar")
    owed = nil
    if selectedClass == savedClass then
      WB.host("appearance", look.race, look.sex, look.skin, look.face, look.hairStyle, look.hairColor, look.facialHair)
    else
      WB.host("testCommand","appearance",tostring(selectedClass),table.concat({look.race,look.sex,look.skin,look.face,look.hairStyle,look.hairColor,look.facialHair},","))
    end
    frame:Hide()
  end)
  -- Escape (CloseWindows hides UISpecialFrames), as Cancel: the player keeps their look. The interface runs OnHide on its next frame, by when
  -- the window may be open again.
  frame:SetScript("OnHide", function()
    if owed and not frame:IsShown() then owed = nil WB.host("appearance") end
  end)
  tinsert(UISpecialFrames, "WBCustomizeFrame")
end

-- Opens the window on their look (the host's __WBAppearance), as the player has it; again, it starts over.
function __WBCustomizeAppearance()
  if not frame then create() end
  local a = WB.appearance or {}
  saved = copy(a.current or { race = 4, sex = 1, skin = 0, face = 0, hairStyle = 0, hairColor = 0, facialHair = 0 })
  selectedClass, savedClass = a.classId or 11, a.classId or 11
  look, picked, owed = copy(saved), {}, true
  ranges = __WBCustomization(look.race, look.sex)
  facing = -15 turn(0)
  fit() show()
  frame:Show()
end
-- The host: the game it was opened for is gone (a load, a new game). Nothing is owed for it.
function __WBCustomizeClose()
  if frame then owed = nil frame:Hide() end
end
