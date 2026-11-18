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
#include "player_sync.hpp"

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
