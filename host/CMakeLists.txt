cmake_minimum_required(VERSION 3.21)
project(thorstream_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT MSVC)
  message(FATAL_ERROR "thorstream host is Windows/MSVC only")
endif()

add_executable(thorstream-host
  src/main.cpp
  src/window_list.cpp
  src/d3d_device.cpp
  src/window_capture.cpp
  src/save_png.cpp
  src/encoder_nvenc.cpp
  src/net_server.cpp
  src/session.cpp
  src/gamepad_vigem.cpp
  src/scaler.cpp
  src/sudovda.cpp
  src/virtual_display.cpp
  src/display_topology.cpp
  src/game_library.cpp
  src/cover_art.cpp
  src/discovery.cpp
  src/session_lock.cpp
  src/web_console.cpp
  src/input_injector.cpp
  third_party/vigem/src/ViGEmClient.cpp
)

target_include_directories(thorstream-host PRIVATE src third_party third_party/vigem)

# Vendored third-party code: not our warnings to fix.
set_source_files_properties(third_party/vigem/src/ViGEmClient.cpp PROPERTIES COMPILE_FLAGS "/W0")

target_compile_options(thorstream-host PRIVATE
  /W4 /permissive- /EHsc /utf-8
  /wd4100          # unreferenced formal parameter - noisy with WinRT event handlers
  /wd4828          # vendored ViGEm headers are Latin-1, not UTF-8
)

target_compile_definitions(thorstream-host PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  UNICODE
  _UNICODE
)

target_link_libraries(thorstream-host PRIVATE
  windowsapp     # C++/WinRT
  d3d11
  dxgi
  dwmapi
  user32
  shcore
  windowscodecs
  d3dcompiler  # runtime HLSL compilation for the scaler
  setupapi     # ViGEmClient + SudoVDA device enumeration
  ole32        # CoCreateGuid for virtual monitor ids
  wtsapi32     # WTSConnectSession, for unlocking a locked session
)

set_target_properties(thorstream-host PROPERTIES
  VS_DPI_AWARE "PerMonitor"

  # Build as a GUI-subsystem binary so Windows never allocates a console for us.
  # Hiding one after the fact still flashes it on screen at logon, which is
  # exactly what a background host must not do. We keep wmain as the entry point
  # and attach to the caller's console when run interactively.
  WIN32_EXECUTABLE TRUE
  LINK_FLAGS "/ENTRY:wmainCRTStartup"
)
