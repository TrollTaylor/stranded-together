// One shared detour on our Sam's MsgDamage handler (DSPlayerEntity 0x14271BC00; docs/enemy-sync.md P3, plan R1): the vehicle
// hits (partner_tackle.cpp) observe and cap through it, the shots (shot_sync.cpp) drop the damage of their own replays, and
// the enemy rounds will register their filters here. One MinHook for all of them, created at start-up only (enabling a hook
// suspends every thread) by the first client that asks (Ensure); byte-checked first, with an off switch: a mismatch leaves it
// uninstalled (`[samdmg] off: ...`) and every client degrades as it says.
//
// The handler (0x14271BC00, void(entity, msg)): when the entity's damage component accepts the message's DamageInfo
// (msg + 0x20) it applies it (0x142352730), else it marks the message handled (msg + 8 = 1). A dropped message is handled
// the same way: msg + 8 = 1 and the original is not called.
#pragma once

#include <cstddef>
#include <cstdint>

namespace st::samdmg {

struct View {
    std::uintptr_t sam = 0, msg = 0;
    std::uintptr_t di = 0;        // msg + 0x20: the DamageInfo
    std::uintptr_t ref = 0;       // Q(di + 0x28): the attack reference
    std::uintptr_t ctx = 0;       // Q(di + 0x40): the attack context
    std::uintptr_t attacker = 0;  // Q(ctx + 0x68) - 0x20: the attacking entity (0 when unreadable)
    float amount = 0;             // di + 0x78
    double hit[3] = {};           // di + 0x80
};
enum class Verdict : std::uint8_t { keep, drop };
// Observers run first, in registration order, on the handler's thread (worker threads: atomics and fault-free reads only);
// they may change the message (the vehicle hits cap its amount). Filters then run in registration order; the first drop
// wins. Both run inside the detour's SEH guard.
using Observer = void (*)(std::uintptr_t sam, std::uintptr_t msg);
using Filter = Verdict (*)(const View& v);

// Start-up only (dllmain's install phase). Creates and enables the detour once; true when it is in. `who` names the client
// for the log.
bool Ensure(const char* who) noexcept;
bool Installed() noexcept;
const char* OffReason() noexcept;  // "" when installed
// At most 4 of each; false when full or the detour is not in.
bool Observe(Observer o) noexcept;
bool Register(Filter f, const char* name) noexcept;
// Counters: messages seen, dropped per filter (registration order), faults in a client.
void Counts(unsigned* seen, unsigned (&dropped)[4], unsigned* faults) noexcept;
// The readers the clients share (fault-free).
View Read(std::uintptr_t sam, std::uintptr_t msg) noexcept;

}  // namespace st::samdmg
