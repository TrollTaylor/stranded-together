# Stranded Together launcher: StrandedTogether.exe (Win32 + DX11 + Dear ImGui,
# WIN32 subsystem), adapted from Diamond Dogs Together's launcher
# (cmake/launcher_gui.cmake + cmake/ddt_launcher_core.cmake there).
#
#   st_imgui            Dear ImGui 1.92.6 (third_party/imgui) with the Win32 + DX11 backends
#   st_launcher_core    everything without a window: the launcher core (Steam discovery,
#                       install gate, session.ini, status, watchdog), the join-code check
#                       (on st_net's DST1 parser), the UI logic, the mock model, the entries
#   st_launcher         the exe (OUTPUT_NAME StrandedTogether)
#   st_launcher_core_tests  pure checks (join code round trip, session.ini text, status parse...)
#   st_launcher_update_tests  the updater: SemVer, release feed, zip verification, install/rollback,
#                           the model on a fake feed; --check-imports guards the exe's imports
#
# Included from the root CMakeLists.txt. Only for MSVC on Windows.
if(NOT (WIN32 AND MSVC))
  return()
endif()
if(NOT TARGET st_net)
  message(WARNING "launcher.cmake: st_net is missing; the launcher is not built")
  return()
endif()

set(_st_launcher_dir "${CMAKE_CURRENT_SOURCE_DIR}/native/launcher")
set(_st_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/launcher")
if(NOT DEFINED ST_BUILD_ID)
  set(ST_BUILD_ID "${PROJECT_VERSION}")
endif()
configure_file(native/launcher/st_launcher_version.h.in "${_st_generated}/st_launcher_version.h" @ONLY)

# ---- Dear ImGui (MIT, third_party/imgui/UPSTREAM.txt)
add_library(st_imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_win32.cpp
  third_party/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(st_imgui PUBLIC third_party/imgui)
target_compile_definitions(st_imgui PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_compile_options(st_imgui PRIVATE /utf-8)
target_link_libraries(st_imgui PRIVATE d3d11 d3dcompiler dwmapi)

# ---- Everything without a window
add_library(st_launcher_core STATIC
  native/launcher/crc32.cpp
  native/launcher/file_io.cpp
  native/launcher/hash.cpp
  native/launcher/inflate.cpp
  native/launcher/zip.cpp
  native/launcher/core/core_env_win32.cpp
  native/launcher/core/core_install.cpp
  native/launcher/core/core_json.cpp
  native/launcher/core/core_log.cpp
  native/launcher/core/core_model.cpp
  native/launcher/core/core_paths.cpp
  native/launcher/core/core_sandbox.cpp
  native/launcher/core/core_selftest.cpp
  native/launcher/core/core_session.cpp
  native/launcher/core/core_text.cpp
  native/launcher/core/core_update.cpp
  native/launcher/core/core_util.cpp
  native/launcher/launcher_join_code.cpp
  native/launcher/launcher_ui_logic.cpp
  native/launcher/launcher_model_mock.cpp
  native/launcher/launcher_entry.cpp)
target_include_directories(st_launcher_core PUBLIC native native/launcher native/launcher/core "${_st_generated}")
# native/mod/sandbox.hpp (header only): the session.ini keys and graphics text shared with the mod
target_include_directories(st_launcher_core PRIVATE native/mod)
# st_net brings stcoop/steam_join_code + link_crypto (the DST1 code). Never winhttp.lib: the
# launcher lives in the game folder next to the mod's winhttp.dll proxy, so the update check
# loads System32's winhttp.dll itself (core_update.cpp; st_launcher_no_winhttp_import checks it).
target_link_libraries(st_launcher_core PUBLIC st_net shell32 ole32 user32 advapi32 bcrypt)
target_compile_definitions(st_launcher_core PRIVATE "ST_LAUNCHER_VERSION=\"${ST_BUILD_ID}\"" UNICODE _UNICODE)
target_compile_options(st_launcher_core PRIVATE /W4 /WX /utf-8 /permissive-)

# ---- StrandedTogether.exe
add_executable(st_launcher WIN32
  native/launcher/launcher_main.cpp
  native/launcher/launcher_app.cpp
  native/launcher/launcher_theme.cpp
  native/launcher/resources/st_launcher.rc
  native/launcher/resources/st_launcher.manifest)
set_target_properties(st_launcher PROPERTIES OUTPUT_NAME StrandedTogether)
# rc.exe reads the icon and the emblem itself; the dependency scan only sees
# #include files, so without this a regenerated icon would not rebuild the exe.
set_source_files_properties(native/launcher/resources/st_launcher.rc PROPERTIES OBJECT_DEPENDS
  "${_st_launcher_dir}/resources/st_launcher.ico;${_st_launcher_dir}/resources/st_logo.png")
target_include_directories(st_launcher PRIVATE native/launcher/resources "${_st_generated}")
target_compile_options(st_launcher PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX /utf-8 /permissive->)
target_compile_definitions(st_launcher PRIVATE UNICODE _UNICODE)
target_link_libraries(st_launcher PRIVATE st_launcher_core st_imgui d3d11 dxgi d3dcompiler dwmapi
                      windowscodecs ole32 shell32 user32 gdi32)
# Static imports from System32 only: the game folder holds the mod's winhttp.dll and
# the game's own d3dcompiler_47.dll (Windows 10 1607+; older systems ignore the flag).
target_link_options(st_launcher PRIVATE /DEPENDENTLOADFLAG:0x800)

# ---- Tests
add_executable(st_launcher_core_tests tests/launcher_core_tests.cpp)
target_link_libraries(st_launcher_core_tests PRIVATE st_launcher_core)
target_compile_options(st_launcher_core_tests PRIVATE /W4 /WX /utf-8 /permissive-)
target_compile_definitions(st_launcher_core_tests PRIVATE UNICODE _UNICODE)
add_test(NAME st_launcher_core_tests COMMAND st_launcher_core_tests)
add_executable(st_launcher_update_tests tests/launcher_update_tests.cpp)
target_link_libraries(st_launcher_update_tests PRIVATE st_launcher_core)
target_compile_options(st_launcher_update_tests PRIVATE /W4 /WX /utf-8 /permissive-)
target_compile_definitions(st_launcher_update_tests PRIVATE UNICODE _UNICODE)
add_test(NAME st_launcher_update_tests COMMAND st_launcher_update_tests)
# StrandedTogether.exe never imports winhttp.dll (statically or delay-loaded) and keeps
# /DEPENDENTLOADFLAG:0x800: the mod's winhttp.dll proxy sits next to it.
add_test(NAME st_launcher_no_winhttp_import COMMAND st_launcher_update_tests --check-imports $<TARGET_FILE:st_launcher>)
# The exe itself: the built-in self-test (it reports, not fails, a build folder that is
# not a game folder), and every page rendered once (hidden window, WARP fallback without
# a GPU) into the build folder. ImGui reports recoverable layout errors on stdout in that
# mode: any of them fails the test.
add_test(NAME st_launcher_selftest COMMAND st_launcher --selftest)
add_test(NAME st_launcher_screens COMMAND st_launcher --screenshot-all "${CMAKE_CURRENT_BINARY_DIR}/launcher-shots" --scale 1)
set_tests_properties(st_launcher_screens PROPERTIES FAIL_REGULAR_EXPRESSION "imgui-error|FAIL:")
# The committed icon and emblem are what the generator makes from docs/readme/logo.png.
find_package(Python3 3.8 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME st_launcher_assets_current
           COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/launcher/make_launcher_assets.py" --check)
endif()
