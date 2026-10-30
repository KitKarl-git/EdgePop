#include "settings.h"

#include "geometry.h"   // edge bits live with the geometry they select

#include <string>

namespace edgepop {
namespace {

unsigned ReadBool(const wchar_t* ini, const wchar_t* section,
                  const wchar_t* key, unsigned dflt) {
    // GetPrivateProfileIntW parses only integers: "true" and "false" BOTH
    // come back 0, so every saved boolean silently loaded as false
    //. Read the string and accept both spellings.
    wchar_t buf[16];
    GetPrivateProfileStringW(section, key, dflt ? L"1" : L"0", buf, 16, ini);
    return (_wcsicmp(buf, L"true") == 0 || _wcsicmp(buf, L"1") == 0) ? 1 : 0;
}

std::wstring ReadStr(const wchar_t* ini, const wchar_t* section,
                     const wchar_t* key, const wchar_t* dflt) {
    wchar_t buf[256];
    GetPrivateProfileStringW(section, key, dflt, buf, 256, ini);
    return buf;
}

void WriteBool(const wchar_t* ini, const wchar_t* section,
               const wchar_t* key, bool v) {
    WritePrivateProfileStringW(section, key, v ? L"true" : L"false", ini);
}

// "left, right, top, bottom" -> bit mask. Unknown tokens are ignored; an
// empty result means "auto" for robustness (a hand-mangled INI can never
// disable every edge by accident).
DWORD ParseEdgeMask(const std::wstring& csv) {
    DWORD mask = 0;
    size_t pos = 0;
    while (pos < csv.size()) {
        size_t comma = csv.find(L',', pos);
        if (comma == std::wstring::npos) comma = csv.size();
        std::wstring tok = csv.substr(pos, comma - pos);
        // trim
        const size_t b = tok.find_first_not_of(L" \t");
        const size_t e = tok.find_last_not_of(L" \t");
        if (b != std::wstring::npos) {
            tok = tok.substr(b, e - b + 1);
            if (tok == L"left"  || tok == L"l") mask |= kEdgeLeftBit;
            if (tok == L"right" || tok == L"r") mask |= kEdgeRightBit;
            if (tok == L"top"   || tok == L"t") mask |= kEdgeTopBit;
            if (tok == L"bottom"|| tok == L"b") mask |= kEdgeBottomBit;
        }
        pos = comma + 1;
    }
    return mask;
}

std::wstring EdgeMaskToCsv(DWORD mask) {
    std::wstring out;
    const struct { DWORD bit; const wchar_t* name; } rows[] = {
        { kEdgeLeftBit,   L"left"   },
        { kEdgeRightBit,  L"right"  },
        { kEdgeTopBit,    L"top"    },
        { kEdgeBottomBit, L"bottom" },
    };
    for (const auto& r : rows) {
        if (mask & r.bit) {
            if (!out.empty()) out += L",";
            out += r.name;
        }
    }
    return out;
}

} // namespace

std::vector<std::wstring> ParseExcludeCsv(const std::wstring& csv) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos < csv.size()) {
        size_t comma = csv.find(L',', pos);
        if (comma == std::wstring::npos) comma = csv.size();
        std::wstring tok = csv.substr(pos, comma - pos);
        const size_t b = tok.find_first_not_of(L" \t");
        const size_t e = tok.find_last_not_of(L" \t");
        if (b != std::wstring::npos) out.push_back(tok.substr(b, e - b + 1));
        pos = comma + 1;
    }
    return out;
}

Settings LoadSettings(const wchar_t* ini) {
    Settings s;
    s.activationEnabled =
        ReadBool(ini, L"trigger", L"activation_enabled", 1) != 0;
    s.activation = (_wcsicmp(
        ReadStr(ini, L"trigger", L"activation_mode", L"edge_dwell").c_str(),
        L"cross_back") == 0)
        ? Activation::CrossBack
        : Activation::EdgeDwell;
    s.crossBackMs = GetPrivateProfileIntW(L"trigger", L"crossback_ms", 600,
                                          ini);
    s.dwellMs = GetPrivateProfileIntW(L"trigger", L"dwell_ms", 250, ini);
    const std::wstring edges = ReadStr(ini, L"trigger", L"edges", L"auto");
    if (_wcsicmp(edges.c_str(), L"auto") == 0) {
        s.edgesAuto = true;
        s.edgeMask = 0;
    } else {
        s.edgeMask = ParseEdgeMask(edges);
        s.edgesAuto = s.edgeMask == 0;
    }
    s.fullscreenGuard = ReadBool(ini, L"trigger", L"fullscreen_guard", 0) != 0;
    s.zonePx       = GetPrivateProfileIntW(L"trigger", L"zone_px", 28, ini);
    s.stopRadiusPx = GetPrivateProfileIntW(L"trigger", L"stop_radius_px", 16, ini);
    s.reactivateDelayS =
        GetPrivateProfileIntW(L"trigger", L"reactivate_delay_s", 3, ini);
    if (s.reactivateDelayS > 10) s.reactivateDelayS = 10;

    s.side = (_wcsicmp(ReadStr(ini, L"popup", L"side", L"main").c_str(),
                       L"neighbor") == 0)
                 ? PopupSide::Neighbor
                 : PopupSide::Main;
    s.chainMode = (_wcsicmp(ReadStr(ini, L"popup", L"chain_mode",
                                    L"neighbor").c_str(),
                            L"direction") == 0)
                      ? ChainMode::Direction
                      : ChainMode::Neighbor;
    s.showIcons   = ReadBool(ini, L"popup", L"show_icons", 1) != 0;
    s.showTitles  = ReadBool(ini, L"popup", L"show_titles", 1) != 0;
    s.thumbnails  = ReadBool(ini, L"popup", L"thumbnails", 1) != 0;
    s.peek = ReadBool(ini, L"popup", L"peek_on_hover", 1) != 0;
    s.cardCloseButtons =
        ReadBool(ini, L"popup", L"card_close_buttons", 1) != 0;
    s.leaveGracePx = GetPrivateProfileIntW(L"popup", L"leave_grace_px", 8,
                                           ini);
    s.stayS = GetPrivateProfileIntW(L"popup", L"stay_s", 2, ini);
    if (s.stayS > 10) s.stayS = 10;
    s.maxWidth = GetPrivateProfileIntW(L"popup", L"max_width", 420, ini);
    // The popup is never narrower than the X button + padding and never
    // wider than 800 px.
    if (s.maxWidth < 120) s.maxWidth = 120;
    if (s.maxWidth > 800) s.maxWidth = 800;
    // Card width: the thumbnail popup's own width, default
    // 300 (420/480 read too wide), hard range 230-400. A missing
    // key loads the default like every other field;'s brief
    // max_width-derived fallback was dropped when the default changed
    // (it would have masked the new value on every upgraded install).
    s.cardWidth = GetPrivateProfileIntW(L"popup", L"card_width", 300, ini);
    if (s.cardWidth < 230) s.cardWidth = 230;
    if (s.cardWidth > 400) s.cardWidth = 400;
    s.cardMaxHeight =
        GetPrivateProfileIntW(L"popup", L"card_max_height", 200, ini);
    if (s.cardMaxHeight < 120) s.cardMaxHeight = 120;
    if (s.cardMaxHeight > 400) s.cardMaxHeight = 400;

    s.virtualDesktops = (_wcsicmp(
        ReadStr(ini, L"windows", L"virtual_desktops", L"current_desktop")
            .c_str(), L"all") == 0)
        ? VirtualDesktops::All
        : VirtualDesktops::Current;
    s.excludeApps = ReadStr(ini, L"windows", L"exclude", L"");

    s.taskbarArrows = ReadBool(ini, L"taskbar", L"arrows", 1) != 0;
    s.persistentNumbers =
        ReadBool(ini, L"taskbar", L"persistent_numbers", 0) != 0;
    s.persistentArrows =
        ReadBool(ini, L"taskbar", L"persistent_arrows", 1) != 0;
    s.persistentBg = ReadBool(ini, L"taskbar", L"persistent_bg", 0) != 0;
    s.hoverNumbers = ReadBool(ini, L"taskbar", L"hover_numbers", 1) != 0;
    s.hoverArrows = ReadBool(ini, L"taskbar", L"hover_arrows", 1) != 0;
    s.hoverBg = ReadBool(ini, L"taskbar", L"hover_bg", 1) != 0;
    // Per-section sizes (5-16). Absent keys fall back to the
    // shared arrow_size when that exists, else the section defaults.
    {
        auto loadSize = [&](const wchar_t* key, unsigned dflt) {
            wchar_t buf[16];
            GetPrivateProfileStringW(L"taskbar", key, L"", buf, 16, ini);
            unsigned v;
            if (buf[0] != 0) {
                v = GetPrivateProfileIntW(L"taskbar", key, dflt, ini);
            } else {
                wchar_t legacy[16];
                GetPrivateProfileStringW(L"taskbar", L"arrow_size", L"",
                                         legacy, 16, ini);
                v = legacy[0] != 0
                        ? GetPrivateProfileIntW(L"taskbar", L"arrow_size",
                                                 dflt, ini)
                        : dflt;
            }
            if (v < 5) v = 5;
            if (v > 16) v = 16;
            return v;
        };
        s.persistentSize = loadSize(L"persistent_size", 6);
        s.hoverSize = loadSize(L"hover_size", 8);
    }
    {
        std::wstring pos =
            ReadStr(ini, L"taskbar", L"position", L"");
        if (pos.empty()) {
            // Legacy key: arrow_spot = left|right (center was the
            // name for left). Absent both -> left.
            pos = ReadStr(ini, L"taskbar", L"arrow_spot", L"left");
        }
        s.arrowPos = _wcsicmp(pos.c_str(), L"right") == 0
                         ? ArrowPos::Right
                     : _wcsicmp(pos.c_str(), L"center") == 0
                         ? ArrowPos::Center
                         : ArrowPos::Left;
    }
    {
        const std::wstring shape =
            ReadStr(ini, L"taskbar", L"arrow_shape", L"chevron");
        s.arrowShape = _wcsicmp(shape.c_str(), L"chevron") == 0
                           ? ArrowShape::Chevron
                       : _wcsicmp(shape.c_str(), L"arrow") == 0
                           ? ArrowShape::Arrow
                           : ArrowShape::Triangle;
    }
    {
        wchar_t buf[32];
        GetPrivateProfileStringW(L"taskbar", L"arrow_color",
                                 L"255,255,255", buf, 32, ini);
        int r = 255, g = 255, b = 255;
        swscanf_s(buf, L"%d ,%d ,%d", &r, &g, &b);
        auto cl = [](int v) { return static_cast<BYTE>(
            v < 0 ? 0 : (v > 255 ? 255 : v)); };
        s.arrowColor = RGB(cl(r), cl(g), cl(b));
    }
    s.arrowShadow = ReadBool(ini, L"taskbar", L"arrow_shadow", 1) != 0;
    // hover_numbers/hover_arrows already read above - a duplicated pair here
    // silently masked reordering mistakes before.

    s.startWithWindows = ReadBool(ini, L"general", L"start_with_windows", 1) != 0;
    s.debugStrips      = ReadBool(ini, L"general", L"debug_strips", 0) != 0;
    return s;
}

void SaveSettings(const wchar_t* ini, const Settings& s) {
    WritePrivateProfileStringW(
        L"trigger", L"activation_mode",
        s.activation == Activation::CrossBack ? L"cross_back"
                                              : L"edge_dwell", ini);
    WritePrivateProfileStringW(L"trigger", L"crossback_ms",
                               std::to_wstring(s.crossBackMs).c_str(), ini);
    WritePrivateProfileStringW(L"trigger", L"dwell_ms",
                               std::to_wstring(s.dwellMs).c_str(), ini);
    WritePrivateProfileStringW(L"trigger", L"edges",
                               s.edgesAuto ? L"auto"
                                           : EdgeMaskToCsv(s.edgeMask).c_str(),
                               ini);
    WriteBool(ini, L"trigger", L"fullscreen_guard", s.fullscreenGuard);
    WritePrivateProfileStringW(L"trigger", L"zone_px",
                               std::to_wstring(s.zonePx).c_str(), ini);
    WritePrivateProfileStringW(L"trigger", L"stop_radius_px",
                               std::to_wstring(s.stopRadiusPx).c_str(), ini);
    WritePrivateProfileStringW(L"trigger", L"reactivate_delay_s",
                               std::to_wstring(s.reactivateDelayS).c_str(),
                               ini);

    WritePrivateProfileStringW(
        L"popup", L"side",
        s.side == PopupSide::Neighbor ? L"neighbor" : L"main", ini);
    WritePrivateProfileStringW(
        L"popup", L"chain_mode",
        s.chainMode == ChainMode::Direction ? L"direction" : L"neighbor", ini);
    WriteBool(ini, L"popup", L"show_icons", s.showIcons);
    WriteBool(ini, L"popup", L"show_titles", s.showTitles);
    WriteBool(ini, L"popup", L"thumbnails", s.thumbnails);
    WriteBool(ini, L"popup", L"peek_on_hover", s.peek);
    WriteBool(ini, L"popup", L"card_close_buttons", s.cardCloseButtons);
    WritePrivateProfileStringW(L"popup", L"leave_grace_px",
                               std::to_wstring(s.leaveGracePx).c_str(), ini);
    WritePrivateProfileStringW(L"popup", L"stay_s",
                               std::to_wstring(s.stayS).c_str(), ini);
    WritePrivateProfileStringW(L"popup", L"max_width",
                               std::to_wstring(s.maxWidth).c_str(), ini);
    WritePrivateProfileStringW(L"popup", L"card_width",
                               std::to_wstring(s.cardWidth).c_str(), ini);
    WritePrivateProfileStringW(L"popup", L"card_max_height",
                               std::to_wstring(s.cardMaxHeight).c_str(),
                               ini);

    WritePrivateProfileStringW(
        L"windows", L"virtual_desktops",
        s.virtualDesktops == VirtualDesktops::All ? L"all"
                                                  : L"current_desktop", ini);
    WritePrivateProfileStringW(L"windows", L"exclude",
                               s.excludeApps.c_str(), ini);

    WriteBool(ini, L"trigger", L"activation_enabled", s.activationEnabled);
    WriteBool(ini, L"taskbar", L"arrows", s.taskbarArrows);
    WriteBool(ini, L"taskbar", L"persistent_numbers", s.persistentNumbers);
    WriteBool(ini, L"taskbar", L"persistent_arrows", s.persistentArrows);
    WritePrivateProfileStringW(
        L"taskbar", L"position",
        s.arrowPos == ArrowPos::Right ? L"right"
                : s.arrowPos == ArrowPos::Center ? L"center"
                                                 : L"left", ini);
    WriteBool(ini, L"taskbar", L"persistent_bg", s.persistentBg);
    WriteBool(ini, L"taskbar", L"hover_bg", s.hoverBg);
    WritePrivateProfileStringW(
        L"taskbar", L"persistent_size",
        std::to_wstring(s.persistentSize).c_str(), ini);
    WritePrivateProfileStringW(
        L"taskbar", L"hover_size", std::to_wstring(s.hoverSize).c_str(),
        ini);
    {
        const wchar_t* shape = L"triangle";
        if (s.arrowShape == ArrowShape::Chevron) shape = L"chevron";
        if (s.arrowShape == ArrowShape::Arrow) shape = L"arrow";
        WritePrivateProfileStringW(L"taskbar", L"arrow_shape", shape, ini);
    }
    {
        wchar_t buf[32];
        swprintf_s(buf, L"%u,%u,%u", GetRValue(s.arrowColor),
                   GetGValue(s.arrowColor), GetBValue(s.arrowColor));
        WritePrivateProfileStringW(L"taskbar", L"arrow_color", buf, ini);
    }
    WriteBool(ini, L"taskbar", L"arrow_shadow", s.arrowShadow);
    WriteBool(ini, L"taskbar", L"hover_numbers", s.hoverNumbers);
    WriteBool(ini, L"taskbar", L"hover_arrows", s.hoverArrows);
    // Dead keys from earlier rounds: the tri-state hover, the
    // two-way spot (replaced by position), and the shared
    // size (replaced by the per-section sizes).
    WritePrivateProfileStringW(L"taskbar", L"hover", nullptr, ini);
    WritePrivateProfileStringW(L"taskbar", L"arrow_spot", nullptr, ini);
    WritePrivateProfileStringW(L"taskbar", L"arrow_size", nullptr, ini);

    WriteBool(ini, L"general", L"start_with_windows", s.startWithWindows);
    WriteBool(ini, L"general", L"debug_strips", s.debugStrips);
}

} // namespace edgepop
