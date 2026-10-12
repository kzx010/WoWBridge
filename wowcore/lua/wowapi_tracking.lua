-- 1.12 tracking is an ordinary helpful aura. Its effects, icon and text come
-- from Spell.dbc; the host owns replacement, form loss and saved auras.
local WB = __WB
function WB.trackingAura()
  local p = WB.resolve("player")
  for _, a in ipairs(p and p.buffs or {}) do
    local info = a.spell and WB.spellInfo(a.spell)
    for _, e in ipairs(info and info.effects or {}) do
      if e.aura == 44 or e.aura == 45 or e.aura == 151 then return a, info end
    end
  end
end
function WB.trackingCreatureMask()
  local a, info = WB.trackingAura()
  for _, e in ipairs(info and info.effects or {}) do
    if e.aura == 44 and e.misc >= 1 and e.misc <= 32 then return 2 ^ (e.misc - 1) end
  end
  return 0
end
function GetTrackingTexture()
  local a, info = WB.trackingAura()
  return info and info.icon or nil
end
function CancelTrackingBuff()
  local a = WB.trackingAura()
  if a then WB.host("cancelAura", a.spell) end
end
