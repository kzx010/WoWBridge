#!/usr/bin/env python3
"""Launch Skyrim (Steam -> SKSE), wait for a log marker / exit / timeout, collect logs and screenshots.

usage: run_skyrim.py [--until REGEX] [--timeout S] [--autorun [--only STEP,...]] [--keep] [--snap NAME@SECONDS ...]
  --until    stop waiting once WoWBridge.log matches REGEX (default: test-suite end marker)
  --autorun  enable the in-game self-test suite for this run (WoWBridge.ini [Test] autorun=1)
  --keep     leave the game running afterwards
"""
import argparse, json, re, shutil, subprocess, sys, time
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[2]
GAME = PROJECT / 'steamapps/common/Skyrim Special Edition'
DOCS = PROJECT / 'steamapps/compatdata/489830/pfx/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition'
SKSE_LOGS = DOCS / 'SKSE'
INI = GAME / 'Data/SKSE/Plugins/WoWBridge.ini'
OUT = Path(__file__).resolve().parent.parent / 'build/runs'
GAME_PROC = r'^S:.*SkyrimSE\.exe'           # Wine process cmdline; anchored so it never matches our own shell


def game_running() -> bool:
    return subprocess.run(['pgrep', '-f', GAME_PROC], capture_output=True).returncode == 0


def stop_game(grace: float = 15) -> None:
    subprocess.run(['pkill', '-f', GAME_PROC])
    deadline = time.time() + grace
    while game_running() and time.time() < deadline:
        time.sleep(0.5)
    if game_running():
        subprocess.run(['pkill', '-9', '-f', GAME_PROC])
    # A launch that hung leaves Steam's launch chain behind (the SKSE loader waiting under the reaper), and the next
    # launch then waits on it for ever.
    subprocess.run(['pkill', '-f', 'skse64_loader.exe'])
    subprocess.run(['pkill', '-f', 'AppId=489830'])


def play_anyway(wait: float = 25.0) -> None:
    """Steam's 'Unable to Sync' warning (Steam Cloud is off for Skyrim, the user's choice) holds the launch: answer it
    'Play anyway' - only while Steam has the focus and its blue button is where the centred dialog puts it."""
    from PIL import Image
    from inputdev import Input, screen_size
    end = time.time() + wait
    while time.time() < end:
        time.sleep(1.5)
        if game_running():
            return
        if kwin('const w = workspace.activeWindow; print("TAG|" + (w ? w.caption : "none"));') != ['Steam']:
            continue
        w, h = screen_size()
        x, y = w // 2 + 123, h // 2 + 77   # 'Play anyway' at 2560x1440: (1403, 797)
        shot = OUT / 'steam_dialog.png'
        subprocess.run(['spectacle', '-b', '-n', '-f', '-o', str(shot)], capture_output=True)
        try:
            r, g, b = Image.open(shot).convert('RGB').getpixel((x, y - 12))   # the button, above its label
        except (OSError, IndexError):
            continue
        # The same blue button is 'Continue' on Steam's 'logged in on another computer' error, which would end the
        # user's other session: read the dialog and answer only the sync warning (the user's rule: never click that).
        dialog = OUT / 'steam_dialog_text.png'   # the centred dialog, enlarged: OCR reads it there, not on the full screen
        Image.open(shot).crop((w // 2 - 380, h // 2 - 160, w // 2 + 380, h // 2 + 160)).resize((1520, 640)).save(dialog)
        text = subprocess.run(['tesseract', str(dialog), '-'], capture_output=True, text=True).stdout
        if 'Unable to Sync' not in text:
            if 'another computer' in text:
                print('Steam asks to disconnect another computer\'s session: left for the user', flush=True)
                return
            continue
        if b > 200 and r < 80 and 100 < g < 200:   # Steam's blue button: pressed until the dialog goes
            time.sleep(1.0)   # a click as it appears is not taken
            Input().click(x, y)
            print(f'answered Steam\'s sync warning: Play anyway at ({x}, {y})', flush=True)


def set_autorun(on: bool, only: str = '') -> None:
    # Tracked by deploy.py; restored to the packaged default after the run. only: the steps to run after the setup.
    text = re.sub(r'(?m)^only=.*\n', '', INI.read_text())
    INI.write_text(re.sub(r'(?m)^autorun=\d', f'autorun={int(on)}' + (f'\nonly={only}' if only else ''), text))


PREFS = DOCS / 'Skyrim.ini'
TEST_PREFS = {('General', 'bAlwaysActive'): '1'}   # keep simulating while unfocused


def ini_override(path: Path, overrides: dict) -> str:
    """Apply section/key overrides to a Bethesda INI; returns the original text for restoring."""
    original = path.read_text(errors='replace')
    lines = original.splitlines()
    for (section, key), value in overrides.items():
        start = next((i for i, l in enumerate(lines) if l.strip().lower() == f'[{section.lower()}]'), None)
        if start is None:
            lines += [f'[{section}]', f'{key}={value}']
            continue
        end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith('[')), len(lines))
        hit = next((i for i in range(start + 1, end) if lines[i].split('=')[0].strip().lower() == key.lower()), None)
        if hit is None:
            lines.insert(start + 1, f'{key}={value}')
        else:
            lines[hit] = f'{key}={value}'
    path.write_text('\n'.join(lines) + '\n')
    return original


sys.path.insert(0, str(Path(__file__).resolve().parent))
from inputdev import focus_window, kwin  # noqa: E402

CAPTION = 'Skyrim Special Edition'


class Injector:
    """Kernel-level virtual keyboard+mouse (/dev/uinput): reaches the focused window like real hardware,
    unlike XTEST which KDE's XWayland does not deliver to the game."""

    def __init__(self):
        from evdev import UInput, ecodes as e
        self.e = e
        caps = {e.EV_KEY: [*range(1, 249), e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE],
                e.EV_REL: [e.REL_X, e.REL_Y, e.REL_WHEEL]}
        self.ui = UInput(caps, name='wowbridge-test-input')
        time.sleep(1.0)   # let libinput/KWin pick up the new device

    def move(self, dx: int, dy: int) -> None:
        self.ui.write(self.e.EV_REL, self.e.REL_X, dx)
        self.ui.write(self.e.EV_REL, self.e.REL_Y, dy)
        self.ui.syn()

    def key(self, code: int, hold: float = 0.08) -> None:
        self.ui.write(self.e.EV_KEY, code, 1)
        self.ui.syn()
        time.sleep(hold)
        self.ui.write(self.e.EV_KEY, code, 0)
        self.ui.syn()


INJ: Injector | None = None
RUN_DIR: Path | None = None
SERVICE: subprocess.Popen | None = None   # the gameplay service this run started (None: one was already running)


def act_mouse_move() -> None:
    for _ in range(10):
        INJ.move(40, 15)
        time.sleep(0.05)


def act_key(name: str) -> None:
    INJ.key(getattr(INJ.e, f'KEY_{name.upper()}'))


def act_service_stop() -> None:
    if SERVICE is None:
        raise RuntimeError('service_stop requires a runner-owned service; port 7878 was already in use. '
                           'Arrange an owned service before rerunning wow_service_offline.')
    if SERVICE.poll() is not None:
        raise RuntimeError('service_stop: runner-owned service exited before the offline probe')
    SERVICE.terminate()
    SERVICE.wait(10)


def act_service_start() -> None:
    global SERVICE
    if SERVICE is None or SERVICE.poll() is None:
        raise RuntimeError('service_start requires the runner-owned service to have stopped')
    SERVICE = start_service()
    if SERVICE is None:
        raise RuntimeError('service_start: another service took port 7878; cannot own the restart')
    time.sleep(1.0)   # listening
    if SERVICE.poll() is not None:
        raise RuntimeError('service_start: restarted service exited before listening')


def act_lclick() -> None:
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.BTN_LEFT, 1)
    INJ.ui.syn()
    time.sleep(0.08)
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.BTN_LEFT, 0)
    INJ.ui.syn()


def act_rclick() -> None:
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.BTN_RIGHT, 1)
    INJ.ui.syn()
    time.sleep(0.08)
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.BTN_RIGHT, 0)
    INJ.ui.syn()


def act_rdrag() -> None:
    """Right button held while the mouse moves right: steering."""
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.BTN_RIGHT, 1)
    INJ.ui.syn()
    time.sleep(0.05)
    for _ in range(10):
        INJ.move(40, 0)
        time.sleep(0.03)
    time.sleep(0.05)
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.BTN_RIGHT, 0)
    INJ.ui.syn()


def act_shift_tab() -> None:
    """Shift held through Tab: Skyrim's Tab menu."""
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.KEY_LEFTSHIFT, 1)
    INJ.ui.syn()
    time.sleep(0.1)
    act_key('tab')
    time.sleep(0.1)
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.KEY_LEFTSHIFT, 0)
    INJ.ui.syn()


def act_alt_lclick() -> None:
    """Alt held through a left click: the user's grab."""
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.KEY_LEFTALT, 1)
    INJ.ui.syn()
    time.sleep(0.1)
    act_lclick()
    time.sleep(0.1)
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.KEY_LEFTALT, 0)
    INJ.ui.syn()


def act_alt_rclick() -> None:
    """Alt held through a right click: the user's attack on anything."""
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.KEY_LEFTALT, 1)
    INJ.ui.syn()
    time.sleep(0.1)
    act_rclick()
    time.sleep(0.1)
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.KEY_LEFTALT, 0)
    INJ.ui.syn()


def act_ctrl_movement(key: str) -> None:
    """Ctrl reaches the game before movement; movement and sneak toggle must both work."""
    INJ.ui.write(INJ.e.EV_KEY, INJ.e.KEY_LEFTCTRL, 1)
    INJ.ui.syn()
    time.sleep(0.12)
    try:
        INJ.key(getattr(INJ.e, f'KEY_{key.upper()}'), 0.5)
    finally:
        INJ.ui.write(INJ.e.EV_KEY, INJ.e.KEY_LEFTCTRL, 0)
        INJ.ui.syn()


ACTIONS = {  # names the plugin requests via "selftest: ACTION <name>" log lines
    'mouse_move': act_mouse_move,
    'rclick': act_rclick,
    'lclick': act_lclick,
    'service_stop': act_service_stop,
    'service_start': act_service_start,
    'rdrag': act_rdrag,
    'key_1': lambda: act_key('1'),
    'alt_rclick': act_alt_rclick,
    'alt_lclick': act_alt_lclick,
    'alt_down': lambda: (INJ.ui.write(INJ.e.EV_KEY, INJ.e.KEY_LEFTALT, 1), INJ.ui.syn()),
    'alt_up': lambda: (INJ.ui.write(INJ.e.EV_KEY, INJ.e.KEY_LEFTALT, 0), INJ.ui.syn()),
    'shift_tab': act_shift_tab,
    'ctrl_tap': lambda: act_key('leftctrl'),
    **{f'ctrl_{key}': (lambda key=key: act_ctrl_movement(key)) for key in 'wsadqe'},
}


def key_action(name: str):
    """key:<evdev key name>[:<seconds held>], e.g. key:tab, key:a:0.5; key:m+esc presses each in turn, 1.5 s apart."""
    _, keys, *hold = name.split(':')

    def run():
        for i, key in enumerate(keys.split('+')):
            if i:
                time.sleep(1.5)
            INJ.key(getattr(INJ.e, f'KEY_{key.upper()}'), float(hold[0]) if hold else 0.08)
    return run


def ldrag_action(name: str):
    """ldrag:X1,Y1,X2,Y2: left-drag between screen pixels in the game's free cursor mode."""
    x1, y1, x2, y2 = map(int, name.split(':', 1)[1].split(','))

    def run():
        # The game's free cursor consumes relative motion at one pixel per unit. Clamp it to the origin first,
        # so a previous action (or a camera drag) cannot leave these absolute coordinates offset.
        INJ.move(-32768, -32768)
        time.sleep(0.15)
        INJ.move(x1, y1)
        time.sleep(0.15)
        INJ.ui.write(INJ.e.EV_KEY, INJ.e.BTN_LEFT, 1)
        INJ.ui.syn()
        try:
            time.sleep(0.1)
            last_x, last_y = x1, y1
            for i in range(1, 21):
                x = round(x1 + (x2 - x1) * i / 20)
                y = round(y1 + (y2 - y1) * i / 20)
                INJ.move(x - last_x, y - last_y)
                last_x, last_y = x, y
                time.sleep(0.03)
            time.sleep(0.1)
        finally:
            INJ.ui.write(INJ.e.EV_KEY, INJ.e.BTN_LEFT, 0)
            INJ.ui.syn()
    return run


def perform(name: str, log_lines: list) -> None:
    # These actions need process ownership, not desktop focus. Never acknowledge a shared-service no-op.
    if name in ('service_stop', 'service_start'):
        try:
            ACTIONS[name]()
        except Exception as exc:              # the step then fails on its own; the rest of the suite still runs
            log_lines.append(f'action {name} FAILED: {exc}')
            return
        log_lines.append(f'action {name} ok (runner-owned service)')
        return
    win = focus_window(CAPTION)               # input goes to the focused window: never type into another one
    if not win:
        time.sleep(0.5)
        win = focus_window(CAPTION)
    if name.startswith('snap:'):
        time.sleep(0.15)                      # let the held state reach the screen
        screenshot(RUN_DIR / f'{name[5:]}.png')
        log_lines.append(f'snap {name[5:]}')
        return
    fn = ACTIONS.get(name) or (key_action(name) if name.startswith('key:') else
                             ldrag_action(name) if name.startswith('ldrag:') else None)
    log_lines.append(f'action {name} focused={win} {"ok" if fn and win else "UNKNOWN" if not fn else "SKIPPED (no focus)"}')
    if fn and win:
        fn()


def screenshot(path: Path) -> None:
    subprocess.run(['spectacle', '-b', '-n', '-f', '-o', str(path)], capture_output=True, timeout=30)


def start_service() -> subprocess.Popen | None:
    """The gameplay service the plugin casts through, unless one is already listening on 7878."""
    import socket
    with socket.socket() as sock:
        if sock.connect_ex(('127.0.0.1', 7878)) == 0:
            return None
    root = Path(__file__).resolve().parent.parent
    return subprocess.Popen([sys.executable, '-m', 'service.server', '--catalog', str(root / 'build/catalog.json'),
                             '--trace', str(root / 'build/service_trace.jsonl')], cwd=root,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)



def suite_failures(run_dir: Path, only: str = '') -> list[str]:
    """Require actual probe success and every requested name, not just an end marker."""
    issues = []
    log = run_dir / 'WoWBridge.log'
    text = log.read_text(errors='replace') if log.exists() else ''
    if not re.search(r'selftest: suite passed\b', text):
        issues.append('self-test suite did not report a pass')
    results = run_dir / 'WoWBridge_test.jsonl'
    try:
        rows = [json.loads(line) for line in results.read_text().splitlines() if line.strip()]
    except (OSError, ValueError) as error:
        return issues + [f'unreadable self-test results: {error}']
    if not rows:
        issues.append('self-test results are empty')
    seen = set()
    for row in rows:
        if isinstance(row, dict) and row.get('event') == 'talent_result':
            detail = row.get('result')
            if not isinstance(detail, dict) or detail.get('pass') is not True:
                issues.append(f"failed talent check: {row.get('step')} / {row.get('talent')}")
            continue
        if not isinstance(row, dict) or not isinstance(row.get('test'), str):
            issues.append('malformed self-test result')
            continue
        seen.add(row['test'])
        if row.get('pass') is not True:
            issues.append(f"failed self-test: {row['test']}")
    requested = {name.strip() for name in only.split(',') if name.strip()}
    if missing := requested - seen:
        issues.append('requested self-tests did not run: ' + ', '.join(sorted(missing)))
    return issues


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--until', default=r'selftest: suite (passed|failed)')
    ap.add_argument('--timeout', type=float, default=240)
    ap.add_argument('--autorun', action='store_true')
    ap.add_argument('--only', default='', help='STEP[,STEP...]: just these steps after the setup')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--snap', action='append', default=[], help='NAME@SECONDS after launch')
    a = ap.parse_args()

    if game_running():
        sys.exit('Skyrim is already running')
    global INJ
    INJ = Injector()
    global RUN_DIR
    run_dir = RUN_DIR = OUT / time.strftime('%Y%m%d-%H%M%S')
    run_dir.mkdir(parents=True)
    log = SKSE_LOGS / 'WoWBridge.log'
    original_ini = INI.read_text()
    original_prefs = ini_override(PREFS, TEST_PREFS)
    set_autorun(a.autorun, a.only)
    global SERVICE
    SERVICE = start_service()
    # The reference WoW client full screen keeps the focus from the game (KWin will not hand it over), and the game
    # then stops at its first frame: out of the way while it runs.
    kwin('for (const w of workspace.windowList()) if (w.caption === "World of Warcraft") w.minimized = true; print("TAG|");')
    t0 = time.time()
    subprocess.Popen(['steam', '-applaunch', '489830'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    play_anyway()
    snaps = sorted((float(s.split('@')[1]), s.split('@')[0]) for s in a.snap)
    started, result = False, 'timeout'
    next_focus, text = 0.0, ''
    done_actions, runner_log = 0, []
    try:
        while time.time() - t0 < a.timeout:
            time.sleep(0.2)
            started |= game_running()
            # Until the suite is under way the game must be the active window: left behind another window it stops at
            # its first frame.
            if started and time.time() >= next_focus and 'console: coc' not in text:
                focus_window(CAPTION)
                next_focus = time.time() + 3
            if snaps and time.time() - t0 >= snaps[0][0]:
                screenshot(run_dir / f'{snaps.pop(0)[1]}.png')
            fresh = log.exists() and log.stat().st_mtime >= t0
            text = log.read_text(errors='replace') if fresh else ''
            actions = re.findall(r'selftest: ACTION (\S+)', text)
            if done_actions < len(actions):        # one action at a time, acknowledged when complete
                perform(actions[done_actions], runner_log)
                done_actions += 1
                (SKSE_LOGS / 'WoWBridge_ack.txt').write_text(str(done_actions))
            if fresh and re.search(a.until, text):
                result = 'marker'
                break
            if started and not game_running():
                result = 'exited'
                break
    finally:
        INI.write_text(original_ini)
        time.sleep(1)
        for f in SKSE_LOGS.glob('*.log'):
            if f.stat().st_mtime >= t0:
                shutil.copy(f, run_dir / f.name)
        for f in [*SKSE_LOGS.glob('*.jsonl'), *SKSE_LOGS.glob('capture_*.png')]:
            if f.stat().st_mtime >= t0:
                shutil.copy(f, run_dir / f.name)
        if SERVICE:
            SERVICE.terminate()
        if not a.keep:
            stop_game()
            PREFS.write_text(original_prefs)   # the game may rewrite prefs on exit; restore after it stops
        (run_dir / 'runner.log').write_text('\n'.join(runner_log))
    print(f'result={result} elapsed={time.time() - t0:.0f}s run_dir={run_dir}')
    print('\n'.join(runner_log))
    for name in ('WoWBridge.log', 'WoWBridge_test.jsonl'):
        p = run_dir / name
        print(f'--- {name}')
        print(p.read_text(errors='replace')[-4000:] if p.exists() else '(missing)')
    if result != 'marker':
        return 1
    if a.autorun:
        issues = suite_failures(run_dir, a.only)
        for issue in issues:
            print('validation failed: ' + issue)
        return 1 if issues else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
