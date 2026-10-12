#!/usr/bin/env python3
"""HUD reference captures from the real WoW 1.12 client (1280x720, useUiScale 0) for the M5 pixel-diff gate.

Also records every visible frame/region rectangle per state (/wbo frames) -> evidence/hudref/frames.lua.
usage: hudref.py setup          put Elunara into the slice state (slice bar, no forms, full health)
       hudref.py --variant NAME [STATE ...]   the same at another interface scale / resolution (VARIANTS), into
                                evidence/hudref/NAME/; the client is put back to the default afterwards
       hudref.py [STATE ...]    capture states (default: all) -> evidence/hudref/<state>.png, and right after it
                                <state>_world.png with the interface hidden (Alt+Z), the background wowcore's
                                render is composited over for the pixel diff
"""
import shutil, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wowref  # noqa: E402

OUT = Path(__file__).resolve().parent.parent / 'evidence/hudref'
TESTER = '[PH[ Combat Tester'
REST = 'point:1000,140'                               # pointer parked over scenery: no tooltip, no UI hover
CLEAN = ['slash:/script ChatFrame1:Clear() GameTooltip:Hide()', REST, 'wait:1.5']
SELF = 'slash:/script TargetUnit("player")'

SETUP = [SELF, *[f'slash:.unlearn {s}' for s in (768, 783, 1066, 9634, 5487)],           # forms: not in the slice
         f'slash:/script TargetByName("Young Nightsaber")', 'slash:.npc delete',   # leftover test spawn
         SELF, 'slash:.modify mana 99999', 'slash:/cast Healing Touch', 'wait:4', 'slash:/cast Healing Touch', 'wait:4',
         'slash:/wbo bar', 'slash:/script ClearTarget()', *CLEAN]

STATES = {
    'hud_idle': ['slash:/script ClearTarget()', *CLEAN],
    'hud_target_debuffs': [f'slash:/wbo target {TESTER}', 'slash:/cast Moonfire', 'wait:1.7', 'slash:/cast Entangling Roots',
                           'wait:2.2', *CLEAN],
    'hud_casting': [SELF, 'slash:/cast Rejuvenation', 'wait:1.7', 'slash:/script ChatFrame1:Clear()', REST,
                    'slash:/cast Healing Touch', 'wait:1.5'],
    'hud_tooltip': ['slash:/script ClearTarget()', 'slash:/script ChatFrame1:Clear()', 'point:184,698', 'wait:1.5'],
    'spellbook': ['slash:/script ClearTarget()', *CLEAN, 'key:p', 'wait:1'],
    # The Escape key's own call: after scripted chat the first Escape press is spent on input focus.
    'game_menu': ['slash:/script ClearTarget()', *CLEAN, 'slash:/script ShowUIPanel(GameMenuFrame)', REST, 'wait:1'],
    # The open Key Bindings frame takes the keyboard, so its frames are recorded by the same line that opens it.
    'key_bindings': ['slash:/script ChatFrame1:Clear()', REST,
                     'slash:/script KeyBindingFrame_LoadUI() ShowUIPanel(KeyBindingFrame) SlashCmdList.WBORACLE("frames key_bindings")',
                     'wait:1'],
}
# Other effective scales and aspects than the default 1280x720 at 720/768. The display here is at most 1544x720, so the
# interface scale setting (useUiScale/uiscale) is what varies the scale; the resolution varies the aspect.
VARIANTS = {
    'scale080': ['slash:/script SetCVar("useUiScale", "1") SetCVar("uiscale", "0.8")'],
    'scale100': ['slash:/script SetCVar("useUiScale", "1") SetCVar("uiscale", "1.0")'],
}
RESTORE = ['slash:/script SetCVar("useUiScale", "0") SetCVar("uiscale", "1.0")']
TEARDOWN = {'spellbook': ['key:escape'], 'game_menu': ['key:escape'], 'key_bindings': ['key:escape']}


def main(argv: list[str]) -> None:
    global OUT
    variant = None
    if argv[:1] == ['--variant']:
        variant, argv = argv[1], argv[2:]
        OUT = OUT / variant
    OUT.mkdir(parents=True, exist_ok=True)
    if argv[:1] == ['setup']:
        wowref.main(SETUP)
        return
    wowref.main(['slash:/run ReloadUI()', 'wait:10', *(VARIANTS[variant] + ['wait:3'] if variant else [])])
    for name in argv or STATES:
        record = [] if name == 'key_bindings' else [f'slash:/wbo frames {name}']
        wowref.main([*STATES[name], f'shot:{name}', *record, 'key:alt+z', 'wait:0.4',
                     f'shot:{name}_world', 'key:alt+z', 'wait:0.4', *TEARDOWN.get(name, [])])
        for shot in (name, f'{name}_world'):
            shutil.copy(wowref.OUT / f'{shot}.png', OUT / f'{shot}.png')
    wowref.main([*(RESTORE if variant else []), 'slash:/run ReloadUI()', 'wait:10'])   # writes the frame rectangles
    import apiref
    apiref.save_sv(OUT / 'frames.lua')


if __name__ == '__main__':
    main(sys.argv[1:])
