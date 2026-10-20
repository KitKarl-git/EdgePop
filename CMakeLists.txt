cmake_minimum_required(VERSION 3.24)

project(EdgePop
    VERSION 1.2.0
    DESCRIPTION "Edge-of-monitor window switcher"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(EdgePop WIN32
    src/main.cpp
    src/geometry.cpp
    src/settings.cpp
    src/fullscreen.cpp
    src/trigger.cpp
    src/winenums.cpp
    src/popup.cpp
    src/taskbarbadges.cpp
    src/osver.cpp
    src/settingsui.cpp
    src/version.rc
    src/settings.rc)

target_include_directories(EdgePop PRIVATE src)

target_compile_definitions(EdgePop PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    # The compiler-enforced Windows floor. 0x0A00 = Win10,
    # NTDDI_WIN10_RS5 = 1809 (build 17763): any API newer than RS5 that is
    # SDK-gated stops compiling, so the Win10 door stays open by machine.
    # Ungated Win11 constants (e.g. DWMWA_WINDOW_CORNER_PREFERENCE) are
    # isolated at their call sites instead.
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=NTDDI_WIN10_RS5)

target_compile_options(EdgePop PRIVATE
    /W4 /permissive- /utf-8 /MP)

target_link_libraries(EdgePop PRIVATE
    user32      # windows, TrackMouseEvent, SetProcessDpiAwarenessContext
    gdi32       # painting (icons into DIBs)
    shell32     # Shell_NotifyIcon, tray
    dwmapi      # DWMWA_CLOAKED (fullscreen + cloak detection)
    shcore      # GetDpiForMonitor (per-monitor scale logging)
    advapi32    # HKCU Run key (start with Windows)
    comctl32    # settings dialog trackbars
    comdlg32    # ChooseColor
    ole32       # CoInitializeEx / IVirtualDesktopManager
    uiautomationcore   # taskbar monitor indicators
    d2d1 d3d11 dxgi dcomp dwrite)   # popup renderer (system DLLs)

# The manifest is embedded from src/version.rc; suppress the linker's own.
target_link_options(EdgePop PRIVATE "/MANIFEST:NO")

# Zone state machine unit tests (console). Build with
#   cmake --build build --config Release --target edgepop_trigger_test
option(EDGEPOP_BUILD_TESTS "Build unit tests" ON)
if(EDGEPOP_BUILD_TESTS)
    add_executable(edgepop_trigger_test
        src/tests/trigger_test.cpp
        src/trigger.cpp
        src/geometry.cpp)
    target_include_directories(edgepop_trigger_test PRIVATE src)
    target_compile_definitions(edgepop_trigger_test PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
        WINVER=0x0A00 NTDDI_VERSION=NTDDI_WIN10_RS5)
    target_compile_options(edgepop_trigger_test PRIVATE /W4 /permissive- /utf-8 /MP)
    target_link_libraries(edgepop_trigger_test PRIVATE user32)
    set_property(TARGET edgepop_trigger_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Alt-Tab window filter unit tests.
if(EDGEPOP_BUILD_TESTS)
    add_executable(edgepop_winenums_test
        src/tests/winenums_test.cpp
        src/winenums.cpp)
    target_include_directories(edgepop_winenums_test PRIVATE src)
    target_compile_definitions(edgepop_winenums_test PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
        WINVER=0x0A00 NTDDI_VERSION=NTDDI_WIN10_RS5)
    target_compile_options(edgepop_winenums_test PRIVATE /W4 /permissive- /utf-8 /MP)
    target_link_libraries(edgepop_winenums_test PRIVATE user32 dwmapi ole32)
    set_property(TARGET edgepop_winenums_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Settings persistence round-trip tests.
if(EDGEPOP_BUILD_TESTS)
    add_executable(edgepop_settings_test
        src/tests/settings_test.cpp
        src/settings.cpp)
    target_include_directories(edgepop_settings_test PRIVATE src)
    target_compile_definitions(edgepop_settings_test PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
        WINVER=0x0A00 NTDDI_VERSION=NTDDI_WIN10_RS5)
    target_compile_options(edgepop_settings_test PRIVATE /W4 /permissive- /utf-8 /MP)
    target_link_libraries(edgepop_settings_test PRIVATE kernel32)
    set_property(TARGET edgepop_settings_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()


# Smallest possible final binary: statically linked CRT, no runtime DLL deps
# (the whole point: the final exe stays tiny).
option(EDGEPOP_STATIC_CRT "Statically link the CRT into the exe" ON)
if(EDGEPOP_STATIC_CRT)
    set_property(TARGET EdgePop PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
