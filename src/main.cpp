// EdgePop - phases 0-3.
//
//   * per-monitor DPI awareness v2 (manifest + programmatic fallback)
//   * monitor enumeration + border-segment math (geometry.cpp)
//   * trigger v2: WH_MOUSE_LL seam-zone stop detection (trigger.cpp),
//     full-screen guard at fire time (fullscreen.cpp)
//   * window enumeration per the Alt-Tab rules (winenums.cpp)
//   * the popup: Direct2D/DirectComposition list with click-to-switch
//     and full dismissal rules (popup.cpp)
//   * settings window with sliders, live-apply (settingsui.cpp);
//     INI next to the exe is the source of truth
//   * debug strips visualize the seams (--debug or the checkbox)
//
// The invisible-window trap (research addendum #1): a layered window with
// alpha == 0 is hit-test transparent - mouse messages pass through it. We
// use alpha == 1 (1/255) which is still invisible but receives input.

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <commctrl.h>   // InitCommonControlsEx (settings dialog)
#include <shellapi.h>   // Shell_NotifyIcon
#include <shellscalingapi.h>  // GetDpiForMonitor
#include <objbase.h>    // CoInitializeEx (IVirtualDesktopManager)
#include <tlhelp32.h>   // single-instance zombie sweep

#include <string>
#include <vector>
#include <algorithm>   // std::sort for chain-mode sections
#include <mutex>       // Log() is called from the main thread and the
                       // taskbar watcher (TBLog -> g_log) - serialize the
                       // shared file pointer

#include "geometry.h"
#include "settings.h"
#include "fullscreen.h"
#include "trigger.h"
#include "winenums.h"
#include "popup.h"
#include "taskbarbadges.h"
#include "osver.h"
#include "settingsui.h"
#include "resource.h"

namespace {

// --- identifiers -----------------------------------------------------------

constexpr wchar_t kMsgWndClass[]   = L"EdgePopMsgWnd";
constexpr wchar_t kStripWndClass[] = L"EdgePopStrip";
constexpr UINT    WM_APP_TRAY = WM_APP + 1;
constexpr UINT    IDM_SETTINGS = 100;
constexpr UINT    IDM_ENABLED = 101;   // tray: activation master switch
constexpr UINT    IDM_EXIT     = 102;
constexpr UINT    IDM_ABOUT    = 103;
constexpr UINT    IDM_DONATE   = 104;
constexpr UINT    kFlashTimerId = 1;
constexpr UINT    kTrayIconId  = 1;
// Trailing debounce for INI writes: slider drags fire WM_HSCROLL
// per pixel and every key is its own file flush - that, not painting, was
// the second half of the slider stutter. The id must NOT collide with any
// other timer on the msg window: kZoneTimerId is 2, and re-using it made
// SetTimer silently re-aim the trigger's 40 ms dwell timer at 350 ms -
// the "thumbnail delay is back" round.
constexpr UINT    kIniSaveTimerId = 0x7A5E;

// --- state -----------------------------------------------------------------

HINSTANCE g_hInst = nullptr;
HWND      g_hwndMsg = nullptr;
bool      g_trayAdded = false;
// Explorer broadcasts this when it (re)creates the taskbar - the tray icon
// died with the old explorer, so re-add ours. Registered at startup
// because the value is per-session, not a constant.
UINT      g_msgTaskbarCreated = 0;

edgepop::Settings g_settings;
std::vector<edgepop::MonitorRect> g_monitors;
std::vector<edgepop::Segment>     g_segments;
std::vector<HWND>                 g_strips;   // debug-only seam markers
edgepop::EdgeZoneTrigger          g_trigger;  // the real detector
edgepop::MonitorRect              g_mainMon;  // cached primary for popups
RECT g_mainRc{};                       // cached primary rect for the guard

// Hover dedup between the hook and (debug) strips, and anti-spam while the
// popup would be open.
size_t     g_lastSeg = static_cast<size_t>(-1);
ULONGLONG  g_lastFireTick = 0;

wchar_t g_iniPath[MAX_PATH] = L"";
wchar_t g_logPath[MAX_PATH] = L"";
HANDLE  g_logFile = INVALID_HANDLE_VALUE;

// --- logging ----------------------------------------------------------------

void Log(const std::wstring& msg) {
    std::wstring line = msg + L"\r\n";
    OutputDebugStringW(line.c_str());
    if (g_logFile != INVALID_HANDLE_VALUE) {
        // Two threads log (main + taskbar watcher); an unsynchronized
        // WriteFile pair interleaves the shared file pointer.
        static std::mutex m;
        std::lock_guard<std::mutex> lock(m);
        DWORD written = 0;
        WriteFile(g_logFile, line.c_str(),
                  static_cast<DWORD>(line.size() * sizeof(wchar_t)),
                  &written, nullptr);
    }
}

// --- paths, log file, settings ----------------------------------------------

void InitPaths() {
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return;
    *slash = L'\0';
    swprintf_s(g_iniPath, L"%s\\EdgePop.ini", exe);
    swprintf_s(g_logPath, L"%s\\EdgePop-debug.log", exe);
}

void OpenLogFile() {
    if (!g_settings.debugStrips || g_logFile != INVALID_HANDLE_VALUE) return;
    // OPEN_ALWAYS + append: toggling debug mid-session keeps earlier lines.
    g_logFile = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(g_logFile, &size)) return;
    SetFilePointer(g_logFile, 0, nullptr, FILE_END);
    if (size.QuadPart == 0) {
        const wchar_t bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(g_logFile, &bom, sizeof(bom), &written, nullptr);
    }
}

// --- autostart (derived state: INI is the truth, registry follows) -----------

bool SetStartWithWindows(bool enable) {
    HKEY run = nullptr;
    const LSTATUS open = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
        KEY_SET_VALUE, &run);
    if (open != ERROR_SUCCESS) return false;
    bool ok;
    if (enable) {
        wchar_t exe[MAX_PATH];
        wchar_t quoted[MAX_PATH + 2];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        swprintf_s(quoted, L"\"%s\"", exe);
        const DWORD bytes =
            static_cast<DWORD>((wcslen(quoted) + 1) * sizeof(wchar_t));
        ok = RegSetValueExW(run, L"EdgePop", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(quoted),
                            bytes) == ERROR_SUCCESS;
    } else {
        const LSTATUS del = RegDeleteValueW(run, L"EdgePop");
        ok = del == ERROR_SUCCESS || del == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(run);
    return ok;
}

// --- strip windows -----------------------------------------------------------

COLORREF StripColor(bool hovered) {
    if (hovered) return RGB(0, 255, 0);      // hover flash: green
    if (g_settings.debugStrips) return RGB(255, 0, 255);  // debug: magenta
    return RGB(0, 0, 0);                     // normal: black at alpha 1/255
}

void RepaintStrip(HWND hwnd) {
    InvalidateRect(hwnd, nullptr, TRUE);
}

void ApplyStripVisibility(HWND hwnd) {
    // alpha 0 would make the window click-through (the invisible-window
    // trap); 1/255 is imperceptible but still receives mouse input.
    BYTE alpha = g_settings.debugStrips ? 255 : 1;
    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
    RepaintStrip(hwnd);
}

// forward: section builder for the popup (defined after FireHover)
std::vector<edgepop::PopupSection> BuildSections(
    const edgepop::Segment& seg);

void FireHover(size_t idx) {
    if (idx >= g_segments.size()) return;
    if (!g_settings.activationEnabled) return;   // master switch

    const ULONGLONG now = GetTickCount64();
    if (idx == g_lastSeg && now - g_lastFireTick < 1500) return;
    g_lastSeg = idx;
    g_lastFireTick = now;

    // Post-close delay: the trigger stays off for a few seconds
    // after the user dismissed the popup with its X (or Esc).
    if (edgepop::Popup::Instance().Suppressed()) {
        Log(L"HOVER segment #" + std::to_wstring(idx) +
            L" SUPPRESSED (post-close delay)");
        return;
    }

    const edgepop::Segment& seg = g_segments[idx];

    if (g_settings.fullscreenGuard &&
        (edgepop::IsFullscreenOnMonitor(g_mainRc) ||
         edgepop::IsFullscreenOnMonitor(seg.neighborRc))) {
        Log(L"HOVER segment #" + std::to_wstring(idx) +
            L" SUPPRESSED (full-screen guard): " +
            edgepop::DescribeSegment(seg));
        return;
    }

    Log(L"HOVER segment #" + std::to_wstring(idx) + L": " +
        edgepop::DescribeSegment(seg));

    // The popup (phase 3). If it comes up empty, we still show it -
    // "No windows on this monitor" is feedback too.
    POINT cursor{};
    GetCursorPos(&cursor);
    const auto sections = BuildSections(seg);
    size_t count = 0;
    for (const auto& s : sections) count += s.items.size();
    Log(L"  popup: " + std::to_wstring(count) +
        L" window(s), " + std::to_wstring(sections.size()) + L" section(s), " +
        std::to_wstring(edgepop::CountCachedSnapshots(sections)) +
        L" snapshot(s) cached");
    edgepop::Popup::Instance().Show(seg, g_mainMon, sections, cursor,
                                    g_settings);

    if (g_settings.debugStrips && idx < g_strips.size() && g_strips[idx]) {
        SetPropW(g_strips[idx], L"hovering", reinterpret_cast<HANDLE>(1));
        RepaintStrip(g_strips[idx]);
        SetTimer(g_strips[idx], kFlashTimerId, 400, nullptr);
    }
}

void OnZoneFire(size_t idx) { FireHover(idx); }
unsigned GetDwellMs() { return g_settings.dwellMs; }
int GetActMode() {
    return g_settings.activation == edgepop::Activation::CrossBack ? 1 : 0;
}
unsigned GetCrossMs() { return g_settings.crossBackMs; }
void PopupLog(const wchar_t* s) { Log(s); }

// Card-close reconcile: the popup re-asks for its segment's real
// window list a beat after a card close - a confirmation dialog can veto
// the WM_CLOSE, and any window closed by other means drops out too.
void PopupRefreshSink(const edgepop::Segment& seg) {
    auto& popup = edgepop::Popup::Instance();
    if (!popup.Visible()) return;
    const auto sections = BuildSections(seg);
    size_t count = 0;
    for (const auto& s : sections) count += s.items.size();
    if (count == 0) {
        popup.Hide();   // nothing left to list
        return;
    }
    popup.Refresh(sections);
}

// The activation master switch: with no zones installed nothing can
// fire, while the LL hook (popup dismissal, taskbar hover) stays alive.
// Every path that rebuilds zones re-applies this, so the switch can't be
// un-done by a display change or settings edit.
void ApplyActivationState() {
    if (g_settings.activationEnabled) {
        g_trigger.UpdateZones(g_segments,
                              static_cast<int>(g_settings.zonePx));
        Log(L"activation: ON");
    } else {
        g_trigger.UpdateZones({}, static_cast<int>(g_settings.zonePx));
        edgepop::Popup::Instance().Hide();
        Log(L"activation: OFF");
    }
}

// Taskbar hover detection for the badges: rides the existing LL
// hook's move callback; a plain state change posts to the overlay.
bool g_inTaskbar = false;
void OnMouseMove(POINT pt) {
    RECT rc;
    const bool in =
        edgepop::TaskbarBadges::Instance().TaskbarRect(&rc) &&
        PtInRect(&rc, pt) != FALSE;
    if (in != g_inTaskbar) {
        g_inTaskbar = in;
        edgepop::TaskbarBadges::Instance().SetTaskbarHover(in);
    }
}

// Settings -> badge renderer config.
edgepop::TaskbarBadgeConfig BadgeConfig(const edgepop::Settings& s) {
    edgepop::TaskbarBadgeConfig c;
    c.enabled = s.taskbarArrows;
    c.shadow = s.arrowShadow;
    c.shape = s.arrowShape;
    c.pos = s.arrowPos;
    c.color = s.arrowColor;
    c.persistentNumbers = s.persistentNumbers;
    c.persistentArrows = s.persistentArrows;
    c.persistentSize = s.persistentSize;
    c.persistentBg = s.persistentBg;
    c.hoverNumbers = s.hoverNumbers;
    c.hoverArrows = s.hoverArrows;
    c.hoverSize = s.hoverSize;
    c.hoverBg = s.hoverBg;
    return c;
}

// Any mouse-down anywhere (from the LL hook): outside-click dismissal for
// the popup and the (?) help card.
void OnAnyMouseDown(POINT pt) {
    edgepop::CloseHelpCardIfOutside(pt);
    edgepop::Popup::Instance().OnOutsideMouseDown(pt);
}

// Wheel notches anywhere (from the LL hook): scroll the overflowed popup
// when the cursor is over it.
void OnAnyMouseWheel(int delta) {
    edgepop::Popup::Instance().OnMouseWheel(delta);
}

// "Display 3" from "\\.\DISPLAY3".
std::wstring MonitorLabel(const wchar_t* device) {
    const wchar_t* p = wcsstr(device, L"DISPLAY");
    if (!p) return device;
    return std::wstring(L"Display ") + (p + 7);
}

// Sections for the fired segment: the neighbor alone, or every monitor
// beyond the seam in the segment's direction.
std::vector<edgepop::PopupSection> BuildSections(
    const edgepop::Segment& seg) {
    using edgepop::ChainMode;
    using edgepop::PopupSection;
    std::vector<PopupSection> out;
    const std::vector<std::wstring> exclude =
        edgepop::ParseExcludeCsv(g_settings.excludeApps);

    if (g_settings.chainMode != ChainMode::Direction) {
        PopupSection s;
        s.items = edgepop::EnumerateWindowsOnMonitor(
            seg.neighbor, g_settings.virtualDesktops, exclude);
        out.push_back(std::move(s));
        return out;
    }

    std::vector<const edgepop::MonitorRect*> beyond;
    for (const auto& m : g_monitors) {
        if (m.handle == g_mainMon.handle) continue;
        const LONG cx = (m.rc.left + m.rc.right) / 2;
        const LONG cy = (m.rc.top + m.rc.bottom) / 2;
        bool ok = false;
        switch (seg.edge) {
            case edgepop::Edge::Right:
                ok = cx >= g_mainRc.right; break;
            case edgepop::Edge::Left:
                ok = cx <= g_mainRc.left; break;
            case edgepop::Edge::Top:
                ok = cy <= g_mainRc.top; break;
            case edgepop::Edge::Bottom:
                ok = cy >= g_mainRc.bottom; break;
        }
        if (ok) beyond.push_back(&m);
    }
    // Nearest to the main monitor first.
    std::sort(beyond.begin(), beyond.end(),
              [&](const edgepop::MonitorRect* a,
                  const edgepop::MonitorRect* b) {
                  const bool horiz = seg.edge == edgepop::Edge::Left ||
                                     seg.edge == edgepop::Edge::Right;
                  const LONG ka = horiz ? a->rc.left : a->rc.top;
                  const LONG kb = horiz ? b->rc.left : b->rc.top;
                  const bool asc = seg.edge == edgepop::Edge::Right ||
                                   seg.edge == edgepop::Edge::Bottom;
                  return asc ? ka < kb : ka > kb;
              });

    for (const auto* m : beyond) {
        PopupSection s;
        s.label = MonitorLabel(m->name);
        s.items = edgepop::EnumerateWindowsOnMonitor(
            m->handle, g_settings.virtualDesktops, exclude);
        if (!s.items.empty()) out.push_back(std::move(s));
    }
    return out;
}

bool MouseButtonsHeld() {
    return (GetKeyState(VK_LBUTTON) & 0x8000) != 0 ||
           (GetKeyState(VK_RBUTTON) & 0x8000) != 0 ||
           (GetKeyState(VK_MBUTTON) & 0x8000) != 0;
}

LRESULT CALLBACK StripWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            const bool hovering = GetPropW(hwnd, L"hovering") != nullptr;
            const HBRUSH brush = CreateSolidBrush(StripColor(hovering));
            FillRect(hdc, &ps.rcPaint, brush);
            DeleteObject(brush);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            // Dragging a window across monitors must not pop anything.
            if (MouseButtonsHeld()) return 0;
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_HOVER | TME_LEAVE;
            tme.hwndTrack = hwnd;
            // 0 = instant per settings spec; TrackMouseEvent needs >= 1 ms.
            tme.dwHoverTime =
                g_settings.dwellMs == 0 ? 1 : g_settings.dwellMs;
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSEHOVER: {
            const size_t idx = static_cast<size_t>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            FireHover(idx);
            return 0;
        }
        case WM_MOUSELEAVE:
            return 0;
        case WM_TIMER:
            KillTimer(hwnd, kFlashTimerId);
            RemovePropW(hwnd, L"hovering");
            RepaintStrip(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // everything painted in WM_PAINT
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_NCDESTROY:
            RemovePropW(hwnd, L"hovering");
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void DestroyStrips() {
    for (HWND hwnd : g_strips) DestroyWindow(hwnd);
    g_strips.clear();
}

void CreateStrips() {
    DestroyStrips();
    // Trigger v2: the hook detects stops near the seam; strips
    // exist only as debug visualization of where the seams are.
    if (!g_settings.debugStrips) return;
    for (size_t i = 0; i < g_segments.size(); ++i) {
        const RECT& r = g_segments[i].strip;
        HWND hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            kStripWndClass, nullptr, WS_POPUP,
            r.left, r.top, r.right - r.left, r.bottom - r.top,
            g_hwndMsg, nullptr, g_hInst, nullptr);
        if (hwnd) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, static_cast<LONG_PTR>(i));
            ApplyStripVisibility(hwnd);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        g_strips.push_back(hwnd);   // nullptr keeps indices aligned
    }
}

// --- geometry refresh --------------------------------------------------------

DWORD ActiveEdgeMask() {
    return g_settings.edgesAuto ? edgepop::kEdgeAll : g_settings.edgeMask;
}

void Rebuild(const wchar_t* why) {
    edgepop::Popup::Instance().Hide();   // stale segment -> stale popup
    g_monitors = edgepop::EnumerateMonitors();
    g_segments = edgepop::ComputeSegments(g_monitors, ActiveEdgeMask());

    const edgepop::MonitorRect* main = edgepop::FindPrimary(g_monitors);
    g_mainRc = main ? main->rc : RECT{};
    g_mainMon = main ? *main : edgepop::MonitorRect{};
    g_trigger.SetStopRadius(static_cast<int>(g_settings.stopRadiusPx));
    ApplyActivationState();   // zones per the master switch
    edgepop::TaskbarBadges::Instance().UpdateMonitors(g_monitors);

    Log(std::wstring(L"--- rebuild (") + (why ? why : L"?") + L") ---");
    for (const auto& m : g_monitors) {
        UINT dpiX = 0, dpiY = 0;
        wchar_t dpi[32] = L"";
        if (SUCCEEDED(GetDpiForMonitor(m.handle, MDT_EFFECTIVE_DPI,
                                       &dpiX, &dpiY))) {
            swprintf_s(dpi, L" dpi=%u%%", dpiX * 100 / 96);
        }
        Log(L"  monitor: " + edgepop::DescribeMonitor(m) + dpi);
    }
    Log(L"  segments: " + std::to_wstring(g_segments.size()));
    for (const auto& s : g_segments)
        Log(L"    " + edgepop::DescribeSegment(s));

    CreateStrips();
}

// --- tray icon ---------------------------------------------------------------

void TrayAdd() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwndMsg;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wcscpy_s(nid.szTip, L"EdgePOP by KitKarl");
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    g_trayAdded = true;
}

void TrayRemove() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwndMsg;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

// Tray menu: the activation switch, then
// Settings / About / Exit - debug and rebuild live behind the Settings
// window's Advanced button.
HMENU BuildMenu() {
    HMENU top = CreatePopupMenu();
    AppendMenuW(top,
                MF_STRING | (g_settings.activationEnabled ? MF_CHECKED : 0),
                IDM_ENABLED, L"Enabled");
    AppendMenuW(top, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(top, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(top, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(top, MF_STRING, IDM_ABOUT, L"About EdgePOP");
    AppendMenuW(top, MF_STRING, IDM_DONATE, L"Donate...");
    AppendMenuW(top, MF_STRING, IDM_EXIT, L"Exit");
    return top;
}

void ShowTrayMenu(int x, int y) {
    HMENU menu = BuildMenu();
    // Required so the menu dismisses when clicking elsewhere.
    SetForegroundWindow(g_hwndMsg);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, g_hwndMsg, nullptr);
    DestroyMenu(menu);
}

// Settings window callbacks.
void ApplySettingsChange(const edgepop::Settings& before,
                          const edgepop::Settings& after) {
    g_settings = after;
    SetTimer(g_hwndMsg, kIniSaveTimerId, 350, nullptr);   // debounced save
    Log(L"settings changed (dialog)");

    if (before.activationEnabled != after.activationEnabled) {
        ApplyActivationState();   // master switch
    }
    if (before.edgesAuto != after.edgesAuto ||
        before.edgeMask != after.edgeMask ||
        before.zonePx != after.zonePx ||
        before.stopRadiusPx != after.stopRadiusPx) {
        Rebuild(L"settings");
    }
    if (before.debugStrips != after.debugStrips) {
        OpenLogFile();
        CreateStrips();
    }
    if (before.taskbarArrows != after.taskbarArrows ||
        before.persistentNumbers != after.persistentNumbers ||
        before.persistentArrows != after.persistentArrows ||
        before.persistentSize != after.persistentSize ||
        before.persistentBg != after.persistentBg ||
        before.hoverNumbers != after.hoverNumbers ||
        before.hoverArrows != after.hoverArrows ||
        before.hoverSize != after.hoverSize ||
        before.hoverBg != after.hoverBg ||
        before.arrowPos != after.arrowPos ||
        before.arrowShape != after.arrowShape ||
        before.arrowColor != after.arrowColor ||
        before.arrowShadow != after.arrowShadow) {
        edgepop::TaskbarBadges::Instance().SetConfig(BadgeConfig(after));
        Log(L"taskbar indicators updated");
    }
    if (before.startWithWindows != after.startWithWindows) {
        // Registry follows the INI; revert the checkbox if denied.
        if (!SetStartWithWindows(after.startWithWindows)) {
            g_settings.startWithWindows = before.startWithWindows;
            edgepop::SaveSettings(g_iniPath, g_settings);
            Log(L"FAILED to update HKCU Run key; setting reverted");
        }
    }
    // dwellMs / popup options are read live at use time; nothing to do.
}

// Advanced dialog actions (1 = rebuild strips).
void OnAdvancedAction(int action) {
    if (action == 1) Rebuild(L"advanced dialog");
}

// --- message window ----------------------------------------------------------

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        // Explorer restarted: the tray icon is gone with the old taskbar.
        // Remove-then-add also retries a startup TrayAdd that lost the
        // race with a still-booting explorer.
        TrayRemove();
        TrayAdd();
        return 0;
    }
    switch (msg) {
        case edgepop::kZoneFireMsg:
            // Cross-back fire, posted out of the LL hook: the
            // heavy path runs here on the message loop, never in-hook.
            OnZoneFire(static_cast<size_t>(wp));
            return 0;
        case edgepop::kMouseDownMsg:
            // Outside-click dismissal, posted out of the LL hook (the
            // hide path renders and captures the screen - gotcha 24).
            OnAnyMouseDown(POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            return 0;
        case edgepop::kMouseWheelMsg:
            // Popup overflow scroll, posted out of the LL hook (the
            // scroll path re-renders and re-registers thumbnails).
            OnAnyMouseWheel(static_cast<int>(
                static_cast<short>(wp)));
            return 0;
        case WM_APP_TRAY: {
            // NOTIFYICON_VERSION_4: LOWORD(lp) = event, cursor packed in wp.
            const UINT event = LOWORD(lp);
            if (event == WM_CONTEXTMENU || event == NIN_SELECT) {
                int x = GET_X_LPARAM(wp);
                int y = GET_Y_LPARAM(wp);
                if (x == 0 && y == 0) {   // defensive fallback
                    POINT pt;
                    GetCursorPos(&pt);
                    x = pt.x; y = pt.y;
                }
                ShowTrayMenu(x, y);
            }
            return 0;
        }
        case WM_COMMAND: {
            const UINT id = LOWORD(wp);
            if (id == IDM_ENABLED) {
                // Tray toggle: flip, apply, debounce-save, and keep
                // an open settings dialog in sync.
                g_settings.activationEnabled = !g_settings.activationEnabled;
                ApplyActivationState();
                SetTimer(hwnd, kIniSaveTimerId, 350, nullptr);
                edgepop::RefreshSettingsDialog(&g_settings);
                Log(L"tray: activation toggled");
                return 0;
            }
            if (id == IDM_SETTINGS) {
                Log(L"settings dialog requested");
                const INT_PTR r = edgepop::ShowSettingsDialog(
                    g_hInst, hwnd, &g_settings, ApplySettingsChange,
                    OnAdvancedAction);
                Log(L"settings dialog returned: " + std::to_wstring(r));
                return 0;
            }
            if (id == IDM_ABOUT) {
                edgepop::ShowAboutDialog(g_hInst, hwnd);
                return 0;
            }
            if (id == IDM_DONATE) {
                ShellExecuteW(hwnd, L"open", L"" EDGEPOP_DONATE_URL,
                              nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (id == IDM_EXIT) {
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == edgepop::kZoneTimerId) {
                g_trigger.OnTimer();
            } else if (wp == kIniSaveTimerId) {
                KillTimer(hwnd, kIniSaveTimerId);
                edgepop::SaveSettings(g_iniPath, g_settings);
            }
            return 0;
        case WM_DISPLAYCHANGE:
            Rebuild(L"display change");
            return 0;
        case WM_DESTROY:
            g_trigger.Uninstall();
            TrayRemove();
            DestroyStrips();
            edgepop::TaskbarBadges::Instance().Stop();
            KillTimer(hwnd, kIniSaveTimerId);   // flush a pending save
            edgepop::SaveSettings(g_iniPath, g_settings);
            if (g_logFile != INVALID_HANDLE_VALUE) {
                CloseHandle(g_logFile);
                g_logFile = INVALID_HANDLE_VALUE;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterClasses() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = g_hInst;
    wc.lpfnWndProc = MsgWndProc;
    wc.lpszClassName = kMsgWndClass;
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = StripWndProc;
    wc.lpszClassName = kStripWndClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
}

} // namespace

// --- single instance -------------------------------------------
//
// Exactly one EdgePop, ever: a second launch pings the running instance's
// hidden message window with SendMessageTimeout(WM_NULL) - a healthy
// instance answers and the newcomer exits quietly; a frozen one times out
// (SMTO_ABORTIFHUNG), gets terminated, and the newcomer takes over the
// (abandoned) mutex. A crashed process needs none of this: the OS releases
// its mutex, so the check below never triggers.

namespace {

// The mutex handle must outlive this function: closing it would destroy
// the mutex (we are the only owner) and re-open the door to a second
// instance. Intentionally never closed.
HANDLE g_singleMutex = nullptr;

void TerminatePid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
}

void TerminateOtherEdgePops() {
    const DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != self &&
                _wcsicmp(pe.szExeFile, L"EdgePop.exe") == 0) {
                TerminatePid(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// Snapshot a window the moment it starts minimizing, so
// its popup card keeps the last-known content - taskbar behavior. The
// hook is out-of-context: the callback arrives on this thread via the
// message loop in wWinMain.
//
// A second hook does the same on every foreground CHANGE: a window the
// user just switched to is definitionally visible on screen, so that is
// the one guaranteed-correct moment to cache it. Close-time captures
// alone left windows that live buried under a maximized window (Karl's
// video monitor) with NO snapshot at all - the D-070 occlusion gate
// correctly refuses to cache their occluder's pixels, so those cards
// fell back to the icon placeholder for the pop-in gap.
HWINEVENTHOOK g_minHook = nullptr;
HWINEVENTHOOK g_fgHook = nullptr;

VOID CALLBACK OnSnapshotEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG,
                              LONG, DWORD, DWORD) {
    if (!hwnd) return;
    if (event == EVENT_SYSTEM_MINIMIZESTART ||
        event == EVENT_SYSTEM_FOREGROUND) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return;   // settings/splash/own UI
        edgepop::CacheWindowSnapshot(hwnd);
    }
}

} // namespace

// Returns true when this launch should run; false = a healthy instance is
// already up and this process should exit.
bool EnsureSingleInstance() {
    g_singleMutex = CreateMutexW(nullptr, TRUE, L"Local\\EdgePop.SingleInstance");
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;

    // Someone is running (or starting). The message window is created
    // before the only slow startup step ends, so poll for a few seconds.
    HWND prev = nullptr;
    for (int i = 0; i < 30 && !prev; ++i) {
        prev = FindWindowW(kMsgWndClass, nullptr);
        if (!prev) Sleep(100);
    }
    if (prev) {
        DWORD_PTR answer = 0;
        if (SendMessageTimeoutW(prev, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000,
                                &answer) != 0) {
            CloseHandle(g_singleMutex);
            g_singleMutex = nullptr;
            return false;   // healthy - the running instance wins
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(prev, &pid);
        TerminatePid(pid);
    } else {
        // Mutex held but no window after 3 s: a windowless zombie - sweep
        // every other EdgePop.exe.
        TerminateOtherEdgePops();
    }

    // The previous owner is gone; acquire the (abandoned) mutex so the
    // NEXT launch sees us as the single instance.
    Sleep(200);   // let its handles actually close
    WaitForSingleObject(g_singleMutex, 3000);
    return true;
}

int WINAPI wWinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE,
                    _In_ PWSTR cmdLine, int) {
    g_hInst = inst;

    // Per-monitor DPI v2: coordinates stay physical across mixed scaling.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    }

    // Windows floor: a clear message instead of a mystery failure
    // on anything older than the supported minimum, Win10 1809 (17763).
    // 0 = the version query itself failed - don't gate on that.
    if (const int build = edgepop::OsBuildNumber();
        build != 0 && build < 17763) {
        MessageBoxW(nullptr,
                    L"EdgePOP requires Windows 10 version 1809 (October 2018 "
                    L"Update) or newer.\n\nPlease update Windows or download "
                    L"an older release at kitkarl.com.",
                    L"EdgePOP", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Runs before anything touches the INI or the log.
    if (!EnsureSingleInstance()) return 0;

    InitPaths();
    g_settings = edgepop::LoadSettings(g_iniPath);
    if (cmdLine && wcsstr(cmdLine, L"--debug")) g_settings.debugStrips = true;
    OpenLogFile();

    // COM for IVirtualDesktopManager (used on the UI thread).
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Trackbars + the SysLink kitkarl.com links in the settings/About
    // dialogs (comctl32; manifest requests v6).
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS;
    InitCommonControlsEx(&icc);

    // Autostart follows the INI on every launch, both ways (INI is the
    // source of truth; the registry is derived state).
    if (g_settings.startWithWindows) {
        if (!SetStartWithWindows(true)) {
            Log(L"WARNING: start_with_windows=true but HKCU Run write failed");
        }
    } else {
        SetStartWithWindows(false);
    }

    RegisterClasses();
    edgepop::Popup::Instance().RegisterClass(inst);
    edgepop::Popup::SetLogSink(PopupLog);
    edgepop::Popup::SetRefreshSink(PopupRefreshSink);   //
    if (!edgepop::Popup::Instance().Create(inst)) {
        Log(L"FATAL: popup window creation failed");
    }

    // Taskbar indicators: watcher thread + click-through overlay.
    // Non-fatal - the app works without them.
    edgepop::TaskbarBadges::Instance().SetLogSink(PopupLog);
    if (!edgepop::TaskbarBadges::Instance().Start(inst)) {
        Log(L"WARNING: taskbar indicators failed to start");
    }
    edgepop::TaskbarBadges::Instance().SetConfig(BadgeConfig(g_settings));

    g_hwndMsg = CreateWindowExW(0, kMsgWndClass, L"EdgePop", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwndMsg) return 1;

    g_msgTaskbarCreated =
        RegisterWindowMessageW(L"TaskbarCreated");   // explorer restarts
    TrayAdd();
    Rebuild(L"startup");
    g_trigger.Install(g_hwndMsg, OnZoneFire, GetDwellMs);
    g_trigger.SetMouseDownHandler(OnAnyMouseDown);
    g_trigger.SetMouseMoveHandler(OnMouseMove);
    g_trigger.SetMouseWheelHandler(OnAnyMouseWheel);
    g_trigger.SetActivationConfig(GetActMode, GetCrossMs);
    if (!g_trigger.Installed()) {
        Log(L"FATAL: WH_MOUSE_LL hook failed; trigger v2 inoperative");
    }
    g_minHook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART,
                                EVENT_SYSTEM_MINIMIZESTART, nullptr,
                                OnSnapshotEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!g_minHook) {
        Log(L"WARNING: minimize-snapshot hook failed");
    }
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
                               EVENT_SYSTEM_FOREGROUND, nullptr,
                               OnSnapshotEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!g_fgHook) {
        Log(L"WARNING: foreground-snapshot hook failed");
    }
    Log(L"EdgePop started (" __DATE__ " " __TIME__ L")");

    // Startup splash: modeless; the pump below drives it.
    edgepop::ShowSplash(g_hInst, g_hwndMsg);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_minHook) UnhookWinEvent(g_minHook);
    if (g_fgHook) UnhookWinEvent(g_fgHook);
    CoUninitialize();
    return 0;
}
