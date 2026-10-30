// Round-trip unit tests for settings persistence: what SaveSettings
// writes must load back identically - including booleans, whose "true"/
// "false" spelling GetPrivateProfileIntW cannot parse.

#include <cstdio>
#include <windows.h>

#include "geometry.h"   // edge mask bits
#include "settings.h"

namespace {

int g_failures = 0;

void Check(const char* name, bool ok) {
    printf("%-52s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}

} // namespace

int main() {
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"edgepop-settings-test.ini");
    DeleteFileW(path);

    edgepop::Settings s;                 // non-default values everywhere
    s.activation = edgepop::Activation::CrossBack;
    s.crossBackMs = 900;
    s.dwellMs = 100;
    s.edgesAuto = false;
    s.edgeMask = edgepop::kEdgeLeftBit | edgepop::kEdgeBottomBit;
    s.fullscreenGuard = false;
    s.zonePx = 40;
    s.stopRadiusPx = 20;
    s.reactivateDelayS = 7;
    s.side = edgepop::PopupSide::Neighbor;
    s.chainMode = edgepop::ChainMode::Direction;
    s.showIcons = false;
    s.showTitles = true;
    s.thumbnails = true;
    s.peek = false;
    s.cardCloseButtons = false;                        //
    s.leaveGracePx = 12;
    s.stayS = 6;
    s.maxWidth = 640;
    s.cardWidth = 380;                                   //
    s.cardMaxHeight = 340;
    s.virtualDesktops = edgepop::VirtualDesktops::All;
    s.excludeApps = L"steam.exe, Discord.exe";
    s.activationEnabled = false;                       //
    s.taskbarArrows = false;                           //
    s.persistentNumbers = true;                        //
    s.persistentArrows = false;                        //
    s.persistentBg = true;                             //
    s.persistentSize = 11;                             //
    s.hoverBg = false;                                 //
    s.hoverSize = 14;                                  //
    s.arrowPos = edgepop::ArrowPos::Right;             //
    s.arrowShape = edgepop::ArrowShape::Chevron;       //
    s.arrowColor = RGB(255, 170, 0);                   //
    s.arrowShadow = false;                             //
    s.hoverNumbers = false;                            //
    s.hoverArrows = true;                              //
    s.startWithWindows = true;
    s.debugStrips = true;
    edgepop::SaveSettings(path, s);

    edgepop::Settings t = edgepop::LoadSettings(path);
    Check("activation round-trips", t.activation == s.activation);
    Check("crossBackMs round-trips", t.crossBackMs == 900);
    Check("dwellMs round-trips", t.dwellMs == 100);
    Check("edgesAuto round-trips", t.edgesAuto == false);
    Check("edgeMask round-trips", t.edgeMask == s.edgeMask);
    Check("fullscreenGuard=false survives reload", t.fullscreenGuard == false);
    Check("zonePx round-trips", t.zonePx == 40);
    Check("stopRadiusPx round-trips", t.stopRadiusPx == 20);
    Check("reactivateDelayS round-trips", t.reactivateDelayS == 7);
    Check("side round-trips", t.side == edgepop::PopupSide::Neighbor);
    Check("chainMode round-trips",
          t.chainMode == edgepop::ChainMode::Direction);
    Check("showIcons=false survives reload", t.showIcons == false);
    Check("showTitles=true survives reload", t.showTitles == true);
    Check("thumbnails=true survives reload", t.thumbnails == true);
    Check("peek=false survives reload", t.peek == false);
    Check("cardCloseButtons=false survives reload",
          t.cardCloseButtons == false);
    Check("leaveGracePx round-trips", t.leaveGracePx == 12);
    Check("stayS round-trips", t.stayS == 6);
    Check("maxWidth round-trips", t.maxWidth == 640);
    Check("cardWidth round-trips", t.cardWidth == 380);
    Check("cardMaxHeight round-trips", t.cardMaxHeight == 340);
    Check("virtualDesktops round-trips",
          t.virtualDesktops == edgepop::VirtualDesktops::All);
    Check("excludeApps round-trips",
          t.excludeApps == L"steam.exe, Discord.exe");
    Check("activationEnabled=false survives reload",
          t.activationEnabled == false);
    Check("taskbarArrows=false survives reload",
          t.taskbarArrows == false);
    Check("persistentNumbers=true survives reload",
          t.persistentNumbers == true);
    Check("persistentArrows=false survives reload",
          t.persistentArrows == false);
    Check("arrowPos=Right round-trips",
          t.arrowPos == edgepop::ArrowPos::Right);
    Check("persistentBg=true survives reload",
          t.persistentBg == true);
    Check("persistentSize=11 round-trips",
          t.persistentSize == 11);
    Check("hoverBg=false survives reload",
          t.hoverBg == false);
    Check("hoverSize=14 round-trips", t.hoverSize == 14);
    Check("arrowShape=Chevron round-trips",
          t.arrowShape == edgepop::ArrowShape::Chevron);
    Check("arrowColor round-trips",
          t.arrowColor == RGB(255, 170, 0));
    Check("arrowShadow=false survives reload",
          t.arrowShadow == false);
    Check("hoverNumbers=false survives reload",
          t.hoverNumbers == false);
    Check("hoverArrows=true survives reload",
          t.hoverArrows == true);
    {
        const auto ex = edgepop::ParseExcludeCsv(L" steam.exe ,,x.exe ");
        Check("ParseExcludeCsv trims and skips empties",
              ex.size() == 2 && ex[0] == L"steam.exe" && ex[1] == L"x.exe");
    }
    Check("startWithWindows=true survives reload",
          t.startWithWindows == true);
    Check("debugStrips=true survives reload", t.debugStrips == true);

    // The bug specifically: true must not load as false. Flip every
    // bool to the opposite of the previous pass and re-round-trip.
    s.fullscreenGuard = true;
    s.showIcons = true;
    s.showTitles = false;
    s.thumbnails = false;
    s.cardCloseButtons = true;                         //
    s.startWithWindows = false;
    s.debugStrips = false;
    s.activationEnabled = true;                        //
    s.taskbarArrows = true;
    s.persistentNumbers = false;
    s.persistentArrows = true;
    s.arrowPos = edgepop::ArrowPos::Center;
    s.persistentSize = 6;
    s.hoverSize = 6;
    s.persistentBg = false;
    s.hoverBg = true;
    s.arrowShape = edgepop::ArrowShape::Arrow;
    s.arrowShadow = true;
    s.hoverNumbers = true;
    s.hoverArrows = false;
    edgepop::SaveSettings(path, s);
    t = edgepop::LoadSettings(path);
    Check("fullscreenGuard=true survives reload",
          t.fullscreenGuard == true);
    Check("showIcons=true survives reload", t.showIcons == true);
    Check("showTitles=false survives reload", t.showTitles == false);
    Check("thumbnails=false survives reload", t.thumbnails == false);
    Check("cardCloseButtons=true survives reload",
          t.cardCloseButtons == true);
    Check("startWithWindows=false survives reload",
          t.startWithWindows == false);
    Check("debugStrips=false survives reload",
          t.debugStrips == false);
    Check("activationEnabled=true survives reload",
          t.activationEnabled == true);
    Check("taskbarArrows=true survives reload",
          t.taskbarArrows == true);
    Check("persistentNumbers=false survives reload",
          t.persistentNumbers == false);
    Check("persistentArrows=true survives reload",
          t.persistentArrows == true);
    Check("arrowPos=Center round-trips",
          t.arrowPos == edgepop::ArrowPos::Center);
    Check("persistentSize=6 round-trips",
          t.persistentSize == 6);
    Check("hoverSize=6 round-trips", t.hoverSize == 6);
    Check("persistentBg=false survives reload",
          t.persistentBg == false);
    Check("hoverBg=true survives reload", t.hoverBg == true);
    Check("arrowShape=Arrow round-trips",
          t.arrowShape == edgepop::ArrowShape::Arrow);
    Check("arrowShadow=true survives reload",
          t.arrowShadow == true);
    Check("hoverNumbers=true survives reload",
          t.hoverNumbers == true);
    Check("hoverArrows=false survives reload",
          t.hoverArrows == false);

    // Hard limits: max_width clamps into 120-800 on load,
    // card_width into 230-400, reactivate_delay_s and stay_s cap
    // at 10; arrow_size clamps 5-16.
    s.maxWidth = 0;
    s.cardWidth = 99;                                    //
    s.cardMaxHeight = 99;
    s.reactivateDelayS = 99;
    s.stayS = 99;
    s.persistentSize = 99;
    s.hoverSize = 99;
    edgepop::SaveSettings(path, s);
    t = edgepop::LoadSettings(path);
    Check("maxWidth=0 clamps to 120 on load",
          t.maxWidth == 120);
    Check("cardWidth=99 clamps to 230 on load",
          t.cardWidth == 230);
    Check("cardMaxHeight=99 clamps to 120 on load",
          t.cardMaxHeight == 120);
    // The upper clamp too, while the file is fresh.
    s.cardWidth = 9999;
    s.cardMaxHeight = 9999;
    edgepop::SaveSettings(path, s);
    t = edgepop::LoadSettings(path);
    Check("cardWidth=9999 clamps to 400 on load",
          t.cardWidth == 400);
    Check("cardMaxHeight=9999 clamps to 400 on load",
          t.cardMaxHeight == 400);
    Check("reactivateDelayS=99 clamps to 10 on load", t.reactivateDelayS == 10);
    Check("stayS=99 clamps to 10 on load", t.stayS == 10);
    Check("persistentSize=99 clamps to 16 on load",
          t.persistentSize == 16);
    Check("hoverSize=99 clamps to 16 on load", t.hoverSize == 16);
    // Legacy shared size feeds both sections when theirs are absent.
    WritePrivateProfileStringW(L"taskbar", L"persistent_size", nullptr, path);
    WritePrivateProfileStringW(L"taskbar", L"hover_size", nullptr, path);
    WritePrivateProfileStringW(L"taskbar", L"arrow_size", L"9", path);
    t = edgepop::LoadSettings(path);
    Check("legacy arrow_size=9 feeds both sections",
          t.persistentSize == 9 && t.hoverSize == 9);

    // Legacy key: arrow_spot = right feeds position when position
    // is absent.
    WritePrivateProfileStringW(L"taskbar", L"position", nullptr, path);
    WritePrivateProfileStringW(L"taskbar", L"arrow_spot", L"right", path);
    t = edgepop::LoadSettings(path);
    Check("legacy arrow_spot=right loads as Right",
          t.arrowPos == edgepop::ArrowPos::Right);

    // Missing keys must load as the DOCUMENTED defaults - the loader's
    // fallback spelled two of them wrong for rounds (thumbnails and
    // start_with_windows loaded as false on a fresh machine,).
    DeleteFileW(path);
    t = edgepop::LoadSettings(path);
    Check("missing keys: thumbnails defaults true", t.thumbnails == true);
    Check("missing keys: startWithWindows defaults true",
          t.startWithWindows == true);
    Check("missing keys: maxWidth defaults 420", t.maxWidth == 420);
    Check("missing keys: cardWidth defaults 300",
          t.cardWidth == 300);
    Check("missing keys: cardMaxHeight defaults 320",
          t.cardMaxHeight == 320);
    Check("missing keys: peek defaults true", t.peek == true);
    Check("missing keys: cardCloseButtons defaults true",
          t.cardCloseButtons == true);
    Check("missing keys: fullscreenGuard defaults false",
          t.fullscreenGuard == false);
    Check("missing keys: stayS defaults 2", t.stayS == 2);
    Check("missing keys: activationEnabled defaults true",
          t.activationEnabled == true);
    Check("missing keys: taskbarArrows defaults true",
          t.taskbarArrows == true);
    Check("missing keys: persistentNumbers defaults false",
          t.persistentNumbers == false);
    Check("missing keys: persistentArrows defaults true",
          t.persistentArrows == true);
    Check("missing keys: arrowPos defaults Left",
          t.arrowPos == edgepop::ArrowPos::Left);
    Check("missing keys: persistentSize defaults 6",
          t.persistentSize == 6);
    Check("missing keys: hoverSize defaults 8", t.hoverSize == 8);
    Check("missing keys: persistentBg defaults false",
          t.persistentBg == false);
    Check("missing keys: hoverBg defaults true",
          t.hoverBg == true);
    Check("missing keys: arrowShape defaults Chevron",
          t.arrowShape == edgepop::ArrowShape::Chevron);
    Check("missing keys: arrowColor defaults white",
          t.arrowColor == RGB(255, 255, 255));
    Check("missing keys: arrowShadow defaults true",
          t.arrowShadow == true);
    Check("missing keys: hoverNumbers defaults true",
          t.hoverNumbers == true);
    Check("missing keys: hoverArrows defaults true",
          t.hoverArrows == true);

    // A saved max_width must NOT leak into card_width when card_width is
    // absent (dropped's derived fallback so the new 300
    // default actually reaches upgraded installs).
    DeleteFileW(path);
    WritePrivateProfileStringW(L"popup", L"max_width", L"600", path);
    t = edgepop::LoadSettings(path);
    Check("absent card_width ignores max_width -> 300",
          t.cardWidth == 300 && t.maxWidth == 600);

    DeleteFileW(path);
    printf("\n%s (%d failures)\n",
           g_failures == 0 ? "ALL TESTS PASS" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
