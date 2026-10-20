// EdgePop geometry core: monitor enumeration and border-segment math.
//
// The model:
// the MAIN monitor is the primary display. For each of its four edges, every
// other monitor that touches that edge contributes a SEGMENT covering the
// perpendicular overlap. Two stacked monitors sharing the main's right edge
// produce two independent segments. Each segment gets its own sentinel strip
// and, later, its own popup content.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace edgepop {

// Sentinel strip width in physical pixels. The research says 1 px; we use 2
// for reliability at mixed-DPI boundaries where the cursor path can skip a
// single pixel. Tunable later via constants, not user settings.
constexpr int kStripWidthPx = 2;

// Edge selection bits for settings-driven edges ([trigger] edges = ...).
constexpr DWORD kEdgeLeftBit   = 0x1;
constexpr DWORD kEdgeRightBit  = 0x2;
constexpr DWORD kEdgeTopBit    = 0x4;
constexpr DWORD kEdgeBottomBit = 0x8;
constexpr DWORD kEdgeAll       = 0xF;

enum class Edge { Left, Right, Top, Bottom };

struct MonitorRect {
    HMONITOR handle = nullptr;
    RECT rc{};                 // virtual-screen coordinates
    bool primary = false;
    wchar_t name[32] = L"";    // device name, e.g. \\.\DISPLAY1
};

struct Segment {
    Edge edge = Edge::Right;
    RECT strip{};              // sentinel rect, placed on the MAIN side of the border
    HMONITOR neighbor = nullptr;
    RECT neighborRc{};         // for logs / popup placement later
};

// Enumerates all monitors in virtual-screen coordinates.
std::vector<MonitorRect> EnumerateMonitors();

// The primary monitor ("main display"). Falls back to the first monitor.
const MonitorRect* FindPrimary(const std::vector<MonitorRect>& monitors);

// Computes one Segment per (main edge x touching neighbor) overlap, limited
// to the edges selected in edgeMask (pass kEdgeAll for "auto" behavior).
// Edges must coincide exactly; monitors merely near each other do not couple.
std::vector<Segment> ComputeSegments(const std::vector<MonitorRect>& monitors,
                                     DWORD edgeMask);

// Human-readable one-liners for debug logs.
std::wstring DescribeMonitor(const MonitorRect& m);
std::wstring DescribeSegment(const Segment& s);
const wchar_t* EdgeName(Edge e);

// --- taskbar-indicator math -------------------------------------------
//
// Direction arrows on the taskbar need, per monitor: the 8-way compass
// direction from the MAIN monitor, how many monitor hops away it is (">" =
// adjacent that way, ">>" = one further), and a stable display number for
// the hover chips (primary = 1, the rest in reading order).

enum class MonitorDir { E = 0, NE, N, NW, W, SW, S, SE };

struct MonitorRelation {
    MonitorDir dir = MonitorDir::E;
    int hops = 1;          // 1 = adjacent in that direction, 2 = one further
    bool main = false;     // the main monitor itself (no indicator)
    int number = 1;        // display number for chips; primary = 1
};

// Pure math, unit-tested: for every monitor in `monitors` (indices align),
// its relation to the primary. Direction is quantized from center-to-center
// vectors: |dx| >= 2|dy| counts as pure horizontal, |dy| >= 2|dx| as pure
// vertical, else diagonal. Hops counts monitors whose center lies inside the
// box spanned by the two centers (so a 3-in-a-row gives main→far = 2).
std::vector<MonitorRelation> RelateMonitorsToMain(
    const std::vector<MonitorRect>& monitors);

} // namespace edgepop
