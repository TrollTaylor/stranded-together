// Round 31 stage 3 (bt_hunt): this feature's own pure logic (no game, no Windows), tested by tests/bt_hunt_tests.cpp. The
// shared codecs, caps and the stage-1 gates stay in bt_rules.hpp (included): HUNT's codec (BtsHunt, Encode / DecodeHunt), the
// sender's HuntTracker, the receiver's HuntView, RainSpotGate / RainSpotLife and the spot constants. This file adds what only
// bt_hunt.cpp uses: the bt_sync 8 test plan, the rain-spot slot book, the spot's gazer rule, the send gate and the crowd pick.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "bt_rules.hpp"

namespace st::btrules::hunt {

constexpr int kStage = 3;

// ---------------------------------------------------------------- the send gate (the build spec's common rules)
// HUNT goes from a two-game session (host or guest) with bt_sync 1 or 2, once the partner's hello offered kCapHunt, while this
// game's hunt reads passed their byte checks (a test HUNT needs no read), and not during a load. nullptr: send; else why not.
struct SendIn {
    int role = 0, sync_level = 0;
    bool peer = false;
    std::uint16_t partner_caps = 0;
    bool reads_ok = false, test = false, loading = false;
};
inline const char* SendGate(const SendIn& in) {
    if (in.role != 1 && in.role != 2) return "not a two-game session";
    if (in.sync_level < 1) return "bt_sync is 0 here";
    if (!in.peer) return "no partner hello yet";
    if (!(in.partner_caps & kCapHunt)) return "the partner's build does not take HUNT (no cap 0x400)";
    if (!in.reads_ok && !in.test) return "this game's hunt reads failed their byte checks";
    if (in.loading) return "a load";
    return nullptr;
}

// ---------------------------------------------------------------- the local crowd (design 2.3)
// The hunter area's crowd: comp = hm+0x58[i] - 0x230 whose comp+0x48 is hm+0x140 - 0x20 (probe op 24's "local yes").
constexpr std::uint64_t kCrowdEntOff = 0x230, kAreaRefOff = 0x20;
inline bool CrowdIsLocal(std::uint64_t comp_48, std::uint64_t area) { return area > kAreaRefOff && comp_48 == area - kAreaRefOff; }
// The natural pull (hm+0x00 hands > 0 or the grab byte hm+0x14 != 0): logged only; PULLED is never sent from it this round
// (probe-findings 12: 112 samples, both zero throughout).
inline bool NaturalPullSeen(std::int32_t hands, unsigned grab) { return hands > 0 || grab != 0; }

// ---------------------------------------------------------------- bt_sync 8: the test HUNT (hidden runs only)
// "bt_sync 8 [secs]": UP at once (r 12 m round the sender's Sam, 2 members), PULLED level 1.10 (hands 1, strength 0.60) from 2 s to
// 6 s (TROUBLE says PULLED then: PulledNow()), CLEAR after secs (default 20, 8..120). "bt_sync 8 -1 <0|1>": this game's rain spots
// follow a real save's gate (the 3 s life, the r + 110 m hold) or not: a rehearsal for a hidden run, whose guest is always
// sandboxed (it only makes the gate stricter).
constexpr double kTestDefaultS = 20.0, kTestMinS = 8.0, kTestMaxS = 120.0;
constexpr double kTestPullFromMs = 2000.0, kTestPullToMs = 6000.0;
constexpr float kTestRadiusM = 12.0f, kTestLevel = 1.10f, kTestStrength = 0.60f;
constexpr int kTestMembers = 2, kTestHands = 1;
enum class Op8 : std::uint8_t { test, rehearse, bad };
struct Op8Args {
    Op8 kind = Op8::bad;
    double secs = kTestDefaultS;
    bool on = false;
};
inline Op8Args ParseOp8(double v1, double v2) {
    Op8Args a;
    if (!std::isfinite(v1) || !std::isfinite(v2)) return a;
    if (v1 < 0) {
        if (v1 != -1.0 || (v2 != 0.0 && v2 != 1.0)) return a;
        a.kind = Op8::rehearse, a.on = v2 != 0.0;
        return a;
    }
    a.kind = Op8::test;
    a.secs = v1 == 0.0 ? kTestDefaultS : std::clamp(v1, kTestMinS, kTestMaxS);
    return a;
}
struct TestFrame {
    bool up = false, pulled = false;
    float level = 0, strength = 0;
    int hands = 0;
};
// The test's state t ms after its start (dur_ms its length): up until dur_ms, pulled inside [2 s, 6 s).
inline TestFrame TestHuntAt(double t_ms, double dur_ms) {
    TestFrame f;
    if (!(t_ms >= 0) || !(t_ms < dur_ms)) return f;
    f.up = true;
    if (t_ms >= kTestPullFromMs && t_ms < kTestPullToMs) f.pulled = true, f.level = kTestLevel, f.strength = kTestStrength, f.hands = kTestHands;
    return f;
}

// ---------------------------------------------------------------- RS: the spot's gazer rule and distance
// RainSpotGate's "a gazer streamed here within 150 m" is read as an ARMED one (flags 0x80000: this game already rains there,
// probe-findings fact 1: ARMED comes with rain; under sun the same gazers read 0x10004), and only before the slot's first spot:
// the spot's own rain arms the gazers under it (that is leg D's pass line), so testing it again would end the spot it made.
constexpr std::uint32_t kGzArmed = 0x80000u;
inline bool SpotGazerBlock(bool made_once, double nearest_armed_m) {
    return !made_once && nearest_armed_m >= 0 && nearest_armed_m <= kSpotGazerNearM;
}
// The spot is a column (its list entry keeps a cell, no height): distances to it are horizontal (also never more than the 3D one,
// so a real save's hold distance errs on the safe side).
inline double Dist2D(const double* a, const double* b) {
    const double dx = a[0] - b[0], dy = a[1] - b[1];
    return std::sqrt(dx * dx + dy * dy);
}

// ---------------------------------------------------------------- RS: the slot book (8 static owner keys)
// A slot is live while its user wants it (SpotWant at least every kWantLifeMs, no SpotDrop / CLEAR), draining after the last
// refresh until the module reads the spot list without its owner (or kGoneWaitMs passed), then free again. Only a free slot is
// handed out, so an owner key never names two spots at once.
enum class SlotState : std::uint8_t { free, live, draining };
constexpr double kRefreshMs = static_cast<double>(kSpotRefreshS) * 1000.0;  // 2 s
constexpr double kWantLifeMs = 3000.0;    // a user quiet this long has ended its source
constexpr double kGoneWaitMs = 15000.0;   // the 10 s life + the poll and a hitch
constexpr double kGonePollMs = 1000.0;
constexpr double kCheckLateMs = 10000.0;  // leg D's "+10 s" raw read
constexpr std::uint8_t kUserNone = 0xFF;
struct SpotSlot {
    SlotState st = SlotState::free;
    std::uint8_t user = kUserNone;
    double pos[3] = {0, 0, 0};
    float radius = 0;
    double want_ms = 0, eval_ms = -1e18, made_ms = -1e18, first_ms = 0, drop_ms = 0, poll_ms = -1e18;
    bool made_once = false, ended = false, held = false;
    const char* end_why = nullptr;   // why the source ended (logged as "rain spot off")
    const char* hold_why = nullptr;  // the hold logged last (a change is logged once)
    int refreshes = 0, checks = 0;
};
struct SpotBook {
    SpotSlot s[kSpotSlots];
    int Find(std::uint8_t user) const {
        for (unsigned i = 0; i < kSpotSlots; ++i)
            if (s[i].st == SlotState::live && s[i].user == user) return static_cast<int>(i);
        return -1;
    }
    unsigned Count(SlotState st) const {
        unsigned n = 0;
        for (const SpotSlot& x : s) n += x.st == st ? 1u : 0u;
        return n;
    }
    // The user's live slot (updated) or a free one (taken); -1 none free. A moved centre keeps the slot (the game's spot moves with
    // the refresh: FUN_142E15DF0 updates the entry the owner names).
    int Want(std::uint8_t user, const double* pos, float radius, double now_ms) {
        int i = Find(user);
        if (i < 0) {
            for (unsigned k = 0; k < kSpotSlots && i < 0; ++k)
                if (s[k].st == SlotState::free) i = static_cast<int>(k);
            if (i < 0) return -1;
            s[i] = SpotSlot{};
            s[i].st = SlotState::live, s[i].user = user;
        }
        SpotSlot& x = s[i];
        for (int k = 0; k < 3; ++k) x.pos[k] = pos[k];
        x.radius = radius, x.want_ms = now_ms, x.ended = false, x.end_why = nullptr;
        return i;
    }
    // The user's source ended: its slot drops at its next evaluation (no further refresh).
    void End(std::uint8_t user, const char* why) {
        const int i = Find(user);
        if (i >= 0 && !s[i].ended) s[i].ended = true, s[i].end_why = why;
    }
    // Live, and its source current (wanted within kWantLifeMs and not ended); sets the end reason when it went quiet.
    bool SourceLive(unsigned i, double now_ms) {
        SpotSlot& x = s[i];
        if (x.st != SlotState::live || x.ended) return false;
        if (now_ms - x.want_ms > kWantLifeMs) {
            x.ended = true, x.end_why = "its source went quiet (no update for 3 s)";
            return false;
        }
        return true;
    }
    // Time to run the gate again: every 2 s (the refresh), or at once when the source ended.
    bool Due(unsigned i, double now_ms) const {
        const SpotSlot& x = s[i];
        return x.st == SlotState::live && (x.ended || now_ms - x.eval_ms >= kRefreshMs);
    }
    // After a drop: a slot that made a spot drains (its owner may still be listed), one that never did is free at once.
    void Release(unsigned i, double now_ms) {
        SpotSlot& x = s[i];
        if (x.made_once) x.st = SlotState::draining, x.drop_ms = now_ms, x.poll_ms = -1e18;
        else x = SpotSlot{};
    }
    void Free(unsigned i) { s[i] = SpotSlot{}; }
    void Clear() {
        for (SpotSlot& x : s) x = SpotSlot{};
    }
    // The link, the partner or the relay gone: every live slot let go at once (no refresh again). One that made a spot drains, so it
    // stays watched (the spot list is only read) until the game drops it and its owner key is not handed out while still listed (gate
    // run 20261128-011308: the old Clear() freed the watch at a "partner gone" and the "rain spot gone" line never came). One that
    // never made a spot is free at once. Returns the live slots let go.
    unsigned LetGo(double now_ms) {
        unsigned n = 0;
        for (unsigned i = 0; i < kSpotSlots; ++i)
            if (s[i].st == SlotState::live) Release(i, now_ms), ++n;
        return n;
    }
    // Another slot of ours that made a spot covering pos (live, or draining: that spot lives on up to its life): the rain there is
    // ours, so the gazer rule must not read the gazers our own spot armed as "this game rains there itself" (a slot booked again
    // after a let-go would otherwise be held by its predecessor's rain).
    bool OwnSpotCovers(unsigned except, const double* pos) const {
        for (unsigned k = 0; k < kSpotSlots; ++k) {
            const SpotSlot& x = s[k];
            if (k == except || x.st == SlotState::free || !x.made_once) continue;
            if (Dist2D(pos, x.pos) <= static_cast<double>(x.radius)) return true;
        }
        return false;
    }
};
enum class Drain : std::uint8_t { wait, gone, stale };
inline Drain DrainVerdict(double now_ms, double drop_ms, bool listed) {
    if (!listed) return Drain::gone;
    return now_ms - drop_ms > kGoneWaitMs ? Drain::stale : Drain::wait;
}

// ---------------------------------------------------------------- the markers (bt_markers.cpp btmarks::Tar's radius)
// The tar ring's radius from HUNT (design 2.3: the real radius, not the fixed 6 m), clamped to 3..30 m so it stays a ring round the
// partner; a PULLED mark tightens toward 40 % of it as the level goes to 1.2 (the drag-under).
constexpr float kRingMinM = 3.0f, kRingMaxM = 30.0f, kRingDefaultM = 6.0f;
inline float TarRingRadius(float hunt_radius, bool pulled, float level) {
    float r = detail::FiniteF(hunt_radius) && hunt_radius > 0 ? hunt_radius : kRingDefaultM;
    r = std::clamp(r, kRingMinM, kRingMaxM);
    if (pulled && detail::FiniteF(level)) {
        const float f = std::clamp(level / kPullUnderLevel, 0.0f, 1.0f);
        r = std::max(kRingMinM * 0.5f, r * (1.0f - 0.6f * f));
    }
    return r;
}

}  // namespace st::btrules::hunt
