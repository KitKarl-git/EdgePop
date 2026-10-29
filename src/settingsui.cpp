// EdgePOP by KitKarl - settings window + About
// Native controls (checks, radios, buttons, sliders) are repainted
// in the VideoSquisher teal; the dialogs carry the app icon and the
// kitkarl.com links.
//
// Look and behavior follow VideoSquisher's help pattern: every setting has
// a small round (?) that hugs the end of its label text (repositioned at
// runtime by measuring the label), shows a tooltip on hover, and opens an
// explanation window on click.
//
//: activation is picked with a two-segment pill toggle; below it one
// settings box whose rows swap with the selected mode (the whole dialog
// reflows - the below-block slides up and the dialog shrinks). Edges are
// Auto OR sides (mutually exclusive radios). Icons/titles can never both
// be off - the popup must always show something.

#include "settingsui.h"

#include <commctrl.h>
#include <commdlg.h>   // ChooseColor
#include <cstdio>
#include <cwchar>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>   // ShellExecuteW (kitkarl.com links)
#include <dwmapi.h>     // DWMWA_WINDOW_CORNER_PREFERENCE

#include <algorithm>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "geometry.h"   // edge mask bits
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")

namespace edgepop {
namespace {

using Microsoft::WRL::ComPtr;

// VideoSquisher palette (light variant),.
constexpr COLORREF kColBg      = RGB(0xFA, 0xF5, 0xEF);   // BG
constexpr COLORREF kColCard    = RGB(0xFF, 0xFF, 0xFF);   // CARD
constexpr COLORREF kColBorder  = RGB(0xE7, 0xDF, 0xD6);   // BORDER
constexpr COLORREF kColText    = RGB(0x3D, 0x37, 0x33);   // TEXT
constexpr COLORREF kColSoft    = RGB(0x8A, 0x7F, 0x76);   // TEXT_SOFT
constexpr COLORREF kColAccent  = RGB(0x2A, 0x9D, 0x8F);   // ACCENT
constexpr COLORREF kColAccDark = RGB(0x21, 0x86, 0x7A);   // ACCENT_DARK
constexpr COLORREF kColAccLight = RGB(0xE4, 0xF2, 0xF0);  // ACCENT_LIGHT

constexpr D2D1_COLOR_F kDBg     = {0xFA / 255.0f, 0xF5 / 255.0f, 0xEF / 255.0f, 1};
constexpr D2D1_COLOR_F kDCard  = {1, 1, 1, 1};
constexpr D2D1_COLOR_F kDBorder = {0xE7 / 255.0f, 0xDF / 255.0f, 0xD6 / 255.0f, 1};
constexpr D2D1_COLOR_F kDText   = {0x3D / 255.0f, 0x37 / 255.0f, 0x33 / 255.0f, 1};
constexpr D2D1_COLOR_F kDSoft   = {0x8A / 255.0f, 0x7F / 255.0f, 0x76 / 255.0f, 1};
constexpr D2D1_COLOR_F kDAccent = {0x2A / 255.0f, 0x9D / 255.0f, 0x8F / 255.0f, 1};
constexpr D2D1_COLOR_F kDAccDark = {0x21 / 255.0f, 0x86 / 255.0f, 0x7A / 255.0f, 1};
constexpr D2D1_COLOR_F kDAccLight = {0xE4 / 255.0f, 0xF2 / 255.0f, 0xF0 / 255.0f, 1};

// (?) help data: tooltip on hover, full text on click.
struct HelpEntry {
    int id;
    const wchar_t* tip;    // hover tooltip (one line)
    const wchar_t* help;   // click window (full explanation)
};

const HelpEntry kHelp[] = {
    { IDC_QQ_ACT,
      L"How the popup is triggered - pick a mode; its settings show below.",
      L"Activation\n\n"
      L"Stop at edge: move the cursor to the border between monitors and "
      L"stop; after the dwell time the popup appears.\n\n"
      L"Cross & return: cross over to the other monitor and come back "
      L"within the cross window - more deliberate, it never fires just "
      L"because you paused near the edge.\n\n"
      L"The box below shows the settings of the selected mode." },
    { IDC_QQ_CROSS,
      L"How long you have to come back after crossing (Cross & return).",
      L"Cross window\n\n"
      L"In Cross & return mode: the maximum time between crossing the "
      L"edge and coming back for the popup to appear. Shorter = more "
      L"deliberate; longer = easier to trigger." },
    { IDC_QQ_DWELL,
      L"How long the cursor must stay put near the edge (Stop at edge).",
      L"Dwell\n\n"
      L"In Stop at edge mode: how long the cursor must remain (nearly) "
      L"still near the border before the popup appears. 0 = instant, at "
      L"the cost of popping whenever you pause at the edge." },
    { IDC_QQ_ZONE,
      L"How close to the border counts as \"at the edge\" (each side).",
      L"Zone width\n\n"
      L"In Stop at edge mode: how far from the seam (on either side) "
      L"still counts as being \"at the edge\". Bigger is easier to hit; "
      L"smaller reduces popups while working near the border." },
    { IDC_QQ_STOP,
      L"Wobble tolerance: movement smaller than this counts as stopped.",
      L"Stop radius\n\n"
      L"In Stop at edge mode: cursor movement smaller than this radius "
      L"counts as \"stopped\". If the popup feels hard to trigger, "
      L"increase this - hands wobble while settling." },
    { IDC_QQ_REDELAY,
      L"How long the trigger stays off after you close the popup (X or Esc).",
      L"Delay activation after close\n\n"
      L"After you dismiss the popup with its close (X) button or Esc, "
      L"the trigger stays off for this many seconds - an accidentally "
      L"fired popup won't immediately come back. 0 turns the pause off." },
    { IDC_QQ_FGS,
      L"Suppress the popup over full-screen games and video.",
      L"Full-screen guard\n\n"
      L"While a borderless full-screen app (game, video player) is in the "
      L"foreground on either monitor of a pair, the popup will not "
      L"appear for that edge. Maximized normal windows do not count." },
    { IDC_QQ_EDGES,
      L"Which edges of the main monitor can trigger - Auto or sides.",
      L"Edges\n\n"
      L"Auto: every edge of the main monitor that touches another "
      L"monitor gets a trigger zone.\n\n"
      L"Sides: use only the edges you check (Left, Right, Top, Bottom).\n\n"
      L"Auto and sides are mutually exclusive - Auto covers everything, "
      L"Sides is for when you want fewer triggers." },
    { IDC_QQ_SIDE,
      L"Which monitor the popup appears on.",
      L"Popup side\n\n"
      L"Main monitor: the popup appears on your main monitor's side of "
      L"the border (default).\n\n"
      L"Neighbor: it appears on the other monitor's side, next to the "
      L"seam." },
    { IDC_QQ_CHAIN,
      L"What to list when monitors continue in the same direction.",
      L"Chain mode\n\n"
      L"Neighbor only: list windows from the monitor directly across "
      L"the seam.\n\n"
      L"All in direction: list windows from every monitor continuing in "
      L"that direction, grouped under per-monitor headers." },
    { IDC_QQ_GRACE,
      L"How far past the popup edge the cursor may wander.",
      L"Leave grace\n\n"
      L"The popup closes shortly after the cursor leaves it. This "
      L"extends the tolerated distance beyond the popup's edge, so "
      L"small overshoots don't close it." },
    { IDC_QQ_STAY,
      L"Minimum seconds the popup stays up, wherever the cursor is.",
      L"Stay time\n\n"
      L"With cross-and-return the cursor often ends up far from the "
      L"popup (back on the monitor it came from). This keeps the popup "
      L"up for at least this many seconds no matter where the cursor "
      L"goes - and the window resets every time you hover the popup, so "
      L"after you move off it lingers a fresh full stay time. "
      L"Clicking a row, the X, or Esc still closes it immediately. "
      L"0 = close purely by cursor presence." },
    { IDC_QQ_TITLE,
      L"Maximum width of the icon+title list popup.",
      L"List width\n\n"
      L"For the list popup (thumbnails off): it is always as narrow as "
      L"its content allows - short titles make a narrow popup, icons-only "
      L"makes it very narrow (with a reasonable minimum). This is the "
      L"ceiling: anything wider than this cuts titles off with an "
      L"ellipsis. Hard range 120-800." },
    { IDC_QQ_CARDW,
      L"The width of the thumbnail-card popup.",
      L"Card width\n\n"
      L"For the thumbnail popup: how wide the preview cards are, "
      L"independent of the list width above. Bigger cards mean bigger "
      L"previews - the preview height grows with the width. Hard range "
      L"230-400." },
    { IDC_QQ_SHOW,
      L"What each popup row shows.",
      L"Row contents\n\n"
      L"Icons, titles and thumbnails can be turned off independently, "
      L"but at least one of the three always stays on - the popup never "
      L"goes completely blank. Thumbnails add a live window preview to "
      L"each entry; minimized windows keep their card with the last "
      L"known snapshot of the window (like the taskbar)." },
    { IDC_QQ_EXCLUDE,
      L"Apps that never appear in the popup.",
      L"Exclude apps\n\n"
      L"Comma-separated exe names of processes whose windows are never "
      L"listed - background helpers, splash screens, updaters, anything "
      L"you switch to by other means. Matched case-insensitively.\n\n"
      L"Example: steam.exe, Discord.exe" },
    { IDC_QQ_PEEK,
      L"Hovering an entry hides the other windows.",
      L"Peek on hover\n\n"
      L"Like the taskbar's thumbnail preview: while you hover an entry, "
      L"every other window on that entry's monitor hides, leaving only "
      L"the hovered window visible on the desktop. Everything returns "
      L"the moment the hover moves off or the popup closes." },
    { IDC_QQ_CLOSEBTN,
      L"A close X floating above each preview's corner.",
      L"Close buttons on cards\n\n"
      L"Adds a small round X in a band above each preview's top-right "
      L"corner - click it to close that window. Previews stay live with "
      L"the buttons on or off: a live preview can never have a button "
      L"painted on it, so the X sits just above it. The big square X "
      L"above the list only ever closes the popup itself." },
    { IDC_QQ_VD,
      L"List windows from the current virtual desktop only, or all.",
      L"Virtual desktops\n\n"
      L"Current: only windows on the virtual desktop you're using "
      L"(same as Alt-Tab).\n\n"
      L"All: windows from every virtual desktop." },
    { IDC_QQ_START,
      L"Start EdgePOP automatically when you sign in.",
      L"Start with Windows\n\n"
      L"Adds EdgePOP to the registry's Run key (current user only - "
      L"no admin rights needed). Turning it off removes the entry." },
    { IDC_QQ_ENABLE,
      L"Master switch for the edge popup (also in the tray menu).",
      L"Activate edge popup\n\n"
      L"The master switch for the whole edge popup - stop at edge, "
      L"cross & return, both modes. When off, every setting in the Edge "
      L"Popup box below greys out and hovering or crossing the monitor "
      L"borders never opens the popup; everything else (settings, "
      L"taskbar indicators) keeps working. Also toggleable from the "
      L"tray icon menu (Enabled)." },
    { IDC_QQ_TB_ARROWS,
      L"Master switch for the taskbar screen indicators.",
      L"Activate arrows on taskbar\n\n"
      L"The master switch for ALL taskbar screen indicators - the "
      L"persistent ones and the hover ones. When off, everything in the "
      L"box below greys out and no indicators are drawn." },
    { IDC_QQ_TB_PERSIST,
      L"What the always-on indicators show under each running item.",
      L"Persistent indicators\n\n"
      L"Always-on indicators resting on the taskbar's edge under every "
      L"running item (they point at the monitor that window is on): "
      L"Numbers (the monitor's number; 1 is always the main monitor) and "
      L"Arrows (one arrow = the adjacent monitor in that direction, two "
      L"= one further), their size, and whether they get a capsule "
      L"background. Background off draws bare glyphs with just the drop "
      L"shadow - the subtle notch look.\n\n"
      L"While the cursor is over the taskbar, the hover indicators "
      L"replace these." },
    { IDC_QQ_TB_PSIZE,
      L"The persistent indicators' glyph size (5-16 px).",
      L"Persistent size\n\n"
      L"The size of the persistent indicators' glyphs in pixels, 5-16. "
      L"Bigger is easier to see; smaller stays out of the way." },
    { IDC_QQ_TB_PBG,
      L"Whether the persistent indicators get a capsule background.",
      L"Persistent background\n\n"
      L"On: each indicator gets a dark capsule, easy to read on any "
      L"wallpaper.\n"
      L"Off: bare glyphs (plus the monitor number if enabled) with just "
      L"the drop shadow - subtle notches on the taskbar edge." },
    { IDC_QQ_TB_HSIZE,
      L"The hover indicators' glyph size (5-16 px).",
      L"Hover size\n\n"
      L"The size of the hover indicators' glyphs in pixels, 5-16 - "
      L"independent of the persistent size." },
    { IDC_QQ_TB_HBG,
      L"Whether the hover indicators get a capsule background.",
      L"Hover background\n\n"
      L"On: each indicator gets a dark capsule (default - they float "
      L"over arbitrary windows while lifted above the previews).\n"
      L"Off: bare glyphs with the drop shadow." },
    { IDC_QQ_TB_SHAPE,
      L"The glyph style: triangle, chevron, or arrow.",
      L"Shape\n\n"
      L"Triangle: a solid filled triangle.\n"
      L"Chevron: an open angle, like >.\n"
      L"Arrow: a shaft with a head, like ->.\n\n"
      L"Same size, position, color and shadow rules apply to all three; "
      L"pick what reads fastest for you." },
    { IDC_QQ_TB_SPOT,
      L"Where the persistent indicators anchor on a taskbar item.",
      L"Position\n\n"
      L"The persistent indicators anchor at the taskbar ITEM (the whole "
      L"button, icon + label): its left edge, its center, or its right "
      L"edge - all resting just above the taskbar's top edge. (Windows "
      L"composites the taskbar above every overlay window, so nothing "
      L"can paint on the taskbar itself.)" },
    { IDC_QQ_TBCOLOR,
      L"Glyph color, and a drop shadow for contrast.",
      L"Color & shadow\n\n"
      L"Color: any color for the arrow glyphs - pick something that "
      L"stands out against your wallpaper.\n\n"
      L"Shadow: draws a dark drop shadow behind each capsule, so the "
      L"indicators stay visible even on a light background." },
    { IDC_QQ_TB_HOVER,
      L"Indicators while the cursor is over the taskbar.",
      L"Hover indicators\n\n"
      L"While the cursor is anywhere over the taskbar, every running "
      L"item gets its indicator lifted above the taskbar (replacing "
      L"the persistent ones while you hover). Both can be on at once:\n\n"
      L"Numbers: the monitor's number (1 is always the main monitor, "
      L"the rest in reading order).\n"
      L"Arrows: the same direction glyphs.\n\n"
      L"When Windows shows its own thumbnail previews above the "
      L"taskbar, the hover indicators rise above the previews (the "
      L"previews draw over everything, so the indicators hop over "
      L"them). Both unchecked = nothing while hovering." },
};

bool IsHelpId(int id) {
    for (const auto& e : kHelp) if (e.id == id) return true;
    return false;
}

HWND g_tips = nullptr;
void (*g_advanced)(int action) = nullptr;   // 1 = rebuild strips

// One background brush for the cream dialogs (created lazily, never freed
// - process lifetime).
HBRUSH BgBrush() {
    static HBRUSH brush = nullptr;
    if (!brush) brush = CreateSolidBrush(kColBg);
    return brush;
}

// --- (?) help card ----------------------------------------------------
//
// A small borderless card beside the (?) - NOT a MessageBox: no system
// sound, no taskbar entry, no OK button. One at a time; closes on any
// click, on its 20 s timer, or with its owner dialog.

// Defined further down (control painting / About sections).
int CtrlDpi(HWND c);
HFONT TitleFont(HWND dlg);
void PreferRoundedCorners(HWND wnd);

constexpr UINT kHelpCloseTimer = 1;
HWND g_helpWnd = nullptr;

struct HelpCard {
    const wchar_t* title;
    const wchar_t* body;
};

INT_PTR CALLBACK HelpCardProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            const auto* card = reinterpret_cast<const HelpCard*>(lp);
            SetDlgItemTextW(dlg, IDC_ST_HELP_TITLE, card->title);
            SetDlgItemTextW(dlg, IDC_ST_HELP_BODY, card->body);
            SendMessageW(GetDlgItem(dlg, IDC_ST_HELP_TITLE), WM_SETFONT,
                         reinterpret_cast<WPARAM>(TitleFont(dlg)), TRUE);
            PreferRoundedCorners(dlg);
            SetTimer(dlg, kHelpCloseTimer, 20000, nullptr);
            return TRUE;
        }
        case WM_TIMER:
            if (wp == kHelpCloseTimer) DestroyWindow(dlg);
            return TRUE;
        case WM_LBUTTONDOWN:
            // Statics are HTTRANSPARENT, so any click in the card lands
            // here - the whole card is the close button.
            DestroyWindow(dlg);
            return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(dlg, &ps);
            RECT rc;
            GetClientRect(dlg, &rc);
            FillRect(dc, &rc, BgBrush());
            HBRUSH frame = CreateSolidBrush(kColBorder);
            FrameRect(dc, &rc, frame);
            DeleteObject(frame);
            EndPaint(dlg, &ps);
            return TRUE;
        }
        case WM_CTLCOLORDLG:
            SetBkColor(reinterpret_cast<HDC>(wp), kColBg);
            return reinterpret_cast<INT_PTR>(BgBrush());
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(wp);
            SetBkColor(dc, kColBg);
            SetTextColor(dc, reinterpret_cast<HWND>(lp) ==
                    GetDlgItem(dlg, IDC_ST_HELP_TITLE) ? kColAccDark
                                                       : kColText);
            return reinterpret_cast<INT_PTR>(BgBrush());
        }
        case WM_DESTROY:
            g_helpWnd = nullptr;
            return TRUE;
    }
    return FALSE;
}

void ShowHelpWindow(HWND dlg, int id) {
    const HelpEntry* entry = nullptr;
    for (const auto& e : kHelp) {
        if (e.id == id) {
            entry = &e;
            break;
        }
    }
    if (!entry) return;
    if (g_helpWnd) {
        // A stale handle must not be re-destroyed (the outside-click path
        // may have closed the card already), and re-clicking the SAME (?)
        // toggles: with the LL hook eating mouse-downs to close the card,
        // blindly recreating on the up-click read as "(?) does nothing"
        //.
        if (!IsWindow(g_helpWnd)) {
            g_helpWnd = nullptr;
        } else if (GetWindowLongPtrW(g_helpWnd, GWLP_USERDATA) == id) {
            DestroyWindow(g_helpWnd);
            return;
        } else {
            DestroyWindow(g_helpWnd);
        }
    }

    // "Title\n\nBody..." -> card title + wrapped body.
    const wchar_t* sep = wcsstr(entry->help, L"\n\n");
    const wchar_t* body = sep ? sep + 2 : entry->help;
    wchar_t title[96]{};
    if (sep && sep - entry->help < 96) {
        wcsncpy_s(title, entry->help,
                  static_cast<size_t>(sep - entry->help));
    } else {
        wcscpy_s(title, L"Help");
    }
    HelpCard card{ title, body };

    HWND card2 = CreateDialogParamW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_HELP), dlg,
        HelpCardProc, reinterpret_cast<LPARAM>(&card));
    if (!card2) return;
    SetWindowLongPtrW(card2, GWLP_USERDATA, id);   // toggle check above
    g_helpWnd = card2;

    // Measure and lay out: fixed body width, wrapped height.
    const int dpi = CtrlDpi(dlg);
    const int W = MulDiv(320, dpi, 96);
    const int m = MulDiv(12, dpi, 96);
    int titleH = 0, bodyH = 0;
    HDC dc = GetDC(card2);
    HFONT bodyFont = reinterpret_cast<HFONT>(
        SendMessageW(dlg, WM_GETFONT, 0, 0));
    if (HGDIOBJ of = SelectObject(dc, bodyFont)) {
        RECT brc = { 0, 0, W, 0 };
        DrawTextW(dc, body, -1, &brc,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        bodyH = brc.bottom;
        SelectObject(dc, of);
    }
    if (HGDIOBJ of = SelectObject(dc, TitleFont(dlg))) {
        RECT trc = { 0, 0, W, 0 };
        DrawTextW(dc, title, -1, &trc,
                  DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        titleH = trc.bottom + MulDiv(6, dpi, 96);
        SelectObject(dc, of);
    }
    ReleaseDC(card2, dc);
    if (bodyH <= 0) bodyH = MulDiv(16, dpi, 96);
    // GDI-measured and static-drawn heights can differ by a hair at some
    // DPIs - pad both so no card ever clips its last line.
    titleH += MulDiv(2, dpi, 96);
    bodyH += MulDiv(4, dpi, 96);

    const int cw = W + m * 2;
    const int chh = m + titleH + bodyH + m;
    SetWindowPos(GetDlgItem(card2, IDC_ST_HELP_TITLE), nullptr,
                 m, m, W, titleH, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(card2, IDC_ST_HELP_BODY), nullptr,
                 m, m + titleH, W, bodyH, SWP_NOZORDER | SWP_NOACTIVATE);

    // Place beside the (?), clamped into that monitor's work area (flip
    // left when there is no room on the right).
    HWND qq = GetDlgItem(dlg, id);
    RECT qr{};
    if (qq) GetWindowRect(qq, &qr);
    HMONITOR mon = MonitorFromWindow(qq ? qq : dlg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    const int gap = MulDiv(10, dpi, 96);
    int x = qr.right + gap;
    int y = (qr.top + qr.bottom) / 2 - chh / 2;
    if (x + cw > mi.rcWork.right) x = qr.left - gap - cw;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (y + chh > mi.rcWork.bottom) y = mi.rcWork.bottom - chh;

    SetWindowPos(card2, HWND_TOPMOST, x, y, cw, chh,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

struct SliderSpec {
    int sliderId;
    int editId;
    int minV;
    int maxV;
    unsigned Settings::*field;
};

const SliderSpec kSliders[] = {
    { IDC_SL_CROSS, IDC_ED_CROSS, 100, 2000, &Settings::crossBackMs },
    { IDC_SL_DWELL, IDC_ED_DWELL, 0, 1000, &Settings::dwellMs },
    { IDC_SL_ZONE, IDC_ED_ZONE, 4, 96, &Settings::zonePx },
    { IDC_SL_STOP, IDC_ED_STOP, 2, 48, &Settings::stopRadiusPx },
    { IDC_SL_REDELAY, IDC_ED_REDELAY, 0, 10, &Settings::reactivateDelayS },
    { IDC_SL_GRACE, IDC_ED_GRACE, 0, 64, &Settings::leaveGracePx },
    { IDC_SL_STAY, IDC_ED_STAY, 0, 10, &Settings::stayS },
    // IDC_SL_TITLE/IDC_ED_TITLE used to be the max-title-chars slider;
    // max width replaced it.
    { IDC_SL_TITLE, IDC_ED_TITLE, 120, 800, &Settings::maxWidth },
    { IDC_SL_CARDW, IDC_ED_CARDW, 230, 400, &Settings::cardWidth },   //
    { IDC_SL_TB_PSIZE, IDC_ED_TB_PSIZE, 5, 16, &Settings::persistentSize },
    { IDC_SL_TB_HSIZE, IDC_ED_TB_HSIZE, 5, 16, &Settings::hoverSize },
};

Settings* g_settings = nullptr;
void (*g_apply)(const Settings&, const Settings&) = nullptr;
bool g_syncing = false;   // suppress feedback loops while writing controls

// The open settings dialog: one instance ever - repeat
// requests (tray menu) just raise the existing window.
HWND g_mainDlg = nullptr;

// The pill control owns this; the dialog reads it on the pill's
// BN_CLICKED notification.
Activation g_pillMode = Activation::EdgeDwell;

// --- (?) owner-draw: small circle, card fill, border, bold accent ? ------

void DrawHelpButton(HWND dlg, const DRAWITEMSTRUCT* dis) {
    const RECT rc = dis->rcItem;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int d = w < h ? w : h;

    // Circle, centered in the (possibly taller/wider) box - the hug pads
    // the hit rect wider than the glyph.
    const int ox = rc.left + (w - d) / 2;
    const int oy = rc.top + (h - d) / 2;
    HBRUSH fill = CreateSolidBrush(
        (dis->itemState & ODS_SELECTED) ? kColAccLight : kColCard);
    HPEN border = CreatePen(PS_SOLID, 1,
        (dis->itemState & ODS_SELECTED) ? kColAccent : kColBorder);
    HGDIOBJ oldFill = SelectObject(dis->hDC, fill);
    HGDIOBJ oldPen = SelectObject(dis->hDC, border);
    SetBkMode(dis->hDC, TRANSPARENT);
    Ellipse(dis->hDC, ox, oy, ox + d, oy + d);

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(dlg, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = SelectObject(dis->hDC, font);
    SetTextColor(dis->hDC, kColAccDark);
    wchar_t q[2] = L"?";
    RECT textRc = { ox, oy, ox + d, oy + d };
    DrawTextW(dis->hDC, q, 1, &textRc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    SelectObject(dis->hDC, oldFont);
    SelectObject(dis->hDC, oldPen);
    SelectObject(dis->hDC, oldFill);
    DeleteObject(border);
    DeleteObject(fill);
}


ComPtr<ID2D1Factory> g_uiD2d;
ComPtr<ID2D1DCRenderTarget> g_uiRT;      // shared by the pill AND the
                                         // re-painted native controls
ComPtr<IDWriteFactory> g_uiDW;
ComPtr<ID2D1SolidColorBrush> g_uiBrush;
ComPtr<ID2D1StrokeStyle> g_uiRoundCap;   // round caps for the check mark
ComPtr<IDWriteTextFormat> g_pillFmt;      // regular, unselected half
ComPtr<IDWriteTextFormat> g_pillFmtSel;   // semibold, selected half
bool g_pillHot = false;

bool EnsureUiResources() {
    if (g_uiRT) return true;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   __uuidof(ID2D1Factory), nullptr,
                                   reinterpret_cast<void**>(
                                       g_uiD2d.GetAddressOf()));
    if (FAILED(hr)) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1_PIXEL_FORMAT{ DXGI_FORMAT_B8G8R8A8_UNORM,
                           D2D1_ALPHA_MODE_PREMULTIPLIED });
    hr = g_uiD2d->CreateDCRenderTarget(&props, &g_uiRT);
    if (FAILED(hr)) return false;
    // Grayscale AA: no ClearType fringing on the cream bg, smooth text
    // (round 1's GDI DrawText read as un-antialiased at my zoom).
    g_uiRT->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    IUnknown* dwUnk = nullptr;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory), &dwUnk);
    if (FAILED(hr)) return false;
    g_uiDW.Attach(reinterpret_cast<IDWriteFactory*>(dwUnk));
    g_uiDW->CreateTextFormat(L"Segoe UI", nullptr,
                             DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 9.5f, L"",
                             &g_pillFmt);
    g_uiDW->CreateTextFormat(L"Segoe UI Semibold", nullptr,
                             DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 9.5f, L"",
                             &g_pillFmtSel);
    for (IDWriteTextFormat* f : { g_pillFmt.Get(), g_pillFmtSel.Get() }) {
        if (f) {
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    g_uiRT->CreateSolidColorBrush(kDCard, &g_uiBrush);
    g_uiD2d->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f),
        nullptr, 0, &g_uiRoundCap);
    return g_pillFmt && g_pillFmtSel && g_uiBrush && g_uiRoundCap;
}
// --- teal native controls --------------------------------------
//
// comctl32 paints checkbox/radio glyphs, slider thumbs and default buttons
// with the SYSTEM accent color (blue on a stock install) and nothing in a
// normal process can redirect that. The palette has no blue, so the glyphs
// are ours: checkboxes/radios/pushbuttons/trackbars are subclassed and
// WM_PAINT is owned outright (native toggle/keyboard/focus/DPI/drag logic
// is untouched - only the painting is replaced). Everything is drawn with
// the shared D2D/DWrite pipeline (round 1 used raw GDI: hard non-AA edges
// - the "rough" look - and NM_CUSTOMDRAW slider parts that flickered while
// dragging). If D2D is unavailable the controls fall back to native paint
// (blue, but functional) rather than blank.

const wchar_t kHotProp[] = L"EdgePopHot";

bool CtrlHot(HWND c) { return GetPropW(c, kHotProp) != nullptr; }

int CtrlDpi(HWND c) { return static_cast<int>(GetDpiForWindow(c)); }

// 96-dpi design pixels scaled to the window's monitor.
float Scaled(HWND c, float px96) {
    return px96 * static_cast<float>(CtrlDpi(c)) / 96.0f;
}

// The dialog font as a DWrite format; DIPs == pixels on the 96-dpi DC
// render target. Cached per (font height, DPI); rebuilt when they change.
IDWriteTextFormat* CtrlTextFormat(HWND c) {
    static ComPtr<IDWriteTextFormat> fmt;
    static int cachedH = 0, cachedDpi = 0;
    HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(GetParent(c), WM_GETFONT, 0, 0));
    LOGFONTW lf{};
    if (!font || !GetObjectW(font, sizeof(lf), &lf) || lf.lfHeight >= 0)
        return nullptr;
    const int dpi = CtrlDpi(c);
    if (!fmt || cachedH != lf.lfHeight || cachedDpi != dpi) {
        fmt.Reset();
        if (SUCCEEDED(g_uiDW->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                -static_cast<float>(lf.lfHeight), L"",
                fmt.GetAddressOf()))) {
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            // One line, clipped like the native control - with wrap on,
            // long labels ("Full-screen guard") broke into two clipped
            // lines.
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            cachedH = lf.lfHeight;
            cachedDpi = dpi;
        }
    }
    return fmt.Get();
}

// Renders one checkbox/radio into an arbitrary DC (the WM_PAINT DC, or a
// WM_PRINTCLIENT capture DC -). Returns false only when the D2D path
// is unusable; the caller then renders a flat GDI fallback instead of ever
// letting the native (blue) paint run.
bool PaintCheckCore(HWND c, HDC dc, const RECT& rc) {
    if (!EnsureUiResources()) return false;
    bool ok = false;
    SIZE ts{};
    wchar_t text[128];
    const int len = GetWindowTextW(c, text, 128);
    do {
        if (FAILED(g_uiRT->BindDC(dc, &rc))) break;
        g_uiRT->BeginDraw();
        const float w = static_cast<float>(rc.right - rc.left);
        const float h = static_cast<float>(rc.bottom - rc.top);
        const float s = Scaled(c, 17.0f);   // glyph edge - bigger than native
        const float gy = (h - s) / 2;
        const float gap = Scaled(c, 8.0f);
        const LONG_PTR kind = GetWindowLongPtrW(c, GWL_STYLE) & 0xF;
        const bool radio = kind == BS_AUTORADIOBUTTON || kind == BS_RADIOBUTTON;
        const bool checked =
            SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool pushed =
            (SendMessageW(c, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
        const bool disabled = !IsWindowEnabled(c);
        const bool hot = CtrlHot(c) && !disabled;

        g_uiBrush->SetColor(kDBg);
        g_uiRT->FillRectangle(D2D1::RectF(0, 0, w, h), g_uiBrush.Get());

        const D2D1_COLOR_F borderC =
            disabled ? kDBorder
                     : ((hot || pushed || checked) ? kDAccent : kDBorder);
        if (radio) {
            const D2D1_ELLIPSE ring = D2D1::Ellipse(
                D2D1::Point2F(0.5f + s / 2, gy + s / 2), s / 2, s / 2);
            g_uiBrush->SetColor(kDCard);
            g_uiRT->FillEllipse(ring, g_uiBrush.Get());
            g_uiBrush->SetColor(borderC);
            g_uiRT->DrawEllipse(ring, g_uiBrush.Get(), 1.3f);
            if (checked) {
                const D2D1_ELLIPSE dot = D2D1::Ellipse(
                    ring.point, s * 0.27f, s * 0.27f);
                g_uiBrush->SetColor(disabled ? kDSoft : kDAccent);
                g_uiRT->FillEllipse(dot, g_uiBrush.Get());
            }
        } else {
            const D2D1_ROUNDED_RECT box = D2D1::RoundedRect(
                D2D1::RectF(0.5f, gy + 0.5f, 0.5f + s, gy + 0.5f + s),
                s * 0.22f, s * 0.22f);
            if (checked && !disabled) {
                g_uiBrush->SetColor(pushed ? kDAccLight : kDAccent);
                g_uiRT->FillRoundedRectangle(box, g_uiBrush.Get());
                g_uiBrush->SetColor(kDAccDark);
                g_uiRT->DrawRoundedRectangle(box, g_uiBrush.Get(), 1.0f);
                g_uiBrush->SetColor(kDCard);
                const D2D1_POINT_2F a{ 0.5f + s * 0.18f, gy + 0.5f + s * 0.49f };
                const D2D1_POINT_2F b{ 0.5f + s * 0.40f, gy + 0.5f + s * 0.71f };
                const D2D1_POINT_2F d{ 0.5f + s * 0.84f, gy + 0.5f + s * 0.25f };
                const float cw = s * 0.155f;
                g_uiRT->DrawLine(a, b, g_uiBrush.Get(), cw, g_uiRoundCap.Get());
                g_uiRT->DrawLine(b, d, g_uiBrush.Get(), cw, g_uiRoundCap.Get());
            } else {
                g_uiBrush->SetColor(pushed ? kDAccLight : kDCard);
                g_uiRT->FillRoundedRectangle(box, g_uiBrush.Get());
                g_uiBrush->SetColor(borderC);
                g_uiRT->DrawRoundedRectangle(box, g_uiBrush.Get(), 1.3f);
            }
        }

        if (len > 0) {
            if (IDWriteTextFormat* fmt = CtrlTextFormat(c)) {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                g_uiBrush->SetColor(disabled ? kDSoft : kDText);
                g_uiRT->DrawTextW(
                    text, UINT32(len), fmt,
                    D2D1::RectF(0.5f + s + gap, 0, w, h), g_uiBrush.Get());
            }
        }
        ok = SUCCEEDED(g_uiRT->EndDraw());
        if (ok && GetFocus() == c) {
            // Width for the focus rect only - GDI measuring is fine here.
            HFONT font = reinterpret_cast<HFONT>(
                SendMessageW(GetParent(c), WM_GETFONT, 0, 0));
            if (font && len > 0) {
                HGDIOBJ of = SelectObject(dc, font);
                GetTextExtentPoint32W(dc, text, len, &ts);
                SelectObject(dc, of);
            }
            RECT frc = {
                rc.left, rc.top,
                std::min<LONG>(rc.right,
                    rc.left + static_cast<LONG>(s + gap) + ts.cx + 4),
                rc.bottom };
            DrawFocusRect(dc, &frc);
        }
    } while (false);
    return ok;
}

// Direct-paint entry for the hidden-dialog diagnostics probe.
bool PaintCheckD2D(HWND c) {
    InvalidateRect(c, nullptr, FALSE);   // paint the WHOLE control
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(c, &ps);
    RECT rc;
    GetClientRect(c, &rc);
    const bool ok = rc.right > rc.left && rc.bottom > rc.top
                        ? PaintCheckCore(c, dc, rc)
                        : true;
    EndPaint(c, &ps);
    return ok;
}

bool PaintPushD2D(HWND c, bool primary) {
    if (!EnsureUiResources()) return false;
    RECT rc;
    GetClientRect(c, &rc);
    if (rc.right <= rc.left || rc.bottom <= rc.top) return true;
    InvalidateRect(c, nullptr, FALSE);   // paint the WHOLE control
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(c, &ps);
    bool ok = false;
    do {
        if (FAILED(g_uiRT->BindDC(dc, &rc))) break;
        g_uiRT->BeginDraw();
        const float w = static_cast<float>(rc.right - rc.left);
        const float h = static_cast<float>(rc.bottom - rc.top);
        const bool disabled = !IsWindowEnabled(c);
        const bool hot = CtrlHot(c) && !disabled;
        const bool pushed =
            (SendMessageW(c, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;

        g_uiBrush->SetColor(kDBg);   // outside the rounded corners: cream
        g_uiRT->FillRectangle(D2D1::RectF(0, 0, w, h), g_uiBrush.Get());

        D2D1_COLOR_F fillC, borderC, textC;
        if (primary) {   // Donate/Close: accent fill, white text
            fillC = hot || pushed ? kDAccDark : kDAccent;
            borderC = kDAccDark;
            textC = kDCard;
        } else {
            fillC = pushed ? kDAccLight : kDCard;
            borderC = hot || pushed ? kDAccent : kDBorder;
            textC = hot ? kDAccDark : kDText;
        }
        if (disabled) {
            fillC = kDBg;
            borderC = kDBorder;
            textC = kDSoft;
        }
        const D2D1_ROUNDED_RECT box = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f),
            Scaled(c, 5.0f), Scaled(c, 5.0f));
        g_uiBrush->SetColor(fillC);
        g_uiRT->FillRoundedRectangle(box, g_uiBrush.Get());
        g_uiBrush->SetColor(borderC);
        g_uiRT->DrawRoundedRectangle(box, g_uiBrush.Get(), 1.2f);

        wchar_t text[128];
        const int len = GetWindowTextW(c, text, 128);
        if (len > 0) {
            if (IDWriteTextFormat* fmt = CtrlTextFormat(c)) {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                g_uiBrush->SetColor(textC);
                g_uiRT->DrawTextW(
                    text, UINT32(len), fmt,
                    D2D1::RectF(2.0f, 0, w - 2.0f, h), g_uiBrush.Get());
            }
        }
        ok = SUCCEEDED(g_uiRT->EndDraw());
        if (ok && GetFocus() == c) {
            RECT frc = rc;
            InflateRect(&frc, -3, -3);
            DrawFocusRect(dc, &frc);
        }
    } while (false);
    EndPaint(c, &ps);
    return ok;
}

// Sliders: channel = border-colored track with an accent fill up to the
// thumb; thumb = full-height accent capsule. TBM_GETCHANNELRECT /
// TBM_GETTHUMBRECT report the NATIVE geometry, so what you see is exactly
// what you grab. Native drag/page/keyboard logic is untouched.
bool PaintTrackD2D(HWND c) {
    if (!EnsureUiResources()) return false;
    RECT rc;
    GetClientRect(c, &rc);
    if (rc.right <= rc.left || rc.bottom <= rc.top) return true;
    // The native control invalidates only what IT thinks changed (old +
    // new thumb slivers) - our whole-client paint gets CLIPPED to that,
    // so dragging left ghost thumbs smeared along the track (the "dashed
    // track" look,). Widen the update region to the full control.
    InvalidateRect(c, nullptr, FALSE);
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(c, &ps);
    bool ok = false;
    do {
        if (FAILED(g_uiRT->BindDC(dc, &rc))) break;
        g_uiRT->BeginDraw();
        const float w = static_cast<float>(rc.right - rc.left);
        const float h = static_cast<float>(rc.bottom - rc.top);
        RECT ch{}, th{};
        SendMessageW(c, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&ch));
        SendMessageW(c, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&th));
        if (ch.right <= ch.left) {
            ch.left = rc.left;
            ch.right = rc.right - 1;
        }

        g_uiBrush->SetColor(kDBg);
        g_uiRT->FillRectangle(D2D1::RectF(0, 0, w, h), g_uiBrush.Get());

        // Anchor everything to the native THUMB rect's center: the rect is
        // the hit area (generous), the visual thumb is a slim capsule in
        // its middle - anchoring the channel to the control center instead
        // made track and thumb sit visibly apart at some DPIs.
        const float tCx = (static_cast<float>(th.left) +
                           static_cast<float>(th.right)) / 2;
        const float tCy = th.bottom > th.top
            ? (static_cast<float>(th.top) +
               static_cast<float>(th.bottom)) / 2
            : h / 2;
        const float chL = static_cast<float>(
            ch.left > rc.left ? ch.left : rc.left);
        const float chR = static_cast<float>(
            ch.right < rc.right ? ch.right : rc.right - 1);
        const float trackH = Scaled(c, 5.0f);
        D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
            D2D1::RectF(chL, tCy - trackH / 2, chR, tCy + trackH / 2),
            trackH / 2, trackH / 2);
        g_uiBrush->SetColor(kDBorder);
        g_uiRT->FillRoundedRectangle(track, g_uiBrush.Get());
        if (tCx > chL + trackH) {
            D2D1_ROUNDED_RECT fill = track;
            fill.rect.right = tCx;
            g_uiBrush->SetColor(kDAccent);
            g_uiRT->FillRoundedRectangle(fill, g_uiBrush.Get());
        }

        // The grabber: a slim vertical capsule (the native look - drawing
        // the thumb rect's full width produced a blobby oval,).
        const bool enabled = IsWindowEnabled(c);
        const bool hot = CtrlHot(c) && enabled;
        const float tw = Scaled(c, 7.0f);
        const float tTop = tCy -
            (th.bottom > th.top
                 ? (static_cast<float>(th.bottom - th.top) / 2 - Scaled(c, 1.5f))
                 : Scaled(c, 10.0f));
        const float tBot = tTop +
            (th.bottom > th.top
                 ? (static_cast<float>(th.bottom - th.top) - Scaled(c, 3.0f))
                 : Scaled(c, 20.0f));
        if (tBot - tTop > 2) {
            // A white halo ring separates the grabber from the track
            // fill (same-accent shapes merge into one blob otherwise).
            const float hw = tw / 2 + Scaled(c, 1.5f);
            D2D1_ROUNDED_RECT halo = D2D1::RoundedRect(
                D2D1::RectF(tCx - hw, tTop - Scaled(c, 1.5f),
                            tCx + hw, tBot + Scaled(c, 1.5f)),
                hw, hw);
            g_uiBrush->SetColor(kDCard);
            g_uiRT->FillRoundedRectangle(halo, g_uiBrush.Get());
            const D2D1_ROUNDED_RECT thumb = D2D1::RoundedRect(
                D2D1::RectF(tCx - tw / 2, tTop, tCx + tw / 2, tBot),
                tw / 2, tw / 2);
            g_uiBrush->SetColor(!enabled      ? kDBorder
                                : hot         ? kDAccDark
                                              : kDAccent);
            g_uiRT->FillRoundedRectangle(thumb, g_uiBrush.Get());
            g_uiBrush->SetColor(kDAccDark);
            g_uiRT->DrawRoundedRectangle(thumb, g_uiBrush.Get(), 1.0f);
        }
        ok = SUCCEEDED(g_uiRT->EndDraw());
    } while (false);
    EndPaint(c, &ps);
    return ok;
}

LRESULT CALLBACK TealCheckProc(HWND c, UINT msg, WPARAM wp, LPARAM lp,
                               UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;   // painted wholesale in WM_PAINT
        case WM_ENABLE:
            // The default WM_ENABLE handling lets comctl32 paint the
            // NATIVE enable/disable transition - the "pulse to Windows
            // default" during master-switch storms. Swallow it; our own
            // disabled styling paints on the next WM_PAINT.
            InvalidateRect(c, nullptr, FALSE);
            return 0;
        case WM_PAINT: {
            // Own the paint end-to-end. DefSubclassProc would render the
            // NATIVE blue glyph; if D2D is down we draw a flat GDI control
            // instead.
            InvalidateRect(c, nullptr, FALSE);   // whole client
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(c, &ps);
            RECT rc;
            GetClientRect(c, &rc);
            if (rc.right > rc.left && rc.bottom > rc.top &&
                !PaintCheckCore(c, dc, rc)) {
                FillRect(dc, &rc, BgBrush());
                wchar_t text[128];
                const int len = GetWindowTextW(c, text, 128);
                if (len > 0) {
                    HFONT font = reinterpret_cast<HFONT>(
                        SendMessageW(GetParent(c), WM_GETFONT, 0, 0));
                    HGDIOBJ of = SelectObject(dc, font);
                    SetBkMode(dc, TRANSPARENT);
                    SetTextColor(dc, IsWindowEnabled(c) ? kColText : kColSoft);
                    RECT tr = rc;
                    tr.left += MulDiv(25, CtrlDpi(c), 96);
                    DrawTextW(dc, text, len, &tr,
                              DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    SelectObject(dc, of);
                }
            }
            EndPaint(c, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: {
            // Alt-Tab previews / captures print the control; the native
            // WM_PRINTCLIENT would render blue glyphs.
            RECT rc;
            GetClientRect(c, &rc);
            if (rc.right > rc.left && rc.bottom > rc.top) {
                PaintCheckCore(c, reinterpret_cast<HDC>(wp), rc);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (!CtrlHot(c)) {
                SetPropW(c, kHotProp, reinterpret_cast<HANDLE>(1));
                InvalidateRect(c, nullptr, FALSE);
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, c, 0 };
                TrackMouseEvent(&tme);
            }
            break;
        case WM_MOUSELEAVE:
            RemovePropW(c, kHotProp);
            InvalidateRect(c, nullptr, FALSE);
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(c, nullptr, FALSE);
            break;
        case WM_NCDESTROY:
            RemovePropW(c, kHotProp);
            break;
    }
    return DefSubclassProc(c, msg, wp, lp);
}

LRESULT CALLBACK TealPushProc(HWND c, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR refData) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_ENABLE:   // no native transition paint
            InvalidateRect(c, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            if (!PaintPushD2D(c, refData != 0))
                return DefSubclassProc(c, msg, wp, lp);
            return 0;
        case WM_MOUSEMOVE:
            if (!CtrlHot(c)) {
                SetPropW(c, kHotProp, reinterpret_cast<HANDLE>(1));
                InvalidateRect(c, nullptr, FALSE);
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, c, 0 };
                TrackMouseEvent(&tme);
            }
            break;
        case WM_MOUSELEAVE:
            RemovePropW(c, kHotProp);
            InvalidateRect(c, nullptr, FALSE);
            return 0;
        case WM_NCDESTROY:
            RemovePropW(c, kHotProp);
            break;
    }
    return DefSubclassProc(c, msg, wp, lp);
}

LRESULT CALLBACK TealTrackProc(HWND c, UINT msg, WPARAM wp, LPARAM lp,
                               UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_ENABLE:   // no native transition paint
            InvalidateRect(c, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            if (!PaintTrackD2D(c)) return DefSubclassProc(c, msg, wp, lp);
            return 0;
        case WM_MOUSEMOVE:
            if (!CtrlHot(c)) {
                SetPropW(c, kHotProp, reinterpret_cast<HANDLE>(1));
                InvalidateRect(c, nullptr, FALSE);
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, c, 0 };
                TrackMouseEvent(&tme);
            }
            break;
        case WM_MOUSELEAVE:
            RemovePropW(c, kHotProp);
            InvalidateRect(c, nullptr, FALSE);
            return 0;
        case WM_NCDESTROY:
            RemovePropW(c, kHotProp);
            break;
    }
    return DefSubclassProc(c, msg, wp, lp);
}

// Buddy edit fields: native EDITs keep their caret, selection and
// typing; only the chrome is ours - a flat 1-px border (teal when focused,
// tan otherwise) drawn in WM_NCPAINT instead of the sunken 3D frame, with
// the cream/dark text colors set by the dialog's WM_CTLCOLOREDIT.
LRESULT CALLBACK TealEditProc(HWND c, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_NCPAINT: {
            RECT rc;
            GetWindowRect(c, &rc);
            OffsetRect(&rc, -rc.left, -rc.top);
            HDC dc = GetWindowDC(c);
            HBRUSH b = CreateSolidBrush(
                GetFocus() == c ? kColAccent : kColBorder);
            FrameRect(dc, &rc, b);
            DeleteObject(b);
            ReleaseDC(c, dc);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            DefSubclassProc(c, msg, wp, lp);
            RedrawWindow(c, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
            return 0;
        case WM_ENABLE:   // no native transition paint
            InvalidateRect(c, nullptr, FALSE);
            RedrawWindow(c, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
            return 0;
    }
    return DefSubclassProc(c, msg, wp, lp);
}

// The kitkarl.com link in the settings top band: a plain static
// colored teal by WM_CTLCOLORSTATIC (SysLinks can't be recolored) plus a
// hand cursor and a click that opens the site.
void OpenSite();   // defined with the site links below
LRESULT CALLBACK TealLinkProc(HWND c, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_LBUTTONUP: {
            RECT rc;
            GetClientRect(c, &rc);
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (PtInRect(&rc, pt)) OpenSite();
            return 0;
        }
    }
    return DefSubclassProc(c, msg, wp, lp);
}

// Subclass every checkbox/radio/plain pushbutton/trackbar in a dialog. The
// (?) buttons are BS_OWNERDRAW and the pill is its own class - both skip.
void StyleDialogControls(HWND dlg) {
    HWND c = GetWindow(dlg, GW_CHILD);
    while (c) {
        wchar_t cls[24];
        if (GetClassNameW(c, cls, 24) > 0) {
            if (_wcsicmp(cls, L"Button") == 0) {
                const LONG_PTR kind = GetWindowLongPtrW(c, GWL_STYLE) & 0xF;
                if (kind == BS_AUTOCHECKBOX || kind == BS_CHECKBOX ||
                    kind == BS_AUTORADIOBUTTON || kind == BS_RADIOBUTTON) {
                    SetWindowSubclass(c, TealCheckProc, 0, 0);
                } else if (kind == BS_PUSHBUTTON || kind == BS_DEFPUSHBUTTON) {
                    // Donate gets the primary look wherever it lives.
                    SetWindowSubclass(c, TealPushProc, 0,
                        (kind == BS_DEFPUSHBUTTON ||
                         GetDlgCtrlID(c) == IDC_BTN_DONATE) ? 1 : 0);
                }
            } else if (_wcsicmp(cls, TRACKBAR_CLASSW) == 0) {
                SetWindowSubclass(c, TealTrackProc, 0, 0);
            } else if (_wcsicmp(cls, L"Edit") == 0) {
                SetWindowSubclass(c, TealEditProc, 0, 0);
            } else if (_wcsicmp(cls, L"Static") == 0 &&
                       GetDlgCtrlID(c) == IDC_LINK_SITE &&
                       GetParent(c) == dlg) {
                SetWindowSubclass(c, TealLinkProc, 0, 0);
            }
        }
        c = GetWindow(c, GW_HWNDNEXT);
    }
}

// Dialogs get the app icon via WM_SETICON: without it the title
// bar, Alt-Tab and the settings taskbar button show a generic one.
// Loaded once, kept for the process lifetime (shared, never destroyed).
void SetAppIcon(HWND wnd) {
    // "small" is a rpcndr.h macro (== char) once the D2D headers are in;
    // keep these names out of its way.
    static HICON bigIc = nullptr;
    static HICON smallIc = nullptr;
    if (!bigIc) {
        bigIc = reinterpret_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
            IMAGE_ICON, GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    }
    if (!smallIc) {
        smallIc = reinterpret_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
            IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    }
    if (bigIc) SendMessageW(wnd, WM_SETICON, ICON_BIG,
                            reinterpret_cast<LPARAM>(bigIc));
    if (smallIc) SendMessageW(wnd, WM_SETICON, ICON_SMALL,
                              reinterpret_cast<LPARAM>(smallIc));
}

void OpenSite() {
    ShellExecuteW(nullptr, L"open", L"" EDGEPOP_SITE_URL, nullptr, nullptr,
                  SW_SHOWNORMAL);
}

void OpenDonate() {
    ShellExecuteW(nullptr, L"open", L"" EDGEPOP_DONATE_URL, nullptr, nullptr,
                  SW_SHOWNORMAL);
}

// RC strings can't carry the quoted href markup, so every link control is
// given plain template text and the real markup is set here (WM_SETTEXT is
// the documented way to change a SysLink's content).
void StyleSiteLink(HWND dlg) {
    SetWindowTextW(
        GetDlgItem(dlg, IDC_LINK_SITE),
        L"<a href=\"" L"" EDGEPOP_SITE_URL
        L"\">Find more useful apps at kitkarl.com</a>");
}

// Shared WM_NOTIFY handling for the styled dialogs: the kitkarl.com links.
// Returns TRUE when the message was handled.
BOOL HandleDialogNotify(HWND, LPARAM lp) {
    const auto* nm = reinterpret_cast<const NMHDR*>(lp);
    if (nm->code == NM_CLICK && nm->idFrom == IDC_LINK_SITE) {
        OpenSite();
        return TRUE;
    }
    return FALSE;
}

// --- the About dialog + splash ---------------------------------

HFONT TitleFont(HWND dlg) {
    static HFONT font = nullptr;   // process-lifetime cache
    if (!font) {
        HFONT base = reinterpret_cast<HFONT>(
            SendMessageW(dlg, WM_GETFONT, 0, 0));
        LOGFONTW lf{};
        if (base && GetObjectW(base, sizeof(lf), &lf)) {
            lf.lfHeight = lf.lfHeight * 14 / 9;   // ~14pt from the 9pt base
            lf.lfWeight = FW_SEMIBOLD;
            wcscpy_s(lf.lfFaceName, L"Segoe UI Semibold");
            font = CreateFontIndirectW(&lf);
        }
    }
    return font ? font
                : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

// The DPI-scaled large app icon for display (About + splash), cached.
HICON LargeAppIcon() {
    static HICON ic = reinterpret_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    return ic;
}

// Win11 rounded corners for the borderless splash. A preference only -
// plain Win10 fails the call harmlessly and leaves square corners; this
// one-liner is the whole isolation point for that API.
void PreferRoundedCorners(HWND wnd) {
    const DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(wnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &pref, sizeof(pref));
}

INT_PTR CALLBACK AboutProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            SetAppIcon(dlg);
            StyleDialogControls(dlg);   // styles the Donate button
            StyleSiteLink(dlg);
            SendMessageW(GetDlgItem(dlg, IDC_ST_ABOUT_TITLE), WM_SETFONT,
                         reinterpret_cast<WPARAM>(TitleFont(dlg)), TRUE);
            wchar_t ver[64]{};
            swprintf_s(ver, L"Version %hs", EDGEPOP_VERSION_TXT);
            SetDlgItemTextW(dlg, IDC_ST_ABOUT_VER, ver);
            // The template's ICON statement shows the 32px image; replace
            // it with the DPI-scaled system-size one for crisp rendering.
            if (HICON ic = LargeAppIcon()) {
                SendMessageW(GetDlgItem(dlg, IDC_IC_APP), STM_SETICON,
                             reinterpret_cast<WPARAM>(ic), 0);
            }
            return TRUE;
        }
        case WM_CTLCOLORDLG:
            SetBkColor(reinterpret_cast<HDC>(wp), kColBg);
            return reinterpret_cast<INT_PTR>(BgBrush());
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(wp);
            const HWND st = reinterpret_cast<HWND>(lp);
            SetBkColor(dc, kColBg);
            SetTextColor(dc, st == GetDlgItem(dlg, IDC_ST_ABOUT_TITLE)
                                 ? kColAccDark
                             : st == GetDlgItem(dlg, IDC_ST_ABOUT_VER) ||
                                       st == GetDlgItem(dlg, IDC_ST_ABOUT_COPY)
                                 ? kColSoft
                                 : kColText);
            return reinterpret_cast<INT_PTR>(BgBrush());
        }
        case WM_NOTIFY:
            return HandleDialogNotify(dlg, lp);
        case WM_COMMAND:
            if (LOWORD(wp) == IDC_BTN_DONATE) {
                OpenDonate();
                return TRUE;
            }
            if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            return FALSE;
    }
    return FALSE;
}

// --- the splash screen -----------------------------------------------
//
// Brief, silent, never steals focus: icon, name + version, the site link
// and the copyright line. Modeless - it lives inside WinMain's regular
// pump - and closes on a timer, on any click, or on the site link.

constexpr UINT kSplashTimer = 1;
HWND g_splash = nullptr;

INT_PTR CALLBACK SplashProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            StyleSiteLink(dlg);
            SendMessageW(GetDlgItem(dlg, IDC_ST_ABOUT_TITLE), WM_SETFONT,
                         reinterpret_cast<WPARAM>(TitleFont(dlg)), TRUE);
            wchar_t ver[64]{};
            swprintf_s(ver, L"Version %hs", EDGEPOP_VERSION_TXT);
            SetDlgItemTextW(dlg, IDC_ST_ABOUT_VER, ver);
            if (HICON ic = LargeAppIcon()) {
                SendMessageW(GetDlgItem(dlg, IDC_IC_APP), STM_SETICON,
                             reinterpret_cast<WPARAM>(ic), 0);
            }
            PreferRoundedCorners(dlg);
            SetTimer(dlg, kSplashTimer, 1600, nullptr);
            return TRUE;
        }
        case WM_TIMER:
            if (wp == kSplashTimer) {
                DestroyWindow(dlg);
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            DestroyWindow(dlg);
            return TRUE;
        case WM_NOTIFY:
            if (HandleDialogNotify(dlg, lp)) {
                DestroyWindow(dlg);   // the link was the click; done
                return TRUE;
            }
            return FALSE;
        case WM_PAINT: {
            // Borderless: paint our own thin frame (children stay inside).
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(dlg, &ps);
            RECT rc;
            GetClientRect(dlg, &rc);
            FillRect(dc, &rc, BgBrush());
            HBRUSH frame = CreateSolidBrush(kColBorder);
            FrameRect(dc, &rc, frame);
            DeleteObject(frame);
            EndPaint(dlg, &ps);
            return TRUE;
        }
        case WM_CTLCOLORDLG:
            SetBkColor(reinterpret_cast<HDC>(wp), kColBg);
            return reinterpret_cast<INT_PTR>(BgBrush());
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(wp);
            const HWND st = reinterpret_cast<HWND>(lp);
            SetBkColor(dc, kColBg);
            SetTextColor(dc, st == GetDlgItem(dlg, IDC_ST_ABOUT_TITLE)
                                 ? kColAccDark
                             : st == GetDlgItem(dlg, IDC_ST_ABOUT_VER) ||
                                       st == GetDlgItem(dlg, IDC_ST_ABOUT_COPY)
                                 ? kColSoft
                                 : kColText);
            return reinterpret_cast<INT_PTR>(BgBrush());
        }
        case WM_DESTROY:
            g_splash = nullptr;
            return TRUE;
    }
    return FALSE;
}

// Move each (?) right after the measured text of its label control -
// VideoSquisher's "hug the text, no gap" behavior.
struct HugPair { int textCtrl; int qqCtrl; };
const HugPair kHugs[] = {
    { IDC_LB_DWELL,     IDC_QQ_DWELL },
    { IDC_LB_ZONE,      IDC_QQ_ZONE },
    { IDC_LB_STOP,      IDC_QQ_STOP },
    { IDC_LB_CROSS,     IDC_QQ_CROSS },
    { IDC_CK_FSGUARD,   IDC_QQ_FGS },
    { IDC_LB_EDGES,     IDC_QQ_EDGES },
    { IDC_LB_REDELAY,   IDC_QQ_REDELAY },
    { IDC_LB_SIDE,      IDC_QQ_SIDE },
    { IDC_LB_CHAIN,     IDC_QQ_CHAIN },
    { IDC_LB_GRACE,     IDC_QQ_GRACE },
    { IDC_LB_STAY,      IDC_QQ_STAY },
    { IDC_LB_TITLE,     IDC_QQ_TITLE },
    { IDC_LB_CARDW,     IDC_QQ_CARDW },
    { IDC_CK_THUMBS,    IDC_QQ_SHOW },
    { IDC_CK_PEEK,      IDC_QQ_PEEK },
    { IDC_CK_CLOSEBTN,  IDC_QQ_CLOSEBTN },
    { IDC_LB_EXCLUDE,   IDC_QQ_EXCLUDE },
    { IDC_LB_VD,        IDC_QQ_VD },
    { IDC_LB_TB_PSIZE,  IDC_QQ_TB_PSIZE },
    { IDC_LB_TB_HSIZE,  IDC_QQ_TB_HSIZE },
    { IDC_CK_TB_PBG,    IDC_QQ_TB_PBG },
    { IDC_CK_TB_HBG,    IDC_QQ_TB_HBG },
    { IDC_LB_TB_SHAPE,  IDC_QQ_TB_SHAPE },
    { IDC_LB_TB_SPOT,   IDC_QQ_TB_SPOT },
    { IDC_LB_TB_PERSIST, IDC_QQ_TB_PERSIST },
    { IDC_LB_TB_HOVER,  IDC_QQ_TB_HOVER },
    { IDC_LB_TB_APPEAR, IDC_QQ_TBCOLOR },
    // Switch rows: (?) hugs the switch label, not a fixed far x.
    { IDC_SW_ENABLE,    IDC_QQ_ENABLE },
    { IDC_SW_ARROWS,    IDC_QQ_TB_ARROWS },
    { IDC_SW_STARTWIN,  IDC_QQ_START },
};

// Width from a control's left edge to the end of its text. For plain
// labels that is just the text width; for checkboxes/radios the drawn
// layout is glyph(17) + gap(8) + text - BCM_GETIDEALSIZE reports the
// NATIVE glyph metrics, which no longer match the teal paint (the text
// would clip and the (?) would sit inside the label,). Switches
// draw their capsule + gap before the label : hug the (?) to the
// switch's text instead of a fixed far-right x).
constexpr wchar_t kSwitchClass[] = L"EdgePopSwitch";

int TextEndOffset(HWND ctrl, HDC dc, HFONT font) {
    wchar_t text[128];
    const int len = GetWindowTextW(ctrl, text, 128);
    if (len <= 0) return 0;
    HGDIOBJ oldFont = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text, len, &sz);
    SelectObject(dc, oldFont);
    const float sc = static_cast<float>(CtrlDpi(ctrl)) / 96.0f;
    wchar_t cls[24];
    if (GetClassNameW(ctrl, cls, 24) > 0 &&
        _wcsicmp(cls, kSwitchClass) == 0) {
        return static_cast<int>((16.0f * 1.85f + 9.0f) * sc) + sz.cx;
    }
    const LONG_PTR style = GetWindowLongPtrW(ctrl, GWL_STYLE);
    const LONG_PTR kind = style & 0xF;
    const bool isButton = (kind >= BS_CHECKBOX && kind <= BS_RADIOBUTTON) ||
                          kind == BS_AUTORADIOBUTTON;
    if (isButton) {
        return static_cast<int>(25.0f * sc) + sz.cx;
    }
    return sz.cx;
}

void HugHelpButtons(HWND dlg) {
    const HFONT font =
        reinterpret_cast<HFONT>(SendMessageW(dlg, WM_GETFONT, 0, 0));
    HDC dc = GetDC(dlg);
    for (const auto& hp : kHugs) {
        HWND label = GetDlgItem(dlg, hp.textCtrl);
        HWND qq = GetDlgItem(dlg, hp.qqCtrl);
        if (!label || !qq) continue;
        RECT rcL, rcQ;
        GetWindowRect(label, &rcL);
        GetWindowRect(qq, &rcQ);
        const int labelH = rcL.bottom - rcL.top;   // both screen: true height
        // Convert BOTH corners: ScreenToClient would only fix (left, top),
        // leaving right/bottom in screen space (the old
        // centering formula cancelled the error out by accident; using the
        // height directly did not and stretched checkboxes into towers).
        MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&rcL), 2);
        MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&rcQ), 2);

        // Checkbox labels fill their whole control rect when they repaint
        // (hot tracking), which painted over the (?) sitting inside it -
        // the "(?) disappears on hover" bug. Shrink checkbox
        // controls to our DRAWN width so nothing
        // overlaps, and place the (?) directly above its label in the
        // z-order so a label repaint can never cover it again. Switches
        // are never shrunk - their whole rect is the click target and the
        // label would clip.
        const LONG_PTR kind = GetWindowLongPtrW(label, GWL_STYLE) & 0xF;
        const bool isButton = (kind >= BS_CHECKBOX && kind <= BS_RADIOBUTTON) ||
                              kind == BS_AUTORADIOBUTTON;
        wchar_t labelCls[24];
        GetClassNameW(label, labelCls, 24);
        const bool isSwitch = _wcsicmp(labelCls, kSwitchClass) == 0;
        if (isButton && !isSwitch) {
            const int w = TextEndOffset(label, dc, font);
            if (w > 0) {
                SetWindowPos(label, nullptr, 0, 0,
                             w + 4, labelH,
                             SWP_NOMOVE | SWP_NOZORDER);
            }
        }

        const int textEnd = TextEndOffset(label, dc, font);
        const int qqH = rcQ.bottom - rcQ.top;
        const int y = rcL.top + (rcL.bottom - rcL.top) / 2 - qqH / 2;
        // round 2: statics/labels keep the plain form - broad
        // z-moves on the hidden modal dialog corrupt win32k's cached
        // visible regions and the row statics get mass-erased ~150 ms
        // after open (frame-captured). The ONE safe exception is the three
        // switch rows: their (?) sits inside the switch's opaque repaint
        // area, so it needs to sit ABOVE the switch (a one-slot lift), with
        // WS_CLIPSIBLINGS on the switch keeping its paints out of the (?).
        // Clicks are additionally routed in SwitchProc.
        if (isSwitch) {
            HWND zAfter = GetWindow(label, GW_HWNDPREV);
            SetWindowPos(qq, zAfter ? zAfter : HWND_TOP,
                         rcL.left + textEnd + 3, y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
            SetWindowLongPtrW(label, GWL_STYLE,
                              GetWindowLongPtrW(label, GWL_STYLE) |
                                  WS_CLIPSIBLINGS);
        } else {
            SetWindowPos(qq, label, rcL.left + textEnd + 3, y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    ReleaseDC(dlg, dc);
}

// --- the activation pill -----------------------------------
//
// A two-segment toggle drawn with D2D (GDI has no antialiased rounded
// rects). Click a half (or press Space) to select it; the control updates
// g_pillMode and notifies the parent with a BN_CLICKED WM_COMMAND.

constexpr wchar_t kPillClass[] = L"EdgePopPill";
constexpr wchar_t kPillLeft[]  = L"Stop at edge";
constexpr wchar_t kPillRight[] = L"Cross & return";


void PillNotifyParent(HWND pill) {
    HWND parent = GetParent(pill);
    if (parent) {
        SendMessageW(parent, WM_COMMAND,
                     MAKEWPARAM(GetDlgCtrlID(pill), BN_CLICKED),
                     reinterpret_cast<LPARAM>(pill));
    }
}

void DrawPill(HWND pill) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(pill, &ps);
    RECT rc;
    GetClientRect(pill, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    do {
        if (w <= 0 || h <= 0) break;
        if (!EnsureUiResources() ||
            FAILED(g_uiRT->BindDC(dc, &rc))) {
            // Fallback: plain GDI so the control is never blank.
            FillRect(dc, &rc, BgBrush());
            HFONT font = reinterpret_cast<HFONT>(
                SendMessageW(GetParent(pill), WM_GETFONT, 0, 0));
            HGDIOBJ old = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            RECT half = rc;
            half.right = rc.left + w / 2;
            SetTextColor(dc, g_pillMode == Activation::EdgeDwell
                                 ? kColAccDark : kColSoft);
            DrawTextW(dc, kPillLeft, -1, &half, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            half = rc;
            half.left = rc.left + w / 2;
            SetTextColor(dc, g_pillMode == Activation::CrossBack
                                 ? kColAccDark : kColSoft);
            DrawTextW(dc, kPillRight, -1, &half, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
            break;
        }
        g_uiRT->BeginDraw();
        const float fw = static_cast<float>(w);
        const float fh = static_cast<float>(h);

        // Track: card fill + border that warms up on hover.
        const D2D1_RECT_F tr = D2D1::RectF(0.5f, 0.5f, fw - 0.5f, fh - 0.5f);
        const float trR = fh / 2 - 0.5f;
        g_uiBrush->SetColor(kDCard);
        g_uiRT->FillRoundedRectangle(D2D1::RoundedRect(tr, trR, trR),
                                       g_uiBrush.Get());
        g_uiBrush->SetColor(g_pillHot ? kDAccent : kDBorder);
        g_uiRT->DrawRoundedRectangle(D2D1::RoundedRect(tr, trR, trR),
                                       g_uiBrush.Get(), 1.0f);

        // Thumb over the selected half.
        const bool left = g_pillMode == Activation::EdgeDwell;
        const D2D1_RECT_F th = left
            ? D2D1::RectF(2.0f, 2.0f, fw / 2 - 2.0f, fh - 2.0f)
            : D2D1::RectF(fw / 2 + 2.0f, 2.0f, fw - 2.0f, fh - 2.0f);
        const float thR = (fh - 4.0f) / 2 - 0.5f;
        g_uiBrush->SetColor(kDAccLight);
        g_uiRT->FillRoundedRectangle(D2D1::RoundedRect(th, thR, thR),
                                       g_uiBrush.Get());
        g_uiBrush->SetColor(kDAccent);
        g_uiRT->DrawRoundedRectangle(D2D1::RoundedRect(th, thR, thR),
                                       g_uiBrush.Get(), 1.0f);

        // Labels: selected half in accent-dark semibold, other in soft.
        g_uiBrush->SetColor(left ? kDAccDark : kDSoft);
        g_uiRT->DrawTextW(kPillLeft, UINT32(lstrlenW(kPillLeft)),
                            g_pillFmt.Get(),
                            D2D1::RectF(0, 0, fw / 2, fh), g_uiBrush.Get());
        g_uiBrush->SetColor(left ? kDSoft : kDAccDark);
        g_uiRT->DrawTextW(kPillRight, UINT32(lstrlenW(kPillRight)),
                            g_pillFmtSel.Get(),
                            D2D1::RectF(fw / 2, 0, fw, fh), g_uiBrush.Get());
        g_uiRT->EndDraw();
    } while (false);
    EndPaint(pill, &ps);
}

LRESULT CALLBACK PillProc(HWND pill, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_BUTTON | DLGC_WANTMESSAGE;
        case WM_ENABLE:   // redraw in the new state
            InvalidateRect(pill, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND: {
            // The D2D track blends over the cream dialog background.
            RECT rc;
            GetClientRect(pill, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, BgBrush());
            return 1;
        }
        case WM_PAINT:
            DrawPill(pill);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT rc;
            GetClientRect(pill, &rc);
            g_pillMode = pt.x < (rc.right - rc.left) / 2
                             ? Activation::EdgeDwell
                             : Activation::CrossBack;
            InvalidateRect(pill, nullptr, FALSE);
            PillNotifyParent(pill);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_SPACE) {
                g_pillMode = g_pillMode == Activation::EdgeDwell
                                 ? Activation::CrossBack
                                 : Activation::EdgeDwell;
                InvalidateRect(pill, nullptr, FALSE);
                PillNotifyParent(pill);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (!g_pillHot) {
                g_pillHot = true;
                InvalidateRect(pill, nullptr, FALSE);
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, pill, 0 };
                TrackMouseEvent(&tme);
            }
            return 0;
        case WM_MOUSELEAVE:
            g_pillHot = false;
            InvalidateRect(pill, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
    }
    return DefWindowProcW(pill, msg, wp, lp);
}

LRESULT CALLBACK SwitchProc(HWND c, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK TabsProc(HWND c, UINT msg, WPARAM wp, LPARAM lp);
constexpr wchar_t kTabsClass[] = L"EdgePopTabs";

void RegisterPillClass(HINSTANCE inst) {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PillProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kPillClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW ws = wc;
    ws.lpfnWndProc = &SwitchProc;
    ws.lpszClassName = kSwitchClass;
    RegisterClassExW(&ws);

    WNDCLASSEXW wt = wc;
    wt.lpfnWndProc = &TabsProc;
    wt.lpszClassName = kTabsClass;
    RegisterClassExW(&wt);
}

// --- on/off switch control ---------------------------------------------
//
// An iOS-style capsule toggle: thumb slides left(off)/right(on), the label
// is the window text. Notifies the parent with BN_CLICKED on click/Space;
// state lives in a window property the dialog reads via SwitchChecked and
// sets via SwitchSet (no BM_* semantics - it is not a Button-class control).

const wchar_t* kSwOnProp = L"EdgePopSwOn";

bool SwitchChecked(HWND c) {
    return GetPropW(c, kSwOnProp) != nullptr;
}

void SwitchSet(HWND c, bool on) {
    if (on) {
        SetPropW(c, kSwOnProp, reinterpret_cast<HANDLE>(
                                   static_cast<INT_PTR>(1)));
    } else {
        RemovePropW(c, kSwOnProp);
    }
    InvalidateRect(c, nullptr, FALSE);
}

void DrawSwitch(HWND c) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(c, &ps);
    RECT rc;
    GetClientRect(c, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    do {
        if (w <= 0 || h <= 0) break;
        if (!EnsureUiResources() || FAILED(g_uiRT->BindDC(dc, &rc))) {
            // Fallback: never a blank control.
            FillRect(dc, &rc, BgBrush());
            break;
        }
        g_uiRT->BeginDraw();
        const float fh = static_cast<float>(h);
        const bool on = SwitchChecked(c);
        const bool hot = CtrlHot(c);

        g_uiBrush->SetColor(kDBg);
        g_uiRT->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(w), fh),
                              g_uiBrush.Get());

        const float gh = Scaled(c, 16.0f);          // glyph height
        const float gw = gh * 1.85f;                // track
        const float gy = (fh - gh) / 2;
        const float trR = gh / 2 - 0.5f;
        const D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
            D2D1::RectF(0.5f, gy + 0.5f, 0.5f + gw, gy + 0.5f + gh),
            trR, trR);
        g_uiBrush->SetColor(on ? kDAccent : kDCard);
        g_uiRT->FillRoundedRectangle(track, g_uiBrush.Get());
        g_uiBrush->SetColor(hot ? kDAccent : kDBorder);
        g_uiRT->DrawRoundedRectangle(track, g_uiBrush.Get(), 1.0f);

        const float tw = gh * 0.74f;                // thumb
        const float tx = on ? 0.5f + gw - tw - Scaled(c, 2.0f)
                            : 0.5f + Scaled(c, 2.0f);
        const D2D1_ROUNDED_RECT thumb = D2D1::RoundedRect(
            D2D1::RectF(tx, gy + Scaled(c, 2.0f), tx + tw,
                        gy + gh - Scaled(c, 2.0f)),
            tw / 2, tw / 2);
        g_uiBrush->SetColor(kDCard);
        g_uiRT->FillRoundedRectangle(thumb, g_uiBrush.Get());
        g_uiBrush->SetColor(kDAccDark);
        g_uiRT->DrawRoundedRectangle(thumb, g_uiBrush.Get(), 0.8f);

        wchar_t text[128];
        const int len = GetWindowTextW(c, text, 128);
        if (len > 0) {
            if (IDWriteTextFormat* fmt = CtrlTextFormat(c)) {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                g_uiBrush->SetColor(!IsWindowEnabled(c) ? kDSoft : kDText);
                g_uiRT->DrawTextW(
                    text, UINT32(len), fmt,
                    D2D1::RectF(gw + Scaled(c, 9.0f), 0,
                                static_cast<float>(w), fh),
                    g_uiBrush.Get());
            }
        }
        g_uiRT->EndDraw();
    } while (false);
    EndPaint(c, &ps);
}

// The (?) hugging this switch (kHugs pairs), or null. The (?) sits INSIDE
// the switch's big click rect (and, in the template's z-order, under it).
HWND SwitchHelpButton(HWND sw) {
    const int id = GetDlgCtrlID(sw);
    int qqId = 0;
    if (id == IDC_SW_ENABLE) qqId = IDC_QQ_ENABLE;
    else if (id == IDC_SW_ARROWS) qqId = IDC_QQ_TB_ARROWS;
    else if (id == IDC_SW_STARTWIN) qqId = IDC_QQ_START;
    else return nullptr;
    HWND p = GetParent(sw);
    return p ? GetDlgItem(p, qqId) : nullptr;
}

LRESULT CALLBACK SwitchProc(HWND c, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_BUTTON | DLGC_WANTMESSAGE;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(c, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, BgBrush());
            return 1;
        }
        case WM_PAINT:
            DrawSwitch(c);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            // A click on the (?) (it lives inside this rect, under us in
            // z) must open the help card, not toggle the switch - the
            // "(?) doesn't work on click" bug. Route it through.
            if (HWND qq = SwitchHelpButton(c)) {
                RECT rq{};
                GetWindowRect(qq, &rq);
                POINT pt{ static_cast<short>(LOWORD(lp)),
                          static_cast<short>(HIWORD(lp)) };
                MapWindowPoints(c, nullptr, &pt, 1);
                if (PtInRect(&rq, pt)) {
                    PostMessageW(qq, WM_LBUTTONDOWN, wp, 0);
                    PostMessageW(qq, WM_LBUTTONUP, 0, 0);
                    return 0;
                }
            }
            SwitchSet(c, !SwitchChecked(c));
            if (HWND p = GetParent(c)) {
                SendMessageW(p, WM_COMMAND,
                             MAKEWPARAM(GetDlgCtrlID(c), BN_CLICKED),
                             reinterpret_cast<LPARAM>(c));
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_SPACE || wp == VK_RETURN) {
                SwitchSet(c, !SwitchChecked(c));
                if (HWND p = GetParent(c)) {
                    SendMessageW(p, WM_COMMAND,
                                 MAKEWPARAM(GetDlgCtrlID(c), BN_CLICKED),
                                 reinterpret_cast<LPARAM>(c));
                }
            }
            return 0;
        case WM_MOUSEMOVE:
            if (!CtrlHot(c)) {
                SetPropW(c, kHotProp, reinterpret_cast<HANDLE>(1));
                InvalidateRect(c, nullptr, FALSE);
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, c, 0 };
                TrackMouseEvent(&tme);
            }
            return 0;
        case WM_MOUSELEAVE:
            RemovePropW(c, kHotProp);
            InvalidateRect(c, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_NCDESTROY:
            RemovePropW(c, kSwOnProp);
            RemovePropW(c, kHotProp);
            return 0;
    }
    return DefWindowProcW(c, msg, wp, lp);
}

// --- tab strip control --------------------------------------------------
//
// A two-segment capsule: "Edge popup" / "Taskbar arrows". Selected segment
// is teal with white semibold text; the other is a white card. Click a
// segment (or Space / Left / Right) to switch; the parent gets BN_CLICKED
// and shows/hides the page's controls. State lives in a window property
// like the switch.

const wchar_t* kTabSelProp = L"EdgePopTabSel";   // set = page 1 (taskbar)
const wchar_t* kTabLabels[2] = { L"Edge popup", L"Taskbar arrows" };

bool TabsSel(HWND c) {
    return GetPropW(c, kTabSelProp) != nullptr;
}

void TabsSet(HWND c, bool taskbar, bool notify) {
    if (taskbar) {
        SetPropW(c, kTabSelProp,
                 reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
    } else {
        RemovePropW(c, kTabSelProp);
    }
    InvalidateRect(c, nullptr, FALSE);
    if (notify && GetParent(c)) {
        SendMessageW(GetParent(c), WM_COMMAND,
                     MAKEWPARAM(GetDlgCtrlID(c), BN_CLICKED),
                     reinterpret_cast<LPARAM>(c));
    }
}

void DrawTabs(HWND c) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(c, &ps);
    RECT rc;
    GetClientRect(c, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    do {
        if (w <= 0 || h <= 0) break;
        if (!EnsureUiResources() || FAILED(g_uiRT->BindDC(dc, &rc))) {
            FillRect(dc, &rc, BgBrush());
            break;
        }
        g_uiRT->BeginDraw();
        const float fh = static_cast<float>(h);
        const float gap = Scaled(c, 4.0f);
        const float segW = (static_cast<float>(w) - gap) / 2;
        const bool selTaskbar = TabsSel(c);
        POINT cur{};
        GetCursorPos(&cur);
        ScreenToClient(c, &cur);

        g_uiBrush->SetColor(kDBg);
        g_uiRT->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(w), fh),
                              g_uiBrush.Get());
        for (int i = 0; i < 2; ++i) {
            const float x = i == 0 ? 0.5f : 0.5f + segW + gap;
            const bool sel = (i == 1) == selTaskbar;
            const bool hot = CtrlHot(c) &&
                             cur.x >= x && cur.x < x + segW;
            const float r = fh / 2 - 0.5f;
            const D2D1_ROUNDED_RECT seg = D2D1::RoundedRect(
                D2D1::RectF(x, 0.5f, x + segW, fh - 0.5f), r, r);
            g_uiBrush->SetColor(sel ? kDAccent : kDCard);
            g_uiRT->FillRoundedRectangle(seg, g_uiBrush.Get());
            g_uiBrush->SetColor(sel ? kDAccDark
                                    : (hot ? kDAccent : kDBorder));
            g_uiRT->DrawRoundedRectangle(seg, g_uiBrush.Get(), 1.0f);
            if (IDWriteTextFormat* fmt = sel ? g_pillFmtSel.Get()
                                             : g_pillFmt.Get()) {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                g_uiBrush->SetColor(sel ? kDCard : kDText);
                g_uiRT->DrawTextW(
                    kTabLabels[i],
                    static_cast<UINT32>(wcslen(kTabLabels[i])), fmt,
                    D2D1::RectF(x, 0, x + segW, fh), g_uiBrush.Get());
            }
        }
        g_uiRT->EndDraw();
    } while (false);
    EndPaint(c, &ps);
}

LRESULT CALLBACK TabsProc(HWND c, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_BUTTON | DLGC_WANTMESSAGE;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(c, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, BgBrush());
            return 1;
        }
        case WM_PAINT:
            DrawTabs(c);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT rc;
            GetClientRect(c, &rc);
            TabsSet(c, pt.x >= (rc.right - rc.left) / 2, true);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_SPACE || wp == VK_RETURN || wp == VK_RIGHT) {
                TabsSet(c, true, true);
            } else if (wp == VK_LEFT) {
                TabsSet(c, false, true);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (!CtrlHot(c)) {
                SetPropW(c, kHotProp, reinterpret_cast<HANDLE>(1));
                InvalidateRect(c, nullptr, FALSE);
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, c, 0 };
                TrackMouseEvent(&tme);
            }
            return 0;
        case WM_MOUSELEAVE:
            RemovePropW(c, kHotProp);
            InvalidateRect(c, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_NCDESTROY:
            RemovePropW(c, kTabSelProp);
            RemovePropW(c, kHotProp);
            return 0;
    }
    return DefWindowProcW(c, msg, wp, lp);
}

// --- activation rows ------------------------------------------
//
// One fixed-size "Activation" group holds the pill and the mode's rows;
// switching just shows/hides the row sets - the dialog never resizes
//.

const int kEdgeRowIds[] = {
    IDC_LB_DWELL, IDC_QQ_DWELL, IDC_SL_DWELL, IDC_ED_DWELL,
    IDC_LB_ZONE, IDC_QQ_ZONE, IDC_SL_ZONE, IDC_ED_ZONE,
    IDC_LB_STOP, IDC_QQ_STOP, IDC_SL_STOP, IDC_ED_STOP,
};
const int kCrossRowIds[] = {
    IDC_LB_CROSS, IDC_QQ_CROSS, IDC_SL_CROSS, IDC_ED_CROSS,
};

// Everything inside the "Edge Popup" box: greys out while the
// Activate-edge-popup switch is off.
const int kEdgePopupDependentIds[] = {
    IDC_PILL_ACT, IDC_QQ_ACT,
    IDC_LB_DWELL, IDC_QQ_DWELL, IDC_SL_DWELL, IDC_ED_DWELL,
    IDC_LB_ZONE, IDC_QQ_ZONE, IDC_SL_ZONE, IDC_ED_ZONE,
    IDC_LB_STOP, IDC_QQ_STOP, IDC_SL_STOP, IDC_ED_STOP,
    IDC_LB_CROSS, IDC_QQ_CROSS, IDC_SL_CROSS, IDC_ED_CROSS,
    IDC_CK_FSGUARD, IDC_QQ_FGS,
    IDC_LB_EDGES, IDC_QQ_EDGES, IDC_RD_ED_AUTO, IDC_RD_ED_SIDES,
    IDC_CK_EDGE_L, IDC_CK_EDGE_R, IDC_CK_EDGE_T, IDC_CK_EDGE_B,
    IDC_LB_REDELAY, IDC_QQ_REDELAY, IDC_SL_REDELAY, IDC_ED_REDELAY,
    IDC_LB_SIDE, IDC_QQ_SIDE, IDC_RD_SIDE_MAIN, IDC_RD_SIDE_NB,
    IDC_LB_CHAIN, IDC_QQ_CHAIN, IDC_RD_CHAIN_NB, IDC_RD_CHAIN_DIR,
    IDC_LB_GRACE, IDC_QQ_GRACE, IDC_SL_GRACE, IDC_ED_GRACE,
    IDC_LB_STAY, IDC_QQ_STAY, IDC_SL_STAY, IDC_ED_STAY,
    IDC_LB_TITLE, IDC_QQ_TITLE, IDC_SL_TITLE, IDC_ED_TITLE,
    IDC_LB_CARDW, IDC_QQ_CARDW, IDC_SL_CARDW, IDC_ED_CARDW,
    IDC_CK_ICONS, IDC_CK_TITLES, IDC_CK_THUMBS, IDC_QQ_SHOW,
    IDC_CK_PEEK, IDC_QQ_PEEK,
    IDC_CK_CLOSEBTN, IDC_QQ_CLOSEBTN,
    IDC_LB_VD, IDC_QQ_VD, IDC_RD_VD_CUR, IDC_RD_VD_ALL,
};

// Everything inside the "Taskbar screen indicators" box: greys out
// while the Activate-arrows switch is off.
const int kTaskbarDependentIds[] = {
    IDC_LB_TB_PERSIST, IDC_QQ_TB_PERSIST, IDC_CK_TB_PNUM, IDC_CK_TB_PARR,
    IDC_LB_TB_HOVER, IDC_QQ_TB_HOVER, IDC_CK_TB_NUM, IDC_CK_TB_ARR,
    IDC_LB_TB_PSIZE, IDC_QQ_TB_PSIZE, IDC_SL_TB_PSIZE, IDC_ED_TB_PSIZE,
    IDC_CK_TB_PBG, IDC_QQ_TB_PBG,
    IDC_LB_TB_HSIZE, IDC_QQ_TB_HSIZE, IDC_SL_TB_HSIZE, IDC_ED_TB_HSIZE,
    IDC_CK_TB_HBG, IDC_QQ_TB_HBG,
    IDC_LB_TB_SHAPE, IDC_QQ_TB_SHAPE,
    IDC_RD_TB_TRI, IDC_RD_TB_CHEV, IDC_RD_TB_ARR,
    IDC_LB_TB_SPOT, IDC_QQ_TB_SPOT,
    IDC_RD_TB_POS_L, IDC_RD_TB_POS_C, IDC_RD_TB_POS_R,
    IDC_LB_TB_APPEAR, IDC_QQ_TBCOLOR,
    IDC_BTN_TB_COLOR, IDC_ST_TBSWATCH, IDC_CK_TB_SHADOW,
};

void SetIdsEnabled(HWND dlg, const int* ids, size_t n, bool on) {
    for (size_t i = 0; i < n; ++i) {
        HWND c = GetDlgItem(dlg, ids[i]);
        if (c) EnableWindow(c, on);
    }
}

void UpdateEdgePopupEnabledRows(HWND dlg) {
    HWND sw = GetDlgItem(dlg, IDC_SW_ENABLE);
    const bool on = sw ? SwitchChecked(sw) : true;
    SetIdsEnabled(dlg, kEdgePopupDependentIds,
                  sizeof(kEdgePopupDependentIds) / sizeof(int), on);
}

void UpdateTaskbarEnabledRows(HWND dlg) {
    HWND sw = GetDlgItem(dlg, IDC_SW_ARROWS);
    const bool on = sw ? SwitchChecked(sw) : true;
    SetIdsEnabled(dlg, kTaskbarDependentIds,
                  sizeof(kTaskbarDependentIds) / sizeof(int), on);
}

// --- tab pages ----------------------------------------------------------
//
// Every control that lives on one page. The dependent-ids arrays already
// enumerate each box's inner controls; pages add the boxes themselves and
// the master-switch rows.

const int kEdgePageIds[] = {
    IDC_SW_ENABLE, IDC_QQ_ENABLE,
    IDC_GB_EDGEPOP, IDC_GB_ACT, IDC_GB_TRIGGER, IDC_GB_POPUP, IDC_GB_WINDOWS,
};
const int kTaskbarPageIds[] = {
    IDC_SW_ARROWS, IDC_QQ_TB_ARROWS,
    IDC_GB_TBAR, IDC_GB_PERSIST, IDC_GB_HOVER,
};

void ShowIds(HWND dlg, const int* ids, size_t n, bool show) {
    for (size_t i = 0; i < n; ++i) {
        HWND c = GetDlgItem(dlg, ids[i]);
        if (c) ShowWindow(c, show ? SW_SHOW : SW_HIDE);
    }
}

void UpdateActivationRows(HWND dlg);   // defined below the tab-page block

void UpdateActivePage(HWND dlg) {
    HWND tabs = GetDlgItem(dlg, IDC_TABS);
    const bool edge = tabs ? !TabsSel(tabs) : true;
    ShowIds(dlg, kEdgePageIds, sizeof(kEdgePageIds) / sizeof(int), edge);
    ShowIds(dlg, kEdgePopupDependentIds,
            sizeof(kEdgePopupDependentIds) / sizeof(int), edge);
    // Both activation-mode row sets live in kEdgePopupDependentIds, so the
    // show above re-reveals the inactive set over the active one (dwell and
    // cross rows share slots) - re-apply the mode split, or every dialog
    // open and tab return stacks the two modes (a latent bug since the
    // tabs landed, unmasked by cross-back mode + the boxes going away).
    if (edge) UpdateActivationRows(dlg);
    ShowIds(dlg, kTaskbarPageIds,
            sizeof(kTaskbarPageIds) / sizeof(int), !edge);
    ShowIds(dlg, kTaskbarDependentIds,
            sizeof(kTaskbarDependentIds) / sizeof(int), !edge);
    // Focus may sit on a control that just hid - park it on the strip.
    HWND f = GetFocus();
    if (f && GetParent(f) == dlg && !IsWindowVisible(f) && tabs) {
        SetFocus(tabs);
    }
}

void UpdateActivationRows(HWND dlg) {
    const bool edge = g_pillMode == Activation::EdgeDwell;
    for (int id : kEdgeRowIds) {
        HWND c = GetDlgItem(dlg, id);
        if (c) ShowWindow(c, edge ? SW_SHOW : SW_HIDE);
    }
    for (int id : kCrossRowIds) {
        HWND c = GetDlgItem(dlg, id);
        if (c) ShowWindow(c, edge ? SW_HIDE : SW_SHOW);
    }
}

// --- edges: Auto OR sides -----------------------------------

void UpdateEdgesMode(HWND dlg) {
    const bool isAuto =
        IsDlgButtonChecked(dlg, IDC_RD_ED_AUTO) == BST_CHECKED;
    static const int sideIds[] = { IDC_CK_EDGE_L, IDC_CK_EDGE_R,
                                   IDC_CK_EDGE_T, IDC_CK_EDGE_B };
    bool any = false;
    for (int id : sideIds) {
        if (IsDlgButtonChecked(dlg, id) == BST_CHECKED) any = true;
    }
    // Leaving Auto with nothing checked would disable every edge - default
    // to all four (same coverage as Auto).
    if (!isAuto && !any) {
        for (int id : sideIds) CheckDlgButton(dlg, id, BST_CHECKED);
        any = true;
    }
    for (int id : sideIds) {
        HWND c = GetDlgItem(dlg, id);
        if (c) EnableWindow(c, !isAuto);
    }
}

// --- control <-> settings ----------------------------------------------------

void SetCheck(HWND dlg, int id, bool v) {
    CheckDlgButton(dlg, id, v ? BST_CHECKED : BST_UNCHECKED);
}

bool GetCheck(HWND dlg, int id) {
    return IsDlgButtonChecked(dlg, id) == BST_CHECKED;
}

void SyncControls(HWND dlg) {
    g_syncing = true;
    g_pillMode = g_settings->activation;
    for (const auto& sl : kSliders) {
        const int v = static_cast<int>(g_settings->*sl.field);
        SendDlgItemMessageW(dlg, sl.sliderId, TBM_SETRANGE, TRUE,
                            MAKELPARAM(sl.minV, sl.maxV));
        SendDlgItemMessageW(dlg, sl.sliderId, TBM_SETPOS, TRUE, v);
        wchar_t buf[16];
        _itow_s(v, buf, 10);
        SetDlgItemTextW(dlg, sl.editId, buf);
    }
    SetCheck(dlg, IDC_CK_FSGUARD, g_settings->fullscreenGuard);
    SetCheck(dlg, IDC_RD_ED_AUTO, g_settings->edgesAuto);
    SetCheck(dlg, IDC_RD_ED_SIDES, !g_settings->edgesAuto);
    SetCheck(dlg, IDC_CK_EDGE_L, (g_settings->edgeMask & kEdgeLeftBit) != 0);
    SetCheck(dlg, IDC_CK_EDGE_R, (g_settings->edgeMask & kEdgeRightBit) != 0);
    SetCheck(dlg, IDC_CK_EDGE_T, (g_settings->edgeMask & kEdgeTopBit) != 0);
    SetCheck(dlg, IDC_CK_EDGE_B, (g_settings->edgeMask & kEdgeBottomBit) != 0);
    SetCheck(dlg, IDC_RD_SIDE_MAIN, g_settings->side == PopupSide::Main);
    SetCheck(dlg, IDC_RD_SIDE_NB, g_settings->side == PopupSide::Neighbor);
    SetCheck(dlg, IDC_RD_CHAIN_NB, g_settings->chainMode == ChainMode::Neighbor);
    SetCheck(dlg, IDC_RD_CHAIN_DIR, g_settings->chainMode == ChainMode::Direction);
    SetCheck(dlg, IDC_CK_ICONS, g_settings->showIcons);
    SetCheck(dlg, IDC_CK_TITLES, g_settings->showTitles);
    SetCheck(dlg, IDC_CK_THUMBS, g_settings->thumbnails);
    SetCheck(dlg, IDC_CK_PEEK, g_settings->peek);
    SetCheck(dlg, IDC_CK_CLOSEBTN, g_settings->cardCloseButtons);
    SetCheck(dlg, IDC_RD_VD_CUR, g_settings->virtualDesktops == VirtualDesktops::Current);
    SetCheck(dlg, IDC_RD_VD_ALL, g_settings->virtualDesktops == VirtualDesktops::All);
    if (HWND sw = GetDlgItem(dlg, IDC_SW_ENABLE)) {
        SwitchSet(sw, g_settings->activationEnabled);   //
    }
    if (HWND sw = GetDlgItem(dlg, IDC_SW_ARROWS)) {
        SwitchSet(sw, g_settings->taskbarArrows);       //
    }
    if (HWND sw = GetDlgItem(dlg, IDC_SW_STARTWIN)) {
        SwitchSet(sw, g_settings->startWithWindows);    //
    }
    SetCheck(dlg, IDC_CK_TB_PNUM, g_settings->persistentNumbers);
    SetCheck(dlg, IDC_CK_TB_PARR, g_settings->persistentArrows);
    SetCheck(dlg, IDC_CK_TB_PBG, g_settings->persistentBg);        //
    SetCheck(dlg, IDC_CK_TB_HBG, g_settings->hoverBg);             //
    SetCheck(dlg, IDC_CK_TB_SHADOW, g_settings->arrowShadow);      //
    SetCheck(dlg, IDC_RD_TB_TRI,
             g_settings->arrowShape == ArrowShape::Triangle);
    SetCheck(dlg, IDC_RD_TB_CHEV,
             g_settings->arrowShape == ArrowShape::Chevron);
    SetCheck(dlg, IDC_RD_TB_ARR,
             g_settings->arrowShape == ArrowShape::Arrow);
    SetCheck(dlg, IDC_RD_TB_POS_L,
             g_settings->arrowPos == ArrowPos::Left);
    SetCheck(dlg, IDC_RD_TB_POS_C,
             g_settings->arrowPos == ArrowPos::Center);
    SetCheck(dlg, IDC_RD_TB_POS_R,
             g_settings->arrowPos == ArrowPos::Right);
    SetCheck(dlg, IDC_CK_TB_NUM, g_settings->hoverNumbers);
    SetCheck(dlg, IDC_CK_TB_ARR, g_settings->hoverArrows);
    g_syncing = false;
    UpdateEdgesMode(dlg);
    UpdateActivationRows(dlg);
    UpdateEdgePopupEnabledRows(dlg);
    UpdateTaskbarEnabledRows(dlg);
    if (HWND swatch = GetDlgItem(dlg, IDC_ST_TBSWATCH)) {
        InvalidateRect(swatch, nullptr, TRUE);   // color may have changed
    }
}

void ReadInto(HWND dlg, Settings& s) {
    for (const auto& sl : kSliders) {
        wchar_t buf[16] = L"";
        if (GetDlgItemTextW(dlg, sl.editId, buf, 16) > 0) {
            int v = _wtoi(buf);
            if (v < sl.minV) v = sl.minV;
            if (v > sl.maxV) v = sl.maxV;
            s.*sl.field = static_cast<unsigned>(v);
        }
    }
    s.activation = g_pillMode;
    s.fullscreenGuard = GetCheck(dlg, IDC_CK_FSGUARD);
    s.edgesAuto = GetCheck(dlg, IDC_RD_ED_AUTO);
    s.edgeMask = (GetCheck(dlg, IDC_CK_EDGE_L) ? kEdgeLeftBit : 0) |
                 (GetCheck(dlg, IDC_CK_EDGE_R) ? kEdgeRightBit : 0) |
                 (GetCheck(dlg, IDC_CK_EDGE_T) ? kEdgeTopBit : 0) |
                 (GetCheck(dlg, IDC_CK_EDGE_B) ? kEdgeBottomBit : 0);
    if (s.edgesAuto) s.edgeMask = 0;
    else if (s.edgeMask == 0) s.edgeMask = kEdgeAll;   // never edge-less
    s.side = GetCheck(dlg, IDC_RD_SIDE_NB) ? PopupSide::Neighbor
                                           : PopupSide::Main;
    s.chainMode = GetCheck(dlg, IDC_RD_CHAIN_DIR)
                      ? ChainMode::Direction
                      : ChainMode::Neighbor;
    s.showIcons = GetCheck(dlg, IDC_CK_ICONS);
    s.showTitles = GetCheck(dlg, IDC_CK_TITLES);
    s.thumbnails = GetCheck(dlg, IDC_CK_THUMBS);
    s.peek = GetCheck(dlg, IDC_CK_PEEK);
    s.cardCloseButtons = GetCheck(dlg, IDC_CK_CLOSEBTN);
    // The popup must always show something, widened to a
    // three-way rule (icons/titles/thumbnails).
    if (!s.showIcons && !s.showTitles && !s.thumbnails) s.showTitles = true;
    s.virtualDesktops = GetCheck(dlg, IDC_RD_VD_ALL)
                            ? VirtualDesktops::All
                            : VirtualDesktops::Current;
    if (HWND sw = GetDlgItem(dlg, IDC_SW_ENABLE)) {
        s.activationEnabled = SwitchChecked(sw);      //
    }
    if (HWND sw = GetDlgItem(dlg, IDC_SW_ARROWS)) {
        s.taskbarArrows = SwitchChecked(sw);          //
    }
    if (HWND sw = GetDlgItem(dlg, IDC_SW_STARTWIN)) {
        s.startWithWindows = SwitchChecked(sw);       //
    }
    s.persistentNumbers = GetCheck(dlg, IDC_CK_TB_PNUM);
    s.persistentArrows = GetCheck(dlg, IDC_CK_TB_PARR);
    s.persistentBg = GetCheck(dlg, IDC_CK_TB_PBG);        //
    s.hoverBg = GetCheck(dlg, IDC_CK_TB_HBG);             //
    s.arrowShadow = GetCheck(dlg, IDC_CK_TB_SHADOW);      //
    s.arrowShape = GetCheck(dlg, IDC_RD_TB_CHEV) ? ArrowShape::Chevron
                   : GetCheck(dlg, IDC_RD_TB_ARR) ? ArrowShape::Arrow
                                                   : ArrowShape::Triangle;
    s.arrowPos = GetCheck(dlg, IDC_RD_TB_POS_R) ? ArrowPos::Right
                : GetCheck(dlg, IDC_RD_TB_POS_C) ? ArrowPos::Center
                                                 : ArrowPos::Left;
    // arrowColor is edited by the Color... button (ChooseColor), not by a
    // control ReadInto can poll - it is preserved by the ApplyNow flow.
    s.hoverNumbers = GetCheck(dlg, IDC_CK_TB_NUM);
    s.hoverArrows = GetCheck(dlg, IDC_CK_TB_ARR);
    // Note: debugStrips is owned by the Advanced dialog (its checkbox
    // applies directly); this dialog must not clobber it.
}

// Persist + apply, keeping the INI as the source of truth.
void ApplyNow(HWND dlg) {
    if (g_syncing || !g_settings || !g_apply) return;
    Settings before = *g_settings;
    ReadInto(dlg, *g_settings);
    g_apply(before, *g_settings);
    if (g_settings->activation != before.activation) {
        UpdateActivationRows(dlg);
    }
}

// --- tooltips ------------------------------------------------------------------

void AttachHelpTool(HWND dlg, const HelpEntry& e) {
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = dlg;
    ti.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(dlg, e.id));
    ti.lpszText = const_cast<wchar_t*>(e.tip);
    SendMessageW(g_tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

// --- advanced dialog ---------------------------------------------

INT_PTR CALLBACK AdvancedProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            SetAppIcon(dlg);
            StyleDialogControls(dlg);
            StyleSiteLink(dlg);
            SetCheck(dlg, IDC_CK_DEBUG, g_settings->debugStrips);
            g_syncing = true;   // the SetDlgItemTextW below fires EN_CHANGE
            SetDlgItemTextW(dlg, IDC_ED_EXCLUDE,
                            g_settings->excludeApps.c_str());
            g_syncing = false;
            HugHelpButtons(dlg);
            return TRUE;
        }
        case WM_CTLCOLORDLG:
            SetBkColor(reinterpret_cast<HDC>(wp), kColBg);
            return reinterpret_cast<INT_PTR>(BgBrush());
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(wp);
            SetBkColor(dc, kColBg);
            SetTextColor(dc, reinterpret_cast<HWND>(lp) ==
                                     GetDlgItem(dlg, IDC_ST_ADVDESC)
                                 ? kColSoft
                                 : kColText);
            return reinterpret_cast<INT_PTR>(BgBrush());
        }
        case WM_CTLCOLOREDIT:   // exclude list, flat teal style
            SetBkColor(reinterpret_cast<HDC>(wp), kColBg);
            SetTextColor(reinterpret_cast<HDC>(wp), kColText);
            return reinterpret_cast<INT_PTR>(BgBrush());
        case WM_DRAWITEM:
            if (IsHelpId(static_cast<int>(wp)))
                DrawHelpButton(dlg, reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDOK || id == IDCANCEL) {
                EndDialog(dlg, id);
                return TRUE;
            }
            if (code == BN_CLICKED) {
                if (IsHelpId(id)) {
                    ShowHelpWindow(dlg, id);
                    return TRUE;
                }
                if (id == IDC_CK_DEBUG && g_settings && g_apply) {
                    const Settings before = *g_settings;
                    g_settings->debugStrips = GetCheck(dlg, IDC_CK_DEBUG);
                    g_apply(before, *g_settings);
                    return TRUE;
                }
                if (id == IDC_BTN_REBUILD && g_advanced) {
                    g_advanced(1);
                    return TRUE;
                }
            }
            if (code == EN_CHANGE && wp != 0 &&
                GetDlgCtrlID(reinterpret_cast<HWND>(lp)) ==
                    IDC_ED_EXCLUDE &&
                g_settings && g_apply && !g_syncing) {
                // The exclude list lives only in this dialog; the main
                // dialog's ReadInto never touches the field, so it can't
                // be clobbered from there.
                wchar_t buf[256] = L"";
                GetDlgItemTextW(dlg, IDC_ED_EXCLUDE, buf, 256);
                const Settings before = *g_settings;
                g_settings->excludeApps = buf;
                g_apply(before, *g_settings);
                return TRUE;
            }
            return FALSE;
        }
    }
    return FALSE;
}

// --- main dialog proc -----------------------------------------------------------

// Vertical scrolling, plain-scrollable-form style: the dialog is
// resizable (WS_THICKFRAME) with WS_VSCROLL; when the client is shorter
// than the content, the scrollbar appears and EVERYTHING scrolls
// together (ScrollWindowEx over the whole client, SW_SCROLLCHILDREN) -
// no fixed zones, no clipping tricks, and the mouse wheel works. Content
// height is measured from the live controls at init.
int g_scrollPos = 0;
int g_contentBottom = 0;    // content bottom (client y at scroll pos 0)

void ScrollDlgTo(HWND dlg, int newPos) {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_PAGE | SIF_RANGE;
    if (!GetScrollInfo(dlg, SB_VERT, &si)) return;
    const int maxS = si.nMax > static_cast<int>(si.nPage)
                         ? si.nMax - static_cast<int>(si.nPage)
                         : 0;
    if (newPos > maxS) newPos = maxS;
    if (newPos < 0) newPos = 0;
    if (newPos == g_scrollPos) return;
    ScrollWindowEx(dlg, 0, g_scrollPos - newPos, nullptr, nullptr, nullptr,
                   nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    g_scrollPos = newPos;
    SetScrollPos(dlg, SB_VERT, newPos, TRUE);
}

void UpdateDialogLayout(HWND dlg) {
    RECT rc;
    GetClientRect(dlg, &rc);
    const int visibleH = rc.bottom;
    const int totalH = g_contentBottom;
    const int maxScroll = totalH > visibleH ? totalH - visibleH : 0;
    if (maxScroll > 0) {
        ShowScrollBar(dlg, SB_VERT, TRUE);
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = totalH;
        si.nPage = static_cast<UINT>(visibleH);
        if (g_scrollPos > maxScroll) ScrollDlgTo(dlg, maxScroll);
        si.nPos = g_scrollPos;
        SetScrollInfo(dlg, SB_VERT, &si, TRUE);
    } else {
        ScrollDlgTo(dlg, 0);   // (no-op when already at 0)
        ShowScrollBar(dlg, SB_VERT, FALSE);
    }
}

// The template is 509 DLU tall; on a short monitor (or a large
// scale factor) that can exceed the work area. Clamp the window inside it -
// the scrollbar then takes over.
void ClampToWorkArea(HWND dlg) {
    HMONITOR mon = MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return;
    RECT rc;
    GetWindowRect(dlg, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    bool move = false;
    if (h > mi.rcWork.bottom - mi.rcWork.top) {
        h = mi.rcWork.bottom - mi.rcWork.top;
        move = true;
    }
    if (w > mi.rcWork.right - mi.rcWork.left) {
        w = mi.rcWork.right - mi.rcWork.left;
        move = true;
    }
    int x = rc.left;
    int y = rc.top;
    if (x < mi.rcWork.left) { x = mi.rcWork.left; move = true; }
    if (y < mi.rcWork.top) { y = mi.rcWork.top; move = true; }
    if (x + w > mi.rcWork.right) { x = mi.rcWork.right - w; move = true; }
    if (y + h > mi.rcWork.bottom) { y = mi.rcWork.bottom - h; move = true; }
    if (move) {
        SetWindowPos(dlg, nullptr, x, y, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG:
            g_mainDlg = dlg;
            SetAppIcon(dlg);
            StyleDialogControls(dlg);
            // The link is a plain static now; underline it so it
            // still reads as a link.
            if (HWND link = GetDlgItem(dlg, IDC_LINK_SITE)) {
                HFONT base = reinterpret_cast<HFONT>(
                    SendMessageW(dlg, WM_GETFONT, 0, 0));
                LOGFONTW lf{};
                if (base && GetObjectW(base, sizeof(lf), &lf)) {
                    lf.lfUnderline = TRUE;
                    HFONT f = CreateFontIndirectW(&lf);
                    if (f) SendMessageW(link, WM_SETFONT,
                                        reinterpret_cast<WPARAM>(f), TRUE);
                }
            }
            SyncControls(dlg);
            HugHelpButtons(dlg);
            g_tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                     WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                     CW_USEDEFAULT, CW_USEDEFAULT,
                                     CW_USEDEFAULT, CW_USEDEFAULT,
                                     dlg, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
            if (g_tips) {
                SendMessageW(g_tips, TTM_SETMAXTIPWIDTH, 0, 420);
                SendMessageW(g_tips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 32000);
                for (const auto& e : kHelp) AttachHelpTool(dlg, e);
            }
            // Edge popup page first.
            if (HWND tabs = GetDlgItem(dlg, IDC_TABS)) {
                TabsSet(tabs, false, false);
            }
            UpdateActivePage(dlg);
            // Content height from the live controls: everything
            // scrolls together, so the bottom is the Close button.
            {
                const int dpi = CtrlDpi(dlg);
                RECT rb{};
                GetWindowRect(GetDlgItem(dlg, IDOK), &rb);
                MapWindowPoints(nullptr, dlg,
                                reinterpret_cast<POINT*>(&rb), 2);
                g_contentBottom = rb.bottom + MulDiv(8, dpi, 96);
                g_scrollPos = 0;
                ShowScrollBar(dlg, SB_VERT, FALSE);
                UpdateDialogLayout(dlg);
            }
            ClampToWorkArea(dlg);   // never taller than the monitor
            return TRUE;
        case WM_SIZE:
            UpdateDialogLayout(dlg);
            return TRUE;
        case WM_MOUSEWHEEL: {
            // Standard wheel scrolling: children's DefWindowProc
            // bubbles WM_MOUSEWHEEL here when they have the focus. High-
            // precision touchpads send sub-notch deltas - integer-dividing
            // truncates them to nothing, so carry the remainder.
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            const int step = MulDiv(48, CtrlDpi(dlg), 96);
            static int wheelCarryDlg = 0;
            wheelCarryDlg += delta;
            const int notches = wheelCarryDlg / WHEEL_DELTA;
            if (notches != 0) {
                wheelCarryDlg -= notches * WHEEL_DELTA;
                ScrollDlgTo(dlg, g_scrollPos - notches * step);
            }
            return TRUE;
        }
        case WM_VSCROLL: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(dlg, SB_VERT, &si);
            int newPos = g_scrollPos;
            switch (LOWORD(wp)) {
                case SB_LINEUP:  newPos -= 24; break;
                case SB_LINEDOWN: newPos += 24; break;
                case SB_PAGEUP:  newPos -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN: newPos += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: newPos = static_cast<int>(
                                            si.nTrackPos); break;
                case SB_TOP: newPos = 0; break;
                case SB_BOTTOM: newPos = static_cast<int>(si.nMax); break;
            }
            ScrollDlgTo(dlg, newPos);
            return TRUE;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const int dpi = CtrlDpi(dlg);
            mmi->ptMinTrackSize.x = MulDiv(300, dpi, 96);
            mmi->ptMinTrackSize.y = MulDiv(320, dpi, 96);
            return TRUE;
        }
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            // A new monitor can be shorter (or scaled larger) than the
            // one the dialog opened on - re-clamp.
            ClampToWorkArea(dlg);
            return TRUE;
        case WM_DESTROY:
            g_mainDlg = nullptr;
            if (g_tips) {
                DestroyWindow(g_tips);
                g_tips = nullptr;
            }
            return TRUE;
        case WM_CTLCOLORDLG:
            SetBkColor(reinterpret_cast<HDC>(wp), kColBg);
            return reinterpret_cast<INT_PTR>(BgBrush());
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(wp);
            const HWND st = reinterpret_cast<HWND>(lp);
            if (st == GetDlgItem(dlg, IDC_ST_TBSWATCH)) {
                // The color swatch IS its background brush. Cached
                // per color; SyncControls invalidates after changes.
                static HBRUSH brush = nullptr;
                static COLORREF painted = CLR_INVALID;
                const COLORREF want = g_settings
                    ? g_settings->arrowColor
                    : static_cast<COLORREF>(RGB(255, 255, 255));
                if (painted != want) {
                    if (brush) DeleteObject(brush);
                    brush = CreateSolidBrush(want);
                    painted = want;
                }
                return reinterpret_cast<INT_PTR>(brush);
            }
            SetBkColor(dc, kColBg);
            SetTextColor(dc, st == GetDlgItem(dlg, IDC_ST_APPLYNOTE)
                                 ? kColSoft
                             : st == GetDlgItem(dlg, IDC_LINK_SITE)
                                 ? kColAccent
                                 : kColText);
            return reinterpret_cast<INT_PTR>(BgBrush());
        }
        case WM_NOTIFY:
            return HandleDialogNotify(dlg, lp);
        case WM_CTLCOLOREDIT: {
            // Buddy edits: cream fill, dark text (the white 3D
            // native fields were the loudest leftover natives). The flat
            // teal border is drawn by TealEditProc's WM_NCPAINT. Disabled
            // boxes get the soft text color.
            const HDC dc = reinterpret_cast<HDC>(wp);
            SetBkColor(dc, kColBg);
            SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lp))
                                 ? kColText
                                 : kColSoft);
            return reinterpret_cast<INT_PTR>(BgBrush());
        }
        case WM_DRAWITEM:
            if (IsHelpId(static_cast<int>(wp)))
                DrawHelpButton(dlg, reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_HSCROLL: {
            if (g_syncing) return FALSE;
            const HWND slider = reinterpret_cast<HWND>(lp);
            for (const auto& sl : kSliders) {
                if (GetDlgItem(dlg, sl.sliderId) == slider) {
                    const int v = static_cast<int>(SendMessageW(
                        slider, TBM_GETPOS, 0, 0));
                    g_syncing = true;
                    wchar_t buf[16];
                    _itow_s(v, buf, 10);
                    SetDlgItemTextW(dlg, sl.editId, buf);
                    g_syncing = false;
                    break;
                }
            }
            ApplyNow(dlg);
            return TRUE;
        }
        case WM_COMMAND: {
            if (g_syncing) return FALSE;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDOK || id == IDCANCEL) {
                EndDialog(dlg, id);
                return TRUE;
            }
            if (code == BN_CLICKED) {
                if (IsHelpId(id)) {
                    ShowHelpWindow(dlg, id);
                    return TRUE;
                }
                if (id == IDC_BTN_DONATE) {
                    OpenDonate();
                    return TRUE;
                }
                if (id == IDC_PILL_ACT) {
                    // g_pillMode was already set by the pill control.
                    ApplyNow(dlg);
                    return TRUE;
                }
                if (id == IDC_CK_ICONS || id == IDC_CK_TITLES ||
                    id == IDC_CK_THUMBS) {
                    // Never a blank popup: the last of the three stays on
                    //.
                    if (!GetCheck(dlg, IDC_CK_ICONS) &&
                        !GetCheck(dlg, IDC_CK_TITLES) &&
                        !GetCheck(dlg, IDC_CK_THUMBS)) {
                        SetCheck(dlg, id, true);
                        return TRUE;
                    }
                }
                if (id == IDC_RD_ED_AUTO || id == IDC_RD_ED_SIDES) {
                    UpdateEdgesMode(dlg);
                    ApplyNow(dlg);
                    return TRUE;
                }
                if (id == IDC_SW_ENABLE) {   // grey the box
                    UpdateEdgePopupEnabledRows(dlg);
                    ApplyNow(dlg);
                    return TRUE;
                }
                if (id == IDC_SW_ARROWS) {   // grey the box
                    UpdateTaskbarEnabledRows(dlg);
                    ApplyNow(dlg);
                    return TRUE;
                }
                if (id == IDC_TABS) {        // page swap
                    UpdateActivePage(dlg);
                    return TRUE;
                }
                if (id == IDC_BTN_TB_COLOR && g_settings && g_apply) {
                    // Native color picker; the swatch previews the
                    // result and the arrows pick it up live.
                    static COLORREF custColors[16]{};
                    CHOOSECOLORW cc{};
                    cc.lStructSize = sizeof(cc);
                    cc.hwndOwner = dlg;
                    cc.rgbResult = g_settings->arrowColor;
                    cc.lpCustColors = custColors;
                    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
                    if (ChooseColorW(&cc)) {
                        const Settings before = *g_settings;
                        g_settings->arrowColor = cc.rgbResult;
                        g_apply(before, *g_settings);
                        if (HWND sw = GetDlgItem(dlg, IDC_ST_TBSWATCH)) {
                            InvalidateRect(sw, nullptr, TRUE);
                        }
                    }
                    return TRUE;
                }
                if (id == IDC_BTN_ADVANCED) {
                    DialogBoxParamW(GetModuleHandleW(nullptr),
                                    MAKEINTRESOURCEW(IDD_ADVANCED), dlg,
                                    AdvancedProc, 0);
                    SyncControls(dlg);
                    return TRUE;
                }
                if (id == IDC_BTN_RESET) {
                    Settings defaults{};
                    const Settings before = *g_settings;
                    *g_settings = defaults;
                    if (g_apply) g_apply(before, defaults);
                    SyncControls(dlg);
                    // SyncControls updates g_pillMode (and swaps the mode
                    // rows), but nothing told the custom pill control to
                    // REDRAW - it kept the pre-reset segment until the
                    // next unrelated paint.
                    if (HWND pill = GetDlgItem(dlg, IDC_PILL_ACT)) {
                        InvalidateRect(pill, nullptr, FALSE);
                    }
                    return TRUE;
                }
                ApplyNow(dlg);
                return TRUE;
            }
            if (code == EN_CHANGE) {
                ApplyNow(dlg);
                return TRUE;
            }
            if (code == EN_KILLFOCUS) {
                // Leaving a numeric field snaps invalid/out-of-range
                // input to the closest legal value, so the
                // box always shows what was actually applied.
                for (const auto& sl : kSliders) {
                    if (id != sl.editId) continue;
                    wchar_t buf[16] = L"";
                    GetDlgItemTextW(dlg, sl.editId, buf, 16);
                    int v = _wtoi(buf);
                    if (v < sl.minV) v = sl.minV;
                    if (v > sl.maxV) v = sl.maxV;
                    wchar_t out[16];
                    _itow_s(v, out, 10);
                    if (wcscmp(buf, out) != 0) {
                        g_syncing = true;
                        SetDlgItemTextW(dlg, sl.editId, out);
                        SendDlgItemMessageW(dlg, sl.sliderId, TBM_SETPOS,
                                            TRUE, v);
                        g_syncing = false;
                    }
                    ApplyNow(dlg);
                    break;
                }
                return TRUE;
            }
            return FALSE;
        }
    }
    return FALSE;
}

} // namespace

INT_PTR ShowSettingsDialog(HINSTANCE inst, HWND parent, Settings* s,
                           void (*apply)(const Settings&, const Settings&),
                           void (*advanced)(int action)) {
    // One instance: raise the existing dialog instead of
    // opening another.
    if (g_mainDlg && IsWindow(g_mainDlg)) {
        if (IsIconic(g_mainDlg)) ShowWindow(g_mainDlg, SW_RESTORE);
        SetForegroundWindow(g_mainDlg);
        return 0;
    }
    g_settings = s;
    g_apply = apply;
    g_advanced = advanced;
    RegisterPillClass(inst);
    EnsureUiResources();   // no first-paint hitch on the pill
    const INT_PTR r = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_SETTINGS),
                                      parent, DialogProc, 0);
    if (r <= 0) {
        wchar_t buf[128];
        swprintf_s(buf,
                   L"EdgePOP by KitKarl - settings dialog failed "
                   L"(result=%d err=%lu).",
                   static_cast<int>(r), GetLastError());
        MessageBoxW(parent, buf, L"EdgePOP by KitKarl",
                    MB_OK | MB_ICONWARNING);
    }
    g_settings = nullptr;
    g_apply = nullptr;
    g_advanced = nullptr;
    return r;
}

INT_PTR ShowAboutDialog(HINSTANCE inst, HWND parent) {
    return DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_ABOUT), parent,
                           AboutProc, 0);
}

// Re-sync the open settings dialog after the struct changed elsewhere
//. No-op when the dialog is closed.
void RefreshSettingsDialog(Settings* s) {
    if (!g_mainDlg || !IsWindow(g_mainDlg)) return;
    if (s) g_settings = s;
    SyncControls(g_mainDlg);
}

void ShowSplash(HINSTANCE inst, HWND owner) {
    if (g_splash) return;
    g_splash = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_SPLASH), owner,
                                  SplashProc, 0);
    if (g_splash) ShowWindow(g_splash, SW_SHOWNOACTIVATE);
}

// Outside-click dismissal: main.cpp feeds every real mouse-down
// from the LL hook here; a click anywhere but on the card closes it.
void CloseHelpCardIfOutside(POINT pt) {
    if (!g_helpWnd || !IsWindow(g_helpWnd)) return;
    RECT rc;
    GetWindowRect(g_helpWnd, &rc);
    if (!PtInRect(&rc, pt)) DestroyWindow(g_helpWnd);
}

} // namespace edgepop
