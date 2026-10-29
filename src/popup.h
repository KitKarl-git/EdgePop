// EdgePop popup: the window list that
// appears when the trigger fires.
//
// Rendering is Direct2D drawn into a composition swapchain
// (DirectComposition + WS_EX_NOREDIRECTIONBITMAP): per-pixel alpha, rounded
// corners drawn as geometry, no activation. The window is
// WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST.
//
// The list pops OUT of the monitor edge with a short scale+fade (mac-dock
// feel,) driven entirely by the compositor: IDCompositionAnimation
// curves on a scale transform + visual opacity, committed once per show -
// no app-side timer. Closing is presence-based: a single
// 100 ms poll timer checks Esc and whether the cursor is still within the
// popup rect (inflated by leave_grace_px); the popup closes ~650 ms after
// the cursor last was inside - regardless of which side of the seam the
// cursor stopped on (fixes the neighbor-side instant-close bug).
//
// Live DWM thumbnails (phase 5): window rows render as cards (preview slot
// + title strip). Thumbnails are registered only while the popup is visible
// and only after the pop-in settles - the compositor animation does not move
// DWM-composited thumbnails - and the swapchain punches transparent holes
// (PRIMITIVE_BLEND_MIN) where they land. Minimized windows and failed
// registrations keep the classic icon+title row/slot placeholder. With
// "labels over previews" the preview region stops just above the label
// (DWM thumbnails composite above EVERY window, so
// nothing can be drawn over a live preview); the label sits unfilled at
// the preview's bottom edge.
//
// Close affordances: the popup's own close is a square tab that
// PROTRUDES above the panel's top edge - clearly chrome, never part of a
// card. Each card's close is a small rounded-square X riding the title
// strip at
// its far right, in line with the icon and app name (taskbar-flyout
// style) - a live DWM thumbnail composites above every window (gotcha
// 18), so the X must sit outside the preview pixels; the title
// ellipsizes short of it. Clicking it WM_CLOSEs that window and the
// popup rebuilds in place.
#pragma once

#include <windows.h>
#include <dwmapi.h>             // HTHUMBNAIL

#include <map>
#include <string>
#include <vector>

#include "geometry.h"
#include "settings.h"
#include "winenums.h"

namespace edgepop {

// Capture `hwnd`'s content into the popup's last-known snapshot cache.
// Called by the minimize-start WinEvent hook so windows
// keep a card snapshot after going down, taskbar-style.
void CacheWindowSnapshot(HWND hwnd);

// One labeled group of windows; label is empty for a single-monitor popup.
struct PopupSection {
    std::wstring label;
    std::vector<WindowItem> items;
};

class Popup {
public:
    static Popup& Instance();

    // Route renderer diagnostics into the app log (call once at startup).
    static void SetLogSink(void (*fn)(const wchar_t*));

    // Content reconcile after a card close: the popup asks the app
    // to re-enumerate its segment a beat after a window was closed - a
    // confirmation dialog can veto the WM_CLOSE and bring the card back.
    static void SetRefreshSink(void (*fn)(const Segment& segment));

    void RegisterClass(HINSTANCE inst);
    bool Create(HINSTANCE inst);   // also prewarms the render stack

    // Shows (or re-shows) the popup for a fired segment.
    void Show(const Segment& segment, const MonitorRect& main,
              const std::vector<PopupSection>& sections, POINT cursor,
              const Settings& s);
    // Replaces the content while visible: same position, no pop-in.
    void Refresh(const std::vector<PopupSection>& sections);
    void Hide();
    bool Visible() const { return hwnd_ != nullptr && visible_; }
    HWND hwnd() const { return hwnd_; }

    // Mouse-down anywhere outside the popup closes it (fed by the LL hook).
    void OnOutsideMouseDown(POINT pt);

    // Wheel notches while the popup is up (fed by the LL hook - a
    // NOACTIVATE popup never has focus, so the OS routes the wheel to
    // some other window; the hook is the reliable path,). Scrolls
    // the overflowed list; ignores events outside the popup.
    void OnMouseWheel(int delta);

    // True while the trigger is paused after the user dismissed the popup
    // with its X (or Esc) - "delay activation after close".
    bool Suppressed() const;

private:
    Popup() = default;

    struct RowInfo {
        bool isHeader = false;
        bool card = false;               // thumbnail card row (phase 5)
        int item = -1;                   // index into items_ (headers: -1)
        std::wstring label;              // header text
        RECT rc{};                       // client coords, filled at render
        RECT thumbSlot{};                // preview region in the card (client px)
        RECT thumbDest{};                // letterboxed DWM thumbnail rect
        bool thumbLive = false;          // thumbnail registered, hole punched
        RECT xCard{};                    // per-card close hit rect; empty when
                                         // off or not a card
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    void EnsureRenderStack();       // D3D11/DXGI/D2D/DWrite/DComp, once
    void Render();                  // full redraw at current size
    float CardSlotH(float popupW) const;  // preview slot height
    // Width sizing: cards live at card_width (floored 230, capped
    // 480); the icon+title list caps at max_width. NaturalWidth is the
    // single content-measuring pass shared by Show and RebuildInPlace -
    // the two hand-copied loops had drifted apart (icons-only rows were
    // only measured in Show).
    float CardWidthOf(const Settings& s) const;
    float ModeWidthOf(const Settings& s) const;   // the mode's width cap
    float NaturalWidth() const;                   // content width, uncapped
    int ItemFromPoint(POINT pt) const;
    void ActivateItem(int item);
    void UpdateHover(int row);
    void UpdateHoverX(bool over);   // close (X) button hover
    void UpdateCardXHover(int row); // per-card close hover
    void CloseCard(int row);        // WM_CLOSE the row's window, rebuild
    void RebuildInPlace();          // re-layout from sections_, no pop-in
    void RecomputeHover();          // hover from the live cursor (post-rebuild)
    void DismissSuppressed();       // X/Esc close: pause the trigger
    void ApplyAnimStart();          // invisible start state, pre-show
    void StartAnimation();          // compositor-driven pop-out
    void RegisterThumbnails();      // live previews, after the pop-in settles
    void UnregisterThumbnails();
    void UpdateHoverGhosting();     // peek-on-hover: hide the others
    void VerifyForeground();        // deferred switch check

    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    bool visible_ = false;
    bool stackOk_ = false;

    Segment segment_{};
    MonitorRect main_{};
    std::vector<PopupSection> sections_;  // what Show/Refresh last built from
    std::vector<WindowItem> items_;   // flat, across sections
    std::vector<RowInfo> rows_;
    int hoverRow_ = -1;
    bool hoverX_ = false;             // close (X) button hover state
    int hoverCardX_ = -1;             // row whose card close is hovered
    RECT xRc_{};                      // close button hit rect (client px)
    Settings settings_{};
    float scale_ = 1.0f;
    float scrollOff_ = 0.0f;          // overflow scroll, client px
    float maxScroll_ = 0.0f;          // 0 = everything fits, no scrolling
    int wheelCarry_ = 0;              // sub-notch wheel remainder (touchpads)

    ULONGLONG lastInsideTick_ = 0;    // presence-based closing
    ULONGLONG showTick_ = 0;          // when this popup appeared (thumb delay)
    ULONGLONG stayAnchorTick_ = 0;    // stay time counts from the last time
                                      // the popup was hovered
    ULONGLONG suppressUntil_ = 0;     // post-dismiss trigger pause
    ULONGLONG reconcileAtTick_ = 0;   // card-close content reconcile

    std::map<HWND, HTHUMBNAIL> thumbs_;
    bool thumbsPending_ = false;      // waiting for the pop-in to settle
    HWND verifyTarget_ = nullptr;     // switch pending verification

    // pop-out anchor: seam-facing edge midpoint (client px) + which axis
    // extrudes
    POINT animAnchorClient_{};
    bool animVerticalSeam_ = true;
};

} // namespace edgepop
