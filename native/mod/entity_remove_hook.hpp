// Round 27 M4, the destroy guard (docs/enemy-sync.md 16.2): a detour on World::RemoveEntityNow (0x14237B390; rcx the world, rdx the
// entity, r8d a flag), the game's own removal of an entity, which marks it (+0x88 bit 31) and then appends it to the destroy list
// (0x14237B7AB): the hook runs before either, so a watched entity is seen BEFORE it can be freed. Two watch slots: real_body's
// partner entity (kWatchBody) and the partner stimulus' body (kWatchStim, enemy_target: its stimulus parked from the detour).
// The detour runs on any thread, possibly with the world's lock held: atomics, a TEB read and etarget::GameRemoving (lock-free,
// SEH reads and atomic stores only) -- no lock, no allocation, no logging, no ReadProcessMemory. Installed on the host only (the
// caller decides), once, after its byte checks.
#pragma once

#include <cstdint>

namespace st::rmhook {

enum : int { kWatchBody = 0, kWatchStim = 1 };
bool Install() noexcept;       // game thread; the byte checks, then MinHook; idempotent (one try per run)
bool Ready() noexcept;         // hooked, enabled, and every byte check passed
const char* State() noexcept;  // "on", "off (byte checks: <name>)", "off (MinHook <status>)", "not installed"
void Counts(unsigned* ok, unsigned* total) noexcept;  // the byte checks passed / made (0 / 0 before Install)
void Watch(int slot, std::uintptr_t e) noexcept;      // game thread; 0 clears it (a new entity clears the slot's removed mark)
bool Removed(int slot, std::uintptr_t e) noexcept;    // the game began removing `e` (sticky until Watch changes the slot); any thread
struct Stats {
    unsigned calls = 0, matched = 0;
    std::uint32_t last_tid = 0;
    bool last_sim = false;
};
Stats Snapshot() noexcept;
// op 12 (hidden runs): the detour's match path for `e` on kWatchStim only (never kWatchBody: real_body keeps a live entity),
// without the original
void Simulate(std::uintptr_t e) noexcept;
// op 13 (hidden runs): [0x147BC4E70] (the world) when its checks passed (its RIP reference in the child loop, the world vtable's
// +0x20, and [world] the vtable); 0 otherwise
std::uintptr_t WorldGlobal() noexcept;
// op 13: world->vt+0x20(world, e, 0, 0) (the child loop's own removal call) under SEH, on the game thread. 1 called, 0 refused
// (no world / its checks), -1 a fault
int TestRemove(std::uintptr_t e) noexcept;

}  // namespace st::rmhook
