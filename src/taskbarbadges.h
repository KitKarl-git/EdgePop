// Taskbar monitor indicators: direction arrows on the taskbar under
// each running icon + optional hover chips above the taskbar.
//
// Everything is read from OUTSIDE explorer.exe: a watcher thread polls the
// Win11 taskbar's UI Automation tree (each running button's AutomationId is
// literally "Window: 0x<hwnd>"),
// maps the HWND to a monitor, and hands a snapshot to a click-through
// layered overlay window z-placed directly above Shell_TrayWnd (the taskbar
// lives in the same z-band as normal windows on this build - band 1).
//
// Rendering goes D2D (WARP) -> CPU-readable bitmap -> DIB ->
// UpdateLayeredWindow: the ID2D1DCRenderTarget path leaves a DIB's alpha at
// zero, which only survives for bright single-tone glyphs; chips
// are dark with light digits, so the full-correctness route it is. The
// surface is tiny (taskbar + a 36 px hover band) and updates at most ~3/s.
#pragma once

#include <windows.h>

#include <vector>

#include "geometry.h"
#include "settings.h"

namespace edgepop {

// Everything the renderer needs, mapped from Settings.
struct TaskbarBadgeConfig {
    bool enabled = true;             // master switch: gates both sections
    // Shared look.
    ArrowShape shape = ArrowShape::Triangle;
    ArrowPos pos = ArrowPos::Left;   // anchor at the taskbar item's left,
                                     // center, or right edge
    COLORREF color = RGB(255, 255, 255);
    bool shadow = true;              // dark drop shadow behind bare glyphs
    // Persistent section (rests on the taskbar edge; yields to hover).
    bool persistentNumbers = false;
    bool persistentArrows = true;
    unsigned persistentSize = 6;
    bool persistentBg = false;       // capsule; off = bare glyphs
    // Hover section (lifted above Windows' thumbnail previews).
    bool hoverNumbers = true;
    bool hoverArrows = true;
    unsigned hoverSize = 8;
    bool hoverBg = true;
};

class TaskbarBadges {
public:
    static TaskbarBadges& Instance();

    // Main thread. Creates the overlay window and starts the UIA watcher.
    bool Start(HINSTANCE inst);
    void Stop();   // joins the watcher, destroys the overlay

    // Main thread, any time. Both features share one watcher.
    void SetConfig(const TaskbarBadgeConfig& cfg);
    void UpdateMonitors(const std::vector<MonitorRect>& monitors);

    // Main thread, fed from the LL hook (the same hook the trigger uses):
    // true while the cursor is over the taskbar. Chips show after this goes
    // true and hide a short grace after it goes false.
    void SetTaskbarHover(bool in);

    // The last taskbar rect the watcher saw (main thread only - the hook
    // callback and the overlay's message processing share this thread).
    // False until the first snapshot arrives.
    bool TaskbarRect(RECT* rc) const;

    void SetLogSink(void (*log)(const wchar_t*));

private:
    TaskbarBadges() = default;
    ~TaskbarBadges() = default;
    TaskbarBadges(const TaskbarBadges&) = delete;
    TaskbarBadges& operator=(const TaskbarBadges&) = delete;
};

} // namespace edgepop
