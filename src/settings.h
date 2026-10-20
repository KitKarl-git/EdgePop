// EdgePop settings: one struct, loaded from and written back to EdgePop.ini
// next to the exe (docs/settings-spec.md is the contract). The tray menu
// mutates this struct and calls SaveSettings - the INI is the single source
// of truth; the HKCU Run key for autostart is derived state, never read back.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace edgepop {

enum class PopupSide { Main, Neighbor };
enum class ChainMode { Neighbor, Direction };
enum class VirtualDesktops { Current, All };
enum class Activation { EdgeDwell, CrossBack };   //
enum class ArrowShape { Triangle, Chevron, Arrow };   //
enum class ArrowPos { Left, Center, Right };           //

struct Settings {
    // [trigger]
    bool     activationEnabled = true;  // master switch: when false, stopping
                                        // at / crossing edges never fires the
                                        // popup; the LL hook stays up
                                        // (popup dismissal + taskbar hover)
    Activation activation = Activation::EdgeDwell;  // stop-at-edge | cross&return
    unsigned crossBackMs = 600;    // cross-back: max time away before return
    unsigned dwellMs = 250;         // 0 = instant (implemented as 1 ms hover)
    bool     edgesAuto = true;      // auto = every edge with a neighbor
    DWORD    edgeMask = 0;          // manual edge selection, bits below
    bool     fullscreenGuard = false;  // off by default since v1.0;
                                       // enable to also suppress over
                                       // borderless-fullscreen games/video
    unsigned zonePx = 28;           // seam zone half-width
    unsigned stopRadiusPx = 16;     // movement below this counts as "stopped"
    unsigned reactivateDelayS = 3;  // trigger pause after popup dismissed
                                    // via X/Esc

    // [popup] - consumed by the popup in phase 3+
    PopupSide side = PopupSide::Main;
    ChainMode chainMode = ChainMode::Neighbor;
    bool showIcons = true;
    bool showTitles = true;
    bool thumbnails = true;         // live DWM previews; only one of
                                    // icons/titles/thumbnails must be on
                                    //. Minimized windows
                                    // keep their card with the last-known
                                    // snapshot.
    bool peek = true;                // hovering an entry hides the other
                                     // windows on its monitor while hovered
                                     // (taskbar-preview behavior,
                                     // note 4)
    bool cardCloseButtons = true;    // small round close on each preview
                                     // card - WM_CLOSEs that window; the
                                     // popup's own square tab is separate
                                     // and always there
    unsigned leaveGracePx = 8;
    unsigned stayS = 2;              // min seconds the popup stays up even
                                     // with the cursor far away; the window
                                     // resets whenever the popup is hovered
                                     //
    unsigned maxWidth = 420;        // icon+title list width cap in px; the
                                    // list popup is as narrow as content
                                    // allows (min 120), titles ellipsize at
                                    // the edge
    unsigned cardWidth = 300;       // thumbnail popup width in px; the
                                    // default is 300 (420/480
                                    // read too wide), hard range 230-400

    // [windows]
    VirtualDesktops virtualDesktops = VirtualDesktops::Current;
    std::wstring excludeApps;   // comma-separated exe names never listed
                               // empty = list everything

    // [taskbar] - screen indicators at the taskbar edge
    bool taskbarArrows = true;         // master switch: gates BOTH the
                                       // persistent and the hover
                                       // indicators
    // Persistent section (always on, resting on the taskbar edge under
    // every running item; yields to the hover set while hovering).
    bool persistentNumbers = false;    // show the monitor number
    bool persistentArrows = true;      // show direction glyphs
    unsigned persistentSize = 6;       // glyph px, hard range 5-16
    bool persistentBg = false;         // capsule background; off = bare
                                       // glyphs with shadow (subtle notch)
    // Hover section (while the cursor is over the taskbar; rises above
    // Windows' thumbnail previews when they appear).
    bool hoverNumbers = true;
    bool hoverArrows = true;
    unsigned hoverSize = 8;            // glyph px, hard range 5-16
    bool hoverBg = true;               // capsule background
    // Shared look.
    ArrowPos arrowPos = ArrowPos::Left;
                                       // anchor at the taskbar ITEM's
                                       // left edge, center, or right edge
    ArrowShape arrowShape = ArrowShape::Chevron;   //
    COLORREF arrowColor = RGB(255, 255, 255);
    bool arrowShadow = true;           // dark drop shadow - a white arrow
                                       // survives a white background

    // [general]
    bool startWithWindows = true;  // on by default; the installer
                                    // sets it too when one exists
    bool debugStrips = false;
};

Settings LoadSettings(const wchar_t* iniPath);
void SaveSettings(const wchar_t* iniPath, const Settings& s);

// "steam.exe, Discord.exe" -> {L"steam.exe", L"discord.exe"} (trimmed).
std::vector<std::wstring> ParseExcludeCsv(const std::wstring& csv);

} // namespace edgepop
