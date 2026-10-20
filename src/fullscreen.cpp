#include "fullscreen.h"

#include <dwmapi.h>

namespace edgepop {

bool IsFullscreenOnMonitor(const RECT& monRc) {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindowVisible(fg)) return false;

    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    // Must fully contain the monitor (games can overshoot by a pixel or two).
    if (wr.left > monRc.left || wr.top > monRc.top ||
        wr.right < monRc.right || wr.bottom < monRc.bottom) {
        return false;
    }

    const LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
    if ((style & (WS_CAPTION | WS_THICKFRAME)) != 0) return false;

    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) &&
        cloaked != 0) {
        return false;
    }
    return true;
}

} // namespace edgepop
