// EdgePop window enumeration: which top-level windows count as
// "apps on this monitor". The rules are the Alt-Tab rules, kept pure and
// unit-testable (WindowTraits -> ShouldListWindow) before any Win32 call
// touches them.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "settings.h"

namespace edgepop {

// Everything the filter needs, extracted from a live HWND so the decision
// itself can be tested without windows.
struct WindowTraits {
    bool visible = false;
    bool hasTitle = false;
    bool toolWindow = false;      // WS_EX_TOOLWINDOW
    bool owned = false;           // has an owner window
    bool cloaked = false;         // DWMWA_CLOAKED (UWP ghosts etc.)
    bool shellChrome = false;     // taskbar / desktop classes we never list
};

// Alt-Tab rules: all six traits must be in the "list it" state.
bool ShouldListWindow(const WindowTraits& t);

struct WindowItem {
    HWND hwnd = nullptr;
    std::wstring title;
    HICON icon = nullptr;         // small icon, may be null (caller renders a
                                  // fallback glyph)
    bool minimized = false;
};

// All listable windows on `mon`, topmost-first (EnumWindows order).
// Minimized windows are bucketed by their last normal position
// (GetWindowPlacement). `vd` filters by virtual desktop when
// current_desktop (IVirtualDesktopManager; failures include the window,
// matching the Alt-Tab mental model closely enough for v1). `exclude` is
// a list of exe names that are never listed.
std::vector<WindowItem> EnumerateWindowsOnMonitor(
    HMONITOR mon, VirtualDesktops vd,
    const std::vector<std::wstring>& exclude = {});

} // namespace edgepop
