-- WoWBridge: Model frames as the 1.12 client runs them. The interface's one model on the HUD is the cooldown
-- (CooldownFrameTemplate: <Model file="Interface\Cooldown\UI-Cooldown-Indicator.mdx">; Cooldown.lua sets the sweep's
-- time with SetSequenceTime as the cooldown runs, then SetSequence(1) and AdvanceTime for the flash, and hides it from
-- OnAnimFinished). Each frame keeps its model, animation and time here and tells the host (__WBModelState), which
-- runs the M2 and draws it; the client calls OnUpdateModel every frame for a shown model, and OnAnimFinished when an
-- advancing animation reaches its end.

local MT = __WoweeFrameMT
local models = {}   -- frame -> { file, anim, ms, advancing, finished }
local elapsed = 0   -- this frame's seconds, for AdvanceTime

local function state(f)
  local s = models[f]
  if not s then s = { anim = 0, ms = 0 } models[f] = s end
  return s
end
-- A host without a renderer (the API tests) runs the scripts and draws nothing.
local function publish(f, s) if __WBModelState then __WBModelState(f, s.file or "", s.anim, s.ms, s.rotation) end end

function MT:SetModel(file)
  local s = state(self)
  s.file = file and string.gsub(file, "%.[mM][dD][xX]$", ".m2")
  publish(self, s)
end
local oldSetUnit,oldRefreshUnit = MT.SetUnit,MT.RefreshUnit
function MT:SetUnit(unit)
  if self:GetName()=="CharacterModelFrame" and unit=="player" then
    local s=state(self) s.file="__player" publish(self,s)
  elseif self:GetName()=="DressUpModel" and unit=="player" then
    if __WBDressLook then __WBDressLook(true) end
    local s=state(self) s.file="__dress" publish(self,s)
  elseif oldSetUnit then return oldSetUnit(self,unit) end
end
function MT:RefreshUnit()
  if self:GetName()=="CharacterModelFrame" or self:GetName()=="DressUpModel" then self:SetUnit("player")
  elseif oldRefreshUnit then return oldRefreshUnit(self) end
end
local oldDress,oldUndress,oldTryOn=MT.Dress,MT.Undress,MT.TryOn
function MT:Dress()
  if self:GetName()=="DressUpModel" then self:SetUnit("player")
  elseif oldDress then return oldDress(self) end
end
function MT:Undress()
  if self:GetName()=="DressUpModel" then
    if __WBDressLook then __WBDressLook(false) end
  elseif oldUndress then return oldUndress(self) end
end
function MT:TryOn(link)
  if self:GetName()~="DressUpModel" then if oldTryOn then return oldTryOn(self,link) end return end
  local id=tonumber(link) or tonumber(string.match(link or "","item:(%d+):"))
  local i=id and __WB.dressItems and __WB.dressItems[id]
  local slots={[1]=0,[2]=1,[3]=2,[4]=3,[5]=4,[6]=5,[7]=6,[8]=7,[9]=8,[10]=9,[11]=10,[12]=12,
    [13]=15,[14]=16,[15]=17,[16]=14,[17]=15,[19]=18,[20]=4,[21]=15,[22]=16,[23]=16,[25]=17,[26]=17,[28]=17}
  local slot=i and slots[i.type]
  if slot and __WBDressTryOn then return __WBDressTryOn(slot,i.display,i.sheath,i.type==17) end
  __WBFire("UI_ERROR_MESSAGE","This item has no supported dressing-room appearance.")
end
function MT:SetRotation(rotation)
  local s=state(self) s.rotation=rotation publish(self,s)
end
function MT:GetRotation() return state(self).rotation or 0 end
function MT:GetModel() local s = models[self] return s and s.file end
function MT:ClearModel() local s = state(self) s.file = nil publish(self, s) end
function MT:SetSequence(anim)
  local s = state(self)
  s.anim, s.ms, s.advancing, s.finished = anim or 0, 0, nil, nil
  publish(self, s)
end
function MT:SetSequenceTime(anim, ms)
  local s = state(self)
  s.anim, s.ms, s.advancing, s.finished = anim or 0, ms or 0, nil, nil
  publish(self, s)
end
-- The animation moves on by this frame's time; at its end the frame hears OnAnimFinished, once.
function MT:AdvanceTime()
  local s = state(self)
  s.ms = s.ms + elapsed * 1000
  local length = s.file and __WBModelDuration and __WBModelDuration(s.file, s.anim)
  if length and s.ms >= length then
    s.ms = length
    if not s.finished then
      s.finished = true
      local finished = self:GetScript("OnAnimFinished")
      if finished then
        local was = this
        this = self
        finished(self)
        this = was
      end
    end
  end
  publish(self, s)
end

-- SetPortraitTexture(texture, unit): the unit's portrait in that texture. The host fills every texture so marked with the
-- unit's live render each frame - her face in the player frame, the character micro button, the character sheet - and
-- leaves a unit with no render (a creature) empty. (WoWee's own binding is in game-state code WoWBridge leaves out.)
function SetPortraitTexture(texture, unit)
  if type(texture) == "table" and unit and __WBPortraitUnit then __WBPortraitUnit(texture, strlower(unit)) end
end

-- Every frame, before the interface is drawn: each shown model's OnUpdateModel (WoWee runs OnUpdate only).
local driver = CreateFrame("Frame")
driver:SetScript("OnUpdate", function()
  elapsed = arg1 or 0
  for f, s in pairs(models) do
    if s.file and f:IsVisible() then
      local update = f:GetScript("OnUpdateModel")
      if update then
        local was = this
        this = f
        update(f)
        this = was
      end
    end
  end
end)
