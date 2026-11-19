// Game-side debug actions and read-only diagnostics for the in-game menu
// (docs/debug-actions.md): teleport (PlayerGame::PlaceOn), unstuck, heal,
// time of day, partner voice ping, and the game-state / load / area / time /
// health readout shown on the Status tab.
//
// Rules (DS's crash reporter fires on ANY access violation, even a handled one):
//   * Every action runs on the game's main thread (st::debug::OnTick, called
//     from st::sync::Tick). Never call these from the render thread.
//   * Game memory is only read with ReadProcessMemory (fails instead of
//     faulting); pointers are validated (vtable in ds.exe, RTTI type) before use.
//   * Every game function and every global/offset is checked against the exact
//     ds.exe bytes at its known RVA once (Resolve). A mismatch disables the
//     features that depend on it and logs why; nothing is ever called blind.
//
// The pure decoders below (flag names, area names) are inline so the overlay
// tests can check them without the game.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace st::game {

// ------------------------------------------------------------------ actions
enum class TeleportMode : int {
    // PlaceOn mode 1: place at (x, y, z) with a short ground probe (ray z+2 m
    // .. z-2 m, nearest hit; keeps z when nothing is hit). Use when z is known
    // (partner position, saved position, typed coordinates).
    place = 1,
    // PlaceOn mode 2: place at (x, y, z), then on the next GameModule update
    // the game casts one ray z+2000 m .. z-2000 m and puts Sam on the topmost
    // surface. Use for "x, y only" (z = ray centre; kSnapRayCentreZ covers the map).
    snap_to_ground = 2,
};
constexpr double kSnapRayCentreZ = 500.0;
constexpr double kTeleportMinIntervalMs = 500.0;

// All functions below: game thread only. `msg` (may be null) receives one
// human-readable result line; the return value is success.
//
// THE local-player teleport. Every teleport in the menu (coordinates, saved
// position, partner, snap) ends here. Checks all preconditions first (see
// docs/debug-actions.md) and verifies the result on later frames (log).
bool TeleportLocalPlayer(double x, double y, double z, TeleportMode mode, char* msg, std::size_t cap) noexcept;
// The same with Sam turned to face (fx, fy) (horizontal; hidden tests: a vehicle's Use Vehicle prompt, a watching point).
bool TeleportLocalPlayerFacing(double x, double y, double z, double fx, double fy, TeleportMode mode, char* msg, std::size_t cap) noexcept;
inline bool TeleportLocalPlayer(double x, double y, double z) noexcept {
    return TeleportLocalPlayer(x, y, z, TeleportMode::place, nullptr, 0);
}
// Teleport (mode 1) to the safe ground transform the game itself records in
// DSPlayerParamComponent (the one its "return to safe position" warp uses).
bool Unstuck(char* msg, std::size_t cap) noexcept;
// amount <= 0: heal to max health.
bool HealLocalPlayer(float amount, char* msg, std::size_t cap) noexcept;
// GameWorldTimeState::SetTimeOfDay(hours, transition 0 = instant). hours in [0, 24).
bool SetTimeOfDay(float hours, char* msg, std::size_t cap) noexcept;
// GameWorldTimeState.EnableDayNightCycle (false = time frozen).
bool SetDayNightCycle(bool enabled, char* msg, std::size_t cap) noexcept;
// DSEchoComponent::Play on the partner entity's echo component: one voice line
// from the partner's position. `partner_entity` is the live partner (0 = none);
// `index` < 0 picks the next line in turn.
bool PartnerVoicePing(std::uintptr_t partner_entity, int index, char* msg, std::size_t cap) noexcept;
// Hologram catalog (docs/character-catalog.md): spawns collectors items [start,
// start + count) of the sorted Type 4 / Type 20 list one at a time `distance_m`
// in front of Sam (0 = default), logs each one, screenshots it (automation) and
// removes it. Runs over many ticks; the log says "[catalog] batch done".
bool StartHoloCatalog(int start, int count, double distance_m, char* msg, std::size_t cap) noexcept;
// Animation clip survey (docs/character-catalog.md): every loaded SkeletonAnimationResource
// with root motion or a walk/run-like path, and which share Sam's skeleton layout. Reads only;
// the memory scan runs on its own thread and ends with "[survey] done".
bool StartClipSurvey(char* msg, std::size_t cap) noexcept;
// Animated-body census, Sam's pose dumps and scan, look-at teleport (anim_census.hpp).
bool StartAnimCensus(double radius_m, int tag, int scan, char* msg, std::size_t cap) noexcept;
bool PoseDump(int tag, int frames, double delay_ms, char* msg, std::size_t cap) noexcept;
bool StartPoseScan(int tag, char* msg, std::size_t cap) noexcept;
bool LookAtBody(int index, double dist_m, char* msg, std::size_t cap) noexcept;
// Real partner body debug commands (real_body.hpp DebugCommand: 1 probe, 2 spawn, 3 remove,
// 4 joint names, 5 stats, 6 mirror).
bool RealBodyCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

// Once per game tick (st::debug::OnTick): resolves on first use and runs the
// post-teleport verification.
void ActionsTick(double now_ms) noexcept;

// ------------------------------------------------------------------ diagnostics
enum Feature : std::uint32_t {
    kFeatTeleport = 1u << 0,
    kFeatUnstuck = 1u << 1,
    kFeatHealth = 1u << 2,
    kFeatHeal = 1u << 3,
    kFeatTime = 1u << 4,
    kFeatArea = 1u << 5,
    kFeatState = 1u << 6,
    kFeatModule = 1u << 7,
    kFeatEcho = 1u << 8,
    kFeatAll = (1u << 9) - 1,
};

struct Diagnostics {
    bool resolved = false;        // the byte checks ran (false: no game / not yet)
    std::uint32_t features = 0;   // Feature bits that passed the byte checks
    char disabled[160] = {};      // "teleport: <check> differs; ..." (empty = all ok)

    // DSGameState: +8 current flags, +0x10 flags requested by script, +0x5E0 post-load phase
    bool state_valid = false;
    std::uint64_t state_flags = 0;
    std::uint64_t state_requested = 0;
    std::int32_t state_phase = 0;  // 2 = "Setup" settle phase after a load

    // GameModule: +0x44C suspend/loading reason mask, +0x450 load-reason byte
    bool module_valid = false;
    std::uint32_t load_mask = 0;
    std::uint8_t load_reason = 0;

    // DSPlayerSystem +0x206D0 bit 0: a pushed transform is pending (a later Pop warps Sam back)
    bool player_system_valid = false;
    bool pushed_transform = false;

    // Area (read-only path: area-change state, only while it is active)
    bool area_valid = false;
    std::uint16_t area = 0xFFFF;

    // GameWorldTimeState (GameModule+0x58)
    bool tod_valid = false;
    float time_of_day = 0;        // hours [0, 24)
    std::int32_t day = 0;         // +0x28 day counter (inferred)
    bool day_night_cycle = false; // +0x2C EnableDayNightCycle

    // Sam
    bool health_valid = false;
    float health = 0, health_max = 0;

    // Teleport readiness right now (cheap reads only; the sequence check runs at call time)
    bool teleport_ready = false;
    char teleport_block[96] = {};
    char last_teleport[160] = {};  // newest teleport and its verification
};
// Game thread. Reads only.
void ReadDiagnostics(Diagnostics* out) noexcept;
// Game thread. Reads only. The DSGameState flags (+8) and the GameModule load mask (+0x44C), the
// two "the world is not being played" signals (name tags hide on kStoppingGameEventMask or any
// load bit). False (both 0) when a byte check failed or a global is not set.
bool ReadStopGates(std::uint64_t* state_flags, std::uint32_t* load_mask) noexcept;
// Game thread. Player collision (partner_collision.cpp): nullptr when the local Sam is on foot in
// the played world, otherwise why not (no player, asleep, loading/suspended, a stopping game event
// such as a menu or cutscene, the post-load setup phase, attached to a parent entity such as a
// vehicle, a ground snap pending; with `sequence` also bound to a sequence, the one game call:
// ControlledEntity_ExportedIsBoundToSequence, byte-checked). Reads only otherwise.
const char* LocalCollisionBlock(bool sequence) noexcept;
// Player collision debug command (partner_collision.cpp): see docs/debug-actions.md "Player collision".
bool CollisionCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Gear debug command (partner_gear.cpp): see docs/debug-actions.md "Gear".
bool GearCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Audio probe debug command (audio_probe.cpp, session.ini audio_probe=1): see docs/debug-actions.md "Audio probe".
bool AudioProbeCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Self voice debug command (self_voice.cpp, session.ini self_voice): see docs/debug-actions.md "Self voice".
bool SelfVoiceCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Partner voice debug command (partner_voice.cpp, session.ini voice_sync=1): see docs/debug-actions.md "Voice sync".
bool VoiceSyncCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Partner sounds debug command (partner_sound.cpp, session.ini sound_sync=1): see docs/debug-actions.md "Sound sync".
bool SoundSyncCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Vehicle sync debug command (vehicle_sync.cpp, session.ini vehicle_sync): see docs/debug-actions.md "Vehicle sync".
bool VehicleSyncCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Host-held save session debug command (coop_session.cpp, session.ini host_save=1): see docs/coop-session.md "Debug action".
bool CoopSessionCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Real shared vehicles debug command (vehicle_share.cpp, vehicle_sync=5 with host_save=1): see docs/debug-actions.md
// "Vehicle share" (op 1 = dump [tag], op 2 = summon kind [m]).
bool VehicleShareCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;
// Real shared vehicles, remote driving (vehicle_drive.cpp): see docs/debug-actions.md "Vehicle drive".
bool VehicleDriveCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

// Name tags debug command (nametags.cpp): 0 normal, 1 self test (a tag above your own Sam),
// 2 log the camera numbers, 3 <0/1> tags off / on.
bool NameTagCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

// ------------------------------------------------------------------ decoders (pure)
// DSGameState flags use EDSGameStateForScript bit values (RTTI enum 0x144C66648).
constexpr std::uint64_t kStoppingGameEventMask = 0x184F407FEC7F7Full;  // IsStoppingGameEvent 0x142DD3760
constexpr std::uint64_t kGameStateSetup = 0x4000000ull;

inline const char* GameStateFlagName(int bit) noexcept {
    static const char* const kNames[] = {
        "TerminalOperation", "TerminalOperationSimple", "PauseMenu", "PrivateRoom", "BaggageSelect", "HandcuffDevice",
        "Cutscene", "WeaponSelector", "Subspace", "KnotSpace", "StrideAreaEvent", "Result", "FastTravel", "GameOver",
        "Faint", "BossBattle", "AfterBeatBoss", "Annihilation", "Movie", "InGameCutscene", "InGameActing",
        "RelocateBaggage", "Loading", "HeartmanRoom", "Renovation", "Onsen", "Setup", "CallingRadio", "SystemPause",
        "AreaChanging", "DirectIntoPrivateRoom"};
    if (bit < 0 || bit >= static_cast<int>(sizeof(kNames) / sizeof(kNames[0]))) return nullptr;
    return kNames[bit];
}

// "Cutscene|Loading|bit40" (or "none"). Returns the length written.
inline std::size_t DescribeGameStateFlags(std::uint64_t flags, char* out, std::size_t cap) noexcept {
    if (!out || !cap) return 0;
    out[0] = 0;
    std::size_t n = 0;
    for (int b = 0; b < 64; ++b) {
        if (!(flags & (1ull << b))) continue;
        char tmp[32];
        const char* name = GameStateFlagName(b);
        if (!name) {
            std::snprintf(tmp, sizeof(tmp), "bit%d", b);
            name = tmp;
        }
        const int w = std::snprintf(out + n, cap - n, "%s%s", n ? "|" : "", name);
        if (w < 0 || static_cast<std::size_t>(w) >= cap - n) {
            n = cap - 1;
            break;
        }
        n += static_cast<std::size_t>(w);
    }
    if (!n) {
        std::snprintf(out, cap, "none");
        n = std::strlen(out);
    }
    return n;
}

// GameModule+0x44C bits (meanings inferred from the code that sets them).
inline std::size_t DescribeLoadMask(std::uint32_t mask, char* out, std::size_t cap) noexcept {
    if (!out || !cap) return 0;
    struct Bit {
        std::uint32_t bit;
        const char* name;
    };
    static const Bit kBits[] = {{0x2, "loading/area change"}, {0x4, "pending"}, {0x8, "pending2"},
                                {0x40, "streaming not ready"}, {0x80, "streaming settle"}, {0x100, "saving"}};
    out[0] = 0;
    std::size_t n = 0;
    std::uint32_t rest = mask;
    for (const Bit& b : kBits) {
        if (!(mask & b.bit)) continue;
        rest &= ~b.bit;
        const int w = std::snprintf(out + n, cap - n, "%s%s", n ? "|" : "", b.name);
        if (w < 0 || static_cast<std::size_t>(w) >= cap - n) return cap - 1;
        n += static_cast<std::size_t>(w);
    }
    if (rest) {
        const int w = std::snprintf(out + n, cap - n, "%s0x%X", n ? "|" : "", rest);
        if (w < 0 || static_cast<std::size_t>(w) >= cap - n) return cap - 1;
        n += static_cast<std::size_t>(w);
    }
    if (!n) {
        std::snprintf(out, cap, "none");
        n = std::strlen(out);
    }
    return n;
}

// EDSArea values.
inline const char* AreaName(std::uint16_t area) noexcept {
    switch (area) {
        case 0: return "Area00";
        case 100: return "Area01";
        case 200: return "Area02";
        case 300: return "Area03";
        case 400: return "Area04";
        case 500: return "Warrior01";
        case 510: return "Warrior02";
        case 520: return "Warrior03";
        case 600: return "Beach01";
        case 700: return "Frange01";
        case 1100: return "Nm01";
        case 1200: return "Nm02";
        case 1400: return "Nm04";
        case 0xFFFF: return "Empty";
        default: return nullptr;
    }
}

// Sanity range for teleport targets (map coordinates are a few km).
inline bool TeleportTargetSane(double x, double y, double z) noexcept {
    const auto finite = [](double v) { return v == v && v - v == 0; };  // not NaN, not inf
    if (!finite(x) || !finite(y) || !finite(z)) return false;
    return x > -50000 && x < 50000 && y > -50000 && y < 50000 && z > -10000 && z < 10000;
}

// Hours -> "14:30".
inline void FormatClock(float hours, char* out, std::size_t cap) noexcept {
    if (!out || !cap) return;
    if (!(hours >= 0.0f && hours < 48.0f)) {
        std::snprintf(out, cap, "?");
        return;
    }
    int minutes = static_cast<int>(hours * 60.0f + 0.5f) % (24 * 60);
    std::snprintf(out, cap, "%02d:%02d", minutes / 60, minutes % 60);
}

}  // namespace st::game
