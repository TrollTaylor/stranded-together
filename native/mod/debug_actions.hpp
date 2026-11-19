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
                          //   11 overlap hiding v[1], 13 meters (session.ini vehicle_sync; docs/real-partner-body.md section 22)
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
enum class Tab : std::uint8_t { status, partner, player, tools };
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
