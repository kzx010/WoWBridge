#!/usr/bin/env python3
"""Launch Skyrim (Steam -> SKSE), wait for a log marker / exit / timeout, collect logs and screenshots.

usage: run_skyrim.py [--until REGEX] [--timeout S] [--autorun [--only STEP,...]] [--keep] [--snap NAME@SECONDS ...]
  --until    stop waiting once WoWBridge.log matches REGEX (default: test-suite end marker)
  --autorun  enable the in-game self-test suite for this run (WoWBridge.ini [Test] autorun=1)
  --keep     leave the game running afterwards
"""
import argparse, json, re, shutil, signal, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import winhost  # noqa: E402

PROJECT = Path(__file__).resolve().parents[2]
GAME = PROJECT / 'steamapps/common/Skyrim Special Edition'
DOCS = (winhost.docs_dir() if winhost.IS_WSL else   # native Windows Skyrim driven from WSL, or Proton on Linux
        PROJECT / 'steamapps/compatdata/489830/pfx/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition')
SKSE_LOGS = DOCS / 'SKSE'
INI = GAME / 'Data/SKSE/Plugins/WoWBridge.ini'
STEAM = GAME.resolve().parents[2]   # Steam's root (steamapps/common/<game> under it)
OUT = Path(__file__).resolve().parent.parent / 'build/runs'
GAME_PROC = r'^S:.*SkyrimSE\.exe'           # Wine process cmdline; anchored so it never matches our own shell


def game_running() -> bool:
    if winhost.IS_WSL:
        return bool(winhost.pids('SkyrimSE.exe'))
    return subprocess.run(['pgrep', '-f', GAME_PROC], capture_output=True).returncode == 0


def stop_game(grace: float = 15) -> None:
    if winhost.IS_WSL:   # a launch refuses to start beside a running game, so these exact PIDs are the run's own
        for force, wait in ((False, grace), (True, 10)):
            for pid in winhost.pids('SkyrimSE.exe'):
                winhost.kill(pid, force)
            deadline = time.time() + wait
            while game_running() and time.time() < deadline:
                time.sleep(0.5)
            if not game_running():
                return
        return
    subprocess.run(['pkill', '-f', GAME_PROC])
    deadline = time.time() + grace
    while game_running() and time.time() < deadline:
        time.sleep(0.5)
    if game_running():
        subprocess.run(['pkill', '-9', '-f', GAME_PROC])
        deadline = time.time() + 10
        while game_running() and time.time() < deadline:   # the next launch must not find it still going
            time.sleep(0.5)
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


def steam_overlay_on() -> bool:
    """Steam's in-game overlay for Skyrim SE (app 489830): on unless its OverlayAppEnable is 0 in a user's
    localconfig.vdf. It takes Shift+Tab, and an injected Shift+Tab opens it over the game, pausing a run."""
    configs = list((STEAM / 'userdata').glob('*/config/localconfig.vdf')) if STEAM.exists() else []
    for path in configs:
        text = path.read_text(errors='replace')
        for start in re.finditer(r'"489830"\s*\{', text):   # the app has several blocks; the setting is in one of them
            depth, i = 1, start.end()
            while depth and i < len(text):
                depth += {'{': 1, '}': -1}.get(text[i], 0)
                i += 1
            if re.search(r'"OverlayAppEnable"\s*"0"', text[start.end():i]):
                return False
    return bool(configs)


def set_autorun(on: bool, only: str = '') -> None:
    # Tracked by deploy.py; restored to the packaged default after the run. only: the steps to run after the setup;
    # steamOverlay: the overlay owns Shift+Tab, so wow_shift_tab reports it instead of pausing the game behind it.
    text = re.sub(r'(?m)^(only|steamOverlay)=.*\n', '', INI.read_text())
    extra = (f'\nonly={only}' if only else '') + ('\nsteamOverlay=1' if on and steam_overlay_on() else '')
    INI.write_text(re.sub(r'(?m)^autorun=\d', f'autorun={int(on)}' + extra, text))


PREFS = DOCS / 'Skyrim.ini'
DISPLAY_PREFS = DOCS / 'SkyrimPrefs.ini'   # [Display] lives here; it overrides the same keys in Skyrim.ini
TEST_PREFS = {('General', 'bAlwaysActive'): '1'}   # keep simulating while unfocused
if winhost.IS_WSL:   # the player's own saves on this PC stay untouched: test runs (and their autosaves) use their own
    TEST_PREFS[('General', 'sLocalSavePath')] = 'WoWBridgeTestSaves\\'


def display_prefs() -> dict:
    """Test runs play full screen as a borderless window the size of the desktop (the user's choice, October 15): it
    covers the desktop like full screen while screen captures and focus changes keep working (an exclusive full
    screen captures black). Only for the run: the player's own display settings are restored after it."""
    try:
        width, height = winhost.screen_size() if winhost.IS_WSL else (0, 0)
    except Exception:
        return {}
    if not width or not height:
        return {}
    return {('Display', 'bFull Screen'): '0', ('Display', 'bBorderless'): '1',
            ('Display', 'iSize W'): str(width), ('Display', 'iSize H'): str(height)}


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


if winhost.IS_WSL:
    from winhost import focus_window  # noqa: E402
else:
    from inputdev import focus_window, kwin  # noqa: E402

CAPTION = 'Skyrim Special Edition'


class Injector:
    """Kernel-level virtual keyboard+mouse (/dev/uinput): reaches the focused window like real hardware,
    unlike XTEST which KDE's XWayland does not deliver to the game."""

    def __init__(self):
        if winhost.IS_WSL:   # Windows SendInput behind the same evdev-style interface
            self.e, self.ui = winhost.ecodes, winhost.UInput()
            return
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
    if winhost.IS_WSL:   # the game window's client area (the desktop when it has none)
        winhost.screenshot(path, CAPTION)
        return
    subprocess.run(['spectacle', '-b', '-n', '-f', '-o', str(path)], capture_output=True, timeout=30)


def start_game() -> None:
    """Start Skyrim through SKSE, the reference WoW client minimized first (its full screen keeps the focus)."""
    if winhost.IS_WSL:   # Steam is running; the loader starts the game directly (no Steam launch dialogs)
        winhost.minimize('World of Warcraft')
        winhost.launch(GAME / 'skse64_loader.exe')
    else:
        kwin('for (const w of workspace.windowList()) if (w.caption === "World of Warcraft") w.minimized = true; print("TAG|");')
        subprocess.Popen(['steam', '-applaunch', '489830'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        play_anyway()


def start_service() -> subprocess.Popen | None:
    """The gameplay service the plugin casts through, unless one is already listening on 7878."""
    import socket
    with socket.socket() as sock:
        if sock.connect_ex(('127.0.0.1', 7878)) == 0:
            return None
    root = Path(__file__).resolve().parent.parent
    trace = (RUN_DIR or root / 'build') / 'service_trace.jsonl'
    return subprocess.Popen([sys.executable, '-m', 'service.server', '--catalog', str(root / 'build/catalog.json'),
                             '--trace', str(trace)], cwd=root,
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


# Single definition of launch order, selections and timeout budgets (seconds).
REQUIRED_SUITE = (
    ('ordinary', '', 3600),   # ~300 default steps: 171 of them took 1809 s (October 15)
    ('explicit-combat', 'wow_prowl_miss,wow_melee_release,wow_feral_release,wow_charge_release,wow_melee_retarget,wow_threat_control,wow_missile_target,wow_swipe_targets,wow_dragon_death_loot', 900),
    ('explicit-world', 'wow_courier_delivery,wow_native_item_text,wow_customize,wow_panel_helpers', 900),
    # Alone: it sets a town on her, and a step after it could not save (Skyrim refuses during combat).
    ('aggression', 'wow_spell_aggression', 300),
    # Each alone: the Survival off/on fixture can queue a native prompt over the start dialog, and the compass
    # fallback needs its launch without the managed compass manifest (WITHHELD).
    ('start-dialog', 'wow_start_dialog', 300),
    ('compass-fallback', 'wow_compass_fallback', 300),
    ('cold-seed', 'wow_talents,wow_talents_mark,wow_cold_persistence_seed', 600),
    ('cold-verify', 'wow_cold_persistence_verify', 300),
    ('onboarding', 'wow_onboarding_activation', 300),
    ('diagnostic', 'wow_dragon_combat,wow_dragon_projectile,wow_flight_controller,wb_sex_relog,wow_flight_kinematic', 900),
)


# A launch's game file set aside for it, restored after it whatever happens.
WITHHELD = {'compass-fallback': GAME / 'Data/SKSE/Plugins/WoWBridge/Icons/compass.json'}


def launch(a, run_dir: Path | None = None) -> dict:
    """One launch, including configuration restoration and evidence collection."""
    if game_running():
        raise RuntimeError('Skyrim is already running')   # a suite records it as this launch's failure
    global INJ
    INJ = Injector()
    global RUN_DIR
    run_dir = RUN_DIR = run_dir or OUT / time.strftime('%Y%m%d-%H%M%S')
    run_dir.mkdir(parents=True)
    log = SKSE_LOGS / 'WoWBridge.log'
    # Read both originals before writing either; every write below is undone in finally, also on SIGTERM (an
    # outer timeout) and on a failure before the game starts: never leave the player's game in autorun.
    original_ini = INI.read_text()
    original_prefs = PREFS.read_text(errors='replace')
    original_display = DISPLAY_PREFS.read_text(errors='replace') if DISPLAY_PREFS.exists() else None
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    global SERVICE
    SERVICE = None
    t0 = time.time()
    snaps = sorted((float(s.split('@')[1]), s.split('@')[0]) for s in a.snap)
    started, result = False, 'timeout'
    next_focus, text = 0.0, ''
    done_actions, runner_log = 0, []
    # Hang watchdog: the plugin's steps stop with the game (a pausing menu, the lock screen, the Steam overlay), and
    # three suites sat frozen for 15-46 minutes before a person noticed. Once the log stops growing for STALL_SECS,
    # the runner screenshots the screen, presses Escape once (it closes a stray Skyrim menu), and gives it another
    # STALL_SECS; still no log, the launch ends as 'stalled' rather than idling to the timeout. The plugin's
    # main-thread hang report triggers the same handling immediately, even when render-thread logs keep growing.
    STALL_SECS = 90
    last_log_size, last_log_change, stall_escapes = -1, time.time(), 0
    hang_reports = 0
    try:
        ini_override(PREFS, TEST_PREFS)
        if original_display is not None:
            ini_override(DISPLAY_PREFS, display_prefs())
        set_autorun(a.autorun, a.only)
        SERVICE = None if getattr(a, 'external_service', False) else start_service()
        # The reference WoW client full screen keeps the focus from the game (KWin will not hand it over), and the
        # game then stops at its first frame: out of the way while it runs.
        # The previous launch's game may still be writing its log as it shuts down: its "suite passed" line then
        # matched this launch's marker 24 s in (cold-verify, suite 231431). Wait for it to exit, then clear its
        # log and results (already copied into its own run directory) so only this launch's lines can match.
        quiet = time.time()
        while game_running() and time.time() - quiet < 60:
            time.sleep(0.5)
        for stale in (log, SKSE_LOGS / 'WoWBridge_test.jsonl'):
            try:
                stale.unlink()
            except FileNotFoundError:
                pass
        t0 = time.time()
        start_game()
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
            if started and fresh:
                reports = text.count('hang: main thread silent')
                new_hang = reports > hang_reports
                hang_reports = reports
                if len(text) != last_log_size:
                    last_log_size, last_log_change = len(text), time.time()
                if new_hang or ('selftest: ' in text and time.time() - last_log_change > STALL_SECS):
                    stall_escapes += 1
                    screenshot(run_dir / f'stall_{stall_escapes}.png')
                    reason = 'plugin reported a main-thread hang' if new_hang else f'no log for {STALL_SECS}s'
                    runner_log.append(f'STALL: {reason} at +{time.time() - t0:.0f}s (last line: '
                                      f'{text.rstrip().rsplit(chr(10), 1)[-1][:160]!r}); active window {winhost.active_caption()!r}')
                    print(runner_log[-1], flush=True)
                    if stall_escapes >= 2:
                        result = 'stalled'
                        break
                    # A hang report is evidence (screenshot + stack already in the log): no Escape, which can derail a
                    # launch that was merely loading; a second report 15 s later ends the launch as stalled.
                    if not new_hang and focus_window(CAPTION):
                        act_key('esc')               # a stray Skyrim menu (Journal, map, system) closes; harmless otherwise
                        runner_log.append('STALL: pressed Escape once')
                    last_log_change = time.time()
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
        signal.signal(signal.SIGTERM, signal.SIG_IGN)   # finish restoring even if a second SIGTERM follows
        try:
            INI.write_text(original_ini)
            time.sleep(1)
            for f in [*SKSE_LOGS.glob('*.log'), *SKSE_LOGS.glob('*.jsonl'), *SKSE_LOGS.glob('capture_*.png')]:
                if f.stat().st_mtime >= t0:
                    shutil.copy(f, run_dir / f.name)
        finally:
            try:
                if SERVICE:
                    SERVICE.terminate()
            finally:
                try:
                    if not a.keep:
                        stop_game()
                finally:
                    PREFS.write_text(original_prefs)   # game may rewrite prefs on exit
                    if original_display is not None:
                        DISPLAY_PREFS.write_text(original_display)
                    (run_dir / 'runner.log').write_text('\n'.join(runner_log))
    print(f'result={result} elapsed={time.time() - t0:.0f}s run_dir={run_dir}')
    print('\n'.join(runner_log))
    for name in ('WoWBridge.log', 'WoWBridge_test.jsonl'):
        p = run_dir / name
        print(f'--- {name}')
        print(p.read_text(errors='replace')[-4000:] if p.exists() else '(missing)')
    issues = suite_failures(run_dir, a.only) if a.autorun else []
    if result != 'marker':
        issues.insert(0, 'launch ended: ' + result)
    for issue in issues:
        print('validation failed: ' + issue)
    return {'result': result, 'run_dir': str(run_dir), 'elapsed': time.time() - t0,
            'issues': issues, 'status': 'failed' if issues else 'passed'}


def step_evidence(run_dir: Path) -> tuple[list, list]:
    """Derive timing from consecutive records, never assume old logs have frames."""
    steps, talents = [], []
    previous_t, previous_frames = 0, None   # the first row's span is the game's loading, not a frame rate
    try:
        lines = (run_dir / 'WoWBridge_test.jsonl').read_text().splitlines()
    except OSError:
        return steps, talents
    for line in lines:
        try:
            row = json.loads(line)
        except ValueError:
            continue   # suite_failures reports malformed evidence
        if not isinstance(row, dict):
            continue
        t, frames = row.get('t'), row.get('frames')
        duration = t - previous_t if isinstance(t, (int, float)) and previous_t is not None else None
        fps = None
        if (duration is not None and duration > 0 and type(frames) is int
                and previous_frames is not None and frames >= previous_frames):
            fps = (frames - previous_frames) / duration
        if row.get('event') == 'talent_result':
            detail = row.get('result')
            if not isinstance(detail, dict) or detail.get('pass') is not True:
                talents.append({**row, 'error': str((detail or {}).get('error', ''))[:300]
                                if isinstance(detail, dict) else 'malformed talent result'})
        elif isinstance(row.get('test'), str):
            step = {'test': row['test'], 'pass': row.get('pass') is True,
                    'error': str(row.get('error', ''))[:300], 'duration': duration, 'fps': fps}
            if 'after_unrestored' in row:
                step['after_unrestored'] = row['after_unrestored']
            steps.append(step)
        previous_t = t if isinstance(t, (int, float)) else previous_t
        previous_frames = frames if type(frames) is int else None
    return steps, talents


def triage_plan(source: dict) -> list:
    affected = {}
    for run in source['launches']:
        if run['name'] == 'diagnostic' or run['status'] == 'passed':
            continue
        names = [step['test'] for step in run.get('steps', []) if not step['pass']]
        names += [row['step'] for row in run.get('failed_talents', []) if row.get('step')]
        seen = {step['test'] for step in run.get('steps', [])}
        names += [name for name in run.get('only', '').split(',') if name and name not in seen]
        # No final row (e.g. crash): reproduce the affected selection.
        affected[run['name']] = ','.join(dict.fromkeys(names)) or run.get('only', '')
    if 'cold-verify' in affected:
        affected['cold-seed'] = dict((name, only) for name, only, _ in REQUIRED_SUITE)['cold-seed']
    return [(name, affected[name], timeout) for name, _, timeout in REQUIRED_SUITE if name in affected]


def run_suite(a) -> int:
    triage = bool(a.rerun_failed)
    source = json.loads((Path(a.rerun_failed) / 'summary.json').read_text()) if triage else None
    plan = triage_plan(source) if triage else list(REQUIRED_SUITE[:-1])
    if a.diagnostic:
        plan.append(REQUIRED_SUITE[-1])
    suite_dir = OUT / ('suite-' + time.strftime('%Y%m%d-%H%M%S'))
    suite_dir.mkdir(parents=True)
    summary = {'suite': 'required', 'triage': triage, 'source': a.rerun_failed, 'launches': []}
    seed_passed = False
    skip = set(filter(None, getattr(a, 'skip', '').split(',')))
    for index, (name, only, timeout) in enumerate(plan):
        run_dir = suite_dir / name
        if name in skip:   # still gating: a suite with a skipped launch never passes
            run = {'status': 'skipped', 'result': 'skipped', 'run_dir': None, 'elapsed': 0,
                   'issues': ['skipped by --skip'], 'steps': [], 'failed_talents': []}
        elif name == 'cold-verify' and not seed_passed:
            run = {'status': 'not_run', 'result': 'not_run', 'run_dir': None, 'elapsed': 0,
                   'issues': ['cold-seed did not pass'], 'steps': [], 'failed_talents': []}
        else:
            options = argparse.Namespace(**vars(a))
            options.autorun, options.only, options.timeout = True, only, timeout
            options.until = r'selftest: suite (passed|failed)'
            options.keep = a.keep and index == len(plan) - 1
            launch_start = time.time()
            withheld = WITHHELD.get(name)
            aside = withheld and withheld.with_name(withheld.name + '.withheld')
            try:
                if withheld:
                    withheld.rename(aside)
                run = launch(options, run_dir)
            except Exception as exc:
                run = {'status': 'failed', 'result': 'error', 'run_dir': str(run_dir),
                       'elapsed': time.time() - launch_start, 'issues': [str(exc)]}
            finally:
                if aside and aside.exists():
                    aside.rename(withheld)
            run['steps'], run['failed_talents'] = step_evidence(run_dir)
        run.update(name=name, only=only, gating=name != 'diagnostic')
        if name == 'cold-seed':
            seed_passed = run['status'] == 'passed'
        summary['launches'].append(run)
    gating_passed = all(run['status'] == 'passed' for run in summary['launches'] if run['gating'])
    summary['verdict'] = ('triage_passed' if gating_passed else 'triage_failed') if triage else ('passed' if gating_passed else 'failed')
    steps = [dict(step, launch=run['name']) for run in summary['launches'] for step in run['steps']]
    summary['slowest_steps'] = sorted((s for s in steps if s['duration'] is not None), key=lambda s: s['duration'], reverse=True)[:10]
    summary['under_30_fps'] = [s for s in steps if s['fps'] is not None and s['fps'] < 30]
    lines = [f"Required suite: {summary['verdict']} ({suite_dir})"]
    for run in summary['launches']:
        lines.append(f"{run['name']}: {run['status']} result={run['result']} elapsed={run['elapsed']:.1f}s run_dir={run['run_dir']}" + (' (diagnostic)' if not run['gating'] else ''))
        lines.extend('  ' + issue for issue in run['issues'])
        lines.extend('  ' + json.dumps(step) for step in run['steps'])
        lines.extend('  failed talent: ' + json.dumps(row) for row in run['failed_talents'])
    lines += ['10 slowest steps:', *[json.dumps(s) for s in summary['slowest_steps']],
              'Steps under 30 fps:', *[json.dumps(s) for s in summary['under_30_fps']]]
    (suite_dir / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    report = '\n'.join(lines) + '\n'
    (suite_dir / 'summary.txt').write_text(report)
    print(report)
    return 0 if gating_passed else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--until', default=r'selftest: suite (passed|failed)')
    ap.add_argument('--timeout', type=float, default=240)
    ap.add_argument('--autorun', action='store_true')
    ap.add_argument('--only', default='', help='STEP[,STEP...]: just these steps after the setup')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--snap', action='append', default=[], help='NAME@SECONDS after launch')
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--suite', choices=['required'])
    mode.add_argument('--rerun-failed', metavar='SUITE_DIR')
    ap.add_argument('--diagnostic', action='store_true')
    ap.add_argument('--skip', default='', help='LAUNCH[,LAUNCH...]: suite launches not run (the suite then fails)')
    ap.add_argument('--external-service', action='store_true',
                    help="the game uses a service started elsewhere (a release's setup.py service on Windows)")
    a = ap.parse_args()
    if a.diagnostic and not (a.suite or a.rerun_failed):
        ap.error('--diagnostic requires --suite or --rerun-failed')
    if (a.suite or a.rerun_failed) and (a.only or a.autorun or a.timeout != 240 or a.until != r'selftest: suite (passed|failed)'):
        ap.error('suite modes choose their own autorun, selections, markers and timeouts')
    if game_running():
        sys.exit('Skyrim is already running')
    if a.suite or a.rerun_failed:
        return run_suite(a)
    return 0 if launch(a)['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
