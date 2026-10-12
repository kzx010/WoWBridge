"""Kernel-level synthetic input (/dev/uinput): reaches the focused window like real hardware.
KDE's XWayland does not deliver XTEST (xdotool) input to games, so all automation uses this.

Two virtual devices: a keyboard + relative mouse, and an absolute pointer (tablet-style) for clicking
at screen coordinates.
"""
import subprocess
import time

from evdev import AbsInfo, UInput, ecodes as e

SHIFTED = {c: b for b, c in zip('1234567890-=[];\'\\,./`', '!@#$%^&*()_+{}:"|<>?~')}
ALIASES = {'escape': 'esc', 'return': 'enter', 'ctrl': 'leftctrl', 'shift': 'leftshift', 'alt': 'leftalt', 'del': 'delete'}
NAMED = {' ': 'SPACE', '\n': 'ENTER', '-': 'MINUS', '=': 'EQUAL', '[': 'LEFTBRACE', ']': 'RIGHTBRACE', ';': 'SEMICOLON',
         "'": 'APOSTROPHE', '\\': 'BACKSLASH', ',': 'COMMA', '.': 'DOT', '/': 'SLASH', '`': 'GRAVE'}


def screen_size() -> tuple[int, int]:
    out = subprocess.run(['xrandr', '--current'], capture_output=True, text=True).stdout
    cur = out.split('current ', 1)[1].split(',', 1)[0]
    w, h = (int(v) for v in cur.split(' x '))
    return w, h


class Input:
    def __init__(self):
        self.w, self.h = screen_size()
        caps = {e.EV_KEY: [*range(1, 249), e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE], e.EV_REL: [e.REL_X, e.REL_Y, e.REL_WHEEL]}
        self.kbd = UInput(caps, name='wowbridge-test-input')
        # Absolute *mouse* (like a VM tablet): buttons + ABS_X/Y, no touch/pen bits, so libinput treats it as a pointer.
        abs_caps = {e.EV_KEY: [e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE],
                    e.EV_ABS: [(e.ABS_X, AbsInfo(0, 0, self.w - 1, 0, 0, 0)), (e.ABS_Y, AbsInfo(0, 0, self.h - 1, 0, 0, 0))]}
        self.ptr = UInput(abs_caps, name='wowbridge-test-pointer')
        time.sleep(1.0)   # let libinput/KWin pick up the new devices

    def wheel(self, notches: int) -> None:
        """Mouse wheel: positive turns it away from you (WoW zooms in), one notch at a time."""
        for _ in range(abs(notches)):
            self.kbd.write(e.EV_REL, e.REL_WHEEL, 1 if notches > 0 else -1)
            self.kbd.syn()
            time.sleep(0.05)

    def move(self, dx: int, dy: int) -> None:
        self.kbd.write(e.EV_REL, e.REL_X, dx)
        self.kbd.write(e.EV_REL, e.REL_Y, dy)
        self.kbd.syn()

    def key(self, code: int | str, hold: float = 0.06, shift: bool = False) -> None:
        if isinstance(code, str):
            code = getattr(e, f'KEY_{ALIASES.get(code.lower(), code).upper()}')
        if shift:
            self.kbd.write(e.EV_KEY, e.KEY_LEFTSHIFT, 1)
        self.kbd.write(e.EV_KEY, code, 1)
        self.kbd.syn()
        time.sleep(hold)
        self.kbd.write(e.EV_KEY, code, 0)
        if shift:
            self.kbd.write(e.EV_KEY, e.KEY_LEFTSHIFT, 0)
        self.kbd.syn()
        time.sleep(0.03)

    def combo(self, keys: str, hold: float = 0.08) -> None:
        """Press a chord such as 'alt+z': modifiers down in order, released in reverse."""
        codes = [getattr(e, f'KEY_{ALIASES.get(k, k).upper()}') for k in keys.lower().split('+')]
        for c in codes:
            self.kbd.write(e.EV_KEY, c, 1)
            self.kbd.syn()
        time.sleep(hold)
        for c in reversed(codes):
            self.kbd.write(e.EV_KEY, c, 0)
            self.kbd.syn()
        time.sleep(0.03)

    def type(self, text: str) -> None:
        for ch in text:
            shift = ch.isupper() or ch in SHIFTED
            base = SHIFTED.get(ch, ch).lower()
            self.key(NAMED.get(base, base), shift=shift)

    def point(self, x: int, y: int) -> None:
        self.ptr.write(e.EV_ABS, e.ABS_X, max(0, min(self.w - 1, x)))
        self.ptr.write(e.EV_ABS, e.ABS_Y, max(0, min(self.h - 1, y)))
        self.ptr.syn()

    def click(self, x: int, y: int, button: int = e.BTN_LEFT, hold: float = 0.08) -> None:
        self.point(x, y)
        time.sleep(0.05)
        self.ptr.write(e.EV_KEY, button, 1)
        self.ptr.syn()
        time.sleep(hold)
        self.ptr.write(e.EV_KEY, button, 0)
        self.ptr.syn()
        time.sleep(0.05)


# Window focus through KWin's scripting interface (no xdotool: its X11 input emulation prompts on KDE Wayland).
FOCUS_JS = '''let found = false;
for (const w of workspace.windowList()) if (w.caption === "CAPTION") {
  found = true;
  if (workspace.activeWindow !== w) {   // re-activating an active game drops its mouse capture
    w.minimized = false;
    if (!w.onAllDesktops && w.desktops.indexOf(workspace.currentDesktop) < 0) w.desktops = [workspace.currentDesktop];
    workspace.raiseWindow(w);
    workspace.activeWindow = w;
  }
}
print("TAG|" + found + "|" + (workspace.activeWindow ? workspace.activeWindow.caption : ""));'''


def kwin(js: str) -> list[str]:
    """Run a KWin script and return what it print()s as `TAG|...` (read back from the user journal)."""
    import subprocess, tempfile
    tag = f'kwin{time.time_ns()}'
    with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False) as f:
        f.write(js.replace('TAG', tag))
    since = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(time.time() - 2))
    q = ['qdbus6', 'org.kde.KWin']
    sid = subprocess.run([*q, '/Scripting', 'org.kde.kwin.Scripting.loadScript', f.name, tag],
                         capture_output=True, text=True).stdout.strip()
    subprocess.run([*q, f'/Scripting/Script{sid}', 'org.kde.kwin.Script.run'], capture_output=True)
    subprocess.run([*q, '/Scripting', 'org.kde.kwin.Scripting.unloadScript', tag], capture_output=True)
    for _ in range(30):
        log = subprocess.run(['journalctl', '--user', '--since', since, '-o', 'cat'], capture_output=True, text=True).stdout
        out = [line.split(tag + '|', 1)[1] for line in log.splitlines() if tag + '|' in line]
        if out:
            return out
        time.sleep(0.1)
    return []


def focus_window(caption: str) -> bool:
    """Raise the window with this exact caption; True when it exists and is then the active window."""
    res = kwin(FOCUS_JS.replace('CAPTION', caption))
    return bool(res) and res[0] == f'true|{caption}'
