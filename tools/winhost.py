"""The native Windows desktop as seen from WSL: where Skyrim keeps its user files, and a client for
tools/winhelper.py (run by Windows Python) that stands in for the Linux evdev/KWin/Spectacle tooling.

UInput and ecodes mimic the slice of python-evdev the runner uses, so its actions stay unchanged; key codes are
evdev's, translated to PC scan codes here.
"""
import functools
import json
import os
import platform
import subprocess
from pathlib import Path

IS_WSL = 'microsoft' in platform.release().lower()
HELPER = Path(__file__).resolve().parent / 'winhelper.py'


def win_path(p: Path | str) -> str:
    return subprocess.run(['wslpath', '-w', str(p)], capture_output=True, text=True, check=True).stdout.strip()


def wsl_path(p: str) -> Path:
    return Path(subprocess.run(['wslpath', '-u', p], capture_output=True, text=True, check=True).stdout.strip())


@functools.cache
def known_folder(name: str) -> Path:
    """'MyDocuments' or 'LocalApplicationData' of the Windows user (Documents may be redirected, e.g. to OneDrive)."""
    out = subprocess.run(['powershell.exe', '-NoProfile', '-Command', f"[Environment]::GetFolderPath('{name}')"],
                         capture_output=True, text=True, check=True, cwd='/mnt/c').stdout.strip()
    return wsl_path(out)


def docs_dir() -> Path:
    """Skyrim's Documents folder: INIs, SKSE logs (and Saves, which the tools never touch)."""
    return Path(os.environ.get('WB_DOCS') or known_folder('MyDocuments') / 'My Games/Skyrim Special Edition')


def plugins_txt() -> Path:
    return Path(os.environ.get('WB_PLUGINS_TXT') or known_folder('LocalApplicationData') / 'Skyrim Special Edition/Plugins.txt')


class Helper:
    def __init__(self):
        self.proc = subprocess.Popen(['py.exe', '-3.12', '-u', win_path(HELPER)], cwd='/mnt/c', text=True,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def __call__(self, op: str, **args) -> dict:
        if self.proc.poll() is not None:
            raise RuntimeError(f'winhelper exited ({self.proc.returncode})')
        self.proc.stdin.write(json.dumps({'op': op, **args}) + '\n')
        self.proc.stdin.flush()
        reply = json.loads(self.proc.stdout.readline())
        if not reply.pop('ok'):
            raise RuntimeError(f'winhelper {op}: {reply["error"]}')
        return reply


@functools.cache
def helper() -> Helper:
    return Helper()


# evdev key names -> codes. Codes 1-88 are PC set-1 scan codes as they are; the rest are E0-extended keys.
_NAMES = ('ESC 1 2 3 4 5 6 7 8 9 0 MINUS EQUAL BACKSPACE TAB Q W E R T Y U I O P LEFTBRACE RIGHTBRACE ENTER LEFTCTRL '
          'A S D F G H J K L SEMICOLON APOSTROPHE GRAVE LEFTSHIFT BACKSLASH Z X C V B N M COMMA DOT SLASH RIGHTSHIFT '
          'KPASTERISK LEFTALT SPACE CAPSLOCK F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 NUMLOCK SCROLLLOCK KP7 KP8 KP9 KPMINUS '
          'KP4 KP5 KP6 KPPLUS KP1 KP2 KP3 KP0 KPDOT').split()
_EXTENDED = {'KPENTER': (96, 0x1C), 'RIGHTCTRL': (97, 0x1D), 'KPSLASH': (98, 0x35), 'SYSRQ': (99, 0x37),
             'RIGHTALT': (100, 0x38), 'HOME': (102, 0x47), 'UP': (103, 0x48), 'PAGEUP': (104, 0x49),
             'LEFT': (105, 0x4B), 'RIGHT': (106, 0x4D), 'END': (107, 0x4F), 'DOWN': (108, 0x50),
             'PAGEDOWN': (109, 0x51), 'INSERT': (110, 0x52), 'DELETE': (111, 0x53), 'LEFTMETA': (125, 0x5B)}
SCAN = {i + 1: (i + 1, False) for i in range(len(_NAMES))} | {86: (86, False), 87: (87, False), 88: (88, False)}
SCAN |= {code: (scan, True) for code, scan in _EXTENDED.values()}


class ecodes:   # noqa: N801 - mirrors evdev.ecodes
    EV_KEY, EV_REL, EV_ABS = 1, 2, 3
    REL_X, REL_Y, REL_WHEEL = 0, 1, 8
    ABS_X, ABS_Y = 0, 1
    BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112


for _i, _n in enumerate(_NAMES):
    setattr(ecodes, f'KEY_{_n}', _i + 1)
for _n, (_code, _) in _EXTENDED.items():
    setattr(ecodes, f'KEY_{_n}', _code)
ecodes.KEY_102ND, ecodes.KEY_F11, ecodes.KEY_F12 = 86, 87, 88
_BUTTONS = {ecodes.BTN_LEFT: 'left', ecodes.BTN_RIGHT: 'right', ecodes.BTN_MIDDLE: 'middle'}


class UInput:
    """Collects evdev-style events and sends them to Windows as one batch on syn()."""

    def __init__(self, *_, **__):
        self.events, self.abs = [], [None, None]

    def write(self, etype: int, code: int, value: int) -> None:
        e = ecodes
        if etype == e.EV_KEY and code in _BUTTONS:
            self.events.append(['button', _BUTTONS[code], bool(value)])
        elif etype == e.EV_KEY:
            scan, extended = SCAN[code]
            self.events.append(['key', scan, extended, bool(value)])
        elif etype == e.EV_REL and code == e.REL_WHEEL:
            self.events.append(['wheel', value])
        elif etype == e.EV_REL:   # consecutive X/Y writes merge into one relative move
            dx, dy = (value, 0) if code == e.REL_X else (0, value)
            if self.events and self.events[-1][0] == 'move':
                self.events[-1][1] += dx
                self.events[-1][2] += dy
            else:
                self.events.append(['move', dx, dy])
        elif etype == e.EV_ABS:
            self.abs[code] = value
            if None not in self.abs:
                self.events.append(['abs', *self.abs])
                self.abs = [None, None]

    def syn(self) -> None:
        if self.events:
            helper()('input', events=self.events)
            self.events = []


def focus_window(caption: str) -> bool:
    """Raise the window with this exact caption; True when it exists and is then the active window."""
    r = helper()('focus', caption=caption)
    return r['found'] and r['active'] == caption


def active_caption() -> str:
    return helper()('active')['active']


def minimize(caption: str) -> bool:
    return helper()('minimize', caption=caption)['found']


def screen_size() -> tuple[int, int]:
    r = helper()('screen')
    return r['width'], r['height']


def screenshot(path: Path, caption: str = '') -> None:
    """PNG of the window's client area (the whole desktop when it is absent)."""
    from PIL import Image
    bmp = path.with_suffix('.bmp')
    helper()('shot', path=win_path(bmp), caption=caption)
    Image.open(bmp).save(path)
    bmp.unlink()


def pids(exe: str) -> list[int]:
    return helper()('pids', name=exe)['pids']


def kill(pid: int, force: bool = False) -> None:
    helper()('kill', pid=pid, force=force)


def launch(exe: Path) -> int:
    return helper()('launch', exe=win_path(exe), cwd=win_path(exe.parent))['pid']
