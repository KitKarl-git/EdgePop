// Taskbar monitor indicators - implementation. See taskbarbadges.h
// for the architecture. Watcher thread: UIA polling only. Overlay: main
// thread only; the watcher communicates via PostMessage'd snapshots.

#include "taskbarbadges.h"

#include <d3d11.h>
#include <d2d1_3.h>
#include <d2d1helper.h>
#include <dwmapi.h>      // DWMWA_CLOAKED
#include <dwrite.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <mutex>
#include <thread>

#include "fullscreen.h"
#include "osver.h"

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "dwmapi.lib")

namespace edgepop {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kOverlayClass[] = L"EdgePopTaskbarBadges";
constexpr UINT kMsgSnapshot = WM_APP + 0x40;
constexpr UINT kMsgHover = WM_APP + 0x41;
constexpr UINT kGraceTimerId = 0x7B4D;   // own window; far from house ids
constexpr UINT kGraceMs = 250;
// The chips float this far above the taskbar by default; the overlay
// reserves the band so chips can also rise ABOVE Windows' taskbar
// thumbnail previews, which are taller (and live in a higher z-band than
// any overlay - they cannot be drawn over, only cleared,).
constexpr int kHoverBandPx = 320;

void (*g_log)(const wchar_t*) = nullptr;

void TBLog(const wchar_t* fmt, ...) {
    if (!g_log) return;
    wchar_t buf[256];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, 256, _TRUNCATE, fmt, args);
    va_end(args);
    g_log(buf);
}

// --- data ---------------------------------------------------------------------

struct BadgeItem {
    RECT icon{};      // the icon sub-rect (falls back to the button rect)
    RECT button{};    // the whole taskbar item
    int dir = 0;      // MonitorDir as int
    int hops = 1;
    int number = 1;
};

struct Snapshot {
    bool trayFound = false;
    bool trayVisible = false;   // false = taskbar slid out of view
                                // (fullscreen app / auto-hide)
    RECT tray{};
    std::vector<BadgeItem> items;   // off-main windows only
};

HINSTANCE g_inst = nullptr;
HWND g_ovl = nullptr;
TaskbarBadgeConfig g_cfg;
bool g_hoverIn = false;
bool g_chipsShown = false;
// Watcher gate: while off, the loop skips the UIA walk - a user
// with "Activate arrows on taskbar" disabled must not pay explorer
// traffic every 300 ms. Atomic: written by SetConfig (main thread),
// read by the watcher.
std::atomic<bool> g_watchEnabled{ true };
Snapshot* g_snap = nullptr;
// The above-taskbar z-insert runs once per show, not per render.
bool g_zPinned = false;

std::mutex g_monMutex;
std::vector<MonitorRect> g_monitors;
std::vector<MonitorRelation> g_rel;   // per-monitor, indices align

std::thread g_watcher;
HANDLE g_stop = nullptr;

// --- watcher thread: UIA polling ----------------------------------------------

// Is the taskbar actually on screen? The overlay is topmost: if it
// outlived a hidden taskbar it would float over fullscreen content.
// Two signals, because the OS hides the taskbar two ways: a
// borderless-fullscreen FOREGROUND window (IsFullscreenOnMonitor - the
// same detector the trigger guard uses; the rect may still look
// on-screen in that state), and physically sliding the window out of
// view (auto-hide parks it off-screen leaving a sliver - caught by the
// intersection test).
bool TaskbarOnScreen(HWND tray, const RECT& rc) {
    if (!IsWindowVisible(tray)) return false;
    const HMONITOR hm = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT isect{};
    if (!hm || !GetMonitorInfoW(hm, &mi) ||
        !IntersectRect(&isect, &rc, &mi.rcMonitor)) {
        return false;
    }
    if (isect.bottom - isect.top < (rc.bottom - rc.top) / 2) {
        return false;   // slid out: at most a sliver peeks into the monitor
    }
    return !IsFullscreenOnMonitor(mi.rcMonitor);
}

void Collect(Snapshot* s, IUIAutomation* uia,
             IUIAutomationCondition* btnCond) {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray || !uia || !btnCond) return;
    if (!GetWindowRect(tray, &s->tray)) return;
    s->trayFound = true;
    s->trayVisible = TaskbarOnScreen(tray, s->tray);
    if (!s->trayVisible) return;   // also skips the UIA walk: hidden
                                    // taskbar has no buttons to read

    std::vector<MonitorRect> mons;
    std::vector<MonitorRelation> rel;
    {
        std::lock_guard<std::mutex> lock(g_monMutex);
        mons = g_monitors;
        rel = g_rel;
    }
    if (mons.empty()) return;   // monitors not pushed yet; try next tick

    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(tray, &root)) || !root) return;
    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(root->FindAll(TreeScope_Descendants, btnCond, &arr)) || !arr)
        return;
    int count = 0;
    arr->get_Length(&count);

    ComPtr<IUIAutomationTreeWalker> walker;
    uia->get_RawViewWalker(&walker);

    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> btn;
        if (FAILED(arr->GetElement(i, &btn)) || !btn) continue;

        // Running buttons carry "Window: 0x<hwnd>"; pinned-only carry
        // "Appid: ..." and get no indicator (verified by the UIA probe).
        BSTR aid = nullptr;
        if (FAILED(btn->get_CurrentAutomationId(&aid)) || !aid) continue;
        HWND target = nullptr;
        if (wcsncmp(aid, L"Window: 0x", 10) == 0) {
            target = reinterpret_cast<HWND>(static_cast<uintptr_t>(
                wcstoul(aid + 10, nullptr, 16)));
        }
        SysFreeString(aid);
        if (!target) continue;

        RECT rcWin{};
        if (IsIconic(target)) {
            // rcNormalPosition is work-area relative; only trust it for
            // minimized windows, where GetWindowRect is (-32000,-32000).
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            if (!GetWindowPlacement(target, &wp)) continue;
            rcWin = wp.rcNormalPosition;
        } else if (!GetWindowRect(target, &rcWin)) {
            continue;
        }
        const HMONITOR hm =
            MonitorFromRect(&rcWin, MONITOR_DEFAULTTONEAREST);
        size_t mi = static_cast<size_t>(-1);
        for (size_t k = 0; k < mons.size(); ++k) {
            if (mons[k].handle == hm) { mi = k; break; }
        }
        if (mi == static_cast<size_t>(-1) || mi >= rel.size() ||
            rel[mi].main) {
            continue;   // window on the main monitor: nothing to indicate
        }

        BadgeItem it{};
        if (FAILED(btn->get_CurrentBoundingRectangle(&it.button))) continue;
        it.icon = it.button;

        // The icon sub-rect anchors the indicator (labeled buttons are
        // wide); fall back to the whole button.
        if (walker) {
            ComPtr<IUIAutomationElement> ch;
            if (SUCCEEDED(walker->GetFirstChildElement(btn.Get(), &ch)) &&
                ch) {
                while (ch) {
                    BSTR cid = nullptr;
                    if (SUCCEEDED(ch->get_CurrentAutomationId(&cid)) && cid) {
                        RECT crc{};
                        if (wcscmp(cid, L"Icon") == 0 &&
                            SUCCEEDED(ch->get_CurrentBoundingRectangle(&crc)) &&
                            crc.right > crc.left && crc.bottom > crc.top) {
                            it.icon = crc;
                            SysFreeString(cid);
                            break;
                        }
                        SysFreeString(cid);
                    }
                    ComPtr<IUIAutomationElement> next;
                    if (FAILED(walker->GetNextSiblingElement(ch.Get(),
                                                             &next)) ||
                        !next) {
                        break;
                    }
                    ch = next;
                }
            }
        }

        it.dir = static_cast<int>(rel[mi].dir);
        it.hops = rel[mi].hops;
        it.number = rel[mi].number;
        s->items.push_back(it);
    }
}

void WatcherLoop() {
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IUIAutomation> uia;
    if (SUCCEEDED(hrCo)) {
        const HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation8),
                                            nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&uia));
        if (FAILED(hr)) TBLog(L"badges: CUIAutomation8 failed hr=0x%08X", hr);
    }
    ComPtr<IUIAutomationCondition> btnCond;
    if (uia) {
        VARIANT v{};
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"Taskbar.TaskListButtonAutomationPeer");
        const HRESULT hr =
            uia->CreatePropertyCondition(UIA_ClassNamePropertyId, v,
                                         &btnCond);
        VariantClear(&v);
        if (FAILED(hr)) TBLog(L"badges: condition failed hr=0x%08X", hr);
    }

    while (WaitForSingleObject(g_stop, 300) == WAIT_TIMEOUT) {
        if (!g_watchEnabled.load(std::memory_order_relaxed)) {
            continue;   // feature off - no UIA traffic
        }
        Snapshot* s = new Snapshot;
        Collect(s, uia.Get(), btnCond.Get());
        if (!g_ovl ||
            !PostMessageW(g_ovl, kMsgSnapshot, 0,
                          reinterpret_cast<LPARAM>(s))) {
            delete s;
        }
    }
    if (SUCCEEDED(hrCo)) CoUninitialize();
}

// --- overlay rendering (main thread) -------------------------------------------
//
// D2D (WARP) -> CPU-readable bitmap -> premultiplied DIB -> UpdateLayered-
// Window. WARP on purpose: the surface is tiny (taskbar + hover band) and
// CPU_READ maps are simplest on a software device. The DC-render-target
// shortcut is unusable here - it leaves alpha at zero and chips
// are dark-on-light, which that trick cannot composite.

ComPtr<ID2D1Factory1> g_d2df;
ComPtr<ID2D1Device> g_d2dDev;
ComPtr<ID2D1DeviceContext> g_dc;
ComPtr<IDWriteFactory> g_dw;
ComPtr<IDWriteTextFormat> g_numFmt;   // cached at g_numFmtPx (see
                                       // NumFormat - size follows the
                                       // slider)
float g_numFmtPx = 0.0f;
ComPtr<ID2D1SolidColorBrush> g_brush;
ComPtr<ID2D1PathGeometry> g_triGeo;     // filled triangle (unit, points E)
ComPtr<ID2D1PathGeometry> g_headGeo;    // "arrow" shape's head
ComPtr<ID2D1StrokeStyle> g_capsStroke;  // round caps: chevron + arrow shaft
bool g_stackTried = false;
bool g_stackOk = false;

HDC g_memDc = nullptr;
HBITMAP g_dib = nullptr;
HBITMAP g_dibOld = nullptr;
void* g_dibBits = nullptr;
SIZE g_dibSize{ 0, 0 };
ComPtr<ID2D1Bitmap1> g_rtBmp;    // TARGET only
ComPtr<ID2D1Bitmap1> g_stage;    // CPU_READ copy (Map source)

bool EnsureStack() {
    if (g_stackTried) return g_stackOk;
    g_stackTried = true;
    do {
        ComPtr<ID3D11Device> d3d;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d, nullptr, nullptr);
        if (FAILED(hr)) {
            TBLog(L"badges: WARP device failed hr=0x%08X", hr);
            break;
        }
        ComPtr<IDXGIDevice> dxgi;
        d3d.As(&dxgi);
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               __uuidof(ID2D1Factory1), nullptr,
                               reinterpret_cast<void**>(
                                   g_d2df.GetAddressOf()));
        if (FAILED(hr)) break;
        if (FAILED(g_d2df->CreateDevice(dxgi.Get(), &g_d2dDev))) break;
        if (FAILED(g_d2dDev->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_dc))) {
            break;
        }
        IUnknown* dwUnk = nullptr;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                       __uuidof(IDWriteFactory), &dwUnk))) {
            break;
        }
        g_dw.Attach(reinterpret_cast<IDWriteFactory*>(dwUnk));
        if (FAILED(g_dc->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::White), &g_brush))) {
            break;
        }

        // Unit shapes point E; rotated/scaled per indicator : three
        // glyph styles share the same placement machinery).
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(g_d2df->CreatePathGeometry(&g_triGeo))) break;
        if (FAILED(g_triGeo->Open(&sink))) break;
        sink->BeginFigure(D2D1::Point2F(0.5f, 0.0f),
                          D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(-0.35f, -0.55f));
        sink->AddLine(D2D1::Point2F(-0.35f, 0.55f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();

        ComPtr<ID2D1GeometrySink> sink2;
        if (FAILED(g_d2df->CreatePathGeometry(&g_headGeo))) break;
        if (FAILED(g_headGeo->Open(&sink2))) break;
        sink2->BeginFigure(D2D1::Point2F(0.5f, 0.0f),
                           D2D1_FIGURE_BEGIN_FILLED);
        sink2->AddLine(D2D1::Point2F(0.08f, -0.38f));
        sink2->AddLine(D2D1::Point2F(0.08f, 0.38f));
        sink2->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink2->Close();

        if (FAILED(g_d2df->CreateStrokeStyle(
                D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                    D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                    D2D1_DASH_STYLE_SOLID, 0.0f),
                nullptr, 0, &g_capsStroke))) {
            break;
        }

        g_stackOk = true;
        TBLog(L"badges: render stack ok");
    } while (false);
    if (!g_stackOk) TBLog(L"badges: render stack FAILED");
    return g_stackOk;
}

// Em size for the indicator digits: they must grow with the Size
// slider like the glyphs do. 1.31x the glyph size reproduces the
// shipped default look exactly (8 px glyphs, 10.5 px digits); the
// floor keeps smaller settings from shrinking below the old fixed
// 10.5 px.
float NumFontPx(float gsz) {
    return std::max(10.5f, gsz * 1.31f);
}

// The persistent and hover sets are strictly exclusive per render, so
// one cached format is enough - it is only recreated when the size
// setting changes.
IDWriteTextFormat* NumFormat(float px) {
    if (g_numFmt && g_numFmtPx == px) return g_numFmt.Get();
    g_numFmt.Reset();
    g_numFmtPx = 0.0f;
    if (SUCCEEDED(g_dw->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px,
            L"", &g_numFmt)) &&
        g_numFmt) {
        g_numFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_numFmt->SetParagraphAlignment(
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_numFmtPx = px;
    }
    return g_numFmt.Get();
}

// Width reserved for the number. The fixed allowance reproduces the
// shipped widths; the digits term (advance ~0.6 em each) takes over
// once the scaled font outgrows it or the number reaches two digits.
float NumBoxWidth(int number, float numPx, float allowance) {
    wchar_t buf[4]{};
    _itow_s(number, buf, 10);
    return std::max(allowance,
                    static_cast<float>(lstrlenW(buf)) * numPx * 0.68f);
}

void FreeSurface() {
    if (g_memDc) {
        if (g_dibOld) SelectObject(g_memDc, g_dibOld);
        DeleteDC(g_memDc);
        g_memDc = nullptr;
        g_dibOld = nullptr;
    }
    if (g_dib) {
        DeleteObject(g_dib);
        g_dib = nullptr;
    }
    g_dibBits = nullptr;
    g_rtBmp.Reset();
    g_stage.Reset();
    g_dibSize = { 0, 0 };
}

bool EnsureSurface(int w, int h) {
    if (g_dibSize.cx == w && g_dibSize.cy == h && g_rtBmp) return true;
    FreeSurface();

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &g_dibBits,
                             nullptr, 0);
    if (!g_dib || !g_dibBits) return false;
    g_memDc = CreateCompatibleDC(nullptr);
    if (!g_memDc) return false;
    g_dibOld = static_cast<HBITMAP>(SelectObject(g_memDc, g_dib));

    // CPU_READ cannot be combined with TARGET on a D2D bitmap (verified
    // live: CreateBitmap failed every tick). The documented shape is two
    // bitmaps: a plain target, then a staging copy that can be Mapped.
    D2D1_BITMAP_PROPERTIES1 props{};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
    if (FAILED(g_dc->CreateBitmap(D2D1::SizeU(
            static_cast<UINT32>(w), static_cast<UINT32>(h)),
            nullptr, 0, &props, &g_rtBmp))) {
        FreeSurface();
        TBLog(L"badges: target bitmap failed");
        return false;
    }
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ |
                          D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    if (FAILED(g_dc->CreateBitmap(D2D1::SizeU(
            static_cast<UINT32>(w), static_cast<UINT32>(h)),
            nullptr, 0, &props, &g_stage))) {
        FreeSurface();
        TBLog(L"badges: staging bitmap failed");
        return false;
    }
    g_dibSize = { w, h };
    return true;
}

// dir: MonitorDir as int (E..SE clockwise on the compass). Screen angles:
// y grows downward, so up-right (NE) is -45 degrees of clockwise rotation.

D2D1_COLOR_F ColorOf(COLORREF c, float a) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                        GetBValue(c) / 255.0f, a);
}

// One unit-space glyph at (cx, cy) in the requested style. `rot` is
// the direction rotation; M = Scale * Rot * Translate (row-vector order).
void DrawGlyphPass(ID2D1DeviceContext* dc, float cx, float cy, float size,
                   ArrowShape shape, D2D1_COLOR_F color,
                   const D2D1_MATRIX_3X2_F& rot) {
    const D2D1_MATRIX_3X2_F m =
        D2D1::Matrix3x2F::Scale(size, size) * rot *
        D2D1::Matrix3x2F::Translation(cx, cy);
    dc->SetTransform(m);
    g_brush->SetColor(color);
    switch (shape) {
        case ArrowShape::Triangle:
            dc->FillGeometry(g_triGeo.Get(), g_brush.Get());
            break;
        case ArrowShape::Chevron:
            dc->DrawLine(D2D1::Point2F(-0.32f, -0.52f),
                         D2D1::Point2F(0.22f, 0.0f), g_brush.Get(), 0.30f,
                         g_capsStroke.Get());
            dc->DrawLine(D2D1::Point2F(0.22f, 0.0f),
                         D2D1::Point2F(-0.32f, 0.52f), g_brush.Get(), 0.30f,
                         g_capsStroke.Get());
            break;
        case ArrowShape::Arrow:
            dc->DrawLine(D2D1::Point2F(-0.48f, 0.0f),
                         D2D1::Point2F(0.14f, 0.0f), g_brush.Get(), 0.16f,
                         g_capsStroke.Get());
            dc->FillGeometry(g_headGeo.Get(), g_brush.Get());
            break;
    }
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

// The full indicator: `hops` glyphs spread along the direction, optionally
// with a drop shadow so a light glyph survives a light background.
void DrawGlyph(ID2D1DeviceContext* dc, float cx, float cy, int dir, int hops,
               float size, ArrowShape shape, D2D1_COLOR_F color,
               bool shadow) {
    const float ang = -static_cast<float>(dir) * 45.0f;
    const float rad = ang * 3.14159265f / 180.0f;
    const float ux = std::cos(rad);
    const float uy = std::sin(rad);
    const int n = hops < 1 ? 1 : (hops > 3 ? 3 : hops);
    const float spacing = size * 0.95f;
    const float total = static_cast<float>(n - 1) * spacing;
    const D2D1_MATRIX_3X2_F rot = D2D1::Matrix3x2F::Rotation(ang);
    for (int i = 0; i < n; ++i) {
        const float off = static_cast<float>(i) * spacing - total / 2;
        const D2D1_POINT_2F base{ cx + off * ux, cy + off * uy };
        if (shadow) {
            const float so = std::max(1.0f, size * 0.16f);
            DrawGlyphPass(dc, base.x + so, base.y + so, size, shape,
                          D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), rot);
        }
        DrawGlyphPass(dc, base.x, base.y, size, shape, color, rot);
    }
}

// Capsule content width: number box + gap + glyph cluster.
float CapsuleWidth(float ch, bool showNum, int number, bool showGlyphs,
                   int hops, float gsz) {
    const int n = hops < 1 ? 1 : (hops > 3 ? 3 : hops);
    const float glyphW =
        (static_cast<float>(n) - 1.0f) * gsz * 0.95f + gsz * 1.15f;
    const float numW =
        showNum ? NumBoxWidth(number, NumFontPx(gsz), ch * 0.62f) : 0.0f;
    const float gap = (showNum && showGlyphs) ? 2.0f : 0.0f;
    const float contentW = numW + gap + (showGlyphs ? glyphW : 0.0f);
    return std::max(ch + 8.0f, contentW + 12.0f);
}

// One indicator capsule at (leftX, top): dark rounded capsule, optional
// shadow silhouette, number (white) and/or direction glyphs (config
// color). Shared by the persistent and the hover indicators.
void DrawCapsule(ID2D1DeviceContext* dc, float leftX, float top, float ch,
                 bool showNum, int number, bool showGlyphs, int dir,
                 int hops, float gsz, ArrowShape shape,
                 D2D1_COLOR_F glyphCol, bool shadow) {
    const float cw =
        CapsuleWidth(ch, showNum, number, showGlyphs, hops, gsz);
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
        D2D1::RectF(leftX, top, leftX + cw, top + ch), ch / 2, ch / 2);
    if (shadow) {
        const float so = std::max(1.0f, ch * 0.10f);
        D2D1_ROUNDED_RECT sh = rr;
        sh.rect.left += so;  sh.rect.top += so;
        sh.rect.right += so; sh.rect.bottom += so;
        g_brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f));
        dc->FillRoundedRectangle(sh, g_brush.Get());
    }
    g_brush->SetColor(D2D1::ColorF(0.07f, 0.07f, 0.08f, 0.82f));
    dc->FillRoundedRectangle(rr, g_brush.Get());
    g_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.5f));
    dc->DrawRoundedRectangle(rr, g_brush.Get(), 1.0f);

    const int n = hops < 1 ? 1 : (hops > 3 ? 3 : hops);
    const float glyphW =
        (static_cast<float>(n) - 1.0f) * gsz * 0.95f + gsz * 1.15f;
    const float numW = showNum
        ? NumBoxWidth(number, NumFontPx(gsz), ch * 0.62f) : 0.0f;
    const float gap = (showNum && showGlyphs) ? 2.0f : 0.0f;
    const float contentW = numW + gap + (showGlyphs ? glyphW : 0.0f);
    float contentX = leftX + (cw - contentW) / 2;
    if (showNum) {
        if (IDWriteTextFormat* fmt = NumFormat(NumFontPx(gsz))) {
            wchar_t num[4]{};
            _itow_s(number, num, 10);
            g_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
            dc->DrawTextW(num, lstrlenW(num), fmt,
                            D2D1::RectF(contentX, top, contentX + numW,
                                        top + ch),
                            g_brush.Get());
            contentX += numW + gap;
        }
    }
    if (showGlyphs) {
        DrawGlyph(dc, contentX + glyphW / 2, top + ch / 2, dir, hops, gsz,
                  shape, glyphCol, false);
    }
}

// While the cursor is on the taskbar, Windows shows its thumbnail-preview
// flyouts above it - higher-band immersive windows that cover our
// indicators (and cannot be drawn over). Detection is GEOMETRIC, not by
// class: two probe rounds showed the flyouts are hosted differently than
// assumed (Xaml_WindowedPopupClass instances float with their bottom edge
// anywhere in ~±45 px around the taskbar top; other builds host them in
// XamlExplorerHostIslandWindow). So: any non-cloaked explorer window
// whose bottom hangs near the taskbar top and whose footprint is
// flyout-sized lifts the indicators.
struct LiftCtx {
    HWND tray = nullptr;
    DWORD trayPid = 0;
    int trayTop = 0;     // the taskbar's top edge
    int zoneTop = 0;     // don't consider anything higher than this
    int minTop = 0;      // result; 0 = nothing to lift above
};

BOOL CALLBACK LiftEnumProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<LiftCtx*>(lp);
    if (h == ctx->tray) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == 0 || pid != ctx->trayPid) return TRUE;
    int cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked,
                                     sizeof(cloaked))) ||
        cloaked != 0) {
        return TRUE;   // hidden flyouts cloak themselves between shows
    }
    RECT r{};
    if (!GetWindowRect(h, &r)) return TRUE;
    // Flyout-shaped: bottom floating around the taskbar's top edge, and
    // small enough to exclude real windows (a maximized window shares the
    // bottom line but is full-screen tall).
    if (r.bottom < ctx->trayTop - 80 || r.bottom > ctx->trayTop + 20) {
        return TRUE;
    }
    if (r.bottom - r.top > 400) return TRUE;    // taller than any flyout
    if (r.right - r.left > 1400) return TRUE;   // wider than any flyout
    if (r.top < ctx->zoneTop) return TRUE;
    if (ctx->minTop == 0 || r.top < ctx->minTop) ctx->minTop = r.top;
    return TRUE;
}

// Bare (no-capsule) content width - matches DrawBare's layout.
float BareWidth(bool showNum, int number, bool showGlyphs, int hops,
                float gsz) {
    const int n = hops < 1 ? 1 : (hops > 3 ? 3 : hops);
    const float glyphW =
        (static_cast<float>(n) - 1.0f) * gsz * 0.95f + gsz * 1.15f;
    const float numW =
        showNum ? NumBoxWidth(number, NumFontPx(gsz), gsz + 4.0f) : 0.0f;
    const float gap = (showNum && showGlyphs) ? 3.0f : 0.0f;
    return numW + gap + (showGlyphs ? glyphW : 0.0f);
}

// One bare (no-capsule) indicator: number and/or glyphs drawn directly
// with the shared drop shadow.
void DrawBare(ID2D1DeviceContext* dc, float leftX, float cy, bool showNum,
              int number, bool showGlyphs, int dir, int hops, float gsz,
              ArrowShape shape, D2D1_COLOR_F col, bool shadow) {
    const int n = hops < 1 ? 1 : (hops > 3 ? 3 : hops);
    const float glyphW =
        (static_cast<float>(n) - 1.0f) * gsz * 0.95f + gsz * 1.15f;
    const float numPx = NumFontPx(gsz);
    const float numW = showNum ? NumBoxWidth(number, numPx, gsz + 4.0f)
                               : 0.0f;
    const float gap = (showNum && showGlyphs) ? 3.0f : 0.0f;
    float cur = leftX;
    const float so = std::max(1.0f, gsz * 0.16f);
    if (showNum) {
        if (IDWriteTextFormat* fmt = NumFormat(numPx)) {
            wchar_t num[4]{};
            _itow_s(number, num, 10);
            const float halfH = std::max(gsz, numPx * 0.7f);
            D2D1_RECT_F box =
                D2D1::RectF(cur, cy - halfH, cur + numW, cy + halfH);
            if (shadow) {
                D2D1_RECT_F sh = box;
                sh.left += so; sh.top += so; sh.right += so;
                sh.bottom += so;
                g_brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f));
                dc->DrawTextW(num, lstrlenW(num), fmt, sh, g_brush.Get());
            }
            g_brush->SetColor(col);
            dc->DrawTextW(num, lstrlenW(num), fmt, box, g_brush.Get());
            cur += numW + gap;
        }
    }
    if (showGlyphs) {
        DrawGlyph(dc, cur + glyphW / 2, cy, dir, hops, gsz, shape, col,
                  shadow);
    }
}

// Left edge x (overlay coords) for an indicator of width cw anchored per
// the Position setting on the taskbar ITEM (whole button). Shared by the
// persistent and the hover sets.
float AnchorX(const BadgeItem& it, int x, float cw) {
    switch (g_cfg.pos) {
        case ArrowPos::Right:
            return static_cast<float>(it.button.right - x) - 2.0f - cw;
        case ArrowPos::Center:
            return static_cast<float>(it.button.left + it.button.right) /
                       2.0f -
                   static_cast<float>(x) - cw / 2;
        case ArrowPos::Left:
        default:
            return static_cast<float>(it.button.left - x) + 2.0f;
    }
}

void Render() {
    if (!g_ovl) return;
    // Strictly mutually exclusive: while the cursor is anywhere
    // over the taskbar - regardless of what the hover section is set to
    // show - the persistent indicators are hidden and the hover set (if
    // it has any content) is shown instead. The chips ride the GRACE
    // state, not the raw hover flag: g_chipsShown stays true through the
    // 250 ms leave-grace so they don't blink off on a momentary slip off
    // the taskbar (the grace timer always intended this; the old
    // g_hoverIn read made it dead code).
    const bool wantChips =
        g_chipsShown && (g_cfg.hoverNumbers || g_cfg.hoverArrows);
    const bool wantPersistent =
        !g_chipsShown && (g_cfg.persistentNumbers || g_cfg.persistentArrows);
    if (!g_snap || !g_snap->trayFound || !g_snap->trayVisible ||
        g_snap->items.empty() ||
        !g_cfg.enabled || (!wantPersistent && !wantChips)) {
        if (IsWindowVisible(g_ovl)) ShowWindow(g_ovl, SW_HIDE);
        g_zPinned = false;   // re-pin z on the next show
        return;
    }

    const RECT& tr = g_snap->tray;
    const int x = tr.left;
    const int y = tr.top - kHoverBandPx;
    const int w = tr.right - tr.left;
    const int h = tr.bottom - y;
    if (w <= 0 || h <= 0) return;
    if (!EnsureStack() || !EnsureSurface(w, h)) return;

    // Size/place, then sit directly above the taskbar in z-order. The
    // taskbar lives in the same z-band as normal windows on this build
    // (probe: Shell_TrayWnd band == 1), so hWndInsertAfter is honored.
    RECT cur{};
    GetWindowRect(g_ovl, &cur);
    if (cur.left != x || cur.top != y ||
        cur.right - cur.left != w || cur.bottom - cur.top != h) {
        SetWindowPos(g_ovl, nullptr, x, y, w, h,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    }
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray && !g_zPinned) {
        // Sit directly ABOVE the taskbar: insert after whatever is
        // directly above it. (…, tray, …) places a window BELOW tray -
        // the old form re-ordered on every render and never stabilized.
        // Once per show is enough: the overlay is topmost and nothing
        // composites over the taskbar anyway.
        HWND above = GetWindow(tray, GW_HWNDPREV);
        SetWindowPos(g_ovl, above ? above : HWND_TOP, 0, 0, 0, 0,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        g_zPinned = true;
    }

    g_dc->SetTarget(g_rtBmp.Get());
    g_dc->BeginDraw();
    g_dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const float oy = static_cast<float>(y);   // screen -> overlay offset

    // The taskbar composites above same-band layered windows on this build
    // (verified live despite Shell_TrayWnd reporting band 1).
    // Everything we
    // draw therefore hugs the taskbar's TOP edge from the work-area side.
    // Two sections with independent controls. Persistent rests on
    // the edge, anchored at the taskbar ITEM's left/center/right, capsule
    // or bare; hover floats higher (rising above thumbnail flyouts) and
    // replaces the persistent set while the cursor is on the taskbar.
    if (wantPersistent) {
        const float gsz = static_cast<float>(g_cfg.persistentSize);
        const float ch = std::max(16.0f, gsz + 8.0f);
        const D2D1_COLOR_F col = ColorOf(g_cfg.color, 0.95f);
        for (const auto& it : g_snap->items) {
            const float cw = g_cfg.persistentBg
                ? CapsuleWidth(ch, g_cfg.persistentNumbers, it.number,
                               g_cfg.persistentArrows, it.hops, gsz)
                : BareWidth(g_cfg.persistentNumbers, it.number,
                            g_cfg.persistentArrows, it.hops, gsz);
            const float leftX = AnchorX(it, x, cw);
            if (g_cfg.persistentBg) {
                DrawCapsule(g_dc.Get(), leftX,
                            static_cast<float>(tr.top) - 1.0f - ch - oy, ch,
                            g_cfg.persistentNumbers, it.number,
                            g_cfg.persistentArrows, it.dir, it.hops, gsz,
                            g_cfg.shape, ColorOf(g_cfg.color, 1.0f),
                            g_cfg.shadow);
            } else {
                const float cy =
                    static_cast<float>(tr.top) - gsz / 2 - 1.0f - oy;
                DrawBare(g_dc.Get(), leftX, cy, g_cfg.persistentNumbers,
                         it.number, g_cfg.persistentArrows, it.dir,
                         it.hops, gsz, g_cfg.shape, col, g_cfg.shadow);
            }
        }
    }

    if (wantChips) {
        // Default: 4 px above the taskbar edge. When Windows shows a
        // flyout over the taskbar (thumbnail previews), the indicators
        // rise above its top edge instead - flyouts are
        // higher-band windows and would otherwise cover them.
        int chipBottom = tr.top - 4;
        {
            HWND trayWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
            if (trayWnd) {
                DWORD trayPid = 0;
                GetWindowThreadProcessId(trayWnd, &trayPid);
                LiftCtx ctx{};
                ctx.tray = trayWnd;
                ctx.trayPid = trayPid;
                ctx.trayTop = tr.top;
                ctx.zoneTop = tr.top - kHoverBandPx + 40;
                EnumWindows(LiftEnumProc, reinterpret_cast<LPARAM>(&ctx));
                if (ctx.minTop > 0) {
                    const int lifted = ctx.minTop - 6;
                    if (lifted < chipBottom) chipBottom = lifted;
                }
            }
        }

        for (const auto& it : g_snap->items) {
            // The hover set anchors on the taskbar ITEM like the
            // persistent set - same Position setting.
            const float gsz2 = static_cast<float>(g_cfg.hoverSize);
            const float chH = std::max(16.0f, gsz2 + 8.0f);
            const float cw = g_cfg.hoverBg
                ? CapsuleWidth(chH, g_cfg.hoverNumbers, it.number,
                               g_cfg.hoverArrows, it.hops, gsz2)
                : BareWidth(g_cfg.hoverNumbers, it.number,
                            g_cfg.hoverArrows, it.hops, gsz2);
            const float leftX = AnchorX(it, x, cw);
            if (g_cfg.hoverBg) {
                DrawCapsule(g_dc.Get(), leftX,
                            static_cast<float>(chipBottom) - chH - oy, chH,
                            g_cfg.hoverNumbers, it.number,
                            g_cfg.hoverArrows, it.dir, it.hops, gsz2,
                            g_cfg.shape, ColorOf(g_cfg.color, 1.0f),
                            false);
            } else {
                const float cy =
                    static_cast<float>(chipBottom) - gsz2 / 2 - oy;
                DrawBare(g_dc.Get(), leftX, cy, g_cfg.hoverNumbers,
                         it.number, g_cfg.hoverArrows, it.dir, it.hops,
                         gsz2, g_cfg.shape, ColorOf(g_cfg.color, 0.95f),
                         g_cfg.shadow);
            }
        }
    }

    const HRESULT hr = g_dc->EndDraw();
    if (FAILED(hr)) {
        TBLog(L"badges: EndDraw hr=0x%08X", hr);
        return;
    }
    g_dc->Flush();
    if (FAILED(g_stage->CopyFromBitmap(nullptr, g_rtBmp.Get(), nullptr))) {
        TBLog(L"badges: staging copy failed");
        return;
    }
    D2D1_MAPPED_RECT mr{};
    if (SUCCEEDED(g_stage->Map(D2D1_MAP_OPTIONS_READ, &mr))) {
        // Count non-transparent pixels once per item-count change: this is
        // the draw-vs-composite diagnostic.
        static int lastLogged = -1;
        const bool logThis =
            lastLogged != static_cast<int>(g_snap->items.size());
        if (logThis) lastLogged = static_cast<int>(g_snap->items.size());
        long painted = 0;
        if (logThis) {
            for (int row = 0; row < h; ++row) {
                const BYTE* px =
                    mr.bits + static_cast<UINT32>(row) * mr.pitch;
                for (int cx = 0; cx < w * 4; cx += 4) {
                    if (px[cx + 3] != 0) ++painted;
                }
            }
            TBLog(L"badges: %d item(s), first icon=(%ld,%ld), %ld px painted",
                  static_cast<int>(g_snap->items.size()),
                  g_snap->items.empty() ? 0 : g_snap->items[0].icon.left,
                  g_snap->items.empty() ? 0 : g_snap->items[0].icon.top,
                  painted);
        }
        const UINT32 stride = static_cast<UINT32>(w) * 4;
        for (int row = 0; row < h; ++row) {
            memcpy(static_cast<BYTE*>(g_dibBits) + row * stride,
                   mr.bits + static_cast<UINT32>(row) * mr.pitch, stride);
        }
        g_stage->Unmap();
        POINT dst{ x, y };
        POINT src{ 0, 0 };
        SIZE sz{ w, h };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        if (!IsWindowVisible(g_ovl)) ShowWindow(g_ovl, SW_SHOWNOACTIVATE);
        if (!UpdateLayeredWindow(g_ovl, nullptr, &dst, &sz, g_memDc, &src,
                                 0, &bf, ULW_ALPHA)) {
            TBLog(L"badges: ULW failed gle=%lu", GetLastError());
        }
    }
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kMsgSnapshot:
            delete g_snap;
            g_snap = reinterpret_cast<Snapshot*>(lp);
            Render();
            return 0;
        case kMsgHover:
            g_hoverIn = wp != 0;
            if (g_hoverIn) {
                KillTimer(hwnd, kGraceTimerId);
                g_chipsShown = true;
            } else {
                SetTimer(hwnd, kGraceTimerId, kGraceMs, nullptr);
            }
            Render();
            return 0;
        case WM_TIMER:
            if (wp == kGraceTimerId) {
                KillTimer(hwnd, kGraceTimerId);
                g_chipsShown = false;
                Render();
            }
            return 0;
        case WM_DESTROY:
            delete g_snap;
            g_snap = nullptr;
            if (g_ovl == hwnd) g_ovl = nullptr;
            g_zPinned = false;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

// --- public API (main thread) ---------------------------------------------------

TaskbarBadges& TaskbarBadges::Instance() {
    static TaskbarBadges inst;
    return inst;
}

bool TaskbarBadges::Start(HINSTANCE inst) {
   // the whole pipeline is built on the Win11 taskbar (Xaml
    // buttons exposing "Window: 0x<hwnd>" AutomationIds,; the
    // classic Win10 taskbar has neither those ids nor the same z-band
    // behavior). On older builds start nothing - no overlay, no UIA
    // polling thread. Every other entry point already no-ops while g_ovl
    // is null, and the edge popup keeps working. 0 = version query
    // failed; run anyway.
    if (const int build = edgepop::OsBuildNumber();
        build != 0 && build < 22000) {
        TBLog(L"taskbar indicators: need the Windows 11 taskbar; "
              L"disabled on this Windows build (%d)", build);
        return true;   // not a failure - the popup side is unaffected
    }
    if (g_ovl) return true;
    g_inst = inst;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.lpfnWndProc = &OverlayProc;
    wc.lpszClassName = kOverlayClass;
    RegisterClassExW(&wc);

    // WS_EX_TRANSPARENT is wanted here : the
    // taskbar must keep every click; the overlay only paints.
    g_ovl = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kOverlayClass, nullptr, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
        inst, nullptr);
    if (!g_ovl) return false;

    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop) {
        DestroyWindow(g_ovl);
        g_ovl = nullptr;
        return false;
    }
    g_watcher = std::thread(WatcherLoop);
    TBLog(L"badges: started");
    return true;
}

void TaskbarBadges::Stop() {
    if (g_stop) {
        SetEvent(g_stop);
        if (g_watcher.joinable()) g_watcher.join();
        CloseHandle(g_stop);
        g_stop = nullptr;
    }
    if (g_ovl) {
        // A kMsgSnapshot may sit in the queue unprocessed; DestroyWindow
        // discards pending messages and would leak its heap Snapshot
        // (the watcher is joined above, so no new posts can race in).
        MSG msg{};
        while (PeekMessageW(&msg, g_ovl, kMsgSnapshot, kMsgSnapshot,
                            PM_REMOVE)) {
            delete reinterpret_cast<Snapshot*>(msg.lParam);
        }
        HWND h = g_ovl;
        g_ovl = nullptr;
        DestroyWindow(h);
    }
    FreeSurface();
}

void TaskbarBadges::SetConfig(const TaskbarBadgeConfig& cfg) {
    g_cfg = cfg;
    g_watchEnabled.store(cfg.enabled, std::memory_order_relaxed);
    Render();
}

void TaskbarBadges::UpdateMonitors(
    const std::vector<MonitorRect>& monitors) {
    std::lock_guard<std::mutex> lock(g_monMutex);
    g_monitors = monitors;
    g_rel = RelateMonitorsToMain(monitors);
}

void TaskbarBadges::SetTaskbarHover(bool in) {
    if (g_ovl) PostMessageW(g_ovl, kMsgHover, in ? 1 : 0, 0);
}

bool TaskbarBadges::TaskbarRect(RECT* rc) const {
    if (!g_snap || !g_snap->trayFound || !rc) return false;
    *rc = g_snap->tray;
    return true;
}

void TaskbarBadges::SetLogSink(void (*log)(const wchar_t*)) {
    g_log = log;
}

} // namespace edgepop
