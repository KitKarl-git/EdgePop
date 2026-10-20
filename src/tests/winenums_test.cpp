// Unit tests for the Alt-Tab window filter.

#include <cstdio>

#include "winenums.h"

namespace {

int g_failures = 0;

void Check(const char* name, bool ok) {
    printf("%-52s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}

edgepop::WindowTraits Good() {
    edgepop::WindowTraits t;
    t.visible = true;
    t.hasTitle = true;
    return t;
}

} // namespace

int main() {
    using edgepop::ShouldListWindow;

    Check("normal window is listed", ShouldListWindow(Good()));

    edgepop::WindowTraits t;
    t = Good(); t.visible = false;
    Check("invisible window not listed", !ShouldListWindow(t));
    t = Good(); t.hasTitle = false;
    Check("untitled window not listed", !ShouldListWindow(t));
    t = Good(); t.toolWindow = true;
    Check("tool window not listed", !ShouldListWindow(t));
    t = Good(); t.owned = true;
    Check("owned/dialog window not listed", !ShouldListWindow(t));
    t = Good(); t.cloaked = true;
    Check("cloaked UWP ghost not listed", !ShouldListWindow(t));
    t = Good(); t.shellChrome = true;
    Check("shell chrome (taskbar/progman) not listed", !ShouldListWindow(t));

    t = Good(); t.visible = false; t.cloaked = true;
    Check("multiple disqualifiers not listed", !ShouldListWindow(t));

    printf("\n%s (%d failure%s)\n",
           g_failures == 0 ? "ALL TESTS PASS" : "TESTS FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
