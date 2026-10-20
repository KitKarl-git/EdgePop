#include "osver.h"

namespace edgepop {

// RtlGetVersion is the one version query that never lies: it reads the
// real kernel build regardless of manifests or compatibility modes. It
// lives in ntdll without an import lib, so resolve it at runtime. Its
// parameter is PRTL_OSVERSIONINFOW, layout-identical to OSVERSIONINFOW
// (both start with the dwOSVersionInfoSize contract).
using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

int OsBuildNumber() {
    static int cached = []() -> int {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return 0;
        const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (!rtlGetVersion) return 0;
        OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (rtlGetVersion(&info) != 0) return 0;
        return static_cast<int>(info.dwBuildNumber);
    }();
    return cached;
}

} // namespace edgepop
