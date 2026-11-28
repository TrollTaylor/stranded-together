// Round 31 (docs/enemy-sync.md section 25; the round 31 build spec, stages 2-8): the shared interface of the seven BT feature
// modules bt_wx, bt_hunt, bt_tar, bt_mirror, bt_void, bt_catcher and bt_extra. bt_sync.cpp owns the BTS1 relay (channel 6, the
// hello and its caps, the generation / session, the drain) and only routes to them:
//   Install        bootstrap thread, once, after bt_sync's own Install (every role: a module refuses itself when it is off);
//   OnHello        game thread, the partner's BTS1 hello once per link session, when the relay turns on (roles differ);
//   OnMsg          game thread, one BTS1 message of this module's types (the header and the exact size already checked by
//                  btrules::DecodeBtsHeader; nothing comes before the partner's hello);
//   Tick           game thread, every BT tick while the sync is not stopped (also with bt_sync=0 and in solo / echo roles: the
//                  context says which), after bt_sync's own SyncTick;
//   OnWorldEvent   game thread, the edges below (each module takes back every record it owns on load_start, link_lost and
//                  partner_gone; load_end is information only);
//   EmergencyStop  any thread, synchronous (dllmain's emergency stop after a tick fault: NO later Tick comes): pass every detour
//                  through and restore what the module wrote, here and now;
//   Caps           the cap bits this module offers in this game's hello (computed when the hello is sent, once per session);
//   Status         one short line for "[bt] sync status" and the 60 s "[bt] r31 status" (no "[bt]" prefix, no newline).
// The modules' own extra hooks (bt_hunt's PULLED feed, bt_void's local voidouts, the gazer update callbacks, the kill look's
// reason, the bt_sync 8..11 test ops) are declared in their own headers; bt_sync.cpp calls each one from the place named there.
// Game-thread helpers (the guarded writes, the managers, the send) are in bt_sync_internal.hpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace st {
struct Config;
}

namespace st::bt {

// Encode a BTS1 message with the sequence number seq() returns, then send() it on channel 6 (link::SendCombat). send() is false
// (and nothing goes) before the partner's hello, after EmergencyStop, or when the link does not take it; each success moves the
// sequence on, so call seq() again for the next message.
using SendFn = bool (*)(const std::uint8_t* msg, std::size_t n) noexcept;
using SeqFn = std::uint32_t (*)() noexcept;

// The edges bt_sync.cpp sees (game thread). Each is sent once per edge to every module.
enum class WorldEvent : std::uint8_t {
    load_start,    // a load began or the local player went (LoadingNow false -> true): every game record is about to be freed
    load_end,      // back in a world (LoadingNow true -> false)
    link_lost,     // the link dropped (connected true -> false) or a new link session started (a newer generation)
    partner_gone,  // the link is up but the partner's SEH1 beacon is older than 12 s (two 5 s beacons; or stopped) while its relay was on
};
const char* WorldEventName(WorldEvent e) noexcept;  // "load start", "load end", "link lost", "partner gone"

// The partner's hello (btrules::BtsHello) as the modules need it. real_save: the partner is a guest playing on its own real save
// (host_save=0): its hello lacks kCapResetApply, which a guest sets only on a sandboxed save with bt_sync 2 (round 28's
// ResetApplyHere). A host is never "real save" here (it is the authority; note the host's own save is real all the same).
struct PeerHello {
    std::uint8_t level = 0, role = 0;  // bt_sync 1|2; 1 host, 2 guest
    std::uint16_t caps = 0;            // btrules::BtsCap bits
    bool real_save = false;
};

// One BT tick's view (game thread), built once per tick by bt_sync.cpp.
struct TickCtx {
    double now_ms = 0;
    int role = 0;        // 1 host, 2 guest, 0 not a two-game session (solo, echo: see echo)
    bool echo = false;   // role echo (the echo partner: a solo test role; bt_sync's relay is off there)
    int sync_level = 0;  // bt_sync here (0 off, 1, 2); 0 also when not a two-game session
    bool link_up = false;           // link::GetInfo().connected
    bool peer = false;              // the partner's hello arrived this link session (the relay is on)
    PeerHello partner;              // valid while peer
    bool real_save = false;         // THIS game is a guest on its own real save (host_save=0): never written beyond cosmetics
    bool saves_redirected = false;  // automation::SavesRedirected(): a sandboxed save (host_save=1 guest, a test save, a hidden run)
    bool hidden_run = false;        // automation::Enabled()
    bool loading = false;           // bt_sync's LoadingNow(): a load in progress or no live local player
    const char* playing_why = nullptr;  // nullptr: the world is played; else why not (asleep, a menu, a cutscene, loading)
    bool carried = false;               // ride::Carried(): this game's Sam is carried (a passenger)
    bool hunted_here = false;           // the hunter manager's area hm+0x140 is up here (HunterAreaUp() == 1)
    std::uint8_t partner_trouble = 0;   // the partner's TROUBLE kind now (btrules::TroubleKind; 0 none, cleared or expired)
    bool me_ok = false;
    double me[3] = {0, 0, 0};  // the local Sam's position
    bool partner_ok = false;   // the partner's body (its live entity, else the newest remote sample: sync::DebugPartnerPosition)
    bool partner_fresh = false;  // the partner's pose frame is under 1 s old (realbody::PartnerFrameFresh(1000))
    double partner_pos[3] = {0, 0, 0};
    SendFn send = nullptr;
    SeqFn seq = nullptr;
};

// One voidout in this game (bt_sync's pass-through detour on FUN_14299B1B0, drained on the game thread): what VOID already
// carries, for bt_void's VOID2. source: btrules::VoidSource (from the caller's return address); ret_va its ds.exe VA (0 unknown).
struct LocalVoid {
    double pos[3] = {0, 0, 0};
    bool pos_ok = false;
    bool caused = false, baggage = false;  // r8b (a gazer caused it), r9b (with baggage)
    std::uint8_t source = 0;
    float far_dist = -1;  // [[mgr+0x68]+0x54], -1 unread
    std::uint64_t ret_va = 0;
};

constexpr std::size_t kStatusCap = 192;  // Status() buffers

}  // namespace st::bt
