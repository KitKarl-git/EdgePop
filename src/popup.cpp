#include "popup.h"

#include <windows.h>
#include <windowsx.h>          // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>          // SHGetFileInfo
#include <shellscalingapi.h>   // GetDpiForMonitor
#include <commoncontrols.h>    // IImageList / SHIL_JUMBO

#include <d2d1_3.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <set>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace edgepop {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kPopupClass[] = L"EdgePopPopup";
constexpr UINT kPollTimer = 3;        // Esc + presence, 100 ms
constexpr UINT kPollMs = 100;
constexpr UINT kVerifyFgTimer = 4;    // deferred switch verification
constexpr ULONGLONG kCloseDelayMs = 650;
constexpr ULONGLONG kReconcileMs = 700;   // card-close re-enumerate

// Visual constants, in 96-dpi units, scaled by the monitor's DPI.
constexpr float kRowH = 34.0f;
constexpr float kHeaderH = 24.0f;
constexpr float kIconSz = 24.0f;
constexpr float kPadX = 10.0f;
constexpr float kCornerR = 8.0f;
// The popup's own close: a square tab PROTRUDING above the panel's top
// edge - visibly chrome, never part of the first card. The window
// grows kXProtrude taller; rows start below it.
constexpr float kXBtnSz = 28.0f;      // the square's side
constexpr float kXProtrude = 24.0f;   // rise above the panel top edge
constexpr float kXInsetX = 10.0f;     // right inset from the panel edge
// Per-card close: a small rounded-square button riding the card's title
// strip, flush right - in line with the icon and app name, Win11
// taskbar-flyout style. A live DWM thumbnail composites above everything
// - nothing can sit on its pixels (gotcha 18) - so the title strip,
// outside the preview slot, is the row the X occupies; the title
// ellipsizes short of it.
constexpr float kCardXSz = 20.0f;     // close button side - same footprint
                                      // as the strip's app icon
constexpr float kCardXR = 5.0f;       // corner radius - the native
                                      // flyout X is a rounded square
constexpr float kCardXGap = 8.0f;     // title-to-X breathing room

// Width: the list popup is as narrow as its content allows,
// floored here and capped by the max_width setting. Cards have their own
// width setting - see CardWidthOf.
constexpr float kMinPopupW = 120.0f;   // icons-only still reads as a popup
constexpr float kEmptyW = 200.0f;      // "no windows" message fits
constexpr float kMinCardW = 230.0f;    // preview cards need real width

// Thumbnail card metrics (96-dpi units, scaled). A card = padded preview
// slot + title strip; the slot letterboxes the window's preview. Cards
// live at the card_width setting, floored kMinCardW and capped kCardWMax;
// the slot height grows with the card's inner width
// (16:9, clamped) - wider cards mean bigger previews, not wider
// letterbox bars.
constexpr float kThumbSlotH = 157.0f;    // slot-height floor
constexpr float kThumbSlotHMax = 260.0f;
constexpr float kCardWMax = 400.0f;      // ceiling - 480 read too wide on
                                         // my desk; 400 keeps headroom
                                         // over the 300 default
constexpr float kThumbStripH = 26.0f;
constexpr float kCardPadY = 8.0f;        // above the preview slot
constexpr float kCardPadBottom = 3.0f;   // under the title strip; 8+8
                                         // between cards read as dead
                                         // space under the title
constexpr ULONGLONG kThumbDelayMs = 240;   // pop-in (220 ms) must settle

// Overflow scrolling: content that exceeds the work area scrolls
// inside a fixed viewport instead of rows being dropped. One wheel notch
// moves this many 96-dpi px; sub-notch touchpad deltas accumulate in
// wheelCarry_ until a full notch assembles.
constexpr float kWheelStep = 120.0f;

// VideoSquisher dark palette: warm dark card, soft cream
// text, teal accent for section headers.
constexpr float kColPanelR = 0x2C / 255.0f, kColPanelG = 0x28 / 255.0f,
                kColPanelB = 0x25 / 255.0f;
constexpr float kColTextR = 0xED / 255.0f, kColTextG = 0xE7 / 255.0f,
                kColTextB = 0xE0 / 255.0f;
constexpr float kColSoftR = 0xA8 / 255.0f, kColSoftG = 0x9E / 255.0f,
                kColSoftB = 0x94 / 255.0f;
constexpr float kColAccR = 0x3A / 255.0f, kColAccG = 0xB8 / 255.0f,
                kColAccB = 0xA8 / 255.0f;
// close-button additions: the square tab rests on a panel-lifted
// fill; the per-card close button hovers warm red - the only red in the
// product,
// reserved for "this ends a real window" (the popup X never wears it).
constexpr float kColLiftR = 0x35 / 255.0f, kColLiftG = 0x30 / 255.0f,
                kColLiftB = 0x2B / 255.0f;
constexpr float kColRedR = 0xC6 / 255.0f, kColRedG = 0x4B / 255.0f,
                kColRedB = 0x3E / 255.0f;

// --- render stack state (created once, prewarmed at startup) ----------------

ComPtr<ID3D11Device> g_d3d;
ComPtr<IDXGIFactory2> g_dxgiFactory;
ComPtr<IDXGISwapChain1> g_swap;
ComPtr<ID2D1Factory1> g_d2dFactory;
ComPtr<ID2D1Device> g_d2dDevice;
ComPtr<ID2D1DeviceContext> g_ctx;
ComPtr<IDWriteFactory> g_dwFactory;
ComPtr<IDCompositionDevice> g_dcomp;
ComPtr<IDCompositionTarget> g_dcompTarget;
ComPtr<IDCompositionVisual> g_visual;
ComPtr<IDCompositionVisual3> g_visual3;   // SetOpacity (Win10 1703+)
ComPtr<IDCompositionScaleTransform> g_scaleTf;   // pop-in
SIZE g_swapSize = { 0, 0 };

// Cached per-scale resources (rebuilt when the DPI scale changes).
float g_resScale = 0.0f;
ComPtr<ID2D1SolidColorBrush> g_text;
ComPtr<ID2D1SolidColorBrush> g_dim;
ComPtr<ID2D1SolidColorBrush> g_hover;
ComPtr<ID2D1SolidColorBrush> g_header;   // section headers: teal accent
ComPtr<ID2D1SolidColorBrush> g_erase;    // alpha 0 - hole punch (MIN blend)
ComPtr<ID2D1SolidColorBrush> g_textShadow;   // floating-label fallback text
ComPtr<ID2D1StrokeStyle> g_roundCap;   // round-cap strokes (card-close X)
ComPtr<IDWriteTextFormat> g_itemFmt;
ComPtr<IDWriteTextFormat> g_headerFmt;
ComPtr<IDWriteInlineObject> g_ellipsis;

// Icon cache, rebuilt on every Show (windows may have changed icons).
std::map<HWND, ComPtr<ID2D1Bitmap>> g_iconCache;

// Pop-out progress polynomials, normalized to reach 1 at
// u = 1. The seam axis overshoots a few percent (mac-dock feel); the cross
// axis and opacity use a plain smoothstep. See StartAnimation.
constexpr float kSeamU = 4.2f, kSeamV = -5.4f, kSeamW = 2.2f;   // 4.2u-5.4u²+2.2u³
constexpr float kSmoothV = 3.0f, kSmoothW = -2.0f;              // 3u²-2u³

void (*g_logSink)(const wchar_t*) = nullptr;
void (*g_refreshSink)(const Segment&) = nullptr;   // card-close reconcile

void PLog(const wchar_t* fmt, ...) {
    if (!g_logSink) return;
    wchar_t buf[256];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    g_logSink(buf);
}

void FakeKeyPress() {
   // a harmless SHIFT press right before SetForegroundWindow
    // defeats the foreground lock for our no-activate popup.
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_SHIFT;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_SHIFT;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// ALT held DOWN ACROSS the SetForegroundWindow call - the canonical
// form of the unlock: with a modifier logically down the system
// believes a keyboard interaction is in flight and honors the switch
// (press-release BEFORE the call often does not, observed on Win11
// 26200: SFGW returns TRUE, the foreground goes NULL and reverts).
void FakeAltHold(bool down) {
    INPUT in[1] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_MENU;
    in[0].ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, in, sizeof(INPUT));
}

// Pids that re-assert the foreground after losing it (WeChat repro):
// the first bounced switch registers them, and every later switch
// AWAY from one skips the soft land-bounce juggling and forces in a
// single decisive transition. Session-lifetime; bounded.
std::set<DWORD> g_fighterPids;

// The forced switch, in rising order of visual cost:
// SwitchToThisWindow is the taskbar's own switch primitive (NT-era,
// still exported) and activates WITHOUT the minimize/restore
// animation; the dance always works but animates the window out of
// the taskbar, so it is the last resort only.
void ForceForeground(HWND target) {
    SwitchToThisWindow(target, TRUE);
    if (GetForegroundWindow() == target) {
        PLog(L"force: SwitchToThisWindow ok");
        return;
    }
    ShowWindow(target, SW_MINIMIZE);
    ShowWindow(target, SW_RESTORE);
    if (GetForegroundWindow() != target) SetForegroundWindow(target);
    PLog(L"force: minimize/restore fallback");
}

// Per-scale cached resources, rebuilt when the DPI scale changes. Called
// from Show (title measurement needs the formats before the first Render)
// and Render.
void EnsureScaleResources(float scale) {
    if (g_resScale == scale || !g_ctx || !g_dwFactory) return;
    g_resScale = scale;
    g_text.Reset(); g_dim.Reset(); g_hover.Reset(); g_header.Reset();
    g_erase.Reset(); g_textShadow.Reset(); g_roundCap.Reset();
    g_itemFmt.Reset(); g_headerFmt.Reset(); g_ellipsis.Reset();
    g_ctx->CreateSolidColorBrush(
        D2D1::ColorF(kColTextR, kColTextG, kColTextB, 0.95f), &g_text);
    g_ctx->CreateSolidColorBrush(
        D2D1::ColorF(0, 0, 0, 0.65f), &g_textShadow);
    g_ctx->CreateSolidColorBrush(
        D2D1::ColorF(kColSoftR, kColSoftG, kColSoftB, 0.95f), &g_dim);
    g_ctx->CreateSolidColorBrush(
        D2D1::ColorF(kColAccR, kColAccG, kColAccB, 0.9f), &g_header);
    g_ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.08f), &g_hover);
    g_ctx->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &g_erase);
    D2D1_STROKE_STYLE_PROPERTIES sp{};
    sp.startCap = D2D1_CAP_STYLE_ROUND;
    sp.endCap = D2D1_CAP_STYLE_ROUND;
    sp.dashCap = D2D1_CAP_STYLE_ROUND;
    g_d2dFactory->CreateStrokeStyle(sp, nullptr, 0, &g_roundCap);
    g_dwFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14.0f * scale, L"", &g_itemFmt);
    if (g_itemFmt) {
        g_itemFmt->SetParagraphAlignment(
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);   // note 7
    }
    g_dwFactory->CreateTextFormat(
        L"Segoe UI Semibold", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        12.0f * scale, L"", &g_headerFmt);
    if (g_headerFmt) {
        g_headerFmt->SetParagraphAlignment(
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    DWRITE_TRIMMING trim{};
    trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    g_dwFactory->CreateEllipsisTrimmingSign(g_itemFmt.Get(),
                                            &g_ellipsis);
    if (g_itemFmt) g_itemFmt->SetTrimming(&trim, g_ellipsis.Get());
}

// HICON -> D2D bitmap via a top-down 32bpp DIB (GDI icons are premultiplied).
ComPtr<ID2D1Bitmap> IconToBitmap(ID2D1DeviceContext* ctx, HICON icon,
                                 UINT size) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(size);
    bi.bmiHeader.biHeight = -static_cast<LONG>(size);   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits,
                                   nullptr, 0);
    if (!dib) return nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(hdc, dib);
    DrawIconEx(hdc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    SelectObject(hdc, old);
    DeleteDC(hdc);

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1_PIXEL_FORMAT{ DXGI_FORMAT_B8G8R8A8_UNORM,
                           D2D1_ALPHA_MODE_PREMULTIPLIED });
    ComPtr<ID2D1Bitmap> bmp;
    ctx->CreateBitmap(D2D1_SIZE_U{ size, size }, bits, size * 4, props,
                      &bmp);
    DeleteObject(dib);
    return bmp;
}

ID2D1Bitmap* IconFor(HWND hwnd, HICON icon, UINT size) {
    auto it = g_iconCache.find(hwnd);
    if (it != g_iconCache.end()) return it->second.Get();
    ComPtr<ID2D1Bitmap> bmp = IconToBitmap(g_ctx.Get(), icon, size);
    if (!bmp) return nullptr;
    ID2D1Bitmap* raw = bmp.Get();
    g_iconCache[hwnd] = std::move(bmp);
    return raw;
}

// --- hi-res icons ----------------------------------------------
//
// WM_GETICON small icons are ~24 px art and pixelate the moment they are
// drawn at 40 px+ card scale. The shell's JUMBO image list carries the
// 256 px art modern apps ship; keyed per exe (all windows of one app share
// it) and rendered once at 64 px - DrawBitmap scales it cleanly to every
// use, down or (slightly) up.
std::map<std::wstring, ComPtr<ID2D1Bitmap>> g_bigIconCache;

ID2D1Bitmap* BigIconFor(HWND hwnd) {
    wchar_t path[MAX_PATH]{};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid) {
        HANDLE proc =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            DWORD n = MAX_PATH;
            if (!QueryFullProcessImageNameW(proc, 0, path, &n)) path[0] = 0;
            CloseHandle(proc);
        }
    }
    if (!path[0]) return nullptr;
    std::wstring key(path);
    _wcslwr_s(path);
    key = path;
    auto it = g_bigIconCache.find(key);
    if (it != g_bigIconCache.end()) return it->second.Get();
    if (g_bigIconCache.size() > 32) g_bigIconCache.clear();

    ComPtr<ID2D1Bitmap> bmp;
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX)) {
        IImageList* il = nullptr;
        if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&il)))) {
            HICON big = nullptr;
            if (SUCCEEDED(il->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &big)) &&
                big) {
                bmp = IconToBitmap(g_ctx.Get(), big, 64);
                DestroyIcon(big);
            }
            il->Release();
        }
    }
    ID2D1Bitmap* raw = bmp ? bmp.Get() : nullptr;
    g_bigIconCache[key] = std::move(bmp);
    return raw;
}

// Preferred icon source for any card/row drawing: hi-res first, the
// window's own small icon as fallback.
ID2D1Bitmap* CardIconFor(const WindowItem& item, UINT size) {
    if (ID2D1Bitmap* big = BigIconFor(item.hwnd)) return big;
    if (item.icon) return IconFor(item.hwnd, item.icon, size);
    return nullptr;
}

// --- last-known static previews --------------------------------
//
// DWM live thumbnails cannot ride the pop-in animation, so a fresh show
// waits ~240 ms before the preview appears. To close that gap, every popup
// close captures a static snapshot of each listed window by reading the
// COMPOSED SCREEN over the window's visible bounds - no message ever
// enters the foreign window (PrintWindow + PW_RENDERFULLCONTENT used to
// live here and permanently froze custom-chrome apps that must service
// its synchronous render request on their UI thread - Windscribe was the
// repro; gotcha 27). The next show draws the snapshot instantly - it is
// swapchain content, so it animates with the card - and the live
// thumbnail seamlessly replaces it.
std::map<HWND, ComPtr<ID2D1Bitmap>> g_snapCache;

ComPtr<ID2D1Bitmap> CaptureSnapshot(HWND src) {
    RECT rc{};
    // Extended frame bounds = the visible window, shadow excluded;
    // GetWindowRect on framed windows includes invisible resize borders.
    if (FAILED(DwmGetWindowAttribute(src, DWMWA_EXTENDED_FRAME_BOUNDS,
                                     &rc, sizeof(rc)))) {
        if (!GetWindowRect(src, &rc)) return nullptr;
    }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib =
        CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) return nullptr;
    HDC screen = GetDC(nullptr);
    HDC hdc = screen ? CreateCompatibleDC(screen) : nullptr;
    BOOL ok = FALSE;
    if (screen && hdc) {
        HGDIOBJ old = SelectObject(hdc, dib);
        // CAPTUREBLT pulls in layered windows the plain blit skips.
        ok = BitBlt(hdc, 0, 0, w, h, screen, rc.left, rc.top,
                    SRCCOPY | CAPTUREBLT);
        SelectObject(hdc, old);
    }
    if (hdc) DeleteDC(hdc);
    if (screen) ReleaseDC(nullptr, screen);

    ComPtr<ID2D1Bitmap> bmp;
    if (ok) {
        // Quality guard: blank/teardown garbage - an
        // all-zero frame, a white tile, or the black-with-a-white-bar
        // ghost of a surface captured after the minimize completed -
        // must not displace a good snapshot or a clean placeholder.
        // Real window content always has mid-tones (text antialiasing,
        // colors); junk frames are pure extremes.
        bool anyContent = false;
        long midTones = 0, samples = 0;
        const DWORD* px = static_cast<const DWORD*>(bits);
        const int step = w > 4 ? w / 4 : 1;   // sample ~4 columns of rows
        for (int y = 0; y < h; y += 8) {
            for (int x = 0; x < w; x += step) {
                const DWORD p = px[y * w + x];
                const int r = static_cast<int>(p & 0xFF);
                const int g2 = static_cast<int>((p >> 8) & 0xFF);
                const int b = static_cast<int>((p >> 16) & 0xFF);
                const int lum = (r * 30 + g2 * 59 + b * 11) / 100;
                if (lum > 6 && lum < 247) ++midTones;
                if ((p & 0x00FFFFFF) != 0) anyContent = true;
                ++samples;
            }
        }
        const bool enoughMidTones =
            samples == 0 || midTones * 100 >= samples * 4;   // >= 4 %
        if (anyContent && enoughMidTones) {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1_PIXEL_FORMAT{ DXGI_FORMAT_B8G8R8A8_UNORM,
                                   D2D1_ALPHA_MODE_PREMULTIPLIED });
            g_ctx->CreateBitmap(D2D1_SIZE_U{ static_cast<UINT>(w),
                                             static_cast<UINT>(h) },
                                bits, static_cast<UINT>(w) * 4, props, &bmp);
        }
    }
    DeleteObject(dib);
    return bmp;
}

void CacheSnapshots(const std::vector<WindowItem>& items) {
    if (!g_ctx) return;
    if (g_snapCache.size() > 24) g_snapCache.clear();
    for (const auto& it : items) {
        if (it.minimized || !it.hwnd || !IsWindow(it.hwnd)) continue;
        if (IsIconic(it.hwnd)) continue;   // blank capture, skip
        ComPtr<ID2D1Bitmap> bmp = CaptureSnapshot(it.hwnd);
        if (bmp) g_snapCache[it.hwnd] = std::move(bmp);
    }
}

// --- peek-on-hover ghosting ------------------------------------
//
// Taskbar-preview behavior: while an entry is hovered, every OTHER window
// on that entry's monitor is hidden, leaving only the hovered window
// visible on the desktop. Hiding = moving the window fully outside the
// virtual screen and restoring its exact rect afterwards - the only
// cross-process hide that provably cannot damage a foreign renderer:
// the app keeps rendering (the swapchain never notices a position
// change), its DWM thumbnail stays alive and the taskbar button stays.
// Every alternative maims or is denied (D-068 probes): toggling
// WS_EX_LAYERED on a non-layered window kills flip-model D3D swapchains
// for good; SetLayeredWindowAttributes on a NATIVELY layered window
// (Qt translucent chrome - Windscribe) hijacks it out of its
// UpdateLayeredWindow mode and it never draws again; DWMWA_CLOAK is
// owner/shell-only (E_ACCESSDENIED from outside); SW_HIDE drops the
// taskbar buttons. Apps that veto the move (WM_WINDOWPOSCHANGING) are
// skipped - peek degrades to leaving them visible.
struct Ghosted {
    HWND hwnd;
    RECT rc;    // exact pre-move rect
};
std::vector<Ghosted> g_ghosted;

void UnghostAll() {
    for (const auto& g : g_ghosted) {
        if (!IsWindow(g.hwnd)) continue;
        SetWindowPos(g.hwnd, nullptr, g.rc.left, g.rc.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_ghosted.clear();
}

void GhostAllBut(HWND keep, VirtualDesktops vd) {
    UnghostAll();
    if (!keep || !IsWindow(keep)) return;
    HMONITOR mon = MonitorFromWindow(keep, MONITOR_DEFAULTTONEAREST);
    if (!mon) return;
    const auto others = EnumerateWindowsOnMonitor(mon, vd);
    PLog(L"peek: %d window(s) on the hovered monitor, keeping hwnd=0x%p",
         static_cast<int>(others.size()), keep);
    // Never ghost the FOREGROUND window: moving the window the user is
    // in out of the virtual screen and back makes focus-recovery apps
    // (WeChat repro) re-assert themselves the moment a popup click
    // takes the foreground away - the switch bounces back within
    // ~100 ms. It stays visible instead; every other window still
    // hides, so peek degrades only when the window being left shares
    // the hovered monitor.
    const HWND fg = GetForegroundWindow();
    const int vsL = GetSystemMetrics(SM_XVIRTUALSCREEN);
    for (const auto& w : others) {
        if (w.hwnd == keep || w.hwnd == fg || w.minimized ||
            !IsWindow(w.hwnd))
            continue;
        RECT rc{};
        if (!GetWindowRect(w.hwnd, &rc)) continue;
        const int width = rc.right - rc.left;
        // Fully left of the virtual screen, unchanged top: same monitor
        // order, minimal surprise on restore.
        if (SetWindowPos(w.hwnd, nullptr, vsL - width - 64, rc.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
            g_ghosted.push_back({ w.hwnd, rc });
        }
    }
}

// --- over-mode labels -------------------
//
// DWM thumbnails composite above EVERY overlapping window - the popup's
// visual tree (probe [B]) AND separate topmost layered overlays placed
// exactly over the preview (probe; an earlier overlay-window attempt
// rendered under the live preview). Nothing can be drawn over a live
// thumbnail. "Labels over previews" therefore stops the preview region
// just above the label and leaves the label band unfilled - the text sits
// directly at the preview's bottom edge, on the panel.

// Natural text width at the current format (no wrapping, no trimming).
float MeasureText(const std::wstring& text, IDWriteTextFormat* fmt) {
    if (!fmt || text.empty()) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(g_dwFactory->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), fmt, 10000.0f,
            100.0f, &layout)) ||
        !layout)
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.width;
}

} // namespace

// Last-chance capture for windows as they minimize: the
// taskbar keeps a snapshot of a window when it goes down; we do the same
// via a global EVENT_SYSTEM_MINIMIZESTART hook (main.cpp installs it) so
// the popup's snapshot cache has the content even if no popup closed
// while the window was visible.
void CacheWindowSnapshot(HWND hwnd) {
    if (!g_ctx || !hwnd || !IsWindow(hwnd)) return;
    // If the minimize already completed by the time this out-of-context
    // callback ran, the surface is torn down and any capture is garbage
    // - skip.
    if (IsIconic(hwnd)) return;
    if (g_snapCache.size() > 24) g_snapCache.clear();
    ComPtr<ID2D1Bitmap> bmp = CaptureSnapshot(hwnd);
    if (bmp) g_snapCache[hwnd] = std::move(bmp);
}

void Popup::SetLogSink(void (*fn)(const wchar_t*)) { g_logSink = fn; }

void Popup::SetRefreshSink(void (*fn)(const Segment&)) { g_refreshSink = fn; }

Popup& Popup::Instance() {
    static Popup p;
    return p;
}

void Popup::RegisterClass(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.lpfnWndProc = &Popup::WndProc;
    wc.lpszClassName = kPopupClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
}

bool Popup::Create(HINSTANCE inst) {
    inst_ = inst;
    // WS_EX_NOREDIRECTIONBITMAP is required for the DirectComposition path:
    // the window has no redirection surface; pixels come from the visual.
    hwnd_ = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
            WS_EX_NOREDIRECTIONBITMAP,
        kPopupClass, L"EdgePop", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
        inst, nullptr);
    if (!hwnd_) return false;
    // Prewarm: D3D/D2D/DComp init takes ~100 ms; doing it here removes the
    // first-show hitch.
    EnsureRenderStack();
    return true;
}

void Popup::EnsureRenderStack() {
    if (stackOk_) return;
    stackOk_ = true;   // only try once; a broken stack shows nothing, logged

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, nullptr, 0, D3D11_SDK_VERSION,
                                   &g_d3d, nullptr, nullptr);
    if (FAILED(hr)) {
        PLog(L"render: D3D hardware failed hr=0x%08X, trying WARP", hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               nullptr, 0, D3D11_SDK_VERSION, &g_d3d, nullptr,
                               nullptr);
        if (FAILED(hr)) {
            PLog(L"render: D3D WARP failed hr=0x%08X", hr);
            stackOk_ = false;
            return;
        }
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    g_d3d.As(&dxgiDevice);
    // Factory from the device's own adapter: guarantees association, which
    // CreateSwapChainForComposition is picky about.
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) { PLog(L"render: GetAdapter failed hr=0x%08X", hr); stackOk_ = false; return; }
    hr = adapter->GetParent(IID_PPV_ARGS(&g_dxgiFactory));
    if (FAILED(hr)) { PLog(L"render: adapter parent factory failed hr=0x%08X", hr); stackOk_ = false; return; }

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                           __uuidof(ID2D1Factory1), nullptr,
                           reinterpret_cast<void**>(g_d2dFactory.GetAddressOf()));
    if (FAILED(hr)) { PLog(L"render: D2D factory failed hr=0x%08X", hr); stackOk_ = false; return; }
    hr = g_d2dFactory->CreateDevice(dxgiDevice.Get(), &g_d2dDevice);
    if (FAILED(hr)) { PLog(L"render: D2D device failed hr=0x%08X", hr); stackOk_ = false; return; }
    hr = g_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                          &g_ctx);
    if (FAILED(hr)) { PLog(L"render: D2D context failed hr=0x%08X", hr); stackOk_ = false; return; }

    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = 64;              // resized to the real popup size on render
    sc.Height = 64;
    sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.SampleDesc = { 1, 0 };
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    // SCALING_NONE is rejected by this machine's driver for composition
    // swapchains (verified with a standalone probe); STRETCH works everywhere.
    sc.Scaling = DXGI_SCALING_STRETCH;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    hr = g_dxgiFactory->CreateSwapChainForComposition(g_d3d.Get(), &sc,
                                                      nullptr, &g_swap);
    if (FAILED(hr)) { PLog(L"render: composition swapchain failed hr=0x%08X", hr); stackOk_ = false; return; }

    IUnknown* dwUnk = nullptr;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             &dwUnk);
    if (FAILED(hr)) { PLog(L"render: DWrite failed hr=0x%08X", hr); stackOk_ = false; return; }
    g_dwFactory.Attach(reinterpret_cast<IDWriteFactory*>(dwUnk));

    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&g_dcomp));
    if (FAILED(hr)) { PLog(L"render: DComp device failed hr=0x%08X", hr); stackOk_ = false; return; }
    hr = g_dcomp->CreateTargetForHwnd(hwnd_, TRUE, &g_dcompTarget);
    if (FAILED(hr)) { PLog(L"render: DComp target failed hr=0x%08X", hr); stackOk_ = false; return; }
    hr = g_dcomp->CreateVisual(&g_visual);
    if (FAILED(hr)) { PLog(L"render: DComp visual failed hr=0x%08X", hr); stackOk_ = false; return; }
    g_visual->SetContent(g_swap.Get());
    hr = g_dcomp->CreateScaleTransform(&g_scaleTf);
    if (FAILED(hr)) { PLog(L"render: DComp scale transform failed hr=0x%08X", hr); stackOk_ = false; return; }
    g_visual->SetTransform(g_scaleTf.Get());
    g_dcompTarget->SetRoot(g_visual.Get());
    g_visual.As(&g_visual3);   // SetOpacity (IDCompositionVisual3)
    g_dcomp->Commit();
    PLog(L"render: stack ok");
}

// The thumbnail card width from the settings, floored/capped to the
// visual limits. LoadSettings clamps the same range; this guards
// struct mutations that skip the loader.
float Popup::CardWidthOf(const Settings& s) const {
    return std::min(std::max(static_cast<float>(s.cardWidth), kMinCardW),
                    kCardWMax);
}

// The width setting that governs the current mode: the icon+title list
// caps at max_width, cards live at card_width.
float Popup::ModeWidthOf(const Settings& s) const {
    return s.thumbnails ? CardWidthOf(s)
                        : static_cast<float>(s.maxWidth);
}

// Natural (uncapped) content width of the flattened rows at the current
// scale: measured header/row text, or the card width for card rows. The
// empty list renders the "no windows" message.
float Popup::NaturalWidth() const {
    if (rows_.empty()) return kEmptyW * scale_;
    const float pad = kPadX * scale_;
    const float iconSz = kIconSz * scale_;
    float nat = 0;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const RowInfo& row = rows_[i];
        if (row.isHeader) {
            nat = std::max(nat, pad + MeasureText(row.label,
                                                  g_headerFmt.Get()) +
                                       pad);
        } else if (row.card) {
            nat = std::max(nat, CardWidthOf(settings_) * scale_);
        } else {
            float rw = pad;
            if (settings_.showIcons) rw += iconSz + pad;
            if (settings_.showTitles) {
                rw += MeasureText(items_[row.item].title, g_itemFmt.Get()) +
                      pad;
            }
            nat = std::max(nat, rw);
        }
    }
    return nat;
}

void Popup::Show(const Segment& segment, const MonitorRect& main,
                 const std::vector<PopupSection>& sections, POINT cursor,
                 const Settings& s) {
    if (!hwnd_) return;
    UnghostAll();               // never inherit ghosts from a prior show
    UnregisterThumbnails();   // stale rects/handles; re-registered per show
    segment_ = segment;
    main_ = main;
    settings_ = s;
    sections_ = sections;
    hoverRow_ = -1;
    hoverCardX_ = -1;
    reconcileAtTick_ = 0;

    // Flatten sections into rows; skip empty sections entirely.
    items_.clear();
    rows_.clear();
    g_iconCache.clear();
    const bool withHeaders = sections.size() > 1;
    for (const auto& sec : sections) {
        if (sec.items.empty()) continue;
        if (withHeaders) {
            RowInfo h{};
            h.isHeader = true;
            h.label = sec.label;
            rows_.push_back(h);
        }
        for (const auto& it : sec.items) {
            RowInfo r{};
            r.item = static_cast<int>(items_.size());
            r.card = s.thumbnails;   // minimized too: snapshot card
            rows_.push_back(r);
            items_.push_back(it);
        }
    }

    EnsureRenderStack();
    if (!stackOk_) return;

    // DPI of the monitor the popup will live on.
    const HMONITOR anchor = s.side == PopupSide::Main
                                ? main.handle
                                : segment.neighbor;
    UINT dpiX = 96, dpiY = 96;
    if (anchor) GetDpiForMonitor(anchor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    scale_ = dpiX / 96.0f;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR hostMon = anchor ? anchor : MonitorFromPoint(cursor,
                                                          MONITOR_DEFAULTTONEAREST);
    RECT rcWork{};
    if (hostMon && GetMonitorInfoW(hostMon, &mi)) {
        rcWork = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    }

    // Width: as narrow as the content allows -
    // titles are measured uncapped and ellipsize at the popup edge -
    // floored at kMinPopupW, capped by the mode's width setting (list =
    // max_width, cards = card_width). The protruding close tab is above
    // the panel, so rows no longer reserve X room.
    EnsureScaleResources(scale_);
    const float nat = NaturalWidth();
    const float w = std::min(std::max(nat, kMinPopupW * scale_),
                             ModeWidthOf(s) * scale_);

    // Height: headers + rows (cards when thumbnails are on), plus the
    // close tab's protrusion above the panel, capped to the work
    // area. The slot height depends on the final width. The strip also
    // hosts the card-close X, so it exists whenever the buttons are on.
    const float stripH =
        (s.showTitles || s.showIcons || s.cardCloseButtons)
            ? kThumbStripH
            : 0.0f;
    const float slotH = CardSlotH(w);
    const float rowH = kRowH * scale_;
    const float headerH = kHeaderH * scale_;
    const float cardH =
        (kCardPadY + kCardPadBottom) * scale_ + slotH + stripH * scale_;
    auto rowHeight = [&](const RowInfo& r) {
        return r.isHeader ? headerH : (r.card ? cardH : rowH);
    };
    float totalRowsH = 0;
    for (const auto& r : rows_)
        totalRowsH += rowHeight(r);
    if (rows_.empty()) totalRowsH = 2 * rowH;   // "no windows" state
    const float availH = static_cast<float>(rcWork.bottom - rcWork.top) -
                         kXProtrude * scale_;
    // Overflow no longer drops rows - every row stays and the list scrolls
    //. The window caps at the work area; the content is a viewport
    // into the full layout.
    const float viewportH = totalRowsH < availH ? totalRowsH : availH;
    maxScroll_ = totalRowsH - viewportH;
    scrollOff_ = 0;
    const float h = viewportH + kXProtrude * scale_;

    // Anchor to the seam; side per setting.
    const float gap = 8.0f * scale_;
    int x, y;
    const RECT& st = segment_.strip;
    if (segment_.edge == Edge::Right) {
        x = static_cast<int>(s.side == PopupSide::Main
                                 ? st.left - w - gap
                                 : st.right + gap);
        y = static_cast<int>(cursor.y - h / 2);
    } else if (segment_.edge == Edge::Left) {
        x = static_cast<int>(s.side == PopupSide::Main
                                 ? st.right + gap
                                 : st.left - w - gap);
        y = static_cast<int>(cursor.y - h / 2);
    } else if (segment_.edge == Edge::Bottom) {
        x = static_cast<int>(cursor.x - w / 2);
        y = static_cast<int>(s.side == PopupSide::Main
                                 ? st.top - h - gap
                                 : st.bottom + gap);
    } else {   // Top
        x = static_cast<int>(cursor.x - w / 2);
        y = static_cast<int>(s.side == PopupSide::Main
                                 ? st.bottom + gap
                                 : st.top - h - gap);
    }
    // Clamp into the host monitor's work area.
    x = x < rcWork.left ? rcWork.left
                        : (x + static_cast<int>(w) > rcWork.right
                               ? rcWork.right - static_cast<int>(w)
                               : x);
    y = y < rcWork.top ? rcWork.top
                       : (y + static_cast<int>(h) > rcWork.bottom
                              ? rcWork.bottom - static_cast<int>(h)
                              : y);

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y,
                 static_cast<int>(w), static_cast<int>(h),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;

    // Pop-OUT anchor: the midpoint of the popup edge that
    // faces the seam, in client coordinates - the pop-in extrudes from
    // the monitor border instead of growing from the center.
    const bool mainSide = s.side == PopupSide::Main;
    int ax, ay;
    switch (segment_.edge) {
        case Edge::Right:
            ax = mainSide ? static_cast<int>(w) : 0;
            ay = static_cast<int>(h) / 2;
            animVerticalSeam_ = true;
            break;
        case Edge::Left:
            ax = mainSide ? 0 : static_cast<int>(w);
            ay = static_cast<int>(h) / 2;
            animVerticalSeam_ = true;
            break;
        case Edge::Bottom:
            ax = static_cast<int>(w) / 2;
            ay = mainSide ? static_cast<int>(h) : 0;
            animVerticalSeam_ = false;
            break;
        default:   // Top
            ax = static_cast<int>(w) / 2;
            ay = mainSide ? 0 : static_cast<int>(h);
            animVerticalSeam_ = false;
            break;
    }
    animAnchorClient_ = { ax, ay };

    lastInsideTick_ = GetTickCount64();
    showTick_ = lastInsideTick_;
    stayAnchorTick_ = showTick_;   // stay window starts at show, resets on hover
    hoverX_ = false;
    SetTimer(hwnd_, kPollTimer, kPollMs, nullptr);
    // Commit the invisible start state before the window appears so the
    // first shown frame is already at scale/opacity 0 - no flash.
    ApplyAnimStart();
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y,
                 static_cast<int>(w), static_cast<int>(h),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
    Render();
    StartAnimation();
    // Preview work waits out the pop-in (kThumbDelayMs): live
    // thumbnails then register in both close-button modes - the poll
    // timer drives it.
    thumbsPending_ = s.thumbnails;
}

void Popup::Hide(bool capture) {
    if (!hwnd_) return;
    KillTimer(hwnd_, kPollTimer);
    KillTimer(hwnd_, kVerifyFgTimer);
    UnghostAll();                  // never leave a ghosted desktop behind
    UnregisterThumbnails();   // plan: previews live only while visible
    // Snap the visual to rest (also cancels a running animation) so the
    // next Show starts from a clean state.
    if (g_scaleTf) {
        g_scaleTf->SetScaleX(1.0f);
        g_scaleTf->SetScaleY(1.0f);
    }
    if (g_visual3) g_visual3->SetOpacity(1.0f);
    if (g_dcomp) g_dcomp->Commit();
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
    hoverRow_ = -1;
    // Snapshots now read the composed screen, so let the DWM drop this
    // popup's pixels first - chain mode can list windows that sit under
    // it, and a capture racing the hide would embed popup pixels.
    // capture=false: the caller (the click-to-switch path) is in a
    // hurry - the switch must run while the click's input entitlement
    // is still ours - and defers the settle+capture to its verify tick.
    if (capture && settings_.thumbnails && !items_.empty()) {
        Sleep(32);
        CacheSnapshots(items_);
    }
}

void Popup::OnOutsideMouseDown(POINT pt) {
    if (!Visible()) return;
    RECT rc;
    if (GetWindowRect(hwnd_, &rc) && !PtInRect(&rc, pt)) {
        Hide();
    }
}

// Overflow scrolling. Wheel notches arrive from the LL hook (the
// popup never has focus, so the OS would hand the wheel to another
// window); events outside the popup are ignored. Sub-notch touchpad
// deltas accumulate until a full notch assembles. Live previews must be
// re-registered at the scrolled slots - their dests are window coords -
// while still-frame cards are swapchain content and scroll for free.
void Popup::OnMouseWheel(int delta) {
    if (!Visible() || maxScroll_ <= 0 || delta == 0) return;
    POINT pt{};
    RECT rc{};
    if (!GetCursorPos(&pt) || !GetWindowRect(hwnd_, &rc) ||
        !PtInRect(&rc, pt)) {
        return;
    }
    wheelCarry_ += delta;
    const int notches = wheelCarry_ / WHEEL_DELTA;   // truncates toward 0
    if (notches == 0) return;
    wheelCarry_ -= notches * WHEEL_DELTA;
    float off = scrollOff_ - notches * kWheelStep * scale_;
    off = off < 0 ? 0 : (off > maxScroll_ ? maxScroll_ : off);
    if (off == scrollOff_) return;
    scrollOff_ = off;
    Render();
    UnregisterThumbnails();
    RegisterThumbnails();
    RecomputeHover();
}

// Replace the popup's content while it stays up: same window,
// same position, no pop-in - used after a card close to reconcile with
// the real window list.
void Popup::Refresh(const std::vector<PopupSection>& sections) {
    if (!hwnd_ || !visible_) return;
    sections_ = sections;
    RebuildInPlace();
}

void Popup::UpdateCardXHover(int row) {
    if (row == hoverCardX_) return;
    hoverCardX_ = row;
    Render();
}

// Close the row's window: WM_CLOSE, drop the card at once (tab-
// close feel), then reconcile a beat later - a confirmation dialog can
// veto the close, and the reconcile brings the card back if it did.
void Popup::CloseCard(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const RowInfo& r = rows_[row];
    if (r.isHeader || !r.card || r.item < 0 ||
        r.item >= static_cast<int>(items_.size()))
        return;
    HWND target = items_[r.item].hwnd;
    if (!target || !IsWindow(target)) return;
    PLog(L"popup: card close hwnd=0x%p", target);
    PostMessageW(target, WM_CLOSE, 0, 0);
    for (auto& sec : sections_) {
        auto& v = sec.items;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [target](const WindowItem& it) {
                                   return it.hwnd == target;
                               }),
                v.end());
    }
    reconcileAtTick_ = GetTickCount64() + kReconcileMs;
    RebuildInPlace();
}

// Re-flatten sections_ into rows and re-layout at the CURRENT top-left:
// the popup shrinks/grows in place, no animation. Thumbnails re-register
// immediately unless the show pop-in is still settling.
void Popup::RebuildInPlace() {
    if (!hwnd_ || !visible_ || !stackOk_) return;
    UnregisterThumbnails();

    items_.clear();
    rows_.clear();
    const bool withHeaders = sections_.size() > 1;
    for (const auto& sec : sections_) {
        if (sec.items.empty()) continue;
        if (withHeaders) {
            RowInfo h{};
            h.isHeader = true;
            h.label = sec.label;
            rows_.push_back(h);
        }
        for (const auto& it : sec.items) {
            RowInfo r{};
            r.item = static_cast<int>(items_.size());
            r.card = settings_.thumbnails;
            rows_.push_back(r);
            items_.push_back(it);
        }
    }
    if (items_.empty()) {
        Hide();   // the last window on the list just closed
        return;
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT rcWork{};
    const HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        rcWork = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    }

    // Same sizing pass as Show, anchored to the
    // current top-left.
    EnsureScaleResources(scale_);
    const float nat = NaturalWidth();
    const float w = std::min(std::max(nat, kMinPopupW * scale_),
                             ModeWidthOf(settings_) * scale_);

    const float stripH =
        (settings_.showTitles || settings_.showIcons ||
         settings_.cardCloseButtons)
            ? kThumbStripH
            : 0.0f;
    const float slotH = CardSlotH(w);
    const float rowH = kRowH * scale_;
    const float headerH = kHeaderH * scale_;
    const float cardH =
        (kCardPadY + kCardPadBottom) * scale_ + slotH + stripH * scale_;
    auto rowHeight = [&](const RowInfo& r) {
        return r.isHeader ? headerH : (r.card ? cardH : rowH);
    };
    float totalRowsH = 0;
    for (const auto& r : rows_)
        totalRowsH += rowHeight(r);
    const float availH = static_cast<float>(rcWork.bottom - rcWork.top) -
                         kXProtrude * scale_;
    // Same no-drop rule as Show; a card close shrinking the list
    // keeps the scroll position, clamped into the new range.
    const float viewportH = totalRowsH < availH ? totalRowsH : availH;
    maxScroll_ = totalRowsH - viewportH;
    scrollOff_ = scrollOff_ > maxScroll_ ? maxScroll_ : scrollOff_;
    if (scrollOff_ < 0) scrollOff_ = 0;
    const float h = viewportH + kXProtrude * scale_;

    RECT rcWnd{};
    GetWindowRect(hwnd_, &rcWnd);
    int x = rcWnd.left, y = rcWnd.top;
    x = x < rcWork.left ? rcWork.left
                        : (x + static_cast<int>(w) > rcWork.right
                               ? rcWork.right - static_cast<int>(w)
                               : x);
    y = y < rcWork.top ? rcWork.top
                       : (y + static_cast<int>(h) > rcWork.bottom
                              ? rcWork.bottom - static_cast<int>(h)
                              : y);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y,
                 static_cast<int>(w), static_cast<int>(h),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Render();
    RecomputeHover();
    // Preview work (still-frame capture or thumbnail registration) runs
    // on the next poll tick - no pop-in is running, but batching keeps
    // the rebuild itself hitch-free.
    thumbsPending_ = settings_.thumbnails;
}

// Preview slot height for a card popup of the given width: 16:9
// of the inner width, clamped - narrow cards keep the classic 157 px
// slot, wide cards get proportionally bigger previews.
float Popup::CardSlotH(float popupW) const {
    const float inner = popupW - 2.0f * kPadX * scale_;
    const float byAspect = inner * 9.0f / 16.0f;
    const float lo = kThumbSlotH * scale_;
    const float hi = kThumbSlotHMax * scale_;
    return byAspect < lo ? lo : (byAspect > hi ? hi : byAspect);
}

// Rows shifted under a stationary cursor after a rebuild: re-derive the
// hover from the live cursor position (WM_MOUSEMOVE only fires on moves).
void Popup::RecomputeHover() {
    POINT pt{};
    if (!GetCursorPos(&pt) || !ScreenToClient(hwnd_, &pt)) {
        UpdateHoverX(false);
        UpdateCardXHover(-1);
        UpdateHover(-1);
        return;
    }
    if (PtInRect(&xRc_, pt)) {
        UpdateHoverX(true);
        UpdateCardXHover(-1);
        UpdateHover(-1);
        return;
    }
    UpdateHoverX(false);
    int cardX = -1, hover = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].isHeader) continue;
        if (PtInRect(&rows_[i].xCard, pt)) cardX = static_cast<int>(i);
        if (PtInRect(&rows_[i].rc, pt)) hover = static_cast<int>(i);
    }
    UpdateCardXHover(cardX);
    UpdateHover(hover);
}

int Popup::ItemFromPoint(POINT pt) const {
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].isHeader) continue;
        if (PtInRect(&rows_[i].rc, pt)) return rows_[i].item;
    }
    return -1;
}

void Popup::UpdateHover(int row) {
    if (row == hoverRow_) return;
    hoverRow_ = row;
    if (settings_.peek) UpdateHoverGhosting();
    Render();
}

// Peek-on-hover: the hovered entry's window stays visible,
// every other window on its monitor hides until the hover moves on.
void Popup::UpdateHoverGhosting() {
    if (hoverRow_ >= 0 && hoverRow_ < static_cast<int>(rows_.size()) &&
        !rows_[hoverRow_].isHeader && rows_[hoverRow_].item >= 0) {
        GhostAllBut(items_[rows_[hoverRow_].item].hwnd,
                    settings_.virtualDesktops);
    } else {
        UnghostAll();
    }
}

void Popup::UpdateHoverX(bool over) {
    if (over == hoverX_) return;
    hoverX_ = over;
    Render();
}

bool Popup::Suppressed() const {
    return suppressUntil_ != 0 && GetTickCount64() < suppressUntil_;
}

// Explicit dismissal (X button / Esc): close and pause the trigger for the
// configured delay so an accidentally-fired popup doesn't immediately
// come back.
void Popup::DismissSuppressed() {
    suppressUntil_ =
        GetTickCount64() + static_cast<ULONGLONG>(settings_.reactivateDelayS) * 1000ull;
    PLog(L"popup: dismissed via X/Esc; trigger paused for %u s",
         settings_.reactivateDelayS);
    Hide();
}

void Popup::ActivateItem(int item) {
    if (item < 0 || item >= static_cast<int>(items_.size())) return;
    HWND target = items_[item].hwnd;
    HWND fgNow0 = GetForegroundWindow();
    if (!target || !IsWindow(target)) {
        Hide();
        return;
    }
    // Switch FIRST, hide after: the foreground right a real click
    // grants this process is fleeting - every millisecond between the
    // button-down and the SetForegroundWindow (the old Hide() ran a
    // 32 ms settle sleep plus screen captures here) is a window where
    // the button-up, delivered to whatever sits under the vanishing
    // popup, hands that right to another queue and the switch is
    // silently refused. Hide() runs right after, its slow capture
    // deferred to the verify tick.
    UnregisterThumbnails();   // previews die with the popup either way
    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    // Leaving a KNOWN re-asserter: any soft switch will land, bounce
    // within ~100 ms and force anyway - skip that juggling and force
    // in one decisive (animation-free) transition.
    DWORD fgPid = 0;
    if (fgNow0) GetWindowThreadProcessId(fgNow0, &fgPid);
    if (fgPid && g_fighterPids.count(fgPid)) {
        PLog(L"activate: fg pid %lu is a known re-asserter - forcing",
             fgPid);
        ForceForeground(target);
    } else {
        FakeKeyPress();
        const BOOL sfgw0 = SetForegroundWindow(target);
        PLog(L"activate: target=0x%p fg-before=0x%p SHIFT-SFGW -> %d "
             L"fg=0x%p",
             target, fgNow0, static_cast<int>(sfgw0),
             GetForegroundWindow());
    }

    Hide(false);
    capturePending_ = settings_.thumbnails && !items_.empty();
    verifyNullRetries_ = 0;
    verifyStick_ = false;
    verifyDanced_ = false;
    // The switch lands ASYNCHRONOUSLY: checking immediately used to fail
    // and fire the escalation (whose minimize/restore step animates the
    // window out of the taskbar even when it was never minimized).
    // Verify after a beat instead.
    verifyTarget_ = target;
    SetTimer(hwnd_, kVerifyFgTimer, 150, nullptr);
}

void Popup::VerifyForeground() {
    if (!verifyTarget_ || !IsWindow(verifyTarget_)) {
        verifyTarget_ = nullptr;
        verifyStick_ = false;
        return;
    }
    const HWND target = verifyTarget_;

    if (GetForegroundWindow() == target) {
        if (verifyStick_) {
            // Landed AND stuck - done.
            verifyTarget_ = nullptr;
            verifyStick_ = false;
            return;
        }
        // Landed - but a soft switch can BOUNCE back within ~100 ms
        // (observed: an app that was peek-moved off-screen and back
        // re-asserts itself right after losing focus, and the OS
        // accepts the re-assert). Confirm it sticks before calling
        // the switch done.
        verifyStick_ = true;
        SetTimer(hwnd_, kVerifyFgTimer, 150, nullptr);
        return;
    }

    if (!verifyStick_) {
        HWND fgNow = GetForegroundWindow();
        // A refused switch on this build momentarily reports NO
        // foreground (NULL) while the OS quietly puts the old window
        // back - and the attach workaround below needs a real
        // foreground thread. Bounded wait-it-out via the timer
        // instead of skipping the workaround.
        if (!fgNow && verifyNullRetries_ < 3) {
            ++verifyNullRetries_;
            SetTimer(hwnd_, kVerifyFgTimer, 100, nullptr);
            return;
        }
        verifyNullRetries_ = 0;
        PLog(L"verify: target=0x%p fg=0x%p (initial switch failed)",
             target, fgNow);

        // Escalation 1: ALT held down ACROSS the retry - the
        // canonical unlock; press-release before the call proved too
        // weak here.
        FakeAltHold(true);
        const BOOL alt = SetForegroundWindow(target);
        FakeAltHold(false);
        PLog(L"verify: alt-hold SFGW -> %d fg=0x%p", static_cast<int>(alt),
             GetForegroundWindow());
        if (GetForegroundWindow() == target) {
            verifyStick_ = true;
            SetTimer(hwnd_, kVerifyFgTimer, 150, nullptr);
            return;
        }

        // Escalation 2: join the foreground's input queue - the
        // documented workaround for the A -> B -> A refusal.
        const DWORD ourThread = GetCurrentThreadId();
        HWND fg = GetForegroundWindow();
        const DWORD fgThread =
            fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        const DWORD tgtThread = GetWindowThreadProcessId(target, nullptr);
        if (fgThread && tgtThread && fgThread != tgtThread) {
            const BOOL a1 = AttachThreadInput(ourThread, fgThread, TRUE);
            const BOOL a2 =
                AttachThreadInput(ourThread, tgtThread, TRUE);
            const BOOL sfgw = SetForegroundWindow(target);
            BringWindowToTop(target);
            PLog(L"verify: attach(%d,%d) SFGW->%d fg=0x%p",
                 static_cast<int>(a1), static_cast<int>(a2),
                 static_cast<int>(sfgw), GetForegroundWindow());
            AttachThreadInput(ourThread, tgtThread, FALSE);
            AttachThreadInput(ourThread, fgThread, FALSE);
            if (GetForegroundWindow() == target) {
                verifyStick_ = true;
                SetTimer(hwnd_, kVerifyFgTimer, 150, nullptr);
                return;
            }
        } else {
            PLog(L"verify: attach skipped (fgThread=%u tgtThread=%u)",
                 fgThread, tgtThread);
        }
    } else {
        // The landed switch BOUNCED - the loser took the foreground
        // back. Register the re-asserter so the NEXT switch away from
        // it forces in one step instead of juggling, then take the
        // foreground back by force now.
        verifyStick_ = false;
        HWND th = GetForegroundWindow();
        DWORD thPid = 0;
        if (th) GetWindowThreadProcessId(th, &thPid);
        if (thPid) {
            if (g_fighterPids.size() >= 16) g_fighterPids.clear();
            g_fighterPids.insert(thPid);
        }
        if (verifyDanced_) {
            verifyTarget_ = nullptr;
            PLog(L"popup: foreground switch refused even after "
                 L"escalation + force (hwnd=0x%p)",
                 target);
            return;
        }
        PLog(L"verify: switch bounced (re-asserter pid %lu) - forcing",
             thPid);
    }

    // Final rung (and the bounce path): the forced switch -
    // SwitchToThisWindow (animation-free) with the minimize/restore
    // dance as its fallback. It is last because soft rungs, when they
    // stick, are the least surprising transition; a forced switch is
    // still far better than a dead click.
    verifyDanced_ = true;
    ForceForeground(target);
    if (GetForegroundWindow() == target) {
        PLog(L"verify: force ok");
        verifyStick_ = true;
        SetTimer(hwnd_, kVerifyFgTimer, 350, nullptr);
        return;
    }
    verifyTarget_ = nullptr;
    verifyStick_ = false;
    PLog(L"popup: foreground switch refused even after escalation "
         L"(hwnd=0x%p)",
         target);
}

// Live DWM previews (phase 5), registered in both close-button modes -
// the X rides the title strip, never on the preview.
// Registered only while the popup is visible, and delayed past the
// compositor pop-in: IDCompositionAnimation moves the visual tree, not
// DWM-composited thumbnails, so an early registration would sit frozen
// while the card scales in. The destination rect letterboxes the
// source's client size inside the card's preview slot.
// Failures (dead window, cross-adapter, protected content) keep the
// card's placeholder; the holes are punched by the re-render at the end.
void Popup::RegisterThumbnails() {
    thumbsPending_ = false;
    if (!hwnd_ || !visible_) return;
    // Scroll viewport: thumbnails letterbox within the VISIBLE
    // part of each slot - a dest reaching above the viewport would
    // composite over the close-tab band (they float above
    // every window pixel), and slots scrolled fully out are skipped.
    RECT rcCli{};
    GetClientRect(hwnd_, &rcCli);
    const LONG vTop = static_cast<LONG>(kXProtrude * scale_);
    const LONG vBot = rcCli.bottom;
    int live = 0, cards = 0;
    for (auto& row : rows_) {
        if (row.isHeader || !row.card) continue;
        ++cards;
        if (row.thumbSlot.right <= row.thumbSlot.left) continue;
        RECT slot = row.thumbSlot;
        if (slot.top < vTop) slot.top = vTop;
        if (slot.bottom > vBot) slot.bottom = vBot;
        if (slot.bottom <= slot.top) continue;
        const WindowItem& it = items_[row.item];
        if (!it.hwnd || !IsWindow(it.hwnd)) continue;
        if (it.minimized || IsIconic(it.hwnd)) {
            continue;   // DWM renders minimized sources blank;
                        // the snapshot card carries it
        }

        HTHUMBNAIL th = nullptr;
        HRESULT hr = DwmRegisterThumbnail(hwnd_, it.hwnd, &th);
        if (FAILED(hr)) {
            PLog(L"thumb: register failed hr=0x%08X", hr);
            continue;
        }
        SIZE src{};
        hr = DwmQueryThumbnailSourceSize(th, &src);
        if (FAILED(hr) || src.cx <= 0 || src.cy <= 0) {
            if (th) DwmUnregisterThumbnail(th);
            PLog(L"thumb: source size failed/empty hr=0x%08X", hr);
            continue;
        }
        const LONG sw = slot.right - slot.left;
        const LONG sh = slot.bottom - slot.top;
        const double k = std::min(sw / static_cast<double>(src.cx),
                                  sh / static_cast<double>(src.cy));
        const LONG dw = static_cast<LONG>(src.cx * k + 0.5);
        const LONG dh = static_cast<LONG>(src.cy * k + 0.5);
        const LONG dx = slot.left + (sw - dw) / 2;
        const LONG dy = slot.top + (sh - dh) / 2;
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                    DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.fSourceClientAreaOnly = TRUE;
        p.opacity = 255;
        p.fVisible = TRUE;
        p.rcDestination = { dx, dy, dx + dw, dy + dh };
        hr = DwmUpdateThumbnailProperties(th, &p);
        if (FAILED(hr)) {
            DwmUnregisterThumbnail(th);
            PLog(L"thumb: update props failed hr=0x%08X", hr);
            continue;
        }
        row.thumbDest = p.rcDestination;
        row.thumbLive = true;
        thumbs_[it.hwnd] = th;
        ++live;
    }
    if (cards > 0) PLog(L"popup: thumbnails %d/%d live", live, cards);
    if (live > 0) Render();   // punch the holes for the live ones
}

void Popup::UnregisterThumbnails() {
    for (auto& kv : thumbs_)
        if (kv.second) DwmUnregisterThumbnail(kv.second);
    thumbs_.clear();
    for (auto& row : rows_) row.thumbLive = false;
    thumbsPending_ = false;
}

void Popup::ApplyAnimStart() {
    if (!g_scaleTf || !g_dcomp) return;
    g_scaleTf->SetCenterX(static_cast<float>(animAnchorClient_.x));
    g_scaleTf->SetCenterY(static_cast<float>(animAnchorClient_.y));
    if (animVerticalSeam_) {
        g_scaleTf->SetScaleX(0.30f);
        g_scaleTf->SetScaleY(0.80f);
    } else {
        g_scaleTf->SetScaleX(0.80f);
        g_scaleTf->SetScaleY(0.30f);
    }
    if (g_visual3) g_visual3->SetOpacity(0.0f);
    g_dcomp->Commit();
}

// Compositor-driven pop-out: the seam-facing axis extrudes
// 0.30 -> 1 with a few percent overshoot, the cross axis 0.80 -> 1, opacity
// 0 -> 1 over the first 130 ms. IDCompositionAnimation hands the curves to
// DComp once - the compositor interpolates at display rate, so there is no
// app-side timer to quantize the motion (the old 15 ms timer stepped in
// GetTickCount64-sized jumps: the "weird jumpy" feel).
void Popup::StartAnimation() {
    if (!g_dcomp || !g_visual || !g_scaleTf) return;
    g_scaleTf->SetCenterX(static_cast<float>(animAnchorClient_.x));
    g_scaleTf->SetCenterY(static_cast<float>(animAnchorClient_.y));

    // scale(u) = c0 + c1*u + c2*u^2 + c3*u^3 across [0, dur]; AddCubic
    // takes seconds, so divide the coefficients by the duration powers.
    const double dur = 0.22, opDur = 0.13;
    ComPtr<IDCompositionAnimation> aSeam, aCross, aOp;
    auto make = [&](float c0, float c1, float c2, float c3, double d,
                    IDCompositionAnimation** out) {
        if (FAILED(g_dcomp->CreateAnimation(out))) return;
        (*out)->Reset();
        (*out)->AddCubic(0.0, c0,
                         static_cast<float>(c1 / d),
                         static_cast<float>(c2 / (d * d)),
                         static_cast<float>(c3 / (d * d * d)));
        (*out)->End(d, 1.0f);
    };
    // Seam axis from 0.30: 0.30 + 0.70 * (4.2u - 5.4u^2 + 2.2u^3).
    make(0.30f, 0.70f * kSeamU, 0.70f * kSeamV, 0.70f * kSeamW, dur,
         aSeam.GetAddressOf());
    // Cross axis from 0.80: 0.80 + 0.20 * (3u^2 - 2u^3).
    make(0.80f, 0.0f, 0.20f * kSmoothV, 0.20f * kSmoothW, dur,
         aCross.GetAddressOf());
    // Opacity from 0: 3u^2 - 2u^3.
    make(0.0f, 0.0f, kSmoothV, kSmoothW, opDur, aOp.GetAddressOf());
    if (!aSeam || !aCross || !aOp) return;

    if (animVerticalSeam_) {
        g_scaleTf->SetScaleX(aSeam.Get());
        g_scaleTf->SetScaleY(aCross.Get());
    } else {
        g_scaleTf->SetScaleX(aCross.Get());
        g_scaleTf->SetScaleY(aSeam.Get());
    }
    if (g_visual3) g_visual3->SetOpacity(aOp.Get());
    g_dcomp->Commit();
}

void Popup::Render() {
    if (!g_ctx || !g_swap) return;

    RECT rc;
    if (!GetClientRect(hwnd_, &rc)) return;
    const UINT w = rc.right - rc.left;
    const UINT h = rc.bottom - rc.top;
    if (w == 0 || h == 0) return;

    if (g_swapSize.cx != static_cast<LONG>(w) ||
        g_swapSize.cy != static_cast<LONG>(h)) {
        g_ctx->SetTarget(nullptr);
        HRESULT hr = g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) return;
        g_swapSize = { static_cast<LONG>(w), static_cast<LONG>(h) };
    }

    // Per-scale cached resources.
    EnsureScaleResources(scale_);

    ComPtr<IDXGISurface> surface;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&surface)))) return;
    ComPtr<ID2D1Bitmap1> bmp;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1_PIXEL_FORMAT{ DXGI_FORMAT_B8G8R8A8_UNORM,
                           D2D1_ALPHA_MODE_PREMULTIPLIED });
    if (FAILED(g_ctx->CreateBitmapFromDxgiSurface(surface.Get(), bp, &bmp))) {
        return;
    }
    g_ctx->SetTarget(bmp.Get());
    g_ctx->BeginDraw();

    // Panel: transparent outside the rounded rect, translucent inside
    // (premultiplied alpha throughout). The rect starts BELOW the close
    // tab's protrusion band.
    const float protrude = kXProtrude * scale_;
    g_ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    ComPtr<ID2D1SolidColorBrush> bg;
    ComPtr<ID2D1RoundedRectangleGeometry> panel;
    g_ctx->CreateSolidColorBrush(
        D2D1::ColorF(kColPanelR, kColPanelG, kColPanelB, 0.97f), &bg);
    D2D1_ROUNDED_RECT rr =
        D2D1::RoundedRect(D2D1::RectF(0.5f, protrude + 0.5f,
                                      static_cast<float>(w) - 0.5f,
                                      static_cast<float>(h) - 0.5f),
                          kCornerR * scale_, kCornerR * scale_);
    if (SUCCEEDED(g_d2dFactory->CreateRoundedRectangleGeometry(
            rr, &panel))) {
        g_ctx->FillGeometry(panel.Get(), bg.Get());
    } else {
        g_ctx->Clear(D2D1::ColorF(kColPanelR, kColPanelG, kColPanelB, 0.97f));
    }

    const float rowH = kRowH * scale_;
    const float headerH = kHeaderH * scale_;
    const float pad = kPadX * scale_;
    const float iconSz = kIconSz * scale_;
    const bool strip = settings_.showTitles || settings_.showIcons ||
                       settings_.cardCloseButtons;
    const float slotH = CardSlotH(static_cast<float>(w));
    const float cardH =
        (kCardPadY + kCardPadBottom +
         (strip ? kThumbStripH : 0.0f)) *
        scale_ + slotH;

    if (rows_.empty()) {
        if (g_itemFmt && g_dim) {
            g_ctx->DrawTextW(L"No windows on this monitor", 26,
                             g_itemFmt.Get(),
                             D2D1::RectF(pad, protrude + rowH * 0.5f,
                                         static_cast<float>(w) - pad,
                                         protrude + rowH * 1.5f),
                             g_dim.Get());
        }
    } else {
        // Shared title drawing: uncapped single line, ellipsized by the
        // layout at the popup edge; minimized windows
        // render dimmed. `shadow` adds the drop used by floating labels
        // (over-mode) so text stays readable on bright previews.
        // `reserveR` keeps the last `reserveR` px of the row free (the
        // card close X lives there) so the text ellipsizes short of it.
        auto drawTitle = [&](const WindowItem& item, float textLeft,
                             float yTop, float h2, bool shadow = false,
                             float reserveR = 0.0f) {
            if (!settings_.showTitles || !g_itemFmt || !g_text) return;
            const float maxW =
                static_cast<float>(w) - textLeft - pad - reserveR;
            ComPtr<IDWriteTextLayout> layout;
            g_dwFactory->CreateTextLayout(
                item.title.c_str(),
                static_cast<UINT32>(item.title.size()),
                g_itemFmt.Get(), maxW, h2, &layout);
            if (layout) {
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING trim{};
                trim.granularity =
                    DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                layout->SetTrimming(&trim, g_ellipsis.Get());
                if (shadow && g_textShadow) {
                    g_ctx->DrawTextLayout(
                        D2D1::Point2F(textLeft + 1.0f * scale_,
                                      yTop + 1.0f * scale_),
                        layout.Get(), g_textShadow.Get());
                }
                g_ctx->DrawTextLayout(
                    D2D1::Point2F(textLeft, yTop), layout.Get(),
                    item.minimized ? g_dim.Get() : g_text.Get());
            }
        };

        // Scroll viewport: rows lay out from the full content top
        // minus the scroll offset, clipped to the panel below the close
        // tab's protrusion - scrolled-out rows never paint outside it.
        g_ctx->PushAxisAlignedClip(
            D2D1::RectF(0.0f, protrude, static_cast<float>(w),
                        static_cast<float>(h)),
            D2D1_ANTIALIAS_MODE_ALIASED);
        float top = protrude - scrollOff_;
        for (size_t r = 0; r < rows_.size(); ++r) {
            RowInfo& row = rows_[r];
            const float rh =
                row.isHeader ? headerH : (row.card ? cardH : rowH);
            row.rc = { 0, static_cast<LONG>(top), static_cast<LONG>(w),
                       static_cast<LONG>(top + rh) };

            if (row.isHeader) {
                if (g_headerFmt && g_dim) {
                    g_ctx->DrawTextW(row.label.c_str(),
                                     static_cast<UINT32>(row.label.size()),
                                     g_headerFmt.Get(),
                                     D2D1::RectF(pad, top,
                                                 static_cast<float>(w) - pad,
                                                 top + headerH),
                                     g_header ? g_header.Get()
                                              : g_dim.Get());
                }
                top += rh;
                continue;
            }

            const WindowItem& item = items_[row.item];
            const bool hot = static_cast<int>(r) == hoverRow_;

            if (row.card) {
                // Thumbnail card: preview slot + title strip under it.
                // Minimized windows keep the card too -
                // DWM cannot render them live, so their slot
                // shows the last-known snapshot (taskbar behavior) with
                // the icon placeholder behind it. The accent border (not
                // a fill) is the hover signal - it reads however DWM
                // composites the preview.
                //
                // Per-card close: the button rides the title strip
                // below the slot, flush right - a live DWM thumbnail
                // composites above everything, so the X must sit outside
                // the preview pixels (drawn with the strip, below).
                const float slotTop = top + kCardPadY * scale_;
                const float slotL = pad;
                const float slotR = static_cast<float>(w) - pad;
                const float slotB = slotTop + slotH;
                row.thumbSlot = { static_cast<LONG>(slotL),
                                  static_cast<LONG>(slotTop),
                                  static_cast<LONG>(slotR),
                                  static_cast<LONG>(slotB) };
                if (hot && g_header) {
                    g_ctx->DrawRectangle(
                        D2D1::RectF(1.0f, top + 1.0f,
                                    static_cast<float>(w) - 1.0f,
                                    top + rh - 1.0f),
                        g_header.Get(), 1.5f * scale_);
                }
                // Slot placeholder: the panel behind the letterbox bars,
                // during the registration delay, and permanently when
                // registration failed or the window is minimized.
                ComPtr<ID2D1SolidColorBrush> slotBg;
                g_ctx->CreateSolidColorBrush(
                    D2D1::ColorF(0x1E / 255.0f, 0x1B / 255.0f, 0x1B / 255.0f,
                                 1.0f),
                    &slotBg);
                if (slotBg) {
                    g_ctx->FillRectangle(D2D1::RectF(slotL, slotTop, slotR,
                                                     slotB),
                                         slotBg.Get());
                }
                if (!row.thumbLive) {
                    // Still frame (default; permanently so for
                    // minimized windows -): swapchain
                    // content, so it rides the pop-in animation.
                    auto sn = g_snapCache.find(item.hwnd);
                    if (sn != g_snapCache.end() && sn->second) {
                        const D2D1_SIZE_F sz = sn->second->GetSize();
                        if (sz.width > 0 && sz.height > 0) {
                            const double k =
                                std::min((slotR - slotL) / sz.width,
                                         (slotB - slotTop) / sz.height);
                            const float dw =
                                static_cast<float>(sz.width * k);
                            const float dh =
                                static_cast<float>(sz.height * k);
                            const float dx =
                                slotL + (slotR - slotL - dw) / 2;
                            const float dy =
                                slotTop + (slotB - slotTop - dh) / 2;
                            g_ctx->DrawBitmap(
                                sn->second.Get(),
                                D2D1::RectF(dx, dy, dx + dw, dy + dh));
                        }
                    } else if (settings_.showIcons && item.icon) {
                        const float bi = 40.0f * scale_;
                        ID2D1Bitmap* iconBmp =
                            CardIconFor(item,
                                        static_cast<UINT>(bi + 0.5f));
                        if (iconBmp) {
                            const float cx = (slotL + slotR) / 2;
                            const float cy = (slotTop + slotB) / 2;
                            g_ctx->DrawBitmap(
                                iconBmp,
                                D2D1::RectF(cx - bi / 2, cy - bi / 2,
                                            cx + bi / 2, cy + bi / 2));
                        }
                    }
                }
                if (strip) {
                    const float sTop = slotB;
                    const float sH = kThumbStripH * scale_;
                    // The strip content centers in strip + bottom pad,
                    // not the strip alone: kCardPadBottom is
                    // panel-colored, so the eye counts it as space below
                    // the row - strip-only centering reads top-heavy
                    // (bottom gap looks bigger than the top one).
                    const float opt = kCardPadBottom * scale_ / 2.0f;
                    if (hot && g_hover) {
                        g_ctx->FillRectangle(
                            D2D1::RectF(0, sTop, static_cast<float>(w),
                                        sTop + sH),
                            g_hover.Get());
                    }
                    float textLeft = pad;
                    if (settings_.showIcons && item.icon) {
                        const float si = 20.0f * scale_;
                        const float iy = sTop + (sH - si) / 2 + opt;
                        ID2D1Bitmap* iconBmp =
                            CardIconFor(item,
                                        static_cast<UINT>(si + 0.5f));
                        if (iconBmp) {
                            g_ctx->DrawBitmap(
                                iconBmp,
                                D2D1::RectF(pad, iy, pad + si, iy + si));
                        }
                        textLeft = pad + si + pad * 0.6f;
                    }
                    // Per-card close button at the strip's far right,
                    // in line with the icon and title: a rounded
                    // square, like the native Win11 flyout close. The
                    // title ellipsizes short of it; the button draws
                    // after the text so its backing sits over any
                    // hover fill. Red on X-hover - the destructive
                    // signal.
                    if (settings_.cardCloseButtons) {
                        const float cs = kCardXSz * scale_;
                        const float cx0 =
                            static_cast<float>(w) - pad - cs;
                        const float cy0 =
                            sTop + (sH - cs) / 2.0f + opt;
                        row.xCard = {
                            static_cast<LONG>(cx0 - 2.0f * scale_),
                            static_cast<LONG>(cy0 - 2.0f * scale_),
                            static_cast<LONG>(cx0 + cs + 2.0f * scale_),
                            static_cast<LONG>(cy0 + cs + 2.0f * scale_)};
                        const bool xHot =
                            static_cast<int>(r) == hoverCardX_;
                        const float ccx = cx0 + cs / 2;
                        const float ccy = cy0 + cs / 2;
                        const float xr = kCardXR * scale_;
                        const D2D1_ROUNDED_RECT xrr = D2D1::RoundedRect(
                            D2D1::RectF(cx0, cy0, cx0 + cs, cy0 + cs),
                            xr, xr);
                        ComPtr<ID2D1SolidColorBrush> back;
                        g_ctx->CreateSolidColorBrush(
                            xHot
                                ? D2D1::ColorF(kColRedR, kColRedG, kColRedB,
                                               1.0f)
                                : D2D1::ColorF(0x1E / 255.0f, 0x1B / 255.0f,
                                               0x1B / 255.0f,
                                               hot ? 0.8f : 0.62f),
                            &back);
                        if (back) {
                            g_ctx->FillRoundedRectangle(xrr, back.Get());
                        }
                        if (!xHot) {
                            ComPtr<ID2D1SolidColorBrush> ring;
                            g_ctx->CreateSolidColorBrush(
                                D2D1::ColorF(kColSoftR, kColSoftG,
                                             kColSoftB, 0.35f),
                                &ring);
                            if (ring) {
                                g_ctx->DrawRoundedRectangle(xrr,
                                                            ring.Get(),
                                                            1.0f);
                            }
                        }
                        ComPtr<ID2D1SolidColorBrush> xg;
                        g_ctx->CreateSolidColorBrush(
                            xHot ? D2D1::ColorF(1, 1, 1, 0.95f)
                                 : D2D1::ColorF(kColTextR, kColTextG,
                                                kColTextB, 0.92f),
                            &xg);
                        if (xg) {
                            const float hs = 4.5f * scale_;
                            g_ctx->DrawLine(
                                D2D1::Point2F(ccx - hs, ccy - hs),
                                D2D1::Point2F(ccx + hs, ccy + hs),
                                xg.Get(), 1.8f * scale_,
                                g_roundCap.Get());
                            g_ctx->DrawLine(
                                D2D1::Point2F(ccx - hs, ccy + hs),
                                D2D1::Point2F(ccx + hs, ccy - hs),
                                xg.Get(), 1.8f * scale_,
                                g_roundCap.Get());
                        }
                        drawTitle(item, textLeft, sTop + opt, sH, false,
                                  cs + kCardXGap * scale_);
                    } else {
                        row.xCard = {};
                        drawTitle(item, textLeft, sTop + opt, sH);
                    }
                } else {
                    row.xCard = {};
                }
            } else {
                // Classic icon+title row.
                if (hot && g_hover) {
                    g_ctx->FillRectangle(
                        D2D1::RectF(0, top, static_cast<float>(w),
                                    top + rowH),
                        g_hover.Get());
                }
                const float iconY = top + (rowH - iconSz) / 2;
                if (settings_.showIcons && item.icon) {
                    ID2D1Bitmap* iconBmp =
                        CardIconFor(item, static_cast<UINT>(iconSz + 0.5f));
                    if (iconBmp) {
                        g_ctx->DrawBitmap(
                            iconBmp,
                            D2D1::RectF(pad, iconY, pad + iconSz,
                                        iconY + iconSz));
                    }
                }
                drawTitle(item, pad + iconSz + pad, top, rowH);
            }
            top += rh;
        }
        g_ctx->PopAxisAlignedClip();

        // Scroll indicator: a slim capsule hugging the right edge
        // while the content overflows - indicator only, the wheel is the
        // input. Starts below the close tab's hit rect so the two never
        // fight over a pixel column.
        if (maxScroll_ > 0) {
            const float trT = protrude + 8.0f * scale_;
            const float trB = static_cast<float>(h) - 2.0f * scale_;
            if (trB - trT > 24.0f * scale_) {
                const float trL = static_cast<float>(w) - 7.0f * scale_;
                const float trR = static_cast<float>(w) - 3.0f * scale_;
                const float trackH = trB - trT;
                const float vpH = static_cast<float>(h) - protrude;
                const float thumbH = std::max(
                    24.0f * scale_, trackH * vpH / (maxScroll_ + vpH));
                const float t = scrollOff_ / maxScroll_;
                const float thY = trT + (trackH - thumbH) * t;
                const float sbR = 2.0f * scale_;
                ComPtr<ID2D1SolidColorBrush> sb;
                g_ctx->CreateSolidColorBrush(
                    D2D1::ColorF(kColSoftR, kColSoftG, kColSoftB, 0.18f), &sb);
                if (sb) {
                    g_ctx->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(trL, trT, trR, trB),
                                          sbR, sbR),
                        sb.Get());
                    sb->SetColor(
                        D2D1::ColorF(kColSoftR, kColSoftG, kColSoftB, 0.45f));
                    g_ctx->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(trL, thY, trR,
                                                      thY + thumbH),
                                          sbR, sbR),
                        sb.Get());
                }
            }
        }
    }

    // Punch transparent holes where the live DWM thumbnails sit: DWM
    // composites them into this window and the swapchain must not paint
    // over the preview. PRIMITIVE_BLEND_MIN with an alpha-0 brush zeroes
    // the premultiplied pixels (an erase). This runs AFTER all row drawing
    // so nothing repaints over the holes.
    bool anyLive = false;
    for (const auto& row : rows_)
        if (row.thumbLive) anyLive = true;
    if (anyLive && g_erase) {
        g_ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        g_ctx->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_MIN);
        for (const auto& row : rows_) {
            if (!row.thumbLive) continue;
            g_ctx->FillRectangle(
                D2D1::RectF(static_cast<float>(row.thumbDest.left),
                            static_cast<float>(row.thumbDest.top),
                            static_cast<float>(row.thumbDest.right),
                            static_cast<float>(row.thumbDest.bottom)),
                g_erase.Get());
        }
        g_ctx->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        g_ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    // Popup close: a square tab protruding ABOVE the panel's top
    // edge, fused into it - visibly the popup's own chrome, never part of
    // the first card (the old in-corner X read as "closes that window").
    // Drawn last so its fused bottom overlap sits over the panel edge.
    // Rest: panel-lifted fill + soft glyph and a faint border; hover: teal
    // accent fill + white glyph - the product's "this control is EdgePop"
    // signal (the per-card close buttons go red instead, never teal).
    {
        const float xs = kXBtnSz * scale_;
        const float inset = kXInsetX * scale_;
        const float xl = static_cast<float>(w) - inset - xs;
        xRc_ = { static_cast<LONG>(xl - 3.0f * scale_), 0,
                 static_cast<LONG>(xl + xs + 3.0f * scale_),
                 static_cast<LONG>(xs + 3.0f * scale_) };
        const float cx = xl + xs / 2;
        const float cy = xs / 2;
        ComPtr<ID2D1SolidColorBrush> fill, bord;
        g_ctx->CreateSolidColorBrush(
            hoverX_
                ? D2D1::ColorF(kColAccR, kColAccG, kColAccB, 0.95f)
                : D2D1::ColorF(kColLiftR, kColLiftG, kColLiftB, 1.0f),
            &fill);
        if (fill) {
            const float r6 = 6.0f * scale_;
            // Round top corners; a 6 px overdraw squares the bottom so the
            // tab reads as growing straight out of the panel.
            g_ctx->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(xl, 0, xl + xs, xs), r6, r6),
                fill.Get());
            g_ctx->FillRectangle(
                D2D1::RectF(xl, xs - r6, xl + xs, xs), fill.Get());
        }
        if (!hoverX_) {
            g_ctx->CreateSolidColorBrush(
                D2D1::ColorF(kColSoftR, kColSoftG, kColSoftB, 0.35f), &bord);
            if (bord) {
                const float r6 = 6.0f * scale_;
                g_ctx->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(xl + 0.5f, 0.5f,
                                                  xl + xs - 0.5f,
                                                  xs - 0.5f),
                                      r6, r6),
                    bord.Get(), 1.0f);
            }
        }
        if (g_text || g_dim) {
            ComPtr<ID2D1SolidColorBrush> xg;
            g_ctx->CreateSolidColorBrush(
                hoverX_
                    ? D2D1::ColorF(1, 1, 1, 0.95f)
                    : D2D1::ColorF(kColSoftR, kColSoftG, kColSoftB, 0.95f),
                &xg);
            if (xg) {
                const float hs = 5.0f * scale_;
                g_ctx->DrawLine(D2D1::Point2F(cx - hs, cy - hs),
                                D2D1::Point2F(cx + hs, cy + hs),
                                xg.Get(), 2.0f * scale_);
                g_ctx->DrawLine(D2D1::Point2F(cx - hs, cy + hs),
                                D2D1::Point2F(cx + hs, cy - hs),
                                xg.Get(), 2.0f * scale_);
            }
        }
    }

    g_ctx->EndDraw();
    g_ctx->SetTarget(nullptr);
    g_swap->Present(0, 0);
}

LRESULT CALLBACK Popup::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Popup& self = Instance();
    switch (msg) {
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (PtInRect(&self.xRc_, pt)) {
                self.UpdateHoverX(true);
                self.UpdateCardXHover(-1);
                self.UpdateHover(-1);   // X owns the corner, not the row
                return 0;
            }
            self.UpdateHoverX(false);
            int cardX = -1;
            int hover = -1;
            for (size_t i = 0; i < self.rows_.size(); ++i) {
                if (self.rows_[i].isHeader) continue;
                if (PtInRect(&self.rows_[i].xCard, pt))
                    cardX = static_cast<int>(i);
                if (PtInRect(&self.rows_[i].rc, pt)) {
                    hover = static_cast<int>(i);
                    break;
                }
            }
            // A hovered card close keeps its row hot (border + peek) -
            // only the click is intercepted.
            self.UpdateCardXHover(cardX);
            self.UpdateHover(hover);
            return 0;
        }
        case WM_TIMER: {
            if (wp == kVerifyFgTimer) {
                KillTimer(hwnd, kVerifyFgTimer);
                self.VerifyForeground();
                // Deferred from the click path: by now the popup is
                // hidden and the switch settled, so the capture
                // settle sleep costs the clicker nothing.
                if (self.capturePending_ && !self.verifyTarget_) {
                    self.capturePending_ = false;
                    Sleep(32);
                    CacheSnapshots(self.items_);
                }
                return 0;
            }
            if (wp == kPollTimer) {
                if (GetAsyncKeyState(VK_ESCAPE) & 0x1) {
                    self.DismissSuppressed();   // Esc is an explicit dismiss
                    return 0;
                }
                // Number keys 1-9 jump straight to that entry (
                // note 8) - the popup never takes focus, so the poll
                // timer is the only input path (same as Esc).
                for (int k = 0; k < 9; ++k) {
                    if (!((GetAsyncKeyState(L'1' + k) & 0x1) ||
                          (GetAsyncKeyState(VK_NUMPAD1 + k) & 0x1)))
                        continue;
                    int nth = 0;
                    for (const auto& row : self.rows_) {
                        if (row.isHeader) continue;
                        if (nth == k) {
                            PLog(L"popup: hotkey %d -> item %d", k + 1,
                                 row.item);
                            self.ActivateItem(row.item);
                            break;
                        }
                        ++nth;
                    }
                    break;
                }
                const ULONGLONG now = GetTickCount64();
                // Card-close reconcile: re-ask the app for the real
                // window list - a close-confirmation dialog can veto the
                // WM_CLOSE and the card must come back.
                if (self.reconcileAtTick_ != 0 &&
                    now >= self.reconcileAtTick_) {
                    self.reconcileAtTick_ = 0;
                    if (g_refreshSink) g_refreshSink(self.segment_);
                    if (!self.visible_) return 0;   // emptied out and hid
                }
                // Live previews wait out the pop-in (kThumbDelayMs), then
                // register once - in both close-button modes; the cache's
                // snapshots only cover the gap. The poll timer is the
                // only timer we run.
                if (self.thumbsPending_ &&
                    now - self.showTick_ >= kThumbDelayMs) {
                    self.RegisterThumbnails();
                }
                // Presence-based close: works whichever side of the seam
                // the cursor stopped on (fixes the neighbor-side bug) -
                // but the popup stays up at least stay_s seconds no
                // matter where the cursor went (cross-and-return lands
                // far from the popup,).
                POINT pt;
                if (GetCursorPos(&pt)) {
                    RECT rc;
                    GetWindowRect(hwnd, &rc);
                    // No app hovered unless the cursor is over one:
                    // WM_MOUSEMOVE already clears over gaps/headers inside,
                    // but once the cursor leaves the popup (stay time /
                    // leave grace keep it up) the last row kept its hover
                    // and its peek ghosting - clear it here.
                    if (!PtInRect(&rc, pt)) {
                        self.UpdateHover(-1);
                        self.UpdateHoverX(false);
                        self.UpdateCardXHover(-1);
                    }
                    InflateRect(&rc, static_cast<int>(self.settings_.leaveGracePx),
                                static_cast<int>(self.settings_.leaveGracePx));
                    if (PtInRect(&rc, pt)) {
                        self.lastInsideTick_ = now;
                        // Hovering resets the stay window: after
                        // moving off, the popup lingers a fresh stay_s.
                        self.stayAnchorTick_ = now;
                    } else if (now - self.lastInsideTick_ > kCloseDelayMs &&
                               now - self.stayAnchorTick_ >=
                                   static_cast<ULONGLONG>(
                                       self.settings_.stayS) * 1000ull) {
                        self.Hide();
                    }
                }
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (PtInRect(&self.xRc_, pt)) {
                self.DismissSuppressed();
                return 0;
            }
            for (size_t i = 0; i < self.rows_.size(); ++i) {
                if (PtInRect(&self.rows_[i].xCard, pt)) {
                    self.CloseCard(static_cast<int>(i));
                    return 0;
                }
            }
            self.ActivateItem(self.ItemFromPoint(pt));
            return 0;
        }
        case WM_SETCURSOR: {
            POINT pt;
            if (LOWORD(lp) == HTCLIENT && GetCursorPos(&pt) &&
                ScreenToClient(hwnd, &pt)) {
                bool hand = PtInRect(&self.xRc_, pt);
                if (!hand) {
                    for (const auto& row : self.rows_) {
                        if (PtInRect(&row.xCard, pt)) {
                            hand = true;
                            break;
                        }
                    }
                }
                if (hand || self.ItemFromPoint(pt) >= 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace edgepop
