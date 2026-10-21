#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <oleacc.h>
#include <objbase.h>
#include <ole2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#ifdef DEBUG_LOG
static void Dbg(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 40) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    slash[1] = 0;
    lstrcatW(path, L"DesktopIconToggle-debug.log");
    FILE* f = _wfopen(path, L"a, ccs=UTF-8");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
#else
#define Dbg(...) ((void)0)
#endif

#define APP_VERSION L"1.0.1"
#define APP_TITLE   L"桌面图标开关"
#define IDI_APPICON 1

#define WM_APP_TRAY   (WM_APP + 1)
#define WM_APP_TOGGLE (WM_APP + 2)

#define TIMER_POLL        1
#define TIMER_TRAY_RETRY  2
#define TRAY_ID           1

#define IDM_TOGGLE    1001
#define IDM_AUTOSTART 1002
#define IDM_ABOUT     1003
#define IDM_EXIT      1004

static const wchar_t* kClassName = L"DesktopIconToggleHiddenWindow";
static const wchar_t* kMutexName = L"Local\\DesktopIconToggle_1_0_0";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"DesktopIconToggle";

static HINSTANCE g_inst = NULL;
static HWND g_hwnd = NULL;
static HICON g_icon = NULL;
static UINT g_taskbarCreated = 0;
static NOTIFYICONDATAW g_nid;
static bool g_iconAdded = false;
static int g_trayRetries = 0;

static bool g_mouseDown = false;
static bool g_firstClick = false;
static bool g_secondClick = false;
static bool g_firstBlank = false;
static bool g_secondBlank = false;
static bool g_firstKnown = false;
static bool g_secondKnown = false;
static DWORD g_firstTick = 0;
static POINT g_firstPoint = {0, 0};
static POINT g_secondPoint = {0, 0};

static bool g_hidByUs = false;
static HWND g_hiddenLv = NULL;
static bool g_warned = false;

struct FindChildCtx {
    const wchar_t* className;
    HWND found;
};

static BOOL CALLBACK FindChildProc(HWND hwnd, LPARAM lp) {
    FindChildCtx* ctx = (FindChildCtx*)lp;
    wchar_t cls[64] = L"";
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, ctx->className) == 0) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND FindChildByClass(HWND parent, const wchar_t* className) {
    if (!parent) return NULL;

    HWND direct = FindWindowExW(parent, NULL, className, NULL);
    if (direct) return direct;

    FindChildCtx ctx;
    ctx.className = className;
    ctx.found = NULL;
    EnumChildWindows(parent, FindChildProc, (LPARAM)&ctx);
    return ctx.found;
}

static BOOL CALLBACK FindDefViewProc(HWND hwnd, LPARAM lp) {
    wchar_t cls[64] = L"";
    if (!GetClassNameW(hwnd, cls, 64)) return TRUE;
    if (wcscmp(cls, L"Progman") != 0 && wcscmp(cls, L"WorkerW") != 0) {
        return TRUE;
    }

    HWND def = FindChildByClass(hwnd, L"SHELLDLL_DefView");
    if (!def) return TRUE;

    HWND* result = (HWND*)lp;
    if (IsWindowVisible(def)) {
        *result = def;
        return FALSE;
    }
    if (!*result) *result = def;
    return TRUE;
}

static HWND FindDesktopDefView() {
    HWND result = NULL;
    EnumWindows(FindDefViewProc, (LPARAM)&result);

    if (!result) {
        HWND progman = FindWindowW(L"Progman", NULL);
        if (progman) {
            result = FindChildByClass(progman, L"SHELLDLL_DefView");
        }
    }
    return result;
}

static HWND FindDesktopListView() {
    HWND def = FindDesktopDefView();
    if (!def) return NULL;
    return FindChildByClass(def, L"SysListView32");
}

static bool IsIconsVisible() {
    HWND lv = FindDesktopListView();
    return lv && IsWindowVisible(lv);
}

static bool IsDesktopWindowAt(POINT pt) {
    HWND h = WindowFromPoint(pt);
    if (!h) return false;

    // Folder windows also contain SHELLDLL_DefView / SysListView32.
    // Only the desktop's top-level host is Progman or WorkerW.
    HWND root = GetAncestor(h, GA_ROOT);
    if (!root) return false;

    wchar_t cls[64] = L"";
    if (!GetClassNameW(root, cls, 64)) return false;
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
}

// True only for a blank part of the desktop (not an icon).
// known=false means the shell accessibility data could not be read.
static bool IsBlankDesktopPoint(POINT pt, bool* known) {
    *known = true;

    if (!IsDesktopWindowAt(pt)) return false;

    HWND lv = FindDesktopListView();
    if (!lv) {
        *known = false;
        return false;
    }
    if (!IsWindowVisible(lv)) {
        return true;
    }

    RECT rc;
    if (!GetWindowRect(lv, &rc) || !PtInRect(&rc, pt)) {
        return false;
    }

    IAccessible* acc = NULL;
    VARIANT child;
    VariantInit(&child);
    HRESULT hr = AccessibleObjectFromPoint(pt, &acc, &child);

    bool blank = false;
    if (SUCCEEDED(hr) && acc) {
        if (child.vt == VT_EMPTY) {
            blank = true;
        } else if (child.vt == VT_I4) {
            blank = (child.lVal == CHILDID_SELF);
        } else {
            *known = false;
        }
    } else {
        *known = false;
    }

    VariantClear(&child);
    if (acc) acc->Release();
    return blank;
}

static void WarnBlocked() {
    if (g_warned) return;
    g_warned = true;
    MessageBoxW(NULL,
        L"无法切换桌面图标。\n\n"
        L"可能是安全软件（例如卡巴斯基）阻止了本程序访问桌面。\n"
        L"请将 DesktopIconToggle.exe 添加到信任/排除列表后重试。",
        APP_TITLE,
        MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
}

static bool ToggleDesktopIcons(bool interactive) {
    HWND lv = FindDesktopListView();
    if (!lv) {
        if (interactive) WarnBlocked();
        return false;
    }

    BOOL wasVisible = IsWindowVisible(lv);
    ShowWindow(lv, wasVisible ? SW_HIDE : SW_SHOW);
    BOOL nowVisible = IsWindowVisible(lv);
    Dbg(L"toggle lv=%p was=%d now=%d\n", lv, (int)wasVisible, (int)nowVisible);

    if (nowVisible == wasVisible) {
        if (interactive) WarnBlocked();
        return false;
    }

    if (wasVisible && !nowVisible) {
        g_hidByUs = true;
        g_hiddenLv = lv;
    } else if (!wasVisible && nowVisible) {
        g_hidByUs = false;
        g_hiddenLv = NULL;
    }
    g_warned = false;
    return true;
}

static void RestoreIconsIfNeeded() {
    if (!g_hidByUs || !g_hiddenLv) return;
    if (IsWindow(g_hiddenLv) && !IsWindowVisible(g_hiddenLv)) {
        ShowWindow(g_hiddenLv, SW_SHOW);
    }
    g_hidByUs = false;
    g_hiddenLv = NULL;
}

static void ResetClickState() {
    g_firstClick = false;
    g_secondClick = false;
    g_firstBlank = false;
    g_secondBlank = false;
    g_firstKnown = false;
    g_secondKnown = false;
}

static int HalfDoubleClickX() {
    int v = GetSystemMetrics(SM_CXDOUBLECLK);
    return v > 0 ? (v + 1) / 2 : 4;
}

static int HalfDoubleClickY() {
    int v = GetSystemMetrics(SM_CYDOUBLECLK);
    return v > 0 ? (v + 1) / 2 : 4;
}

static void OnMouseDown(DWORD now) {
    POINT pt;
    if (!GetCursorPos(&pt)) return;

    bool known = false;
    bool blank = IsBlankDesktopPoint(pt, &known);

    DWORD dblTime = GetDoubleClickTime();
    if (dblTime == 0) dblTime = 500;

    bool isSecond = g_firstClick &&
                    (now - g_firstTick) <= dblTime &&
                    abs(pt.x - g_firstPoint.x) <= HalfDoubleClickX() &&
                    abs(pt.y - g_firstPoint.y) <= HalfDoubleClickY();

    Dbg(L"down pt=%ld,%ld blank=%d known=%d second=%d\n",
        (long)pt.x, (long)pt.y, (int)blank, (int)known, (int)isSecond);

    if (isSecond) {
        g_secondClick = true;
        g_secondBlank = blank;
        g_secondKnown = known;
        g_secondPoint = pt;
    } else {
        g_firstClick = true;
        g_secondClick = false;
        g_firstTick = now;
        g_firstPoint = pt;
        g_firstBlank = blank;
        g_firstKnown = known;
        g_secondKnown = false;
    }
}

static void OnMouseUp() {
    if (!g_secondClick) return;

    POINT pt = {0, 0};
    bool moved = true;
    if (GetCursorPos(&pt)) {
        moved = abs(pt.x - g_secondPoint.x) > HalfDoubleClickX() ||
                abs(pt.y - g_secondPoint.y) > HalfDoubleClickY();
    }

    bool known = false;
    bool blank = false;
    if (!moved) {
        blank = IsBlankDesktopPoint(pt, &known);
    }

    Dbg(L"up pt=%ld,%ld moved=%d blank=%d known=%d first=(%d,%d) second=(%d,%d)\n",
        (long)pt.x, (long)pt.y, (int)moved, (int)blank, (int)known,
        (int)g_firstBlank, (int)g_firstKnown, (int)g_secondBlank, (int)g_secondKnown);

    if (!moved && known && g_secondKnown && g_firstKnown &&
        blank && g_secondBlank && g_firstBlank) {
        Dbg(L"toggle requested\n");
        ToggleDesktopIcons(true);
    } else if (!moved && (!g_firstKnown || !g_secondKnown || !known)) {
        WarnBlocked();
    }

    ResetClickState();
}

static void PollMouse() {
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    DWORD now = GetTickCount();

    static int heartbeat = 0;
    if (++heartbeat % 120 == 0) {
        POINT hp = {0, 0};
        GetCursorPos(&hp);
        Dbg(L"heartbeat down=%d cursor=%ld,%ld\n",
            (int)down, (long)hp.x, (long)hp.y);
    }

    if (down && !g_mouseDown) {
        OnMouseDown(now);
    } else if (!down && g_mouseDown) {
        OnMouseUp();
    }
    g_mouseDown = down;

    if (!down && (g_firstClick || g_secondClick)) {
        DWORD dblTime = GetDoubleClickTime();
        if (dblTime == 0) dblTime = 500;
        if ((now - g_firstTick) > dblTime + 500) {
            ResetClickState();
        }
    }
}

static bool BuildAutoStartCommand(wchar_t* out, DWORD cch) {
    if (!out || cch < 4) return false;
    DWORD n = GetModuleFileNameW(NULL, out + 1, cch - 3);
    if (n == 0 || n >= cch - 3) return false;
    out[0] = L'"';
    out[n + 1] = L'"';
    out[n + 2] = 0;
    return true;
}

static bool IsAutoStart() {
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    LONG r = RegQueryValueExW(key, kRunValue, NULL, &type, NULL, &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

static bool SetAutoStart(bool enable) {
    HKEY key = NULL;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, &disp) != ERROR_SUCCESS) {
        return false;
    }

    LONG r = ERROR_SUCCESS;
    if (enable) {
        wchar_t cmd[32768];
        if (!BuildAutoStartCommand(cmd, 32768)) {
            RegCloseKey(key);
            return false;
        }
        DWORD bytes = (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t));
        r = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                           (const BYTE*)cmd, bytes);
    } else {
        r = RegDeleteValueW(key, kRunValue);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }

    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

static bool AddTrayIcon() {
    if (g_iconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_iconAdded = false;
    }

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icon ? g_icon : LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"桌面图标开关\n双击桌面空白处显示/隐藏", 128);

    SetLastError(0);
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &g_nid);
    Dbg(L"tray add ok=%d err=%lu hwnd=%p\n", (int)ok, GetLastError(), g_hwnd);
    if (ok) {
        g_iconAdded = true;
        return true;
    }
    return false;
}

static void RemoveTrayIcon() {
    if (g_iconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_iconAdded = false;
    }
}

static void ShowAbout() {
    MessageBoxW(NULL,
        APP_TITLE L"  v" APP_VERSION L"\n\n"
        L"双击桌面空白处：显示 / 隐藏桌面图标\n"
        L"右键任务栏托盘图标：打开菜单\n\n"
        L"退出时，本程序会自动恢复由它隐藏的图标。\n"
        L"本程序不联网、不收集任何数据。",
        APP_TITLE,
        MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
}

static void HandleCommand(int id) {
    switch (id) {
        case IDM_TOGGLE:
            ToggleDesktopIcons(true);
            break;
        case IDM_AUTOSTART:
            if (!SetAutoStart(!IsAutoStart())) {
                MessageBoxW(NULL, L"无法修改开机自动启动设置。", APP_TITLE,
                            MB_OK | MB_ICONWARNING | MB_TOPMOST);
            }
            break;
        case IDM_ABOUT:
            ShowAbout();
            break;
        case IDM_EXIT:
            DestroyWindow(g_hwnd);
            break;
        default:
            break;
    }
}

static void ShowTrayMenu() {
    POINT pt;
    if (!GetCursorPos(&pt)) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    bool visible = IsIconsVisible();
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE,
                visible ? L"隐藏桌面图标" : L"显示桌面图标");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (IsAutoStart() ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"开机自动启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"使用说明");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    SetForegroundWindow(g_hwnd);
    int cmd = (int)TrackPopupMenu(menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);

    if (cmd != 0) HandleCommand(cmd);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreated != 0 && msg == g_taskbarCreated) {
        if (AddTrayIcon()) {
            KillTimer(hwnd, TIMER_TRAY_RETRY);
        } else {
            g_trayRetries = 0;
            SetTimer(hwnd, TIMER_TRAY_RETRY, 700, NULL);
        }
        return 0;
    }

    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_POLL) {
                static int timerLogs = 0;
                if (timerLogs < 5) {
                    Dbg(L"timer tick %d\n", timerLogs);
                    timerLogs++;
                }
                PollMouse();
            } else if (wParam == TIMER_TRAY_RETRY) {
                if (AddTrayIcon()) {
                    KillTimer(hwnd, TIMER_TRAY_RETRY);
                } else if (++g_trayRetries >= 8) {
                    KillTimer(hwnd, TIMER_TRAY_RETRY);
                    MessageBoxW(NULL,
                        L"无法创建托盘图标。\n\n"
                        L"可能是安全软件阻止了本程序。\n"
                        L"请将 DesktopIconToggle.exe 添加到信任/排除列表后重新运行。",
                        APP_TITLE,
                        MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
                    DestroyWindow(hwnd);
                }
            }
            return 0;

        case WM_APP_TRAY:
            if (lParam == WM_LBUTTONDBLCLK) {
                ToggleDesktopIcons(true);
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayMenu();
            }
            return 0;

        case WM_APP_TOGGLE:
            ToggleDesktopIcons(true);
            return 0;

        case WM_COMMAND:
            HandleCommand(LOWORD(wParam));
            return 0;

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            if (wParam) RestoreIconsIfNeeded();
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_POLL);
            KillTimer(hwnd, TIMER_TRAY_RETRY);
            RemoveTrayIcon();
            RestoreIconsIfNeeded();
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetDpiCtxFn)(HANDLE);
        SetDpiCtxFn fn = (SetDpiCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn && fn((HANDLE)-4)) return;
    }
    SetProcessDPIAware();
}

static void ShowUsage() {
    MessageBoxW(NULL,
        APP_TITLE L"\n\n"
        L"双击桌面空白处：显示 / 隐藏桌面图标\n"
        L"右键托盘图标：菜单（含开机自启、退出）\n\n"
        L"命令行：DesktopIconToggle.exe --toggle\n"
        L"        DesktopIconToggle.exe --help",
        APP_TITLE, MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_inst = hInstance;
    Dbg(L"winmain start\n");
    EnableDpiAwareness();
    Dbg(L"dpi set\n");

    HRESULT oleHr = OleInitialize(NULL);
    Dbg(L"ole init hr=0x%08lX\n", (unsigned long)oleHr);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    bool oneShot = false;
    if (argc > 1 && argv && argv[1]) {
        if (wcscmp(argv[1], L"--help") == 0 || wcscmp(argv[1], L"-h") == 0) {
            ShowUsage();
            LocalFree(argv);
            if (SUCCEEDED(oleHr)) OleUninitialize();
            return 0;
        }
        if (wcscmp(argv[1], L"--version") == 0) {
            MessageBoxW(NULL, APP_TITLE L"  v" APP_VERSION, APP_TITLE,
                        MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
            LocalFree(argv);
            if (SUCCEEDED(oleHr)) OleUninitialize();
            return 0;
        }
        if (wcscmp(argv[1], L"--toggle") == 0) {
            oneShot = true;
        }
    }
    if (argv) LocalFree(argv);

    if (oneShot) {
        HWND existing = FindWindowW(kClassName, NULL);
        if (existing) {
            PostMessageW(existing, WM_APP_TOGGLE, 0, 0);
            if (SUCCEEDED(oleHr)) OleUninitialize();
            return 0;
        }
        bool ok = ToggleDesktopIcons(false);
        if (SUCCEEDED(oleHr)) OleUninitialize();
        return ok ? 0 : 1;
    }

    HANDLE mutex = CreateMutexW(NULL, FALSE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        if (SUCCEEDED(oleHr)) OleUninitialize();
        return 0;
    }

    g_icon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APPICON));
    if (!g_icon) g_icon = LoadIconW(NULL, IDI_APPLICATION);

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = g_icon;
    wc.lpszClassName = kClassName;

    if (!RegisterClassW(&wc)) {
        if (mutex) CloseHandle(mutex);
        if (SUCCEEDED(oleHr)) OleUninitialize();
        return 1;
    }

    Dbg(L"creating window\n");
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, APP_TITLE,
                             WS_POPUP, 0, 0, 0, 0,
                             NULL, NULL, g_inst, NULL);
    Dbg(L"window created hwnd=%p\n", g_hwnd);
    if (!g_hwnd) {
        if (mutex) CloseHandle(mutex);
        if (SUCCEEDED(oleHr)) OleUninitialize();
        return 1;
    }

    Dbg(L"add tray begin\n");
    if (!AddTrayIcon()) {
        g_trayRetries = 0;
        SetTimer(g_hwnd, TIMER_TRAY_RETRY, 700, NULL);
    }
    Dbg(L"add tray end added=%d\n", (int)g_iconAdded);
    SetLastError(0);
    UINT_PTR timerId = SetTimer(g_hwnd, TIMER_POLL, 16, NULL);
    Dbg(L"timer set id=%llu err=%lu\n",
        (unsigned long long)timerId, GetLastError());

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex) CloseHandle(mutex);
    if (SUCCEEDED(oleHr)) OleUninitialize();
    return (int)msg.wParam;
}
