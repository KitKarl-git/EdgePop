#include "trigger.h"

namespace edgepop {
namespace {

bool MouseButtonsHeld() {
    return (GetKeyState(VK_LBUTTON) & 0x8000) != 0 ||
           (GetKeyState(VK_RBUTTON) & 0x8000) != 0 ||
           (GetKeyState(VK_MBUTTON) & 0x8000) != 0;
}

// Test seam: the unit tests run on my desktop, where my real
// mouse buttons leak into GetKeyState and randomly suppress fires
// (dragging defers dwell, disarms cross-back). Null = the real state.
bool (*g_buttonsHeld)() = nullptr;

EdgeZoneTrigger* g_instance = nullptr;
HWND g_hookOwner = nullptr;   // where mouse-down/wheel get posted
void (*g_onMouseDown)(POINT) = nullptr;
void (*g_onMouseMove)(POINT) = nullptr;
void (*g_onMouseWheel)(int) = nullptr;
int (*g_actMode)() = nullptr;
unsigned (*g_actCrossMs)() = nullptr;

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION && g_instance) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (wp == WM_MOUSEMOVE) {
            // Fast path: one call that rectangle-tests a couple of zones and
            // returns. Must stay trivial - this runs on every mouse move,
            // system-wide.
            g_instance->OnTimerIfMoving(info->pt);
            if (g_onMouseMove) g_onMouseMove(info->pt);
        } else if (wp == WM_MOUSEWHEEL) {
            if (g_onMouseWheel && g_hookOwner) {
                PostMessageW(g_hookOwner, kMouseWheelMsg,
                             static_cast<WPARAM>(static_cast<short>(
                                 HIWORD(info->mouseData))),
                             0);
            }
        } else if ((wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN ||
                    wp == WM_NCLBUTTONDOWN) &&
                   g_onMouseDown && g_hookOwner) {
            // Posted, never run in-hook: the dismissal path hides the
            // popup, which re-renders and captures the screen.
            PostMessageW(g_hookOwner, kMouseDownMsg, 0,
                         MAKELPARAM(info->pt.x, info->pt.y));
        }
    }
    return CallNextHookEx(nullptr, nCode, wp, lp);
}

} // namespace

void EdgeZoneTrigger::SetCallbacks(void (*onFire)(size_t),
                                    unsigned (*dwellMs)()) {
    onFire_ = onFire;
    dwellMs_ = dwellMs;
}

void EdgeZoneTrigger::SetMouseDownHandler(void (*onDown)(POINT)) {
    g_onMouseDown = onDown;
}

void EdgeZoneTrigger::SetMouseWheelHandler(void (*onWheel)(int)) {
    g_onMouseWheel = onWheel;
}

void EdgeZoneTrigger::SetButtonsHeldQuery(bool (*q)()) {
    g_buttonsHeld = q;
}

void EdgeZoneTrigger::SetMouseMoveHandler(void (*onMove)(POINT)) {
    g_onMouseMove = onMove;
}

void EdgeZoneTrigger::SetActivationConfig(int (*mode)(),
                                           unsigned (*crossMs)()) {
    g_actMode = mode;
    g_actCrossMs = crossMs;
}

void EdgeZoneTrigger::Install(HWND owner, void (*onFire)(size_t),
                               unsigned (*dwellMs)()) {
    owner_ = owner;
    g_hookOwner = owner;
    SetCallbacks(onFire, dwellMs);
    g_instance = this;
    hook_ = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
}

void EdgeZoneTrigger::Uninstall() {
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (timerActive_ && owner_) {
        KillTimer(owner_, kZoneTimerId);
        timerActive_ = false;
    }
    if (g_instance == this) g_instance = nullptr;
}

void EdgeZoneTrigger::UpdateZones(const std::vector<Segment>& segments,
                                   int zonePx) {
    zones_.clear();
    for (size_t i = 0; i < segments.size(); ++i) {
        Zone z{};
        z.rect = segments[i].strip;
        InflateRect(&z.rect, zonePx, zonePx);
        z.segment = i;
        z.edge = segments[i].edge;
        // The border line: the main monitor's edge the strip sits on.
        switch (segments[i].edge) {
            case Edge::Left:   z.seam = segments[i].strip.left;  break;
            case Edge::Right:  z.seam = segments[i].strip.right; break;
            case Edge::Top:    z.seam = segments[i].strip.top;   break;
            case Edge::Bottom: z.seam = segments[i].strip.bottom; break;
        }
        zones_.push_back(z);
    }
}

// Cross-and-return activation: the corridor is the zone's
// perpendicular span, unlimited along the seam direction - a "cross" can be
// as deep as the user likes, only the return window is timed. Fire happens
// on the return event itself, so no timer is needed in this mode.

// The fire path (window enumeration + popup show) is far too heavy for the
// LL-hook callback - past LowLevelHooksTimeout Windows silently unhooks us
// and the whole trigger dies. With an owner window the fire is
// posted to the message loop; the unit tests (no owner) get the direct
// callback.
void EdgeZoneTrigger::RequestFire(size_t segment) {
    if (!onFire_) return;
    if (owner_) {
        PostMessageW(owner_, kZoneFireMsg, static_cast<WPARAM>(segment), 0);
    } else {
        onFire_(segment);
    }
}

void EdgeZoneTrigger::ProcessCrossBack(Zone& z, POINT pt, ULONGLONG now,
                                       bool dragging) {
    const bool verticalSeam = z.edge == Edge::Left || z.edge == Edge::Right;
    const bool inCorridor = verticalSeam
        ? (pt.y >= z.rect.top && pt.y < z.rect.bottom)
        : (pt.x >= z.rect.left && pt.x < z.rect.right);
    if (!inCorridor) {
        z.prevSide = 0;
        z.crossTick = 0;
        return;
    }
    int side;
    switch (z.edge) {
        // Neighbor side = 1. Half-open pixel ranges: the monitor owning
        // the seam line keeps its boundary pixel - `<=` on Left/Top used
        // to hand main's edge pixel to the neighbor.
        case Edge::Right:  side = pt.x >= z.seam ? 1 : -1; break;  // main is left
        case Edge::Left:   side = pt.x <  z.seam ? 1 : -1; break;  // main is right
        case Edge::Bottom: side = pt.y >= z.seam ? 1 : -1; break;
        default:           side = pt.y <  z.seam ? 1 : -1; break;  // Top
    }
    if (dragging) {
        z.prevSide = side;
        z.crossTick = 0;
    } else if (z.prevSide != 0 && z.prevSide != side) {
        // Direction-agnostic: the first crossing from either side
        // arms, the return within the window fires - starting from the
        // neighbor monitor works exactly like starting from the main one.
        if (z.crossTick == 0) {
            z.crossTick = now;                    // went over (either way)
        } else if (g_actCrossMs &&
                   now - z.crossTick <= g_actCrossMs()) {
            RequestFire(z.segment);               // came back in time
            z.crossTick = 0;
        } else {
            z.crossTick = now;                    // window expired: re-arm
        }
        z.prevSide = side;
    } else {
        z.prevSide = side;
    }
}

void EdgeZoneTrigger::ProcessPoint(POINT pt, bool allowFire) {
    const ULONGLONG now = GetTickCount64();
    const bool dragging = g_buttonsHeld ? g_buttonsHeld()
                                        : MouseButtonsHeld();
    const int actMode = g_actMode ? g_actMode() : 0;
    bool anyInside = false;

    for (auto& z : zones_) {
        if (actMode == 1) {           // cross-and-return
            ProcessCrossBack(z, pt, now, dragging);
            continue;
        }
        const bool in = PtInRect(&z.rect, pt) != FALSE;
        if (!in) {
            z.inside = false;
            z.fired = false;
            continue;
        }
        anyInside = true;
        if (!z.inside) {                    // zone entry
            z.inside = true;
            z.fired = false;
            z.anchor = pt;
            z.stillSince = now;
            continue;
        }
        if (dragging) {                     // postpone while a button is held
            z.anchor = pt;
            z.stillSince = now;
            continue;
        }
        if (z.fired) continue;              // one fire per stay

        const int dx = pt.x - z.anchor.x;
        const int dy = pt.y - z.anchor.y;
        const long long rr = static_cast<long long>(stopRadiusPx_) * stopRadiusPx_;
        if (static_cast<long long>(dx) * dx +
                static_cast<long long>(dy) * dy > rr) {
            z.anchor = pt;                  // still moving; restart dwell
            z.stillSince = now;
        } else if (allowFire && dwellMs_ &&
                   now - z.stillSince >= dwellMs_()) {
            z.fired = true;
            if (onFire_) onFire_(z.segment);
        }
    }

    if (actMode == 0 && anyInside && !timerActive_ && owner_) {
        timerActive_ = SetTimer(owner_, kZoneTimerId, kZoneTimerMs,
                                nullptr) != 0;
    } else if ((actMode == 1 || !anyInside) && timerActive_ && owner_) {
        KillTimer(owner_, kZoneTimerId);
        timerActive_ = false;
    }
}

void EdgeZoneTrigger::OnTimer() {
    POINT pt;
    if (!GetCursorPos(&pt)) return;
    ProcessPoint(pt, true);
}

void EdgeZoneTrigger::OnTimerIfMoving(POINT pt) {
    ProcessPoint(pt, false);
}

} // namespace edgepop
