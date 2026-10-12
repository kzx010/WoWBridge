-- Host POI hit testing shares the terrain projection. Use the actual client
-- tooltip, anchored like MinimapZoneTextButton, without changing recorded APIs.
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
