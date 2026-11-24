// One shared detour on the Humanoid MsgDamage handler (MULEs, Demens, warriors: 0x1437393D0, reached through the vtable thunk
// 0x143744380; docs/enemy-sync.md 8), the sam_damage pattern for this world's NPCs: the shots (shot_sync.cpp) drop the hits of
// their replays here as a backstop (the drop at the bullet is the main one), and enemy sync (enemy_sync.cpp) drops every hit on a
// frozen mirror (P1a: the host decides; P2 turns our own hits into HIT messages). One MinHook for all of them, created at start-up
// only (enabling a hook suspends every thread) by the first client that asks (Ensure); byte-checked first: a mismatch leaves it
// uninstalled and every client degrades as it says.
//
// The handler is void(receiver, msg) [I: rcx is the receiving entity, as for Sam's 0x14271BC00; the enemy filter counts how its
// value matches a mirror]. A dropped message is handled as the handler does with one it refuses: msg + 8 = 1, the original not
// called.
#pragma once

#include <cstddef>
#include <cstdint>

#include "sam_damage.hpp"

namespace st::npcdmg {

enum class Verdict : std::uint8_t { keep, drop };
// Filters run in registration order on the handler's thread (worker threads: atomics and fault-free reads only), inside the
// detour's SEH guard; the first drop wins. `v` is samdmg::Read(receiver, msg) (its `sam` field is the receiver).
using Filter = Verdict (*)(std::uintptr_t receiver, std::uintptr_t msg, const samdmg::View& v);

// Start-up only. Creates and enables the detour once; true when it is in. `who` names the client for the log.
bool Ensure(const char* who) noexcept;
bool Installed() noexcept;
const char* OffReason() noexcept;  // "" when installed
// At most 4; false when full or the detour is not in.
bool Register(Filter f, const char* name) noexcept;
// Messages seen, dropped per filter (registration order), faults in a filter.
void Counts(unsigned* seen, unsigned (&dropped)[4], unsigned* faults) noexcept;

}  // namespace st::npcdmg
