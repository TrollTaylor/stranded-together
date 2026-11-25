// Debug-menu action queue, action registry/handlers and the status snapshot.
// See debug_actions.hpp for the threading contract.
#include "debug_actions.hpp"

#include <windows.h>

#include <array>
#include <cstdio>
#include <utility>
#include <cstring>

#include "dev_tools.hpp"
#include "enemy_census.hpp"
#include "enemy_combat.hpp"
#include "enemy_sync.hpp"
#include "shot_sync.hpp"
#include "log.hpp"
#include "player_sync.hpp"
#include "partner_tackle.hpp"
#include "ride_together.hpp"
#include "save_guard.hpp"
#include "story_state.hpp"

namespace st::debug {
namespace {

// ------------------------------------------------------------------ locks
struct SrwGuard {
    explicit SrwGuard(SRWLOCK& l) : lock(l) { AcquireSRWLockExclusive(&lock); }
    ~SrwGuard() { ReleaseSRWLockExclusive(&lock); }
    SrwGuard(const SrwGuard&) = delete;
    SrwGuard& operator=(const SrwGuard&) = delete;
    SRWLOCK& lock;
};

template <std::size_t N>
void Copy(char (&dst)[N], const char* src) noexcept {
    if (!src) src = "";
    std::strncpy(dst, src, N - 1);
    dst[N - 1] = 0;
}

// ------------------------------------------------------------------ handlers
// Game thread only. Each writes a short human message; false = failed/refused.
bool DoRespawn(const Request&, char* msg, std::size_t cap) {
    sync::DebugRespawnPartner();
    std::snprintf(msg, cap, "partner respawn requested");
    return true;
}
bool DoRemove(const Request&, char* msg, std::size_t cap) {
    const bool had = sync::DebugRemovePartner();
    std::snprintf(msg, cap, had ? "partner removed; spawning held until Respawn" : "no partner; spawning held until Respawn");
    return true;
}
bool DoShow(const Request&, char* msg, std::size_t cap) {
    const bool ok = sync::DebugSetPartnerVisible(true);
    std::snprintf(msg, cap, ok ? "partner shown" : "no live partner (or SetVisible unavailable)");
    return ok;
}
bool DoHide(const Request&, char* msg, std::size_t cap) {
    const bool ok = sync::DebugSetPartnerVisible(false);
    std::snprintf(msg, cap, ok ? "partner hidden" : "no live partner (or SetVisible unavailable)");
    return ok;
}
bool DoEchoTuning(const Request& r, char* msg, std::size_t cap) {
    sync::DebugSetEchoTuning(r.f[0], r.f[1]);
    std::snprintf(msg, cap, "echo delay %.2f s, offset %.2f m", r.f[0], r.f[1]);
    return true;
}
bool DoVerbose(const Request& r, char* msg, std::size_t cap) {
    sync::DebugSetVerbose(r.flag);
    std::snprintf(msg, cap, "verbose logging %s", r.flag ? "on" : "off");
    return true;
}
bool DoLogPositions(const Request&, char* msg, std::size_t cap) {
    sync::DebugLogPositions();
    std::snprintf(msg, cap, "positions written to the log");
    return true;
}
bool DoLogPartner(const Request&, char* msg, std::size_t cap) {
    const bool ok = sync::DebugLogPartner();
    std::snprintf(msg, cap, ok ? "partner details written to the log" : "no live partner");
    return ok;
}
bool DoLogStatus(const Request&, char* msg, std::size_t cap) {
    const Snapshot s = GetSnapshot();
    Log("[debug] status: role=%s state=%s link=%s/%s/%s peer=%s rtt=%u rx=%llu tx=%llu", s.role, s.mod_state, s.link_state,
        s.transport, s.route, s.peer, s.rtt_ms, static_cast<unsigned long long>(s.rx), static_cast<unsigned long long>(s.tx));
    Log("[debug] status: local=%p valid=%d awake=%d flags=0x%llX pos=(%.2f, %.2f, %.2f) speed=%.2f tick=%.1f Hz",
        reinterpret_cast<void*>(s.local_entity), s.local_valid ? 1 : 0, s.local_awake ? 1 : 0,
        static_cast<unsigned long long>(s.local_flags), s.local_pos[0], s.local_pos[1], s.local_pos[2], s.speed, s.tick_hz);
    Log("[debug] status: partner=%p state=%s source=%s flags=0x%llX pos=(%.2f, %.2f, %.2f) remote_fresh=%d age=%.0f ms",
        reinterpret_cast<void*>(s.partner_entity), s.partner_state, s.partner_source,
        static_cast<unsigned long long>(s.partner_flags), s.partner_pos[0], s.partner_pos[1], s.partner_pos[2],
        s.remote_fresh ? 1 : 0, s.remote_age_ms);
    char d[4096];
    FormatDiagnostics(s, d, sizeof(d));
    for (char* line = d; *line;) {
        char* end = std::strchr(line, '\n');
        if (end) *end = 0;
        if (std::strncmp(line, "game", 4) == 0 || std::strncmp(line, "load", 4) == 0 || std::strncmp(line, "area", 4) == 0 ||
            std::strncmp(line, "time", 4) == 0 || std::strncmp(line, "health", 6) == 0 || std::strncmp(line, "teleport", 8) == 0 ||
            std::strncmp(line, "last", 4) == 0 || std::strncmp(line, "pushed", 6) == 0)
            Log("[debug] status: %s", line);
        if (!end) break;
        line = end + 1;
    }
    std::snprintf(msg, cap, "status written to the log");
    return true;
}
bool DoTeleport(const Request& r, char* msg, std::size_t cap) {
    const game::TeleportMode mode = r.i == 2 ? game::TeleportMode::snap_to_ground : game::TeleportMode::place;
    char why[160] = {};
    const bool ok = game::TeleportLocalPlayer(r.v[0], r.v[1], r.v[2], mode, why, sizeof(why));
    if (ok) std::snprintf(msg, cap, "%s", why);
    else std::snprintf(msg, cap, "teleport to (%.2f, %.2f, %.2f) failed: %s", r.v[0], r.v[1], r.v[2], why);
    return ok;
}
bool DoTeleportToPartner(const Request&, char* msg, std::size_t cap) {
    double p[3] = {};
    if (!sync::DebugPartnerPosition(p)) {
        std::snprintf(msg, cap, "no partner position yet");
        return false;
    }
    char why[160] = {};
    const bool ok = game::TeleportLocalPlayer(p[0], p[1], p[2], game::TeleportMode::place, why, sizeof(why));
    std::snprintf(msg, cap, ok ? "to the partner (%.2f, %.2f, %.2f): %s" : "teleport to the partner (%.2f, %.2f, %.2f) failed: %s",
                  p[0], p[1], p[2], why);
    return ok;
}
bool DoUnstuck(const Request&, char* msg, std::size_t cap) { return game::Unstuck(msg, cap); }
bool DoHeal(const Request& r, char* msg, std::size_t cap) { return game::HealLocalPlayer(r.f[0], msg, cap); }
// The world time is world state a save may keep: never on a real save (dev_rules.hpp's rule, from every tab).
bool RefusedOnRealSave(const char* what, char* msg, std::size_t cap) {
    const char* why = dev::RealSaveRefusal();
    if (!why) return false;
    std::snprintf(msg, cap, "%s %s", what, why);  // the action's result line logs it
    return true;
}
bool DoSetTimeOfDay(const Request& r, char* msg, std::size_t cap) {
    if (RefusedOnRealSave("time of day", msg, cap)) return false;
    return game::SetTimeOfDay(r.f[0], msg, cap);
}
bool DoDayNightCycle(const Request& r, char* msg, std::size_t cap) {
    if (RefusedOnRealSave("day/night cycle", msg, cap)) return false;
    return game::SetDayNightCycle(r.flag, msg, cap);
}
bool DoHoloCatalog(const Request& r, char* msg, std::size_t cap) {
    const int start = static_cast<int>(r.v[0]);
    const int count = r.v[1] >= 1 ? static_cast<int>(r.v[1]) : 1;
    return game::StartHoloCatalog(start, count, r.v[2], msg, cap);
}
bool DoClipSurvey(const Request&, char* msg, std::size_t cap) { return game::StartClipSurvey(msg, cap); }
bool DoAnimCensus(const Request& r, char* msg, std::size_t cap) {
    return game::StartAnimCensus(r.v[0], static_cast<int>(r.v[1]), static_cast<int>(r.v[2]), msg, cap);
}
bool DoPoseDump(const Request& r, char* msg, std::size_t cap) {
    return game::PoseDump(static_cast<int>(r.v[0]), r.v[1] >= 1 ? static_cast<int>(r.v[1]) : 1, r.v[2], msg, cap);
}
bool DoPoseScan(const Request& r, char* msg, std::size_t cap) { return game::StartPoseScan(static_cast<int>(r.v[0]), msg, cap); }
bool DoLookAtBody(const Request& r, char* msg, std::size_t cap) { return game::LookAtBody(static_cast<int>(r.v[0]), r.v[1], msg, cap); }
bool DoSnapTeleport(const Request& r, char* msg, std::size_t cap) {
    char why[160] = {};
    const double z = r.v[2] != 0 ? r.v[2] : game::kSnapRayCentreZ;
    const bool ok = game::TeleportLocalPlayer(r.v[0], r.v[1], z, game::TeleportMode::snap_to_ground, why, sizeof(why));
    std::snprintf(msg, cap, ok ? "snap teleport to (%.2f, %.2f, %.2f): %s" : "snap teleport to (%.2f, %.2f, %.2f) failed: %s", r.v[0], r.v[1],
                  z, why);
    return ok;
}
bool DoRealProbe(const Request& r, char* msg, std::size_t cap) { return game::RealBodyCommand(1, r.v, msg, cap); }
bool DoRealSpawn(const Request& r, char* msg, std::size_t cap) { return game::RealBodyCommand(2, r.v, msg, cap); }
bool DoRealRemove(const Request& r, char* msg, std::size_t cap) { return game::RealBodyCommand(3, r.v, msg, cap); }
bool DoRealNames(const Request& r, char* msg, std::size_t cap) { return game::RealBodyCommand(4, r.v, msg, cap); }
bool DoRealStats(const Request& r, char* msg, std::size_t cap) { return game::RealBodyCommand(5, r.v, msg, cap); }
bool DoRealMirror(const Request& r, char* msg, std::size_t cap) { return game::RealBodyCommand(6, r.v, msg, cap); }
bool DoRealControl(const Request& r, char* msg, std::size_t cap) { return game::RealBodyCommand(7, r.v, msg, cap); }
bool DoSelfView(const Request& r, char* msg, std::size_t cap) { return game::RealBodyCommand(8, r.v, msg, cap); }
bool DoRealOutfit(const Request& r, char* msg, std::size_t cap) { return game::RealBodyCommand(9, r.v, msg, cap); }
bool DoNameTags(const Request& r, char* msg, std::size_t cap) { return game::NameTagCommand(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoCollision(const Request& r, char* msg, std::size_t cap) { return game::CollisionCommand(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoGear(const Request& r, char* msg, std::size_t cap) { return game::GearCommand(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoSmooth(const Request& r, char* msg, std::size_t cap) { return game::RealBodyCommand(10, r.v, msg, cap); }
bool DoTex(const Request& r, char* msg, std::size_t cap) { return game::RealBodyCommand(11, r.v, msg, cap); }
bool DoEnemyCensus(const Request& r, char* msg, std::size_t cap) {
    return enemy::Census(static_cast<int>(r.v[0]), r.v[1], static_cast<int>(r.v[2]), msg, cap);
}
bool DoEnemyProbe(const Request& r, char* msg, std::size_t cap) { return esync::Probe(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoShots(const Request& r, char* msg, std::size_t cap) { return shots::Command(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoShotsTest(const Request& r, char* msg, std::size_t cap) {
    return shots::Test(r.v[0], static_cast<int>(r.v[1]), static_cast<int>(r.v[2]), msg, cap);
}
bool DoShotsEcho(const Request& r, char* msg, std::size_t cap) { return shots::Echo(r.v[0] != 0, r.v[1], msg, cap); }
bool DoEnemySync(const Request& r, char* msg, std::size_t cap) { return esync::Command(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoEnemyLook(const Request& r, char* msg, std::size_t cap) { return esync::Look(r.v, msg, cap); }
bool DoEnemyShotTest(const Request& r, char* msg, std::size_t cap) { return esync::ShotTest(r.v, msg, cap); }
bool DoEnemyDump(const Request& r, char* msg, std::size_t cap) { return ecombat::Dump(r.v, msg, cap); }
bool DoEnemyAttack(const Request& r, char* msg, std::size_t cap) { return ecombat::AttackInfo(r.v, msg, cap); }
bool DoEnemyCtx(const Request& r, char* msg, std::size_t cap) { return ecombat::CtxTest(r.v, msg, cap); }
bool DoEnemyHitMode(const Request& r, char* msg, std::size_t cap) { return ecombat::HitMode(r.v, msg, cap); }
bool DoEnemyHit(const Request& r, char* msg, std::size_t cap) { return ecombat::HitTest(r.v, msg, cap); }
bool DoEnemyHitSend(const Request& r, char* msg, std::size_t cap) { return ecombat::HitSend(r.v, msg, cap); }
bool DoEnemyHitWatch(const Request& r, char* msg, std::size_t cap) { return ecombat::HitWatch(r.v, msg, cap); }
bool DoEnemyTarget(const Request& r, char* msg, std::size_t cap) { return ecombat::TargetProbe(r.v, msg, cap); }
bool DoEnemyOwn(const Request& r, char* msg, std::size_t cap) { return esync::Own(r.v, msg, cap); }
bool DoAudioProbe(const Request& r, char* msg, std::size_t cap) { return game::AudioProbeCommand(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoSelfVoice(const Request& r, char* msg, std::size_t cap) { return game::SelfVoiceCommand(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoVoiceSync(const Request& r, char* msg, std::size_t cap) { return game::VoiceSyncCommand(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoSoundSync(const Request& r, char* msg, std::size_t cap) { return game::SoundSyncCommand(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoVehicleSync(const Request& r, char* msg, std::size_t cap) { return game::VehicleSyncCommand(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoCoopSession(const Request& r, char* msg, std::size_t cap) { return game::CoopSessionCommand(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoVehicleShare(const Request& r, char* msg, std::size_t cap) { return game::VehicleShareCommand(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoVehicleDump(const Request& r, char* msg, std::size_t cap) { return game::VehicleShareCommand(1, {1, r.v[0], 0}, msg, cap); }
bool DoVehicleSummon(const Request& r, char* msg, std::size_t cap) { return game::VehicleShareCommand(2, {2, r.v[0], r.v[1]}, msg, cap); }
bool DoVehicleDrive(const Request& r, char* msg, std::size_t cap) { return game::VehicleDriveCommand(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoRideState(const Request&, char* msg, std::size_t cap) { return ride::StateCommand(msg, cap); }
bool DoRideBoard(const Request&, char* msg, std::size_t cap) { return ride::BoardCommand(msg, cap); }
bool DoRideExit(const Request& r, char* msg, std::size_t cap) { return ride::ExitCommand(r.v, msg, cap); }
bool DoSeatMeasure(const Request& r, char* msg, std::size_t cap) { return ride::SeatMeasureCommand(r.v, msg, cap); }
bool DoRideTest(const Request& r, char* msg, std::size_t cap) { return ride::Command(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoSaveGuard(const Request& r, char* msg, std::size_t cap) { return saveguard::Command(static_cast<int>(r.v[0]), r.v, msg, cap); }
bool DoVehicleHit(const Request& r, char* msg, std::size_t cap) { return vhit::Command(static_cast<int>(r.v[0]), r.v, msg, cap); }
template <dev::Op kOp>
bool DoDev(const Request& r, char* msg, std::size_t cap) {
    return dev::Command(kOp, r.v, msg, cap);
}
bool DoStory(const Request& r, char* msg, std::size_t cap) { return story::Command(story::Op::show, r.v, msg, cap); }
bool DoStoryLoad(const Request& r, char* msg, std::size_t cap) { return story::Command(story::Op::load, r.v, msg, cap); }
template <int kTool>
bool DoWorld(const Request& r, char* msg, std::size_t cap) {
    return game::WorldShareCommand(kTool, r.v, msg, cap);
}
bool DoVoicePing(const Request& r, char* msg, std::size_t cap) {
    // The live partner entity, exactly as the Partner tab sees it (0 when there is none).
    Snapshot s;
    sync::FillDebugSnapshot(s);
    return game::PartnerVoicePing(s.partner_voice_entity ? s.partner_voice_entity : s.partner_entity, r.i - 1, msg, cap);
}

void FillSnapshot(Snapshot& s, double now_ms);

bool SetClipboardUtf8(const char* text) {
    const int wn = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (wn <= 0) return false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wn) * sizeof(wchar_t));
    if (!mem) return false;
    if (auto* w = static_cast<wchar_t*>(GlobalLock(mem))) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, w, wn);
        GlobalUnlock(mem);
    }
    if (!OpenClipboard(nullptr)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);
    return ok;
}

bool DoCopyDiagnostics(const Request&, char* msg, std::size_t cap);

struct Row {
    ActionInfo info;
    Handler handler;
};

// THE registry: display order inside each tab, and the handler that runs on
// the game thread.
constexpr Row kRows[] = {
    {{Action::respawn_partner, Tab::partner, "Respawn partner", "Remove the partner and spawn it again (same as F7).", true}, &DoRespawn},
    {{Action::remove_partner, Tab::partner, "Remove partner", "Remove the partner and keep it away until Respawn.", true}, &DoRemove},
    {{Action::show_partner, Tab::partner, "Show", "Entity SetVisible(true) on the partner.", true}, &DoShow},
    {{Action::hide_partner, Tab::partner, "Hide", "Entity SetVisible(false) on the partner.", true}, &DoHide},
    {{Action::log_partner, Tab::partner, "Log partner details", "Type, flags, model and components of the partner entity.", true}, &DoLogPartner},
    {{Action::set_echo_tuning, Tab::partner, "Echo tuning", "Echo role: replay delay and sideways offset.", false}, &DoEchoTuning},
    {{Action::voice_ping, Tab::partner, "Voice ping", "Play one echo voice line from the partner (DSEchoComponent::Play).", true}, &DoVoicePing},
    {{Action::teleport, Tab::player, "Teleport", "Teleport Sam to coordinates (PlayerGame::PlaceOn).", false}, &DoTeleport},
    {{Action::teleport_to_partner, Tab::player, "Teleport to partner", "Teleport Sam to the partner's position.", false}, &DoTeleportToPartner},
    {{Action::teleport_to_marker, Tab::player, "Teleport to map marker",
      "Sam on the ground at your newest map marker on this map (or the one picked: v0 = its id + 1), then waits for the ground to stream in; "
      "if he falls through it, he is put back where he started. Any session.", false},
     &DoDev<dev::Op::marker_go>},
    {{Action::unstuck, Tab::player, "Unstuck", "Teleport Sam to the last safe ground position the game recorded.", true}, &DoUnstuck},
    {{Action::heal, Tab::player, "Heal", "Heal Sam to max health (Entity_ExportedHeal).", true}, &DoHeal},
    {{Action::log_positions, Tab::player, "Log positions", "Sam and partner positions and rotation to the log (same as F6).", true}, &DoLogPositions},
    {{Action::set_time_of_day, Tab::player, "Set time of day", "GameWorldTimeState::SetTimeOfDay (instant).", false}, &DoSetTimeOfDay},
    {{Action::set_day_night_cycle, Tab::player, "Freeze time", "Turn the day/night cycle off or on (EnableDayNightCycle).", false}, &DoDayNightCycle},
    {{Action::copy_diagnostics, Tab::status, "Copy diagnostics", "Status and game diagnostics to the clipboard and the log.", true}, &DoCopyDiagnostics},
    {{Action::log_status, Tab::tools, "Log status", "Everything on the Status tab to the log.", true}, &DoLogStatus},
    {{Action::set_verbose, Tab::tools, "Verbose logging", "Per-2s trace lines from the sync.", false}, &DoVerbose},
    {{Action::holo_catalog, Tab::tools, "Holo catalog", "Spawn, log and screenshot collectors holograms one by one (start count [distance]).", false}, &DoHoloCatalog},
    {{Action::clip_survey, Tab::tools, "Clip survey", "Log every loaded animation clip with root motion, and which fit Sam's skeleton.", false}, &DoClipSurvey},
    {{Action::anim_census, Tab::tools, "Anim census", "Log every animated body in the world (radius tag scan).", false}, &DoAnimCensus},
    {{Action::pose_dump, Tab::tools, "Pose dump", "Dump Sam's animation manager and model memory (tag frames delay_ms).", false}, &DoPoseDump},
    {{Action::pose_scan, Tab::tools, "Pose scan", "Scan memory for world-space joint data near Sam (tag).", false}, &DoPoseScan},
    {{Action::look_at_body, Tab::tools, "Look at body", "Teleport Sam behind the n-th body of the last census (index distance).", false}, &DoLookAtBody},
    {{Action::snap_teleport, Tab::tools, "Snap teleport", "Teleport Sam to x y and snap to the topmost surface (PlaceOn mode 2).", false}, &DoSnapTeleport},
    {{Action::real_probe, Tab::tools, "Real probe", "Real body: the player's resource path/GUID, the streaming manager, Sam's model.", false}, &DoRealProbe},
    {{Action::real_spawn, Tab::tools, "Real spawn", "Spawn a real character body ahead of Sam (body id, distance, mirror 0/1).", false}, &DoRealSpawn},
    {{Action::real_remove, Tab::tools, "Real remove", "Remove the debug real body.", false}, &DoRealRemove},
    {{Action::real_names, Tab::tools, "Real names", "Log Sam's and the real body's joint names.", false}, &DoRealNames},
    {{Action::real_stats, Tab::tools, "Real stats", "Pose hook counters and the partner-vs-published check.", false}, &DoRealStats},
    {{Action::real_mirror, Tab::tools, "Real mirror", "The debug real body copies Sam's live pose (1) or keeps its own (0).", false}, &DoRealMirror},
    {{Action::real_control, Tab::tools, "Real control", "Load the n-th streamed-out collectors hologram through the real body's path+GUID route.", false}, &DoRealControl},
    {{Action::self_view, Tab::tools, "Self view", "Show yourself as a character in place of Sam on this screen (1/0, body id, follow mode).", false}, &DoSelfView},
    {{Action::real_outfit, Tab::tools, "Real outfit", "Partner Sam's outfit: 0 log, 1 like your Sam, 2 exos + suit colour, 3 override off. Fragile's porter suit: 5 c colour (-1 follow), 6 log. Colour menu: 9 mask (1 byte follows her pick, default on; 2 Fragile in it, default off), 10 log, 11 open it (hidden runs), 12 note a suit colour preview (hidden runs).", false}, &DoRealOutfit},
    {{Action::name_tags, Tab::tools, "Name tags", "Name tags: 0 normal, 1 self test above your own Sam, 2 log the camera, 3 0/1 off/on.", false}, &DoNameTags},
    {{Action::collision, Tab::tools, "Collision", "Player collision: 0 log, 1 0/1 off/on, 2 ms trace, 3 m pin the partner ahead, 4 layer type.", false}, &DoCollision},
    {{Action::gear, Tab::tools, "Gear", "Gear on the bodies: 0 log, 1 0/1 partner props, 2 0/1 self-view props, 4 0/1 own gear on the debug body, 7 0..2 mirror level, 9 0/1/2 census.", false}, &DoGear},
    {{Action::smooth, Tab::tools, "Smooth", "Body animation above 60 fps: 0 log, 1 mask (0..7), 2 frame cap fps (0 off), 3 record n frames.", false}, &DoSmooth},
    {{Action::audio_probe, Tab::tools, "Audio probe", "Audio probe (audio_probe=1): 0 summary, 1 dump, 2 n test situation, 3 player components, 4 reset.", false}, &DoAudioProbe},
    {{Action::self_voice, Tab::tools, "Self voice", "Sam's voice muted for a non-Sam player (self_voice): 0 log, 1 0/1 mute off/on, 2 n cargo menu figure says list n.", false}, &DoSelfVoice},
    {{Action::voice_sync, Tab::tools, "Voice sync", "Partner voice (voice_sync=1): 0 log, 1 n play sentence n on the partner, 2 0/1 test: send as Sam, 3 rebuild the map.", false}, &DoVoiceSync},
    {{Action::sound_sync, Tab::tools, "Sound sync", "Partner sounds (sound_sync=1): 0 log, 1 0/1 log every mirrored post, 2 stop the partner's sounds.", false}, &DoSoundSync},
    {{Action::vehicle_sync, Tab::tools, "Vehicle sync", "Vehicles (vehicle_sync): 0 log, 1 mode 0 off/1, 2 solid/3 holograms/4 roadster, 2 copy override 0..5, 3 trace ms, 4 offset right/forward cm, 5 describe, 6 offset up cm, 7 list, 11 overlap hiding 0/1, 13 meters.", false}, &DoVehicleSync},
    {{Action::coop_session, Tab::tools, "Coop session", "Host-held save session (host_save=1): 0 log, 1 ms test link blackout, 2 offer the newest save (host), 3 accept the teleport offer (guest), 4 log the slots (host), 5 t i test load (host), 6 test quit, 7 [1|2] test retry [query|forced] (host), 8 0/1/2 test: hide the waiting panel [and lift the input block] (guest), 9 t i test: a slot stands in for a New Game's first save (host); 1 and 5-9 in hidden runs only.", false}, &DoCoopSession},
    {{Action::vehicle_share, Tab::tools, "Vehicle share", "Real shared vehicles (vehicle_sync=5, host_save=1): 0 status, 1 dump, 2 kind [m] summon, 3 snapshot now, 4 binding report, 5 0..3 mirror path (tests), 6 kind [m] test removal, 7 counters, 8 [kind] [m] persisted entries (host).", false}, &DoVehicleShare},
    {{Action::vehicle_dump, Tab::tools, "Vehicle dump", "Every vehicle unit in use (roadster, truck, trike pools) with its id, flags, GUID, place and binding, to the log. v0 = tag.", false}, &DoVehicleDump},
    {{Action::vehicle_summon, Tab::tools, "Vehicle summon", "Summon a vehicle through the game's own request path: v0 kind (1 roadster, 2 truck, 3 trike), v1 metres ahead (default 8). On the guest the request goes to the host.", false}, &DoVehicleSummon},
    {{Action::vehicle_drive, Tab::tools, "Vehicle drive", "Real shared vehicles, remote driving (vehicle_sync=5): 0 status, 1 vehicles (owner, follower, errors), 2 0/1 native correction, 3 parked m / driven m ignore bands, 4 0/1 corrections (tests), 5 0/1 native lamps / sounds, 6 kind [m] test push (tests), 7 life test (tests), 8 battery probe, 9 [m] our Sam beside the partner's vehicle (tests); v0.21: 10 dv one odd drive frame (tests), 11 fwd right step response (tests), 12 m/s laps owner autopilot on play test 3's road (tests), 13 0/1 v0.20's follower, 14 place (tests), 15 ms input lead, 16 kp ki, 17 kyaw tau, 18 max dv / correction, 19 s controller trace, 20 sign max pursuit.", false}, &DoVehicleDrive},
    {{Action::ride_state, Tab::tools, "Ride state", "Riding together (vehicle_sync=5): the state, the counters and the seats.", true}, &DoRideState},
    {{Action::ride_board, Tab::tools, "Ride board", "Ride the partner's vehicle now (as a 0.6 s hold of X beside its door).", true}, &DoRideBoard},
    {{Action::ride_exit, Tab::tools, "Ride exit", "Get off the partner's vehicle now (1: also above 2 m/s, tests).", false}, &DoRideExit},
    {{Action::seat_measure, Tab::tools, "Seat measure", "The vehicle our Sam drives (or last drove): his root and pelvis, the game's seat helpers, in its frame.", false}, &DoSeatMeasure},
    {{Action::ride_test, Tab::tools, "Ride test", "Hidden tests: 1 solo seat carry, 2 end, 4 crash, 5 flip / 6 stop our vehicle, 7 taken, 30-33 seat tuning; v0.21: 15 shield and health state, 16 0/1 landing shield and health guard off / on, 17 0/1 v0.20's crash rule / the confirmed one, 18 0/1 health write watchpoint; passenger pose / camera: 20 0/1 ride camera, 21 0/1 camera meter 1 s, 22 0/1 pose v0.20 / seated idle, 23 <what> <v> pose tuning, 24 pose state, 25 0/1 camera ignores the vehicle, 26 0/1 hold the seat teleport while stopped, 27 0/1/2 yaw quaternion, 28 0/1 heading follow; v0.22: 34 0/1 boots meter 1 s, 34 2 boots mark, 35 0/1 boots guard off / on.", false}, &DoRideTest},
    {{Action::vehicle_hit, Tab::tools, "Vehicle hit", "Vehicle hits (vehicle_sync=5): 0 state and counters, 1 kind path (0 auto, 1 direct, 2 bracket), 2 one hit on our Sam now (tests), 8 [m ahead] [m right] our Sam in front of the partner's vehicle (tests), 9 the capsule's vehicle boxes.", false}, &DoVehicleHit},
    {{Action::save_guard, Tab::tools, "Save guard", "No save captures Sam while he rides as a passenger: 0 state, 1 [type] a save now (tests: 4 auto, 1 manual), 2 Sam's place vs the last held save, 3 load the run's newest save (host, tests), 4 0/1 the hold off/on (tests), 5 why a save is refused now (tests), 6 / 7 [s] the next forced save stays pending until a boarding / without one (tests).", false}, &DoSaveGuard},
    // Dev tools (the Dev tools page draws them with its own widgets; the labels are the hidden scenarios' action names)
    {{Action::dev_status, Tab::dev, "Dev status", "Dev tools: 0 log which tools this session allows; 1 0/1 test: act as on a real save (hidden runs).", false},
     &DoDev<dev::Op::status>},
    {{Action::dev_vehicle, Tab::dev, "Dev vehicle", "Spawn a vehicle ahead of Sam: kind (1 roadster, 2 truck, 3 trike), variant, metres (sandboxed saves only).", false},
     &DoDev<dev::Op::vehicle>},
    {{Action::dev_vehicle_remove, Tab::dev, "Dev vehicle remove", "Remove the nearest vehicle nobody is on (within m, default 15; sandboxed saves only).", false},
     &DoDev<dev::Op::vehicle_remove>},
    {{Action::dev_vehicle_fix, Tab::dev, "Dev vehicle fix", "Repair and recharge the nearest vehicle (within m, default 10; sandboxed saves only).", false},
     &DoDev<dev::Op::vehicle_fix>},
    {{Action::dev_refill, Tab::dev, "Dev refill", "Health, stamina, battery and blood to full (the game's RecoverFull).", false}, &DoDev<dev::Op::refill>},
    {{Action::dev_god, Tab::dev, "Dev god", "God mode: 0 off, 1 on, 2 invulnerable (the game's DebugSettings).", false}, &DoDev<dev::Op::god>},
    {{Action::dev_ammo, Tab::dev, "Dev ammo", "Infinite ammo 0/1 (the game's DebugSettings).", false}, &DoDev<dev::Op::ammo>},
    {{Action::dev_weather, Tab::dev, "Dev weather", "Weather in this area: 0 clear, 1 timefall, 2 heavy timefall, 3 the game's mix (sandboxed saves only).", false},
     &DoDev<dev::Op::weather>},
    {{Action::dev_enemies, Tab::dev, "Dev enemies", "1 resurrect every MULE / terrorist, 2 remove them, 3 reset the BTs (sandboxed saves only).", false},
     &DoDev<dev::Op::enemies>},
    {{Action::dev_places, Tab::dev, "Dev places", "The current area's places (delivery points) to the log.", false}, &DoDev<dev::Op::places>},
    {{Action::dev_place, Tab::dev, "Dev place", "Sam beside place n (nearest first) of kind k (0 any, -1 delivery base, 5 post box, 20 MULE post), outside its structure, facing it; 3rd number (hidden runs): 1 its own point, 2 test the retry (its own point first).", false}, &DoDev<dev::Op::place>},
    {{Action::dev_items, Tab::dev, "Dev items", "1 new boots, 2 n chiral crystals (sandboxed saves only).", false}, &DoDev<dev::Op::items>},
    {{Action::story, Tab::dev, "Story", "Story state: 0 the episode and orders to the log, 1 every main / sub mission's state, 2 the manual slots (story points), 3 the load's gate here.", false},
     &DoStory},
    {{Action::story_load, Tab::dev, "Story load", "Load story point manualsave<n> (the story library's are 10..19; sandboxed saves only, never on the guest).", false},
     &DoStoryLoad},
    {{Action::dev_goto, Tab::dev, "Dev goto", "Teleport Sam anywhere in this area: x y [z] (z 0 = the topmost ground), then wait for the ground to stream in (sandboxed saves only).", false},
     &DoDev<dev::Op::go_to>},
    {{Action::dev_boots, Tab::dev, "Dev boots", "Boots: 0 log, 1 <percent> set the life (sandboxed saves only), 2 <0/1> a 1 s meter in the log, 3 <type 0..10> equip that boot type (sandboxed saves only).", false},
     &DoDev<dev::Op::boots>},
    {{Action::world_dump, Tab::dev, "World dump", "Structures (world sync): the census to the log: counts per category and net type, the nearest n (category c or -1 all, n default 16). Read only.", false},
     &DoWorld<0>},
    {{Action::world_sync, Tab::dev, "World sync", "Shared structures: 0 status, 1 snapshot now, 2 binding report, 3 counters (and the online gate's), 4 <id> one structure (and its materials), 5 <0/1> hold sending (tests), 10 <type|-1> <m> count near Sam, 11 <id|0> record round trip, 12 <id | 0> [m] Sam next to a structure (hidden runs), 13 the online gate, 14 <id | 0 nearest ladder / rope | -1 off> watch its flags, 15 <id | 0> [deg] aim the camera at a structure (hidden runs), 16 <id | 0 all within m> [m] zipline links.", false},
     &DoWorld<1>},
    {{Action::dev_build, Tab::dev, "Dev build", "A structure in front of Sam: <type> <anim 0/1> [m ahead, default 8] (the game's own queue, offline; sandboxed saves only; the guest asks the host).", false},
     &DoWorld<2>},
    {{Action::dev_build_remove, Tab::dev, "Dev build remove", "Remove a structure: <id | 0 the nearest synced one> [1 dismantle (RemoveManually) | 0 the removal bits] (sandboxed saves only).", false},
     &DoWorld<3>},
    {{Action::dev_build_decay, Tab::dev, "Dev build decay", "A structure's timefall durability: <id | 0 nearest> <seconds> (host, sandboxed saves only).", false},
     &DoWorld<4>},
    {{Action::dev_build_mats, Tab::dev, "Dev build mats", "Contribute to a structure as its terminal does, without the UI or the inventory: <id | 0 nearest> <0 materials | 1 repair> <share 0..1 of what the next level still needs | repair units> (sandboxed saves only; the guest's go to the host).", false},
     &DoWorld<5>},
    {{Action::dev_ladder, Tab::dev, "Dev ladder", "A ladder in front of Sam as the player's commit places one: <length m, 0 = 9.65> <mode 0/1> [m ahead, default 3] (offline; sandboxed saves only; the guest asks the host).", false},
     &DoWorld<6>},
    {{Action::dev_rope, Tab::dev, "Dev rope", "A climbing anchor with its rope in front of Sam: <length 1..30 m> <level 0..2> [m ahead, default 3] (offline; sandboxed saves only; the guest asks the host).", false},
     &DoWorld<7>},
    {{Action::tex, Tab::tools, "Tex", "Fragile's edited textures: 0 status, 1 rescan the folder, 7 resend the look; hidden runs: 2 probe dump, 3 c colour (-1 follow), 4 0/1 off/on, 5 filter 0/1/2, 9 n test pattern.", false},
     &DoTex},
    {{Action::enemy_census, Tab::tools, "Enemy census", "Enemy sync P0, read only: every MULE / Demens / warrior actor with its GUID, state, place and entity to the log: <tag> [radius m, 0 all] [flags: 1 every other actor type, 2 the MULE killed records].", false},
     &DoEnemyCensus},
    {{Action::enemy_probe, Tab::tools, "Enemy probe", "Enemy sync P1-0, hidden runs only: 0 [m] check and pick the nearest realized MULE, 3 s path the next tests' length and path, 1 lever mode freeze / drive / release / watch, 2 s our Sam's pose onto it, 9 release.", false},
     &DoEnemyProbe},
    {{Action::shots, Tab::tools, "Shots", "The players' shots on the other screen (shots_sync): 0 status, 1 on, 2 off, 3 m deg our Sam beside the partner body facing it, 4 [deg] [1 steered, 2 steered then 2 s of fire] aim the camera at it, 5 target n aimed test, 6 cargo target, 8 muzzle magazine, 9 yaw pitch aim offset, 10 m the enemy replays' start along the shot (hidden runs).", false}, &DoShots},
    {{Action::shots_test, Tab::tools, "Shots test", "One replayed bullet at our own Sam (the last captured shot's ids): m ahead, flags (1 the filter keeps it, 2 no weapon, 4 no magazine, 16 leak test), count.", false},
     &DoShotsTest},
    {{Action::shots_echo, Tab::tools, "Shots echo", "Solo: our own shots replayed beside us: 0/1, m to the right (default 2.5).", false}, &DoShotsEcho},
    {{Action::enemy_sync, Tab::tools, "Enemy sync", "Enemy mirrors (enemy_sync): 0 status, 1 on, 2 off (every mirror released), 3 the mirror table, 4 the hits' outcome table; ownership (P1b: only with enemy_owner=1 on both games, off by default and not offered to players yet): 5 its table, 8 0/1 off (everything back to the host) / on; hidden runs: 6 0/1 far placement variant, 7 0/3 the teleport cadence.", false},
     &DoEnemySync},
    {{Action::enemy_look, Tab::tools, "Enemy look", "Hidden runs: our Sam m from an enemy, facing it: <id | 0 nearest (the guest: a frozen mirror) | -1 the one the partner faces | -2 the newest replayed (the host: sent) shooter | -3 behind it | -4 beside its line | -5 the newest HIT's enemy | -6 the probe's pick | -7 the nearest the guest owns | -8 in front of the newest one, along its facing> <m> [aim height m, default 1.2]; a steered aim (shots 4 <deg> 1) then follows it.", false},
     &DoEnemyLook},
    {{Action::enemy_shot_test, Tab::tools, "Enemy shot test", "Hidden runs, the guest: 3 enemy replays level at our Sam's chest from 2 m, from a mirror's side (its hand's line and 50 deg either side): <id | 0 the last looked-at, else nearest> [1 nohurt].", false},
     &DoEnemyShotTest},
    {{Action::enemy_dump, Tab::tools, "Enemy dump", "Hidden runs (enemy hits): a MULE's actor (0..0x1000), its damage component (0..0x700) and entity +0x80..+0xA0 as hex lines: <id | 0 the probe's pick / the newest HIT's / the nearest> <tag>.", false},
     &DoEnemyDump},
    {{Action::enemy_attack, Tab::tools, "Enemy attack", "Hidden runs (enemy hits): an attack's registry entry (damage, stun, ranges, flags): <attack id>.", false},
     &DoEnemyAttack},
    {{Action::enemy_ctx, Tab::tools, "Enemy ctx", "Hidden runs (enemy hits): an attack context made and inspected, nothing delivered: <attack> <owner 1 body | 2 none | 3 sam>.", false},
     &DoEnemyCtx},
    {{Action::enemy_hit_mode, Tab::tools, "Enemy hit mode", "Hidden runs (enemy hits): the next enemy_hit's part (-1 none, -2 head, -3 chest, >= 0 an index), owner (0 credit, 1 body, 2 none, 3 sam, -1 the session's), path (0 direct, 1 query, -1).", false},
     &DoEnemyHitMode},
    {{Action::enemy_hit, Tab::tools, "Enemy hit", "Hidden runs (enemy hits): hits applied on this game with the host's apply code, 150 ms apart, then the effect line: <attack> [count] [id | 0].", false},
     &DoEnemyHit},
    {{Action::enemy_hit_send, Tab::tools, "Enemy hit send", "Hidden runs, the guest (enemy hits): test HITs (kind 4) at a mirror's chest to the host: <attack> [count] [id | 0 the newest look's].", false},
     &DoEnemyHitSend},
    {{Action::enemy_hit_watch, Tab::tools, "Enemy hit watch", "Hidden runs (enemy hits): every DamageInfo on one enemy logged at OnHit and the MULE handler (its hits pass): <id | 0> <0 off | 1 on>.", false},
     &DoEnemyHitWatch},
    {{Action::enemy_target, Tab::tools, "Enemy target", "Hidden runs, the host (round 20 M1 probe): <op> [id | 0] [arg]: 0 read her body / our Sam / the MULE (AI agent, faction, hostility), 1 / 2 L1 alert (perfect / plain) -> 0 her body | 1 our Sam, 3 L3 phase 1..5, 4 0/1 her body's hit watch, 7 s follow (the MULE's facing), 8 the visual stimuli read, 9 <0 remove | 1 her agent | 2 + Sam's message | 3 the decoy> her body's visual stimulus (round 20 M2).", false},
     &DoEnemyTarget},
    {{Action::enemy_own, Tab::tools, "Enemy own", "Hidden runs, the host (enemy_owner): a forced hand-over: <id | 0 the nearest the host owns to the partner | -5 the nearest the guest owns> <1 to the host | 2 to the guest>.", false},
     &DoEnemyOwn},
    {{Action::dev_marker, Tab::dev, "Dev marker", "Map markers: 0 log them, 1 <m east> <m north> add one there through the game's own add (hidden runs only).", false},
     &DoDev<dev::Op::marker_test>},
};
constexpr std::size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);
static_assert(kRowCount == static_cast<std::size_t>(Action::count), "every Action needs exactly one registry row");

template <std::size_t... I>
constexpr std::array<ActionInfo, kRowCount> MakeInfos(std::index_sequence<I...>) {
    return {{kRows[I].info...}};
}
constexpr std::array<ActionInfo, kRowCount> kInfos = MakeInfos(std::make_index_sequence<kRowCount>{});

Handler g_test_handlers[static_cast<std::size_t>(Action::count)] = {};

const Row* RowFor(Action a) noexcept {
    for (const Row& r : kRows)
        if (r.info.id == a) return &r;
    return nullptr;
}

// ------------------------------------------------------------------ queue
constexpr std::size_t kQueueCap = 64;
SRWLOCK g_queue_lock = SRWLOCK_INIT;
Request g_queue[kQueueCap];
std::size_t g_queue_count = 0;
std::uint32_t g_next_serial = 1;

constexpr std::size_t kResultCap = 32;
SRWLOCK g_result_lock = SRWLOCK_INIT;
Result g_results[kResultCap];
std::size_t g_result_head = 0, g_result_count = 0;
double g_last_tick_ms = 0;

void AddResult(const Result& r) noexcept {
    SrwGuard g(g_result_lock);
    g_results[(g_result_head + g_result_count) % kResultCap] = r;
    if (g_result_count < kResultCap) ++g_result_count;
    else g_result_head = (g_result_head + 1) % kResultCap;
}

// ------------------------------------------------------------------ snapshot
SRWLOCK g_snap_lock = SRWLOCK_INIT;
Snapshot g_snap;
std::uint64_t g_snap_serial = 0;
double g_next_publish_ms = 0;
char g_mod_state[96] = "starting";
char g_role[16] = "none";
volatile LONG g_menu_open = 0;

}  // namespace

const ActionInfo* Actions(std::size_t* count) noexcept {
    if (count) *count = kRowCount;
    return kInfos.data();
}

const ActionInfo* Find(Action a) noexcept {
    const Row* r = RowFor(a);
    return r ? &r->info : nullptr;
}

const char* ActionName(Action a) noexcept {
    const Row* r = RowFor(a);
    return r ? r->info.label : "?";
}

std::uint32_t Queue(const Request& in) noexcept {
    if (in.action >= Action::count) return 0;
    SrwGuard g(g_queue_lock);
    if (g_queue_count >= kQueueCap) return 0;
    Request r = in;
    r.serial = g_next_serial++;
    if (g_next_serial == 0) g_next_serial = 1;
    g_queue[g_queue_count++] = r;
    return r.serial;
}

std::uint32_t Queue(Action a) noexcept {
    Request r;
    r.action = a;
    return Queue(r);
}

std::size_t Pending() noexcept {
    SrwGuard g(g_queue_lock);
    return g_queue_count;
}

std::size_t Drain() noexcept {
    Request batch[kQueueCap];
    std::size_t n = 0;
    {
        SrwGuard g(g_queue_lock);
        n = g_queue_count;
        for (std::size_t i = 0; i < n; ++i) batch[i] = g_queue[i];
        g_queue_count = 0;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const Request& r = batch[i];
        const Row* row = RowFor(r.action);
        if (!row) continue;
        Handler h = g_test_handlers[static_cast<std::size_t>(r.action)];
        if (!h) h = row->handler;
        Result res;
        res.serial = r.serial;
        res.action = r.action;
        res.time_ms = g_last_tick_ms;
        res.ok = h(r, res.text, sizeof(res.text));
        Log("[debug] action #%u %s: %s%s", r.serial, row->info.label, res.ok ? "" : "FAILED: ", res.text);
        AddResult(res);
    }
    return n;
}

void OnTick(double now_ms) noexcept {
    g_last_tick_ms = now_ms;
    game::ActionsTick(now_ms);  // resolves game_actions once, post-teleport checks
    dev::Tick(now_ms);          // the dev tools' checks, place list and page view
    Drain();
    const bool open = MenuOpen();
    if (open || now_ms >= g_next_publish_ms) {
        g_next_publish_ms = now_ms + 500;
        Snapshot s;
        FillSnapshot(s, now_ms);
        Publish(s);
    }
}

namespace {
void FillSnapshot(Snapshot& s, double now_ms) {
    s.time_ms = now_ms;
    {
        SrwGuard g(g_snap_lock);  // SetRoleLabel writes g_role from another thread
        Copy(s.role, g_role);
    }
    sync::FillDebugSnapshot(s);
    game::ReadDiagnostics(&s.diag);
}

bool DoCopyDiagnostics(const Request&, char* msg, std::size_t cap) {
    Snapshot s;
    FillSnapshot(s, g_last_tick_ms);
    {
        SrwGuard g(g_snap_lock);
        Copy(s.mod_state, g_mod_state);
    }
    static char text[4096];
    const std::size_t n = FormatDiagnostics(s, text, sizeof(text));
    // One log line per report line.
    for (char* line = text; *line;) {
        char* end = std::strchr(line, '\n');
        if (end) *end = 0;
        Log("[debug] diag: %s", line);
        if (!end) break;
        *end = '\n';
        line = end + 1;
    }
    const bool ok = SetClipboardUtf8(text);
    std::snprintf(msg, cap, ok ? "%zu bytes copied to the clipboard (and the log)" : "clipboard busy; report written to the log", n);
    return ok;
}
}  // namespace

std::size_t FormatDiagnostics(const Snapshot& s, char* out, std::size_t cap) noexcept {
    if (!out || !cap) return 0;
    out[0] = 0;
    std::size_t n = 0;
    auto add = [&](const char* fmt, auto... args) {
        if (n + 1 >= cap) return;
        const int w = std::snprintf(out + n, cap - n, fmt, args...);
        if (w < 0) return;
        n += static_cast<std::size_t>(w);
        if (n >= cap) n = cap - 1;
    };
    const game::Diagnostics& d = s.diag;
    char a[192], b[192];
    add("Stranded Together diagnostics (tick %.0f ms)\n", s.time_ms);
    add("role=%s mod=%s link=%s %s %s peer=%s rtt=%u ms rx=%llu tx=%llu\n", s.role, s.mod_state, s.link_state, s.transport,
        s.route, s.peer[0] ? s.peer : "-", s.rtt_ms, static_cast<unsigned long long>(s.rx), static_cast<unsigned long long>(s.tx));
    add("sam=%p present=%d valid=%d awake=%d flags=0x%llX pos=(%.2f, %.2f, %.2f) speed=%.2f\n",
        reinterpret_cast<void*>(s.local_entity), s.local_present ? 1 : 0, s.local_valid ? 1 : 0, s.local_awake ? 1 : 0,
        static_cast<unsigned long long>(s.local_flags), s.local_pos[0], s.local_pos[1], s.local_pos[2], static_cast<double>(s.speed));
    add("partner=%p state=%s source=%s flags=0x%llX pos=(%.2f, %.2f, %.2f)%s remote=%s age=%.0f ms\n",
        reinterpret_cast<void*>(s.partner_entity), s.partner_state, s.partner_source[0] ? s.partner_source : "-",
        static_cast<unsigned long long>(s.partner_flags), s.partner_pos[0], s.partner_pos[1], s.partner_pos[2],
        s.partner_pos_valid ? "" : " (invalid)", s.remote_fresh ? "fresh" : "stale", s.remote_age_ms);
    if (!d.resolved) {
        add("game: not read (no game)\n");
        return n;
    }
    add("game checks: features=0x%X%s%s\n", d.features, d.disabled[0] ? " disabled: " : " (all ok)", d.disabled);
    if (d.state_valid) {
        game::DescribeGameStateFlags(d.state_flags, a, sizeof(a));
        game::DescribeGameStateFlags(d.state_requested, b, sizeof(b));
        add("game state: 0x%llX [%s] requested 0x%llX [%s] phase %d%s\n", static_cast<unsigned long long>(d.state_flags), a,
            static_cast<unsigned long long>(d.state_requested), b, d.state_phase, d.state_phase == 2 ? " (setup)" : "");
    } else {
        add("game state: n/a\n");
    }
    if (d.module_valid) {
        game::DescribeLoadMask(d.load_mask, a, sizeof(a));
        add("load mask: 0x%X [%s] reason %u\n", d.load_mask, a, d.load_reason);
    } else {
        add("load mask: n/a\n");
    }
    if (d.player_system_valid) add("pushed player transform: %s\n", d.pushed_transform ? "PENDING" : "no");
    if (d.area_valid) {
        const char* an = game::AreaName(d.area);
        add("area: %u %s\n", d.area, an ? an : "?");
    } else {
        add("area: n/a (only read while an area change is active)\n");
    }
    if (d.tod_valid) {
        game::FormatClock(d.time_of_day, a, sizeof(a));
        add("time of day: %s (%.3f h) day %d, cycle %s\n", a, static_cast<double>(d.time_of_day), d.day,
            d.day_night_cycle ? "on" : "FROZEN");
    } else {
        add("time of day: n/a\n");
    }
    if (d.health_valid) add("health: %.1f / %.1f\n", static_cast<double>(d.health), static_cast<double>(d.health_max));
    else add("health: n/a\n");
    add("teleport: %s%s\n", d.teleport_ready ? "ready" : "blocked: ", d.teleport_ready ? "" : d.teleport_block);
    if (d.last_teleport[0]) add("last teleport: %s\n", d.last_teleport);
    return n;
}

std::size_t RecentResults(Result* out, std::size_t max) noexcept {
    SrwGuard g(g_result_lock);
    const std::size_t n = g_result_count < max ? g_result_count : max;
    const std::size_t skip = g_result_count - n;
    for (std::size_t i = 0; i < n; ++i) out[i] = g_results[(g_result_head + skip + i) % kResultCap];
    return n;
}

bool ResultFor(std::uint32_t serial, Result* out) noexcept {
    SrwGuard g(g_result_lock);
    for (std::size_t i = g_result_count; i-- > 0;) {
        const Result& r = g_results[(g_result_head + i) % kResultCap];
        if (r.serial == serial) {
            if (out) *out = r;
            return true;
        }
    }
    return false;
}

void Publish(const Snapshot& s) noexcept {
    SrwGuard g(g_snap_lock);
    g_snap = s;
    g_snap.serial = ++g_snap_serial;
    Copy(g_snap.mod_state, g_mod_state);
}

Snapshot GetSnapshot() noexcept {
    AcquireSRWLockShared(&g_snap_lock);
    Snapshot s = g_snap;
    ReleaseSRWLockShared(&g_snap_lock);
    if (s.serial == 0) {
        SrwGuard g(g_snap_lock);
        Copy(s.mod_state, g_mod_state);
        Copy(s.role, g_role);
    }
    return s;
}

void SetModState(const char* text) noexcept {
    SrwGuard g(g_snap_lock);
    Copy(g_mod_state, text);
    Copy(g_snap.mod_state, text);
}

const char* RoleLabel() noexcept { return g_role; }
void SetRoleLabel(const char* role) noexcept {
    SrwGuard g(g_snap_lock);
    Copy(g_role, role);
}

void SetMenuOpen(bool open) noexcept { InterlockedExchange(&g_menu_open, open ? 1 : 0); }
bool MenuOpen() noexcept { return InterlockedCompareExchange(&g_menu_open, 0, 0) != 0; }

void SetHandlerForTest(Action a, Handler fn) noexcept {
    if (a < Action::count) g_test_handlers[static_cast<std::size_t>(a)] = fn;
}

void ResetForTest() noexcept {
    {
        SrwGuard g(g_queue_lock);
        g_queue_count = 0;
    }
    {
        SrwGuard g(g_result_lock);
        g_result_count = g_result_head = 0;
    }
    for (Handler& h : g_test_handlers) h = nullptr;
}

}  // namespace st::debug

