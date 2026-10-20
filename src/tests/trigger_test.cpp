// Unit tests for the EdgeZoneTrigger state machine.
//
// Drives ProcessPoint directly with synthetic cursor positions and real
// (short) sleeps for time - no hooks, no windows, no injected input. This is
// the deterministic verification of "stopped near the seam fires, everything
// else doesn't" that synthetic-input E2E testing couldn't provide on this
// host (the low-level hook never sees SendInput from a sandboxed shell).
//
// Zone under test matches the fixture: strip (2558,9,2560,1089), zone
// inflated by 20 px -> x in [2538,2580), stop radius 8.

#include <cstdio>
#include <string>
#include <vector>

#include "geometry.h"
#include "trigger.h"

namespace {

int g_fires = 0;
unsigned g_testDwellMs = 250;
int g_testMode = 0;              // 0 = edge dwell, 1 = cross-back
unsigned g_testCrossMs = 600;

void CountFire(size_t) { ++g_fires; }
unsigned Dwell() { return g_testDwellMs; }
int ActMode() { return g_testMode; }
unsigned CrossMs() { return g_testCrossMs; }

// The drag check reads real mouse buttons; this suite runs on my live
// desktop, so his clicks used to leak in and randomly suppress fires.
bool NoButtonsHeld() { return false; }

edgepop::Segment MakeFixtureSegment() {
    edgepop::Segment s{};
    s.edge = edgepop::Edge::Right;
    s.strip = { 2558, 9, 2560, 1089 };
    s.neighborRc = { 2560, 9, 4480, 1089 };
    return s;
}

int g_failures = 0;

void Check(const char* name, bool ok) {
    printf("%-52s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}

void Move(edgepop::EdgeZoneTrigger& t, int x, int y) {
    t.ProcessPoint({ x, y }, false);   // hook-path semantics
}

void DwellCheck(edgepop::EdgeZoneTrigger& t, int x, int y) {
    t.ProcessPoint({ x, y }, true);    // timer-path semantics
}

// Decelerating approach like a human flinging toward the edge.
void ApproachAndStop(edgepop::EdgeZoneTrigger& t, int endX, int y = 600) {
    int x = 2000;
    while (endX - x > 1) {
        x += (endX - x) / 2;
        Move(t, x, y);
        Sleep(35);
    }
    Move(t, endX, y);
}

} // namespace

int main() {
    edgepop::EdgeZoneTrigger t;
    t.SetCallbacks(&CountFire, &Dwell);
    t.SetActivationConfig(&ActMode, &CrossMs);
    t.SetButtonsHeldQuery(&NoButtonsHeld);

    std::vector<edgepop::Segment> segments = { MakeFixtureSegment() };
    t.UpdateZones(segments, 20);
    t.SetStopRadius(8);

    // 1. Stop inside the zone (main side, 15 px short of the seam) fires.
    ApproachAndStop(t, 2545);
    Sleep(300);
    DwellCheck(t, 2545, 600);
    Check("stop at x=2545 (in zone, main side) fires", g_fires == 1);

    // 2. One fire per stay: further dwell checks don't re-fire.
    DwellCheck(t, 2545, 600);
    DwellCheck(t, 2546, 600);
    Check("no double fire while staying in zone", g_fires == 1);

    // 3. Leaving and returning re-arms.
    Move(t, 1500, 600);
    Sleep(50);
    ApproachAndStop(t, 2562);
    Sleep(300);
    DwellCheck(t, 2562, 600);
    Check("stop at x=2562 (in zone, neighbor side) fires", g_fires == 2);

    // 4. Stop short of the zone never fires.
    Move(t, 1500, 600);
    Sleep(50);
    ApproachAndStop(t, 2530);
    Sleep(300);
    DwellCheck(t, 2530, 600);
    Check("stop at x=2530 (outside zone) does not fire", g_fires == 2);

    // 5. Stop deep past the zone does not fire.
    Move(t, 1500, 600);
    Sleep(50);
    ApproachAndStop(t, 2600);
    Sleep(300);
    DwellCheck(t, 2600, 600);
    Check("stop at x=2600 (past zone) does not fire", g_fires == 2);

    // 6. Fast pass-through never fires: cross the zone with big steps and no
    //    stop; dwell checks see movement and keep restarting the timer.
    Move(t, 1500, 600);
    Sleep(50);
    for (int x = 2500; x <= 2620; x += 40) {
        Move(t, x, 600);
        DwellCheck(t, x, 600);   // timer runs during transit
        Sleep(20);
    }
    Sleep(400);
    Check("fast pass-through does not fire", g_fires == 2);

    // 7. Slow creep through the zone with sub-radius steps but brief total
    //    transit (< dwell) does not fire.
    Move(t, 1500, 600);
    Sleep(50);
    for (int x = 2540; x <= 2576; x += 3) {
        Move(t, x, 600);
        DwellCheck(t, x, 600);
        Sleep(15);   // ~180ms total transit < 250ms dwell
    }
    Move(t, 2650, 600);
    Sleep(50);
    Check("sub-dwell creep through zone does not fire", g_fires == 2);

    // 8. Zero dwell (instant mode) fires on the first timer check after
    //    stopping, still requiring a stop (movement keeps deferring).
    g_testDwellMs = 0;
    Move(t, 1500, 600);
    Sleep(50);
    ApproachAndStop(t, 2555);
    DwellCheck(t, 2555, 600);
    Check("dwell=0 fires on first stop check", g_fires == 3);
    g_testDwellMs = 250;

    // 9. Zone tracks a rebuilt segment list (display change).
    t.UpdateZones({}, 20);
    Move(t, 2545, 600);
    Sleep(300);
    DwellCheck(t, 2545, 600);
    Check("empty segment list disables firing", g_fires == 3);
    t.UpdateZones(segments, 20);

    // --- cross-back activation mode ---
    g_testMode = 1;
    const int baseFires = g_fires;

    // 10. Cross and return within the window fires.
    Move(t, 2500, 600);          // main side
    Move(t, 2562, 600);          // crossed over
    Sleep(100);
    Move(t, 2540, 600);          // returned
    Check("cross & return within window fires", g_fires == baseFires + 1);

    // 10b. Starting from the NEIGHBOR side works identically :
    //      arming is direction-agnostic).
    Move(t, 2600, 620);          // neighbor side
    Move(t, 2530, 620);          // crossed to main
    Sleep(80);
    Move(t, 2600, 620);          // returned to neighbor
    Check("cross & return from neighbor side fires",
          g_fires == baseFires + 2);
    Move(t, 2600, 5000);         // leave corridor: reset crossing state

    // 11. Return too late does not fire.
    Move(t, 2500, 700);
    Move(t, 2580, 700);
    Sleep(900);                  // > crossBackMs
    Move(t, 2540, 700);
    Check("late return does not fire", g_fires == baseFires + 2);

    // 12. Merely stopping on either side does not fire (no dwell in this
    //     mode - that's the point of it).
    Move(t, 2545, 600);
    Sleep(400);
    DwellCheck(t, 2545, 600);
    Check("no dwell firing in cross-back mode", g_fires == baseFires + 2);
    Move(t, 2545, 5000);         // leave corridor: reset crossing state

    // 13. Deep cross still counts: crossing far past the zone, returning.
    Move(t, 2500, 500);
    Move(t, 2700, 500);          // deep into the neighbor
    Sleep(80);
    Move(t, 2530, 500);          // back
    Check("deep cross & return fires", g_fires == baseFires + 3);

    // 14. Leaving the corridor resets the crossing.
    Move(t, 2500, 600);
    Move(t, 2562, 600);
    Move(t, 2562, 5000);         // out of corridor (y far away)
    Sleep(50);
    Move(t, 2540, 600);          // return in a different corridor pass
    Check("corridor exit resets crossing", g_fires == baseFires + 3);

    g_testMode = 0;

    // --- RelateMonitorsToMain: taskbar arrow/hop/number math ------
    {
        using edgepop::MonitorDir;
        using edgepop::MonitorRect;
        // my live layout: primary left, two stacked to the right.
        std::vector<MonitorRect> mons;
        MonitorRect prim{}; prim.handle = (HMONITOR)1;
        prim.rc = { 0, 0, 2560, 1440 }; prim.primary = true;
        MonitorRect upper{}; upper.handle = (HMONITOR)2;
        upper.rc = { 2560, 9, 4480, 1089 };
        MonitorRect lower{}; lower.handle = (HMONITOR)3;
        lower.rc = { 2560, 1089, 3840, 1809 };
        mons = { prim, upper, lower };
        auto rel = edgepop::RelateMonitorsToMain(mons);
        Check("relate: primary flagged", rel[0].main);
        Check("relate: primary number is 1", rel[0].number == 1);
        Check("relate: upper-right is E", rel[1].dir == MonitorDir::E);
        Check("relate: lower-right is E", rel[2].dir == MonitorDir::E);
        Check("relate: adjacent = 1 hop each", rel[1].hops == 1 && rel[2].hops == 1);
        Check("relate: reading order numbers 2,3",
              rel[1].number == 2 && rel[2].number == 3);

        // Three in a row: the far one is two hops. ("far" is a minwindef.h
        // macro - same trap class as `small`,.)
        MonitorRect mid{}; mid.handle = (HMONITOR)4;
        mid.rc = { 2560, 0, 5120, 1440 };
        MonitorRect farRight{}; farRight.handle = (HMONITOR)5;
        farRight.rc = { 5120, 0, 7680, 1440 };
        rel = edgepop::RelateMonitorsToMain({ prim, mid, farRight });
        Check("relate: row adjacent = 1 hop", rel[1].hops == 1);
        Check("relate: row far = 2 hops", rel[2].hops == 2);

        // Directly above: N, one hop.
        MonitorRect above{}; above.handle = (HMONITOR)6;
        above.rc = { 0, -1440, 2560, 0 };
        rel = edgepop::RelateMonitorsToMain({ prim, above });
        Check("relate: monitor above is N", rel[1].dir == MonitorDir::N);
        Check("relate: above = 1 hop", rel[1].hops == 1);

        // Diagonal: dx and dy comparable -> NE.
        MonitorRect diag{}; diag.handle = (HMONITOR)7;
        diag.rc = { 2560, -1440, 5120, 0 };
        rel = edgepop::RelateMonitorsToMain({ prim, diag });
        Check("relate: diagonal is NE", rel[1].dir == MonitorDir::NE);

        // Monitor below: S.
        MonitorRect below{}; below.handle = (HMONITOR)8;
        below.rc = { 0, 1440, 2560, 2880 };
        rel = edgepop::RelateMonitorsToMain({ prim, below });
        Check("relate: monitor below is S", rel[1].dir == MonitorDir::S);
    }

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL TESTS PASS" : "TESTS FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
