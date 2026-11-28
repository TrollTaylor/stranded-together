// Round 31 stage 0 (docs/enemy-sync.md section 25): what bt_probe_r31.cpp borrows from bt_sync.cpp. Each one is a thin wrapper
// (defined at the end of bt_sync.cpp) around the anonymous-namespace helper of the same name, so the round 31 probes reuse the
// census, the byte-checked hooks and the play gates instead of copying them. Game thread only (the Tick and the action).
#pragma once

#include <cstddef>
#include <cstdint>

namespace st::bt::internal {

double NowMs() noexcept;                                           // the last BT tick's clock
bool LocalPos(double (&out)[3]) noexcept;                          // the local Sam's position (false: no world)
bool SamFacing(double (&me)[3], double* fx, double* fy) noexcept;  // and his facing (the transform's forward row)
bool LoadingNow() noexcept;                                        // no live player or a load in progress
const char* PlayingWhy() noexcept;                                 // nullptr: played; else why not (asleep, a menu, a scene)

// One gazer of the census (TakeCensus): the gazer list's actors, resolved through the registry.
struct GzInfo {
    std::uintptr_t actor = 0;
    std::uint32_t id = 0, loc = 0, field = 0, flags = 0;
    std::int32_t state = -1;
    float pdist = -1, trig = -1;  // +0x204 the game's distance to the local player, [+0x388] +0x98 the near radius
    bool located = false, pos_ok = false;
    double pos[3] = {0, 0, 0};
};
// The gazers now (at most cap; resolved actors only). -1: the gazer list or its fields failed bt_sync's byte checks.
int Gazers(GzInfo* out, int cap) noexcept;
bool GazerKillOk() noexcept;  // bt_probe 10's checks (the gazer update hook and its kill) passed
bool VoidOk() noexcept;       // bt_probe 11's checks (the voidout FUN_14299B1B0 and the GameOver flow) passed
// The voidout FUN_14299B1B0's prologue as shipped, also with bt_sync's own voidout hook in (bt_sync >= 1): MinHook's jmp to its relay
// and the detour checked, the bytes it moved compared in the trampoline, the rest in place. Stage 0: two-games had "void OFF".
bool VoidPrologueOk() noexcept;
bool VoidHookIn() noexcept;  // bt_sync's own voidout hook is in (a call to FUN_14299B1B0 is recorded and relayed as a VOID)
bool InstallGz(char* msg, std::size_t cap) noexcept;  // bt_world's gazer update hook in (byte-checked)
bool MhInit(char* msg, std::size_t cap) noexcept;     // MinHook initialised (once for every module)
// The GameOver flow gate FUN_14341F840 (bt_probe 11's): in after this (true), it counts every call through it and drops one only
// inside bt_probe 11's guarded crater.
bool FlowGate(char* msg, std::size_t cap) noexcept;
unsigned FlowCalls() noexcept;  // calls through the gate since it went in (dropped ones included)

}  // namespace st::bt::internal
