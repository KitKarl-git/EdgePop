#include "geometry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace edgepop {
namespace {

BOOL CALLBACK MonitorEnumProc(HMONITOR hmon, HDC, LPRECT /*rc*/, LPARAM lp) {
    auto* list = reinterpret_cast<std::vector<MonitorRect>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmon, &mi)) {
        MonitorRect m{};
        m.handle  = hmon;
        m.rc      = mi.rcMonitor;
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        wcsncpy_s(m.name, mi.szDevice, _TRUNCATE);
        list->push_back(m);
    }
    return TRUE;
}

// Overlap of [a0,a1) and [b0,b1); false when empty.
bool Overlap(int a0, int a1, int b0, int b1, int* lo, int* hi) {
    const int l = a0 > b0 ? a0 : b0;
    const int h = a1 < b1 ? a1 : b1;
    if (l >= h) return false;
    *lo = l;
    *hi = h;
    return true;
}

} // namespace

std::vector<MonitorRect> EnumerateMonitors() {
    std::vector<MonitorRect> list;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
                        reinterpret_cast<LPARAM>(&list));
    return list;
}

const MonitorRect* FindPrimary(const std::vector<MonitorRect>& monitors) {
    for (const auto& m : monitors) {
        if (m.primary) return &m;
    }
    return monitors.empty() ? nullptr : &monitors[0];
}

std::vector<Segment> ComputeSegments(const std::vector<MonitorRect>& monitors,
                                     DWORD edgeMask) {
    std::vector<Segment> segments;
    const MonitorRect* main = FindPrimary(monitors);
    if (!main) return segments;

    for (const auto& nb : monitors) {
        if (nb.handle == main->handle) continue;

        int lo = 0, hi = 0;

        // Neighbor sits to the LEFT of main: shares main's left edge line.
        if ((edgeMask & kEdgeLeftBit) && nb.rc.right == main->rc.left &&
            Overlap(main->rc.top, main->rc.bottom, nb.rc.top, nb.rc.bottom, &lo, &hi)) {
            Segment s{};
            s.edge = Edge::Left;
            s.strip = { main->rc.left, lo, main->rc.left + kStripWidthPx, hi };
            s.neighbor = nb.handle;
            s.neighborRc = nb.rc;
            segments.push_back(s);
        }
        // Neighbor sits to the RIGHT of main.
        if ((edgeMask & kEdgeRightBit) && nb.rc.left == main->rc.right &&
            Overlap(main->rc.top, main->rc.bottom, nb.rc.top, nb.rc.bottom, &lo, &hi)) {
            Segment s{};
            s.edge = Edge::Right;
            s.strip = { main->rc.right - kStripWidthPx, lo, main->rc.right, hi };
            s.neighbor = nb.handle;
            s.neighborRc = nb.rc;
            segments.push_back(s);
        }
        // Neighbor sits ABOVE main.
        if ((edgeMask & kEdgeTopBit) && nb.rc.bottom == main->rc.top &&
            Overlap(main->rc.left, main->rc.right, nb.rc.left, nb.rc.right, &lo, &hi)) {
            Segment s{};
            s.edge = Edge::Top;
            s.strip = { lo, main->rc.top, hi, main->rc.top + kStripWidthPx };
            s.neighbor = nb.handle;
            s.neighborRc = nb.rc;
            segments.push_back(s);
        }
        // Neighbor sits BELOW main.
        if ((edgeMask & kEdgeBottomBit) && nb.rc.top == main->rc.bottom &&
            Overlap(main->rc.left, main->rc.right, nb.rc.left, nb.rc.right, &lo, &hi)) {
            Segment s{};
            s.edge = Edge::Bottom;
            s.strip = { lo, main->rc.bottom - kStripWidthPx, hi, main->rc.bottom };
            s.neighbor = nb.handle;
            s.neighborRc = nb.rc;
            segments.push_back(s);
        }
    }
    return segments;
}

const wchar_t* EdgeName(Edge e) {
    switch (e) {
        case Edge::Left:   return L"left";
        case Edge::Right:  return L"right";
        case Edge::Top:    return L"top";
        case Edge::Bottom: return L"bottom";
    }
    return L"?";
}

std::wstring DescribeMonitor(const MonitorRect& m) {
    wchar_t buf[160];
    swprintf_s(buf, L"%s rc=(%ld,%ld,%ld,%ld) size=%ldx%ld%s",
               m.name,
               m.rc.left, m.rc.top, m.rc.right, m.rc.bottom,
               m.rc.right - m.rc.left, m.rc.bottom - m.rc.top,
               m.primary ? L" [PRIMARY]" : L"");
    return buf;
}

std::wstring DescribeSegment(const Segment& s) {
    wchar_t buf[200];
    swprintf_s(buf, L"%s-edge strip=(%ld,%ld,%ld,%ld) neighbor rc=(%ld,%ld,%ld,%ld)",
               EdgeName(s.edge),
               s.strip.left, s.strip.top, s.strip.right, s.strip.bottom,
               s.neighborRc.left, s.neighborRc.top,
               s.neighborRc.right, s.neighborRc.bottom);
    return buf;
}

std::vector<MonitorRelation> RelateMonitorsToMain(
    const std::vector<MonitorRect>& monitors) {
    const MonitorRect* main = FindPrimary(monitors);
    std::vector<MonitorRelation> out(monitors.size());

    // Display numbers: the primary is always 1; the rest follow reading
    // order (top to bottom, then left to right).
    std::vector<size_t> rest;
    for (size_t i = 0; i < monitors.size(); ++i) {
        if (!main || monitors[i].handle != main->handle) rest.push_back(i);
    }
    std::sort(rest.begin(), rest.end(), [&](size_t a, size_t b) {
        const RECT& ra = monitors[a].rc;
        const RECT& rb = monitors[b].rc;
        if (ra.top != rb.top) return ra.top < rb.top;
        return ra.left < rb.left;
    });

    for (size_t i = 0; i < monitors.size(); ++i) {
        out[i].main = main && monitors[i].handle == main->handle;
        out[i].number = out[i].main ? 1 : 2;
    }
    for (size_t k = 0; k < rest.size(); ++k) out[rest[k]].number =
        static_cast<int>(k) + 2;

    if (!main) return out;
    const long mcx = (main->rc.left + main->rc.right) / 2;
    const long mcy = (main->rc.top + main->rc.bottom) / 2;

    for (size_t i = 0; i < monitors.size(); ++i) {
        if (out[i].main) continue;
        const RECT& rc = monitors[i].rc;
        const long tcx = (rc.left + rc.right) / 2;
        const long tcy = (rc.top + rc.bottom) / 2;
        const long long dx = static_cast<long long>(tcx) - mcx;
        const long long dy = static_cast<long long>(tcy) - mcy;
        const long long adx = dx > 0 ? dx : -dx;
        const long long ady = dy > 0 ? dy : -dy;

        if (adx >= 2 * ady) {
            out[i].dir = dx > 0 ? MonitorDir::E : MonitorDir::W;
        } else if (ady >= 2 * adx) {
            out[i].dir = dy < 0 ? MonitorDir::N : MonitorDir::S;
        } else {
            const bool right = dx > 0;
            const bool up = dy < 0;
            out[i].dir = right ? (up ? MonitorDir::NE : MonitorDir::SE)
                               : (up ? MonitorDir::NW : MonitorDir::SW);
        }

        // Hops: monitors whose center lies inside the box spanned by the
        // two centers are "in between" and each adds one arrow.
        const long lox = mcx < tcx ? mcx : tcx, hix = mcx < tcx ? tcx : mcx;
        const long loy = mcy < tcy ? mcy : tcy, hiy = mcy < tcy ? tcy : mcy;
        int between = 0;
        for (size_t j = 0; j < monitors.size(); ++j) {
            if (j == i || out[j].main) continue;
            const RECT& o = monitors[j].rc;
            const long cx = (o.left + o.right) / 2;
            const long cy = (o.top + o.bottom) / 2;
            if (cx >= lox && cx <= hix && cy >= loy && cy <= hiy) ++between;
        }
        out[i].hops = 1 + between;
        if (out[i].hops > 3) out[i].hops = 3;
    }
    return out;
}

} // namespace edgepop
