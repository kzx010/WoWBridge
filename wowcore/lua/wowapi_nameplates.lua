-- 1.12 builds its nameplates natively, not from an XML template. Its unchanged Bindings.xml and
-- UpdateNameplates call these four client functions. Keep the empty client's functions until live data arrives.
local WB = __WB
local pool, byGuid, installed = {}, {}, false -- widgets stay out of __WBReloadStateText
local refresh
local function finite(v) return type(v)=="number" and v==v and math.abs(v)<math.huge end
local function create(index)
  local name="WBNameplate"..index
  local f=CreateFrame("Button",name,WorldFrame)
  f:SetFrameStrata("WORLD") f:SetFrameLevel(1)
  f:SetWidth(128) f:SetHeight(36)
  f:RegisterForClicks("LeftButtonUp","RightButtonUp")
  f.health=CreateFrame("StatusBar",name.."Health",f)
  f.health:SetFrameStrata("WORLD") f.health:SetFrameLevel(1)
  f.health:SetWidth(101) f.health:SetHeight(9) f.health:SetPoint("BOTTOMLEFT",f,"BOTTOMLEFT",5,5)
  f.health:SetStatusBarTexture("Interface\\TargetingFrame\\UI-StatusBar")
  local back=f:CreateTexture(name.."Background","BACKGROUND")
  back:SetTexture(0,0,0,0.85) back:SetAllPoints(f.health)
  -- The archive border includes the level box at its right. Its top half is transparent.
  f.border=f.health:CreateTexture(name.."Border","OVERLAY")
  f.border:SetTexture("Interface\\Tooltips\\Nameplate-Border")
  f.border:SetWidth(128) f.border:SetHeight(32) f.border:SetPoint("BOTTOMLEFT",f,"BOTTOMLEFT",0,0)
  f.glow=f.health:CreateTexture(name.."Glow","OVERLAY")
  f.glow:SetTexture("Interface\\Tooltips\\Nameplate-Glow") f.glow:SetBlendMode("ADD")
  f.glow:SetWidth(128) f.glow:SetHeight(32) f.glow:SetPoint("BOTTOMLEFT",f,"BOTTOMLEFT",0,0) f.glow:Hide()
  f.name=f.health:CreateFontString(name.."Name","OVERLAY")
  f.name:SetFont("Fonts\\FRIZQT__.TTF",12,"OUTLINE")
  f.name:SetPoint("BOTTOM",f,"BOTTOM",0,20) f.name:SetTextColor(1,1,1)
  f.level=f.health:CreateFontString(name.."Level","OVERLAY")
  f.level:SetFont("Fonts\\FRIZQT__.TTF",10,"OUTLINE") f.level:SetPoint("CENTER",f,"BOTTOMLEFT",117,9)
  f.skull=f.health:CreateTexture(name.."Skull","OVERLAY")
  f.skull:SetTexture("Interface\\TargetingFrame\\UI-TargetingFrame-Skull")
  f.skull:SetWidth(16) f.skull:SetHeight(16) f.skull:SetPoint("CENTER",f.level,"CENTER",0,0)
  f:SetScript("OnMouseDown",function() this.pressedGuid,this.pressedEpoch=this.guid,this.epoch end)
  f:SetScript("OnClick",function()
    local guid,epoch=this.pressedGuid or this.guid,this.pressedEpoch or this.epoch
    this.pressedGuid,this.pressedEpoch=nil,nil
    -- The pool may have been reused between press and release. Never target its replacement actor.
    if guid==this.guid and epoch==this.epoch and this:IsVisible() then WB.host("nameplateTarget",guid,epoch) end
  end)
  f:SetScript("OnEnter",function() this.hovered=true this.glow:Show() end)
  f:SetScript("OnLeave",function() this.hovered=false if this.guid~=WB.tokens.target then this.glow:Hide() end end)
  return f
end
refresh=function()
  if not installed or not WB.nameplates then return end
  byGuid={}
  local count=0
  local width,height=__WBViewportSize()
  for _,u in ipairs(WB.nameplates.units or {}) do
    local friendly=(u.reaction or 4)>4
    if (friendly and WB.plateFriends or not friendly and WB.plateEnemies) and
       finite(u.x) and finite(u.y) and u.x>=0 and u.x<=1 and u.y>=0 and u.y<=1 and
       finite(u.distance) and u.distance<=20 and u.distance>=0 and not u.dead and
       finite(u.health) and finite(u.healthMax) and u.health>0 and u.healthMax>0 then
      count=count+1
      local f=pool[count] or create(count) pool[count]=f
      f.guid,f.epoch=u.guid,WB.nameplates.epoch byGuid[u.guid]=f
      f:ClearAllPoints()
      f:SetPoint("BOTTOM",WorldFrame,"BOTTOMLEFT",u.x*width,(1-u.y)*height+6)
      f.name:SetText(u.name or "")
      local r,g,b=1,0,0
      if friendly then r,g,b=0,1,0 elseif (u.reaction or 4)==4 then r,g,b=1,1,0 end
      f.health:SetStatusBarColor(r,g,b)
      f.health:SetMinMaxValues(0,u.healthMax) f.health:SetValue(math.min(u.health,u.healthMax))
      local elite=u.classification=="elite" or u.classification=="rareelite" or u.classification=="worldboss"
      local level=u.level or 0
      f.level:SetFont("Fonts\\FRIZQT__.TTF",(elite or level>=100) and 8 or 10,"OUTLINE")
      f.level:SetText(tostring(level)..(elite and "+" or ""))
      local color=u.attackable and GetDifficultyColor(level) or {r=1,g=0.82,b=0}
      f.level:SetTextColor(color.r,color.g,color.b)
      if level<=0 then f.level:Hide() f.skull:Show() else f.level:Show() f.skull:Hide() end
      if u.classification=="rare" or u.classification=="rareelite" then f.border:SetVertexColor(0.75,0.75,0.75)
      else f.border:SetVertexColor(1,1,1) end
      f:Show()
      if u.guid==WB.tokens.target or f.hovered then f.glow:Show() else f.glow:Hide() end
    end
  end
  for i=count+1,table.getn(pool) do pool[i].guid=nil pool[i].hovered=false pool[i]:Hide() end
end
function __WBInstallNameplates()
  if not WorldFrame then return end
  if not installed then
    installed=true
    ShowNameplates=function() WB.plateEnemies=true refresh() end
    HideNameplates=function() WB.plateEnemies=false refresh() end
    ShowFriendNameplates=function() WB.plateFriends=true refresh() end
    HideFriendNameplates=function() WB.plateFriends=false refresh() end
    -- Resize/UI scale and target glow also refresh when the copied actor data did not change.
    local driver=CreateFrame("Frame",nil,WorldFrame)
    driver:SetScript("OnUpdate",refresh)
  end
  NAMEPLATES_ON=WB.plateEnemies and 1 or nil
  FRIENDNAMEPLATES_ON=WB.plateFriends and 1 or nil
  refresh()
end
WB.installNameplates=__WBInstallNameplates
function __WBNameplates(snapshot)
  if WB.nameplates and (snapshot.epoch or 0)<(WB.nameplates.epoch or 0) then return end
  if not WB.liveNameplates then
    WB.plateEnemies=NAMEPLATES_ON and true or false
    WB.plateFriends=FRIENDNAMEPLATES_ON and true or false
  end
  WB.liveNameplates=true WB.nameplates=snapshot
  __WBInstallNameplates()
end
-- Native probes read the currently visible actor's plate, never a recycled widget from an earlier snapshot.
function __WBNameplate(guid) return byGuid[guid] end
