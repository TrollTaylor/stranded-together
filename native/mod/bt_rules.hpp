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
// Who called the voidout (the detour's return address, the four E8 call sites in ds.exe DC; round 28 lane B1 scan).
enum VoidSource : std::uint8_t { kVoidSrcUnknown = 0, kVoidSrcB3B0 = 1, kVoidSrcCatch = 2, kVoidSrcPending = 3, kVoidSrcCatcher = 4, kVoidSrcTest = 0xFF };
inline const char* VoidSourceName(std::uint8_t s) {
    switch (s) {
        case kVoidSrcB3B0: return "FUN_14299B3B0";
        case kVoidSrcCatch: return "gazer catch";
        case kVoidSrcPending: return "manager (pending area)";
        case kVoidSrcCatcher: return "catcher";
        case kVoidSrcTest: return "test";
    }
    return "unknown caller";
}
// The partner's screen gets a notice for a voidout of the partner's player; the manager's "pending area" path (old craters
// replayed by the manager update, [I]) and unknown callers are logged only.
inline bool VoidToast(std::uint8_t source) { return source == kVoidSrcCatch || source == kVoidSrcCatcher || source == kVoidSrcB3B0 || source == kVoidSrcTest; }

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
// A decoder refuses another magic, version, type, size, n, a non-zero pad or a non-finite float.
constexpr std::size_t kBtsHeader = 12, kBtsHelloSize = 20, kBtsFieldBase = 36, kBtsFieldEntry = 16, kBtsResetSize = 20,
                      kBtsNoticedSize = 52, kBtsVoidSize = 44;
constexpr unsigned kBtsFieldMax = 6;
constexpr std::size_t kBtsMaxSize = kBtsFieldBase + kBtsFieldMax * kBtsFieldEntry;  // 132 B
enum BtsType : std::uint8_t { kBtsHello = 1, kBtsField = 2, kBtsReset = 3, kBtsNoticed = 4, kBtsVoid = 5 };
enum BtsCap : std::uint16_t { kCapField = 1, kCapNoticed = 2, kCapVoid = 4, kCapResetApply = 8 };
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

}  // namespace st::btrules
