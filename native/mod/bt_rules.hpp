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
// 0x1 "realized": set while the actor state (+0x24) is 2 (0x1429956A8: cmp [r14+0x24],2; jne; or ecx,1) [V code; round 31 PF fact 2]
// 0x8 "dead": set when the health +0x180 <= 0 (0x1429956B9: vcomiss 0, [r14+0x180]; setae; .. or ecx,8) [V code; V run b3]
// 0x1000 "near": set at 0x142995C6A when the gazer's distance to the LOCAL player (+0x204, written at 0x1429953EB from
//   FUN_14283EC70 +0x270) is within its trigger radius ([+0x388] +0x98), state 0, a target at +0x3B0 and no 0x400000; cleared
//   at 0x142995526 when +0x204 > [[+0x388] +0x48] +0x48 [V code; V run: host 9.3 m -> set, the guest's copy of the same gazer
//   with the host's puppet 9.3 m away -> never set]
// 0x80000 "idle": cleared in the same write that sets 0x1000 (not set again in the run) [V code; I meaning]
// 0x1000000 "pending": recomputed every update from a call on +0x348 (0x1429956F3); set for ~2 s after the gazer spawns [V run; I]
constexpr std::uint32_t kFlagRealized = 0x1u, kFlagDead = 0x8u, kFlagNear = 0x1000u, kFlagIdle = 0x80000u, kFlagPending = 0x1000000u;
inline void FlagsText(std::uint32_t f, char* out, std::size_t cap) {
    if (!out || !cap) return;
    std::snprintf(out, cap, "%s%s%s%s%s%s%s%s%s%s", (f & kFlagRealized) ? " realized" : "", (f & kFlagDead) ? " dead" : "", (f & kFlagNear) ? " near" : "", (f & kFlagIdle) ? " idle" : "",
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
// Round 31 (types 9..16, the round-31 section at the end of this file; sizes are whole messages, the 12 B header included):
//   9 WXROW   (n 1..25), 36 + 8 n: the host's effective forecast, one chunk of the packed set
//  10 WXBEAT  (n 0), 40 B          11 HUNT (n 0), 56 B          12 GZST2 (n 0..6), 16 + 36 n
//  13 VOID2   (n 0), 60 B          14 CATCH (n 0..4), 16 + 32 n 15 REWARD (n 0), 32 B         16 RESPAWN (n 0), 24 B
// A decoder refuses another magic, version, type, size, n, a non-zero pad or a non-finite float.
constexpr std::size_t kBtsHeader = 12, kBtsHelloSize = 20, kBtsFieldBase = 36, kBtsFieldEntry = 16, kBtsResetSize = 20,
                      kBtsNoticedSize = 52, kBtsVoidSize = 44, kBtsTroubleSize = 44, kBtsGzstBase = 16, kBtsGzstEntry = 28,
                      kBtsKillSize = 48;
constexpr std::size_t kBtsWxRowBase = 36, kBtsWxRowEntry = 8, kBtsWxBeatSize = 40, kBtsHuntSize = 56, kBtsGzst2Base = 16,
                      kBtsGzst2Entry = 36, kBtsVoid2Size = 60, kBtsCatchBase = 16, kBtsCatchEntry = 32, kBtsRewardSize = 32,
                      kBtsRespawnSize = 24;
constexpr unsigned kBtsFieldMax = 6, kBtsGzstMax = 8, kBtsWxRowMax = 25, kBtsGzst2Max = 6, kBtsCatchMax = 4;
// The biggest BTS1 message: a full GZST, 240 B (a full FIELD is 132 B, a full WXROW 236 B, a full GZST2 232 B); link.cpp's BTS1
// slot (kBtMax) is 256 B.
constexpr std::size_t kBtsMaxSize = kBtsGzstBase + kBtsGzstMax * kBtsGzstEntry;
static_assert(kBtsMaxSize >= kBtsFieldBase + kBtsFieldMax * kBtsFieldEntry && kBtsMaxSize <= 256, "BTS1 sizes");
static_assert(kBtsWxRowBase + kBtsWxRowMax * kBtsWxRowEntry <= kBtsMaxSize && kBtsGzst2Base + kBtsGzst2Max * kBtsGzst2Entry <= kBtsMaxSize &&
                  kBtsCatchBase + kBtsCatchMax * kBtsCatchEntry <= kBtsMaxSize && kBtsWxBeatSize <= kBtsMaxSize && kBtsHuntSize <= kBtsMaxSize &&
                  kBtsVoid2Size <= kBtsMaxSize && kBtsRewardSize <= kBtsMaxSize && kBtsRespawnSize <= kBtsMaxSize,
              "round 31 BTS1 sizes");
enum BtsType : std::uint8_t {
    kBtsHello = 1, kBtsField = 2, kBtsReset = 3, kBtsNoticed = 4, kBtsVoid = 5, kBtsTrouble = 6, kBtsGzst = 7, kBtsKill = 8,
    kBtsWxRow = 9, kBtsWxBeat = 10, kBtsHunt = 11, kBtsGzst2 = 12, kBtsVoid2 = 13, kBtsCatch = 14, kBtsReward = 15, kBtsRespawn = 16  // round 31
};
// Round 31 fills the u16: every bit is now taken. A later feature needs a HELLO v2 (DecodeHello refuses a non-zero pad, so the
// caps cannot be widened in place).
enum BtsCap : std::uint16_t {
    kCapField = 1, kCapNoticed = 2, kCapVoid = 4, kCapResetApply = 8,
    kCapTrouble = 0x10, kCapGzst = 0x20, kCapKill = 0x40, kCapKillApply = 0x80,  // round 30 S1 / S2
    kCapWx = 0x100,          // round 31: this game takes part in WX (the host sends WXROW; both send WXBEAT; 0x200 applies)
    kCapWxApply = 0x200,     // this game applies the host's forecast (a sandboxed guest with bt_weather 2)
    kCapHunt = 0x400,        // HUNT (and the rain spots it implies)
    kCapGzst2 = 0x800,       // GZST2: "I mirror here"
    kCapVoid2 = 0x1000,      // VOID2
    kCapWorldApply = 0x2000, // craters and respawn applied here (the host or a sandboxed guest)
    kCapCatch = 0x4000,      // CATCH
    kCapReward = 0x8000      // rewards applied here (the host or a sandboxed guest)
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
        case kBtsWxRow: return h.n >= 1 && h.n <= kBtsWxRowMax && size == kBtsWxRowBase + h.n * kBtsWxRowEntry;
        case kBtsWxBeat: return h.n == 0 && size == kBtsWxBeatSize;
        case kBtsHunt: return h.n == 0 && size == kBtsHuntSize;
        case kBtsGzst2: return h.n <= kBtsGzst2Max && size == kBtsGzst2Base + h.n * kBtsGzst2Entry;
        case kBtsVoid2: return h.n == 0 && size == kBtsVoid2Size;
        case kBtsCatch: return h.n <= kBtsCatchMax && size == kBtsCatchBase + h.n * kBtsCatchEntry;
        case kBtsReward: return h.n == 0 && size == kBtsRewardSize;
        case kBtsRespawn: return h.n == 0 && size == kBtsRespawnSize;
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
// update (+0x2B0 > 0, so the death edge can fire), not realized (+0x1B4 bit 0x1) nor already killed (bit 0x8).
// Round 31 stage 8 (the kill look, leg K): allow_realized also takes a realized copy (state 2, bit 0x1) and refuses only the
// killed bit. It is passed only while the kill look is on (bt_kill_look=1, default 0): leg K decides whether the update's kill
// path gives a state-2 gazer its death; until it passes, every other caller keeps the state-0 rule.
enum KillGuardResult : int { kKillOk = 0, kKillState = 1, kKillHealth = 2, kKillPrev = 3, kKillFlags = 4 };
inline int KillGuard(int state, float health, float prev, std::uint32_t flags, bool allow_realized = false) {
    if (state != 0 && !(allow_realized && state == 2)) return kKillState;
    if (!(health > 0)) return kKillHealth;
    if (!(prev > 0)) return kKillPrev;
    if (flags & (allow_realized ? 0x8u : 0x9u)) return kKillFlags;
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
// kTrfVehicle is reserved (not read yet). kTrPulled: sent since round 31 (the hunter manager's hm+0x00 / +0x14, design 2.3); every
// older decoder already takes kind 2.
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
    if (m.kind < kTrHunted || m.kind > kTrClear) return 0;  // round 31: PULLED too
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
    (void)state;  // round 31: the state no longer counts (2 is "realized", PF fact 2); the GZST2 entry carries it
    if (flags & kFlagDead) b |= kGzDead;
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

// The sender's trouble, one step per census (1 Hz) or test step. The priority is CATCHER > CAUGHT > PULLED > HUNTED (round 31 adds
// PULLED); an episode runs from the first trouble to the CLEAR (episode 0 is never used). A message goes on each kind change, on a
// catcher health change of 1 point or more (at most 2 Hz), and as a keepalive every second (the receiver's markers expire 3 s after
// the last one); CLEAR goes once.
struct TroubleIn {
    bool hunted = false, caught = false, catcher = false;
    float catcher_hp = 0;  // the catcher record's +0xF4
    // round 31: set only by test op `bt_sync 8`. The natural read (hm+0x00 > 0 or hm+0x14 != 0: hands on Sam, the drag-under
    // level) was never seen non-zero (112 samples, probe-findings 12), so the sender logs it but does not set this from it yet.
    bool pulled = false;
};
struct TroubleTracker {
    std::uint8_t kind = 0;  // 0 none, else the kind being sent
    std::uint16_t episode = 0;
    float hp_max = 0, sent_pct = -1;
    double sent_ms = -1e18;
    // The kind to send now (0 nothing); *pct is CATCHER's health in percent of the most seen this episode, else -1.
    std::uint8_t Step(const TroubleIn& in, double now_ms, float* pct) {
        if (pct) *pct = -1;
        const std::uint8_t want = in.catcher ? kTrCatcher : in.caught ? kTrCaught : in.pulled ? kTrPulled : in.hunted ? kTrHunted : 0;
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
    float radius = 0;  // tar: the ring's radius in m (0: the default kTarRadiusM)
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
    // Round 31 stage 0 (runs 20261127-21xx): this game's player id is 0 ("[enemy] hits: .. the registry's player id 0x0: equal"),
    // and +0xD0 also reads 0 on kills nobody caused, so killer 0 is no proof: with id 0 only the stats reasons above relay.
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
// 7 a real local kill (hi16 lo16, as bt_probe 10); round 31 (the feature modules): 8 test HUNT / PULLED (bt_hunt), 9 test voidout
// (bt_void), 10 test catcher (bt_catcher), 11 bt_extra's test (RESPAWN, the sky, the real-save stand-in), 12 test tar HUNT
// (bt_tar), 13 test mirror turn-only (bt_mirror). -1 anything else.
inline int SyncOp(double v) {
    if (!std::isfinite(v) || v < 0 || v > 13 || v != std::floor(v)) return -1;
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

// ================================================================ round 31 stage 1: the pure part of "BTs fully working in co-op"
// (scratchpad round31/design.md sections 2.0-2.10; docs/enemy-sync.md from section 25 when the stages land). Nothing here calls
// the game. Every gate returns a decision and, unless it is the "go" decision, a why text for the log (KillApplyGate's style).
// Section 2.0's rule for a guest on her own real save (host_save=0, `sandboxed` false): no world write ever (no forecast, no crater
// record, no crystals, no respawn); only logs, markers, blasts, the gazer mirror's cosmetic fields and distant rain spots.

// Config levels (session.ini): a number, malformed / negative / NaN = 0, otherwise floor clamped to 0..max.
inline int CfgLevel(double v, int max) {
    if (!(v >= 0)) return 0;
    const double f = std::floor(v);
    return f >= max ? max : static_cast<int>(f);
}
// bt_void_flash_m: the voidout flash's FadeStart for a partner blast, metres (default 150; NaN / negative -> 150; at most 5000).
constexpr int kFlashStartDefaultM = 150, kFlashStartMaxM = 5000;
inline int FlashStartM(double v) {
    if (!(v >= 0) || !std::isfinite(v)) return kFlashStartDefaultM;
    return v >= kFlashStartMaxM ? kFlashStartMaxM : static_cast<int>(v);
}

// ---------------------------------------------------------------- WX: the forecast hash (FUN_142E183F0, ds.exe DC) [V code]
// The job generates a base schedule byte for slot time T and region r (row + 0x0C[i]) at 0x142E1875B..0x142E18846. Read from the
// instructions, not the decompile (the casts are the instructions' own: vcvtss2sd / vcvtsd2ss):
//   s = floorf(T / len + 0.5f)                     len = ws+0x64B4 (600.0); 0.5f at 0x143B87F90; floorf = the import at 0x143B24D08
//   a = (s * len) / 3633.0f                        float; 3633.0f = 0x45631000 at 0x143E0997C
//   b = (float)r * 0.0078125f                      float; 0x3C000000 at 0x143C07E04
//   x = (float)((double)a * 2.067390879775102 + (double)b * 12.451168662908248)        doubles at 0x143DB7388 / 0x143DB73B0
//   f = (float)((double)(x - floorf(x)) - 0.5)                                          0.5 at 0x143B8F2C0
//   d = (double)(f * f)                            the square is a float multiply (vmulss)
//   p = (float)(((d * 53.392573080032136 - 38.026512460676564) * d + 6.182785114200511) * (double)f)   0x143DB73C0 / B8 / A0
//   y = (float)((double)p * 43758.5453)            a DOUBLE multiply: 0x40E55DD17318FC50 at 0x143DB73E0 (not 43758.547f)
//   n = y - floorf(y)                              float
// Then the type row at 0x144C77ED0 + type * 0x10 {sunny, cloudy, rain, heavy} (the row's +0x00 type, a movsxd i32):
//   n >= (rain + cloudy) + heavy -> 0 sunny; n >= heavy + rain -> 1 cloudy; n < heavy -> 3 heavy; else 2 rain
// (vcomiss + jb: a NaN n falls through to 3). Types 0..13 only: type 14 (Invalid / NotChange) would read the next table's pointers.
struct ForecastProb {
    float sunny, cloudy, rain, heavy;
};
constexpr int kForecastTypeCount = 14;  // EDSWeatherForecastType 0..13 (9..13 the ...ForMule copies of 0, 2, 1, 7, 8)
// clang-format off
inline constexpr ForecastProb kForecastTable[kForecastTypeCount] = {  // 0x144C77ED0, byte for byte (tests check the bits)
    {0, 0.15f, 0.75f, 0.1f},  {0, 0.9f, 0.1f, 0},  {0, 0.5f, 0.45f, 0.05f}, {0, 0, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}, {1, 0, 0, 0},
    {0, 0.2f, 0.8f, 0},       {0, 0.5f, 0.5f, 0},  {0, 0.15f, 0.75f, 0.1f}, {0, 0.5f, 0.45f, 0.05f}, {0, 0.9f, 0.1f, 0},
    {0, 0.2f, 0.8f, 0},       {0, 0.5f, 0.5f, 0},
};
// clang-format on
constexpr float kForecastSlotLen = 600.0f;     // ws+0x64B4
constexpr unsigned kWxRegions = 64, kWxSlots = 32;
constexpr std::uint8_t kForecastNone = 0xFF;  // not a type the table holds
inline float ForecastNoise(float t, int region, float slot_len = kForecastSlotLen) {
    const float s = std::floor(t / slot_len + 0.5f);
    const float a = (s * slot_len) / 3633.0f;
    const float b = static_cast<float>(region) * 0.0078125f;
    const float x = static_cast<float>(static_cast<double>(a) * 2.067390879775102 + static_cast<double>(b) * 12.451168662908248);
    const float f = static_cast<float>(static_cast<double>(x - std::floor(x)) - 0.5);
    const float f2 = f * f;
    const double d = static_cast<double>(f2);
    const float p = static_cast<float>(((d * 53.392573080032136 - 38.026512460676564) * d + 6.182785114200511) * static_cast<double>(f));
    const float y = static_cast<float>(static_cast<double>(p) * 43758.5453);
    return y - std::floor(y);
}
inline std::uint8_t ForecastByte(float n, const ForecastProb& pr) {
    if (n >= (pr.rain + pr.cloudy) + pr.heavy) return 0;
    if (n >= pr.heavy + pr.rain) return 1;
    return (n < pr.heavy || std::isnan(n)) ? 3 : 2;
}
// The base byte the job writes for slot time t, region r, forecast type `type`; kForecastNone for a type outside 0..13.
inline std::uint8_t ForecastSlot(float t, int region, int type, float slot_len = kForecastSlotLen) {
    if (type < 0 || type >= kForecastTypeCount) return kForecastNone;
    return ForecastByte(ForecastNoise(t, region, slot_len), kForecastTable[type]);
}
// The job's slot times: T0 = the clock, then T += len in float (vaddss xmm9, xmm7 at 0x142E1887A), index (int)((T - clock) / len).
// T after i steps; NOT the base index that T lands in (ForecastSlotIndex: the game truncates, so the two can differ).
inline float ForecastSlotTime(float clock, int i, float slot_len = kForecastSlotLen) {
    float t = clock;
    for (int k = 0; k < i; ++k) t += slot_len;
    return t;
}
// The base index the job writes T's byte to: (int)((T - clock) / len), all in float, truncated (vsubss / vdivss / vcvttss2si at
// 0x142E18700). When T crosses into the next power-of-two range it rounds, so (T - clock) / len can fall just below the step
// number: two steps then land in one index (the later write wins) and the last index is not written that run.
inline int ForecastSlotIndex(float clock, float t, float slot_len = kForecastSlotLen) {
    const float d = t - clock;  // each step rounded to float, as the game does it
    const float q = d / slot_len;
    if (!(q > -2147483648.0f && q < 2147483648.0f)) return -1;
    return static_cast<int>(q);  // truncation toward zero (cvttss2si)
}
// The job's own replay of one generation from `clock`: steps T = clock, T += len while T < end (end = clock + 32 len, xmm8) and the
// index < 32 (0x142E1870C). out[i]: the step whose byte index i holds after the run (-1: not written); returns the steps run.
inline int ForecastReplay(float clock, float end, int (&out)[kWxSlots], float (&t_out)[kWxSlots], float slot_len = kForecastSlotLen) {
    for (unsigned i = 0; i < kWxSlots; ++i) out[i] = -1, t_out[i] = 0;
    float t = clock;
    int steps = 0;
    while (end > t && steps < 1000) {
        const int idx = ForecastSlotIndex(clock, t, slot_len);
        if (idx < 0 || idx >= static_cast<int>(kWxSlots)) break;
        out[idx] = steps, t_out[idx] = t;
        ++steps;
        t += slot_len;
    }
    return steps;
}
// Probe 17's golden vectors go here (from its log: `[bt] wx golden: T .. region .. type .. slot .. game ..`, the game's own base
// bytes). Until then the sentinel row only.
struct ForecastGolden {
    float t;
    int region, type;
    std::uint8_t want;  // kForecastNone: the sentinel (skipped)
};
inline constexpr ForecastGolden kForecastGolden[] = {{0, 0, 0, kForecastNone}};

// ---------------------------------------------------------------- WX: the packed schedule
// One region's effective schedule (row + 0x2C[32], values 0..3) in a u64: 32 slots x 2 bits, slot 0 in the low bits.
inline unsigned Popcount64(std::uint64_t v) {
    unsigned n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}
inline bool PackRow(const std::uint8_t* eff32, std::uint64_t* out) {
    if (!eff32 || !out) return false;
    std::uint64_t v = 0;
    for (unsigned i = 0; i < kWxSlots; ++i) {
        if (eff32[i] > 3) return false;
        v |= static_cast<std::uint64_t>(eff32[i]) << (2 * i);
    }
    *out = v;
    return true;
}
inline void UnpackRow(std::uint64_t v, std::uint8_t* eff32) {
    for (unsigned i = 0; i < kWxSlots; ++i) eff32[i] = static_cast<std::uint8_t>((v >> (2 * i)) & 3u);
}
// The whole set: rows[k] is the k-th region of the mask (ascending region index); n == popcount(mask).
struct WxSchedule {
    std::uint8_t slot = 0;  // ws+0x64A0, 1..3
    std::uint32_t gen = 0;  // the sender's change counter
    float clock = 0;        // ws+0x64A8 at the snapshot
    std::uint64_t mask = 0; // ws+0x48
    unsigned n = 0;
    std::uint64_t rows[kWxRegions] = {};
    std::uint32_t crc = 0;  // ScheduleCrc(mask, rows, n)
};
// crc32c (Crc32cU64), initial ~0, over the mask then each row in order, final ~.
inline std::uint32_t ScheduleCrc(std::uint64_t mask, const std::uint64_t* rows, unsigned n) {
    std::uint32_t c = Crc32cU64(0xFFFFFFFFu, mask);
    for (unsigned i = 0; i < n && rows; ++i) c = Crc32cU64(c, rows[i]);
    return ~c;
}
// eff[r][32] per region index r (only the masked ones are read). False: a byte above 3.
inline bool PackSchedule(std::uint64_t mask, const std::uint8_t (*eff)[kWxSlots], WxSchedule& out) {
    if (!eff) return false;
    out.mask = mask;
    out.n = 0;
    for (unsigned r = 0; r < kWxRegions; ++r) {
        if (!(mask >> r & 1u)) continue;
        if (!PackRow(eff[r], &out.rows[out.n])) return false;
        ++out.n;
    }
    out.crc = ScheduleCrc(out.mask, out.rows, out.n);
    return true;
}
// The other way; regions outside the mask are zeroed. False: n is not popcount(mask).
inline bool UnpackSchedule(const WxSchedule& s, std::uint8_t (*eff)[kWxSlots]) {
    if (!eff || s.n != Popcount64(s.mask) || s.n > kWxRegions) return false;
    unsigned k = 0;
    for (unsigned r = 0; r < kWxRegions; ++r) {
        if (s.mask >> r & 1u) UnpackRow(s.rows[k++], eff[r]);
        else std::memset(eff[r], 0, kWxSlots);
    }
    return true;
}
// The compare log's count: regions whose rows differ, plus regions in only one mask. 0 = "schedule equal".
inline int ScheduleDiffRegions(const WxSchedule& a, const WxSchedule& b) {
    int d = 0;
    unsigned ka = 0, kb = 0;
    for (unsigned r = 0; r < kWxRegions; ++r) {
        const bool ia = (a.mask >> r & 1u) != 0, ib = (b.mask >> r & 1u) != 0;
        if (ia && ib) {
            if (ka < a.n && kb < b.n && a.rows[ka] != b.rows[kb]) ++d;
        } else if (ia || ib) {
            ++d;
        }
        ka += ia ? 1u : 0u;
        kb += ib ? 1u : 0u;
    }
    return d;
}

namespace detail {
inline void P64(std::uint8_t* p, std::size_t& at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[at++] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline std::uint64_t G64(const std::uint8_t* p, std::size_t& at) {
    const std::uint64_t v = Le64(p + at);
    at += 8;
    return v;
}
inline void PD(std::uint8_t* p, std::size_t& at, double d) {
    std::uint64_t v = 0;
    std::memcpy(&v, &d, 8);
    P64(p, at, v);
}
inline bool GD(const std::uint8_t* p, std::size_t& at, double& out) {
    const std::uint64_t v = G64(p, at);
    std::memcpy(&out, &v, 8);
    return std::isfinite(out) && std::fabs(out) <= 1e7;
}
inline bool PosOk(const float* p) {
    for (int i = 0; i < 3; ++i)
        if (!FiniteF(p[i])) return false;
    return true;
}
}  // namespace detail

// ---------------------------------------------------------------- 9 WXROW
// Body 24 B: u8 slot | u8 first (index in mask order) | u8 total (regions in the mask) | u8 flags (kWxrLast) | u32 gen | f32 clock |
// u32 crc (of the whole packed set) | u64 mask; then n x u64 rows. 64 regions take 3 messages (25 + 25 + 14).
constexpr std::uint8_t kWxrLast = 1;
struct BtsWxRow {
    std::uint8_t slot = 0, first = 0, total = 0, flags = 0;
    std::uint32_t gen = 0;
    float clock = 0;
    std::uint32_t crc = 0;
    std::uint64_t mask = 0;
    unsigned n = 0;
    std::uint64_t rows[kBtsWxRowMax] = {};
};
inline bool WxRowValid(const BtsWxRow& m) {
    return m.slot >= 1 && m.slot <= 3 && m.total >= 1 && m.total <= kWxRegions && m.total == Popcount64(m.mask) && m.n >= 1 &&
           m.n <= kBtsWxRowMax && m.first + m.n <= m.total && (m.flags & ~kWxrLast) == 0 &&
           ((m.flags & kWxrLast) != 0) == (m.first + m.n == m.total) && detail::FiniteF(m.clock);
}
inline std::size_t EncodeWxRow(std::uint32_t seq, const BtsWxRow& m, std::uint8_t* out, std::size_t cap) {
    if (!out || !WxRowValid(m) || cap < kBtsWxRowBase + m.n * kBtsWxRowEntry) return 0;
    std::size_t at = detail::Head(out, kBtsWxRow, static_cast<std::uint8_t>(m.n), seq);
    detail::P8(out, at, m.slot);
    detail::P8(out, at, m.first);
    detail::P8(out, at, m.total);
    detail::P8(out, at, m.flags);
    detail::P32(out, at, m.gen);
    detail::PF(out, at, m.clock);
    detail::P32(out, at, m.crc);
    detail::P64(out, at, m.mask);
    for (unsigned i = 0; i < m.n; ++i) detail::P64(out, at, m.rows[i]);
    return at;
}
inline bool DecodeWxRow(const std::uint8_t* d, std::size_t size, BtsWxRow& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsWxRow) return false;
    std::size_t at = kBtsHeader;
    m.slot = detail::G8(d, at);
    m.first = detail::G8(d, at);
    m.total = detail::G8(d, at);
    m.flags = detail::G8(d, at);
    m.gen = detail::G32(d, at);
    const bool ok = detail::GF(d, at, m.clock);
    m.crc = detail::G32(d, at);
    m.mask = detail::G64(d, at);
    m.n = h.n;
    for (unsigned i = 0; i < m.n; ++i) m.rows[i] = detail::G64(d, at);
    return ok && WxRowValid(m);
}
// The sender's chunks for one snapshot (at most 3). 0: a bad snapshot (slot, n, mask, clock).
constexpr unsigned kWxChunksMax = (kWxRegions + kBtsWxRowMax - 1) / kBtsWxRowMax;
inline unsigned WxChunks(const WxSchedule& s, BtsWxRow* out, unsigned cap) {
    if (!out || s.slot < 1 || s.slot > 3 || s.n < 1 || s.n > kWxRegions || s.n != Popcount64(s.mask) || !detail::FiniteF(s.clock)) return 0;
    const unsigned need = (s.n + kBtsWxRowMax - 1) / kBtsWxRowMax;
    if (cap < need) return 0;
    for (unsigned c = 0; c < need; ++c) {
        BtsWxRow& m = out[c];
        m = BtsWxRow{};
        m.slot = s.slot, m.gen = s.gen, m.clock = s.clock, m.crc = s.crc, m.mask = s.mask;
        m.total = static_cast<std::uint8_t>(s.n);
        m.first = static_cast<std::uint8_t>(c * kBtsWxRowMax);
        m.n = std::min<unsigned>(kBtsWxRowMax, s.n - m.first);
        m.flags = m.first + m.n == s.n ? kWxrLast : 0;
        for (unsigned i = 0; i < m.n; ++i) m.rows[i] = s.rows[m.first + i];
    }
    return need;
}

// The guest's chunk assembler. A snapshot counts only when every chunk of the same gen (the same slot, mask, total, clock and crc)
// is in and the crc of the rows matches; until then `done` (the last complete one) stays what the guest applies. A chunk of a newer
// gen drops the partial set; an older gen is stale; a chunk whose header disagrees with the partial set restarts it from that chunk.
enum class WxPut { refused, partial, complete, duplicate, stale, restarted, crc_mismatch };
inline const char* WxPutName(WxPut p) {
    switch (p) {
        case WxPut::refused: return "refused (bad chunk)";
        case WxPut::partial: return "partial";
        case WxPut::complete: return "complete";
        case WxPut::duplicate: return "duplicate (already complete)";
        case WxPut::stale: return "stale (an older gen)";
        case WxPut::restarted: return "restarted (the chunk header differs from the partial set)";
        case WxPut::crc_mismatch: return "crc mismatch (dropped)";
    }
    return "?";
}
inline bool GenNewer(std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a - b) > 0; }
struct WxAssembler {
    WxSchedule done;
    bool have_done = false;
    WxSchedule part;
    bool have_part = false;
    std::uint64_t got = 0;  // bit k: row k of the partial set is in
    int crc_fails = 0;
    void Reset() { *this = WxAssembler{}; }
    WxPut Take(const BtsWxRow& m) {
        if (!WxRowValid(m)) return WxPut::refused;
        if (have_done && m.gen == done.gen) return m.crc == done.crc && m.mask == done.mask ? WxPut::duplicate : WxPut::stale;
        if (have_done && GenNewer(done.gen, m.gen)) return WxPut::stale;
        if (have_part && GenNewer(part.gen, m.gen)) return WxPut::stale;
        WxPut how = WxPut::partial;
        const bool same = have_part && part.gen == m.gen;
        if (same && (part.slot != m.slot || part.mask != m.mask || part.n != m.total || part.crc != m.crc || part.clock != m.clock))
            how = WxPut::restarted;
        if (!same || how == WxPut::restarted) {
            part = WxSchedule{};
            part.slot = m.slot, part.gen = m.gen, part.clock = m.clock, part.mask = m.mask, part.n = m.total, part.crc = m.crc;
            have_part = true, got = 0;
        }
        for (unsigned i = 0; i < m.n; ++i) {
            const unsigned k = m.first + i;
            if ((got >> k & 1u) && part.rows[k] != m.rows[i]) {  // the same row twice, two values: start over from this chunk
                part.rows[k] = m.rows[i];
                got = 0;
                for (unsigned j = 0; j < m.n; ++j) part.rows[m.first + j] = m.rows[j], got |= 1ull << (m.first + j);
                how = WxPut::restarted;
                break;
            }
            part.rows[k] = m.rows[i];
            got |= 1ull << k;
        }
        const std::uint64_t all = part.n >= 64 ? ~0ull : ((1ull << part.n) - 1);
        if (got != all) return how;
        have_part = false, got = 0;
        if (ScheduleCrc(part.mask, part.rows, part.n) != part.crc) {
            ++crc_fails;
            return WxPut::crc_mismatch;
        }
        done = part, have_done = true;
        return WxPut::complete;
    }
};

// ---------------------------------------------------------------- 10 WXBEAT
// Body 28 B: u8 slot | u8 state (kWxPlaying..) | u16 0 | u32 gen | f32 clock | u32 crc | f32 raw rain at the sender's Sam
// (FUN_142E0C3F0) | u8 the sender's region (FUN_142E0C6B0; 0xFF unknown) | u8 its eff[0] (0xFF unknown) | u16 0 | u32 sender ms.
enum WxBeatState : std::uint8_t { kWxPlaying = 0, kWxPaused = 1, kWxLoading = 2 };
constexpr std::uint8_t kWxUnknown = 0xFF;
struct BtsWxBeat {
    std::uint8_t slot = 0, state = 0;
    std::uint32_t gen = 0;
    float clock = 0;
    std::uint32_t crc = 0;
    float rain = 0;
    std::uint8_t region = kWxUnknown, eff0 = kWxUnknown;
    std::uint32_t sender_ms = 0;
};
inline bool WxBeatValid(const BtsWxBeat& m) {
    return m.slot <= 3 && m.state <= kWxLoading && (m.region < kWxRegions || m.region == kWxUnknown) && (m.eff0 <= 3 || m.eff0 == kWxUnknown) &&
           detail::FiniteF(m.clock) && detail::FiniteF(m.rain);
}
inline std::size_t EncodeWxBeat(std::uint32_t seq, const BtsWxBeat& m, std::uint8_t* out, std::size_t cap) {
    if (!out || cap < kBtsWxBeatSize || !WxBeatValid(m)) return 0;
    std::size_t at = detail::Head(out, kBtsWxBeat, 0, seq);
    detail::P8(out, at, m.slot);
    detail::P8(out, at, m.state);
    detail::P16(out, at, 0);
    detail::P32(out, at, m.gen);
    detail::PF(out, at, m.clock);
    detail::P32(out, at, m.crc);
    detail::PF(out, at, m.rain);
    detail::P8(out, at, m.region);
    detail::P8(out, at, m.eff0);
    detail::P16(out, at, 0);
    detail::P32(out, at, m.sender_ms);
    return at;
}
inline bool DecodeWxBeat(const std::uint8_t* d, std::size_t size, BtsWxBeat& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsWxBeat) return false;
    std::size_t at = kBtsHeader;
    m.slot = detail::G8(d, at);
    m.state = detail::G8(d, at);
    bool ok = detail::G16(d, at) == 0;
    m.gen = detail::G32(d, at);
    ok &= detail::GF(d, at, m.clock);
    m.crc = detail::G32(d, at);
    ok &= detail::GF(d, at, m.rain);
    m.region = detail::G8(d, at);
    m.eff0 = detail::G8(d, at);
    ok &= detail::G16(d, at) == 0;
    m.sender_ms = detail::G32(d, at);
    return ok && WxBeatValid(m);
}

// ---------------------------------------------------------------- WX: the guest's apply gate (design 2.1)
// apply: write the host's rows in the job detour this run (instant: also +0x6515 = 1, once); restore: undo our write now (row+0x08
// = row+0x04, +0x64AC = +0x64B0 = clock - 1); hold: nothing to do (or nothing yet). A failing rule is a restore only while our write
// is in (`applied`), else a hold. A load holds (the job is not ours to touch then) and the caller re-arms the instant first apply.
constexpr double kWxStaleMs = 10000.0;
enum class WxCall { apply, restore, hold };
struct WxVerdict {
    WxCall call = WxCall::hold;
    const char* why = nullptr;
    bool instant = false;
};
struct WxApplyIn {
    int bt_weather = 0;        // this game's bt_weather
    int role = 0;              // 1 host, 2 guest
    bool sandboxed = false;    // automation::SavesRedirected()
    bool link = false;         // a two-game session with the partner's HELLO and cap kCapWx
    bool emergency = false;    // EmergencyStop
    bool loading = false;      // LoadingNow
    bool have_snapshot = false;
    int slot_here = 0;         // ws+0x64A0
    int slot_host = 0;         // the snapshot's slot
    double age_ms = 0;         // since the snapshot was taken (its WXBEAT or the last chunk)
    bool mask_equal = false;   // the snapshot's mask == ws+0x48 here
    bool applied = false;      // our write is in now
    bool rearm = false;        // a session start, a new slot or after a load: the next apply is instant
};
inline WxVerdict WxApplyGate(const WxApplyIn& in) {
    auto stop = [&](const char* w) { return WxVerdict{in.applied ? WxCall::restore : WxCall::hold, w, false}; };
    if (in.emergency) return stop("EmergencyStop");
    if (in.role != 2) return stop("not the guest (the host sends its forecast)");
    if (in.bt_weather < 2) return stop("bt_weather is below 2 here (compare only)");
    if (!in.sandboxed) return stop("this game plays on its own real save (host_save=0)");
    if (!in.link) return stop("link lost");
    if (in.loading) return WxVerdict{WxCall::hold, "a load", false};
    if (!in.have_snapshot) return stop("no complete snapshot from the host yet");
    if (in.slot_host < 1 || in.slot_host > 3 || in.slot_here != in.slot_host) return stop("another area (the forecast slots differ)");
    if (!(in.age_ms >= 0) || in.age_ms > kWxStaleMs) return stop("stale (no host forecast for 10 s)");
    if (!in.mask_equal) return stop("the region mask differs");
    return WxVerdict{WxCall::apply, nullptr, !in.applied || in.rearm};
}
inline const char* WxCallName(WxCall c) { return c == WxCall::apply ? "apply" : c == WxCall::restore ? "restore" : "hold"; }

// ---------------------------------------------------------------- 11 HUNT and PULLED (design 2.3)
// Body 44 B: u8 kind (kHuntUp / kHuntClear) | u8 flags (kHf*) | u16 episode | f32 centre[3] | f32 radius | u8 hands (hm+0x00, clamped
// to 255) | u8 members | u16 0 | f32 level (hm+0x10; 1.2 = drag-under) | f32 strength (hm+0x04) | f32 dir[2] (hm+0x08 / +0x0C) |
// u32 sender ms.
enum HuntKind : std::uint8_t { kHuntUp = 1, kHuntClear = 2 };
enum HuntFlag : std::uint8_t { kHfGold = 1, kHfVehicle = 2, kHfPulled = 4, kHfTest = 8, kHfDeep = 0x10 };
constexpr float kHuntRadiusMax = 1000.0f, kPullDeepLevel = 1.0f, kPullUnderLevel = 1.2f;
struct BtsHunt {
    std::uint8_t kind = 0, flags = 0;
    std::uint16_t episode = 0;
    float center[3] = {};
    float radius = 0;
    std::uint8_t hands = 0, members = 0;
    float level = 0, strength = 0, dir[2] = {};
    std::uint32_t sender_ms = 0;
};
inline bool HuntValid(const BtsHunt& m) {
    return (m.kind == kHuntUp || m.kind == kHuntClear) && (m.flags & ~0x1Fu) == 0 && m.episode != 0 && detail::PosOk(m.center) &&
           detail::FiniteF(m.radius) && m.radius >= 0 && m.radius <= kHuntRadiusMax && detail::FiniteF(m.level) && detail::FiniteF(m.strength) &&
           detail::FiniteF(m.dir[0]) && detail::FiniteF(m.dir[1]);
}
inline std::size_t EncodeHunt(std::uint32_t seq, const BtsHunt& m, std::uint8_t* out, std::size_t cap) {
    if (!out || cap < kBtsHuntSize || !HuntValid(m)) return 0;
    std::size_t at = detail::Head(out, kBtsHunt, 0, seq);
    detail::P8(out, at, m.kind);
    detail::P8(out, at, m.flags);
    detail::P16(out, at, m.episode);
    for (int i = 0; i < 3; ++i) detail::PF(out, at, m.center[i]);
    detail::PF(out, at, m.radius);
    detail::P8(out, at, m.hands);
    detail::P8(out, at, m.members);
    detail::P16(out, at, 0);
    detail::PF(out, at, m.level);
    detail::PF(out, at, m.strength);
    detail::PF(out, at, m.dir[0]);
    detail::PF(out, at, m.dir[1]);
    detail::P32(out, at, m.sender_ms);
    return at;
}
inline bool DecodeHunt(const std::uint8_t* d, std::size_t size, BtsHunt& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsHunt) return false;
    std::size_t at = kBtsHeader;
    m.kind = detail::G8(d, at);
    m.flags = detail::G8(d, at);
    m.episode = detail::G16(d, at);
    bool ok = true;
    for (int i = 0; i < 3; ++i) ok &= detail::GF(d, at, m.center[i]);
    ok &= detail::GF(d, at, m.radius);
    m.hands = detail::G8(d, at);
    m.members = detail::G8(d, at);
    ok &= detail::G16(d, at) == 0;
    ok &= detail::GF(d, at, m.level);
    ok &= detail::GF(d, at, m.strength);
    ok &= detail::GF(d, at, m.dir[0]);
    ok &= detail::GF(d, at, m.dir[1]);
    m.sender_ms = detail::G32(d, at);
    return ok && HuntValid(m);
}

// The sender's HUNT episode machine. Active while hm+0x140 is set (up) or Sam is pulled (`pulled`: set only by test op bt_sync 8
// this round; the natural hm+0x00 > 0 or hm+0x14 != 0 was never seen non-zero, probe-findings 12). An episode
// runs from the first active step to the CLEAR (episode 0 is never used). UP goes at 4 Hz while active and at once on a flag edge
// (pulled, deep, gold, vehicle); CLEAR goes once.
constexpr double kHuntSendMs = 250.0;
struct HuntIn {
    bool up = false, pulled = false, gold = false, vehicle = false, test = false;
    float center[3] = {};
    float radius = 0;
    int hands = 0, members = 0;
    float level = 0, strength = 0, dir[2] = {};
};
inline std::uint8_t HuntFlags(const HuntIn& in) {
    return static_cast<std::uint8_t>((in.gold ? kHfGold : 0) | (in.vehicle ? kHfVehicle : 0) | (in.pulled ? kHfPulled : 0) |
                                     (in.test ? kHfTest : 0) | (in.pulled && in.level >= kPullDeepLevel ? kHfDeep : 0));
}
struct HuntTracker {
    bool active = false;
    std::uint16_t episode = 0;
    std::uint8_t flags_sent = 0;
    double sent_ms = -1e18, start_ms = 0;
    BtsHunt last;
    // kHuntUp / kHuntClear with *out filled, or 0 (nothing to send now).
    std::uint8_t Step(const HuntIn& in, double now_ms, BtsHunt* out) {
        const bool want = in.up || in.pulled;
        if (!want) {
            if (!active) return 0;
            active = false;
            BtsHunt c = last;
            c.kind = kHuntClear, c.flags = static_cast<std::uint8_t>(last.flags & kHfTest), c.hands = 0, c.level = 0, c.strength = 0;
            c.dir[0] = c.dir[1] = 0;
            sent_ms = now_ms;
            if (out) *out = c;
            return kHuntClear;
        }
        bool edge = false;
        if (!active) {
            active = true, edge = true, start_ms = now_ms;
            if (++episode == 0) episode = 1;
        }
        const std::uint8_t f = HuntFlags(in);
        edge |= f != flags_sent;
        if (!edge && now_ms - sent_ms < kHuntSendMs) return 0;
        BtsHunt m;
        m.kind = kHuntUp, m.flags = f, m.episode = episode;
        for (int i = 0; i < 3; ++i) m.center[i] = detail::FiniteF(in.center[i]) ? in.center[i] : 0.0f;
        m.radius = detail::FiniteF(in.radius) ? std::clamp(in.radius, 0.0f, kHuntRadiusMax) : 0.0f;
        m.hands = static_cast<std::uint8_t>(std::clamp(in.hands, 0, 255));
        m.members = static_cast<std::uint8_t>(std::clamp(in.members, 0, 255));
        m.level = detail::FiniteF(in.level) ? in.level : 0.0f;
        m.strength = detail::FiniteF(in.strength) ? in.strength : 0.0f;
        m.dir[0] = detail::FiniteF(in.dir[0]) ? in.dir[0] : 0.0f;
        m.dir[1] = detail::FiniteF(in.dir[1]) ? in.dir[1] : 0.0f;
        flags_sent = f, sent_ms = now_ms, last = m;
        if (out) *out = m;
        return kHuntUp;
    }
};
// The receiver's view: which HUNT lines are logged (UP on a new episode, PULLED on the pulled or deep flag rising, CLEAR once with
// the episode's length) and the expiry (no HUNT for 3 s ends it, as a CLEAR would). SinceMs feeds TarFeelGate's 3 s rule.
struct HuntView {
    bool up = false;
    std::uint16_t episode = 0;
    std::uint8_t flags = 0;
    double start_ms = 0, seen_ms = -1e18;
    BtsHunt last;
    struct Out {
        bool up = false, pulled = false, clear = false;
        double secs = 0;  // CLEAR: the episode's length
    };
    Out Take(const BtsHunt& m, double now_ms) {
        Out o;
        if (m.kind == kHuntClear) {
            if (up) o.clear = true, o.secs = (now_ms - start_ms) / 1000.0;
            up = false, flags = 0, seen_ms = now_ms;
            return o;
        }
        if (!up || m.episode != episode) {
            o.up = true, start_ms = now_ms, flags = 0;
        }
        const std::uint8_t rise = static_cast<std::uint8_t>(m.flags & ~flags);
        o.pulled = (rise & (kHfPulled | kHfDeep)) != 0 && (m.flags & kHfPulled) != 0;
        up = true, episode = m.episode, flags = m.flags, seen_ms = now_ms, last = m;
        return o;
    }
    bool Expire(double now_ms) {
        if (!up || now_ms - seen_ms <= kMarkLifeMs) return false;
        up = false, flags = 0;
        return true;
    }
    double SinceMs(double now_ms) const { return now_ms - seen_ms; }
};

// ---------------------------------------------------------------- RS: the rain spot gate (design 2.2)
// make: call FUN_142E14F60 now (create or refresh, life RainSpotLife); hold: no call now and keep the slot (an existing spot keeps
// its last life and expires on its own); drop: no call, free the slot and log "rain spot off".
// review (rules 2): the game keeps a spot until its life runs out after the last refresh (w_183f0: entry +0x18 -= dt, removed below
// 0), so on a real save the hold distance must cover what Sam can travel in that life: the life there is the 2 s refresh + 1 s,
// and the margin r + 20 m + 30 m/s (a truck at top speed) x 3 s = r + 110 m. At the last refresh she was beyond it, so when the
// spot is gone she is still more than r + 20 m away.
constexpr unsigned kSpotSlots = 8;
constexpr double kSpotGazerNearM = 150.0, kSpotRadiusMaxM = 400.0;
constexpr float kSpotLifeS = 10.0f, kSpotHuntRadiusM = 50.0f, kSpotFieldRadiusM = 100.0f, kSpotCatcherExtraM = 30.0f;
constexpr float kSpotRefreshS = 2.0f, kSpotLifeRealS = kSpotRefreshS + 1.0f;  // a real-save game's spots: refresh + 1 s
constexpr double kSpotRealClearM = 20.0, kSpotRealSpeedMps = 30.0;
constexpr double kSpotRealMarginM = kSpotRealClearM + kSpotRealSpeedMps * static_cast<double>(kSpotLifeRealS);  // 110 m
// The life passed to FUN_142E14F60: 10 s, or 3 s on a real save (it outlives the last refresh by at most that). Review (safety 1):
// RainSpotGate now drops every spot on a real save, so the 3 s life and the r + 110 m margin are a second lock only.
inline float RainSpotLife(bool real_save) { return real_save ? kSpotLifeRealS : kSpotLifeS; }
// The drop text on a real-save guest (RainSpotGate, and bt_hunt's SpotsOff before any slot is booked).
constexpr const char* kSpotRealSaveWhy = "this game plays on its own real save (host_save=0): a rain spot is timefall there";
enum class SpotCall { make, hold, drop };
struct SpotVerdict {
    SpotCall call = SpotCall::drop;
    const char* why = nullptr;
};
struct RainSpotIn {
    int bt_rain = 0;
    bool source_live = false;          // its HUNT / CATCH / FIELD is current
    bool real_save = false;            // this game is a guest on her own real save
    const char* not_playing = nullptr; // a load, a menu, a cutscene, Sam asleep
    bool carried = false;              // Sam rides as a passenger
    double my_dist_m = -1;             // this game's Sam to the spot's centre (-1 unknown)
    float radius = 0;
    bool gazer_near = false;           // a gazer streamed here within 150 m of the spot
    unsigned slots_used = 0;           // our spot slots in use (this one not counted when it has a slot)
    bool has_slot = false;
};
inline SpotVerdict RainSpotGate(const RainSpotIn& in) {
    auto drop = [](const char* w) { return SpotVerdict{SpotCall::drop, w}; };
    auto hold = [](const char* w) { return SpotVerdict{SpotCall::hold, w}; };
    if (in.bt_rain < 1) return drop("bt_rain is 0 here");
    // review (safety 1): a rain spot is real timefall at that place (her cargo, vehicles and structures there age and her BTs
    // there arm: world state in her own save), not a cosmetic effect, so a real-save guest never makes one (as DrySpotAllowed)
    if (in.real_save) return drop(kSpotRealSaveWhy);
    if (!in.source_live) return drop("its source ended");
    if (!(in.radius > 0) || !(in.radius <= kSpotRadiusMaxM)) return drop("a bad radius");
    if (in.gazer_near) return drop("a gazer streamed here is within 150 m (this game makes its own rain there)");
    if (in.not_playing) return hold(in.not_playing);
    if (!in.has_slot && in.slots_used >= kSpotSlots) return hold("all 8 spot slots are in use");
    return SpotVerdict{SpotCall::make, nullptr};
}
// A catcher's spot radius: ActiveAreaDistance + 30 m (the CATCH radius; 130 m with the default 100 m).
inline float CatcherSpotRadius(float active_area_m) {
    return (detail::FiniteF(active_area_m) && active_area_m > 0 ? active_area_m : 100.0f) + kSpotCatcherExtraM;
}
// The catcher-end dry spot (kind 0, 300 s) changes region rain (row+0x58 bit 1): a forecast change, so never on a real save.
inline bool DrySpotAllowed(bool real_save, int bt_rain) { return !real_save && bt_rain >= 1; }

// ---------------------------------------------------------------- TF: the tar feel record gate (design 2.4)
// in: the record should be in the list (add it if not; refresh its centre and radius); out: take it out now (if in); hold: leave
// the list as it is this tick (no add).
constexpr double kTarNearM = 300.0, kTarQuietMs = 3000.0;
constexpr int kTarListMax = 10;
enum class TarCall { in, out, hold };
struct TarVerdict {
    TarCall call = TarCall::out;
    const char* why = nullptr;
};
struct TarFeelIn {
    int bt_tar = 0;
    bool emergency = false;
    bool hunt_up = false;              // HuntView.up
    double since_hunt_ms = 1e18;       // since the last HUNT
    bool loading = false;
    const char* not_playing = nullptr; // a menu, a cutscene, Sam asleep (not a load)
    bool carried = false;
    bool real_save = false;
    bool real_save_ok = false;         // probe 26 showed no cargo, health or saved-state change (false until then)
    bool hm_ready = false;             // hm+0x138 set
    double dist_m = -1;                // the partner's tar centre to this game's Sam (-1 unknown)
    int count = 0;                     // the list's count (hm+0xC0)
    bool ours_in = false;
};
inline TarVerdict TarFeelGate(const TarFeelIn& in) {
    auto out = [](const char* w) { return TarVerdict{TarCall::out, w}; };
    if (in.emergency) return out("EmergencyStop");
    if (in.bt_tar < 1) return out("bt_tar is 0 here");
    if (!in.hunt_up) return out("the partner's hunt ended (CLEAR)");
    if (in.since_hunt_ms > kTarQuietMs) return out("no HUNT for 3 s");
    if (in.loading) return out("a load");
    if (in.carried) return out("Sam rides as a passenger");
    if (in.real_save && !in.real_save_ok) return out("this game plays on its own real save (host_save=0): tar feel waits for probe 26");
    if (!in.hm_ready) return out("the hunter manager is not ready (+0x138)");
    if (!(in.dist_m >= 0) || in.dist_m > kTarNearM) return out("the partner's tar is more than 300 m from Sam");
    if (in.not_playing) return TarVerdict{TarCall::hold, in.not_playing};
    if (!in.ours_in && in.count >= kTarListMax) return TarVerdict{TarCall::hold, "list full (10): not added"};
    return TarVerdict{TarCall::in, nullptr};
}

// ---------------------------------------------------------------- 12 GZST2 and GM, the gazer mirror (design 2.5)
// Body 4 + 36 n: u32 sender ms | n x { u32 field | u32 loc | u8 state | u8 mode (+0x1A8) | u8 bits (kGz*) | u8 kind (+0x214) |
// u32 action hash (+0x200) | u8 blend (+0x20C * 255) | u8 agitation (* 255) | u8 edges (kGe*) | u8 0 | f32 entity pos[3] |
// i16 yaw (centidegrees, -18000..18000) | u16 the sender's distance +0x204 (decimetres; 0xFFFF unknown) }
enum GzEdge : std::uint8_t { kGeFound = 1, kGeCaught = 2 };
constexpr std::uint16_t kGzDistUnknown = 0xFFFF;
struct Gz2Entry {
    std::uint32_t field = 0, loc = 0;
    std::uint8_t state = 0, mode = 0, bits = 0, kind = 0;
    std::uint32_t action = 0;
    std::uint8_t blend = 0, agit = 0, edges = 0;
    float pos[3] = {};
    std::int16_t yaw = 0;
    std::uint16_t dist_dm = kGzDistUnknown;
};
struct BtsGzst2 {
    std::uint32_t sender_ms = 0;
    unsigned n = 0;
    Gz2Entry e[kBtsGzst2Max];
};
inline bool Gz2EntryValid(const Gz2Entry& e) {
    return (e.bits & ~0x3Fu) == 0 && (e.edges & ~0x03u) == 0 && detail::PosOk(e.pos) && e.yaw >= -18000 && e.yaw <= 18000;
}
inline std::size_t EncodeGzst2(std::uint32_t seq, const BtsGzst2& m, std::uint8_t* out, std::size_t cap) {
    if (!out || m.n > kBtsGzst2Max || cap < kBtsGzst2Base + m.n * kBtsGzst2Entry) return 0;
    for (unsigned i = 0; i < m.n; ++i)
        if (!Gz2EntryValid(m.e[i])) return 0;
    std::size_t at = detail::Head(out, kBtsGzst2, static_cast<std::uint8_t>(m.n), seq);
    detail::P32(out, at, m.sender_ms);
    for (unsigned i = 0; i < m.n; ++i) {
        const Gz2Entry& e = m.e[i];
        detail::P32(out, at, e.field);
        detail::P32(out, at, e.loc);
        detail::P8(out, at, e.state);
        detail::P8(out, at, e.mode);
        detail::P8(out, at, e.bits);
        detail::P8(out, at, e.kind);
        detail::P32(out, at, e.action);
        detail::P8(out, at, e.blend);
        detail::P8(out, at, e.agit);
        detail::P8(out, at, e.edges);
        detail::P8(out, at, 0);
        for (int k = 0; k < 3; ++k) detail::PF(out, at, e.pos[k]);
        detail::P16(out, at, static_cast<std::uint16_t>(e.yaw));
        detail::P16(out, at, e.dist_dm);
    }
    return at;
}
inline bool DecodeGzst2(const std::uint8_t* d, std::size_t size, BtsGzst2& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsGzst2) return false;
    std::size_t at = kBtsHeader;
    m.sender_ms = detail::G32(d, at);
    m.n = h.n;
    bool ok = true;
    for (unsigned i = 0; i < m.n; ++i) {
        Gz2Entry& e = m.e[i];
        e.field = detail::G32(d, at);
        e.loc = detail::G32(d, at);
        e.state = detail::G8(d, at);
        e.mode = detail::G8(d, at);
        e.bits = detail::G8(d, at);
        e.kind = detail::G8(d, at);
        e.action = detail::G32(d, at);
        e.blend = detail::G8(d, at);
        e.agit = detail::G8(d, at);
        e.edges = detail::G8(d, at);
        ok &= detail::G8(d, at) == 0;
        for (int k = 0; k < 3; ++k) ok &= detail::GF(d, at, e.pos[k]);
        e.yaw = static_cast<std::int16_t>(detail::G16(d, at));
        e.dist_dm = detail::G16(d, at);
        ok &= Gz2EntryValid(e);
    }
    return ok;
}
// The field packers: 0..1 -> 0..255 (NaN 0); radians -> centidegrees in -18000..18000; metres -> decimetres (0xFFFF unknown, at most
// 0xFFFE).
inline std::uint8_t Unit8(float v) {
    if (!(v > 0)) return 0;
    return v >= 1 ? 255 : static_cast<std::uint8_t>(v * 255.0f + 0.5f);
}
inline std::int16_t YawCdeg(double rad) {
    if (!std::isfinite(rad)) return 0;
    double deg = std::fmod(rad * 57.29577951308232, 360.0);
    if (deg > 180.0) deg -= 360.0;
    if (deg < -180.0) deg += 360.0;
    return static_cast<std::int16_t>(std::lround(std::clamp(deg, -180.0, 180.0) * 100.0));
}
inline std::uint16_t DistDm(double m) {
    if (!(m >= 0) || !std::isfinite(m)) return kGzDistUnknown;
    const double dm = m * 10.0 + 0.5;
    return dm >= 65534.0 ? static_cast<std::uint16_t>(0xFFFE) : static_cast<std::uint16_t>(dm);
}
// The mirror's actions are never the catch path. 0x3752F65E is refused whatever probe 21 shows (design 2.5).
constexpr std::uint32_t kGzActionFound = 0x6221E6CB, kGzActionCatch = 0x5A7E4B1D, kGzMsgCaught = 0x682DFB3B, kGzActionNever = 0x3752F65E;
inline bool GzMirrorDrops(std::uint32_t action_or_msg) {
    return action_or_msg == kGzActionFound || action_or_msg == kGzActionCatch || action_or_msg == kGzMsgCaught;
}

// GzMirrorGate, per gazer, with hysteresis on the local distance: on above trigger + 5 m, released below trigger + 2 m (between
// the two a mirror keeps its state). mirror: (keep) mirroring it this frame; release: stop now (the update rewrites the flags next
// frame, no restore needed); none: not mirrored, nothing to do.
constexpr double kGzOnMarginM = 5.0, kGzOffMarginM = 2.0, kGzQuietMs = 2000.0;
constexpr unsigned kGzMirrorMax = 6;
constexpr float kGzAgitOn = 0.5f;
enum class GzCall { mirror, release, none };
struct GzVerdict {
    GzCall call = GzCall::none;
    const char* why = nullptr;
};
struct GzMirrorIn {
    int bt_gazer_mirror = 0;
    bool emergency = false;
    bool mirrored = false;             // this gazer is mirrored now
    unsigned mirrored_n = 0;           // all mirrored now (this one included when mirrored)
    std::uint8_t partner_bits = 0;     // its last GZST2 entry's bits
    double since_gzst2_ms = 1e18;      // since that entry
    bool resolved = false;             // the same field and locator, live and streamed here
    bool loading = false;
    const char* not_playing = nullptr;
    int state = 0;                     // +0x24 here
    bool dead = false, caught_here = false;
    double local_dist_m = -1;          // +0x204 here (this game's player)
    double trigger_m = -1;             // [+0x388] +0x98 here
};
inline GzVerdict GzMirrorGate(const GzMirrorIn& in) {
    auto stop = [&](const char* w) { return GzVerdict{in.mirrored ? GzCall::release : GzCall::none, w}; };
    if (in.emergency) return stop("EmergencyStop");
    if (in.bt_gazer_mirror < 1) return stop("bt_gazer_mirror is 0 here");
    if (in.loading) return stop("a load");
    if (in.since_gzst2_ms > kGzQuietMs) return stop("no GZST2 for 2 s");
    if ((in.partner_bits & kGzDead) || !(in.partner_bits & (kGzNear | kGzReact | kGzCaught))) return stop("the partner's gazer no longer senses her");
    if (!in.resolved) return stop("not live and streamed here");
    if (in.not_playing) return stop(in.not_playing);
    if (in.state != 0) return stop("not active here (state not 0)");
    if (in.dead) return stop("dead here");
    if (in.caught_here) return stop("it holds this game's player");
    if (!(in.local_dist_m >= 0) || !(in.trigger_m >= 0)) return stop("its distance or trigger radius is unknown here");
    if (in.mirrored) {
        if (in.local_dist_m < in.trigger_m + kGzOffMarginM) return stop("this game's player came within its trigger radius + 2 m");
        return GzVerdict{GzCall::mirror, nullptr};
    }
    if (!(in.local_dist_m > in.trigger_m + kGzOnMarginM)) return stop("this game's player is within its trigger radius + 5 m");
    if (in.mirrored_n >= kGzMirrorMax) return stop("6 gazers are mirrored already");
    return GzVerdict{GzCall::mirror, nullptr};
}
// The yaw step toward the partner body, rate-limited (90 deg/s): the new yaw from `cur` toward `want` (radians) after dt seconds.
constexpr double kGzYawRateRad = 1.5707963267948966;
inline double YawStep(double cur, double want, double dt_s) {
    if (!std::isfinite(cur) || !std::isfinite(want) || !(dt_s > 0)) return cur;
    const double pi = 3.141592653589793;
    double d = std::fmod(want - cur, 2 * pi);
    if (d > pi) d -= 2 * pi;
    if (d < -pi) d += 2 * pi;
    const double lim = kGzYawRateRad * dt_s;
    return cur + std::clamp(d, -lim, lim);
}

// ---------------------------------------------------------------- 13 VOID2 and VB, the blast (design 2.6)
// Body 48 B: u8 source (VoidSource) | u8 reason | u8 flags (kV2*) | u8 catcher class + 1 (0 none) | f64 pos[3] | f32 crater radius
// (0 none) | f32 the sender player's distance | u32 sender ms | u32 voidout id | u32 world print.
enum Void2Flag : std::uint8_t {
    kV2Gazer = 1, kV2Baggage = 2, kV2Far = 4, kV2Test = 8, kV2Catcher = 0x10, kV2CraterWanted = 0x20
};
// EDSCatcherType (0x144BFEB74; catcher.md): 0 Octpus (the tar-man), 1 Quad (the lion), 2 Whale, 3 Titan, 4 Ghost, 5..7 Reserve.
// On the wire as class + 1 (0 = unknown), at most 8.
constexpr std::uint8_t kCatcherClassMax1 = 8;
inline const char* CatcherTypeName(std::uint8_t class1) {
    switch (class1) {
        case 1: return "Octpus";
        case 2: return "Quad";
        case 3: return "Whale";
        case 4: return "Titan";
        case 5: return "Ghost";
        case 6:
        case 7:
        case 8: return "Reserve";
    }
    return "?";
}
inline const char* CatcherLook(std::uint8_t class1) {
    switch (class1) {
        case 1: return "tar-man";
        case 2: return "lion";
        case 3: return "whale";
        case 4: return "giant";
        case 5: return "ghost";
    }
    return "?";
}
struct BtsVoid2 {
    std::uint8_t source = 0, reason = 0, flags = 0, class1 = 0;
    double pos[3] = {};
    float crater_r = 0, player_dist = -1;
    std::uint32_t sender_ms = 0, void_id = 0, world = 0;
};
inline bool Void2Valid(const BtsVoid2& m) {
    bool ok = (m.flags & ~0x3Fu) == 0 && m.class1 <= kCatcherClassMax1 && detail::FiniteF(m.crater_r) && m.crater_r >= 0 && m.crater_r <= 1000 &&
              detail::FiniteF(m.player_dist);
    for (int i = 0; i < 3; ++i) ok &= std::isfinite(m.pos[i]) && std::fabs(m.pos[i]) <= 1e7;
    return ok;
}
inline std::size_t EncodeVoid2(std::uint32_t seq, const BtsVoid2& m, std::uint8_t* out, std::size_t cap) {
    if (!out || cap < kBtsVoid2Size || !Void2Valid(m)) return 0;
    std::size_t at = detail::Head(out, kBtsVoid2, 0, seq);
    detail::P8(out, at, m.source);
    detail::P8(out, at, m.reason);
    detail::P8(out, at, m.flags);
    detail::P8(out, at, m.class1);
    for (int i = 0; i < 3; ++i) detail::PD(out, at, m.pos[i]);
    detail::PF(out, at, m.crater_r);
    detail::PF(out, at, m.player_dist);
    detail::P32(out, at, m.sender_ms);
    detail::P32(out, at, m.void_id);
    detail::P32(out, at, m.world);
    return at;
}
inline bool DecodeVoid2(const std::uint8_t* d, std::size_t size, BtsVoid2& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsVoid2) return false;
    std::size_t at = kBtsHeader;
    m.source = detail::G8(d, at);
    m.reason = detail::G8(d, at);
    m.flags = detail::G8(d, at);
    m.class1 = detail::G8(d, at);
    bool ok = true;
    for (int i = 0; i < 3; ++i) ok &= detail::GD(d, at, m.pos[i]);
    ok &= detail::GF(d, at, m.crater_r);
    ok &= detail::GF(d, at, m.player_dist);
    m.sender_ms = detail::G32(d, at);
    m.void_id = detail::G32(d, at);
    m.world = detail::G32(d, at);
    return ok && Void2Valid(m);
}

// BlastGate: the receiver's cosmetic blast (every save). play: now (far: the far resource res+0x30, else res+0x28); hold: later (up
// to 10 s after it came); drop: never. One mirrored blast at a time; a load drops the queue.
// review (stage 6 VB): probe 29 played blasts only at 300, 600 and 1000 m (the far resource) with dev_god 2 holding Sam, so the
// near resource res+0x28 and a blast inside the receiver's radius are untested. Until the stage 6 gate's dev_god 0 leg (0 / 30 /
// 90 m: Sam's health, vehicle hp, cargo durability, flow calls) passes, a real-save guest drops blasts nearer than 300 m (the
// nearest tested distance) or at an unknown distance.
constexpr double kBlastQueueMs = 10000.0, kBlastWindowMs = 9000.0, kBlastRefMs = 16000.0, kBlastFarDefaultM = 100.0;
constexpr double kBlastRealMinM = 300.0;
enum class BlastCall { play, hold, drop };
struct BlastVerdict {
    BlastCall call = BlastCall::drop;
    const char* why = nullptr;
    bool far_res = false;  // (not `far`: a windows.h macro)
};
struct BlastIn {
    int bt_void = 0;
    bool emergency = false;
    bool pos_ok = false;               // VOID2 decoded (its position is sane)
    double age_ms = 0;                 // since it came
    bool loading = false;
    const char* not_playing = nullptr;
    bool blast_active = false;         // a mirrored blast is playing (its 16 s ref is held)
    bool has_resource = false;         // [gazerMgr+0x68]
    double dist_m = -1;                // this game's Sam to the spot (-1 unknown: near resource)
    double far_m = kBlastFarDefaultM;  // res+0x54
    bool real_save = false;            // this game is a guest on her own real save (host_save=0)
};
inline BlastVerdict BlastGate(const BlastIn& in) {
    auto drop = [](const char* w) { return BlastVerdict{BlastCall::drop, w, false}; };
    auto hold = [](const char* w) { return BlastVerdict{BlastCall::hold, w, false}; };
    if (in.emergency) return drop("EmergencyStop");
    if (in.bt_void < 1) return drop("bt_void is 0 here");
    if (!in.pos_ok) return drop("a bad position");
    if (in.loading) return drop("a load");
    if (!(in.age_ms <= kBlastQueueMs)) return drop("queued 10 s without a chance");
    if (in.real_save && !(in.dist_m >= kBlastRealMinM))
        return drop("within 300 m of you or at an unknown distance (real save: the near blast is untested)");
    if (in.not_playing) return hold(in.not_playing);
    if (in.blast_active) return hold("another partner blast is playing");
    if (!in.has_resource) return hold("the gazer manager has no resource (+0x68)");
    const double far_m = in.far_m > 0 && std::isfinite(in.far_m) ? in.far_m : kBlastFarDefaultM;
    return BlastVerdict{BlastCall::play, nullptr, in.dist_m >= far_m};
}
// The flash's distance fade as the effector applies it [guess, probe 29 mode 3]: 1 within FadeStart, 0 beyond FadeStart +
// FadeRange, linear between. The partner blast's effector gets FadeStart = bt_void_flash_m, FadeRange = 450 (originals 11500 /
// 11800, restored when the 9 s window closes).
constexpr float kFlashRangeM = 450.0f, kFlashOrigStart = 11500.0f, kFlashOrigRange = 11800.0f;
inline float FlashFade(double dist_m, double start_m = kFlashStartDefaultM, double range_m = kFlashRangeM) {
    if (!std::isfinite(dist_m) || dist_m <= start_m) return 1.0f;
    if (!(range_m > 0) || dist_m >= start_m + range_m) return 0.0f;
    return static_cast<float>(1.0 - (dist_m - start_m) / range_m);
}

// ---------------------------------------------------------------- CR: the crater record gate (design 2.7)
constexpr double kCraterHoldMs = 60000.0, kCraterNearExtraM = 10.0;
constexpr float kCraterRadiusDefault = 107.0f, kCraterDelayS = 8.1f;
struct CraterApplyIn {
    int bt_void = 0;
    int role = 0;
    bool sandboxed = false;
    bool sender_sandboxed = false;   // the host's rule for a guest's crater
    bool world_equal = false;
    bool records_equal = false;      // FIELD "records equal"
    std::uint8_t flags = 0;          // the VOID2's kV2*
    bool gazer_crater_ok = false;    // probe 35 showed the base game leaves a record after a gazer / corpse voidout (false until then)
    double age_ms = 0;
    bool loading = false;
    const char* not_playing = nullptr;
    bool carried = false;
};
inline KillVerdict CraterApplyGate(const CraterApplyIn& in) {
    auto refuse = [](const char* w) { return KillVerdict{KillCall::refuse, w}; };
    auto hold = [](const char* w) { return KillVerdict{KillCall::hold, w}; };
    if (in.bt_void < 2) return refuse("bt_void is below 2 here (the blast only)");
    if (!(in.flags & kV2CraterWanted)) return refuse("the sender wants no crater");
    if (!(in.flags & kV2Catcher) && !in.gazer_crater_ok) return refuse("not a catcher voidout (the blast only, until probe 35)");
    if (in.role == 2) {
        if (!in.sandboxed) return refuse("this game plays on its own real save (host_save=0): no crater record");
    } else if (in.role == 1) {
        if (!in.sender_sandboxed) return refuse("the guest plays on her own real save: her craters stay in her world");
        if (!in.world_equal) return refuse("another world (the world prints differ or are unknown)");
        if (!in.records_equal) return refuse("the BT records differ (FIELD compare)");
    } else {
        return refuse("not a two-game role");
    }
    if (!(in.age_ms <= kCraterHoldMs)) return refuse("held 60 s without a chance (expired)");
    if (in.loading) return refuse("a load (pending craters are dropped)");
    if (in.not_playing) return hold(in.not_playing);
    if (in.carried) return hold("Sam rides as a passenger");
    return KillVerdict{KillCall::apply, nullptr};
}
// The record's radius (the sender's, default 107) and flags bit 0 IsDisableCrater (no entity, no WDM) when Sam or a shared vehicle is
// within radius + 10 m (-1: unknown, not near).
inline float CraterRadius(float r) { return detail::FiniteF(r) && r > 0 && r <= 1000 ? r : kCraterRadiusDefault; }
inline std::uint32_t CraterFlags(double sam_dist_m, double vehicle_dist_m, float radius) {
    const double lim = static_cast<double>(CraterRadius(radius)) + kCraterNearExtraM;
    const bool inside = (sam_dist_m >= 0 && sam_dist_m <= lim) || (vehicle_dist_m >= 0 && vehicle_dist_m <= lim);
    return inside ? 1u : 0u;
}

// ---------------------------------------------------------------- 14 CATCH and CS (design 2.8)
// Body 4 + 32 n: u32 sender ms | n x { u32 key (the sender's actor id; a key only) | u8 class + 1 | u8 flags (kCf*) | u8 edge
// (kCe*) | u8 0 | f32 pos[3] (record +0x68) | f32 bar z (record +0xB0 z) | f32 hp % | f32 sub % }.
enum CatchFlag : std::uint8_t { kCfCombat = 1, kCfAnnihilating = 2, kCfBoss = 4, kCfDefeated = 8, kCfTest = 0x10 };
enum CatchEdge : std::uint8_t { kCeNone = 0, kCeSpawn = 1, kCeDefeat = 2, kCeAnnihilate = 3, kCeGone = 4 };
inline const char* CatchEdgeName(std::uint8_t e) {
    switch (e) {
        case kCeSpawn: return "SPAWN";
        case kCeDefeat: return "DEFEAT";
        case kCeAnnihilate: return "ANNIHILATE";
        case kCeGone: return "GONE";
    }
    return "";
}
struct CatchEntry {
    std::uint32_t key = 0;
    std::uint8_t class1 = 0, flags = 0, edge = 0;
    float pos[3] = {};
    float bar_z = 0, hp = -1, sub = -1;
};
struct BtsCatch {
    std::uint32_t sender_ms = 0;
    unsigned n = 0;
    CatchEntry e[kBtsCatchMax];
};
inline bool CatchEntryValid(const CatchEntry& e) {
    return e.class1 <= kCatcherClassMax1 && (e.flags & ~0x1Fu) == 0 && e.edge <= kCeGone && detail::PosOk(e.pos) && detail::FiniteF(e.bar_z) &&
           detail::FiniteF(e.hp) && detail::FiniteF(e.sub);
}
inline std::size_t EncodeCatch(std::uint32_t seq, const BtsCatch& m, std::uint8_t* out, std::size_t cap) {
    if (!out || m.n > kBtsCatchMax || cap < kBtsCatchBase + m.n * kBtsCatchEntry) return 0;
    for (unsigned i = 0; i < m.n; ++i)
        if (!CatchEntryValid(m.e[i])) return 0;
    std::size_t at = detail::Head(out, kBtsCatch, static_cast<std::uint8_t>(m.n), seq);
    detail::P32(out, at, m.sender_ms);
    for (unsigned i = 0; i < m.n; ++i) {
        const CatchEntry& e = m.e[i];
        detail::P32(out, at, e.key);
        detail::P8(out, at, e.class1);
        detail::P8(out, at, e.flags);
        detail::P8(out, at, e.edge);
        detail::P8(out, at, 0);
        for (int k = 0; k < 3; ++k) detail::PF(out, at, e.pos[k]);
        detail::PF(out, at, e.bar_z);
        detail::PF(out, at, e.hp);
        detail::PF(out, at, e.sub);
    }
    return at;
}
inline bool DecodeCatch(const std::uint8_t* d, std::size_t size, BtsCatch& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsCatch) return false;
    std::size_t at = kBtsHeader;
    m.sender_ms = detail::G32(d, at);
    m.n = h.n;
    bool ok = true;
    for (unsigned i = 0; i < m.n; ++i) {
        CatchEntry& e = m.e[i];
        e.key = detail::G32(d, at);
        e.class1 = detail::G8(d, at);
        e.flags = detail::G8(d, at);
        e.edge = detail::G8(d, at);
        ok &= detail::G8(d, at) == 0;
        for (int k = 0; k < 3; ++k) ok &= detail::GF(d, at, e.pos[k]);
        ok &= detail::GF(d, at, e.bar_z);
        ok &= detail::GF(d, at, e.hp);
        ok &= detail::GF(d, at, e.sub);
        ok &= CatchEntryValid(e);
    }
    return ok;
}

// The catcher manager's notify FUN_142a1c0e0(mgr, id, hash, 0): the hashes design 2.8 names [hashes from catcher.md; which one means
// abandon is probe 32's].
constexpr std::uint32_t kCatchNotifySpawn = 0x63DB9ED5, kCatchNotifyDefeat = 0x4C8F5A9D, kCatchNotifyAnnihilate = 0x033DB880;
inline std::uint8_t CatchEdgeOfNotify(std::uint32_t hash) {
    return hash == kCatchNotifySpawn ? kCeSpawn : hash == kCatchNotifyDefeat ? kCeDefeat : hash == kCatchNotifyAnnihilate ? kCeAnnihilate : kCeNone;
}
// CatcherEdges: the record diff (and the notify hook's edges, when the hook is in). A record (below mgr+0x88) is active while its
// +0xF0 is clear. SPAWN: a key becomes active (or the spawn notify); DEFEAT / ANNIHILATE: the notify; GONE: an active key goes
// inactive or missing with no DEFEAT or ANNIHILATE before it (also the no-hook fallback for the defeat). A key known only from a
// notify and never seen in the records is dropped (GONE) after kCatchMissSteps record steps without it.
struct CatcherSeen {
    std::uint32_t key = 0;
    std::uint8_t class1 = 0;
    bool inactive = false;  // record +0xF0
};
struct CatcherEdgeOut {
    std::uint32_t key = 0;
    std::uint8_t class1 = 0, edge = 0;
};
constexpr unsigned kCatchTrackMax = 8, kCatchMissSteps = 4;
struct CatcherEdges {
    struct Track {
        std::uint32_t key = 0;
        std::uint8_t class1 = 0;
        bool ended = false, seen = false;
        unsigned misses = 0;
    };
    Track t[kCatchTrackMax];
    unsigned n = 0;
    Track* Find(std::uint32_t key) {
        for (unsigned i = 0; i < n; ++i)
            if (t[i].key == key) return &t[i];
        return nullptr;
    }
    void Remove(unsigned i) { t[i] = t[--n]; }
    // The hook's edge (0 when it is not one to send, e.g. a second SPAWN or an end after the end).
    std::uint8_t Notify(std::uint32_t key, std::uint32_t hash, std::uint8_t class1) {
        const std::uint8_t e = CatchEdgeOfNotify(hash);
        if (!e) return kCeNone;
        Track* k = Find(key);
        if (e == kCeSpawn) {
            if (k) return kCeNone;
            if (n >= kCatchTrackMax) return kCeNone;
            t[n++] = Track{key, class1, false, false, 0};
            return kCeSpawn;
        }
        if (!k) {
            if (n >= kCatchTrackMax) return e;
            t[n++] = Track{key, class1, true, false, 0};
            return e;
        }
        if (k->ended) return kCeNone;
        k->ended = true;
        return e;
    }
    // One record pass; the edges in *out (at most cap). Returns their count.
    unsigned Step(const CatcherSeen* recs, unsigned nrecs, CatcherEdgeOut* out, unsigned cap) {
        unsigned k = 0;
        auto put = [&](std::uint32_t key, std::uint8_t cls, std::uint8_t e) {
            if (out && k < cap) out[k++] = CatcherEdgeOut{key, cls, e};
        };
        for (unsigned i = 0; i < nrecs && recs; ++i) {
            const CatcherSeen& r = recs[i];
            Track* tr = Find(r.key);
            if (!r.inactive && !tr && n < kCatchTrackMax) {
                t[n++] = Track{r.key, r.class1, false, true, 0};
                put(r.key, r.class1, kCeSpawn);
            } else if (tr) {
                tr->seen = true, tr->misses = 0;
                if (r.class1) tr->class1 = r.class1;
            }
        }
        for (unsigned i = 0; i < n;) {
            Track& tr = t[i];
            const CatcherSeen* r = nullptr;
            for (unsigned j = 0; j < nrecs && recs; ++j)
                if (recs[j].key == tr.key) r = &recs[j];
            bool gone = false;
            if (r) gone = r->inactive;
            else gone = tr.seen || ++tr.misses >= kCatchMissSteps;
            if (!gone) {
                ++i;
                continue;
            }
            if (!tr.ended) put(tr.key, tr.class1, kCeGone);
            Remove(i);
        }
        return k;
    }
};

// ---------------------------------------------------------------- 15 REWARD and RW, the crystals (design 2.8)
// Body 20 B: u8 kind (kRewardCrystals) | u8 class + 1 | u16 0 | u32 count | u32 catcher key | u32 episode | u32 sender ms.
enum RewardKind : std::uint8_t { kRewardCrystals = 1 };
constexpr std::uint32_t kRewardCountMax = 100000;
struct BtsReward {
    std::uint8_t kind = 0, class1 = 0;
    std::uint32_t count = 0, key = 0, episode = 0, sender_ms = 0;
};
inline bool RewardValid(const BtsReward& m) {
    return m.kind == kRewardCrystals && m.class1 <= kCatcherClassMax1 && m.count >= 1 && m.count <= kRewardCountMax;
}
inline std::size_t EncodeReward(std::uint32_t seq, const BtsReward& m, std::uint8_t* out, std::size_t cap) {
    if (!out || cap < kBtsRewardSize || !RewardValid(m)) return 0;
    std::size_t at = detail::Head(out, kBtsReward, 0, seq);
    detail::P8(out, at, m.kind);
    detail::P8(out, at, m.class1);
    detail::P16(out, at, 0);
    detail::P32(out, at, m.count);
    detail::P32(out, at, m.key);
    detail::P32(out, at, m.episode);
    detail::P32(out, at, m.sender_ms);
    return at;
}
inline bool DecodeReward(const std::uint8_t* d, std::size_t size, BtsReward& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsReward) return false;
    std::size_t at = kBtsHeader;
    m.kind = detail::G8(d, at);
    m.class1 = detail::G8(d, at);
    const bool ok = detail::G16(d, at) == 0;
    m.count = detail::G32(d, at);
    m.key = detail::G32(d, at);
    m.episode = detail::G32(d, at);
    m.sender_ms = detail::G32(d, at);
    return ok && RewardValid(m);
}
// RewardGate: give: add `amount` through AddChiralCrystalCount 0x1426E6530 (game thread); hold: later; refuse: never. The host or
// a sandboxed guest only; never twice for the same (catcher key, episode); capped at DSPlayerSystem +0x1B80 (count +0x1B7C).
constexpr double kRewardHoldMs = 60000.0;
struct RewardVerdict {
    KillCall call = KillCall::refuse;
    const char* why = nullptr;
    std::uint32_t amount = 0;
};
struct RewardIn {
    int bt_crystals = 0;
    int role = 0;
    bool sandboxed = false;
    std::uint8_t kind = 0;
    std::uint32_t count = 0;
    bool already = false;              // this (key, episode) was given
    double age_ms = 0;
    bool loading = false;
    const char* not_playing = nullptr;
    std::int64_t have = -1;            // +0x1B7C (-1 unknown)
    std::int64_t max = -1;             // +0x1B80 (-1 unknown)
};
inline RewardVerdict RewardGate(const RewardIn& in) {
    auto refuse = [](const char* w) { return RewardVerdict{KillCall::refuse, w, 0}; };
    auto hold = [](const char* w) { return RewardVerdict{KillCall::hold, w, 0}; };
    if (in.bt_crystals < 1) return refuse("bt_crystals is 0 here");
    if (in.kind != kRewardCrystals) return refuse("not a crystal reward");
    if (in.role == 2) {
        if (!in.sandboxed) return refuse("real save: this game plays on its own real save (host_save=0)");
    } else if (in.role != 1) {
        return refuse("not a two-game role");
    }
    if (in.already) return refuse("already given (the same catcher and episode)");
    if (in.count == 0 || in.count > kRewardCountMax) return refuse("a bad count");
    if (!(in.age_ms <= kRewardHoldMs)) return refuse("held 60 s without a chance (expired)");
    if (in.loading) return refuse("a load (pending rewards are dropped)");
    if (in.not_playing) return hold(in.not_playing);
    if (in.have < 0 || in.max <= 0) return hold("the crystal count or cap is unknown here (+0x1B7C / +0x1B80)");
    if (in.have >= in.max) return refuse("at the cap already");
    const std::int64_t room = in.max - in.have;
    return RewardVerdict{KillCall::apply, nullptr, static_cast<std::uint32_t>(std::min<std::int64_t>(room, in.count))};
}
// The (key, episode) pairs given, so a resent REWARD is not paid twice.
struct RewardRing {
    std::uint64_t k[16] = {};
    unsigned next = 0;
    static std::uint64_t Key(std::uint32_t key, std::uint32_t episode) { return static_cast<std::uint64_t>(key) << 32 | episode; }
    bool Has(std::uint32_t key, std::uint32_t episode) const {
        const std::uint64_t q = Key(key, episode);
        for (unsigned i = 0; i < 16 && i < next; ++i)
            if (k[i] == q) return true;
        return false;
    }
    void Add(std::uint32_t key, std::uint32_t episode) {
        k[next % 16] = Key(key, episode);
        ++next;
    }
};

// ---------------------------------------------------------------- 16 RESPAWN (design 2.9)
// Body 12 B: u32 scene hash (GetUUIDHashOfGazerScene 0x1429A71E0) | u32 field id | u32 sender ms.
struct BtsRespawn {
    std::uint32_t scene = 0, field = 0, sender_ms = 0;
};
inline std::size_t EncodeRespawn(std::uint32_t seq, const BtsRespawn& m, std::uint8_t* out, std::size_t cap) {
    if (!out || cap < kBtsRespawnSize || m.scene == 0) return 0;
    std::size_t at = detail::Head(out, kBtsRespawn, 0, seq);
    detail::P32(out, at, m.scene);
    detail::P32(out, at, m.field);
    detail::P32(out, at, m.sender_ms);
    return at;
}
inline bool DecodeRespawn(const std::uint8_t* d, std::size_t size, BtsRespawn& m) {
    BtsHeader h;
    if (!DecodeBtsHeader(d, size, h) || h.type != kBtsRespawn) return false;
    std::size_t at = kBtsHeader;
    m.scene = detail::G32(d, at);
    m.field = detail::G32(d, at);
    m.sender_ms = detail::G32(d, at);
    return m.scene != 0;
}
// The guest's respawn apply (EnableRespawnGazers on its own scene for the field): a sandboxed guest only; the host only sends.
struct RespawnIn {
    int bt_respawn = 0;
    int role = 0;
    bool sandboxed = false;
    bool scene_here = false;           // the field id matched this game's scene pointer
    double age_ms = 0;
    bool loading = false;
    const char* not_playing = nullptr;
};
inline KillVerdict RespawnApplyGate(const RespawnIn& in) {
    auto refuse = [](const char* w) { return KillVerdict{KillCall::refuse, w}; };
    if (in.bt_respawn < 1) return refuse("bt_respawn is 0 here");
    if (in.role != 2) return refuse("not the guest (the host sends its respawn-on)");
    if (!in.sandboxed) return refuse("this game plays on its own real save (host_save=0): no respawn");
    if (!(in.age_ms <= kKillHoldMs)) return refuse("held 60 s without a chance (expired)");
    if (in.loading) return refuse("a load");
    if (in.not_playing) return KillVerdict{KillCall::hold, in.not_playing};
    if (!in.scene_here) return KillVerdict{KillCall::hold, "the field's scene is not loaded here"};
    return KillVerdict{KillCall::apply, nullptr};
}

}  // namespace st::btrules
