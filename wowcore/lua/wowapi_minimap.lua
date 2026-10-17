-- Host POI hit testing shares the terrain projection. Use the actual client
-- tooltip, anchored like MinimapZoneTextButton, without changing recorded APIs.
__WB.cvarDefaults.wbMinimapNativeArt = "1"
RegisterCVar("wbMinimapNativeArt", 1)

-- UiSession's shared blip/tooltip gate has the legacy name trackingCreatureMask.
-- Resources use the same yellow ObjectIcons cell, projection and clipping. Give
-- them a separate token so changing trackers also rejects an old copied snapshot.
-- Classic creature types occupy bits 0..8; bit 16 tags PLAYER_TRACK_RESOURCES.
local creatureMask = __WB.trackingCreatureMask
function __WB.trackingResourceMask()
  local a, info = __WB.trackingAura()
  for _, e in ipairs(info and info.effects or {}) do
    if a.spell == 2481 and e.aura == 45 and e.misc == 6 then return 32 end
  end
  return 0
end
function __WB.trackingCreatureMask()
  local resources = __WB.trackingResourceMask()
  if resources ~= 0 then return 65536 + resources end
  return creatureMask()
end

local owned, shown
function __WBMinimapTooltip(name)
  __WB.minimapHover = name
  if not GameTooltip or not Minimap then return end
  if name then
    if shown ~= name or not GameTooltip:IsOwned(Minimap) or not GameTooltip:IsShown() then
      GameTooltip:SetOwner(Minimap, "ANCHOR_LEFT")
      GameTooltip:SetText(name, 1, 1, 1)
      GameTooltip:Show()
    end
    owned, shown = true, name
  else
    if owned and GameTooltip:IsOwned(Minimap) then GameTooltip:Hide() end
    owned, shown = nil, nil
  end
end
