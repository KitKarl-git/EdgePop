// Full-screen detection for the phase-2 trigger guard.
//
// Called only at the moment a hover fires - never on a timer - so it costs
// nothing while the user isn't at a border. The check is the classic one
// hot-corner tools converged on:
//   * the FOREGROUND window (a background window covering the screen is not
//     "the user is watching a movie"; a maximized normal app is background
//     to whatever has focus),
//   * its rect fully contains the monitor,
//   * it has no WS_CAPTION / WS_THICKFRAME - a maximized browser keeps those
//     styles, a borderless-fullscreen game or video does not,
//   * and it is not a cloaked UWP ghost.
#pragma once

#include <windows.h>

namespace edgepop {

bool IsFullscreenOnMonitor(const RECT& monitorRc);

} // namespace edgepop
