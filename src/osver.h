// True OS build number, immune to the manifest compat lies of
// GetVersionEx. Callers compare against known builds:
// 17763 = Windows 10 1809 (the supported floor), 22000 = Windows 11.
#pragma once

#include <windows.h>

namespace edgepop {

// Returns the RTL build number, or 0 if the ntdll export is missing
// (pre-Vista; treat 0 as "unknown, don't gate on it").
int OsBuildNumber();

} // namespace edgepop
