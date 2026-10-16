// Alt+Shift switches between keyboard layouts in most-recently-used order:
// first press -> previous layout; pressing Shift again while Alt is held walks
// the system layout list in the direction native Windows uses (Left Shift
// down, Right Shift up). The second press skips the layout the chord started
// from so that it reaches the third layout; after that all layouts are cycled. Holding Alt for a moment opens the
// standard Windows layout flyout: the app "types" Win+Space on the user's behalf and steers the flyout's highlight to
// the layout that Alt+Shift selected. Nothing is persisted: the low-level hook is the only "modification" and it
// disappears when the process exits, restoring the default Windows behavior.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#define WM_TRAY (WM_APP + 1)
#define IDI_APP 1  // must match LayoutToggle.rc
#define ID_EXIT 1
#define TIMER_POLL 1
#define TIMER_FLYOUT 2
#define TIMER_READY 3
#define FLYOUT_READY_MS 350
#define FLYOUT_DELAY_MS 400
#define SETTLE_MS 800
#define MAX_LAYOUTS 32

static HHOOK g_hook;
static HWND g_wnd;
static HWND g_fg;                // window that gets the layout change
static HKL g_list[MAX_LAYOUTS];  // MRU order, g_list[0] is the current layout
static int g_n;
static BOOL g_session;       // Alt+Shift chord in progress
static HKL g_orig, g_sel;    // layout the chord started from / selected now
static int g_presses;        // Shift presses in the current chord
static BOOL g_flyout;        // Win is held down by us, flyout is open
static BOOL g_ready;         // the flyout accepts Space taps
static BOOL g_closePending;  // Alt was released before the flyout was ready
static int g_pos;            // flyout highlight, index in system order
static DWORD g_settleUntil;  // ignore tracking while a switch is applied
static BOOL g_altDown, g_shiftDown, g_shiftBack, g_cancel;
static BOOL g_swallowShiftUp, g_swallowAltUp;

static HKL ForegroundLayout(HWND* outWnd)
{
    HWND fg = GetForegroundWindow();
    if (outWnd) {
        *outWnd = fg;
    }
    if (!fg) {
        return NULL;
    }
    return GetKeyboardLayout(GetWindowThreadProcessId(fg, NULL));
}

static int IndexOf(HKL l)
{
    for (int i = 0; i < g_n; i++) {
        if (g_list[i] == l) {
            return i;
        }
    }
    return -1;
}

static void MoveToFront(int i)
{
    HKL l = g_list[i];
    for (; i > 0; i--) {
        g_list[i] = g_list[i - 1];
    }
    g_list[0] = l;
}

// Drops removed layouts, appends newly installed ones, keeps MRU order.
static void RefreshList(void)
{
    HKL sys[MAX_LAYOUTS];
    int ns = GetKeyboardLayoutList(MAX_LAYOUTS, sys);
    int n = 0;
    for (int i = 0; i < g_n; i++) {
        for (int j = 0; j < ns; j++) {
            if (sys[j] == g_list[i]) {
                g_list[n++] = g_list[i];
                break;
            }
        }
    }
    g_n = n;
    for (int j = 0; j < ns; j++) {
        if (IndexOf(sys[j]) < 0) {
            g_list[g_n++] = sys[j];
        }
    }
}

static void Track(void)
{
    if (g_session || (int)(GetTickCount() - g_settleUntil) < 0) {
        return;
    }
    RefreshList();
    HKL l = ForegroundLayout(NULL);
    if (!l) {
        return;
    }
    int i = IndexOf(l);
    if (i < 0 && g_n < MAX_LAYOUTS) {
        g_list[g_n] = l;
        i = g_n++;
    }
    if (i > 0) {
        MoveToFront(i);
    }
}

static void SendKeys(const WORD* vks, const BOOL* ups, int count)
{
    INPUT in[64] = {0};
    if (count > 64) {
        count = 64;
    }
    for (int i = 0; i < count; i++) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = vks[i];
        in[i].ki.dwFlags = ups[i] ? KEYEVENTF_KEYUP : 0;
        if (vks[i] == VK_LWIN) {
            in[i].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
    }
    SendInput(count, in, sizeof(INPUT));
}

static void Tap(WORD vk, int times)
{
    WORD vks[64];
    BOOL ups[64];
    int n = 0;
    for (int i = 0; i < times && n + 2 <= 64; i++) {
        vks[n] = vk;
        ups[n++] = FALSE;
        vks[n] = vk;
        ups[n++] = TRUE;
    }
    SendKeys(vks, ups, n);
}

static BOOL InList(const HKL* a, int n, HKL l)
{
    for (int i = 0; i < n; i++) {
        if (a[i] == l) {
            return TRUE;
        }
    }
    return FALSE;
}

// Layouts in the order Windows cycles them (the language list order shown in
// Settings and in the flyout), which differs from GetKeyboardLayoutList order.
static int SystemOrder(HKL* out)
{
    HKL sys[MAX_LAYOUTS];
    int ns = GetKeyboardLayoutList(MAX_LAYOUTS, sys), n = 0;
    static const TCHAR profile[] = TEXT("Control Panel\\International\\User Profile");
    HKEY key;
    TCHAR* langs = NULL;
    DWORD size = 0, type = 0;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, profile, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if (RegQueryValueEx(key, TEXT("Languages"), NULL, &type, NULL, &size) == ERROR_SUCCESS &&
            type == REG_MULTI_SZ) {
            // Two extra zeroed characters guarantee the list is terminated.
            langs = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2 * sizeof(TCHAR));
            if (langs && RegQueryValueEx(key, TEXT("Languages"), NULL, NULL, (BYTE*)langs, &size) != ERROR_SUCCESS) {
                HeapFree(GetProcessHeap(), 0, langs);
                langs = NULL;
            }
        }
        RegCloseKey(key);
    }
    if (langs) {
        for (const TCHAR* lang = langs; *lang; lang += lstrlen(lang) + 1) {
            TCHAR name[64], tag[64];
            HKEY lk;
            lstrcpyn(tag, lang, 64);
            SIZE_T subLen = lstrlen(profile) + 1 + lstrlen(lang) + 1;
            TCHAR* sub = HeapAlloc(GetProcessHeap(), 0, subLen * sizeof(TCHAR));
            if (!sub) {
                break;
            }
            wsprintf(sub, TEXT("%s\\%s"), profile, lang);
            int before = n;
            LONG rc = RegOpenKeyEx(HKEY_CURRENT_USER, sub, 0, KEY_READ, &lk);
            HeapFree(GetProcessHeap(), 0, sub);
            if (rc == ERROR_SUCCESS) {
                for (DWORD i = 0, len = 64; RegEnumValue(lk, i, name, &len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
                     i++, len = 64) {
                    if (len == 13 && name[4] == TEXT(':')) {
                        HKL h = LoadKeyboardLayout(name + 5, 0);
                        if (h && InList(sys, ns, h) && !InList(out, n, h) && n < MAX_LAYOUTS) {
                            out[n++] = h;
                        }
                    }
                }
                RegCloseKey(lk);
            }
            if (n == before) {
                // No explicit input method: the language's default layout.
                for (TCHAR* c = tag; *c; c++) {
                    if (*c == TEXT('-')) {
                        *c = 0;
                        break;
                    }
                }
                LCID lc = LocaleNameToLCID(tag, 0);
                for (int j = 0; lc && j < ns; j++) {
                    if (PRIMARYLANGID(LOWORD((UINT_PTR)sys[j])) == PRIMARYLANGID(lc) && !InList(out, n, sys[j]) &&
                        n < MAX_LAYOUTS) {
                        out[n++] = sys[j];
                        break;
                    }
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, langs);
    }
    for (int j = 0; j < ns; j++) {
        if (!InList(out, n, sys[j]) && n < MAX_LAYOUTS) {
            out[n++] = sys[j];
        }
    }
    return n;
}

static int SystemIndex(HKL l, int* total)
{
    HKL sys[MAX_LAYOUTS];
    int ns = SystemOrder(sys);
    *total = ns;
    for (int i = 0; i < ns; i++) {
        if (sys[i] == l) {
            return i;
        }
    }
    return -1;
}

// Steps through the system layout list (the order the flyout shows) in the
// direction of the pressed Shift, so the highlight always moves the same way
// as with the default Windows hotkey. The layout the chord started from is
// skipped when asked, so that a second press reaches the third layout.
static HKL StepSystemOrder(HKL from, BOOL backward, HKL skip)
{
    HKL sys[MAX_LAYOUTS];
    int ns = SystemOrder(sys), i = 0;
    for (; i < ns; i++) {
        if (sys[i] == from) {
            break;
        }
    }
    for (int k = 0; k < ns; k++) {
        i = (i + (backward ? ns - 1 : 1)) % ns;
        if (sys[i] != skip || ns < 3) {
            return sys[i];
        }
    }
    return from;
}

// Moves the Win+Space highlight to the layout selected by the chord. The
// flyout ignores Space for a short while after it opens, so wait for that.
static void SteerFlyout(void)
{
    int n, target = SystemIndex(g_sel, &n);
    if (!g_ready || target < 0 || n < 2) {
        return;
    }
    int steps = (target - g_pos + n) % n;
    if (steps) {
        Tap(VK_SPACE, steps);
    }
    g_pos = target;
}

static void OpenFlyout(void)
{
    int n, cur;
    if (!g_session || !g_altDown || g_flyout) {
        return;
    }
    cur = SystemIndex(g_sel, &n);
    if (cur < 0 || n < 2) {
        return;
    }
    // The system must not see Alt as held, or Win+Space won't match.
    WORD vks[3] = {VK_LMENU, VK_RMENU, VK_LWIN};
    BOOL ups[3] = {TRUE, TRUE, FALSE};
    SendKeys(vks, ups, 3);
    g_flyout = TRUE;
    g_ready = g_closePending = FALSE;
    g_swallowAltUp = TRUE;  // also swallows Alt auto-repeat
    Tap(VK_SPACE, 1);       // opens the flyout, highlighting the next layout
    g_pos = (cur + 1) % n;
    SetTimer(g_wnd, TIMER_READY, FLYOUT_READY_MS, NULL);
}

// Releasing Win commits the highlighted layout. If the flyout isn't accepting
// input yet, the release is postponed until the highlight has been steered.
static void CloseFlyout(BOOL force)
{
    if (!g_flyout) {
        return;
    }
    if (!g_ready && !force) {
        g_closePending = TRUE;
        return;
    }
    KillTimer(g_wnd, TIMER_READY);
    g_flyout = g_ready = g_closePending = FALSE;
    WORD vk = VK_LWIN;
    BOOL up = TRUE;
    SendKeys(&vk, &up, 1);
}

static void FlyoutReady(void)
{
    KillTimer(g_wnd, TIMER_READY);
    g_ready = TRUE;
    SteerFlyout();
    if (g_closePending) {
        CloseFlyout(FALSE);
    }
}

static void EndSession(void)
{
    if (!g_session) {
        return;
    }
    g_session = FALSE;
    KillTimer(g_wnd, TIMER_FLYOUT);
    CloseFlyout(FALSE);
    int i = IndexOf(g_sel);
    if (i > 0) {
        MoveToFront(i);
    }
    g_settleUntil = GetTickCount() + SETTLE_MS;
}

static void Press(BOOL backward)
{
    // Dummy key so the lone Alt release doesn't activate the menu bar.
    WORD vk = 0xFF;
    BOOL ups[2] = {FALSE, TRUE};
    WORD vks[2] = {vk, vk};
    SendKeys(vks, ups, 2);

    if (!g_session) {
        g_settleUntil = 0;
        Track();
        if (g_n < 2) {
            return;
        }
        g_session = TRUE;
        g_closePending = FALSE;
        g_presses = 0;
        g_orig = g_sel = g_list[0];
        ForegroundLayout(&g_fg);
        SetTimer(g_wnd, TIMER_FLYOUT, FLYOUT_DELAY_MS, NULL);
        g_sel = g_list[1];  // first press: the previously used layout
    } else {
        g_sel = StepSystemOrder(g_sel, backward, g_presses == 1 ? g_orig : NULL);
    }
    g_presses++;
    if (g_flyout) {
        SteerFlyout();
    } else if (g_fg) {
        PostMessage(g_fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)g_sel);
    }
}

static LRESULT CALLBACK KbHook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
#ifdef TESTING
        // The test harness marks its synthetic keys so they are treated as physical.
        BOOL ours = !(k->flags & LLKHF_INJECTED) || k->dwExtraInfo == 0x4C54;
#else
        BOOL ours = !(k->flags & LLKHF_INJECTED);
#endif
        if (ours) {
            BOOL down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
            BOOL isAlt = (k->vkCode == VK_LMENU);  // native hotkey ignores Right Alt
            BOOL isShift = (k->vkCode == VK_LSHIFT || k->vkCode == VK_RSHIFT);

            if (isShift) {
                if (down) {
                    BOOL repeat = g_shiftDown;
                    g_shiftDown = TRUE;
                    g_shiftBack = (k->vkCode == VK_RSHIFT);
                    if (repeat && g_swallowShiftUp) {
                        return 1;
                    }
                    if (g_altDown && !g_cancel) {
                        Press(g_shiftBack);
                        g_swallowShiftUp = TRUE;
                        return 1;
                    }
                } else {
                    g_shiftDown = FALSE;
                    if (g_swallowShiftUp) {
                        g_swallowShiftUp = FALSE;
                        return 1;
                    }
                }
            } else if (isAlt) {
                if (down) {
                    BOOL repeat = g_altDown;
                    g_altDown = TRUE;
                    if (repeat) {
                        if (g_swallowAltUp) {
                            return 1;
                        }
                    } else {
                        g_cancel = FALSE;
                        if (g_shiftDown && !g_swallowShiftUp) {
                            // Shift was pressed first, then Alt.
                            Press(g_shiftBack);
                            g_swallowAltUp = TRUE;
                            return 1;
                        }
                    }
                } else {
                    g_altDown = FALSE;
                    EndSession();
                    if (g_swallowAltUp) {
                        g_swallowAltUp = FALSE;
                        return 1;
                    }
                }
            } else if (down && g_altDown) {
                g_cancel = TRUE;  // e.g. Alt+Shift+Tab is not ours
            }
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static void AddTrayIcon(HWND wnd)
{
    NOTIFYICONDATA nid = {0};
    nid.cbSize = sizeof(nid);
    nid.hWnd = wnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP));
    if (!nid.hIcon) {
        nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    }
    lstrcpy(nid.szTip, TEXT("Layout Toggle (Alt+Shift)"));
    Shell_NotifyIcon(NIM_ADD, &nid);
}

static void RemoveTrayIcon(HWND wnd)
{
    NOTIFYICONDATA nid = {0};
    nid.cbSize = sizeof(nid);
    nid.hWnd = wnd;
    nid.uID = 1;
    Shell_NotifyIcon(NIM_DELETE, &nid);
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static UINT taskbarCreated;
    switch (msg) {
        case WM_CREATE:
            taskbarCreated = RegisterWindowMessage(TEXT("TaskbarCreated"));
            AddTrayIcon(wnd);
            SetTimer(wnd, TIMER_POLL, 100, NULL);
            return 0;
        case WM_TIMER:
            if (wp == TIMER_POLL) {
                Track();
            } else if (wp == TIMER_READY) {
                FlyoutReady();
            } else if (wp == TIMER_FLYOUT) {
                KillTimer(wnd, TIMER_FLYOUT);
                OpenFlyout();
            }
            return 0;
        case WM_TRAY:
            if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU m = CreatePopupMenu();
                AppendMenu(m, MF_STRING, ID_EXIT, TEXT("Exit"));
                SetForegroundWindow(wnd);
                if (TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd, NULL) == ID_EXIT) {
                    DestroyWindow(wnd);
                }
                PostMessage(wnd, WM_NULL, 0, 0);
                DestroyMenu(m);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(wnd, TIMER_POLL);
            CloseFlyout(TRUE);  // never leave Win stuck down
            RemoveTrayIcon(wnd);
            PostQuitMessage(0);
            return 0;
        default:
            if (msg == taskbarCreated && msg) {
                AddTrayIcon(wnd);
                return 0;
            }
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prevInst, LPSTR cmd, int show)
{
    (void)prevInst;
    (void)cmd;
    (void)show;
    HANDLE mutex = CreateMutex(NULL, TRUE, TEXT("Local\\LayoutToggle_SingleInstance"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = TEXT("LayoutToggleWnd");
    RegisterClass(&wc);

    g_wnd = CreateWindowEx(0, wc.lpszClassName, TEXT("LayoutToggle"), 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
    if (!g_wnd) {
        return 1;
    }

    Track();
    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, KbHook, inst, 0);
    if (!g_hook) {
        DestroyWindow(g_wnd);
        return 1;
    }

    MSG m;
    while (GetMessage(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    UnhookWindowsHookEx(g_hook);
    CloseHandle(mutex);
    return 0;
}

// No CRT: the process entry point gets no arguments and must exit explicitly.
__attribute__((force_align_arg_pointer)) void WinMainCRTStartup(void)
{
    ExitProcess((UINT)WinMain(GetModuleHandle(NULL), NULL, NULL, SW_SHOWDEFAULT));
}
