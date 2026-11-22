// Shared structures (world sync, milestones W1 + W2 + W3; docs/world-sync.md). session.ini world_sync=1 with host_save=1.
//
// "Each structure is a real one in each world." A structure either player builds with the PCC becomes a real own
// (Player-type) construction point in BOTH worlds, with the game's own build-up and printer on both screens, and stays in
// the host's save. The host's world decides everything; the guest's world is a disposable mirror (the host-held save).
//
//   host builds    the game's own commit runs untouched (its EnqueueInfo is watched, its SetLastBuilt gives the printer's
//                  place); the census sees the new CP and sends ADD {its record, the printer's place}; the guest mirrors it
//                  with the same id (exact, else a mapped id) and the build-up / printer play on her screen.
//   guest builds   her commit runs (the PCC item really leaves her inventory) but its EnqueueInfo is HELD and its
//                  SetLastBuilt deferred; GBUILD goes to the host, which creates the structure (offline bit, build-up,
//                  printer at her PCC) and answers ADD with her request id; she then mirrors it with the host's id.
//   join / load    the guest loaded the host's save, so the saved structures share ids: SUM (64 bucket hashes) ->
//                  BUCKET_REQ -> DIGEST -> bind / NEED / remove; CHECK every 2 s heals any drift.
//   removals       the host's census classifies each disappearance (manual, evicted, decay, area) and sends REM before the
//                  tick's ADDs; a guest dismantle runs locally and sends REMREQ (the host applies RemoveManually).
//   contributions  (W2) each player pays at their own terminal from their own inventory (native). The guest's AddOwn /
//                  AddRepair run in her world and go to the host as MATS; her own level-up request is swallowed. The host
//                  applies MATS with the game's AddOthers (the silent "others contributed" bucket), the game's Status decides
//                  the level-up (QueueLevelUp), and STATE (absolute: own, invested, repair, durability, level-up) makes her
//                  numbers equal and her level-up follow. One shared total; each level-up plays once in each world. DUR
//                  batches the durabilities (timefall) every 5 s.
//   ladders, ropes (W3) the same pipeline (the player's commit enqueues them like a PCC structure; no printer). A pick-up
//                  (the item goes back into the player's inventory) is a removal: the host's -> REM pickup, the guest's ->
//                  REMREQ pickup (the host removes its copy with the removal bits: no item for the host).
//
// Never uploads: the mod's structures carry the game's offline bit (record +0x62E -> cp+0xAB bit 7; createObject refuses
// them), and online_gate.hpp drops the online service's calls for them (everything on the guest; everything in hidden runs).
//
// Every game address is in a start-up byte-check table (kSites in world_share.cpp); a mismatch turns world sync off with a
// one-line reason. Game calls run on the game thread (Tick, Command) under SEH; the detours only copy and pass.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"

namespace st::wshare {

// Bootstrap thread, after MH_Initialize and gate::Install, before the frame pump is hooked: decides the mode, runs the byte
// checks (for the read-only tools too) and installs the detours. No-op for the sync unless world_sync=1, host_save=1 and
// the role is host or guest.
void Install(const Config& config) noexcept;
// The sync is on in this game.
bool Active() noexcept;
const char* OffReason() noexcept;

struct TickIn {
    game::Entity* local = nullptr;
    bool local_ok = false;
    bool have_partner = false;           // the partner body's place this tick
    game::WorldTransform partner{};
};
// Game thread, every tick (sync::Tick).
void Tick(const TickIn& in, double now_ms) noexcept;
// The per-frame tick faulted: the detours pass everything through, nothing more is sent. Any thread; idempotent.
void EmergencyStop() noexcept;

// Debug / automation tools (debug_actions.cpp via game::WorldShareCommand; game thread):
enum class Tool : int {
    dump = 0,         // world_dump [cat] [n]: the census (read only, any session)
    sync = 1,         // world_sync <op> ...: status, snapshot, binding, counters, one CP, test holds, count check, round trip
    build = 2,        // dev_build <type> <anim 0/1> [m ahead]: a structure at Sam's front (sandboxed saves only; guest: a GBUILD)
    remove = 3,       // dev_build_remove <hid | 0 nearest> [manual 0/1] (sandboxed saves only; guest: through RemoveManually)
    decay = 4,        // dev_build_decay <hid | 0 nearest> <seconds>: its durability (host, sandboxed saves only)
    mats = 5,         // dev_build_mats <hid | 0 nearest> <0 contribute | 1 repair> <share of the next level's need | repair units> (W2)
    ladder = 6,       // dev_ladder <length m, 0 = 9.65> <mode 0/1> [m ahead]: a ladder (W3; sandboxed saves only; guest: a GBUILD)
    rope = 7,         // dev_rope <length 1..30> <level 0..2> [m ahead]: a climbing anchor with its rope (W3)
};
bool Command(Tool tool, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::wshare
