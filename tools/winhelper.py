"""Windows side of the test runner's desktop access, run by Windows Python (no packages) and driven by
tools/winhost.py from WSL: one JSON request per stdin line, one JSON reply per stdout line.

Input goes through SendInput with scan codes, so DirectInput games see it like hardware; it lands in whatever window
has the focus, which is why winhost checks the focus before every action.
"""
import ctypes
import json
import subprocess
import sys
from ctypes import wintypes as w

user32 = ctypes.WinDLL('user32', use_last_error=True)
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
gdi32 = ctypes.WinDLL('gdi32')
ctypes.windll.shcore.SetProcessDpiAwareness(2)   # per-monitor aware: coordinates and captures in real pixels

for name, res, args in (
        ('GetForegroundWindow', w.HWND, ()), ('GetWindowTextW', ctypes.c_int, (w.HWND, w.LPWSTR, ctypes.c_int)),
        ('GetWindowTextLengthW', ctypes.c_int, (w.HWND,)), ('IsWindowVisible', w.BOOL, (w.HWND,)),
        ('IsIconic', w.BOOL, (w.HWND,)), ('ShowWindow', w.BOOL, (w.HWND, ctypes.c_int)),
        ('SetForegroundWindow', w.BOOL, (w.HWND,)), ('BringWindowToTop', w.BOOL, (w.HWND,)),
        ('GetWindowThreadProcessId', w.DWORD, (w.HWND, ctypes.POINTER(w.DWORD))),
        ('AttachThreadInput', w.BOOL, (w.DWORD, w.DWORD, w.BOOL)), ('GetClientRect', w.BOOL, (w.HWND, ctypes.POINTER(w.RECT))),
        ('ClientToScreen', w.BOOL, (w.HWND, ctypes.POINTER(w.POINT))), ('SetCursorPos', w.BOOL, (ctypes.c_int, ctypes.c_int)),
        ('GetSystemMetrics', ctypes.c_int, (ctypes.c_int,)), ('GetDC', w.HDC, (w.HWND,)),
        ('ReleaseDC', ctypes.c_int, (w.HWND, w.HDC))):
    fn = getattr(user32, name)
    fn.restype, fn.argtypes = res, args
WNDENUMPROC = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
user32.EnumWindows.argtypes = (WNDENUMPROC, w.LPARAM)
gdi32.CreateCompatibleDC.restype = w.HDC
gdi32.CreateCompatibleBitmap.restype = w.HBITMAP
gdi32.CreateCompatibleDC.argtypes = (w.HDC,)
gdi32.CreateCompatibleBitmap.argtypes = (w.HDC, ctypes.c_int, ctypes.c_int)
gdi32.SelectObject.argtypes = (w.HDC, w.HGDIOBJ)
gdi32.SelectObject.restype = w.HGDIOBJ
gdi32.BitBlt.argtypes = (w.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.HDC, ctypes.c_int, ctypes.c_int, w.DWORD)
gdi32.GetDIBits.argtypes = (w.HDC, w.HBITMAP, w.UINT, w.UINT, ctypes.c_void_p, ctypes.c_void_p, w.UINT)
gdi32.DeleteObject.argtypes = (w.HGDIOBJ,)
gdi32.DeleteDC.argtypes = (w.HDC,)
kernel32.GetCurrentThreadId.restype = w.DWORD


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [('dx', w.LONG), ('dy', w.LONG), ('mouseData', w.DWORD), ('dwFlags', w.DWORD), ('time', w.DWORD),
                ('dwExtraInfo', ctypes.c_size_t)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [('wVk', w.WORD), ('wScan', w.WORD), ('dwFlags', w.DWORD), ('time', w.DWORD),
                ('dwExtraInfo', ctypes.c_size_t)]


class _INPUTUNION(ctypes.Union):
    _fields_ = [('mi', MOUSEINPUT), ('ki', KEYBDINPUT), ('pad', ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [('type', w.DWORD), ('u', _INPUTUNION)]


user32.SendInput.argtypes = (w.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
BUTTONS = {'left': (0x2, 0x4), 'right': (0x8, 0x10), 'middle': (0x20, 0x40)}


def send(inputs: list) -> None:
    arr = (INPUT * len(inputs))(*inputs)
    if user32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT)) != len(inputs):
        raise OSError(f'SendInput blocked (error {ctypes.get_last_error()})')


def mouse(flags: int, dx: int = 0, dy: int = 0, data: int = 0) -> INPUT:
    i = INPUT(type=0)
    i.u.mi = MOUSEINPUT(dx, dy, data & 0xFFFFFFFF, flags, 0, 0)
    return i


def key(scan: int, extended: bool, down: bool) -> INPUT:
    i = INPUT(type=1)
    i.u.ki = KEYBDINPUT(0, scan, 0x8 | (0x1 if extended else 0) | (0 if down else 0x2), 0, 0)
    return i


def op_input(events: list) -> dict:
    """events: ["key", scan, extended, down] | ["button", name, down] | ["move", dx, dy] | ["wheel", notches] |
    ["abs", x, y], sent in order as one batch."""
    batch = []
    for ev in events:
        kind = ev[0]
        if kind == 'key':
            batch.append(key(ev[1], ev[2], ev[3]))
        elif kind == 'button':
            batch.append(mouse(BUTTONS[ev[1]][0 if ev[2] else 1]))
        elif kind == 'move':
            batch.append(mouse(0x1, ev[1], ev[2]))
        elif kind == 'wheel':
            batch.append(mouse(0x800, data=120 * ev[1]))
        elif kind == 'abs':
            if batch:
                send(batch)
                batch = []
            user32.SetCursorPos(ev[1], ev[2])
    if batch:
        send(batch)
    return {}


def title(hwnd) -> str:
    n = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(hwnd, buf, n + 1)
    return buf.value


def find(caption: str):
    hits = []

    def cb(hwnd, _):
        if user32.IsWindowVisible(hwnd) and title(hwnd) == caption:
            hits.append(hwnd)
        return True
    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return hits[0] if hits else None


def op_focus(caption: str) -> dict:
    """Raise the window with this exact caption unless it already has the focus (re-activating an active game drops
    its mouse capture). Windows refuses SetForegroundWindow to background processes, so borrow the foreground
    thread's input state for the call."""
    hwnd = find(caption)
    if hwnd and user32.GetForegroundWindow() != hwnd:
        if user32.IsIconic(hwnd):
            user32.ShowWindow(hwnd, 9)   # SW_RESTORE
        fg = user32.GetForegroundWindow()
        fg_thread = user32.GetWindowThreadProcessId(fg, None) if fg else 0
        me = kernel32.GetCurrentThreadId()
        attached = bool(fg_thread) and fg_thread != me and user32.AttachThreadInput(me, fg_thread, True)
        try:
            user32.BringWindowToTop(hwnd)
            user32.SetForegroundWindow(hwnd)
        finally:
            if attached:
                user32.AttachThreadInput(me, fg_thread, False)
    fg = user32.GetForegroundWindow()
    return {'found': bool(hwnd), 'active': title(fg) if fg else ''}


def op_active() -> dict:
    fg = user32.GetForegroundWindow()
    return {'active': title(fg) if fg else ''}


def op_minimize(caption: str) -> dict:
    hwnd = find(caption)
    if hwnd:
        user32.ShowWindow(hwnd, 6)   # SW_MINIMIZE
    return {'found': bool(hwnd)}


def client_rect(hwnd) -> tuple[int, int, int, int]:
    r, p = w.RECT(), w.POINT(0, 0)
    user32.GetClientRect(hwnd, ctypes.byref(r))
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    return p.x, p.y, r.right - r.left, r.bottom - r.top


def op_screen() -> dict:
    return {'width': user32.GetSystemMetrics(0), 'height': user32.GetSystemMetrics(1)}


def op_shot(path: str, caption: str = '') -> dict:
    """Capture the window's client area (or the whole virtual desktop) from the screen into a 24-bit BMP."""
    hwnd = find(caption) if caption else None
    if hwnd:
        x, y, cw, ch = client_rect(hwnd)
    else:
        x, y, cw, ch = (user32.GetSystemMetrics(i) for i in (76, 77, 78, 79))   # SM_*VIRTUALSCREEN
    screen = user32.GetDC(None)
    mem = gdi32.CreateCompatibleDC(screen)
    bmp = gdi32.CreateCompatibleBitmap(screen, cw, ch)
    old = gdi32.SelectObject(mem, bmp)
    try:
        gdi32.BitBlt(mem, 0, 0, cw, ch, screen, x, y, 0x00CC0020 | 0x40000000)   # SRCCOPY | CAPTUREBLT
        stride = (cw * 3 + 3) & ~3
        header = (ctypes.c_byte * 40)()
        ctypes.memmove(header, (40).to_bytes(4, 'little') + cw.to_bytes(4, 'little', signed=True)
                       + ch.to_bytes(4, 'little', signed=True) + (1).to_bytes(2, 'little') + (24).to_bytes(2, 'little')
                       + bytes(24), 40)
        pixels = ctypes.create_string_buffer(stride * ch)
        gdi32.GetDIBits(mem, bmp, 0, ch, pixels, header, 0)   # bottom-up rows, as BMP stores them
    finally:
        gdi32.SelectObject(mem, old)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mem)
        user32.ReleaseDC(None, screen)
    size = 14 + 40 + stride * ch
    with open(path, 'wb') as f:
        f.write(b'BM' + size.to_bytes(4, 'little') + bytes(4) + (54).to_bytes(4, 'little'))
        f.write(bytes(header) + pixels.raw)
    return {'window': bool(hwnd), 'rect': [x, y, cw, ch]}


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', w.DWORD), ('cntUsage', w.DWORD), ('th32ProcessID', w.DWORD), ('th32DefaultHeapID', ctypes.c_size_t),
                ('th32ModuleID', w.DWORD), ('cntThreads', w.DWORD), ('th32ParentProcessID', w.DWORD),
                ('pcPriClassBase', w.LONG), ('dwFlags', w.DWORD), ('szExeFile', w.WCHAR * 260)]


kernel32.CreateToolhelp32Snapshot.restype = w.HANDLE
kernel32.Process32FirstW.argtypes = kernel32.Process32NextW.argtypes = (w.HANDLE, ctypes.POINTER(PROCESSENTRY32W))
kernel32.CloseHandle.argtypes = (w.HANDLE,)


def op_pids(name: str) -> dict:
    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)   # TH32CS_SNAPPROCESS
    entry, found = PROCESSENTRY32W(), []
    entry.dwSize = ctypes.sizeof(entry)
    try:
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            if entry.szExeFile.lower() == name.lower():
                found.append(entry.th32ProcessID)
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return {'pids': found}


def op_kill(pid: int, force: bool = False) -> dict:
    """One exact PID: a close request first, termination only when asked."""
    r = subprocess.run(['taskkill', '/PID', str(int(pid)), *(['/F'] if force else [])], capture_output=True, text=True)
    return {'code': r.returncode, 'out': (r.stdout + r.stderr).strip()}


def op_launch(exe: str, cwd: str) -> dict:
    p = subprocess.Popen([exe], cwd=cwd, creationflags=0x00000008 | 0x00000200,   # DETACHED | NEW_PROCESS_GROUP
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return {'pid': p.pid}


OPS = {'input': op_input, 'focus': op_focus, 'active': op_active, 'minimize': op_minimize, 'screen': op_screen,
       'shot': op_shot, 'pids': op_pids, 'kill': op_kill, 'launch': op_launch}


def main() -> None:
    for line in sys.stdin:
        try:
            req = json.loads(line)
            reply = {'ok': True, **OPS[req.pop('op')](**req)}
        except Exception as exc:   # reported to the caller, which decides; the helper keeps serving
            reply = {'ok': False, 'error': f'{type(exc).__name__}: {exc}'}
        sys.stdout.write(json.dumps(reply) + '\n')
        sys.stdout.flush()


if __name__ == '__main__':
    main()
