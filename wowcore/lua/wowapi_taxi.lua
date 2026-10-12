-- Live carriage sessions drive Blizzard's unmodified 1.12 TaxiFrame. The
-- recorded closures remain installed until the first main-thread snapshot.
local WB = __WB
local recorded, live = {}, {}
local terrainAvailable = false
for _, name in ipairs({"NumTaxiNodes", "TaxiNodeName", "TaxiNodeGetType", "TaxiNodePosition", "TaxiNodeCost",
  "TaxiNodeSetCurrent", "GetNumRoutes", "TaxiGetSrcX", "TaxiGetSrcY", "TaxiGetDestX", "TaxiGetDestY",
  "TakeTaxiNode", "CloseTaxiMap", "SetTaxiMap"}) do recorded[name] = _G[name] end
local oldName = UnitName
function UnitName(unit)
  if type(unit)=="string" and string.lower(unit)=="npc" and WB.taxi then return WB.taxi.name, nil end
  return oldName(unit)
end
local function node(index) return WB.taxi and WB.taxi.nodes[index or 0] end
local function current()
  for _, n in ipairs(WB.taxi and WB.taxi.nodes or {}) do if n.current then return n end end
end
local function view()
  local m = WB.taxi
  if not m then WB.taxiView=nil return end
  local lowX, lowY, highX, highY = math.huge, math.huge, -math.huge, -math.huge
  for _, n in ipairs(m.nodes) do
    lowX, lowY = math.min(lowX,n.x), math.min(lowY,n.y)
    highX, highY = math.max(highX,n.x), math.max(highY,n.y)
  end
  -- One shared north-up projection for terrain, node buttons and route lines.
  local aspect = TAXI_MAP_WIDTH / TAXI_MAP_HEIGHT
  local width = math.max(64, highX-lowX, (highY-lowY)*aspect)*1.15
  local cx, cy = (lowX+highX)/2, (lowY+highY)/2
  WB.taxiView={lowX=cx-width/2,highX=cx+width/2,lowY=cy-width/aspect/2,highY=cy+width/aspect/2}
end
local function position(n)
  local v=WB.taxiView
  if not n or not v then return 0,0 end
  -- TaxiFrame anchors from BOTTOMLEFT; atlas coordinates are north/up.
  return (n.x-v.lowX)/(v.highX-v.lowX), (n.y-v.lowY)/(v.highY-v.lowY)
end
local function install() for name, fn in pairs(live) do _G[name]=fn end end
function __WBTaxi(snapshot)
  install()
  local previous=WB.taxi
  WB.liveTaxi=true WB.taxi=snapshot WB.taxiPending=nil
  view()
  if snapshot then __WBFire("TAXIMAP_OPENED")
  elseif previous then GameTooltip:Hide() __WBFire("TAXIMAP_CLOSED") end
end
function __WBInstallTaxi()
  if not WB.liveTaxi then return end
  install() view()
  if WB.taxi then __WBFire("TAXIMAP_OPENED") end
end
function NumTaxiNodes() return WB.taxi and table.getn(WB.taxi.nodes) or 0 end
function TaxiNodeName(index) local n=node(index) return n and n.name or "INVALID" end
function TaxiNodeGetType(index)
  local n=node(index) return n and (n.current and "CURRENT" or "REACHABLE") or "NONE"
end
function TaxiNodePosition(index) return position(node(index)) end
function TaxiNodeCost(index) local n=node(index) return n and n.cost*10000 or 0 end
function TaxiNodeSetCurrent(index) return index end
function GetNumRoutes(index) local n=node(index) return n and not n.current and 1 or 0 end
function TaxiGetSrcX(index, route) local x=position(current()) return x end
function TaxiGetSrcY(index, route) local _,y=position(current()) return y end
function TaxiGetDestX(index, route) local x=position(node(index)) return x end
function TaxiGetDestY(index, route) local _,y=position(node(index)) return y end
function SetTaxiMap(texture)
  -- The native atlas renderer paints TaxiRouteMap beneath its client routes
  -- and buttons; never display a WoW continent when Skyrim terrain is absent.
  if terrainAvailable then texture:SetTexture(nil) else texture:SetTexture(0.08,0.08,0.08) end
end
function __WBTaxiTerrain(available)
  if terrainAvailable==available then return end
  terrainAvailable=available
  if WB.taxi and TaxiMap then SetTaxiMap(TaxiMap) end
end
function TakeTaxiNode(index)
  local m,n=WB.taxi,node(index)
  if not m or not n or n.current or WB.taxiPending then return end
  if GetMoney()<n.cost*10000 then __WBFire("UI_ERROR_MESSAGE",ERR_NOT_ENOUGH_MONEY) return end
  WB.taxiPending=true
  WB.host("taxi","travel",n.id,m.session,n.cost)
end
function CloseTaxiMap()
  local m=WB.taxi
  if m then WB.host("taxi","close",0,m.session,0) __WBTaxi(nil) end
end
for name, fn in pairs(recorded) do live[name]=_G[name] _G[name]=fn end
