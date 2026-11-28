// Round 31 stage 4 (bt_tar): this feature's own pure logic (no game, no Windows), tested by tests/bt_tar_tests.cpp. The
// shared codecs, caps and the stage-1 gates stay in bt_rules.hpp (included); new pure helpers of this feature go here, so the
// seven builders never edit one file at once.
//
// TF (design 2.4, build spec stage 4, probe-findings 13): while the partner's HUNT is UP, one module-owned 0x20-byte record
// {f64 pos[3]; f32 radius +0x18; f32 height +0x1C} sits in the hunter manager's list 0 (count i32 hm+0xC0, cap 10; ptr[10]
// hm+0xD0; SRW lock hm+0x130, exclusive). The player locomotion FUN_1428097C0 reads it (the reader FUN_1429BE1F0) and Sam's
// smoothed tar level [Sam+0x54C0]+0x844 rises inside it (0.84 in 3.7 s, 1.0 inside; probe 26). List 1 (+0x60 / +0x70) feeds
// the gazers' in-tar fact and is never used. Nothing pulls: no hands, no drag (those come only from hm+0x50).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "bt_rules.hpp"

namespace st::btrules::tar {

constexpr int kStage = 4;

// THE ONE-LINE GATE CHANGE (build spec stage 4): a guest on her own real save (host_save=0) gets no tar record until the gate's
// record-out lines read `cargo d 0.000` with carried pieces (tar-feel-echo, two-games-bt-trouble-tar); then flip this to true.
// The deciding reads are the clean ones (Sam still while the record is in: tar-feel-echo leg B, the two-game guest) with the
// no-record control (tar-feel-echo leg C, the realsave leg) at 0 too. Run 20261128-020913 read cargo d -632.983 (22 pieces)
// across an episode with a dev_goto in it (not clean, not 0): false stands.
constexpr bool kRealSaveOk = false;

constexpr float kHeightM = 2.0f;                           // the crowd's +0x114 default (the record's +0x1C)
constexpr float kRadiusMinM = 1.0f, kRadiusMaxM = 100.0f;  // a sanity clamp of HUNT's radius (the crowds seen: 6..50 m)
constexpr float kInsideIn = 0.5f, kInsideMin = 1.0f;       // the locomotion's inside test: 2D d < max(r - 0.5, 1.0) (0x14280AEC4)
constexpr float kInLevel = 0.5f;                           // "Sam in it" is logged once per entry when +0x844 reaches this
constexpr double kReaddMs = 1000.0;    // after an out that does not end the episode (a menu edge, the game clearing the list)
constexpr double kMoveM = 0.5;         // the record is refreshed when HUNT's centre moved this far ...
constexpr float kRadiusStepM = 0.25f;  // ... or its radius changed this much
constexpr double kSampleMs = 100.0;    // +0x844 and the inside test, while the record is in
constexpr double kCheckMs = 250.0;     // the list read back (is our pointer still there?)
constexpr double kLineMs = 2000.0;     // the periodic "+t s" line while the record is in
constexpr double kNeutralZ = -100000.0;  // the fallback when a removal cannot run: the record's z far below any ground

// ---- bt_sync 12, the test HUNT (hidden runs): a HUNT made here, encoded and decoded as the partner's would be (the echo).
constexpr double kTestAheadM = 8.0;
constexpr float kTestRadiusM = 6.0f;
constexpr double kTestMs = 20000.0, kTestSendMs = 250.0;
constexpr std::uint16_t kTestEpisode0 = 0xF000;  // the test episodes count up from here (a partner's start at 1)
enum class TestMode : std::uint8_t { bad, ahead, at, real_on, real_off, stop };
// v1 v2: 0 0 a test HUNT 8 m ahead of Sam; x y (x != 0) one centred at (x, y, Sam's z); 0 1 / 0 2 this game counts as a
// real-save guest for the tar gate (on / off: the refusal path on a sandboxed save); 0 3 the running test ends now (its CLEAR).
inline TestMode TestArgs(double a, double b) {
    if (!std::isfinite(a) || !std::isfinite(b)) return TestMode::bad;
    if (a == 0) {
        if (b == 0) return TestMode::ahead;
        if (b == 1) return TestMode::real_on;
        if (b == 2) return TestMode::real_off;
        if (b == 3) return TestMode::stop;
        return TestMode::bad;
    }
    if (std::fabs(a) > 1e6 || std::fabs(b) > 1e6) return TestMode::bad;
    return TestMode::at;
}

// ---- the gate: TarFeelGate's order (bt_rules.hpp) with this stage's real-save wording and decision (kRealSaveOk), and the
// source rule (a two-game session at bt_sync 2 with the partner's hello and its cap 0x400, or the bt_sync 12 test) in front.
enum class Why : std::uint8_t {
    none, emergency, off, no_source, cleared, quiet, load, carried, real_save, hm_not_ready, too_far, not_playing, list_full,
    link_lost, partner_gone, gone_from_list, hm_changed
};
inline const char* WhyText(Why w) {
    switch (w) {
        case Why::emergency: return "EmergencyStop";
        case Why::off: return "bt_tar is 0 here";
        case Why::no_source: return "no HUNT source (bt_sync 2 with the partner's hello and cap 0x400, or bt_sync 12)";
        case Why::cleared: return "the partner's hunt ended (CLEAR)";
        case Why::quiet: return "no HUNT for 3 s";
        case Why::load: return "a load";
        case Why::carried: return "Sam rides as a passenger";
        case Why::real_save: return "this game plays on its own real save (host_save=0)";
        case Why::hm_not_ready: return "the hunter manager is not ready (+0x138)";
        case Why::too_far: return "the partner's tar is more than 300 m from Sam";
        case Why::not_playing: return "not played now";
        case Why::list_full: return "list full (10): not added";
        case Why::link_lost: return "the link dropped";
        case Why::partner_gone: return "the partner is gone";
        case Why::gone_from_list: return "the game took it out of the list";
        case Why::hm_changed: return "the hunter manager changed";
        default: return "";
    }
}
// An out for this reason ends the episode's record for good (no re-add hold-off needed: a new HUNT starts a new one).
inline bool EndsEpisode(Why w) { return w == Why::emergency || w == Why::off || w == Why::cleared || w == Why::quiet; }
struct FeelVerdict {
    TarCall call = TarCall::out;
    Why why = Why::none;
    const char* not_playing = nullptr;  // Why::not_playing: the game's reason (a menu, a cutscene, Sam asleep)
};
inline FeelVerdict FeelGate(const TarFeelIn& in, bool source_ok) {
    auto out = [](Why w) { return FeelVerdict{TarCall::out, w, nullptr}; };
    if (in.emergency) return out(Why::emergency);
    if (in.bt_tar < 1) return out(Why::off);
    if (!source_ok) return out(Why::no_source);
    if (!in.hunt_up) return out(Why::cleared);
    if (in.since_hunt_ms > kTarQuietMs) return out(Why::quiet);
    if (in.loading) return out(Why::load);
    if (in.carried) return out(Why::carried);
    if (in.real_save && !in.real_save_ok) return out(Why::real_save);
    if (!in.hm_ready) return out(Why::hm_not_ready);
    if (!(in.dist_m >= 0) || in.dist_m > kTarNearM) return out(Why::too_far);
    if (in.not_playing) return FeelVerdict{TarCall::hold, Why::not_playing, in.not_playing};
    if (!in.ours_in && in.count >= kTarListMax) return FeelVerdict{TarCall::hold, Why::list_full, nullptr};
    return FeelVerdict{TarCall::in, Why::none, nullptr};
}

// ---- the record and the locomotion's inside test
struct Inside {
    bool in = false;
    double d2 = -1, rin = 0, top = 0;
};
inline Inside InsideTest(const double* me, const double* pos, float radius, float height) {
    Inside o;
    if (!me || !pos) return o;
    const double dx = me[0] - pos[0], dy = me[1] - pos[1];
    o.d2 = std::sqrt(dx * dx + dy * dy);
    o.rin = std::fmax(static_cast<double>(radius) - kInsideIn, static_cast<double>(kInsideMin));
    o.top = pos[2] + static_cast<double>(height);
    o.in = o.d2 < o.rin && me[2] < o.top;
    return o;
}
// The record's centre and radius from a HUNT (f32 to f64; the radius clamped to 1..100 m). False: no usable centre.
inline bool RecordFrom(const BtsHunt& m, double (&pos)[3], float& radius) {
    if (!detail::PosOk(m.center) || !detail::FiniteF(m.radius)) return false;
    for (int i = 0; i < 3; ++i) pos[i] = static_cast<double>(m.center[i]);
    if (!PosSane(pos)) return false;
    radius = m.radius < kRadiusMinM ? kRadiusMinM : m.radius > kRadiusMaxM ? kRadiusMaxM : m.radius;
    return true;
}
inline bool NeedsRefresh(const double* cur, float cur_r, const double* want, float want_r) {
    return Dist3(cur, want) >= kMoveM || std::fabs(cur_r - want_r) >= kRadiusStepM;
}

// ---- the list edits (the module runs these on the game's own hm+0xC0 / +0xD0 inside its SEH, under hm+0x130; probe 26's code)
// 1 added, 0 not (full, or a bad count).
inline int ListAdd(std::int32_t* n, std::uintptr_t* a, std::uintptr_t rec) {
    if (*n < 0 || *n >= kTarListMax) return 0;
    a[(*n)++] = rec;
    return 1;
}
// 1 removed (the later entries moved down, the freed tail slot zeroed), 0 not found.
inline int ListRemove(std::int32_t* n, std::uintptr_t* a, std::uintptr_t rec) {
    for (std::int32_t i = 0; i < *n && i < kTarListMax; ++i)
        if (a[i] == rec) {
            for (std::int32_t j = i; j + 1 < *n && j + 1 < kTarListMax; ++j) a[j] = a[j + 1];
            a[--*n] = 0;
            return 1;
        }
    return 0;
}
// The read-back (a copy of the list): is rec in its first n entries? (-1: a bad count.)
inline int ListHas(std::int32_t n, const std::uintptr_t* a, std::uintptr_t rec) {
    if (n < 0 || n > kTarListMax) return -1;
    for (std::int32_t i = 0; i < n; ++i)
        if (a[i] == rec) return 1;
    return 0;
}

// ---- the cargo read (the gate's `cargo d`): the pieces Sam carries (the baggage manager's DSBaggage2 +0x34 life), before and
// after. A carried piece (shot_sync.cpp's rule for `shots 6`): in use with a type the place step takes, no entity of its own
// (+0xD8 == -1), on a holder (+0x90), within 2.5 m of Sam.
constexpr int kCargoMax = 48;
constexpr double kCargoNearM = 2.5;
inline bool CarriedPiece(std::uint8_t type, std::int32_t entity_index, std::uint64_t holder, double dist_m) {
    if (!type) return false;
    if (!(static_cast<std::uint8_t>(type - 1) < 8 || type > 10)) return false;
    return entity_index == -1 && holder != 0 && dist_m <= kCargoNearM;
}
struct CargoSnap {
    bool ok = false;
    int n = 0, more = 0;  // more: carried pieces beyond kCargoMax (not compared)
    std::uint32_t idx[kCargoMax] = {};
    float life[kCargoMax] = {};
    void Add(std::uint32_t i, float l) {
        if (!std::isfinite(l)) return;
        if (n >= kCargoMax) {
            ++more;
            return;
        }
        idx[n] = i, life[n] = l, ++n;
    }
};
struct CargoDelta {
    bool ok = false;
    int pieces = 0, lost = 0;  // matched in both reads; carried before, not after
    double d = 0;              // the sum of (after - before) over the matched pieces (exactly 0 below 1e-6)
    // gate run 20261128-020913 (tar-feel-echo: cargo d -632.983 over 22 pieces): which pieces wore and by how much, so a
    // nonzero read says whether one piece took a knock or all of them wore together
    int worn = 0;                       // matched pieces whose life fell (by more than kCargoWornEps)
    std::uint32_t worst_idx = 0;        // the piece that fell most (when worn > 0) ...
    float worst_before = 0, worst_after = 0;  // ... its life in the two reads
};
constexpr double kCargoWornEps = 1e-4;
inline CargoDelta CargoDiff(const CargoSnap& a, const CargoSnap& b) {
    CargoDelta o;
    if (!a.ok || !b.ok) return o;
    o.ok = true;
    for (int i = 0; i < a.n; ++i) {
        int k = -1;
        for (int j = 0; j < b.n && k < 0; ++j)
            if (b.idx[j] == a.idx[i]) k = j;
        if (k < 0) {
            ++o.lost;
            continue;
        }
        ++o.pieces;
        const double di = static_cast<double>(b.life[k]) - static_cast<double>(a.life[i]);
        o.d += di;
        if (di < -kCargoWornEps) {
            if (!o.worn || di < static_cast<double>(o.worst_after) - static_cast<double>(o.worst_before))
                o.worst_idx = a.idx[i], o.worst_before = a.life[i], o.worst_after = b.life[k];
            ++o.worn;
        }
    }
    if (std::fabs(o.d) < 1e-6) o.d = 0;
    return o;
}
// "cargo d 0.000 (3 pieces)", "cargo d -0.120 (2 pieces, 1 no longer carried)", "cargo d n/a (why)".
inline void CargoText(const CargoDelta& c, const char* na_why, char* out, std::size_t cap) {
    if (!out || !cap) return;
    if (!c.ok) std::snprintf(out, cap, "cargo d n/a (%s)", na_why ? na_why : "not read");
    else if (c.lost) std::snprintf(out, cap, "cargo d %.3f (%d pieces, %d no longer carried)", c.d, c.pieces, c.lost);
    else std::snprintf(out, cap, "cargo d %.3f (%d pieces)", c.d, c.pieces);
}
// After CargoText's ")": "; worn 3 of 22, the most #1234 900.0 -> 850.2" when a piece's life fell, else "" (the gate regexes
// end at CargoText's ")", so this tail never changes what they match).
inline void CargoWornText(const CargoDelta& c, char* out, std::size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (c.ok && c.worn)
        std::snprintf(out, cap, "; worn %d of %d, the most #%u %.1f -> %.1f", c.worn, c.pieces, c.worst_idx, c.worst_before, c.worst_after);
}

// ---- the log rate limit: at most kLogBurst lines per channel per kLogWindowMs; the next line logged says how many were dropped.
constexpr double kLogWindowMs = 10000.0;
constexpr int kLogBurst = 6;
struct LineLimit {
    double start = -1e18;
    int n = 0, dropped = 0;
    // True: log now (*dropped_out: the lines dropped before this one, 0 normally).
    bool Allow(double now_ms, int* dropped_out) {
        if (now_ms - start >= kLogWindowMs || now_ms < start) start = now_ms, n = 0;
        if (n >= kLogBurst) {
            ++dropped;
            return false;
        }
        ++n;
        if (dropped_out) *dropped_out = dropped;
        dropped = 0;
        return true;
    }
};

}  // namespace st::btrules::tar
