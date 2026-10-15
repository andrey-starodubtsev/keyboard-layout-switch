"""Win32 harness: a focused test window, marked key injection, layout observation."""
import ctypes
import ctypes.wintypes as w
import os
import subprocess
import threading
import time

u = ctypes.windll.user32
k = ctypes.windll.kernel32
u.GetKeyboardLayout.restype = ctypes.c_void_p
u.GetKeyboardLayout.argtypes = [w.DWORD]
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.DefWindowProcW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.DefWindowProcW.restype = ctypes.c_ssize_t
u.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.c_void_p]

MAGIC = 0x4C54  # must match the TESTING hook in LayoutToggle.c
WM_INPUTLANGCHANGE = 0x51
WM_INPUTLANGCHANGEREQUEST = 0x50
HERE = os.path.dirname(os.path.abspath(__file__))
TEST_EXE = os.path.join(HERE, "LayoutToggle_test.exe")

VK = {"LSHIFT": 0xA0, "RSHIFT": 0xA1, "LALT": 0xA4, "RALT": 0xA5,
      "LCTRL": 0xA2, "TAB": 0x09, "A": 0x41, "ESC": 0x1B}
EXTENDED = {"RALT", "RCTRL"}


class KBDI(ctypes.Structure):
    _fields_ = [("wVk", w.WORD), ("wScan", w.WORD), ("dwFlags", w.DWORD),
                ("time", w.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KBDI), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", w.DWORD), ("u", _U)]


def send_key(name, down):
    vk = VK[name]
    flags = 0 if down else 2
    if name in EXTENDED:
        flags |= 1
    inp = INPUT(type=1)
    inp.ki = KBDI(vk, u.MapVirtualKeyW(vk, 0), flags, 0, MAGIC)
    u.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def system_layouts():
    n = u.GetKeyboardLayoutList(0, None)
    arr = (ctypes.c_void_p * n)()
    u.GetKeyboardLayoutList(n, arr)
    return [int(x) for x in arr]


class TestWindow:
    """Top-level window on its own thread; owns the keyboard layout under test."""

    def __init__(self):
        self.hwnd = None
        self.tid = None
        self.changes = []
        self._ready = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        if not self._ready.wait(5):
            raise RuntimeError("window did not start")

    def _run(self):
        WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, w.HWND, w.UINT, w.WPARAM, w.LPARAM)

        def proc(h, m, wp, lp):
            if m == WM_INPUTLANGCHANGE:
                self.changes.append(lp)
            return u.DefWindowProcW(h, m, wp, lp)

        self._proc = WNDPROC(proc)

        class WNDCLASS(ctypes.Structure):
            _fields_ = [("style", w.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                        ("cbWndExtra", ctypes.c_int), ("hInstance", w.HINSTANCE), ("hIcon", w.HANDLE),
                        ("hCursor", w.HANDLE), ("hbrBackground", w.HANDLE),
                        ("lpszMenuName", w.LPCWSTR), ("lpszClassName", w.LPCWSTR)]

        u.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASS)]
        u.CreateWindowExW.restype = w.HWND
        u.CreateWindowExW.argtypes = [w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD, ctypes.c_int, ctypes.c_int,
                                      ctypes.c_int, ctypes.c_int, w.HWND, w.HANDLE, w.HINSTANCE, w.LPVOID]
        wc = WNDCLASS()
        wc.lpfnWndProc = self._proc
        wc.lpszClassName = "LayoutTestWnd"
        wc.hInstance = k.GetModuleHandleW(None)
        u.RegisterClassW(ctypes.byref(wc))
        self.hwnd = u.CreateWindowExW(0, wc.lpszClassName, "layout test", 0x10CF0000,
                                      100, 100, 400, 200, None, None, wc.hInstance, None)
        self.tid = u.GetWindowThreadProcessId(self.hwnd, None)
        self._ready.set()
        msg = w.MSG()
        while u.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            u.TranslateMessage(ctypes.byref(msg))
            u.DispatchMessageW(ctypes.byref(msg))

    def focus(self):
        # An injected Alt tap lets SetForegroundWindow through the foreground lock.
        for _ in range(10):
            send_key("LALT", True)
            send_key("LALT", False)
            u.SetForegroundWindow(self.hwnd)
            time.sleep(0.1)
            if u.GetForegroundWindow() == self.hwnd:
                return
        raise RuntimeError("could not focus the test window")

    def layout(self):
        return int(u.GetKeyboardLayout(self.tid) or 0)

    def set_layout(self, hkl, timeout=2.0):
        u.PostMessageW(self.hwnd, WM_INPUTLANGCHANGEREQUEST, 0, hkl)
        end = time.time() + timeout
        while time.time() < end:
            if self.layout() == hkl:
                return
            time.sleep(0.02)
        raise RuntimeError("layout did not change to %x" % hkl)

    def close(self):
        u.PostMessageW(self.hwnd, 0x0010, 0, 0)


class Override:
    """Runs the test build of the app for the duration of a with-block."""

    def __enter__(self):
        self.p = subprocess.Popen([TEST_EXE])
        time.sleep(0.8)
        return self

    def __exit__(self, *a):
        self.p.kill()
        self.p.wait()
        time.sleep(0.3)


def release_all():
    for n in ("LSHIFT", "RSHIFT", "LALT", "RALT", "LCTRL"):
        send_key(n, False)


def run_scenario(win, mode, history, steps):  # mode is informational
    """Returns the final layout index in the system list.

    history: layout indexes visited in order before the scenario; the last one is
    the current layout, the one before it is the most recently used.
    steps: ("down"|"up", key) or ("wait", seconds).
    """
    sysl = system_layouts()

    def go():
        win.focus()
        for i in history:
            win.set_layout(sysl[i])
            time.sleep(0.25)   # let the app observe each layout (poll is 100 ms)
        time.sleep(0.3)
        for s in steps:
            if s[0] == "wait":
                time.sleep(s[1])
            else:
                send_key(s[1], s[0] == "down")
                time.sleep(0.05)
        release_all()
        time.sleep(1.2)        # flyout commit and settle
        return sysl.index(win.layout())

    try:
        return go()
    finally:
        release_all()
