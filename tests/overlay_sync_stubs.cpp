// Stand-in for player_sync's debug-menu accessors and for game_actions
// (st_overlay_tests and st_overlay_harness link debug_actions.cpp without the
// game-facing sync or game code).
// Every call records the calling thread so tests can prove actions run on the
// thread that drains the queue (the mod's tick thread).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "debug_actions.hpp"
#include "dev_tools.hpp"
#include "enemy_census.hpp"
#include "enemy_combat.hpp"
#include "enemy_sync.hpp"
#include "shot_sync.hpp"
#include "player_sync.hpp"
#include "story_state.hpp"

namespace st::sync::stub {
std::atomic<int> respawns{0}, removes{0}, visible_calls{0}, log_positions{0}, log_partner{0}, tuning_calls{0};
std::atomic<unsigned long> last_thread{0};
float delay = 2.0f, offset = 0.0f;
bool verbose = false;
bool have_partner = true;
double partner[3] = {1012.5, 48.25, -2210.75};
double local[3] = {1000.0, 47.5, -2200.0};
}  // namespace st::sync::stub

namespace st::sync {
namespace {
void Seen() { stub::last_thread.store(GetCurrentThreadId()); }
template <std::size_t N>
void Put(char (&dst)[N], const char* src) {
    std::strncpy(dst, src, N - 1);
    dst[N - 1] = 0;
}
}  // namespace

void FillDebugSnapshot(debug::Snapshot& s) noexcept {
    Seen();
    s.net = true;
    Put(s.link_state, "connected");
    Put(s.transport, "udp");
    Put(s.route, "direct");
    Put(s.peer, "127.0.0.1:27556");
    s.rtt_ms = 23;
    s.rx = 1234;
    s.tx = 1240;
    s.local_present = s.local_valid = s.local_awake = true;
    s.local_entity = 0x7FF6A0001000ull;
    s.local_flags = 0x1013;
    std::memcpy(s.local_pos, stub::local, sizeof(s.local_pos));
    s.speed = 4.2f;
    s.partner_entity = stub::have_partner ? 0x7FF6A0002000ull : 0;
    Put(s.partner_state, stub::have_partner ? "visible" : "none");
    Put(s.partner_source, "echo");
    s.partner_flags = 0x1003;
    s.partner_pos_valid = stub::have_partner;
    std::memcpy(s.partner_pos, stub::partner, sizeof(s.partner_pos));
    s.remote_fresh = true;
    s.remote_age_ms = 31;
    s.echo_delay_s = stub::delay;
    s.echo_offset_m = stub::offset;
    s.interp_delay_ms = 100;
    s.send_hz = 30;
    s.verbose = stub::verbose;
    s.tick_hz = 60.0f;
    s.ticks = 4321;
}

void DebugRespawnPartner() noexcept {
    Seen();
    ++stub::respawns;
}
bool DebugRemovePartner() noexcept {
    Seen();
    ++stub::removes;
    return stub::have_partner;
}
bool DebugSetPartnerVisible(bool) noexcept {
    Seen();
    ++stub::visible_calls;
    return stub::have_partner;
}
void DebugSetEchoTuning(float delay_s, float offset_m) noexcept {
    Seen();
    ++stub::tuning_calls;
    stub::delay = delay_s;
    stub::offset = offset_m;
}
void DebugSetVerbose(bool on) noexcept {
    Seen();
    stub::verbose = on;
}
void DebugLogPositions() noexcept {
    Seen();
    ++stub::log_positions;
}
bool DebugLogPartner() noexcept {
    Seen();
    ++stub::log_partner;
    return stub::have_partner;
}
bool DebugPartnerPosition(double (&out)[3]) noexcept {
    Seen();
    if (!stub::have_partner) return false;
    std::memcpy(out, stub::partner, sizeof(out));
    return true;
}

}  // namespace st::sync

// ---------------------------------------------------------------- game_actions stand-in
// Defaults keep the harness's expectation: teleport is refused with "not implemented".
namespace st::game::stub {
std::atomic<int> teleports{0}, unstucks{0}, heals{0}, times{0}, cycles{0}, pings{0}, ticks{0};
bool teleport_ok = false;
double last_target[3] = {};
int last_mode = 0;
float last_heal = -1, last_hours = -1;
bool last_cycle = true;
std::uintptr_t last_ping_entity = 0;
int last_ping_index = 99;
std::atomic<int> catalogs{0}, surveys{0}, censuses{0}, real_cmds{0};
int last_real_op = 0;
int last_catalog_start = -1, last_catalog_count = -1;
double last_catalog_distance = -1;
}  // namespace st::game::stub

namespace st::game {
namespace {
void Seen() { sync::stub::last_thread.store(GetCurrentThreadId()); }
void Say(char* msg, std::size_t cap, const char* text) {
    if (msg && cap) std::snprintf(msg, cap, "%s", text);
}
}  // namespace

bool TeleportLocalPlayer(double x, double y, double z, TeleportMode mode, char* msg, std::size_t cap) noexcept {
    Seen();
    ++stub::teleports;
    stub::last_target[0] = x, stub::last_target[1] = y, stub::last_target[2] = z;
    stub::last_mode = static_cast<int>(mode);
    if (!stub::teleport_ok) {
        Say(msg, cap, "not implemented in the test stub");
        return false;
    }
    if (msg && cap) std::snprintf(msg, cap, "placed at (%.2f, %.2f, %.2f)", x, y, z);
    return true;
}
bool Unstuck(char* msg, std::size_t cap) noexcept {
    Seen();
    ++stub::unstucks;
    Say(msg, cap, "the game has no safe position recorded (stub)");
    return false;
}
bool HealLocalPlayer(float amount, char* msg, std::size_t cap) noexcept {
    Seen();
    ++stub::heals;
    stub::last_heal = amount;
    Say(msg, cap, "health 50.0 -> 100.0 / 100.0");
    return true;
}
bool SetTimeOfDay(float hours, char* msg, std::size_t cap) noexcept {
    Seen();
    ++stub::times;
    stub::last_hours = hours;
    const bool ok = hours >= 0.0f && hours < 24.0f;
    Say(msg, cap, ok ? "time of day set" : "time is outside 0..24");
    return ok;
}
bool SetDayNightCycle(bool enabled, char* msg, std::size_t cap) noexcept {
    Seen();
    ++stub::cycles;
    stub::last_cycle = enabled;
    Say(msg, cap, enabled ? "day/night cycle on" : "day/night cycle off (time frozen)");
    return true;
}
bool PartnerVoicePing(std::uintptr_t partner_entity, int index, char* msg, std::size_t cap) noexcept {
    Seen();
    ++stub::pings;
    stub::last_ping_entity = partner_entity;
    stub::last_ping_index = index;
    Say(msg, cap, partner_entity ? "voice line queued" : "no live partner");
    return partner_entity != 0;
}
void ActionsTick(double) noexcept { ++stub::ticks; }
bool StartHoloCatalog(int start, int count, double distance_m, char* msg, std::size_t cap) noexcept {
    Seen();
    ++stub::catalogs;
    stub::last_catalog_start = start;
    stub::last_catalog_count = count;
    stub::last_catalog_distance = distance_m;
    Say(msg, cap, "catalog batch started (stub)");
    return true;
}
bool StartClipSurvey(char* msg, std::size_t cap) noexcept {
    Seen();
    ++stub::surveys;
    Say(msg, cap, "clip survey started (stub)");
    return true;
}
bool StartAnimCensus(double, int, int, char* msg, std::size_t cap) noexcept {
    Seen();
    ++stub::censuses;
    Say(msg, cap, "census (stub)");
    return true;
}
bool PoseDump(int, int, double, char* msg, std::size_t cap) noexcept {
    Seen();
    ++stub::censuses;
    Say(msg, cap, "pose dump (stub)");
    return true;
}
bool StartPoseScan(int, char* msg, std::size_t cap) noexcept {
    Seen();
    ++stub::censuses;
    Say(msg, cap, "pose scan (stub)");
    return true;
}
bool LookAtBody(int, double, char* msg, std::size_t cap) noexcept {
    Seen();
    ++stub::censuses;
    Say(msg, cap, "look at (stub)");
    return true;
}
bool RealBodyCommand(int op, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    stub::last_real_op = op;
    ++stub::real_cmds;
    Say(msg, cap, "real body (stub)");
    return true;
}
bool NameTagCommand(int, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    Say(msg, cap, "name tags (stub)");
    return true;
}
bool CollisionCommand(int, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    Say(msg, cap, "collision (stub)");
    return true;
}
bool GearCommand(int, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    Say(msg, cap, "gear (stub)");
    return true;
}
bool AudioProbeCommand(int, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    Say(msg, cap, "audio probe (stub)");
    return true;
}
bool SelfVoiceCommand(int, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    Say(msg, cap, "self voice (stub)");
    return true;
}
bool VoiceSyncCommand(int, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    Say(msg, cap, "voice sync (stub)");
    return true;
}
bool SoundSyncCommand(int, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    Say(msg, cap, "sound sync (stub)");
    return true;
}
bool VehicleSyncCommand(int, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    Say(msg, cap, "vehicle sync (stub)");
    return true;
}
bool CoopSessionCommand(int, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    Say(msg, cap, "coop session (stub)");
    return true;
}
bool VehicleShareCommand(int, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    Say(msg, cap, "vehicle share (stub)");
    return true;
}
bool WorldShareCommand(int, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    Say(msg, cap, "world share (stub)");
    return true;
}
bool VehicleDriveCommand(int, const double (&)[3], char* msg, std::size_t cap) noexcept {
    Seen();
    Say(msg, cap, "vehicle drive (stub)");
    return true;
}

void ReadDiagnostics(Diagnostics* d) noexcept {
    *d = Diagnostics{};
    d->resolved = true;
    d->features = kFeatAll & ~kFeatEcho;
    std::snprintf(d->disabled, sizeof(d->disabled), "voice ping: DSEchoComponent::Play (stub)");
    d->state_valid = true;
    d->state_flags = 0x2000000000ull;  // an unnamed internal bit
    d->state_requested = 0;
    d->module_valid = true;
    d->area_valid = true;
    d->area = 200;
    d->tod_valid = true;
    d->time_of_day = 14.5f;
    d->day = 3;
    d->day_night_cycle = stub::last_cycle;
    d->player_system_valid = true;
    d->health_valid = true;
    d->health = 85.0f;
    d->health_max = 100.0f;
    d->teleport_ready = true;
    std::snprintf(d->last_teleport, sizeof(d->last_teleport), "teleport (1.0, 2.0, 3.0) mode 1: verified, 0.3 m off");
}

}  // namespace st::game

// Riding together (ride_together.cpp): the debug rows' handlers.
namespace st::ride {
bool StateCommand(char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "ride state (stub)"), true; }
bool BoardCommand(char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "ride board (stub)"), true; }
bool ExitCommand(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "ride exit (stub)"), true; }
bool SeatMeasureCommand(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "seat measure (stub)"), true; }
bool Command(int, const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "ride test (stub)"), true; }
}  // namespace st::ride

// Vehicle hits (partner_tackle.cpp): the debug row's handler.
namespace st::vhit {
bool Command(int, const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "vehicle hit (stub)"), true; }
}  // namespace st::vhit

// The save guard (save_guard.cpp): the debug row's handler.
namespace st::saveguard {
bool Command(int, const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "save guard (stub)"), true; }
}  // namespace st::saveguard

// Dev tools (dev_tools.cpp): the debug rows' handler, the tick, the Dev tools page's view.
namespace st::dev {
void Tick(double) noexcept {}
const char* stub_real_save_refusal = nullptr;  // tests set kRealSaveReason to play a real-save game
const char* RealSaveRefusal() noexcept { return stub_real_save_refusal; }
bool Command(Op op, const double (&)[3], char* msg, std::size_t cap) noexcept {
    return std::snprintf(msg, cap, "dev op %d (stub)", static_cast<int>(op)), true;
}
View GetView() noexcept {
    View v;
    v.serial = 1;
    v.enabled = true;
    std::snprintf(v.session, sizeof(v.session), "sandboxed save (stub)");
    for (int i = 0; i < 4; ++i) v.ok[i] = true;
    std::snprintf(v.note[1], sizeof(v.note[1]), "your world only (not shared)");
    v.god = 0;
    v.ammo = 0;
    v.area = 200;
    v.place_count = 2;
    std::snprintf(v.places[0].label, sizeof(v.places[0].label), "delivery base #1");
    v.places[0].dist_m = 12;
    std::snprintf(v.places[1].label, sizeof(v.places[1].label), "prepper shelter #4");
    v.places[1].dist_m = 820;
    return v;
}
MarkerView GetMarkerView() noexcept {
    MarkerView v;
    v.serial = 1;
    v.checked = v.ok = true;
    v.area = 200;
    v.count = 2;
    std::snprintf(v.items[0].name, sizeof(v.items[0].name), "A0");
    v.items[0].id = 0, v.items[0].dist_m = 1240, v.items[0].snapped = true, v.items[0].area = 200;  // ids start at 0
    std::snprintf(v.items[1].name, sizeof(v.items[1].name), "C0");
    v.items[1].id = 9, v.items[1].dist_m = 230, v.items[1].snapped = true, v.items[1].area = 200;
    v.newest = 1;
    return v;
}
GotoState GetGotoState() noexcept {
    GotoState g;
    g.phase = GotoState::settled;
    g.serial = 3;
    std::snprintf(g.text, sizeof(g.text), "goto #3: settled at (1.00, 2.00, 3.00) after 1.5 s");
    return g;
}
std::uint32_t GotoSerial() noexcept { return 3; }
}  // namespace st::dev

// Story tools (story_state.cpp): the debug rows' handler and the Dev tools page's Story section.
namespace st::story {
bool Command(Op op, const double (&)[3], char* msg, std::size_t cap) noexcept {
    return std::snprintf(msg, cap, "story op %d (stub)", static_cast<int>(op)), true;
}
View GetView() noexcept {
    View v;
    v.serial = 1;
    v.probe_ok = v.load_ok = v.load_allowed = true;
    v.summary.valid = true;
    v.summary.episode = 2;
    v.summary.main_total = 48, v.summary.main_done = 4, v.summary.sub_total = 34, v.summary.highest_done_order = 4;
    storyrules::AddProgress(v.summary, 60, 5, true);
    storyrules::FormatSummary(v.summary, v.line, sizeof(v.line));
    v.slot_count = 2;
    v.slots[0].index = 10;
    std::snprintf(v.slots[0].title, sizeof(v.slots[0].title), "Order In Progress: No. 1 \"Smart Drug Delivery\" - 00:21:07");
    v.slots[1].index = 13;
    std::snprintf(v.slots[1].title, sizeof(v.slots[1].title), "Order In Progress: No. 5 \"Rare Metal Delivery\" - 02:34:08");
    std::snprintf(v.slots[1].detail, sizeof(v.slots[1].detail), "Head west! / Waystation West of Capital Knot City");
    return v;
}
}  // namespace st::story

// ------------------------------------------------------------------ enemy census (enemy_census.cpp)
namespace st::esync {
bool Probe(int, const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy probe (stub)"), true; }
bool Command(int, const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy sync (stub)"), true; }
bool Look(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy look (stub)"), true; }
bool ShotTest(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy shot test (stub)"), true; }
bool Own(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy own (stub)"), true; }
bool Camp(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy camp (stub)"), true; }
}  // namespace st::esync
namespace st::ecombat {
bool Dump(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy dump (stub)"), true; }
bool AttackInfo(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy attack (stub)"), true; }
bool CtxTest(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy ctx (stub)"), true; }
bool HitMode(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy hit mode (stub)"), true; }
bool HitTest(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy hit (stub)"), true; }
bool HitSend(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy hit send (stub)"), true; }
bool HitWatch(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy hit watch (stub)"), true; }
bool Restrain(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy restrain (stub)"), true; }
bool M2Probe(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy m2 (stub)"), true; }
bool TargetProbe(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy target (stub)"), true; }
}  // namespace st::ecombat
namespace st::l1probe {
bool Command(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy l1 (stub)"), true; }
}  // namespace st::l1probe
namespace st::etarget {
bool Command(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "enemy tgt (stub)"), true; }
}  // namespace st::etarget
namespace st::bt {  // round 28 P3 (bt_sync.cpp)
bool Command(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "bt probe (stub)"), true; }
void Tick(double) noexcept {}
bool SyncCommand(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "bt sync (stub)"), true; }  // round 28 B1
void EmergencyStop() noexcept {}
}  // namespace st::bt
namespace st::odra {  // round 28 P4 (odradek_sync.cpp)
bool Command(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "odra (stub)"), true; }
bool Replay(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "odra replay (stub)"), true; }
}  // namespace st::odra
namespace st::truckv3 {  // round 28 P2 (truck_v3.cpp)
bool Command(const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "truck v3 (stub)"), true; }
}  // namespace st::truckv3
namespace st::shots {
bool Command(int, const double (&)[3], char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "shots (stub)"), true; }
bool Test(double, int, int, char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "shots test (stub)"), true; }
bool Echo(bool, double, char* msg, std::size_t cap) noexcept { return std::snprintf(msg, cap, "shots echo (stub)"), true; }
}  // namespace st::shots
namespace st::enemy {
bool Census(int tag, double, int, char* msg, std::size_t cap) noexcept {
    return std::snprintf(msg, cap, "enemy census tag %d (stub)", tag), true;
}
}  // namespace st::enemy
