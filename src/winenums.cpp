#include "winenums.h"

#include <dwmapi.h>
#include <objbase.h>
#include <shobjidl_core.h>

#include <cwchar>

namespace edgepop {
namespace {

// {AA509086-5CA9-4C25-8F95-589D3C07B7A9} - CLSID_VirtualDesktopManager
// (documented GUID; some SDK header combinations don't declare the extern).
const CLSID kCLSID_VirtualDesktopManager = {
    0xaa509086, 0x5ca9, 0x4c25,
    { 0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb7, 0xa9 } };

bool IsShellChromeClass(const wchar_t* cls) {
    // Real names are Shell_TrayWnd / Shell_SecondaryTrayWnd - the earlier
    // "…Window" spellings never matched, so the taskbar was only excluded
    // incidentally by the no-title rule.
    return wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           wcscmp(cls, L"Progman") == 0 ||
           wcscmp(cls, L"WorkerW") == 0 ||
           wcsncmp(cls, L"EdgePop", 7) == 0;  // our own windows
}

// DWMWA_CLOAKED: APP (1) = the app hid it (suspended UWP...); SHELL (2) =
// the shell hid it - which is how windows on OTHER virtual desktops look.
// App-cloaked is never listed; shell-cloaked is only a current-desktop
// filter (rejecting both made "all desktops" behave like
// "current", since every other-desktop window is shell-cloaked).
int CloakState(HWND h) {
    int cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked,
                                     sizeof(cloaked)))) {
        return 0;
    }
    return cloaked;
}

// IVirtualDesktopManager is the documented surface (IsWindowOnCurrent...)
// - we never enumerate desktops. Created lazily, cached for the process.
IVirtualDesktopManager* g_vdManager = nullptr;
bool g_vdTried = false;

bool OnCurrentVirtualDesktop(HWND h) {
    if (!g_vdTried) {
        g_vdTried = true;
        CoCreateInstance(kCLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&g_vdManager));
    }
    if (!g_vdManager) return true;   // can't tell -> behave like Alt-Tab
    BOOL onCurrent = FALSE;
    if (FAILED(g_vdManager->IsWindowOnCurrentVirtualDesktop(h, &onCurrent))) {
        return true;
    }
    return onCurrent != FALSE;
}

// WM_GETICON to a foreign window must never block: a hung target would
// stall the hook-owning thread and, past LowLevelHooksTimeout, Windows
// silently removes the LL hook. 200 ms, then fall through.
HICON QueryIcon(HWND h, WPARAM which) {
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(h, WM_GETICON, which, 0, SMTO_ABORTIFHUNG, 200,
                            &result) != 0) {
        return reinterpret_cast<HICON>(result);
    }
    return nullptr;
}

struct EnumCtx {
    HMONITOR mon;
    VirtualDesktops vd;
    const std::vector<std::wstring>* exclude;   // exe names
    std::vector<WindowItem>* out;
};

// Basename of the window's process image (e.g. L"steam.exe"), empty on
// any failure - a window we can't identify is never excluded.
std::wstring ProcessExeName(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return std::wstring();
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return std::wstring();
    wchar_t path[MAX_PATH]{};
    DWORD n = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &n);
    CloseHandle(proc);
    if (!ok) return std::wstring();
    const wchar_t* base = wcsrchr(path, L'\\');
    return base ? base + 1 : path;
}

BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);

    wchar_t cls[64];
    const int clsLen = GetClassNameW(h, cls, 64);
    if (clsLen <= 0 || IsShellChromeClass(cls)) return TRUE;

    wchar_t title[256];
    const int titleLen = GetWindowTextW(h, title, 256);

    WindowTraits t;
    const int cloak = CloakState(h);
    t.visible = IsWindowVisible(h) != FALSE;
    t.hasTitle = titleLen > 0;
    t.toolWindow = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
    t.owned = GetWindow(h, GW_OWNER) != nullptr;
    t.cloaked = (cloak & DWM_CLOAKED_APP) != 0 ||
                (ctx->vd == VirtualDesktops::Current &&
                 (cloak & DWM_CLOAKED_SHELL) != 0);
    t.shellChrome = false;   // class check above already filtered

    if (!ShouldListWindow(t)) return TRUE;

    // User blacklist by exe name.
    if (ctx->exclude && !ctx->exclude->empty()) {
        const std::wstring exe = ProcessExeName(h);
        for (const auto& bad : *ctx->exclude) {
            if (_wcsicmp(exe.c_str(), bad.c_str()) == 0) return TRUE;
        }
    }

    // Which monitor? Minimized windows have no live rect; use the last
    // normal position.
    BOOL minimized = IsIconic(h);
    HMONITOR mon = nullptr;
    if (minimized) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        if (GetWindowPlacement(h, &wp)) {
            mon = MonitorFromRect(&wp.rcNormalPosition,
                                  MONITOR_DEFAULTTONEAREST);
        }
    } else {
        mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    }
    if (mon != ctx->mon) return TRUE;

    if (ctx->vd == VirtualDesktops::Current && !OnCurrentVirtualDesktop(h)) {
        return TRUE;
    }

    WindowItem item;
    item.hwnd = h;
    item.title = title;
    item.minimized = minimized != FALSE;
    item.icon = QueryIcon(h, ICON_SMALL2);
    if (!item.icon) item.icon = QueryIcon(h, ICON_SMALL);
    if (!item.icon) {
        item.icon = reinterpret_cast<HICON>(
            GetClassLongPtrW(h, GCLP_HICONSM));
    }
    ctx->out->push_back(std::move(item));
    return TRUE;
}

} // namespace

bool ShouldListWindow(const WindowTraits& t) {
    return t.visible && t.hasTitle && !t.toolWindow && !t.owned &&
           !t.cloaked && !t.shellChrome;
}

std::vector<WindowItem> EnumerateWindowsOnMonitor(
    HMONITOR mon, VirtualDesktops vd,
    const std::vector<std::wstring>& exclude) {
    std::vector<WindowItem> out;
    EnumCtx ctx{ mon, vd, &exclude, &out };
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

} // namespace edgepop
