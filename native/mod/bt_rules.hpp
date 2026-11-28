// BT sync B0 (round 28 P3, the BT field census; bt_sync.hpp): the pure part, unit-tested in tests/bt_rules_tests.cpp.
//
// The ids the probe logs are the game's own (FUN_1429A31A0, the gazer kill, and FUN_14299F650, the field registration, both
// [V] in ds.exe DC): crc32c over the 16 bytes of a GGUUID, as two little-endian u64 with the SSE4.2 crc32 instruction, initial
// value 0 and no final xor.
//   field id   = crc32c(the gazer scene's GUID, [locator +0x80] +0x98)   (the key of the gazer manager's +0x1750 map)
//   gazer id   = crc32c(the gazer locator's GUID, locator +0x88)         (what the field's killed list stores)
// Both come from static archive data, so two games give the same numbers for the same field and the same gazer.
//
// kKnownFields is the area-200 table from the archive (round 28 static answer Q3/Q4, scratchpad round28/btlane/fields.py over
// arc/btscan_enemy.jsonl): the field id, the scene's short name, its DSGazerGameActorLocator count and the locator print (Fnv1a
// over the sorted gazer ids, LocatorPrint below) of every locator in it, and the locators' centroid (x, y). Numbers only: no
// game data is shipped beyond these hashes and counts.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace st::btrules {

// ---------------------------------------------------------------- actor ids (GameActorRegistry 0x1422FA980: type = id >> 13 & 0xFF)
constexpr std::uint32_t kActorNone = 0x1FFFFF;
constexpr std::uint32_t kGazerActorType = 8;  // EnableRespawnGazers 0x1429A76D0 broadcasts to type 8 [V]
constexpr std::uint32_t ActorType(std::uint32_t id) { return (id >> 13) & 0xFFu; }
constexpr std::uint32_t ActorSlot(std::uint32_t id) { return id & 0x1FFFu; }

// ---------------------------------------------------------------- the game's hashes
inline std::uint32_t Crc32cU64(std::uint32_t crc, std::uint64_t q) {
    for (int i = 0; i < 8; ++i) {
        crc ^= static_cast<std::uint32_t>((q >> (8 * i)) & 0xFFu);
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
    }
    return crc;
}
inline std::uint64_t Le64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
// crc32c(0, lo u64) then crc32c(that, hi u64), as at 0x1429A3238 / 0x1429A32A0.
inline std::uint32_t GuidHash(const std::uint8_t* guid16) { return Crc32cU64(Crc32cU64(0, Le64(guid16)), Le64(guid16 + 8)); }
inline bool GuidZero(const std::uint8_t* guid16) {
    for (int i = 0; i < 16; ++i)
        if (guid16[i]) return false;
    return true;
}

constexpr std::uint32_t kFnvSeed = 2166136261u;
inline std::uint32_t Fnv1a(std::uint32_t h, const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}
inline std::uint32_t FnvU32(std::uint32_t h, std::uint32_t v) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v >> 16),
                               static_cast<std::uint8_t>(v >> 24)};
    return Fnv1a(h, b, 4);
}

// Order-free prints (the same number on both games for the same set).
// Gazer ids: sorted, duplicates kept (a duplicate is itself a difference worth seeing).
inline std::uint32_t LocatorPrint(std::vector<std::uint32_t> ids) {
    std::sort(ids.begin(), ids.end());
    std::uint32_t h = kFnvSeed;
    for (std::uint32_t v : ids) h = FnvU32(h, v);
    return h;
}
// Fields: (field id, total locators), sorted by id.
inline std::uint32_t FieldsPrint(std::vector<std::pair<std::uint32_t, std::int32_t>> f) {
    std::sort(f.begin(), f.end());
    std::uint32_t h = kFnvSeed;
    for (const auto& x : f) {
        h = FnvU32(h, x.first);
        h = FnvU32(h, static_cast<std::uint32_t>(x.second));
    }
    return h;
}
// Killed lists: per field (sorted by id) its id, then its killed gazer ids sorted.
struct FieldKills {
    std::uint32_t id = 0;
    std::vector<std::uint32_t> killed;
};
inline std::uint32_t KillsPrint(std::vector<FieldKills> f) {
    std::sort(f.begin(), f.end(), [](const FieldKills& a, const FieldKills& b) { return a.id < b.id; });
    std::uint32_t h = kFnvSeed;
    for (FieldKills& x : f) {
        h = FnvU32(h, x.id);
        std::sort(x.killed.begin(), x.killed.end());
        for (std::uint32_t k : x.killed) h = FnvU32(h, k);
    }
    return h;
}

// ---------------------------------------------------------------- the area-200 fields from the archive
struct KnownField {
    std::uint32_t id;
    const char* name;
    int locators;
    std::uint32_t loc_print;  // LocatorPrint of all its locators
    float cx, cy;             // the locators' centroid
};
// clang-format off
inline constexpr KnownField kKnownFields[] = {
    {0x28597730u, "bt202", 33, 0x39CD9F8Au, 1292.0f, 1743.0f},
    {0xB79B84E0u, "bt203_000", 35, 0x932A3E53u, 536.0f, 73.0f},
    {0xED4409EEu, "bt203_02", 39, 0xB07AB7F8u, 645.0f, 287.0f},
    {0x36E2C2AFu, "bt206_001", 21, 0xF75157D6u, 1329.0f, -663.0f},
    {0x48665E92u, "bt206_002", 65, 0xCDD37EDEu, 1241.0f, -740.0f},
    {0x57D9D8CAu, "bt206_003", 55, 0xB393C8BDu, 1056.0f, -571.0f},
    {0xAFB8DBEAu, "bt207_000", 18, 0x4C1B7F4Eu, 1759.0f, -496.0f},
    {0xE9ECD61Du, "bt207_001", 3, 0xFFB01370u, 1676.0f, -575.0f},
    {0x1CF597BBu, "bt207_002", 8, 0x89916F36u, 1859.0f, -511.0f},
    {0x022FDEB6u, "bt207_003", 10, 0x65BE7313u, 1791.0f, -389.0f},
    {0xDC5678A4u, "bt208", 52, 0xE4F7E1C1u, 662.0f, 1514.0f},
    {0xAF0CE561u, "bt209", 54, 0x10318600u, -1069.0f, 686.0f},
    {0x86F98B6Au, "bt210", 37, 0x6F690639u, -1914.0f, 658.0f},
    {0x390B7DE4u, "bt211_01", 35, 0xDBE6C47Cu, -864.0f, -5.0f},
    {0x8F93CEA1u, "bt211", 17, 0x3318518Du, -678.0f, 134.0f},
    {0xDD241D28u, "bt211_m00430_01", 13, 0x2C619DF4u, -692.0f, 35.0f},
    {0x40E15D48u, "bt212", 77, 0xF1B80E3Fu, -1291.0f, -668.0f},
    {0x4AD18562u, "bt213", 29, 0x71089B1Au, -1997.0f, -1023.0f},
    {0x5AD1F292u, "bt213_02", 11, 0xEF00A562u, -1925.0f, -1005.0f},
    {0x74F135D0u, "bt213_03", 16, 0x1ADBEC2Du, -2006.0f, -948.0f},
    {0xDDE9B279u, "bt213_04", 20, 0x8E7EE7EDu, -2115.0f, -990.0f},
    {0x8F5EBA71u, "bt218", 24, 0x5B7C3000u, -2693.0f, -2187.0f},
    {0xAE4D3749u, "bt221_000", 25, 0x2351942Du, -775.0f, -1001.0f},
    {0x7B23FF63u, "bt222_000", 49, 0x54E2F93Cu, -799.0f, -1679.0f},
    {0x9B8C9866u, "bt233", 15, 0x7B9C7910u, 965.0f, 2058.0f},
};
// clang-format on
inline const KnownField* FindKnownField(std::uint32_t id) {
    for (const KnownField& f : kKnownFields)
        if (f.id == id) return &f;
    return nullptr;
}
inline const char* FieldName(std::uint32_t id) {
    const KnownField* f = FindKnownField(id);
    return f ? f->name : "?";
}

// ---------------------------------------------------------------- what a field looks like to the probe
struct FieldSeen {
    std::uint32_t id = 0;
    std::int32_t killed = 0;  // state +0x00
    std::int32_t timers = 0;  // state +0x10 (the respawn timer bytes, one per killed entry)
    std::int32_t total = 0;   // state +0x20 (DSGazerGameActorLocator count, FUN_14299F650)
    int live = 0;             // live gazer actors of this field the probe resolved
    std::uint32_t live_print = kFnvSeed;  // LocatorPrint over those
};
// state +0x20 before the scene registers its total (FUN_14299F650), and after ResetAllGazers (0x1429A17A0 writes [8] = 0x7FFFFFFF)
// [V code; V run round 28 P3: 18 of 31 records at load, and bt211_01 35 -> 0x7FFFFFFF after dev_enemies 3, live gazers untouched]
constexpr std::int32_t kTotalUnset = 0x7FFFFFFF;
inline bool TotalKnown(std::int32_t total) { return total > 0 && total != kTotalUnset; }
// Cleared as the game decides it (0x1429A33E3: killed == total, then message 0x324D8FD4).
inline bool FieldCleared(const FieldSeen& f) { return TotalKnown(f.total) && f.killed == f.total; }

// The static check: "static" when the archive knows the field; "all" when every locator is live and the print matches.
enum class StaticMatch { unknown, total_unset, total_differs, partial, all, print_differs };
inline StaticMatch MatchStatic(const FieldSeen& f) {
    const KnownField* k = FindKnownField(f.id);
    if (!k) return StaticMatch::unknown;
    if (f.total == kTotalUnset) return StaticMatch::total_unset;
    if (f.total != k->locators) return StaticMatch::total_differs;
    if (f.live != k->locators) return StaticMatch::partial;
    return f.live_print == k->loc_print ? StaticMatch::all : StaticMatch::print_differs;
}
inline const char* MatchName(StaticMatch m) {
    switch (m) {
        case StaticMatch::unknown: return "not in the archive table";
        case StaticMatch::total_unset: return "total unset (reset, or not registered yet)";
        case StaticMatch::total_differs: return "total differs from the archive";
        case StaticMatch::partial: return "partly live";
        case StaticMatch::all: return "all live, print = archive";
        case StaticMatch::print_differs: return "all live, print DIFFERS";
    }
    return "?";
}

// ---------------------------------------------------------------- the gazer actor's flags (+0x1B4), the bits the code names
// 0x10 / 0x40: (flags & 0x50) == 0x50 gazers react to a found-player broadcast (0x14299EFB0, message 0x7989539C) [V test; I meaning]
// 0x400000: the kill does not reach the field's killed list (0x142995F6A) [V]
// 0x2000000: caught / consume (message 0x682DFB3B) [V]
// 0x4000000: respawn enabled (message 0x58305569, EnableRespawnGazers) [V]
// From the gazer update FUN_142995210 (round 28 P3 run + static):
// 0x1: set while the actor state (+0x24) is 2 (0x1429956A8) [V code; I "dead"]
// 0x1000 "near": set at 0x142995C6A when the gazer's distance to the LOCAL player (+0x204, written at 0x1429953EB from
//   FUN_14283EC70 +0x270) is within its trigger radius ([+0x388] +0x98), state 0, a target at +0x3B0 and no 0x400000; cleared
//   at 0x142995526 when +0x204 > [[+0x388] +0x48] +0x48 [V code; V run: host 9.3 m -> set, the guest's copy of the same gazer
//   with the host's puppet 9.3 m away -> never set]
// 0x80000 "idle": cleared in the same write that sets 0x1000 (not set again in the run) [V code; I meaning]
// 0x1000000 "pending": recomputed every update from a call on +0x348 (0x1429956F3); set for ~2 s after the gazer spawns [V run; I]
constexpr std::uint32_t kFlagDead = 0x1u, kFlagNear = 0x1000u, kFlagIdle = 0x80000u, kFlagPending = 0x1000000u;
inline void FlagsText(std::uint32_t f, char* out, std::size_t cap) {
    if (!out || !cap) return;
    std::snprintf(out, cap, "%s%s%s%s%s%s%s%s%s", (f & kFlagDead) ? " dead" : "", (f & kFlagNear) ? " near" : "", (f & kFlagIdle) ? " idle" : "",
                  (f & kFlagPending) ? " pending" : "", (f & 0x50u) == 0x50u ? " react" : (f & 0x50u) ? " part-react" : "",
                  (f & 0x400000u) ? " no-kill-list" : "", (f & 0x2000000u) ? " caught" : "", (f & 0x4000000u) ? " respawn" : "", f ? "" : " none");
}
// +1 when `bit` rises from prev to now, -1 when it falls, 0 otherwise.
inline int FlagEdge(std::uint32_t prev, std::uint32_t now, std::uint32_t bit) {
    const bool a = (prev & bit) != 0, b = (now & bit) != 0;
    return a == b ? 0 : b ? 1 : -1;
}

// ---------------------------------------------------------------- probe modes and the per-second line budget
// bt_probe (session.ini and the action's v0): 0 off, 1 a census line once a second (and the change lines), 2 also a full dump
// (every gazer, every field record, the catchers) when gazers first appear (ini) / now (action).
inline int ProbeMode(double v) {
    if (!(v >= 0)) return 0;  // also NaN
    return v >= 2 ? 2 : v >= 1 ? 1 : 0;
}
// The probe's interval (the action's v1): 0 = 1 s; 0.25 .. 30 s.
inline double ProbeIntervalMs(double secs) {
    if (!(secs > 0)) return 1000.0;
    return std::clamp(secs, 0.25, 30.0) * 1000.0;
}
// At most `cap` change lines a window (1 s); the rest are counted and reported in the next census line.
struct LineBudget {
    double window_ms = 1000.0;
    int cap = 16;
    double start_ms = -1e18;
    int used = 0;
    int dropped = 0;
    bool Take(double now_ms) {
        if (now_ms - start_ms >= window_ms) start_ms = now_ms, used = 0;
        if (used < cap) return ++used, true;
        ++dropped;
        return false;
    }
    int TakeDropped() {
        const int d = dropped;
        dropped = 0;
        return d;
    }
};

inline double Dist3(const double* a, const double* b) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
inline bool PosSane(const double* p) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(p[i]) || std::fabs(p[i]) > 1e7) return false;
    return true;
}

// ---------------------------------------------------------------- the run's verdict on perception (the stop line)
// The local player counts as perceived by the BTs in a census when the hunter manager has an active hunter area (the found-player
// action spawned it, FUN_1429B8D60) or at least one gazer has the "near" bit (0x1000, the local player within its trigger radius).
// Not the gazer manager's +0x230: round 28 P3 showed it at 1.000 with no gazer loaded on both games. It is a player stillness meter
// (0x1429A47E7: rises over PlayerConcentrationTime while the player's position and camera do not change, falls over
// PlayerUnconcentrationTime, 0 while +0x23B) [V code; I meaning], logged as "still".
struct Perceive {
    bool hunter = false;
    int near_n = 0;  // gazers with kFlagNear
    bool any() const { return hunter || near_n > 0; }
};

// ================================================================ B1 (round 28 lane B1; bt_sync=1|2): the BT field sync's pure part
// What the probe runs proved (round 28 P3 runs 20261127-000239 and -002916), and what B1 builds on:
//  - both games stream the same gazers from the same locators (equal locator prints per field, equal record sets); actor ids differ
//    per game AND per stream-in (40AB1A93 was 1000E, then 10010 after a re-entry): everything is keyed by the locator / field hash;
//  - a gazer senses only its own game's player (the near bit 0x1000, +0x204 = the distance to FUN_14283EC70's player): the partner's
//    puppet next to it changes nothing. So "the partner is noticed" is the partner's own game telling us (NOTICED);
//  - the hunter area (+0x140) never went non-null (12 s at 9.3 m, 20 s at 3 m, god mode): "found" is kept as a second NOTICED kind,
//    but the gate uses the near rise;
//  - ResetAllGazers resets every record (total 0x7FFFFFFF, killed lists cleared) and leaves live gazers alone; a field's total comes
//    back when the scene registers it again (re-entry). Safe to mirror (RESET), detected by polling (LooksLikeReset).

// bt_sync (session.ini): 0 off (nothing runs, nothing is sent), 1 relay (FIELD compare, NOTICED and VOID notices, the
// pass-through voidout detour; no game call), 2 (the default since round 28) also the host's RESET applied on the guest
// (ResetAllGazers, game thread).
inline int SyncLevel(double v) {
    if (!(v >= 0)) return 0;  // also NaN
    return v >= 2 ? 2 : v >= 1 ? 1 : 0;
}

// The records' summary a reset is read from (the gazer manager's +0x1750 map).
struct RecSummary {
    int fields = 0;            // records
    int unset = 0;             // records with total == kTotalUnset
    int killed_total = 0;      // sum of the records' killed counts
    std::uint32_t ids = kFnvSeed;  // LocatorPrint over the record ids (the field ids only: a reset does not change them)
};
// ResetAllGazers as the poll sees it: the same records (count and ids), all of them unset now, not all before, nothing killed.
// A load (the map rebuilt) changes the ids or the count; a re-registration lowers `unset`; neither reads as a reset.
inline bool LooksLikeReset(const RecSummary& prev, const RecSummary& now) {
    return prev.fields >= 2 && now.fields == prev.fields && now.ids == prev.ids && now.unset == now.fields && prev.unset < prev.fields &&
           now.killed_total == 0;
}
// A reset this game saw within this long after it applied the host's RESET is that one (not sent back).
constexpr double kResetEchoMs = 5000.0;
inline bool ResetEcho(double now_ms, double applied_ms) { return now_ms >= applied_ms && now_ms - applied_ms <= kResetEchoMs; }
// u16 epochs, wrap-safe: a is newer than b.
inline bool EpochNewer(std::uint16_t a, std::uint16_t b) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b)) > 0; }

// NOTICED's rate limit: at most one per field every kNoticeFieldMs, and the same gazer (locator) again only after kNoticeLocMs.
constexpr double kNoticeFieldMs = 5000.0, kNoticeLocMs = 15000.0;
struct NoticeGate {
    struct Last {
        std::uint32_t key = 0;
        double ms = -1e18;
    };
    Last fields[16], locs[32];
    unsigned nf = 0, nl = 0;  // the next slot to reuse (round robin)
    static Last* Find(Last* a, unsigned n, std::uint32_t key) {
        for (unsigned i = 0; i < n; ++i)
            if (a[i].key == key && a[i].ms > -1e17) return &a[i];
        return nullptr;
    }
    bool Take(std::uint32_t field, std::uint32_t loc, double now_ms) {
        Last* f = Find(fields, 16, field);
        Last* l = Find(locs, 32, loc);
        if (f && now_ms - f->ms < kNoticeFieldMs) return false;
        if (l && now_ms - l->ms < kNoticeLocMs) return false;
        if (!f) f = &fields[nf++ % 16];
        if (!l) l = &locs[nl++ % 32];
        f->key = field, f->ms = now_ms;
        l->key = loc, l->ms = now_ms;
        return true;
    }
};

// The voidout FUN_14299B1B0(mgr, pos, gazer_caused, with_baggage): the far crater when the local player is at least the
// resource's FarAnnihilationDistance ([mgr+0x68] +0x54) from pos, then GameOver with EDSMissionReasonGameOver 13
// GazerAnnihilation / 14 GazerFarAnnihilation (gazer caused) or 3 Annihilation / 16 AnnihilationWithBaggage [V code r-bt 2.8;
// I which reason goes with which pair: the relay only names it].
inline std::uint8_t VoidReason(bool gazer_caused, bool with_baggage, bool far_crater) {
    if (gazer_caused) return far_crater ? 14 : 13;
    return with_baggage ? 16 : 3;
}
constexpr std::uint8_t kVoidReasonTest = 0xFF;
inline const char* VoidReasonName(std::uint8_t r) {
    switch (r) {
        case 3: return "Annihilation";
        case 13: return "GazerAnnihilation";
        case 14: return "GazerFarAnnihilation";
        case 16: return "AnnihilationWithBaggage";
        case kVoidReasonTest: return "test";
    }
    return "?";
}
// Who called the voidout (the detour's return address, the four E8 call sites in ds.exe DC; round 28 lane B1 scan). The numbers
// are on the wire (BTS1 VOID's source byte) and do not change. Round 30 renamed them after reading the callers [V code]:
//   1 FUN_14299B3B0, reached only from the timer list FUN_142B21CA0: a scripted / timed voidout;
//   2 FUN_14299EFB0, the gazer catch (message 0x682DFB3B);
//   3 the gazer manager update FUN_1429A3F50: the corpse list +0x1760 -> FUN_1429A1230 -> +0x1780 bit 0, 3 s later (a human
//     corpse left to necrosis);
//   4 FUN_142A97650, queued by the MULE actor FUN_142AA2230: a MULE corpse left to necrosis (not the BT catcher).
// None of them says whose player it hits: a corpse voidout fires wherever that corpse is, so a VOID alone is never "the partner
// was voided out" (VoidVerdict below).
enum VoidSource : std::uint8_t {
    kVoidSrcUnknown = 0,
    kVoidSrcScripted = 1,
    kVoidSrcCatch = 2,
    kVoidSrcCorpseNecrosis = 3,
    kVoidSrcMuleCorpse = 4,
    kVoidSrcTest = 0xFF
};
inline const char* VoidSourceName(std::uint8_t s) {
    switch (s) {
        case kVoidSrcScripted: return "scripted / timed";
        case kVoidSrcCatch: return "gazer catch";
        case kVoidSrcCorpseNecrosis: return "corpse necrosis (manager, 3 s)";
        case kVoidSrcMuleCorpse: return "MULE corpse (necrosis)";
        case kVoidSrcTest: return "test";
    }
    return "unknown caller";
}

// Round 30: the receiver's verdict on a partner VOID. "Your partner was voided out" only when the partner's own game went to
// GameOver (their SEH1 beacon's player_down == 1, game state bit 13) within kVoidDownWindowMs of the VOID, before or after it;
// otherwise the VOID is logged only (a corpse or a MULE corpse voiding out near them, or a scripted crater). A test VOID always
// gets the test notice. down_ms < 0: no GameOver seen yet.
// Round 30 review: the GameOver reaches us only through the down channel (the partner's enemy_sync down tick, sent in its beacon's
// byte 22 only while OUR beacon carries kPartRestrain; both need enemy_sync + enemy_hits + enemy_restrain_relay, on by default
// only with host_save). down_channel false (shots::DownChannel()): no GameOver can come, so the verdict is at once round 28's
// per-source rule for the sources that can be the partner's own player (VoidMayBePlayer: the gazer catch and the scripted / timed
// voidout: no_channel_toast); the corpse sources and unknown callers are logged only (no_channel_log).
constexpr double kVoidDownWindowMs = 5000.0;
constexpr std::uint8_t kBeaconDownDead = 1;  // shotw::Beacon::player_down: 1 = GameOver
inline bool VoidMayBePlayer(std::uint8_t source) { return source == kVoidSrcCatch || source == kVoidSrcScripted; }
enum class VoidCall : std::uint8_t { wait, partner, test, log_only, no_channel_toast, no_channel_log };
inline VoidCall VoidVerdict(bool test, double void_ms, double down_ms, double now_ms, bool down_channel = true,
                            std::uint8_t source = kVoidSrcUnknown) {
    if (test) return VoidCall::test;
    if (down_ms >= 0 && std::fabs(down_ms - void_ms) <= kVoidDownWindowMs) return VoidCall::partner;
    if (!down_channel) return VoidMayBePlayer(source) ? VoidCall::no_channel_toast : VoidCall::no_channel_log;
    if (now_ms - void_ms < kVoidDownWindowMs) return VoidCall::wait;
    return VoidCall::log_only;
}
inline const char* VoidCallName(VoidCall c) {
    switch (c) {
        case VoidCall::wait: return "wait";
        case VoidCall::partner: return "partner";
        case VoidCall::test: return "test";
        case VoidCall::log_only: return "log only";
        case VoidCall::no_channel_toast: return "no down channel: toast by source";
        case VoidCall::no_channel_log: return "no down channel: log only";
    }
    return "?";
}

// ---------------------------------------------------------------- BTS1 (channel 6, link::SendCombat; received apart from SCB1)
// Little-endian. Header 12 B: 'BTS1' | u8 type | u8 version 1 | u8 n | u8 0 | u32 seq (the sender's BTS1 count this link session).
//   1 HELLO   (n 0), body 8 B: u8 level (bt_sync 1..2) | u8 role (1 host, 2 guest) | u16 caps (kCap*) | u32 0
//   2 FIELD   (n 0..6), body 24 + 16 n: u16 epoch (the sender's resets) | u16 applied (the last host epoch it applied, guest) |
//             u16 fields | u16 unset | u16 cleared | u16 live gazers | u32 ids print | u32 kills print | u32 live locator print |
//             n x { u32 field id | u16 live | u16 killed | i32 total | u32 live print }  (the fields with live gazers, most first)
//   3 RESET   (n 0), body 8 B: u16 epoch | u16 fields | u32 ids print (before the reset)
//   4 NOTICED (n 0), body 40 B: u8 kind (kNotice*) | u8 near now | u16 0 | u32 field id | u32 locator hash | f32 gazer pos[3] |
//             f32 the sender's player pos[3] | f32 the game's distance (+0x204, m; -1 unknown)
//   5 VOID    (n 0), body 32 B: u8 reason | u8 source (VoidSource) | u8 flags (kVoid*) | u8 0 | f32 pos[3] | f32 the sender's player
//             distance to pos | f32 the far-crater distance (-1 unknown) | u32 sender ms | u32 0
// Round 30 BT S1 / S2 (sent only to a partner whose HELLO caps carry the type's bit: an older build never receives them):
//   6 TROUBLE (n 0), body 32 B: u8 kind (kTr*) | u8 flags (kTrf*) | u16 episode | f32 pos[3] (the sender's player; a catcher's own
//             place for CATCHER) | f32 hp (CATCHER: percent of the most seen this episode; else -1) | u32 catcher class (0: not read)
//             | u32 field | u32 loc (the catching gazer for CAUGHT, else 0)
//   7 GZST    (n 0..8), body 4 + 28 n: u32 sender ms | n x { u32 field | u32 loc | u8 state (+0x24) | u8 mode (+0x1A8) | u8 bits
//             (kGz*) | u8 0 | f32 pos[3] | f32 approach (+0xB4) }  (bits 0: that gazer stopped sensing the sender)
//   8 KILL    (n 0), body 36 B: u32 field | u32 loc | u32 reason (+0xCC) | u32 world (the sender's enemy GUID print, 0 unknown) |
//             u8 cause (kCause*) | u8 flags (kKf*) | u16 0 | f32 pos[3] | u32 sender ms
// A decoder refuses another magic, version, type, size, n, a non-zero pad or a non-finite float.
constexpr std::size_t kBtsHeader = 12, kBtsHelloSize = 20, kBtsFieldBase = 36, kBtsFieldEntry = 16, kBtsResetSize = 20,
                      kBtsNoticedSize = 52, kBtsVoidSize = 44, kBtsTroubleSize = 44, kBtsGzstBase = 16, kBtsGzstEntry = 28,
                      kBtsKillSize = 48;
constexpr unsigned kBtsFieldMax = 6, kBtsGzstMax = 8;
// The biggest BTS1 message: a full GZST, 240 B (a full FIELD is 132 B); link.cpp's BTS1 slot (kBtMax) is 256 B.
constexpr std::size_t kBtsMaxSize = kBtsGzstBase + kBtsGzstMax * kBtsGzstEntry;
static_assert(kBtsMaxSize >= kBtsFieldBase + kBtsFieldMax * kBtsFieldEntry && kBtsMaxSize <= 256, "BTS1 sizes");
enum BtsType : std::uint8_t {
    kBtsHello = 1, kBtsField = 2, kBtsReset = 3, kBtsNoticed = 4, kBtsVoid = 5, kBtsTrouble = 6, kBtsGzst = 7, kBtsKill = 8
};
enum BtsCap : std::uint16_t {
    kCapField = 1, kCapNoticed = 2, kCapVoid = 4, kCapResetApply = 8,
    kCapTrouble = 0x10, kCapGzst = 0x20, kCapKill = 0x40, kCapKillApply = 0x80  // round 30 S1 / S2
};
enum NoticeKind : std::uint8_t { kNoticeNear = 1, kNoticeFound = 2, kNoticeTest = 9 };
enum VoidFlag : std::uint8_t { kVoidGazer = 1, kVoidBaggage = 2, kVoidFar = 4, kVoidTest = 8 };

struct BtsHeader {
    std::uint8_t type = 0, n = 0;
    std::uint32_t seq = 0;
};
struct BtsHello {
    std::uint8_t level = 0, role = 0;
    std::uint16_t caps = 0;
};
struct FieldEntry {
    std::uint32_t id = 0;
    std::uint16_t live = 0, killed = 0;
    std::int32_t total = 0;
    std::uint32_t print = kFnvSeed;
};
struct FieldMsg {
    std::uint16_t epoch = 0, applied = 0, fields = 0, unset = 0, cleared = 0, live = 0;
    std::uint32_t ids = kFnvSeed, kills = kFnvSeed, locs = kFnvSeed;
    unsigned n = 0;
    FieldEntry e[kBtsFieldMax];
};
inline bool operator==(const FieldEntry& a, const FieldEntry& b) {
    return a.id == b.id && a.live == b.live && a.killed == b.killed && a.total == b.total && a.print == b.print;
}
// The content (not the epochs' bookkeeping alone): what makes a new FIELD worth sending.
inline bool SameContent(const FieldMsg& a, const FieldMsg& b) {
    if (a.epoch != b.epoch || a.applied != b.applied || a.fields != b.fields || a.unset != b.unset || a.cleared != b.cleared || a.live != b.live ||
        a.ids != b.ids || a.kills != b.kills || a.locs != b.locs || a.n != b.n)
        return false;
    for (unsigned i = 0; i < a.n && i < kBtsFieldMax; ++i)
        if (!(a.e[i] == b.e[i])) return false;
    return true;
}
struct BtsReset {
    std::uint16_t epoch = 0, fields = 0;
    std::uint32_t ids = 0;
};
struct BtsNoticed {
    std::uint8_t kind = 0, near_n = 0;
    std::uint32_t field = 0, loc = 0;
    float gazer[3] = {}, player[3] = {};
    float game_dist = -1;
};
struct BtsVoid {
    std::uint8_t reason = 0, source = 0, flags = 0;
    float pos[3] = {};
    float player_dist = -1, far_dist = -1;
    std::uint32_t sender_ms = 0;
};

namespace detail {
inline void P8(std::uint8_t* p, std::size_t& at, std::uint8_t v) { p[at++] = v; }
inline void P16(std::uint8_t* p, std::size_t& at, std::uint16_t v) { p[at++] = static_cast<std::uint8_t>(v), p[at++] = static_cast<std::uint8_t>(v >> 8); }
inline void P32(std::uint8_t* p, std::size_t& at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[at++] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void PF(std::uint8_t* p, std::size_t& at, float f) {
    std::uint32_t v = 0;
    std::memcpy(&v, &f, 4);
    P32(p, at, v);
}
inline std::uint8_t G8(const std::uint8_t* p, std::size_t& at) { return p[at++]; }
inline std::uint16_t G16(const std::uint8_t* p, std::size_t& at) {
    const std::uint16_t v = static_cast<std::uint16_t>(p[at] | (p[at + 1] << 8));
    at += 2;
    return v;
}
inline std::uint32_t G32(const std::uint8_t* p, std::size_t& at) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[at + static_cast<std::size_t>(i)];
    at += 4;
    return v;
}
inline bool GF(const std::uint8_t* p, std::size_t& at, float& out) {
    const std::uint32_t v = G32(p, at);
    std::memcpy(&out, &v, 4);
    return std::isfinite(out);
}
inline std::size_t Head(std::uint8_t* out, std::uint8_t type, std::uint8_t n, std::uint32_t seq) {
    std::size_t at = 0;
    out[at++] = 'B', out[at++] = 'T', out[at++] = 'S', out[at++] = '1';
    P8(out, at, type);
    P8(out, at, 1);
    P8(out, at, n);
    P8(out, at, 0);
    P32(out, at, seq);
    return at;
}
inline bool FiniteF(float f) { return std::isfinite(f) != 0; }
}  // namespace detail

inline bool LooksLikeBts(const std::uint8_t* d, std::size_t n) { return d && n >= kBtsHeader && d[0] == 'B' && d[1] == 'T' && d[2] == 'S' && d[3] == '1'; }
// The header and the message's exact size for its type / n.
inline bool DecodeBtsHeader(const std::uint8_t* d, std::size_t size, BtsHeader& h) {
    if (!LooksLikeBts(d, size) || d[5] != 1 || d[7] != 0) return false;
    h.type = d[4];
    h.n = d[6];
    std::size_t at = 8;
    h.seq = detail::G32(d, at);
    switch (h.type) {
        case kBtsHello: return h.n == 0 && size == kBtsHelloSize;
        case kBtsField: return h.n <= kBtsFieldMax && size == kBtsFieldBase + h.n * kBtsFieldEntry;
        case kBtsReset: return h.n == 0 && size == kBtsResetSize;
        case kBtsNoticed: return h.n == 0 && size == kBtsNoticedSize;
        case kBtsVoid: return h.n == 0 && size == kBtsVoidSize;
        case kBtsTrouble: return h.n == 0 && size == kBtsTroubleSize;
        case kBtsGzst: return h.n <= kBtsGzstMax && size == kBtsGzstBase + h.n * kBtsGzstEntry;
        case kBtsKill: return h.n == 0 && size == kBtsKillSize;
    }
    return false;
}
inline std::size_t EncodeHello(std::uint32_t seq, const BtsHello& m, std::uint8_t* out, std::size_t cap) {
    if (!out || cap < kBtsHelloSize || m.level < 1 || m.level > 2 || (m.role != 1 && m.role != 2)) return 0;
    std::size_t at = detail::Head(out, kBtsHello, 0, seq);
    detail::P8(out, at, m.level);
    detail::P8(out, at, m.role);
    detail::P16(out, at, m.caps);
    detail::P32(out, at, 0);
    return at;
}
inline bool DecodeHello(const std::uint8_t* d, std::size_t size, BtsHello& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsHello) return false;
    std::size_t at = kBtsHeader;
    m.level = detail::G8(d, at);
    m.role = detail::G8(d, at);
    m.caps = detail::G16(d, at);
    return detail::G32(d, at) == 0 && m.level >= 1 && m.level <= 2 && (m.role == 1 || m.role == 2);
}
inline std::size_t EncodeField(std::uint32_t seq, const FieldMsg& m, std::uint8_t* out, std::size_t cap) {
    if (!out || m.n > kBtsFieldMax || cap < kBtsFieldBase + m.n * kBtsFieldEntry) return 0;
    std::size_t at = detail::Head(out, kBtsField, static_cast<std::uint8_t>(m.n), seq);
    detail::P16(out, at, m.epoch);
    detail::P16(out, at, m.applied);
    detail::P16(out, at, m.fields);
    detail::P16(out, at, m.unset);
    detail::P16(out, at, m.cleared);
    detail::P16(out, at, m.live);
    detail::P32(out, at, m.ids);
    detail::P32(out, at, m.kills);
    detail::P32(out, at, m.locs);
    for (unsigned i = 0; i < m.n; ++i) {
        detail::P32(out, at, m.e[i].id);
        detail::P16(out, at, m.e[i].live);
        detail::P16(out, at, m.e[i].killed);
        detail::P32(out, at, static_cast<std::uint32_t>(m.e[i].total));
        detail::P32(out, at, m.e[i].print);
    }
    return at;
}
inline bool DecodeField(const std::uint8_t* d, std::size_t size, FieldMsg& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsField) return false;
    std::size_t at = kBtsHeader;
    m.epoch = detail::G16(d, at);
    m.applied = detail::G16(d, at);
    m.fields = detail::G16(d, at);
    m.unset = detail::G16(d, at);
    m.cleared = detail::G16(d, at);
    m.live = detail::G16(d, at);
    m.ids = detail::G32(d, at);
    m.kills = detail::G32(d, at);
    m.locs = detail::G32(d, at);
    m.n = h.n;
    for (unsigned i = 0; i < m.n; ++i) {
        m.e[i].id = detail::G32(d, at);
        m.e[i].live = detail::G16(d, at);
        m.e[i].killed = detail::G16(d, at);
        m.e[i].total = static_cast<std::int32_t>(detail::G32(d, at));
        m.e[i].print = detail::G32(d, at);
    }
    return m.unset <= m.fields && m.cleared <= m.fields;
}
inline std::size_t EncodeReset(std::uint32_t seq, const BtsReset& m, std::uint8_t* out, std::size_t cap) {
    if (!out || cap < kBtsResetSize || m.epoch == 0) return 0;
    std::size_t at = detail::Head(out, kBtsReset, 0, seq);
    detail::P16(out, at, m.epoch);
    detail::P16(out, at, m.fields);
    detail::P32(out, at, m.ids);
    return at;
}
inline bool DecodeReset(const std::uint8_t* d, std::size_t size, BtsReset& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsReset) return false;
    std::size_t at = kBtsHeader;
    m.epoch = detail::G16(d, at);
    m.fields = detail::G16(d, at);
    m.ids = detail::G32(d, at);
    return m.epoch != 0;
}
inline std::size_t EncodeNoticed(std::uint32_t seq, const BtsNoticed& m, std::uint8_t* out, std::size_t cap) {
    if (!out || cap < kBtsNoticedSize || (m.kind != kNoticeNear && m.kind != kNoticeFound && m.kind != kNoticeTest)) return 0;
    for (int i = 0; i < 3; ++i)
        if (!detail::FiniteF(m.gazer[i]) || !detail::FiniteF(m.player[i])) return 0;
    if (!detail::FiniteF(m.game_dist)) return 0;
    std::size_t at = detail::Head(out, kBtsNoticed, 0, seq);
    detail::P8(out, at, m.kind);
    detail::P8(out, at, m.near_n);
    detail::P16(out, at, 0);
    detail::P32(out, at, m.field);
    detail::P32(out, at, m.loc);
    for (int i = 0; i < 3; ++i) detail::PF(out, at, m.gazer[i]);
    for (int i = 0; i < 3; ++i) detail::PF(out, at, m.player[i]);
    detail::PF(out, at, m.game_dist);
    return at;
}
inline bool DecodeNoticed(const std::uint8_t* d, std::size_t size, BtsNoticed& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsNoticed) return false;
    std::size_t at = kBtsHeader;
    m.kind = detail::G8(d, at);
    m.near_n = detail::G8(d, at);
    if (detail::G16(d, at) != 0) return false;
    m.field = detail::G32(d, at);
    m.loc = detail::G32(d, at);
    bool ok = true;
    for (int i = 0; i < 3; ++i) ok &= detail::GF(d, at, m.gazer[i]);
    for (int i = 0; i < 3; ++i) ok &= detail::GF(d, at, m.player[i]);
    ok &= detail::GF(d, at, m.game_dist);
    return ok && (m.kind == kNoticeNear || m.kind == kNoticeFound || m.kind == kNoticeTest);
}
inline std::size_t EncodeVoid(std::uint32_t seq, const BtsVoid& m, std::uint8_t* out, std::size_t cap) {
    if (!out || cap < kBtsVoidSize) return 0;
    for (int i = 0; i < 3; ++i)
        if (!detail::FiniteF(m.pos[i])) return 0;
    if (!detail::FiniteF(m.player_dist) || !detail::FiniteF(m.far_dist)) return 0;
    std::size_t at = detail::Head(out, kBtsVoid, 0, seq);
    detail::P8(out, at, m.reason);
    detail::P8(out, at, m.source);
    detail::P8(out, at, m.flags);
    detail::P8(out, at, 0);
    for (int i = 0; i < 3; ++i) detail::PF(out, at, m.pos[i]);
    detail::PF(out, at, m.player_dist);
    detail::PF(out, at, m.far_dist);
    detail::P32(out, at, m.sender_ms);
    detail::P32(out, at, 0);
    return at;
}
inline bool DecodeVoid(const std::uint8_t* d, std::size_t size, BtsVoid& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsVoid) return false;
    std::size_t at = kBtsHeader;
    m.reason = detail::G8(d, at);
    m.source = detail::G8(d, at);
    m.flags = detail::G8(d, at);
    if (detail::G8(d, at) != 0) return false;
    bool ok = true;
    for (int i = 0; i < 3; ++i) ok &= detail::GF(d, at, m.pos[i]);
    ok &= detail::GF(d, at, m.player_dist);
    ok &= detail::GF(d, at, m.far_dist);
    m.sender_ms = detail::G32(d, at);
    return ok && detail::G32(d, at) == 0 && (m.flags & ~0x0Fu) == 0;
}

// ---------------------------------------------------------------- the FIELD compare (each game, at each partner FIELD)
// records: the same record set (count and ids); kills: the same killed lists; per live field of the partner's: "equal" when this
// game has it live with the same count and print, "differs" when it has it live otherwise, "not here" when not streamed here
// (each game streams gazers around its own player only).
struct FieldCompare {
    bool records = false, kills = false;
    int live_equal = 0, live_differs = 0, live_not_here = 0;
    std::uint32_t Key(const FieldMsg& here, const FieldMsg& there) const {
        std::uint32_t h = kFnvSeed;
        h = FnvU32(h, (records ? 1u : 0u) | (kills ? 2u : 0u));
        h = FnvU32(h, static_cast<std::uint32_t>(live_equal) | static_cast<std::uint32_t>(live_differs) << 8 | static_cast<std::uint32_t>(live_not_here) << 16);
        h = FnvU32(h, static_cast<std::uint32_t>(here.unset) | static_cast<std::uint32_t>(there.unset) << 16);
        h = FnvU32(h, static_cast<std::uint32_t>(here.epoch) | static_cast<std::uint32_t>(there.epoch) << 16);
        return FnvU32(h, static_cast<std::uint32_t>(here.applied) | static_cast<std::uint32_t>(there.applied) << 16);
    }
};
inline const FieldEntry* FindEntry(const FieldMsg& m, std::uint32_t id) {
    for (unsigned i = 0; i < m.n && i < kBtsFieldMax; ++i)
        if (m.e[i].id == id) return &m.e[i];
    return nullptr;
}
inline FieldCompare CompareFields(const FieldMsg& here, const FieldMsg& there) {
    FieldCompare c;
    c.records = here.fields == there.fields && here.ids == there.ids;
    c.kills = here.kills == there.kills;
    for (unsigned i = 0; i < there.n && i < kBtsFieldMax; ++i) {
        const FieldEntry* l = FindEntry(here, there.e[i].id);
        if (!l || l->live == 0) ++c.live_not_here;
        else if (l->live == there.e[i].live && l->print == there.e[i].print) ++c.live_equal;
        else ++c.live_differs;
    }
    return c;
}
// One live field's compare word for the line.
inline const char* EntryCompareName(const FieldMsg& here, const FieldEntry& there) {
    const FieldEntry* l = FindEntry(here, there.id);
    if (!l || l->live == 0) return "not here";
    return (l->live == there.live && l->print == there.print) ? "print equal" : "print DIFFERS";
}

// ================================================================ round 30 BT S0: the world probes (bt_probe 9..14; hidden runs only)
// The action's v0: 0..2 stay ProbeMode; the exact integers 9..14 are the round-30 probe ops (13 is not built). Anything else is
// ProbeMode's clamp (7 -> 2, as before).
enum ProbeOpId : int { kOpThreads = 9, kOpKill = 10, kOpCrater = 11, kOpWeather = 12, kOpUnbuilt = 13, kOpHunter = 14 };
inline int ProbeOp(double v) {
    if (v >= 9 && v <= 14 && v == std::floor(v)) return static_cast<int>(v);
    return ProbeMode(v);
}

// The game's own gazer kills (FUN_142995210 and the message handlers, ds.exe DC) [V code; I the meaning of each reason]:
// the reason written to +0xCC and the messages that end a gazer.
constexpr std::uint32_t kKillRainGone = 0x01322187;   // gz_update: the rain under it ended (PTR_144BF0F28 effect, no stats)
constexpr std::uint32_t kKillCrater = 0x5414860D;     // a voidout crater
constexpr std::uint32_t kKillStatsA = 0x523D4DC0;     // with stats (FUN_142F21230)
constexpr std::uint32_t kKillStatsB = 0x74FB05EE;     // with stats (FUN_142F21230)
constexpr std::uint32_t kGazerMsgIds[] = {0x656FB96A, 0x682DFB3B, 0x7989539C, 0x6221E6CB, 0x5A7E4B1D};  // 0x682DFB3B = the catch

// bt_probe 10's locator: two 16-bit halves (the action's numbers stop at 1e9, a locator hash does not): hi 0x40AB lo 0x1A93 ->
// 0x40AB1A93. hi above 0xFFFF is the whole hash (lo must be 0); 0 0 = the nearest live gazer. False: not a locator.
inline bool LocArg(double hi, double lo, std::uint32_t* out) {
    if (!out || !(hi >= 0) || !(lo >= 0) || hi != std::floor(hi) || lo != std::floor(lo)) return false;
    if (hi > 0xFFFF) {
        if (lo != 0 || hi > 4294967295.0) return false;
        *out = static_cast<std::uint32_t>(hi);
        return true;
    }
    if (lo > 0xFFFF) return false;
    *out = static_cast<std::uint32_t>(hi) << 16 | static_cast<std::uint32_t>(lo);
    return true;
}

// P2's kill is applied only to a gazer the update would kill the same way: state 0, alive (+0x180 health > 0), alive at the last
// update (+0x2B0 > 0, so the death edge can fire), not dead (+0x1B4 bit 0x1) nor already killed (bit 0x8).
enum KillGuardResult : int { kKillOk = 0, kKillState = 1, kKillHealth = 2, kKillPrev = 3, kKillFlags = 4 };
inline int KillGuard(int state, float health, float prev, std::uint32_t flags) {
    if (state != 0) return kKillState;
    if (!(health > 0)) return kKillHealth;
    if (!(prev > 0)) return kKillPrev;
    if (flags & 0x9u) return kKillFlags;
    return kKillOk;
}
inline const char* KillGuardText(int r) {
    switch (r) {
        case kKillOk: return "ok";
        case kKillState: return "state not 0";
        case kKillHealth: return "health (+0x180) not above 0";
        case kKillPrev: return "last health (+0x2B0) not above 0";
        case kKillFlags: return "dead or killed (+0x1B4 & 0x9)";
    }
    return "?";
}

// P3's gate (the guarded crater): the voidout returns at once without a resource (+0x68); it makes a crater only when the slot
// (+0x2B0) is empty; and the local player must be at least the far-crater distance ([+0x68] +0x54) from the spot (the far crater,
// nothing near Sam). nullptr = go.
inline const char* CraterGate(bool has_resource, bool slot_empty, double player_dist, double far_dist) {
    if (!has_resource) return "the gazer manager has no resource (+0x68)";
    if (!slot_empty) return "a crater is already up (+0x2B0)";
    if (!(far_dist > 0) || !std::isfinite(far_dist)) return "the far-crater distance is unknown";
    if (!(player_dist >= far_dist)) return "the local player is inside the far-crater distance";
    return nullptr;
}

// A point m metres ahead of the player along their facing (fx, fy; the world transform's second row, Z up), at their height.
inline bool AheadOf(const double* me, double fx, double fy, double m, double* out) {
    if (!me || !out) return false;
    const double l = std::sqrt(fx * fx + fy * fy);
    if (!(l > 1e-6) || !std::isfinite(m)) return false;
    out[0] = me[0] + fx / l * m;
    out[1] = me[1] + fy / l * m;
    out[2] = me[2];
    return true;
}

// ================================================================ round 30 BT S1: TROUBLE and GZST (the partner's trouble, markers only)
// Nothing received here changes the game: the receiver logs, draws markers (bt_markers.cpp) and at most one toast per episode.
enum TroubleKind : std::uint8_t { kTrHunted = 1, kTrPulled = 2, kTrCaught = 3, kTrCatcher = 4, kTrClear = 5 };
// kTrfVehicle is reserved (not read yet); kTrPulled is decoded but never sent (no pull reading exists: plan P11).
enum TroubleFlag : std::uint8_t { kTrfGold = 1, kTrfVehicle = 2, kTrfCarried = 4, kTrfTest = 8 };
inline const char* TroubleName(std::uint8_t k) {
    switch (k) {
        case kTrHunted: return "HUNTED";
        case kTrPulled: return "PULLED";
        case kTrCaught: return "CAUGHT";
        case kTrCatcher: return "CATCHER";
        case kTrClear: return "CLEAR";
    }
    return "?";
}
struct BtsTrouble {
    std::uint8_t kind = 0, flags = 0;
    std::uint16_t episode = 0;
    float pos[3] = {};
    float hp = -1;
    std::uint32_t cclass = 0, field = 0, loc = 0;
};
inline std::size_t EncodeTrouble(std::uint32_t seq, const BtsTrouble& m, std::uint8_t* out, std::size_t cap) {
    if (!out || cap < kBtsTroubleSize) return 0;
    if (m.kind != kTrHunted && m.kind != kTrCaught && m.kind != kTrCatcher && m.kind != kTrClear) return 0;  // never PULLED
    if ((m.flags & ~0x0Fu) != 0 || !detail::FiniteF(m.hp)) return 0;
    for (int i = 0; i < 3; ++i)
        if (!detail::FiniteF(m.pos[i])) return 0;
    std::size_t at = detail::Head(out, kBtsTrouble, 0, seq);
    detail::P8(out, at, m.kind);
    detail::P8(out, at, m.flags);
    detail::P16(out, at, m.episode);
    for (int i = 0; i < 3; ++i) detail::PF(out, at, m.pos[i]);
    detail::PF(out, at, m.hp);
    detail::P32(out, at, m.cclass);
    detail::P32(out, at, m.field);
    detail::P32(out, at, m.loc);
    return at;
}
inline bool DecodeTrouble(const std::uint8_t* d, std::size_t size, BtsTrouble& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsTrouble) return false;
    std::size_t at = kBtsHeader;
    m.kind = detail::G8(d, at);
    m.flags = detail::G8(d, at);
    m.episode = detail::G16(d, at);
    bool ok = true;
    for (int i = 0; i < 3; ++i) ok &= detail::GF(d, at, m.pos[i]);
    ok &= detail::GF(d, at, m.hp);
    m.cclass = detail::G32(d, at);
    m.field = detail::G32(d, at);
    m.loc = detail::G32(d, at);
    return ok && m.kind >= kTrHunted && m.kind <= kTrClear && (m.flags & ~0x0Fu) == 0;
}

// GZST entry bits, from the gazer's +0x1B4 flags and +0x24 state (see "the gazer actor's flags" above).
enum GzBit : std::uint8_t { kGzNear = 1, kGzRealized = 2, kGzReact = 4, kGzCaught = 8, kGzDead = 0x10, kGzTest = 0x20 };
constexpr std::uint32_t kFlagJustRealized = 0x4u, kFlagCaught = 0x2000000u;
inline std::uint8_t GzBits(std::uint32_t flags, std::int32_t state) {
    std::uint8_t b = 0;
    if (flags & kFlagNear) b |= kGzNear;
    if (flags & kFlagJustRealized) b |= kGzRealized;
    if ((flags & 0x50u) == 0x50u) b |= kGzReact;
    if (flags & kFlagCaught) b |= kGzCaught;
    if ((flags & kFlagDead) || state == 2) b |= kGzDead;
    return b;
}
// A gazer worth a marker on the partner's screen: it senses, reacts to or holds the sender (a dead one never). "realized" alone is
// not enough: +0x1B4 0x4 is set on idle gazers too (flags 01010004 in the probe runs).
inline bool GzInteresting(std::uint8_t bits) { return (bits & kGzDead) == 0 && (bits & (kGzNear | kGzReact | kGzCaught | kGzTest)) != 0; }
inline void GzBitsText(std::uint8_t b, char* out, std::size_t cap) {
    if (!out || !cap) return;
    std::snprintf(out, cap, "%s%s%s%s%s%s%s", (b & kGzNear) ? " near" : "", (b & kGzRealized) ? " realized" : "", (b & kGzReact) ? " react" : "",
                  (b & kGzCaught) ? " caught" : "", (b & kGzDead) ? " dead" : "", (b & kGzTest) ? " test" : "", b ? "" : " none (ended)");
}
struct GzEntry {
    std::uint32_t field = 0, loc = 0;
    std::uint8_t state = 0, mode = 0, bits = 0;
    float pos[3] = {};
    float approach = 0;
};
struct BtsGzst {
    std::uint32_t sender_ms = 0;
    unsigned n = 0;
    GzEntry e[kBtsGzstMax];
};
inline std::size_t EncodeGzst(std::uint32_t seq, const BtsGzst& m, std::uint8_t* out, std::size_t cap) {
    if (!out || m.n > kBtsGzstMax || cap < kBtsGzstBase + m.n * kBtsGzstEntry) return 0;
    for (unsigned i = 0; i < m.n; ++i) {
        if ((m.e[i].bits & ~0x3Fu) != 0 || !detail::FiniteF(m.e[i].approach)) return 0;
        for (int k = 0; k < 3; ++k)
            if (!detail::FiniteF(m.e[i].pos[k])) return 0;
    }
    std::size_t at = detail::Head(out, kBtsGzst, static_cast<std::uint8_t>(m.n), seq);
    detail::P32(out, at, m.sender_ms);
    for (unsigned i = 0; i < m.n; ++i) {
        const GzEntry& e = m.e[i];
        detail::P32(out, at, e.field);
        detail::P32(out, at, e.loc);
        detail::P8(out, at, e.state);
        detail::P8(out, at, e.mode);
        detail::P8(out, at, e.bits);
        detail::P8(out, at, 0);
        for (int k = 0; k < 3; ++k) detail::PF(out, at, e.pos[k]);
        detail::PF(out, at, e.approach);
    }
    return at;
}
inline bool DecodeGzst(const std::uint8_t* d, std::size_t size, BtsGzst& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsGzst) return false;
    std::size_t at = kBtsHeader;
    m.sender_ms = detail::G32(d, at);
    m.n = h.n;
    bool ok = true;
    for (unsigned i = 0; i < m.n; ++i) {
        GzEntry& e = m.e[i];
        e.field = detail::G32(d, at);
        e.loc = detail::G32(d, at);
        e.state = detail::G8(d, at);
        e.mode = detail::G8(d, at);
        e.bits = detail::G8(d, at);
        ok &= detail::G8(d, at) == 0;
        for (int k = 0; k < 3; ++k) ok &= detail::GF(d, at, e.pos[k]);
        ok &= detail::GF(d, at, e.approach);
        ok &= (e.bits & ~0x3Fu) == 0;
    }
    return ok;
}

// The sender's trouble, one step per census (1 Hz) or test step. The priority is CATCHER > CAUGHT > HUNTED; an episode runs from the
// first trouble to the CLEAR (episode 0 is never used). A message goes on each kind change, on a catcher health change of 1 point or
// more (at most 2 Hz), and as a keepalive every second (the receiver's markers expire 3 s after the last one); CLEAR goes once.
struct TroubleIn {
    bool hunted = false, caught = false, catcher = false;
    float catcher_hp = 0;  // the catcher record's +0xF4
};
struct TroubleTracker {
    std::uint8_t kind = 0;  // 0 none, else the kind being sent
    std::uint16_t episode = 0;
    float hp_max = 0, sent_pct = -1;
    double sent_ms = -1e18;
    // The kind to send now (0 nothing); *pct is CATCHER's health in percent of the most seen this episode, else -1.
    std::uint8_t Step(const TroubleIn& in, double now_ms, float* pct) {
        if (pct) *pct = -1;
        const std::uint8_t want = in.catcher ? kTrCatcher : in.caught ? kTrCaught : in.hunted ? kTrHunted : 0;
        if (!want) {
            if (!kind) return 0;
            kind = 0, hp_max = 0, sent_pct = -1, sent_ms = now_ms;
            return kTrClear;
        }
        if (!kind) {
            if (++episode == 0) episode = 1;
            hp_max = 0, sent_pct = -1;
        }
        float p = -1;
        if (want == kTrCatcher) {
            const float hp = std::isfinite(in.catcher_hp) && in.catcher_hp > 0 ? in.catcher_hp : 0.0f;
            if (hp > hp_max) hp_max = hp;
            p = hp_max > 0 ? 100.0f * hp / hp_max : 0.0f;
        }
        if (pct) *pct = p;
        const bool edge = want != kind;
        const bool hp_step = want == kTrCatcher && std::fabs(p - sent_pct) >= 1.0f && now_ms - sent_ms >= 500.0;
        if (!edge && !hp_step && now_ms - sent_ms < 1000.0) return 0;
        kind = want, sent_pct = p, sent_ms = now_ms;
        return want;
    }
};

// The receiver's view of the partner's trouble: which TROUBLE lines are logged (a kind or episode change, a catcher health change of
// 10 points or more) and the one toast per episode (CAUGHT or the catcher fight starting). Expired: no TROUBLE for 3 s.
constexpr double kMarkLifeMs = 3000.0;
struct TroubleView {
    std::uint8_t kind = 0;
    std::uint16_t episode = 0, toasted = 0;
    float logged_hp = -1;
    double seen_ms = -1e18;
    bool have_toasted = false;
    struct Out {
        bool log = false, toast = false;
    };
    Out Take(const BtsTrouble& m, double now_ms) {
        Out o;
        if (m.kind == kTrClear) {
            o.log = kind != 0;
            kind = 0, logged_hp = -1;
            return o;
        }
        o.log = m.kind != kind || m.episode != episode || (m.kind == kTrCatcher && std::fabs(m.hp - logged_hp) >= 10.0f);
        if (o.log) logged_hp = m.kind == kTrCatcher ? m.hp : -1.0f;
        if ((m.kind == kTrCaught || m.kind == kTrCatcher) && (!have_toasted || toasted != m.episode)) {
            o.toast = true;
            have_toasted = true;
            toasted = m.episode;
        }
        kind = m.kind, episode = m.episode, seen_ms = now_ms;
        return o;
    }
    // True once when the trouble went quiet (no CLEAR came within kMarkLifeMs).
    bool Expire(double now_ms) {
        if (!kind || now_ms - seen_ms <= kMarkLifeMs) return false;
        kind = 0, logged_hp = -1;
        return true;
    }
};

// The receiver's markers (bt_markers.cpp): keyed entries refreshed by the messages, gone kMarkLifeMs after their last refresh. At
// most kMarkCap; a new one past the cap replaces the stalest.
enum MarkKind : std::uint8_t { kMarkGazer = 1, kMarkTar = 2, kMarkCatcher = 3 };
constexpr unsigned kMarkCap = 8;
struct MarkEntry {
    std::uint64_t key = 0;
    std::uint8_t kind = 0;
    double pos[3] = {};
    float hp = -1;  // catcher: percent; else -1
    bool gold = false, test = false;
    double seen_ms = 0;
};
struct MarkerBook {
    MarkEntry e[kMarkCap];
    unsigned n = 0;
    void Put(const MarkEntry& m) {
        for (unsigned i = 0; i < n; ++i)
            if (e[i].key == m.key) {
                e[i] = m;
                return;
            }
        if (n < kMarkCap) {
            e[n++] = m;
            return;
        }
        unsigned old = 0;
        for (unsigned i = 1; i < n; ++i)
            if (e[i].seen_ms < e[old].seen_ms) old = i;
        e[old] = m;
    }
    void Drop(std::uint64_t key) {
        for (unsigned i = 0; i < n; ++i)
            if (e[i].key == key) {
                e[i] = e[--n];
                return;
            }
    }
    void DropKind(std::uint8_t kind) {
        for (unsigned i = 0; i < n;)
            if (e[i].kind == kind) e[i] = e[--n];
            else ++i;
    }
    void Expire(double now_ms) {
        for (unsigned i = 0; i < n;)
            if (now_ms - e[i].seen_ms > kMarkLifeMs) e[i] = e[--n];
            else ++i;
    }
};
constexpr std::uint64_t kMarkKeyTar = 0x100000000ull, kMarkKeyCatcher = 0x200000000ull;
inline std::uint64_t GazerMarkKey(std::uint32_t loc) { return loc; }

// ================================================================ round 30 BT S2: KILL (gazer kills in both worlds)
enum KillCause : std::uint8_t { kCausePlayer = 1, kCauseStats = 2, kCauseTest = 3 };
// kKfSandboxed: the sender plays on a sandboxed save (the host applies a guest's kill only then); kKfMirrored is reserved (an
// applied partner kill is never sent back, so no such KILL is built).
enum KillFlag : std::uint8_t { kKfSandboxed = 0x01, kKfMirrored = 0x40, kKfTest = 0x80 };
inline const char* CauseName(std::uint8_t c) {
    return c == kCausePlayer ? "the player" : c == kCauseStats ? "a player's weapon (stats reason)" : c == kCauseTest ? "a test" : "?";
}
struct BtsKill {
    std::uint32_t field = 0, loc = 0, reason = 0, world = 0;
    std::uint8_t cause = 0, flags = 0;
    float pos[3] = {};
    std::uint32_t sender_ms = 0;
};
inline std::size_t EncodeKill(std::uint32_t seq, const BtsKill& m, std::uint8_t* out, std::size_t cap) {
    if (!out || cap < kBtsKillSize || m.loc == 0 || m.cause < kCausePlayer || m.cause > kCauseTest || (m.flags & ~0xC1u) != 0) return 0;
    for (int i = 0; i < 3; ++i)
        if (!detail::FiniteF(m.pos[i])) return 0;
    std::size_t at = detail::Head(out, kBtsKill, 0, seq);
    detail::P32(out, at, m.field);
    detail::P32(out, at, m.loc);
    detail::P32(out, at, m.reason);
    detail::P32(out, at, m.world);
    detail::P8(out, at, m.cause);
    detail::P8(out, at, m.flags);
    detail::P16(out, at, 0);
    for (int i = 0; i < 3; ++i) detail::PF(out, at, m.pos[i]);
    detail::P32(out, at, m.sender_ms);
    return at;
}
inline bool DecodeKill(const std::uint8_t* d, std::size_t size, BtsKill& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsKill) return false;
    std::size_t at = kBtsHeader;
    m.field = detail::G32(d, at);
    m.loc = detail::G32(d, at);
    m.reason = detail::G32(d, at);
    m.world = detail::G32(d, at);
    m.cause = detail::G8(d, at);
    m.flags = detail::G8(d, at);
    bool ok = detail::G16(d, at) == 0;
    for (int i = 0; i < 3; ++i) ok &= detail::GF(d, at, m.pos[i]);
    m.sender_ms = detail::G32(d, at);
    return ok && m.loc != 0 && m.cause >= kCausePlayer && m.cause <= kCauseTest && (m.flags & ~0xC1u) == 0;
}

// What a kill seen in this game's gazer update was (bt_world's tag): its own (none), the test op's (bt_sync 7), a partner kill
// applied here (mirror), the probe's (bt_probe 10).
enum KillTag : std::uint8_t { kTagNone = 0, kTagTest = 1, kTagMirror = 2, kTagProbe = 3 };
constexpr double kKillResetQuietMs = 5000.0, kKillEchoMs = 10000.0, kKillHoldMs = 60000.0, kKillMinDistM = 15.0;
// The relay rule: nullptr = send (cause in *cause), else why not. Only kills a player caused are sent: the killer (+0xD0) is this
// game's player, or the reason is one of the two stats reasons; the rain-gone and crater kills stay in each world. Never a kill
// applied from the partner, a probe's, an echo of the partner's (the same locator within 10 s) or one within 5 s of a reset.
inline const char* KillRelay(std::uint32_t reason, std::uint32_t killer, bool player_ok, std::uint32_t player_id, std::uint8_t tag, bool echo,
                             double ms_since_reset, std::uint8_t* cause) {
    if (cause) *cause = 0;
    if (tag == kTagMirror) return "a partner kill applied here (never sent back)";
    if (tag == kTagProbe) return "the probe's kill (bt_probe 10)";
    if (echo) return "the partner's kill of this gazer (echo, 10 s)";
    if (tag == kTagTest) {
        if (cause) *cause = kCauseTest;
        return nullptr;
    }
    if (ms_since_reset >= 0 && ms_since_reset < kKillResetQuietMs) return "within 5 s of a reset";
    if (reason == kKillRainGone) return "the rain under it ended (each world its own)";
    if (reason == kKillCrater) return "a voidout crater (each world its own)";
    if (reason == kKillStatsA || reason == kKillStatsB) {
        if (cause) *cause = kCauseStats;
        return nullptr;
    }
    if (player_ok && player_id != 0 && killer == player_id) {
        if (cause) *cause = kCausePlayer;
        return nullptr;
    }
    return "not caused by a player";
}

// The receiver's gate for one partner kill (the plan's apply rules and Must-NOT 5). apply: arm it now; hold: try again later (up to
// kKillHoldMs after it came); refuse: dropped with the reason.
enum class KillCall { apply, hold, refuse };
struct KillVerdict {
    KillCall call = KillCall::refuse;
    const char* why = nullptr;
};
struct KillApplyIn {
    int bt_kill = 0;                  // this game's bt_kill
    int role = 0;                     // 1 host, 2 guest
    bool sandboxed = false;           // this game: automation::SavesRedirected()
    bool sender_sandboxed = false;    // the KILL's kKfSandboxed
    bool world_equal = false;         // the KILL's world print equals ours (both known)
    bool records_equal = false;       // the last FIELD compare: the same BT record set
    double age_ms = 0;                // since it came
    const char* not_playing = nullptr;  // a load, menu, cutscene, Sam asleep (nullptr: playing)
    bool carried = false;             // Sam rides as a passenger
    int bt_allowed = 1;               // FUN_14299C1A0: 1 yes, 0 no, -1 unknown
    bool streamed = false;            // this game has that locator live
    bool scene_owner = false;         // +0x3A8: the locator's scene (FUN_142994B40), set for every placed gazer; logged only
    int guard = kKillOk;              // KillGuard on this game's copy
    double dist_m = -1;               // this game's copy to this game's player (-1 unknown)
};
inline KillVerdict KillApplyGate(const KillApplyIn& in) {
    auto refuse = [](const char* w) { return KillVerdict{KillCall::refuse, w}; };
    auto hold = [](const char* w) { return KillVerdict{KillCall::hold, w}; };
    if (in.bt_kill < 2) return refuse("bt_kill is below 2 here (relay only)");
    if (in.role == 2) {
        if (!in.sandboxed) return refuse("this game plays on its own real save (host_save=0): a partner's kill never changes it");
    } else if (in.role == 1) {
        if (!in.sender_sandboxed) return refuse("the guest plays on her own real save: her kills stay in her world");
        if (!in.world_equal) return refuse("another world (the enemy GUID prints differ or are unknown)");
        if (!in.records_equal) return refuse("the BT records differ (FIELD compare)");
    } else {
        return refuse("not a two-game role");
    }
    if (in.age_ms > kKillHoldMs) return refuse("held 60 s without a chance (expired)");
    if (in.not_playing) return hold(in.not_playing);
    if (in.carried) return hold("Sam rides as a passenger");
    if (in.bt_allowed == 0) return hold("BT gameplay is off here now (FUN_14299C1A0)");
    if (in.bt_allowed < 0) return hold("the BT gameplay check is unknown here");
    if (!in.streamed) return hold("the gazer is not streamed in here");
    // round 30: +0x3A8 is not a mission group. FUN_142994B40 sets it to [[actor+0x18]+0x80], the scene of the gazer's own locator,
    // so every placed gazer has it (the host's own test kill applies on such gazers). It never refuses a kill; it is only logged.
    if (in.guard == kKillFlags || in.guard == kKillHealth) return refuse("it already ended here");
    if (in.guard == kKillState) return hold("the gazer is not active here (state not 0)");
    if (in.guard == kKillPrev) return hold("the gazer has not updated here yet (+0x2B0 not above 0)");
    if (!(in.dist_m >= 0)) return hold("its distance to this game's player is unknown");
    if (in.dist_m < kKillMinDistM) return hold("within 15 m of this game's player");
    return KillVerdict{KillCall::apply, nullptr};
}

// The receiver's echo ring: locators killed from the partner within kKillEchoMs (a death edge of the same gazer here is not sent).
struct EchoRing {
    std::uint32_t loc[16] = {};
    double ms[16] = {};
    unsigned next = 0;
    void Add(std::uint32_t l, double now_ms) {
        loc[next % 16] = l;
        ms[next % 16] = now_ms;
        ++next;
    }
    bool Has(std::uint32_t l, double now_ms) const {
        if (!l) return false;
        for (unsigned i = 0; i < 16; ++i)
            if (loc[i] == l && now_ms >= ms[i] && now_ms - ms[i] <= kKillEchoMs) return true;
        return false;
    }
};

// bt_sync's op (the action's v0): 0 status, 1 FIELD now, 3 test VOID, 4 test NOTICED, 5 test TROUBLE sequence, 6 test GZST,
// 7 a real local kill (hi16 lo16, as bt_probe 10). -1 anything else.
inline int SyncOp(double v) {
    if (!std::isfinite(v) || v < 0 || v > 7 || v != std::floor(v)) return -1;
    const int op = static_cast<int>(v);
    return op == 2 ? -1 : op;
}
// bt_sync 5's test sequence: HUNTED at 0, CATCHER 100 % at 1.5 s, 60 % at 3 s, 20 % at 4.5 s, CLEAR at 6 s. The step due at t ms
// after the start (the index into kTestSteps), or -1 before the first.
struct TestStep {
    double at_ms;
    std::uint8_t kind;
    float hp;
};
constexpr TestStep kTestSteps[] = {{0, kTrHunted, -1}, {1500, kTrCatcher, 100}, {3000, kTrCatcher, 60}, {4500, kTrCatcher, 20}, {6000, kTrClear, -1}};
constexpr unsigned kTestStepCount = sizeof(kTestSteps) / sizeof(kTestSteps[0]);
inline int TestStepAt(double t_ms) {
    int k = -1;
    for (unsigned i = 0; i < kTestStepCount; ++i)
        if (t_ms >= kTestSteps[i].at_ms) k = static_cast<int>(i);
    return k;
}

}  // namespace st::btrules
