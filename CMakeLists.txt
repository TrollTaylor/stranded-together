cmake_minimum_required(VERSION 3.20)
project(stranded_together VERSION 0.19.0 LANGUAGES C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()

# Build id both players' links compare (HELLO build field, 16 bytes max).
set(ST_BUILD_ID "${PROJECT_VERSION}")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                  OUTPUT_VARIABLE _st_git OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _st_git_rc)
  if(_st_git_rc EQUAL 0 AND _st_git MATCHES "^[0-9a-f]+$")
    set(ST_BUILD_ID "${PROJECT_VERSION}+${_st_git}")
    # Tracked files changed (at configure time): a "-xx" suffix, so a development build never passes the
    # HELLO build check against the release made from the same commit (tools/release.ps1 needs a
    # clean tree, so releases never carry it). Re-evaluated whenever CMake re-configures.
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE _st_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _st_dirty_rc)
    if(_st_dirty_rc EQUAL 0 AND NOT _st_dirty STREQUAL "")
      # The suffix is "-" and two hex digits of a hash of the files that define the wire (line endings
      # normalised), so two development builds with different frames (a version bump of STC1, ...) also
      # fail the check instead of silently dropping each other's frames; editing one of them re-runs
      # this configure step. The commit is shortened when needed to stay within the 16 bytes.
      set(_st_wire_files native/mod/pose_wire.hpp native/mod/pose_wire.cpp native/include/stcoop/link_protocol.hpp
                         native/net/link_protocol.cpp native/mod/save_xfer.hpp native/mod/save_xfer.cpp)
      set(_st_wire_text "")
      foreach(_f IN LISTS _st_wire_files)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${_f}" _t)
        string(REPLACE "\r\n" "\n" _t "${_t}")
        string(APPEND _st_wire_text "${_f}\n${_t}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
      endforeach()
      string(SHA256 _st_wire_hash "${_st_wire_text}")
      string(SUBSTRING "${_st_wire_hash}" 0 2 _st_wire_hash)
      string(LENGTH "${PROJECT_VERSION}" _st_vlen)
      math(EXPR _st_glen "16 - ${_st_vlen} - 1 - 3")
      if(_st_glen GREATER 7)
        set(_st_glen 7)
      endif()
      if(_st_glen LESS 4)
        set(_st_glen 4)
      endif()
      string(SUBSTRING "${_st_git}" 0 ${_st_glen} _st_git_short)
      set(ST_BUILD_ID "${PROJECT_VERSION}+${_st_git_short}-${_st_wire_hash}")
    endif()
  endif()
endif()
message(STATUS "stranded-together build id ${ST_BUILD_ID}")

set(ST_WARNINGS /W4 /WX /utf-8 /permissive-)

# ---- MinHook (BSD-2-Clause, third_party/minhook)
add_library(st_minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c)
target_include_directories(st_minhook PUBLIC third_party/minhook/include)

# ---- Network layer: ported from Diamond Dogs Together (game-agnostic link v2 + Steam P2P)
add_library(st_net STATIC
  native/net/import_table.cpp
  native/net/link_crypto.cpp
  native/net/link_protocol.cpp
  native/net/link_session.cpp
  native/net/pose_interp.cpp
  native/net/steam_join_code.cpp
  native/net/steam_net_callbacks.cpp
  native/net/steam_net_dispatch.cpp
  native/net/steam_net_gates.cpp
  native/net/steam_transport.cpp
  native/net/udp_transport.cpp
  native/net/win_import_patch.cpp)
target_include_directories(st_net PUBLIC native/include)

target_compile_options(st_net PRIVATE ${ST_WARNINGS})
target_link_libraries(st_net PUBLIC ws2_32 bcrypt)

# ---- Dear ImGui for the in-game debug menu: core + DX12 backend (third_party/imgui,
# v1.92.6). Own build (not the launcher's st_imgui): IM_ASSERT never aborts the game
# (native/mod/overlay/overlay_imconfig.h).
add_library(st_mod_imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_dx12.cpp)
target_include_directories(st_mod_imgui PUBLIC third_party/imgui third_party/imgui/backends native/mod/overlay)
target_compile_definitions(st_mod_imgui PUBLIC "IMGUI_USER_CONFIG=\"overlay_imconfig.h\"")
target_compile_options(st_mod_imgui PRIVATE /utf-8)
target_link_libraries(st_mod_imgui PUBLIC dxgi d3dcompiler)

# Overlay + debug-menu sources shared by the mod and the overlay harness.
set(ST_OVERLAY_SOURCES
  native/mod/overlay/overlay.cpp
  native/mod/overlay/overlay_input.cpp
  native/mod/overlay/overlay_keys.cpp
  native/mod/overlay/overlay_menu.cpp
  native/mod/overlay/overlay_positions.cpp
  native/mod/overlay/overlay_renderer.cpp
  native/mod/overlay/overlay_tags.cpp
  native/mod/overlay/overlay_font.cpp
  native/mod/game_font.cpp
  native/mod/game_font_raster.cpp)

# ---- The mod: winhttp.dll proxy loaded by ds.exe from the game folder
add_library(st_mod SHARED
  native/mod/dllmain.cpp
  native/mod/log.cpp
  native/mod/config.cpp
  native/mod/game_api.cpp
  native/mod/steam_owner.cpp
  native/mod/link.cpp
  native/mod/pose_wire.cpp
  native/mod/player_sync.cpp
  native/mod/partner_source.cpp
  native/mod/status.cpp
  native/mod/debug_actions.cpp
  native/mod/game_actions.cpp
  native/mod/partner_holo.cpp
  native/mod/holo_catalog.cpp
  native/mod/anim_census.cpp
  native/mod/body_table.cpp
  native/mod/body_choice.cpp
  native/mod/real_body.cpp
  native/mod/partner_outfit.cpp
  native/mod/nametags.cpp
  native/mod/partner_collision.cpp
  native/mod/partner_gear.cpp
  native/mod/gear_look.cpp
  native/mod/menu_figure.cpp
  native/mod/audio_probe.cpp
  native/mod/self_voice.cpp
  native/mod/partner_voice.cpp
  native/mod/partner_sound.cpp
  native/mod/vehicle_sync.cpp
  native/mod/vehicle_wire.cpp
  native/mod/vehicle_share.cpp
  native/mod/vehicle_drive.cpp
  native/mod/ride_wire.cpp
  native/mod/ride_together.cpp
  native/mod/save_guard.cpp
  native/mod/tackle_rules.cpp
  native/mod/partner_tackle.cpp
  native/mod/save_xfer.cpp
  native/mod/save_xfer_io.cpp
  native/mod/coop_session.cpp
  native/mod/title_wait.cpp
  native/mod/dev_tools.cpp
  native/mod/story_state.cpp
  native/mod/automation.cpp

  native/mod/sandbox.cpp
  ${ST_OVERLAY_SOURCES})
set_target_properties(st_mod PROPERTIES OUTPUT_NAME winhttp PREFIX "")
target_include_directories(st_mod PRIVATE native/mod)
target_compile_definitions(st_mod PRIVATE "ST_BUILD_ID=\"${ST_BUILD_ID}\"" UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(st_mod PRIVATE ${ST_WARNINGS})
target_link_libraries(st_mod PRIVATE st_net st_minhook user32 st_mod_imgui delayimp)
# The proxy emits its own winhttp.lib; never link the SDK one into the mod.
# Graphics DLLs load only when the debug menu is used (the game delay-loads them too).
target_link_options(st_mod PRIVATE /DELAYLOAD:dxgi.dll /DELAYLOAD:d3dcompiler_47.dll)

# ---- Debug menu checks (no game, no GPU): action queue/registry, log ring,
# saved-position parsing, key names. tests/overlay_sync_stubs.cpp stands in for
# player_sync's debug accessors.
add_executable(st_overlay_tests tests/overlay_tests.cpp tests/overlay_sync_stubs.cpp
               native/mod/debug_actions.cpp native/mod/log.cpp
               native/mod/overlay/overlay_keys.cpp native/mod/overlay/overlay_positions.cpp)
target_include_directories(st_overlay_tests PRIVATE native/mod native/mod/overlay third_party/imgui)
target_compile_definitions(st_overlay_tests PRIVATE "IMGUI_USER_CONFIG=\"overlay_imconfig.h\"" UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(st_overlay_tests PRIVATE ${ST_WARNINGS})
add_test(NAME st_overlay_tests COMMAND st_overlay_tests)

# ---- st_overlay_harness: its own window + D3D12 device + DIRECT queue + flip-model
# swapchain, the SAME overlay hook path as the mod, menu opened programmatically,
# pixel checks, ResizeBuffers (incl. a 10-bit format) and PNG screenshots in the
# build directory. Needs a GPU or WARP (--warp); see docs/debug-menu.md.
add_executable(st_overlay_harness tests/overlay_harness.cpp tests/overlay_sync_stubs.cpp
               native/mod/debug_actions.cpp native/mod/log.cpp ${ST_OVERLAY_SOURCES})
target_include_directories(st_overlay_harness PRIVATE native/mod native/mod/overlay)
target_compile_definitions(st_overlay_harness PRIVATE "ST_BUILD_ID=\"${ST_BUILD_ID}\"" UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(st_overlay_harness PRIVATE ${ST_WARNINGS})
target_link_libraries(st_overlay_harness PRIVATE st_minhook st_mod_imgui d3d12 user32 windowscodecs ole32 bcrypt)
add_test(NAME st_overlay_harness COMMAND st_overlay_harness --out "${CMAKE_CURRENT_BINARY_DIR}")

# ---- Pure tests (no game needed)
add_executable(st_net_tests tests/net_tests.cpp native/mod/pose_wire.cpp native/mod/body_table.cpp native/mod/body_choice.cpp
               native/mod/game_font.cpp)
target_include_directories(st_net_tests PRIVATE native/mod)
target_link_libraries(st_net_tests PRIVATE st_net)
target_compile_options(st_net_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_net_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_net_tests COMMAND st_net_tests)

# ---- The host-held save's transfer (STX1, native/mod/save_xfer.*): the wire, the sender and receiver state
# machines, SHA-256, checkpoint.dat validation, the link's burst cap, and the read-only reader / sandbox writer on
# temporary folders (never a real save).
add_executable(st_save_xfer_tests tests/save_xfer_tests.cpp native/mod/save_xfer.cpp native/mod/save_xfer_io.cpp)
target_include_directories(st_save_xfer_tests PRIVATE native/mod native/include)
target_compile_options(st_save_xfer_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_save_xfer_tests PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
add_test(NAME st_save_xfer_tests COMMAND st_save_xfer_tests)

# ---- The real shared vehicles' pure part (SVM1 on link channel 3, native/mod/vehicle_wire.*): every message's strict
# decoder, the snapshot's chunking and reassembly, the spawn request flags, the gate, the binding and the host's rules.
add_executable(st_vehicle_wire_tests tests/vehicle_wire_tests.cpp native/mod/vehicle_wire.cpp)
target_include_directories(st_vehicle_wire_tests PRIVATE native/mod)
target_compile_options(st_vehicle_wire_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_vehicle_wire_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_vehicle_wire_tests COMMAND st_vehicle_wire_tests)

# ---- Riding together's pure part (STQ1 inside SVM1 type 48 on link channel 3, native/mod/ride_wire.*): the message's strict
# decoder, the seats, boarding, the driver's answer, the forced exits, the detectors and the exit placement.
add_executable(st_ride_wire_tests tests/ride_wire_tests.cpp native/mod/ride_wire.cpp native/mod/vehicle_wire.cpp)
target_include_directories(st_ride_wire_tests PRIVATE native/mod)
target_compile_options(st_ride_wire_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_ride_wire_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_ride_wire_tests COMMAND st_ride_wire_tests)

# ---- Vehicle hits' pure part (native/mod/tackle_rules.*): the path per kind, the gate, the sphere, the hit filter, the damage
# cap, the rate limit, the STH1 hit event on channel 3 and the partner capsule's vehicle rules.
add_executable(st_tackle_rules_tests tests/tackle_rules_tests.cpp native/mod/tackle_rules.cpp native/mod/vehicle_wire.cpp)
target_include_directories(st_tackle_rules_tests PRIVATE native/mod)
target_compile_options(st_tackle_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_tackle_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_tackle_rules_tests COMMAND st_tackle_rules_tests)

# ---- Dev tools' pure part (native/mod/dev_rules.hpp): which tool runs in which session (the real-save rule), the vehicle
# kinds and variants, the weather presets, the place labels.
add_executable(st_dev_rules_tests tests/dev_rules_tests.cpp)
target_include_directories(st_dev_rules_tests PRIVATE native/mod)
target_compile_options(st_dev_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_dev_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_dev_rules_tests COMMAND st_dev_rules_tests)

# ---- Story tools' pure part (native/mod/story_rules.hpp): the episodes, the episode hash (CRC-32C as the game's crc32
# instruction), the mission states, the summary line, the story-point load rule, the [Slot] text of a save.
add_executable(st_story_rules_tests tests/story_rules_tests.cpp)
target_include_directories(st_story_rules_tests PRIVATE native/mod)
target_compile_options(st_story_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_story_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_story_rules_tests COMMAND st_story_rules_tests)

# ---- st_fakepeer: console stand-in for the second player (UDP), + self-test
add_executable(st_fakepeer tools/fakepeer/fakepeer.cpp native/mod/link.cpp native/mod/steam_owner.cpp
               native/mod/log.cpp native/mod/pose_wire.cpp)
target_include_directories(st_fakepeer PRIVATE native/mod)
target_compile_definitions(st_fakepeer PRIVATE "ST_BUILD_ID=\"${ST_BUILD_ID}\"" _CRT_SECURE_NO_WARNINGS)
target_compile_options(st_fakepeer PRIVATE ${ST_WARNINGS})
target_link_libraries(st_fakepeer PRIVATE st_net st_minhook)
add_test(NAME st_fakepeer_selftest COMMAND st_fakepeer --selftest)

# ---- Launcher (StrandedTogether.exe)
include(cmake/launcher.cmake OPTIONAL)

# ---- Hidden test environment (private-desktop game runs): tools/testenv
include(cmake/testenv.cmake OPTIONAL)
