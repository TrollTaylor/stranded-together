// Debug-menu plumbing shared by the overlay (render thread) and the mod's tick
// (game main thread, st::sync::Tick):
//
//   render thread  --Queue(Request)-->  [locked queue]  --Drain on tick-->  handler (game thread)
//   tick thread    --Publish(Snapshot)--> [locked copy] --GetSnapshot-->    menu (render thread)
//
// The menu never touches game memory: everything it shows is a copy the tick
// published, and everything it does is a request the tick executes.
//
// Adding an action: one enum value below + one row in kRows (debug_actions.cpp)
// with its handler there. Game-side work lives in game_actions.cpp (st::game). The menu lists rows by tab, so most actions need no
// menu code at all.
#pragma once
#include <cstddef>
#include <cstdint>

#include "game_actions.hpp"

namespace st::debug {

enum class Action : std::uint8_t {
    respawn_partner,      // what F7 does: remove the partner, spawn again next frame
    remove_partner,       // remove and hold spawning until "respawn"
    show_partner,
    hide_partner,
    set_echo_tuning,      // f[0] = echo delay (s), f[1] = sideways offset (m)
    set_verbose,          // flag
    log_positions,        // what F6 does
    log_partner,          // partner entity details (type, flags, components)
    log_status,           // the whole snapshot to the log
    teleport,             // v = x, y, z; i = game::TeleportMode (0 = place, 2 = snap to ground from x, y)
    teleport_to_partner,
    unstuck,              // teleport to the game's recorded safe ground position
    heal,                 // f[0] = amount (0 = to max health)
    set_time_of_day,      // f[0] = hours [0, 24)
    set_day_night_cycle,  // flag = cycle enabled (false = time frozen)
    voice_ping,           // partner echo voice line; i = line number from 1 (0 = next in turn)
    copy_diagnostics,     // status + game diagnostics to the clipboard and the log
    holo_catalog,         // v[0] = first index, v[1] = count, v[2] = distance in front of Sam (m, 0 = default)
    clip_survey,          // loaded animation clips with root motion / on Sam's skeleton, to the log
    anim_census,          // v[0] = radius m (0 = all), v[1] = tag, v[2] = scan bits (1 anim/humanoid, 2 places)
    pose_dump,            // v[0] = tag, v[1] = frames (1..8), v[2] = delay ms
    pose_scan,            // v[0] = tag: world-space float triples near Sam + Skeleton refs (worker thread)
    look_at_body,         // v[0] = index in the last census, v[1] = distance m (0 = 4 m)
    snap_teleport,        // v = x, y, z (z 0 = 500): PlaceOn mode 2 (snap to the topmost surface)
    real_probe,           // real body (docs/real-partner-body.md): player ref path/GUID order, manager, Sam's model
    real_spawn,           // v[0] = body id (body_table), v[1] = distance m, v[2] = 1 mirror Sam's live pose
    real_remove,          // remove the debug real body
    real_names,           // Sam's and the body's 77 joint names
    real_stats,           // pose hook counters + partner-vs-published check
    real_mirror,          // v[0] = 1 on / 0 off: the debug body copies Sam's live pose
    real_control,         // v[0] = n: the n-th streamed-out collectors item through the same path+GUID route (control)
    self_view,            // v[0] = 1 on / 0 off, v[1] = body id (0 = my_character), v[2] = follow (1 Sam's update, 2 the body's, 3 tick, 0 unchanged)
    real_outfit,          // v[0] = 0 log, 1 dress the slot's Sam body like our Sam, 2 exos v[1] + suit colour v[2], 3 override off,
                          //   4 (hidden tests on scratch saves only) set our own Sam's suit colour byte to v[1]
    name_tags,            // v[0] = 0 normal, 1 self test (a tag above your own Sam), 2 log the camera, 3 tags off (v[1] 0) / on (1)
    collision,            // v[0] = 0 log, 1 off (v[1] 0) / on (1), 2 per-tick distance trace for v[1] ms, 3 pin the partner v[1] m
                          //   ahead (0 = follow again), 4 reconfigure: layer v[1] (9/10), type v[2] (0 rigid body, 1 proxy)
    gear,                 // v[0] = 0 log, 1 partner props off (v[1] 0) / on (1), 2 self-view props off / on, 3 test: only the first v[1]
                          //   cargo pieces (-1 all; hidden tests only), 4 our own gear on the debug body off / on
    smooth,               // v[0] = 0 log, 1 smooth feature mask v[1] (0..7, realbody::kSmooth*), 2 frame cap v[1] fps (0 off),
                          //   3 record a per-frame sequence of v[1] ticks to the log (docs/real-partner-body.md section 3.5)
    audio_probe,          // v[0] = 0 summary, 1 dump, 2 test situation v[1], 3 player components, 4 reset (audio_probe=1; docs/audio-research.md)
    self_voice,           // v[0] = 0 log, 1 mute off / on v[1] (when armed), 2 the cargo menu figure says line list v[1] (session.ini self_voice)
    voice_sync,           // v[0] = 0 log, 1 play sentence v[1] on the partner's talking entity, 2 test: send as Sam v[1] 0/1, 3 rebuild the map
                          //   (session.ini voice_sync=1; docs/audio-research.md section 15)
    sound_sync,           // v[0] = 0 log, 1 verbose v[1] 0/1, 2 stop the partner's sounds (session.ini sound_sync=1; docs/audio-research.md section 16)
    vehicle_sync,         // v[0] = 0 log, 1 mode v[1], 2 copy override v[1], 3 trace v[1] ms, 4 offset right v[1] / forward v[2] cm, 5 describe,
                          //   6 offset up v[1] cm, 7 list, 8 / 12 / 9 / 16 / 17 placements (hidden tests), 10 hold sends v[1] ms,
                          //   11 overlap hiding v[1], 13 meters, 21 the copy's collision, 22 / 23 pin the copy (hidden tests)
                          //   (session.ini vehicle_sync; docs/real-partner-body.md section 22)
    coop_session,         // v[0] = 0 log, 1 test link blackout v[1] ms, 2 offer the newest save (host), 3 accept the teleport offer
                          //   (guest), 4 log the slots (host), 5 test in-world load type v[1] index v[2] (host), 6 test QuitGame,
                          //   7 test checkpoint retry (host), 8 test: waiting panel hidden v[1] (guest); 1 and 5-8 hidden runs only
                          //   (session.ini host_save=1; docs/coop-session.md)
    vehicle_share,        // v[0] = 0 status, 1 dump [tag], 2 summon kind [m], 3 snapshot now, 4 binding report, 5 mirror path v[1] (tests),
                          //   6 remove the nearest vehicle of kind v[1] within v[2] m (tests), 7 counters, 8 persisted entries (host; vehicle_sync=5;
                          //   docs/real-partner-body.md section 23)
    vehicle_dump,         // v[0] = tag: every in-use vehicle unit of the three pools, with its binding (vehicle_share)
    vehicle_summon,       // v[0] = kind (1 roadster, 2 truck, 3 trike), v[1] = metres ahead (0 = 8): through the game's own request path
    vehicle_drive,        // v[0] = 0 status, 1 vehicles, 2 native correction v[1], 3 ignore bands v[1] / v[2] m, 4 corrections v[1] (tests),
                          //   5 native fx v[1], 6 kind v[1] [m v[2]] test push (tests), 7 life v[1] test (tests), 8 battery probe,
                          //   9 [m] our Sam beside the vehicle the partner drives (tests)
                          //   (vehicle_drive.cpp; docs/real-partner-body.md section 23.11)
    ride_state,           // riding together (ride_together.cpp; docs/real-partner-body.md section 24): the state and counters
    ride_board,           // board the partner-driven vehicle now (a completed hold; the boarding checks apply)
    ride_exit,            // get off now; v[0] = 1 also above 2 m/s (tests)
    seat_measure,         // the vehicle our Sam drives (or last drove): his root / pelvis and the game's seat helpers in its frame
    ride_test,            // v[0] = 1 solo seat carry, 2 end, 4 crash dv v[1], 5 flip / 6 stop our vehicle, 7 taken, 30-33 seat tuning (tests)
    vehicle_hit,          // vehicle hits (partner_tackle.cpp; docs/real-partner-body.md section 25): v[0] = 0 state, 1 kind v[1] path v[2],
                          //   2 one hit on our Sam now (tests), 8 [m ahead] [m right] our Sam in front of the partner's vehicle (tests), 9 the capsule's boxes
    save_guard,           // the save guard (save_guard.cpp; docs/real-partner-body.md section 24.12): v[0] = 0 state, 1 [type] a save now (tests),
                          //   2 our Sam's place vs the last held save, 3 load the newest save of the run (host, tests), 4 0/1 the hold off / on (tests),
                          //   5 why a save is refused now (tests), 6 / 7 [s] the next forced save stays pending until a boarding / without one (tests)
    // Dev tools (dev_tools.hpp; dev_tools=1 or a hidden run; docs/debug-actions.md "Dev tools"). Tools that change what a save
    // keeps are refused unless this game's saves are sandboxed (dev_rules.hpp).
    dev_status,           // v[0] = 0 log the gates, 1 v[1] = 1 test: act as on a real save (hidden runs), 0 back
    dev_vehicle,          // v[0] = kind (1 roadster, 2 truck, 3 trike), v[1] = variant, v[2] = metres ahead (0 = 8)
    dev_vehicle_remove,   // v[0] = max metres (0 = 15): the nearest vehicle nobody is on; v[1] = 1 the newest dev vehicle instead
    dev_vehicle_fix,      // v[0] = max metres (0 = 10): repair and recharge the nearest vehicle
    dev_refill,           // health, stamina, battery, blood (RecoverFull)
    dev_god,              // v[0] = 0 off, 1 on, 2 invulnerable
    dev_ammo,             // v[0] = 0 / 1 infinite ammo
    dev_weather,          // v[0] = 0 clear, 1 timefall, 2 heavy timefall, 3 the game's mix
    dev_enemies,          // v[0] = 1 resurrect every MULE / terrorist, 2 remove them, 3 reset the BTs
    dev_places,           // the current area's places (delivery points) to the log
    dev_place,            // v[0] = index in the place list: teleport there
    dev_items,            // v[0] = 1 boots, 2 chiral crystals +v[1] (0 = 100)
    // Story tools (story_state.hpp; docs/dev-tools.md "Story tools"): read only, except the load (sandboxed saves only)
    story,                // v[0] = 0 the story state to the log, 1 every main / sub mission's state, 2 the manual slots (story points),
                          //   3 whether story_load is allowed here (and why not)
    story_load,           // v[0] = manual slot: the game's Load Game of manualsave<v0> (the story library's are 10..19)
    // Hidden-test world tools (docs/dev-tools.md "Go anywhere", "Boots"; dev_tools.cpp)
    dev_goto,             // v[0] x, v[1] y, v[2] z (0 = the ground under x, y): Sam there, then the ground streams in (sandboxed saves only)
    dev_boots,            // v[0] = 0 log the boots' life, 1 v[1] = percent: set it (sandboxed saves only), 2 v[1] = 0/1 a 1 s meter, 3 v[1] = boot type (SetShoesForcibly)
    // Shared structures (world_share.cpp; docs/world-sync.md, docs/debug-actions.md "World sync")
    world_dump,           // v[0] = category (-1 all), v[1] = n nearest (0 = 16): the census to the log (read only)
    world_sync,           // v[0] = op (0 status, 1 snapshot, 2 binding, 3 counters, 4 one CP, 5 hold sends, 10 count check, 11 round trip, 13 gate, 14 watch)
    dev_build,            // v[0] = type, v[1] = anim 0/1, v[2] = m ahead (sandboxed saves only; the guest asks the host)
    dev_build_remove,     // v[0] = id (0 = nearest synced), v[1] = 1 dismantle / 0 removal bits
    dev_build_decay,      // v[0] = id (0 = nearest), v[1] = durability seconds (host)
    dev_build_mats,       // v[0] = id (0 = nearest), v[1] = 0 contribute / 1 repair, v[2] = share of the next level's need (0..1] / repair units
    dev_ladder,           // v[0] = length m (0 = 9.65), v[1] = mode 0/1, v[2] = m ahead (W3; sandboxed saves only; the guest asks the host)
    dev_rope,             // v[0] = length 1..30 m, v[1] = level 0..2, v[2] = m ahead (W3)
    // Fragile's edited textures (fragile_textures.hpp; docs/debug-actions.md "Fragile's textures")
    tex,                  // v[0] = mode (0 status, 1 rescan, 2 probe, 3 colour, 4 off/on, 5 filter, 6 slot offset, 7 resend, 9 pattern), v[1], v[2]
    // Enemy sync P0 (enemy_census.hpp; docs/enemy-sync.md): read only
    enemy_census,         // v[0] = tag, v[1] = radius m (0 = all), v[2] = flags (1 every other actor type, 2 the MULE manager's killed records)
    // Enemy sync P1-0 (enemy_sync.hpp): the freeze probe, hidden runs only
    enemy_probe,          // v[0] = op (0 check + pick [v[1] radius], 1 lever v[1] mode v[2], 2 pose v[1] s, 3 secs v[1] path v[2], 9 release)
    // Shots (shot_sync.hpp; docs/enemy-sync.md P3a)
    shots,                // v[0] = 0 status, 1 on, 2 off; hidden runs: 3 / 4 / 9 placement and aim, 5 aimed test, 6 cargo target, 8 ids
    shots_test,           // v[0] = metres ahead, v[1] = flags (1 the filter keeps it, 2 no weapon, 4 no magazine, 16 leak test), v[2] = count
    shots_echo,           // v[0] = 0/1, v[1] = metres to the right (default 2.5)
    // Enemy sync P1a (enemy_sync.hpp; docs/enemy-sync.md 8)
    enemy_sync,           // v[0] = op (0 status, 1 on, 2 off, 3 mirror table; hidden runs: 6 variant v[1], 7 cadence v[1])
    enemy_look,           // v[0] = id (0 nearest, -1 the one the partner faces), v[1] = m (hidden runs)
    enemy_shot_test,      // v[0] = id (0 nearest mirror), v[1] = 1 nohurt (hidden runs, the guest)
    count

};

struct Request {
    Action action = Action::count;
    double v[3] = {};
    float f[2] = {};
    std::int32_t i = 0;
    bool flag = false;
    std::uint32_t serial = 0;  // filled by Queue
};

// Where an action shows up in the menu (and whether it needs a partner).
enum class Tab : std::uint8_t { status, partner, player, tools, dev };
struct ActionInfo {
    Action id;
    Tab tab;
    const char* label;    // button text
    const char* help;     // tooltip
    bool button;          // false: driven by a dedicated widget (sliders, teleport form)
};
// Registry, in display order.
const ActionInfo* Actions(std::size_t* count) noexcept;
const ActionInfo* Find(Action a) noexcept;
const char* ActionName(Action a) noexcept;

// Any thread. Returns the request serial (never 0), or 0 when the queue is full.
std::uint32_t Queue(const Request& r) noexcept;
std::uint32_t Queue(Action a) noexcept;
// Pending requests (menu display / tests).
std::size_t Pending() noexcept;

// Game thread (st::sync::Tick): runs queued actions, then publishes a fresh
// snapshot when one is due (every tick while the menu is open, else 2 Hz).
void OnTick(double now_ms) noexcept;
// Runs queued actions only. OnTick calls it; exposed for tests and harnesses.
std::size_t Drain() noexcept;

// Outcome of an executed action, newest last.
struct Result {
    std::uint32_t serial = 0;
    Action action = Action::count;
    bool ok = false;
    double time_ms = 0;  // tick clock when it ran
    char text[160] = {};
};
// Copies up to `max` newest results (oldest first); returns how many.
std::size_t RecentResults(Result* out, std::size_t max) noexcept;
// The result for one serial, if it ran already.
bool ResultFor(std::uint32_t serial, Result* out) noexcept;

// ------------------------------------------------------------------ snapshot
struct Snapshot {
    std::uint64_t serial = 0;  // bumps on every Publish
    double time_ms = 0;        // tick clock (link::NowMsPrecise)
    char role[16] = "none";
    char mod_state[96] = "starting";

    // Link (host/guest only)
    bool net = false;
    char link_state[24] = "off";
    char transport[12] = {};
    char route[12] = {};
    char peer[48] = {};
    char reason[96] = {};
    std::uint32_t rtt_ms = 0;
    std::uint64_t rx = 0, tx = 0;

    // Local Sam
    bool local_present = false;   // GetLocalDSPlayerEntity returned an entity
    bool local_valid = false;     // its transform was read this tick
    bool local_awake = false;
    std::uintptr_t local_entity = 0;
    std::uint64_t local_flags = 0;
    double local_pos[3] = {};
    float local_rot[3][3] = {};
    float speed = 0;

    // Partner (our spawned entity) and the newest remote pose
    std::uintptr_t partner_entity = 0;
    std::uintptr_t partner_voice_entity = 0;  // entity carrying the DSEchoComponent (echo body or partner_voice=1 echo)
    char partner_state[16] = "none";
    char partner_source[24] = {};
    std::uint64_t partner_flags = 0;
    bool partner_pos_valid = false;
    double partner_pos[3] = {};
    bool spawn_disabled = false;
    bool spawn_held = false;      // "remove partner" from the menu holds spawning
    bool remote_fresh = false;
    double remote_age_ms = 0;

    // Tuning
    float echo_delay_s = 0;
    float echo_offset_m = 0;
    std::uint32_t interp_delay_ms = 0;
    std::uint32_t send_hz = 0;
    bool verbose = false;

    // Rates
    float tick_hz = 0;
    std::uint64_t ticks = 0;

    // Game state, load mask, area, time of day, health, teleport readiness
    // (game_actions.cpp; read-only, filled on the game thread).
    game::Diagnostics diag;
};

// Plain-text report of a snapshot (what "Copy diagnostics" puts on the
// clipboard). Returns the length written.
std::size_t FormatDiagnostics(const Snapshot& s, char* out, std::size_t cap) noexcept;

// Tick thread. Copies under a lock.
void Publish(const Snapshot& s) noexcept;
// Any thread: a copy of the newest snapshot (serial 0 before the first tick).
Snapshot GetSnapshot() noexcept;

// Bootstrap thread: free text shown in the Status tab ("ready (echo)", "game
// functions not found", ...).
void SetModState(const char* text) noexcept;
const char* RoleLabel() noexcept;
void SetRoleLabel(const char* role) noexcept;

// The overlay sets this while the menu is open; the tick publishes every frame
// then, and the mod's own hotkeys (F6/F7, read with GetAsyncKeyState) stand down.
void SetMenuOpen(bool open) noexcept;
bool MenuOpen() noexcept;

// Tests: replace a handler's game call. `fn` returns ok and writes a message.
using Handler = bool (*)(const Request&, char* msg, std::size_t cap);
void SetHandlerForTest(Action a, Handler fn) noexcept;
void ResetForTest() noexcept;

}  // namespace st::debug
