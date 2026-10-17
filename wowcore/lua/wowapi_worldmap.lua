-- The unchanged 1.12 WorldMapFrame over Skyrim's copied exterior data. Installed
-- only in game (or explicitly in tests); recorded map answers remain untouched.
local WB = __WB
local installed = false -- Lua closures/widgets are recreated by ReloadUI
local hint = "Fast travel: Skyrim's map (Shift+M)."
local function finite(n) return type(n)=="number" and n==n and math.abs(n)<math.huge end
local function pose() return WB.worldmap end
local function title() local p=pose() return p and (p.name or p.worldspace) or "Skyrim" end
local function poiTooltip(frame)
  WorldMapFrame.poiHighlight=1
  WorldMapFrameAreaLabel:SetText(frame.name) WorldMapFrameAreaDescription:SetText(frame.description)
  WorldMapTooltip:SetOwner(frame,"ANCHOR_RIGHT") WorldMapTooltip:SetText(frame.name,1,1,1)
  WorldMapTooltip:AddLine(frame.description,1,1,1,1) WorldMapTooltip:Show()
end
local function project(x,y)
  local v=WB.worldMapView
  if not v or not finite(x) or not finite(y) then return end
  x,y=(x-v.lowX)/(v.highX-v.lowX),(v.highY-y)/(v.highY-v.lowY)
  if x>=0 and x<=1 and y>=0 and y<=1 then return x,y end
end
local function view()
  local b,p=WB.worldMapBounds,pose()
  if not b or not p then WB.worldMapView=nil return end
  local aspect=WorldMapButton:GetWidth()/WorldMapButton:GetHeight()
  local width,height,cx,cy
  if WB.worldMapZone==1 then
    width=math.min(1600,math.max(b.highX-b.lowX,(b.highY-b.lowY)*aspect))
    height=width/aspect
    cx,cy=WB.worldMapCentreX or p.x,WB.worldMapCentreY or p.y
    WB.worldMapCentreX,WB.worldMapCentreY=cx,cy
  else
    width=math.max(b.highX-b.lowX,(b.highY-b.lowY)*aspect) height=width/aspect
    cx,cy=(b.lowX+b.highX)/2,(b.lowY+b.highY)/2
  end
  WB.worldMapView={lowX=cx-width/2,lowY=cy-height/2,highX=cx+width/2,highY=cy+height/2}
end
local function landmarks()
  local list={}
  local p=pose()
  for _,poi in ipairs(p and p.pois or {}) do
    if type(poi.name)=="string" and poi.name~="" and
      (poi.kind=="quest" or poi.kind=="custom" or poi.kind=="location" and poi.discovered) then
      local x,y=project(poi.x,poi.y)
      if x then
        tinsert(list,{name=poi.name,description=(poi.kind=="quest" and "Quest target. " or
          poi.kind=="location" and "Discovered location. " or "Custom map marker. ")..hint,
          icon=poi.kind=="quest" and 6 or poi.kind=="custom" and 0 or
            (finite(poi.icon) and poi.icon>=0 and poi.icon<=42 and math.floor(poi.icon) or 5),x=x,y=y})
      end
    end
  end
  WB.worldMapLandmarks=list
end
local function refresh()
  view() landmarks()
  if WB.worldMapInstalled then __WBFire("WORLD_MAP_UPDATE") end
end
-- Render-thread atlas coverage, never engine references. A missing atlas clears
-- all geography instead of retaining the previous world's terrain or arrow.
function __WBWorldMapAtlas(lowX,lowY,highX,highY)
  if not WB.worldMapInstalled then return end
  local p=pose()
  local valid=p and finite(p.x) and finite(p.y) and finite(p.heading) and
    finite(lowX) and finite(lowY) and finite(highX) and finite(highY) and highX>lowX and highY>lowY
  WB.worldMapBounds=valid and {lowX=lowX,lowY=lowY,highX=highX,highY=highY} or nil
  local world=p and p.worldspace
  if WB.worldMapWorld~=world then
    WB.worldMapWorld=world WB.worldMapZone=1 WB.worldMapCentreX=nil WB.worldMapCentreY=nil
  end
  view() landmarks()
  -- Only rebuild the client's POI widgets when their displayed answers change.
  local signature={tostring(world),title(),tostring(WB.worldMapZone),tostring(valid)}
  for _,i in ipairs(WB.worldMapLandmarks) do
    tinsert(signature,string.format("%s|%s|%d|%.9f|%.9f",i.name,i.description,i.icon,i.x,i.y))
  end
  signature=table.concat(signature,"\n")
  if signature~=WB.worldMapSignature then WB.worldMapSignature=signature __WBFire("WORLD_MAP_UPDATE") end
  if WorldMapFrame:IsShown() then
    WBWorldMapNotice:SetText((valid and (p.interior and "Interior: showing exterior. " or "") or "Terrain unavailable. ")..hint)
  end
end

function __WBOpenSkyrimMap()
  HideUIPanel(WorldMapFrame)
  WB.host("menu","MapMenu")
end
function __WBInstallWorldMap()
  if installed then return end installed=true
  WB.worldMapInstalled=true WB.worldMapZone=WB.worldMapZone or 1 WB.worldMapSignature=nil
  -- These APIs are consumed by Blizzard's own dropdowns, click handlers, POI
  -- builder and player hit box. Terrain is the native rectangular atlas hook.
  GetMapContinents=function() return title() end
  GetMapZones=function(continent) if continent==1 then return "Local view" end end
  GetMapInfo=function() return pose() and pose().worldspace or "Skyrim",668,1002 end
  GetCurrentMapContinent=function() return 1 end
  GetCurrentMapZone=function() return WB.worldMapZone or 0 end
  GetNumMapOverlays=function() return 0 end
  GetMapOverlayInfo=function() return nil,0,0,0,0,0,0 end
  GetNumMapLandmarks=function() return table.getn(WB.worldMapLandmarks or {}) end
  GetMapLandmarkInfo=function(index)
    if not index then error("Usage: GetMapLandmarkInfo(index)",2) end
    local i=(WB.worldMapLandmarks or {})[index]
    if i then return i.name,i.description,i.icon,i.x,i.y end
    return nil,"",0,0,0
  end
  SetMapZoom=function(continent,zone)
    WB.worldMapZone=continent==1 and zone==1 and 1 or 0
    WB.worldMapCentreX=nil WB.worldMapCentreY=nil refresh()
  end
  SetMapToCurrentZone=function() SetMapZoom(1,1) end
  ProcessMapClick=function(x,y)
    local v=WB.worldMapView
    if v and WB.worldMapZone==0 and finite(x) and finite(y) and x>=0 and x<=1 and y>=0 and y<=1 then
      WB.worldMapCentreX=v.lowX+x*(v.highX-v.lowX) WB.worldMapCentreY=v.highY-y*(v.highY-v.lowY)
      WB.worldMapZone=1 refresh()
    end
  end
  UpdateMapHighlight=function(x,y)
    if WB.worldMapView and finite(x) and finite(y) and x>=0 and x<=1 and y>=0 and y<=1 then return title() end
  end
  GetPlayerMapPosition=function(unit)
    if UnitIsUnit(unit,"player") then
      local p=pose() local x,y
      if p then x,y=project(p.x,p.y) end
      if x then return x,y end
    end
    return 0,0
  end
  GetCorpseMapPosition=function() return 0,0 end
  ShowWorldMapArrowFrame=function(shown) if shown then WorldMapPlayer:Show() else WorldMapPlayer:Hide() end end
  local update=WorldMapFrame_Update
  WorldMapFrame_Update=function()
    local owner=WorldMapTooltip:IsShown() and WorldMapTooltip:GetOwner()
    update()
    for i=1,NUM_WORLDMAP_DETAIL_TILES do getglobal("WorldMapDetailTile"..i):Hide() end
    if WB.worldMapZone==1 then WorldMapZoomOutButton:Enable() else WorldMapZoomOutButton:Disable() end
    UIDropDownMenu_SetText(title(),WorldMapContinentDropDown)
    UIDropDownMenu_SetText(WB.worldMapZone==1 and "Local view" or "Entire worldspace",WorldMapZoneDropDown)
    WorldMapFrame.poiHighlight=nil WorldMapFrameAreaLabel:SetText(title()) WorldMapFrameAreaDescription:SetText("")
    WorldMapTooltip:Hide()
    if owner and string.find(owner:GetName() or "","^WorldMapFramePOI") and owner:IsShown() then poiTooltip(owner) end
  end
  -- Skyrim objective sentences are longer than the client's short area names.
  -- Keep them in its area label, sized with the client's ordinary large font.
  WorldMapFrameAreaLabel:SetFontObject("GameFontNormalLarge")
  WorldMapFrameAreaDescription:SetFontObject("GameFontHighlightSmall")
  -- The client puts a POI's name/description above the map. Also use its map
  -- tooltip so the fast-travel key is visible where the player is pointing.
  local enter,leave=WorldMapPOI_OnEnter,WorldMapPOI_OnLeave
  WorldMapPOI_OnEnter=function()
    enter() poiTooltip(this)
  end
  WorldMapPOI_OnLeave=function() leave() WorldMapTooltip:Hide() end
  -- Existing POIs were built during the recorded login; reattach their scripts.
  for i=1,NUM_WORLDMAP_POIS do
    local f=getglobal("WorldMapFramePOI"..i)
    f:SetScript("OnEnter",WorldMapPOI_OnEnter) f:SetScript("OnLeave",WorldMapPOI_OnLeave)
  end
  local notice=WorldMapFrame:CreateFontString("WBWorldMapNotice","OVERLAY","GameFontHighlightSmall")
  notice:SetPoint("BOTTOM",WorldMapDetailFrame,"BOTTOM",0,8) notice:SetText(hint)
  -- Reserve Shift+M even with old WTF bindings. The full-screen map consumes
  -- keyboard input before bindings, so its own key script handles the same key.
  local action=GetBindingAction
  GetBindingAction=function(key) if key=="SHIFT-M" then return "WBSKYRIMMAP" end return action(key) end
  __WoweeBindingScripts.WBSKYRIMMAP=function(state) if state~="up" then __WBOpenSkyrimMap() end end
  local key=WorldMapFrame:GetScript("OnKeyDown")
  WorldMapFrame:SetScript("OnKeyDown",function()
    if arg1=="M" and IsShiftKeyDown() and not IsControlKeyDown() and not IsAltKeyDown() then __WBOpenSkyrimMap()
    else key() end
  end)
  -- Every client world-map entry point (binding, minimap button and direct calls)
  -- uses Skyrim's full map. Keep the atlas APIs for explicit debug previews.
  ToggleWorldMap=__WBOpenSkyrimMap
  -- M and the world-map micro button suffice; no extra button over the minimap.
  WorldMapFrame_Update()
end
