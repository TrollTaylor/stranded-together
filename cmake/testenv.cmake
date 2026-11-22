# Hidden test environment (docs/hidden-testing.md): Death Stranding on a private,
# never-displayed Windows desktop, driven through the mod's automation channel.
#
#   st_testenv_core         files/hashes, scenario language, game-folder backup/restore
#   st_testenv_supervisor   creates the private desktop + job, runs the probe or the game,
#                           feeds scenarios, enforces timeouts, collects, restores (GUI subsystem)
#   st_testenv_probe        D3D12 on a private desktop: present rate, occlusion, capture
#   st_testenv_fakegame     stand-in ds.exe for the end-to-end ctest (loads the real mod)
#   st_testenv_tests        pure checks + the --e2e driver
#
# ctests: st_testenv_tests (pure), st_testenv_probe_private and st_testenv_e2e (both
# create a private desktop and need a D3D12 adapter or WARP; they share a resource
# lock because the supervisor allows one run at a time). None of them touches the
# real game folder.
# Included from the root CMakeLists.txt. Only for MSVC on Windows.
if(NOT (WIN32 AND MSVC))
  return()
endif()
if(NOT TARGET st_mod OR NOT TARGET st_mod_imgui)
  message(WARNING "testenv.cmake: st_mod / st_mod_imgui missing; the hidden test environment is not built")
  return()
endif()

set(_st_te "${CMAKE_CURRENT_SOURCE_DIR}/native/testenv")
set(_st_te_defs UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)

add_library(st_testenv_core STATIC ${_st_te}/testenv_core.cpp)
target_include_directories(st_testenv_core PUBLIC ${_st_te} ${CMAKE_CURRENT_SOURCE_DIR}/native/mod)
target_compile_definitions(st_testenv_core PUBLIC ${_st_te_defs})
target_compile_options(st_testenv_core PRIVATE ${ST_WARNINGS})
target_link_libraries(st_testenv_core PUBLIC bcrypt)

add_executable(st_testenv_supervisor WIN32 ${_st_te}/supervisor.cpp)
target_compile_options(st_testenv_supervisor PRIVATE ${ST_WARNINGS})
target_link_libraries(st_testenv_supervisor PRIVATE st_testenv_core user32 shell32 pdh)

# The probe captures with the mod's own code (overlay_renderer.cpp).
add_executable(st_testenv_probe WIN32 ${_st_te}/probe.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/native/mod/overlay/overlay_renderer.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/native/mod/log.cpp)
target_include_directories(st_testenv_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/native/mod/overlay)
target_compile_options(st_testenv_probe PRIVATE ${ST_WARNINGS})
target_link_libraries(st_testenv_probe PRIVATE st_testenv_core st_mod_imgui d3d12 dxgi user32 shell32)

add_executable(st_testenv_fakegame WIN32 ${_st_te}/fakegame.cpp)
target_compile_definitions(st_testenv_fakegame PRIVATE ${_st_te_defs})
target_compile_options(st_testenv_fakegame PRIVATE ${ST_WARNINGS})
target_link_libraries(st_testenv_fakegame PRIVATE d3d12 dxgi winhttp user32)

add_executable(st_testenv_tests ${_st_te}/testenv_tests.cpp)
target_compile_options(st_testenv_tests PRIVATE ${ST_WARNINGS})
target_link_libraries(st_testenv_tests PRIVATE st_testenv_core windowscodecs ole32)
add_dependencies(st_testenv_tests st_testenv_supervisor st_testenv_probe st_testenv_fakegame st_mod)

set(_st_te_work "${CMAKE_CURRENT_BINARY_DIR}/testenv-work")
add_test(NAME st_testenv_tests
         COMMAND st_testenv_tests --scenarios "${CMAKE_CURRENT_SOURCE_DIR}/tools/testenv/scenarios" --work "${_st_te_work}")
add_test(NAME st_testenv_probe_private
         COMMAND st_testenv_supervisor --probe $<TARGET_FILE:st_testenv_probe> --run-dir "${_st_te_work}/probe" --seconds 4)
add_test(NAME st_testenv_e2e
         COMMAND st_testenv_tests --e2e --supervisor $<TARGET_FILE:st_testenv_supervisor>
                 --fakegame $<TARGET_FILE:st_testenv_fakegame> --mod $<TARGET_FILE:st_mod>
                 --scenario "${CMAKE_CURRENT_SOURCE_DIR}/tools/testenv/scenarios/fake-smoke.txt" --work "${_st_te_work}")
set_tests_properties(st_testenv_probe_private st_testenv_e2e PROPERTIES RESOURCE_LOCK st_testenv_desktop TIMEOUT 300)
set_tests_properties(st_testenv_e2e PROPERTIES SKIP_RETURN_CODE 77)

# Prune-TestRuns.ps1 (old runs deleted before each run) on a fake runs folder in %TEMP%, when PowerShell 7 is found.
find_program(ST_PWSH pwsh)
if(ST_PWSH)
  add_test(NAME st_testenv_prune_tests
           COMMAND "${ST_PWSH}" -NoProfile -NonInteractive -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/testenv/Test-PruneTestRuns.ps1")
  set_tests_properties(st_testenv_prune_tests PROPERTIES RESOURCE_LOCK st_testenv_desktop)
endif()
