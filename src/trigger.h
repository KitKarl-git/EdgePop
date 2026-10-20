// Edge-zone stop detector - trigger v2.
//
// Phase 2's sentinel strips alone proved un-landable: the strip is 2 px wide
// and a human "stopping at the edge" stops anywhere in roughly +-30 px. The
// measured robot hit rate was 2/7; humans do worse.
//
// This module keeps the zero-idle-cost property but changes WHAT is detected:
// "cursor stopped near the seam", via a WH_MOUSE_LL hook. The callback does a
// handful of rectangle tests and returns; only when the cursor is inside an
// edge zone does real work start. A stopped mouse generates no events, so
// dwell is re-checked by a short timer that exists ONLY while the cursor is
// inside a zone (armed on entry, killed on leave).
//
// Strips are no longer detectors - they are debug visualization of the seams.
#pragma once

#include <windows.h>

#include <vector>

#include "geometry.h"

namespace edgepop {

// How far from the seam (each side) still counts as "at the edge" and what
// movement counts as "stopped" are runtime settings (zone_px default 28,
// stop_radius_px default 16 - values that measured well on my desk).
// Re-check interval for dwell while inside a zone:
constexpr UINT kZoneTimerId = 2;
constexpr UINT kZoneTimerMs = 40;

// Cross-and-return fires from the hook callback, but the heavy path
// (enumerate + popup show) must run on the message loop: past
// LowLevelHooksTimeout Windows silently removes the LL hook. The
// trigger posts this message to the owner window; wp = segment index.
constexpr UINT kZoneFireMsg = WM_APP + 0x10;

// Hook-path entry is on the class (OnTimerIfMoving); zones are built from
// geometry::Segment lists.

class EdgeZoneTrigger {
public:
    // Installs the low-level mouse hook. onFire receives the segment index
    // once the cursor has dwelled inside that segment's zone; dwellMs is
    // polled at fire-check time.
    void Install(_In_opt_ HWND owner,
                 _In_opt_ void (*onFire)(size_t),
                 _In_opt_ unsigned (*dwellMs)());
    void Uninstall();
    bool Installed() const { return hook_ != nullptr; }

    // Callback wiring without the hook (unit tests).
    void SetCallbacks(_In_opt_ void (*onFire)(size_t),
                      _In_opt_ unsigned (*dwellMs)());

    // Mouse-down events anywhere (fed from the same LL hook) - used by the
    // popup for outside-click dismissal.
    void SetMouseDownHandler(_In_opt_ void (*onDown)(POINT));

    // Wheel events from the same LL hook (signed notches' worth of delta) -
    // a NOACTIVATE popup never has focus, so the OS would route the wheel
    // to some other window; the hook is the reliable path.
    void SetMouseWheelHandler(_In_opt_ void (*onWheel)(int));

    // Test seam for the drag check: the suite must not depend on the
    // desktop's live mouse-button state (my clicks randomly suppress
    // fires otherwise). Null (default) = real GetKeyState.
    void SetButtonsHeldQuery(_In_opt_ bool (*q)());

    // Every mouse move from the same LL hook (one function-pointer call on
    // the fast path) - used by the taskbar badges for hover detection
    //. Keep the callback trivial; it fires per pixel system-wide.
    void SetMouseMoveHandler(_In_opt_ void (*onMove)(POINT));

    // Activation mode config: mode() 0 = stop-at-edge dwell,
    // 1 = cross-and-return. crossMs() is the return window.
    void SetActivationConfig(_In_opt_ int (*mode)(),
                             _In_opt_ unsigned (*crossMs)());

    // Rebuilds zones from the current segments (display change / settings).
    void UpdateZones(_In_ const std::vector<Segment>& segments, int zonePx);
    void SetStopRadius(int px) { stopRadiusPx_ = px; }

    // Dwell re-check, driven by the owner window's WM_TIMER.
    void OnTimer();

    // Hook-path entry: position update without firing (the timer fires).
    void OnTimerIfMoving(POINT pt);

    // Core state machine, one mouse position at a time. allowFire is false
    // on the hook path (moves) and true on the timer path (dwell checks).
    // Public for unit tests.
    void ProcessPoint(POINT pt, bool allowFire);

private:
    struct Zone {
        RECT rect{};           // seam strip inflated by zonePx (dwell mode)
        size_t segment = 0;
        Edge edge = Edge::Right;
        LONG seam = 0;         // the border line coordinate
        POINT anchor{};        // position when movement last stopped
        ULONGLONG stillSince = 0;
        bool inside = false;
        bool fired = false;
        // cross-back state: side of the seam (-1 main, +1 neighbor)
        int prevSide = 0;
        ULONGLONG crossTick = 0;
    };

    void ProcessCrossBack(Zone& z, POINT pt, ULONGLONG now, bool dragging);

    // Fire out of the hook's way: posted to the owner window when there is
    // one, called directly otherwise.
    void RequestFire(size_t segment);

    HWND owner_ = nullptr;
    void (*onFire_)(size_t) = nullptr;
    unsigned (*dwellMs_)() = nullptr;
    HHOOK hook_ = nullptr;
    std::vector<Zone> zones_;
    bool timerActive_ = false;
    int stopRadiusPx_ = 16;
};

} // namespace edgepop
