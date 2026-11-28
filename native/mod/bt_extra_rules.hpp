// Round 31 stage 8 (bt_extra): this feature's own pure logic (no game, no Windows), tested by tests/bt_extra_tests.cpp. The
// shared codecs, caps and the stage-1 gates stay in bt_rules.hpp (included: BtsRespawn, RespawnApplyGate, the kill reasons);
// new pure helpers of this feature go here, so the seven builders never edit one file at once.
//
// The three parts (the round 31 build spec, Stage 8):
//   kill look  the +0xCC reason of an applied partner kill: 0x5414860D (stats d 0 / 0, PF 21) with bt_kill_look=1, else 0 (bt_world's
//              rain-gone 0x01322187). Never 0x74FB05EE / 0x523D4DC0: those credit the local player's stats (+1 / +1).
//   RESPAWN    the host's EnableRespawnGazers 0x1429A76D0(scene) told to a sandboxed guest (16 RESPAWN, cap 0x2000). The scene's id is
//              GetUUIDHashOfGazerScene 0x1429A71E0 = crc32c over the scene's GUID (+0x98): the same value as the census's field id
//              (btrules::GuidHash), so the wire's scene hash and field id are equal.
//   SKY        SetStartControlFlags FUN_142E1FED0(1) every 0.1 s while the partner is in BT trouble within 150 m (design 2.10); stop
//              calling to restore (the bit's 0.2 s timer clears it). Built off (bt_sky=0) until bt-sky-sun passes.
#pragma once

#include <cmath>
#include <cstdint>

#include "bt_rules.hpp"

namespace st::btrules::extra {

constexpr int kStage = 8;

// ---------------------------------------------------------------- the kill look
constexpr std::uint32_t kKillLookReason = kKillCrater;  // 0x5414860D: dead bit 0x8, the field's count, no stats (PF 21)
static_assert(kKillLookReason != kKillStatsA && kKillLookReason != kKillStatsB, "the kill look never credits the local player");
// The reason bt_sync.cpp's partner-kill arm writes (0: bt_world's default, the rain-gone 0x01322187). checks_ok: this module's
// stats byte checks (the "no credit" finding holds only for the stats code as probed). real_save: never on a real-save guest
// (KillApplyGate refuses there already; this is the second lock).
inline std::uint32_t KillLookReason(int bt_kill_look, bool checks_ok, bool real_save, bool stopped) {
    if (bt_kill_look < 1 || !checks_ok || real_save || stopped) return 0;
    return kKillLookReason;
}
// The window that ends with the stats line: open at the first armed kill, extended by each applied one, closed kWindowMs after the
// last applied (or kArmWaitMs after the arm when no applied kill came).
constexpr double kKillWindowMs = 5000.0, kKillArmWaitMs = 15000.0;
struct KillWindow {
    bool open = false;
    double arm_ms = 0, last_ms = -1;  // last_ms: the last applied kill (-1 none yet)
    int applied = 0, realized = 0;
    void Arm(double now_ms) {
        if (open) return;
        *this = KillWindow{};
        open = true, arm_ms = now_ms;
    }
    void Applied(double now_ms, bool was_realized) {
        if (!open) return;
        ++applied;
        if (was_realized) ++realized;
        last_ms = now_ms;
    }
    // True when the window is over now (the caller logs the stats line and calls Close).
    bool Due(double now_ms) const {
        if (!open) return false;
        if (last_ms >= 0) return now_ms - last_ms >= kKillWindowMs;
        return now_ms - arm_ms >= kKillArmWaitMs;
    }
    void Close() { open = false; }
};
// The stats delta (0x79 / 0x7A are kept as u32 counters): signed, wrap-safe.
inline int StatDelta(std::uint32_t before, std::uint32_t after) { return static_cast<int>(after - before); }
// The gazer's flags after its update: 0x1 is set at 0x1429956A8 only while the actor state (+0x24) is 2, the gazer realized (an
// entity and its DSGazerComponent; PF fact 2: bt_rules.hpp's "kFlagDead" name for 0x1 is wrong, the dead bit is 0x8).
constexpr std::uint32_t kFlagRealized = 0x1u, kFlagDeadBit = 0x8u, kFlagRespawn = 0x4000000u;
inline bool Realized(std::uint32_t flags, std::int32_t state) { return (flags & kFlagRealized) != 0 || state == 2; }

// ---------------------------------------------------------------- RESPAWN
// The host's send: a partner kill is not involved; the guest's own gate (RespawnApplyGate) is the safety. Review (wire 2): sent only
// to a guest whose hello offers cap 0x2000 (kCapWorldApply: a sandboxed guest with bt_respawn=1 and the respawn checks ok); an
// older build (v0.44: caps 0xFF, no type 16) or a real-save guest gets none. Once per field per kRespawnQuietMs.
constexpr double kRespawnQuietMs = 5000.0, kRespawnDupMs = 10000.0;
struct RespawnSendIn {
    int bt_respawn = 0;
    int role = 0;          // this game: 1 host
    int sync_level = 0;    // bt_sync here
    bool peer = false;     // the partner's hello arrived
    int peer_role = 0;     // 2 guest
    int peer_level = 0;    // its bt_sync
    std::uint16_t peer_caps = 0;  // its hello's caps (0x2000: it applies a respawn)
    bool checks_ok = false;
    std::uint32_t field = 0;
    double since_last_ms = 1e18;  // since this field was last sent
};
inline const char* RespawnSendWhyNot(const RespawnSendIn& in) {
    if (in.bt_respawn < 1) return "bt_respawn is 0 here";
    if (!in.checks_ok) return "the respawn byte checks failed here";
    if (in.role != 1) return "not the host";
    if (in.sync_level < 2) return "bt_sync is below 2 here";
    if (!in.peer) return "no partner hello";
    if (in.peer_role != 2) return "the partner is not a guest";
    if (in.peer_level < 2) return "the partner's bt_sync is below 2";
    if (!(in.peer_caps & 0x2000u)) return "the guest applies no respawn (no cap 0x2000)";  // btrules::kCapWorldApply
    if (in.field == 0) return "the scene's GUID was unreadable (no field id)";
    if (in.since_last_ms < kRespawnQuietMs) return "sent for this field within 5 s";
    return nullptr;
}
// The fields seen lately (the host's sends, the guest's applies): Has within ms.
struct FieldRing {
    std::uint32_t f[8] = {};
    double ms[8] = {};
    unsigned next = 0;
    void Add(std::uint32_t field, double now_ms) {
        f[next % 8] = field, ms[next % 8] = now_ms;
        ++next;
    }
    double Since(std::uint32_t field, double now_ms) const {  // ms since the newest entry for field (1e18 none)
        double best = 1e18;
        for (unsigned i = 0; i < 8; ++i)
            if (f[i] == field && field && now_ms >= ms[i] && now_ms - ms[i] < best) best = now_ms - ms[i];
        return best;
    }
    void Clear() { *this = FieldRing{}; }
};
// The detour's lock-free slot: a field id and its flags packed in one u64 (0 = empty). bit 0 a call seen, bit 1 the GUID read.
inline std::uint64_t PackCall(std::uint32_t field, bool guid_ok) {
    return static_cast<std::uint64_t>(field) << 32 | (guid_ok ? 3u : 1u);
}
inline bool UnpackCall(std::uint64_t v, std::uint32_t* field, bool* guid_ok) {
    if (!(v & 1u)) return false;
    *field = static_cast<std::uint32_t>(v >> 32);
    *guid_ok = (v & 2u) != 0;
    return true;
}
// The guest's scene match: a gazer of the field whose +0x3A8 (the message handler's compare at 0x14299F3C2) is its locator's scene
// and whose scene GUID hashes to the field.
struct SceneSeen {
    std::uint32_t field = 0;     // the census field (GuidHash of [[actor+0x18]+0x80]+0x98)
    std::uintptr_t owner = 0;    // +0x3A8
    std::uintptr_t loc_scene = 0;  // [[actor+0x18]+0x80]
    std::uint32_t owner_hash = 0;  // GuidHash of owner+0x98 (0 unread)
};
inline bool SceneMatches(const SceneSeen& s, std::uint32_t want) {
    return want != 0 && s.field == want && s.owner != 0 && s.owner == s.loc_scene && s.owner_hash == want;
}
constexpr unsigned kRespawnPending = 4;

// ---------------------------------------------------------------- SKY
constexpr double kSkyPeriodMs = 100.0, kSkyNearM = 150.0;
// Until bt-sky-sun shows a visual-only change, a real-save guest never gets the call (+0x6508 also feeds the weather update's state
// machine and FUN_143356F00): flip together with the bt_sky default.
constexpr bool kSkyRealSaveOk = false;
struct SkyIn {
    int bt_sky = 0;
    bool checks_ok = false;
    int sync_level = 0;
    bool peer = false;
    std::uint8_t partner_trouble = 0;  // TroubleKind
    bool partner_ok = false, me_ok = false;
    double dist_m = -1;                // the partner to the local Sam
    bool loading = false;
    const char* not_playing = nullptr;
    bool carried = false;
    bool real_save = false;
    bool stopped = false;
};
// nullptr: call now (every kSkyPeriodMs); else why not.
inline const char* SkyWhyNot(const SkyIn& in) {
    if (in.stopped) return "stopped";
    if (in.bt_sky < 1) return "bt_sky is 0 here";
    if (!in.checks_ok) return "the sky byte checks failed here";
    if (in.real_save && !kSkyRealSaveOk) return "a real-save guest (until bt-sky-sun passes)";
    if (in.sync_level < 1 || !in.peer) return "no partner";
    if (in.loading) return "loading";
    if (in.not_playing) return in.not_playing;
    if (in.carried) return "Sam rides as a passenger";
    if (in.partner_trouble < kTrHunted || in.partner_trouble > kTrCatcher) return "the partner is not in BT trouble";
    if (!in.partner_ok || !in.me_ok || !(in.dist_m >= 0)) return "the partner's position is unknown";
    if (in.dist_m > kSkyNearM) return "the partner is more than 150 m away";
    return nullptr;
}
inline bool SkyDue(double now_ms, double next_ms) { return now_ms >= next_ms; }

// ---------------------------------------------------------------- the test op (bt_sync 11 <sub> <arg>)
// sub 0: the test RESPAWN on the host (arg 0 = the nearest gazer's field, else that field id); 1: the sky call for arg seconds
// (1..120, default 10; any role, bt_sky=1); 2: act as a real-save guest for the RESPAWN gate (arg 1 on, 0 off; restrictive only).
enum TestSub : int { kSubRespawn = 0, kSubSky = 1, kSubRealSave = 2, kSubBad = -1 };
inline int TestSubOf(double v1) {
    if (!std::isfinite(v1) || v1 != std::floor(v1) || v1 < 0 || v1 > 2) return kSubBad;
    return static_cast<int>(v1);
}
inline double SkySeconds(double v2) {
    if (!std::isfinite(v2) || v2 == 0) return 10.0;
    if (v2 < 1 || v2 > 120) return -1;
    return std::floor(v2);
}
inline bool FieldArg(double v2, std::uint32_t* out) {  // 0 the nearest; else a u32 field id
    if (!std::isfinite(v2) || v2 < 0 || v2 > 4294967295.0 || v2 != std::floor(v2)) return false;
    *out = static_cast<std::uint32_t>(v2);
    return true;
}

// ---------------------------------------------------------------- the log budget ("[bt] extra:" lines)
// At most kLines per kWindowMs per kind; the first line after a dropped run says how many were dropped.
struct LineBudget {
    static constexpr double kWindowMs = 10000.0;
    static constexpr int kLines = 8;
    double start = -1e18;
    int used = 0, dropped = 0;
    // True: log now (*dropped_before: lines dropped since the last logged one).
    bool Take(double now_ms, int* dropped_before) {
        if (now_ms - start >= kWindowMs || now_ms < start) start = now_ms, used = 0;
        if (used >= kLines) {
            ++dropped;
            return false;
        }
        ++used;
        if (dropped_before) *dropped_before = dropped;
        dropped = 0;
        return true;
    }
};

}  // namespace st::btrules::extra
