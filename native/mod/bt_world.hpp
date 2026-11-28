// Round 30 BT S2 (docs/enemy-sync.md section 24): the gazer update hook, shared by the kill relay (bt_kill) and the stage-0 probes
// (bt_probe 9 / 10). ONE MinHook detour on the gazer update FUN_142995210 (ds.exe DC, reached only through the vtable slot
// 0x143DA5848), installed lazily on the game thread and byte-checked first. The detour does two jobs and nothing else (atomics,
// guarded reads and the guarded four-store kill only; no allocation, no logging, no other game call):
//   apply  before the original: a kill armed for `this` (ArmKill) is applied as the update's own rain-gone kill (0x1429957C8:
//          +0xCC reason 0x01322187, +0xD0 = +0x08, +0x180 = 0, +0x1B4 |= 8), guarded by btrules::KillGuard;
//   detect after the original: the death edge (the last update's health +0x2B0 above 0 before the call, +0x180 at or below 0 after
//          it) goes into a lock-free ring of 32 {actor, id, +0xCC reason, +0xD0 killer, +0x1B4, position, tag}.
// The game thread drains the ring (TakeEvents) and maps an actor to its locator itself (bt_sync.cpp).
// Also two guarded game reads for the receiver's gate: FUN_14299C1A0 ("BT gameplay allowed", a pure query of globals) and the
// actor registry's local player id (vt+0x40), each byte-checked at its call site in the gazer update.
#pragma once
#include <cstddef>
#include <cstdint>

namespace st::btworld {

// The byte checks (first call; logged once, "[bt] world byte checks: n/m ok ...").
enum Bit : unsigned {
    kBitHook = 1u,     // the update's prologue and vtable slot: the hook may go in
    kBitKill = 2u,     // the update's own kill, its death edge and its +0x2B0 copy: the apply and the detect are what the game does
    kBitAllowed = 4u,  // FUN_14299C1A0 and its call site in the update
    kBitPlayer = 8u,   // the registry's local player id (vt+0x40) as the update reads it
    kBitScene = 16u,   // the update's +0x3A8 read (the locator's scene)
};
unsigned Check() noexcept;
// Game thread. Installs the hook once (byte checks first). `why` names the caller in the log line. False with *msg on failure.
bool InstallHook(const char* why, char* msg, std::size_t cap) noexcept;
bool HookIn() noexcept;

// Game thread: arms a kill for one gazer actor (applied at its next update, before the original). tag: btrules::KillTag.
// `id`: its actor id (+0x08) now; review (crash 3): the update kills it only while +0x08 still reads that (the actor freed and
// another made at that address in between: applied = kApplyStale, nothing written).
// False when the hook is out, all 8 slots are armed or that actor is armed already.
// Round 31 (bt_probe 36 kill_look): `reason` the +0xCC reason written (0 = the rain-gone 0x01322187, the default) and `killer`
// the +0xD0 killer id (0 = the gazer's own id, the default; kKillerZero writes 0: the local Sam's id is 0, stage 0); every other
// caller keeps the round 30 kill. `allow_realized`: KillGuard also takes a realized copy (state 2; flags mask 0x8) for this slot;
// only bt_sync's partner-kill arm with the kill look on (bt_kill_look=1) passes true; bt_probe 36 and every other caller false.
constexpr std::uint32_t kKillerZero = 0xFFFFFFFFu;
bool ArmKill(std::uintptr_t actor, std::uint32_t id, std::uint8_t tag, std::uint32_t reason = 0, std::uint32_t killer = 0,
             bool allow_realized = false) noexcept;
// Game thread: takes an armed kill back. True when it was still pending (the update never took it).
bool Disarm(std::uintptr_t actor) noexcept;
bool Armed(std::uintptr_t actor) noexcept;

// One event from the detour: a death edge, or an armed kill's outcome (applied >= 0: KillGuard's result, -1 the kill faulted;
// -2 not an armed kill, -3 another gazer at that address: its id changed). For an applied kill (result kKillOk) edge is true too.
struct Event {
    std::uintptr_t actor = 0;
    std::uint32_t id = 0, reason = 0, killer = 0, flags = 0;
    float prev_after = 0;  // +0x2B0 after the original (0 when the update ran to its end after a kill)
    double pos[3] = {};
    bool edge = false, pos_ok = false, read_ok = false;
    std::uint8_t tag = 0;
    int applied = -2;
};
constexpr int kNotArmed = -2, kApplyFault = -1, kApplyStale = -3;
// Game thread: the events since the last call (oldest first), up to cap. *lost: events dropped since the last call (ring full).
unsigned TakeEvents(Event* out, unsigned cap, unsigned* lost) noexcept;

// Game thread, guarded game calls: FUN_14299C1A0 (1 BT gameplay allowed, 0 not, -1 unknown / unchecked / faulted).
int BtAllowed() noexcept;
// The registry's local player actor id. False: unchecked, unreadable or faulted.
bool PlayerId(std::uint32_t* out) noexcept;
// The gazer's +0x3A8, the locator's scene (FUN_142994B40 sets it to [[actor+0x18]+0x80]), read safely. Logged only.
// False: unchecked or unreadable.
bool SceneOwner(std::uintptr_t actor, std::uintptr_t* out) noexcept;

// P1 (bt_probe 9): the update's calls on the Tick thread and on others, the distinct threads (+ more than 8).
void NoteTickThread() noexcept;
void Threads(unsigned* tick, unsigned* other, unsigned* threads, bool* over) noexcept;

// Round 31 (bt_probe 22 gz_mirror): one callback run inside the detour right after the original update, on the game thread, for
// every gazer updated (nullptr: none). It must be quick, take guarded writes only and never log. EmergencyStop clears it.
// Round 31 features: one slot per user, called in slot order (the probe's, bt_mirror's, bt_extra's); each user sets and clears
// only its own slot. The one-argument form is the probe's slot (bt_probe_r31.cpp).
using AfterUpdateFn = void (*)(std::uintptr_t actor) noexcept;
enum AfterSlot : unsigned { kAfterProbe = 0, kAfterMirror = 1, kAfterExtra = 2, kAfterSlots = 3 };
void SetAfterUpdate(AfterUpdateFn fn) noexcept;
void SetAfterUpdate(AfterSlot slot, AfterUpdateFn fn) noexcept;

// EmergencyStop: the detour goes straight through; armed kills dropped.
void EmergencyStop() noexcept;

}  // namespace st::btworld
