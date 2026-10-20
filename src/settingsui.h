// EdgePop settings dialog: one window, live-apply. Sliders carry
// buddy edit boxes for exact numeric input; every change is written
// through to the INI and applied immediately via the `apply` callback
// (which receives before/after so the caller can react to specific
// fields - rebuild zones, touch the autostart registry key, etc).
#pragma once

#include <windows.h>

#include "settings.h"

namespace edgepop {

// Returns the DialogBoxParam result (-1 on failure). `advanced` receives
// actions from the Advanced dialog (1 = rebuild strips).
INT_PTR ShowSettingsDialog(HINSTANCE inst, HWND parent, Settings* s,
                           void (*apply)(const Settings& before,
                                         const Settings& after),
                           void (*advanced)(int action) = nullptr);

// The About box: app icon, version, the kitkarl.com link.
INT_PTR ShowAboutDialog(HINSTANCE inst, HWND parent);

// The startup splash: modeless, never steals focus, closes itself
// (timer / click / link). Call once from WinMain before the message pump.
void ShowSplash(HINSTANCE inst, HWND owner);

// Closes the (?) help card when a mouse-down lands outside it;
// feed it from the LL-hook mouse-down path like the popup's.
void CloseHelpCardIfOutside(POINT pt);

// Re-syncs the open settings dialog after the Settings struct was changed
// outside the dialog. Safe no-op
// when the dialog is closed.
void RefreshSettingsDialog(Settings* s);

} // namespace edgepop
