-- Capture-specific states. Keep the seven historical HUD fixtures untouched.
-- These reproduce the interface content, not the reference client's 3D world.
local function forms()
  __WBDruidForms(9634)
  __WB.actions={}
  __WBFire('ACTIONBAR_SLOT_CHANGED',0)
end
function __WBFidelityForm(index,prowl)
  forms()
  __WB.tokens.target=__WB.tokens.player
  __WBForm(index,nil,prowl)
  __WBAuraAdd('player',9885,false,1800)
  if index==3 then __WBAuraAdd('player',768,false,0) end
  if index==1 then __WBAuraAdd('player',9634,false,0) end
  __WBFire('PLAYER_TARGET_CHANGED')
  __WBFire('UNIT_AURA','target')
end
function __WBFidelityCharacter()
  -- Reference equipment and placement, with the authorized Skyrim sheet/tab
  -- adaptations. Do not replace the Stormrage slots with the mixed-item demo.
  __WBInstallCharacter()
  __WBApplyLive({player={nativeSheet={health=3213,healthMax=3213,magicka=5284,magickaMax=5284,
    stamina=100,staminaMax=100,weight=142,carry=300,armor=1422,damage=255,offDamage=0,
    fire=40,frost=10,shock=20,magic=0,poison=30,disease=0}}})
  __WBCharacterLook({race=4,sex=1,skin=5,face=8,hairStyle=5,hairColor=3},
    {{slot=0,displayId=33655},{slot=2,displayId=30546},{slot=4,displayId=30536},
     {slot=5,displayId=30541},{slot=6,displayId=30540},{slot=7,displayId=30542},
     {slot=8,displayId=30548},{slot=9,displayId=34016},{slot=15,displayId=20298,sheath=2}})
  SetCVar('statusBarText',1)
  TextStatusBar_UpdateTextString(PlayerFrameHealthBar)
  TextStatusBar_UpdateTextString(PlayerFrameManaBar)
  ToggleCharacter('PaperDollFrame')
end
HUD_STATES.fidelity_character={fixture='idle',settle=2,run='__WBFidelityCharacter()'}
HUD_STATES.fidelity_character_back={fixture='idle',settle=2,run=[[
  __WBFidelityCharacter() CharacterModelFrame:SetRotation(math.pi)
]]}
HUD_STATES.fidelity_character_left={fixture='idle',settle=2,run=[[
  __WBFidelityCharacter() CharacterModelFrame:SetRotation(math.pi/2)
]]}
HUD_STATES.fidelity_character_right={fixture='idle',settle=2,run=[[
  __WBFidelityCharacter() CharacterModelFrame:SetRotation(-math.pi/2)
]]}
HUD_STATES.fidelity_cat={fixture='idle',settle=1,run=[[
  __WBFidelityForm(3)
  __WB.units[1].power=20 __WBFire('UNIT_MANA','player') __WBFire('UNIT_MANA','target')
  GameTooltip_SetDefaultAnchor(GameTooltip,ShapeshiftButton3) GameTooltip:SetShapeshift(3)
]],mouse={{0,360},{285,655}}}
HUD_STATES.fidelity_bear={fixture='idle',settle=1,run=[[
  __WBFidelityForm(1)
  GameTooltip_SetDefaultAnchor(GameTooltip,ShapeshiftButton1) GameTooltip:SetShapeshift(1)
]],mouse={{0,360},{215,655}}}
HUD_STATES.fidelity_prowl_before={fixture='idle',settle=1,run=[[
  __WBFidelityForm(3) __WB.tokens.target=nil __WBFire('PLAYER_TARGET_CHANGED')
  __WB.units[1].power=40 __WBFire('UNIT_MANA','player')
]]}
HUD_STATES.fidelity_prowl={fixture='idle',settle=1,run=[[
  __WBFidelityForm(3,true) __WB.tokens.target=nil __WBFire('PLAYER_TARGET_CHANGED')
  __WB.units[1].power=40 __WBFire('UNIT_MANA','player')
]]}
HUD_STATES.fidelity_aquatic_error={fixture='idle',settle=0.2,run=[[
  __WBDruidForms(9634) __WB.actions={}
  -- Match visible art; the capture does not record the ranks of these slots.
  for n,id in ipairs({6603,9912,9835,25299,25297,9853,17402,22812,339,9885,5225,783}) do
    __WB.actions[n]={spell=id}
  end
  __WBFire('ACTIONBAR_SLOT_CHANGED',0)
  __WBAuraAdd('player',9885,false,1800) __WBUiError('SPELL_FAILED_ONLY_UNDERWATER')
]]}
-- levelref captures the world celebration; these reproduce its Cat HUD only.
HUD_STATES.fidelity_level_start={fixture='idle',settle=0.2,run='__WBFidelityForm(3,true)'}
HUD_STATES.fidelity_level_1s={fixture='idle',settle=1,run='__WBFidelityForm(3,true)'}
HUD_STATES.fidelity_buff_frames={fixture='self',settle=1,run=[[
  __WB.units[1].buffs={} __WBAuraAdd('player',9885,false,1800)
  -- Values are not recorded: these reproduce the visible bar fractions only.
  __WBApplyLive({player={health=1900,healthMax=3213,power=4350,powerMax=5284},world={hour=2,minute=46}})
  __WBFire('PLAYER_TARGET_CHANGED') __WBFire('UNIT_AURA','target')
]]}
HUD_STATES.fidelity_spellbook={fixture='idle',settle=1,run='ToggleSpellBook(BOOKTYPE_SPELL)',
  print="'checked='..tostring(SpellButton1:GetChecked())..','..tostring(SpellButton3:GetChecked())"}

-- Explicit UI scales recorded in hudref/scale080 and scale100. hud_render
-- applies user_scale before loading FrameXML, like the plugin's startup path.
for _,scale in ipairs({0.8,1.0}) do
  local suffix=scale==0.8 and '080' or '100'
  for _,state in ipairs({'hud_idle','spellbook','game_menu'}) do
    -- Both files labelled spellbook actually show GameMenuFrame. Their
    -- frames.lua blocks confirm it; reproduce pixels, not the filename.
    local source=state=='spellbook' and 'game_menu' or state
    local copy={} for k,v in pairs(HUD_STATES[source]) do copy[k]=v end
    copy.user_scale=scale
    HUD_STATES['fidelity_'..state..'_scale'..suffix]=copy
  end
end
-- The scale080 "idle" shot has the book, the client's removed Honor panel,
-- a combat tester target, status text and oracle debug messages. Keep the
-- supported book/target state; fidelity_diff excludes the removed panel.
HUD_STATES.fidelity_hud_idle_scale080.fixture='target'
HUD_STATES.fidelity_hud_idle_scale080.run=[[
  __WB.units[2].debuffs={} __WBFire('PLAYER_TARGET_CHANGED')
  SetCVar('statusBarText',1)
  TextStatusBar_UpdateTextString(PlayerFrameHealthBar)
  TextStatusBar_UpdateTextString(PlayerFrameManaBar)
  ToggleSpellBook(BOOKTYPE_SPELL)
]]

for _,elapsed in ipairs({0.10,0.40,0.75,1.10,1.35,1.50,1.60,1.80}) do
  HUD_STATES[string.format('fidelity_gcd_%.2f',elapsed)]={fixture='idle',settle=elapsed,run=[[
    __WBAuraAdd('player',9885,false,1800)
    __WBApplyLive({world={zone='Kharanos',hour=2,minute=0}})
    __WBGcd(1.5)
  ]]}
end
-- Empty client taxi data correctly closes the window in DrawOneHopLines.
-- Suppress that OnShow handler only in this isolated art/layout fixture.
-- No routes are invented; this cannot verify real taxi interaction.
HUD_STATES.fidelity_taxi_layout={fixture='idle',settle=1,run=[[
  TaxiFrame:SetScript('OnShow',nil) TaxiFrame:Show()
]]}

-- Clean native-resolution client captures from tools/clientref.py (2026-10-10).
-- Preserve the historical mislabelled states above. These reproduce the new
-- humanoid HUD, four learned forms, full bars and populated twelve-slot page.
local function clientRefHud()
  local wb=__WB
  wb.forms={} wb.formIndex=0 wb.activeFormSpell=nil
  __WBDruidForms(9634)
  wb.actions={}
  for n,id in ipairs({6603,9912,9835,25299,25297,9853,17402,25298,29166,9885,9910,22812}) do
    wb.actions[n]={spell=id}
  end
  wb.world.hour=0 wb.world.minute=0
  SetCVar('statusBarText',0)
  __WBFire('ACTIONBAR_SLOT_CHANGED',0)
  __WBFire('UPDATE_SHAPESHIFT_FORMS')
  __WBFire('PLAYER_TARGET_CHANGED')
  TextStatusBar_UpdateTextString(PlayerFrameHealthBar)
  TextStatusBar_UpdateTextString(PlayerFrameManaBar)
  local old=this this=GameTimeFrame GameTimeFrame_Update() this=old
end
__WBClientRefHud=clientRefHud
for _,scale in ipairs({0.8,1.0}) do
  local suffix=scale==0.8 and '080' or '100'
  HUD_STATES['clientref_hud_idle_scale'..suffix]={fixture='idle',user_scale=scale,settle=2,
    run='__WBClientRefHud()'}
  HUD_STATES['clientref_spellbook_scale'..suffix]={fixture='idle',user_scale=scale,settle=2,
    run="__WBClientRefHud() ToggleSpellBook(BOOKTYPE_SPELL)"}
end
