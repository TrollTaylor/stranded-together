// Round 28 P1 (docs/enemy-sync.md 17): the enemy attack probe. Logs only ("[phit] ..." lines); no gameplay change. Behind
// session.ini enemy_phit_probe (default 0); the watch itself (enemy_tgt 14 1) runs in hidden runs only.
//
// Question (round 28 plan P1): how does a MULE's punch, kick, push, baton or tackle reach a body, and would it reach her partner
// body on the host? Three counting detours, installed at the first enemy_tgt 14 1 / 14 3 in a hidden run (game thread; after partner_tackle's
// start-up byte checks of the same query, which a detour would change), each byte-checked first, each its own on / off:
//   the attack sphere query 0x1429934E0 (every caller, the mod's own calls flagged by their return address in this module);
//   the attack capsule query 0x14298A3F0 (DSWeaponEntity's shape-1 strikes bypass the sphere: static RE round 28 Q1);
//   MeleeAttackEventInstance Start 0x1424BAA70 (the paired grab: its attacker and target actors after the original).
// While a watch is on each call is written to a lock-free ring (atomics only; fault-free reads: ReadProcessMemory, no __try, no
// lock, no allocation, no logging): the caller's call site (through the four sphere wrappers to their caller), the attack
// context's attacker id (P+0x20), attack id (P+0x0C), power (P+0x64) and resource default amount, the query's centre (or axis),
// radius, amount and filter layer, the thread, the query's result, and the gap between the query shape and two standing
// bodies (r 0.35 m, h 1.8 m): the partner body (realbody's live entity) and our Sam, from a seqlock'd record the tick writes.
// The game thread drains the ring every tick into per-attacker / per-site tallies and logs a bounded set of lines; the watch's
// summary ends with "[phit] summary (<why>): ..." (gate lines in tools/testenv/scenarios/two-games-enemy-attacks-probe.txt).
// Every detour calls the original with the arguments it got, always; after the emergency stop they only pass through.
//
// Round 28 E2 (docs/enemy-sync.md 20): the PHIT relay, "MULEs that punch, kick, swing a baton or tackle her actually hurt her".
// Behind session.ini enemy_partner_melee (default 1, proven in run 20261127-033043: 1 the MULE's mirror as the attacker with the
// game's own amount; 2 no attacker and the attack entry's amount, which applies with no damage in our runs), on both games, and
// refused unless both run it: the guest says hello (SCB1 type 12, kind 3, once per link session and world, on a change, then
// every 30 s while her key is on); the host relays only on a hello of the current link session.
//   The host: the same sphere / capsule detours (installed at start-up with the key, after vhit's checks; byte-checked) test
//   every MULE melee query (0x16D..0x171; not the mod's own calls) against her partner body's capsule; one that reaches it
//   (gap <= 0) goes to a second lock-free ring. The tick drains it: one PHIT per MULE + attack within 300 ms, at most 3 per MULE
//   and 8 in all in any 1 s, not for a MULE the guest owns or a frozen copy; SCB1 type 12 to the guest ("[phit] PHIT sent #n").
//   The guest: her real Sam is struck by that attack (ecombat::PhitApply: the attack context of the MULE's mirror and that attack
//   id, the game's attack query at her chest keeping only her Sam), unless she moved more than 4 m since the host saw it
//   ("[phit] PHIT dodged"), is down, in a vehicle, a cutscene or a load. Her health is watched for 1 s after
//   ("[phit] PHIT applied #n ... health a -> b (hurt)").
// Redirect bit 64 (enemy_melee_face, enemy_target.cpp) aims the MULE melee state at her body when its pick is her; it needs this
// relay armed (ephit::RelayArmed).
#pragma once

#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "enemy_combat.hpp"

namespace st::ephit {

void Install(const Config& c) noexcept;  // dllmain (the keys; with enemy_partner_melee the sphere / capsule / grab detours go in here)
// player_sync, after etarget::Tick (game thread): the position record, the ring drain, the watch's lines and its end; the relay.
void Tick(const ecombat::TickIn& in, double now) noexcept;
// enemy_tgt 14 <op> [s] (etarget::Command forwards op 14; game thread): 1 [s] the watch on for s seconds (default 60, at most
// 300, a longer one capped at 300; the counters reset; the hooks go in first), 0 the watch off and its summary, 2 the summary
// now (the watch goes on), 3 the status line (in a hidden run the hooks go in first, if not yet; elsewhere the byte checks and
// the status only: no hook goes in mid-game outside hidden runs). Op 3 in any session with the key; 0..2 hidden runs only.
// E2 (enemy_partner_melee; hidden runs only): 5 <m> the dodge test (the guest: each PHIT counts her as moved m more; 0 ends it),
// 6 <attack> the local apply test (the guest: a PHIT of that attack from the newest enemy_look's MULE, applied as if received),
// 7 the relay's status line, 8 <0|1|2> the apply mode for this run (-1 back to the key).
bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept;
// The mod's emergency stop (any thread): every detour passes through from now on.
void EmergencyStop() noexcept;

// E2: enemy_combat's DrainCombat hands every SCB1 type-12 message here (game thread; `guids` this game's fingerprint).
void OnScb(const std::uint8_t* d, std::size_t n, std::uint32_t guids) noexcept;
// E2 (any thread, atomics): the host relays now (its key, the guest's hello within 6 s, the link, a query detour in).
bool RelayArmed() noexcept;
// E2 (any thread, atomics): `ctx` is the attack context of this game's own PHIT apply (within 2 s of it). enemy_sync's
// MirrorAttackerFilter keeps such a hit on our Sam although its attacker is a frozen mirror (INTEGRATION, round 28 E2).
bool IsOwnApply(std::uintptr_t ctx) noexcept;

}  // namespace st::ephit

namespace st::ecombat {
// E2 (enemy_combat.cpp; game thread): the attack `attack` of `owner` (0 none) struck at `centre` (radius, amount: -1 the game's
// own) through vhit::QueryHit (filter `filter`'s bodies excluded), every receiver but our Sam `sam` refused. `before` runs with
// the new context before the query (the exemption's record). 1 delivered to our Sam, 0 refused (out.step), -1 a fault.
struct PhitApplyArgs {
    std::uintptr_t owner = 0, filter = 0, sam = 0;
    std::uint16_t attack = 0;
    double centre[3] = {};
    float radius = 0.5f, amount = -1.0f;
    void (*before)(std::uintptr_t ctx) = nullptr;
};
struct PhitApplyOut {
    std::uintptr_t ctx = 0, res = 0;
    int step = 0;  // 0 off (enemy hits' apply checks / stopped), 1 no context, 2 not an attack context, 3 no attack entry,
                   // 7 the query (its checks), 8 the query held no part of our Sam
    unsigned seen = 0, kept = 0;
    std::uint32_t part = 0xFFFFFFFFu;
    double hit[3] = {};
};
int PhitApply(const PhitApplyArgs& a, PhitApplyOut& o) noexcept;
// The attack registry entry's +0x28 bits (phrules::PhitAmount); 0 when there is no entry.
std::uint32_t PhitEntry28(std::uint16_t attack) noexcept;
}  // namespace st::ecombat
