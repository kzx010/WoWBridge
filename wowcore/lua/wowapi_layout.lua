-- The client's layout-cache.txt: named user-placed frames, in the character's WTF folder.
-- 1.12 writes Frame, FrameLevel, X, Y, W, H (screen position in the frame's units).
-- The host loads this before login; restore after the interface's variable/login defaults,
-- before PLAYER_ENTERING_WORLD. SetPoint intentionally clears user placement, so mark it last.
local cache = {}
local function finite(n) return n and n == n and math.abs(n) < 3.402823466e38 end -- native float range
local function valid(r)
  return finite(r.X) and finite(r.Y) and finite(r.W) and r.W > 0 and
         finite(r.H) and r.H > 0 and finite(r.FrameLevel) and r.FrameLevel >= 0 and
         r.FrameLevel <= 2147483647 and r.FrameLevel == math.floor(r.FrameLevel)
end

function __WBLoadLayoutCache(text)
  cache = {}
  local row
  for line in string.gmatch(text or "", "[^\r\n]+") do
    local _, _, key, value = string.find(line, "^(%w+):%s*(.-)%s*$")
    if key == "Frame" then
      row = {}
      cache[value] = row
    elseif row and (key == "FrameLevel" or key == "X" or key == "Y" or key == "W" or key == "H") then
      row[key] = tonumber(value)
    end
  end
end

function __WBRestoreLayout()
  for name, row in pairs(cache) do
    local f = rawget(_G, name)   -- a cache entry must not create a missing-API fallback object
    if type(f) == "table" and f.IsObjectType and f:IsObjectType("Frame") and valid(row) then
      f:ClearAllPoints()
      f:SetWidth(row.W)
      f:SetHeight(row.H)
      f:SetFrameLevel(row.FrameLevel)
      f:SetPoint("BOTTOMLEFT", UIParent, "BOTTOMLEFT", row.X, row.Y)
      f:SetUserPlaced(true)
      cache[name] = nil
    end
  end
end

function __WBLayoutCacheText()
  local rows = {}
  -- Keep frames belonging to an addon that has not loaded in this session.
  for name, row in pairs(cache) do if valid(row) then rows[name] = row end end
  for _, f in pairs(__WoweeFramesByWid) do
    local name = f:GetName()
    if name and name ~= "" and f:IsObjectType("Frame") and f:IsUserPlaced() then
      local scale = UIParent:GetEffectiveScale() / f:GetEffectiveScale()
      local row = { FrameLevel = f:GetFrameLevel(), W = f:GetWidth(), H = f:GetHeight(),
                    X = f:GetLeft() - UIParent:GetLeft() * scale,
                    Y = f:GetBottom() - UIParent:GetBottom() * scale }
      if valid(row) then rows[name] = row end
    end
  end
  local names, out = {}, {}
  for name in pairs(rows) do table.insert(names, name) end
  table.sort(names)
  for _, name in ipairs(names) do
    local r = rows[name]
    -- Retain subpixel offsets so a reload does not accumulate rounding drift.
    table.insert(out, string.format("Frame: %s\nFrameLevel: %d\nX: %.9g\nY: %.9g\nW: %.9g\nH: %.9g\n",
                                   name, r.FrameLevel, r.X, r.Y, r.W, r.H))
  end
  return table.concat(out)
end

local fire = __WBFire
function __WBFire(event, ...)
  if event == "PLAYER_ENTERING_WORLD" then __WBRestoreLayout() end
  return fire(event, unpack(arg, 1, arg.n))
end
