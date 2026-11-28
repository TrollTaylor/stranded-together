cmake_minimum_required(VERSION 3.20)
project(stranded_together VERSION 0.45.0 LANGUAGES C CXX)

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
      # (v0.21: also the shared-vehicle frames, the ride messages and STH1: a new DRIVE flag or ride
      # REQUEST reason passed HELLO between two dirty builds of one commit and then every such frame was dropped)
      set(_st_wire_files native/mod/pose_wire.hpp native/mod/pose_wire.cpp native/include/stcoop/link_protocol.hpp
                         native/net/link_protocol.cpp native/mod/save_xfer.hpp native/mod/save_xfer.cpp
                         native/mod/vehicle_wire.hpp native/mod/vehicle_wire.cpp native/mod/ride_wire.hpp native/mod/ride_wire.cpp
                         native/mod/tackle_rules.hpp native/mod/tackle_rules.cpp native/mod/world_wire.hpp native/mod/world_wire.cpp
                         native/mod/world_rules.hpp native/mod/texture_wire.hpp native/mod/texture_wire.cpp
                         native/mod/face_rules.hpp native/mod/shot_wire.hpp native/mod/shot_wire.cpp
                         native/mod/enemy_wire.hpp native/mod/enemy_wire.cpp)
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
  native/mod/overlay/mouse_lock.cpp
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
  native/mod/enemy_census.cpp
  native/mod/body_table.cpp
  native/mod/body_choice.cpp
  native/mod/real_body.cpp
  native/mod/face_sync.cpp
  native/mod/partner_outfit.cpp
  native/mod/suit_colour.cpp
  native/mod/fragile_textures.cpp
  native/mod/texture_looks.cpp
  native/mod/texture_wire.cpp
  native/mod/texture_share.cpp
  native/mod/nametags.cpp
  native/mod/partner_collision.cpp
  native/mod/partner_gear.cpp
  native/mod/hood_fold.cpp
  native/mod/held_items.cpp
  native/mod/bag_model.cpp
  native/mod/gear_look.cpp
  native/mod/menu_figure.cpp
  native/mod/wardrobe.cpp
  native/mod/audio_probe.cpp
  native/mod/self_voice.cpp
  native/mod/partner_voice.cpp
  native/mod/partner_sound.cpp
  native/mod/vehicle_sync.cpp
  native/mod/vehicle_wire.cpp
  native/mod/vehicle_share.cpp
  native/mod/world_wire.cpp
  native/mod/world_share.cpp
  native/mod/online_gate.cpp
  native/mod/vehicle_drive.cpp
  native/mod/ride_wire.cpp
  native/mod/ride_together.cpp
  native/mod/ride_camera.cpp
  native/mod/save_guard.cpp
  native/mod/tackle_rules.cpp
  native/mod/partner_tackle.cpp
  native/mod/sam_damage.cpp
  native/mod/shot_wire.cpp
  native/mod/shot_sync.cpp
  native/mod/enemy_sync.cpp
  native/mod/enemy_combat.cpp
  native/mod/enemy_l1_probe.cpp
  native/mod/bt_sync.cpp
  native/mod/bt_world.cpp
  native/mod/bt_probe_r31.cpp
  native/mod/bt_markers.cpp
  native/mod/bt_wx.cpp
  native/mod/bt_hunt.cpp
  native/mod/bt_tar.cpp
  native/mod/bt_mirror.cpp
  native/mod/bt_void.cpp
  native/mod/bt_catcher.cpp
  native/mod/bt_extra.cpp
  native/mod/enemy_sense_hooks.cpp
  native/mod/odradek_sync.cpp
  native/mod/enemy_target.cpp
  native/mod/truck_v3.cpp
  native/mod/entity_remove_hook.cpp
  native/mod/enemy_phit.cpp
  native/mod/seh_guard.cpp
  native/mod/enemy_wire.cpp
  native/mod/npc_damage.cpp
  native/mod/save_xfer.cpp
  native/mod/save_xfer_io.cpp
  native/mod/coop_session.cpp
  native/mod/title_wait.cpp
  native/mod/dev_tools.cpp
  native/mod/quickstart.cpp
  native/mod/story_state.cpp
  native/mod/automation.cpp

  native/mod/sandbox.cpp
  ${ST_OVERLAY_SOURCES})
set_target_properties(st_mod PROPERTIES OUTPUT_NAME winhttp PREFIX "")
target_include_directories(st_mod PRIVATE native/mod)
target_compile_definitions(st_mod PRIVATE "ST_BUILD_ID=\"${ST_BUILD_ID}\"" UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(st_mod PRIVATE ${ST_WARNINGS})
target_link_libraries(st_mod PRIVATE st_net st_minhook user32 st_mod_imgui st_textures delayimp)
# The proxy emits its own winhttp.lib; never link the SDK one into the mod.
# Graphics DLLs load only when the debug menu is used (the game delay-loads them too).
target_link_options(st_mod PRIVATE /DELAYLOAD:dxgi.dll /DELAYLOAD:d3dcompiler_47.dll /DELAYLOAD:windowscodecs.dll /DELAYLOAD:ole32.dll
                    /DELAYLOAD:bcrypt.dll)
# A map file (winhttp.map next to the DLL) so a '[seh] ... from' caller rva resolves to a function (round 27 run 2).
target_link_options(st_mod PRIVATE /MAP)

# ---- Debug menu checks (no game, no GPU): action queue/registry, log ring,
# saved-position parsing, key names. tests/overlay_sync_stubs.cpp stands in for
# player_sync's debug accessors.
add_executable(st_overlay_tests tests/overlay_tests.cpp tests/overlay_sync_stubs.cpp
               native/mod/debug_actions.cpp native/mod/log.cpp
               native/mod/overlay/overlay_keys.cpp native/mod/overlay/overlay_positions.cpp native/mod/overlay/mouse_lock.cpp)
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

# ---- Dev give's pure part (native/mod/give_rules.hpp): the baggage the dev tools may give, the groups, the refusals, the
# weight budget, the session.ini values.
add_executable(st_give_rules_tests tests/give_rules_tests.cpp)
target_include_directories(st_give_rules_tests PRIVATE native/mod)
target_compile_options(st_give_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_give_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_give_rules_tests COMMAND st_give_rules_tests)

# ---- Story tools' pure part (native/mod/story_rules.hpp): the episodes, the episode hash (CRC-32C as the game's crc32
# instruction), the mission states, the summary line, the story-point load rule, the [Slot] text of a save.
add_executable(st_story_rules_tests tests/story_rules_tests.cpp)
target_include_directories(st_story_rules_tests PRIVATE native/mod)
target_compile_options(st_story_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_story_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_story_rules_tests COMMAND st_story_rules_tests)

# ---- Self view follow mode 4's pure part (native/mod/self_predict.hpp): the step / copy decision, the seated step, the
# consistency gate on a seat snap, the dt scale, the rotation power.
add_executable(st_self_predict_tests tests/self_predict_tests.cpp)
target_include_directories(st_self_predict_tests PRIVATE native/mod)
target_compile_options(st_self_predict_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_self_predict_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_self_predict_tests COMMAND st_self_predict_tests)

# ---- The carried passenger's seated idle pose, the pure part (native/mod/ride_pose_math.hpp): quaternions and joint rows,
# the frame alignment, the two-bone arm IK that rests the hands on the thighs.
add_executable(st_ride_pose_tests tests/ride_pose_tests.cpp)
target_include_directories(st_ride_pose_tests PRIVATE native/mod)
target_compile_options(st_ride_pose_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_ride_pose_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_ride_pose_tests COMMAND st_ride_pose_tests)

# ---- Gear props' one-frame extrapolation of the tick's placement, the pure part (native/mod/gear_extrap.hpp): the body frame,
# the history and its key, constant velocity and turn, the gates.
add_executable(st_gear_extrap_tests tests/gear_extrap_tests.cpp)
target_include_directories(st_gear_extrap_tests PRIVATE native/mod)
target_compile_options(st_gear_extrap_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_gear_extrap_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_gear_extrap_tests COMMAND st_gear_extrap_tests)

# ---- Gear that wears out, the pure part (native/mod/gear_wear.hpp): the durability's percent and log steps, the rain cover's
# resource (docs/real-partner-body.md section 28).
add_executable(st_gear_wear_tests tests/gear_wear_tests.cpp)
target_include_directories(st_gear_wear_tests PRIVATE native/mod)
target_compile_options(st_gear_wear_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_gear_wear_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_gear_wear_tests COMMAND st_gear_wear_tests)

# ---- Gear kept across loads and the private room, the pure part (native/mod/gear_keep.hpp): the BB pod under the self view
# (the game's rule, its cross-check, the fallback) and what the private room took (docs/real-partner-body.md section 35).
add_executable(st_gear_keep_tests tests/gear_keep_tests.cpp)
target_include_directories(st_gear_keep_tests PRIVATE native/mod)
target_compile_options(st_gear_keep_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_gear_keep_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_gear_keep_tests COMMAND st_gear_keep_tests)

# ---- The partner's Odradek, the pure part (native/mod/gear_odra.hpp): the part an STC1 odradek item names, the sender's presence
# rule and hold, the anchor joint, the HLP_Odradek comparison (docs/real-partner-body.md section 12.9).
add_executable(st_gear_odra_tests tests/gear_odra_tests.cpp)
target_include_directories(st_gear_odra_tests PRIVATE native/mod)
target_compile_options(st_gear_odra_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_gear_odra_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_gear_odra_tests COMMAND st_gear_odra_tests)

# ---- The partner's Odradek animates, the pure part (native/mod/odra_pose_rules.hpp, round 29 O2): the skeletons, the sender's gate
# and schedule, the routing, the playback bracket and stale rule, the arm's FK, the body on its tip, the FK check (section 12.10).
add_executable(st_odra_pose_tests tests/odra_pose_tests.cpp)
target_include_directories(st_odra_pose_tests PRIVATE native/mod)
target_compile_options(st_odra_pose_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_odra_pose_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_odra_pose_tests COMMAND st_odra_pose_tests)

# ---- Fragile's boots, the pure part (native/mod/boots_look.hpp): the style, the boots' ArtParts bits (settled, worn, her own
# boots), the boot type -> folder, Worn / Gold / Silver and the look keys (docs/real-partner-body.md section 30).
add_executable(st_boots_look_tests tests/boots_look_tests.cpp)
target_include_directories(st_boots_look_tests PRIVATE native/mod)
target_compile_options(st_boots_look_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_boots_look_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_boots_look_tests COMMAND st_boots_look_tests)

# ---- The self view in the private room, the pure part (native/mod/room_outfit_rules.hpp): room_outfit, Sam's body variant
# -> her look, when her body may change, the clothes-change window (docs/real-partner-body.md section 34).
add_executable(st_room_outfit_tests tests/room_outfit_tests.cpp)
target_include_directories(st_room_outfit_tests PRIVATE native/mod)
target_compile_options(st_room_outfit_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_room_outfit_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_room_outfit_tests COMMAND st_room_outfit_tests)
add_executable(st_enemy_census_tests tests/enemy_census_tests.cpp)
target_include_directories(st_enemy_census_tests PRIVATE native/mod)
target_compile_options(st_enemy_census_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_enemy_census_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_enemy_census_tests COMMAND st_enemy_census_tests)
add_executable(st_bt_rules_tests tests/bt_rules_tests.cpp)
target_include_directories(st_bt_rules_tests PRIVATE native/mod)
target_compile_options(st_bt_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_bt_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_bt_rules_tests COMMAND st_bt_rules_tests)
# Round 31: one pure-logic test per BT feature module (tests/bt_<x>_tests.cpp, native/mod/bt_<x>_rules.hpp), so the builders
# never share a test file.
foreach(st_bt_x wx hunt tar mirror void catcher extra)
  add_executable(st_bt_${st_bt_x}_tests tests/bt_${st_bt_x}_tests.cpp)
  target_include_directories(st_bt_${st_bt_x}_tests PRIVATE native/mod)
  target_compile_options(st_bt_${st_bt_x}_tests PRIVATE ${ST_WARNINGS})
  target_compile_definitions(st_bt_${st_bt_x}_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  add_test(NAME st_bt_${st_bt_x}_tests COMMAND st_bt_${st_bt_x}_tests)
endforeach()

# ---- Shots, the pure part (native/mod/shot_wire.*): the SFX1 / SEH1 codecs, the record flag byte, the replay set, the dedupe,
# the hand anchor, the play gate, pulls, the rate cap (docs/enemy-sync.md P3a).
add_executable(st_shot_wire_tests tests/shot_wire_tests.cpp native/mod/shot_wire.cpp)
target_include_directories(st_shot_wire_tests PRIVATE native/mod)
target_compile_options(st_shot_wire_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_shot_wire_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_shot_wire_tests COMMAND st_shot_wire_tests)

# ---- Enemy sync P1a, the pure part (native/mod/enemy_wire.*, enemy_rules.hpp; docs/enemy-sync.md 8): the SEN1 / SEP1 codecs and
# the enemy SFX1 frame; the schedule, pose slots, interpolation, band, entry, lease, the entity set, the damage policy.
add_executable(st_enemy_wire_tests tests/enemy_wire_tests.cpp native/mod/enemy_wire.cpp native/mod/pose_wire.cpp native/mod/shot_wire.cpp)
target_include_directories(st_enemy_wire_tests PRIVATE native/mod)
target_compile_options(st_enemy_wire_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_enemy_wire_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_enemy_wire_tests COMMAND st_enemy_wire_tests)
add_executable(st_enemy_rules_tests tests/enemy_rules_tests.cpp)
target_include_directories(st_enemy_rules_tests PRIVATE native/mod)
target_compile_options(st_enemy_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_enemy_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_enemy_rules_tests COMMAND st_enemy_rules_tests)
add_executable(st_vehicle_truck_rules_tests tests/vehicle_truck_rules_tests.cpp)
target_include_directories(st_vehicle_truck_rules_tests PRIVATE native/mod)
target_compile_options(st_vehicle_truck_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_vehicle_truck_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_vehicle_truck_rules_tests COMMAND st_vehicle_truck_rules_tests)
add_executable(st_truck_v3_rules_tests tests/truck_v3_rules_tests.cpp)
target_include_directories(st_truck_v3_rules_tests PRIVATE native/mod)
target_compile_options(st_truck_v3_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_truck_v3_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_truck_v3_rules_tests COMMAND st_truck_v3_rules_tests)
add_executable(st_enemy_probe_rules_tests tests/enemy_probe_rules_tests.cpp)
target_include_directories(st_enemy_probe_rules_tests PRIVATE native/mod)
target_compile_options(st_enemy_probe_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_enemy_probe_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_enemy_probe_rules_tests COMMAND st_enemy_probe_rules_tests)

# ---- Round 27 M1 (enemy_l1_rules.hpp): the L1 probe's attribution, trackers and decision
add_executable(st_enemy_l1_rules_tests tests/enemy_l1_rules_tests.cpp)
target_include_directories(st_enemy_l1_rules_tests PRIVATE native/mod)
target_compile_options(st_enemy_l1_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_enemy_l1_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_enemy_l1_rules_tests COMMAND st_enemy_l1_rules_tests)
add_executable(st_odra_rules_tests tests/odra_rules_tests.cpp)
target_include_directories(st_odra_rules_tests PRIVATE native/mod)
target_compile_options(st_odra_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_odra_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_odra_rules_tests COMMAND st_odra_rules_tests)
add_executable(st_enemy_target_rules_tests tests/enemy_target_rules_tests.cpp)
target_include_directories(st_enemy_target_rules_tests PRIVATE native/mod)
target_compile_options(st_enemy_target_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_enemy_target_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_enemy_target_rules_tests COMMAND st_enemy_target_rules_tests)
add_executable(st_enemy_phit_rules_tests tests/enemy_phit_rules_tests.cpp)
target_include_directories(st_enemy_phit_rules_tests PRIVATE native/mod)
target_compile_options(st_enemy_phit_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_enemy_phit_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_enemy_phit_rules_tests COMMAND st_enemy_phit_rules_tests)

# ---- Fragile's face (face_rules.hpp): the wire ids of Sam's facial clips, the manager's choice, when a face gets a clip
add_executable(st_face_rules_tests tests/face_rules_tests.cpp)
target_include_directories(st_face_rules_tests PRIVATE native/mod)
target_compile_options(st_face_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_face_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_face_rules_tests COMMAND st_face_rules_tests)

# ---- Sam's skeletons on Fragile, the pure part (native/mod/exo_parts.hpp): the mesh controller's formula without the self
# view's bit, the skeleton types, the exo carrier's parts plan (docs/real-partner-body.md section 32).
add_executable(st_exo_parts_tests tests/exo_parts_tests.cpp)
target_include_directories(st_exo_parts_tests PRIVATE native/mod)
target_compile_options(st_exo_parts_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_exo_parts_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_exo_parts_tests COMMAND st_exo_parts_tests)

# ---- Held items on the other screen, the pure part (native/mod/held_rules.hpp): which weapon resources become held clones in
# each held_items mode, which component resources a clone keeps, the prop keys (docs/real-partner-body.md section 31).
add_executable(st_held_rules_tests tests/held_rules_tests.cpp)
target_include_directories(st_held_rules_tests PRIVATE native/mod)
target_compile_options(st_held_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_held_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_held_rules_tests COMMAND st_held_rules_tests)

# ---- The carried cargo on the other screen, the pure part (native/mod/cargo_rules.hpp): which carried pieces are sent with
# which model byte (the hip crates as the baggage config's exact model), what the receiver draws (docs/real-partner-body.md
# section 39).
add_executable(st_cargo_rules_tests tests/cargo_rules_tests.cpp)
target_include_directories(st_cargo_rules_tests PRIVATE native/mod)
target_compile_options(st_cargo_rules_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_cargo_rules_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_cargo_rules_tests COMMAND st_cargo_rules_tests)

# ---- Fragile's porter suit in Sam's suit colours, the pure part (native/mod/suit_colour_math.hpp): the colour -> constant
# textures mapping (her own look at 0, the palette scaled to her navy, the silver / gold metals), the debug override.
add_executable(st_suit_colour_tests tests/suit_colour_tests.cpp)
target_include_directories(st_suit_colour_tests PRIVATE native/mod)
target_compile_options(st_suit_colour_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_suit_colour_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_suit_colour_tests COMMAND st_suit_colour_tests)

# ---- Shared structures (world sync), the pure part (native/mod/world_wire.*, world_rules.hpp): the SWS1 messages' strict codecs,
# the Record, the join check's bucket hashes, the binding, the guest build check, the removal classes, the online gate's verdict.
add_executable(st_world_wire_tests tests/world_wire_tests.cpp native/mod/world_wire.cpp)
target_include_directories(st_world_wire_tests PRIVATE native/mod)
target_compile_options(st_world_wire_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_world_wire_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME st_world_wire_tests COMMAND st_world_wire_tests)

# ---- Fragile's textures (docs/textures.md): st_textures = the pure pixel code, PNG (WIC), hashes, the slot table and
# the manifest (the launcher now; the mod's loader later); st_textures_extract = the read-only Decima archive reader,
# the game's Oodle loaded by full path and the extraction plan: only the launcher's --extract-textures child and the
# tests link it, never the mod.
add_library(st_textures STATIC native/textures/tex_codec.cpp native/textures/png_wic.cpp native/textures/tex_hash.cpp
            native/textures/tex_manifest.cpp native/textures/tex_blob.cpp native/textures/suit_defaults.cpp
            native/textures/logo_recipe.cpp)
target_include_directories(st_textures PUBLIC native/textures)
# the ready-made colours' palette (suit_colour_math.hpp, header-only)
target_include_directories(st_textures PRIVATE native/mod)
target_compile_options(st_textures PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_textures PRIVATE UNICODE _UNICODE)
target_link_libraries(st_textures PUBLIC bcrypt windowscodecs ole32)
add_library(st_textures_extract STATIC native/textures/decima_archive.cpp native/textures/decima_core.cpp
            native/textures/oodle_dyn.cpp native/textures/extract_plan.cpp)
# native/mod/suit_colour_math.hpp (header only): Sam's palette colours for the swatches and previews
target_include_directories(st_textures_extract PRIVATE native/mod)
target_compile_options(st_textures_extract PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_textures_extract PRIVATE UNICODE _UNICODE)
target_link_libraries(st_textures_extract PUBLIC st_textures)
# Codecs, packing, mips, manifest, slot table, PNG, the archive reader on a synthetic archive and the extraction's
# folder rules on a temp folder (no game data).
add_executable(st_texture_tests tests/texture_tests.cpp)
target_include_directories(st_texture_tests PRIVATE native/mod)
target_compile_options(st_texture_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_texture_tests PRIVATE UNICODE _UNICODE)
target_link_libraries(st_texture_tests PRIVATE st_textures_extract)
add_test(NAME st_texture_tests COMMAND st_texture_tests)
# Round 8: Fragile's ready-made suit colours (suit_defaults.hpp): golden hashes from the Python model, the folder rule.
add_executable(st_suit_defaults_tests tests/suit_defaults_tests.cpp)
target_include_directories(st_suit_defaults_tests PRIVATE native/mod)
target_compile_options(st_suit_defaults_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_suit_defaults_tests PRIVATE UNICODE _UNICODE)
target_link_libraries(st_suit_defaults_tests PRIVATE st_textures_extract)
add_test(NAME st_suit_defaults_tests COMMAND st_suit_defaults_tests)
# Round 20: the Fragile Express logo (logo_recipe.hpp, logo_slots.hpp): the resampler, the wordmark's turn, the stamps.
add_executable(st_logo_tests tests/logo_tests.cpp)
target_include_directories(st_logo_tests PRIVATE native/mod)
target_compile_options(st_logo_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_logo_tests PRIVATE UNICODE _UNICODE)
target_link_libraries(st_logo_tests PRIVATE st_textures)
add_test(NAME st_logo_tests COMMAND st_logo_tests)
# Round 6 T2: the blob (build / strict validate), the STT1 messages, the receiver's reassembly and the sender's pacing.
add_executable(st_texture_wire_tests tests/texture_wire_tests.cpp native/mod/texture_wire.cpp)
target_include_directories(st_texture_wire_tests PRIVATE native/mod)
target_compile_options(st_texture_wire_tests PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_texture_wire_tests PRIVATE UNICODE _UNICODE)
target_link_libraries(st_texture_wire_tests PRIVATE st_textures)
add_test(NAME st_texture_wire_tests COMMAND st_texture_wire_tests)
# st_textool: hidden scenarios' bold generated test patterns over a textures folder's editable pictures (no game data).
add_executable(st_textool tools/textool/textool.cpp)
target_compile_options(st_textool PRIVATE ${ST_WARNINGS})
target_compile_definitions(st_textool PRIVATE UNICODE _UNICODE)
target_link_libraries(st_textool PRIVATE st_textures)

# ---- st_fakepeer: console stand-in for the second player (UDP), + self-test
add_executable(st_fakepeer tools/fakepeer/fakepeer.cpp native/mod/link.cpp native/mod/steam_owner.cpp
               native/mod/log.cpp native/mod/pose_wire.cpp)
target_include_directories(st_fakepeer PRIVATE native/mod)
target_compile_definitions(st_fakepeer PRIVATE "ST_BUILD_ID=\"${ST_BUILD_ID}\"" _CRT_SECURE_NO_WARNINGS)
target_compile_options(st_fakepeer PRIVATE ${ST_WARNINGS})
target_link_libraries(st_fakepeer PRIVATE st_net st_minhook)
add_test(NAME st_fakepeer_selftest COMMAND st_fakepeer --selftest)

# ---- devdash (tools/devdash): parser and API tests, only when a Python 3 is found (never needed to build)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME st_devdash_tests
           COMMAND ${Python3_EXECUTABLE} -B -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tools/devdash" -p "test_*.py")
  # the round 27 enemy-target gate's verdict rules on a synthetic run-3 fixture (tests/fixtures/enemy-target-gate)
  add_test(NAME st_enemy_target_gate_tests
           COMMAND ${Python3_EXECUTABLE} -I -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/enemy_target_gate_test.py")
endif()

# ---- Launcher (StrandedTogether.exe)
include(cmake/launcher.cmake OPTIONAL)

# ---- Hidden test environment (private-desktop game runs): tools/testenv
include(cmake/testenv.cmake OPTIONAL)
