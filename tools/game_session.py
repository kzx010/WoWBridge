#!/usr/bin/env python3
"""Hold one test launch of Skyrim open and drive it from a command file, for interactive fixture work such as the
onboarding save (New Game -> offer -> preset -> name -> save in the intro).

Same safeguards as run_skyrim.launch: test prefs (on WSL the WoWBridgeTestSaves save path), autorun off, the gameplay service,
every change restored and the game stopped when the session ends. Commands, one per line, appended to
build/session/cmd.txt: key NAME [HOLD] | type TEXT | console TEXT | click | move DX DY | snap NAME | wait SECONDS |
quit. Each command acts only while the game window has the focus; results go to build/session/log.txt.
"""
import argparse
import signal
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_skyrim as rs  # noqa: E402

SESSION = Path(__file__).resolve().parent.parent / 'build/session'
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
           '_': 'MINUS', '+': 'EQUAL', ':': 'SEMICOLON', '"': 'APOSTROPHE', '<': 'COMMA', '>': 'DOT', '?': 'SLASH'}
PLAIN = {' ': 'SPACE', '-': 'MINUS', '=': 'EQUAL', ';': 'SEMICOLON', "'": 'APOSTROPHE', ',': 'COMMA', '.': 'DOT',
         '/': 'SLASH', '\\': 'BACKSLASH', '`': 'GRAVE'}


def code(name: str) -> int:
    return getattr(rs.INJ.e, 'KEY_' + name.upper())


def type_text(text: str) -> None:
    e = rs.INJ.e
    for ch in text:
        shift = ch.isupper() or ch in SHIFTED
        name = SHIFTED.get(ch) or PLAIN.get(ch) or ch.upper()
        if shift:
            rs.INJ.ui.write(e.EV_KEY, e.KEY_LEFTSHIFT, 1); rs.INJ.ui.syn()
        rs.INJ.key(code(name), 0.04)
        if shift:
            rs.INJ.ui.write(e.EV_KEY, e.KEY_LEFTSHIFT, 0); rs.INJ.ui.syn()
        time.sleep(0.04)


def run(command: str, log) -> bool:
    op, _, rest = command.strip().partition(' ')
    if op == 'quit':
        return False
    if op == 'wait':
        time.sleep(float(rest))
    elif op == 'snap':
        rs.screenshot(SESSION / f'{rest}.png')
    else:
        if not rs.focus_window(rs.CAPTION):
            log(f'skipped (game not focused): {command.strip()}')
            return True
        time.sleep(0.2)
        if op == 'key':
            name, _, hold = rest.partition(' ')
            rs.INJ.key(code(name), float(hold or 0.08))
        elif op == 'type':
            type_text(rest)
        elif op == 'console':   # open, type, run, close
            rs.INJ.key(code('GRAVE')); time.sleep(0.5)
            type_text(rest); time.sleep(0.2)
            rs.INJ.key(code('ENTER')); time.sleep(1.0)
            rs.INJ.key(code('GRAVE'))
        elif op == 'click':
            rs.act_lclick()
        elif op == 'move':
            dx, dy = (int(v) for v in rest.split())
            rs.INJ.move(dx, dy)
        else:
            log(f'unknown: {command.strip()}')
            return True
    log(f'done: {command.strip()}')
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--timeout', type=float, default=1800, help='seconds before the session ends by itself')
    a = ap.parse_args()
    if rs.game_running():
        print('Skyrim is already running'); return 1
    SESSION.mkdir(parents=True, exist_ok=True)
    cmd, out = SESSION / 'cmd.txt', SESSION / 'log.txt'
    cmd.write_text(''); out.write_text('')

    def log(line: str) -> None:
        with out.open('a') as f:
            f.write(time.strftime('%H:%M:%S ') + line + '\n')

    rs.INJ = rs.Injector()
    original_ini, original_prefs = rs.INI.read_text(), rs.PREFS.read_text(errors='replace')
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    service = None
    try:
        rs.ini_override(rs.PREFS, rs.TEST_PREFS)
        rs.set_autorun(False)
        service = rs.start_service()
        rs.start_game()
        log('launched')
        t0, done = time.time(), 0
        while time.time() - t0 < a.timeout:
            time.sleep(0.3)
            if time.time() - t0 > 60 and not rs.game_running():
                log('game exited'); break
            lines = cmd.read_text().splitlines()
            while done < len(lines):
                done += 1
                if not run(lines[done - 1], log):
                    return 0
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        try:
            rs.INI.write_text(original_ini)
            if service:
                service.terminate()
        finally:
            try:
                rs.stop_game()
            finally:
                rs.PREFS.write_text(original_prefs)
                log('ended; prefs restored')
    return 0


if __name__ == '__main__':
    sys.exit(main())
