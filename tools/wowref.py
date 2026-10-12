#!/usr/bin/env python3
"""Drive the reference WoW 1.12 client (fullscreen 1280x720, centered on the desktop) and capture it.

usage: wowref.py STEP [STEP ...]
  key:NAME        press a key (evdev name: escape, enter, tab, f1 ...) or a chord (alt+z)
  type:TEXT       type text (use \\n for Enter)
  click:X,Y       left click at WoW pixel (X,Y)      rclick:X,Y  right click
  shiftclick:X,Y  native Shift + left click (quantity controls)
  point:X,Y       move the pointer to WoW pixel (X,Y) without clicking
  wait:SECONDS    sleep
  hold:KEY,SECS   hold a key down (movement)
  shot:NAME       screenshot -> build/wowref/NAME.png (cropped to the WoW frame)
  slash:TEXT      open chat, type a slash command, send it
  login           log in using WOWBRIDGE_REFERENCE_PASSWORD (environment only)
  launch          start the client (WoW/run-wow.sh) if it is not running, wait for its window

Input goes to whatever has focus, so every input step first raises the WoW window through KWin and checks it is the
active window; without it the run stops rather than type into another program.
"""
import os, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inputdev import Input, e, focus_window  # noqa: E402
from PIL import Image  # noqa: E402

OUT = Path(__file__).resolve().parent.parent / 'build/wowref'
RUN_WOW = Path(__file__).resolve().parent.parent.parent / 'WoW/run-wow.sh'
GAME_W, GAME_H = 1280, 720
CAPTION = 'World of Warcraft'
def saved_variables() -> Path:
    configured = os.environ.get('WOWBRIDGE_REFERENCE_SAVED_VARIABLES')
    if configured:
        return Path(configured)
    candidates = sorted((RUN_WOW.parent / 'client/WTF/Account').glob('*/SavedVariables/WBOracle.lua'))
    if len(candidates) != 1:
        raise RuntimeError('set WOWBRIDGE_REFERENCE_SAVED_VARIABLES when the recorder account is missing or ambiguous')
    return candidates[0]


def focus() -> bool:
    """Raise the WoW window; True when it exists and is the active window."""
    return focus_window(CAPTION)


def frame_origin(inp: Input) -> tuple[int, int, float]:
    scale = min(inp.w / GAME_W, inp.h / GAME_H)
    return int((inp.w - GAME_W * scale) / 2), int((inp.h - GAME_H * scale) / 2), scale


def main(steps: list[str]) -> None:
    password = None
    for step in steps:
        op, _, arg = step.partition(':')
        if op == 'login':
            if arg:
                sys.exit('wowref: pass the reference password through WOWBRIDGE_REFERENCE_PASSWORD, not command arguments')
            password = os.environ.get('WOWBRIDGE_REFERENCE_PASSWORD')
            if not password:
                sys.exit('wowref: set WOWBRIDGE_REFERENCE_PASSWORD before login')
    inp = Input()
    ox, oy, k = frame_origin(inp)
    to_screen = lambda x, y: (ox + int(int(x) * k), oy + int(int(y) * k))
    OUT.mkdir(parents=True, exist_ok=True)
    for step in steps:
        op, _, arg = step.partition(':')
        if op in ('key', 'type', 'click', 'rclick', 'shiftclick', 'point', 'wheel', 'hold', 'slash', 'login') and not focus():
            time.sleep(0.5)
            if not focus():
                sys.exit(f'wowref: the WoW window is not running or will not take focus; stopped before {op!r}')
        if op == 'launch':
            if not focus():
                subprocess.Popen([RUN_WOW], cwd=RUN_WOW.parent, start_new_session=True,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                for _ in range(90):
                    time.sleep(1)
                    if focus():
                        break
                else:
                    sys.exit('wowref: the client window did not appear')
                time.sleep(8)    # login screen
        elif op == 'key':
            inp.combo(arg) if '+' in arg else inp.key(arg)
        elif op == 'type':
            inp.type(arg.replace('\\n', '\n'))
        elif op in ('click', 'rclick'):
            x, y = arg.split(',')
            inp.click(*to_screen(x, y), button=e.BTN_LEFT if op == 'click' else e.BTN_RIGHT)
        elif op == 'shiftclick':
            x, y = arg.split(',')
            inp.kbd.write(e.EV_KEY, e.KEY_LEFTSHIFT, 1); inp.kbd.syn()
            try:
                inp.click(*to_screen(x, y))
            finally:
                inp.kbd.write(e.EV_KEY, e.KEY_LEFTSHIFT, 0); inp.kbd.syn()
        elif op == 'point':      # point:X,Y  hover without clicking (tooltips)
            x, y = arg.split(',')
            inp.point(*to_screen(x, y))
        elif op == 'wheel':      # wheel:N  N notches away from you (zoom in), negative towards you
            inp.wheel(int(arg))
        elif op == 'hold':       # hold:KEY,SECONDS
            hold_key, secs = arg.split(',')
            inp.key(hold_key, hold=float(secs))
        elif op == 'wait':
            time.sleep(float(arg))
        elif op == 'slash':
            inp.key('enter'); time.sleep(0.2); inp.type(arg); time.sleep(0.1); inp.key('enter'); time.sleep(0.3)
        elif op == 'login':      # login screen -> character select -> world (1280x720 layout)
            time.sleep(2); inp.click(*to_screen(648, 447)); inp.type(password); inp.key('enter')
            # The realm wizard appears on every login against this realm; on character select these three
            # positions are empty scenery, so clicking them unconditionally is harmless.
            time.sleep(7); inp.click(*to_screen(1027, 239)); time.sleep(0.5); inp.click(*to_screen(1128, 441))
            time.sleep(4); inp.click(*to_screen(540, 380))
            time.sleep(7); inp.click(*to_screen(638, 661)); time.sleep(12)
        elif op == 'shot':
            tmp = OUT / '.desktop.png'
            subprocess.run(['spectacle', '-b', '-n', '-f', '-o', str(tmp)], capture_output=True, timeout=30)
            img = Image.open(tmp).crop((ox, oy, ox + int(GAME_W * k), oy + int(GAME_H * k)))
            img.save(OUT / f'{arg}.png')
            print(f'shot {OUT / arg}.png {img.size}')
        else:
            sys.exit(f'unknown step {step}')


if __name__ == '__main__':
    main(sys.argv[1:])
