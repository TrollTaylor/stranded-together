// Shots (docs/enemy-sync.md P3a, "ship A"): a player's gunfire shown on the other screen with the game's own muzzle flash,
// trail and impacts. The shooter's game catches every bullet record its Sam's weapon enqueues (a detour on the bullet system's
// Enqueue 0x142916860, capture only), sends each shot twice in SFX1 frames (shot_wire.hpp); the other game holds it until the
// partner body plays that moment and replays it through the game's own CreateBullet (0x14294AD40) from the partner body's
// gun, with the shooter's weapon id (the muzzle flash) and magazine id (the magazine's effect). Replayed bullets never damage
// the receiver's Sam or Humanoids: their attack references go into the replay set, and the shared Sam damage detour
// (sam_damage.hpp) and a detour on the Humanoid MsgDamage handler (0x1437393D0, MULEs / Demens) drop their hits (vehicles,
// cargo, structures and BTs are not filtered). Every value the game reads under the bullet system's lock (the category, the
// attack id, +0xD1) is checked on the receiver first: a fault there cannot be recovered. session.ini shots_sync (default off;
// see config.hpp), shots_muzzle, shots_sound, shots_friendly_fire, shots_npc_drop.
//
// R1 also lives here: the SEH1 combat beacon (both games say which combat parts they run), and the counters the enemy probe
// reads (shots by an owner entity). Every site is byte-checked before use; every call SEH-guarded; the first fault stops it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"

namespace st::shots {

// Start-up (dllmain, before the tick): the byte checks, the Enqueue detour and the damage filters when shots_sync is on (or a
// hidden run may turn it on). Hooks go in now only (enabling one suspends every thread).
void Install(const Config& config) noexcept;
bool Active() noexcept;             // capture, send and replay on now
const char* OffReason() noexcept;   // "" when on, or not wanted (shots_sync=0)

struct TickIn {
    game::Entity* local = nullptr;  // our Sam
    bool local_ok = false;
    game::Entity* body = nullptr;   // the partner body (real or hologram), nullptr when none
    bool have_play = false;         // the pose play time (the partner's unwrapped sender ms)
    double play_ms = 0;
    bool net = false;               // a host / guest link (not solo)
    std::uint8_t role = 0;          // 1 host, 2 guest, 0 solo
};
void Tick(const TickIn& in, double now) noexcept;  // game thread

// Debug actions (docs/debug-actions.md "Shots"); game thread. op: 0 status, 1 on, 2 off, 3 <m> <deg> (hidden runs) our Sam m from the
// partner body, deg around its heading (0 in front, 90 its right), facing it (the topmost ground there); 4 [deg] (hidden runs) the
// game camera checked every tick until it looks at the partner body within deg (default 12): "[shots] camera aimed #n".
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// shots_test <dist_m> <flags> [count]: CreateBullet from dist m ahead of our Sam toward his chest through the replay path, with
// the last captured shot's attack / weapon / magazine ids. flags: 1 the filter keeps it (proof under dev_god 0), 2 no weapon id,
// 4 no magazine id, 16 a leak test: `count` replays (<= 400, 4 a tick) into the ground 12 m ahead, away from Sam, with the
// process's working set logged before and after.
bool Test(double dist_m, int flags, int count, char* msg, std::size_t cap) noexcept;
// shots_echo <0|1> [offset_m]: solo: our own captured shots also replayed here from offset_m (default 2.5) to the right.
bool Echo(bool on, double offset_m, char* msg, std::size_t cap) noexcept;

// The enemy probe (enemy_sync.cpp): bullets enqueued by an owner entity while armed (a table of 16).
void ArmOwnerCounts(bool on) noexcept;
unsigned ShotsBy(std::uintptr_t entity) noexcept;
bool EnqueueHooked() noexcept;

}  // namespace st::shots
