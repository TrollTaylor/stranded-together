// Round 31 stage 3 (bt_hunt): the pure part's checks (native/mod/bt_hunt_rules.hpp; the shared codecs in bt_rules.hpp). No game.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <algorithm>

#include "bt_hunt_rules.hpp"

namespace {
int g_checks = 0, g_fail = 0;
}  // namespace
#define CHECK(c)                                                     \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static_assert(st::btrules::hunt::kStage == 3, "the stage");

namespace R = st::btrules;
namespace H = st::btrules::hunt;

// The shared BTS1 header round trip.
void TestHello() {
    R::BtsHello h;
    h.level = 2, h.role = 1, h.caps = R::kCapField | R::kCapHunt;
    std::uint8_t b[R::kBtsHelloSize] = {};
    const std::size_t n = R::EncodeHello(1, h, b, sizeof(b));
    R::BtsHello o;
    CHECK(n == R::kBtsHelloSize && R::DecodeHello(b, n, o) && o.caps == h.caps && o.level == 2 && o.role == 1);
}

void TestSendGate() {
    H::SendIn in;
    in.role = 1, in.sync_level = 2, in.peer = true, in.partner_caps = R::kCapHunt, in.reads_ok = true;
    CHECK(H::SendGate(in) == nullptr);
    H::SendIn x = in;
    x.role = 0;
    CHECK(H::SendGate(x) != nullptr);
    x = in, x.sync_level = 0;
    CHECK(H::SendGate(x) != nullptr);
    x = in, x.sync_level = 1;  // bt_sync 1 sends
    CHECK(H::SendGate(x) == nullptr);
    x = in, x.peer = false;
    CHECK(H::SendGate(x) != nullptr);
    x = in, x.partner_caps = R::kCapTrouble;
    CHECK(H::SendGate(x) != nullptr && std::strstr(H::SendGate(x), "0x400"));
    x = in, x.reads_ok = false;
    CHECK(H::SendGate(x) != nullptr);
    x.test = true;  // a test HUNT reads nothing
    CHECK(H::SendGate(x) == nullptr);
    x = in, x.loading = true;
    CHECK(H::SendGate(x) != nullptr);
    x = in, x.role = 2;
    CHECK(H::SendGate(x) == nullptr);
}

void TestCrowdAndPull() {
    CHECK(H::CrowdIsLocal(0x1000, 0x1020));
    CHECK(!H::CrowdIsLocal(0x1020, 0x1020));
    CHECK(!H::CrowdIsLocal(0, 0));      // no area: never local
    CHECK(!H::CrowdIsLocal(0, 0x20));   // area 0x20 would make 0 "local"
    CHECK(!H::NaturalPullSeen(0, 0));
    CHECK(H::NaturalPullSeen(1, 0) && H::NaturalPullSeen(0, 1) && !H::NaturalPullSeen(-1, 0));
}

void TestOp8() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    H::Op8Args a = H::ParseOp8(0, 0);
    CHECK(a.kind == H::Op8::test && a.secs == H::kTestDefaultS);
    a = H::ParseOp8(14, 0);
    CHECK(a.kind == H::Op8::test && a.secs == 14);
    a = H::ParseOp8(3, 0);
    CHECK(a.kind == H::Op8::test && a.secs == H::kTestMinS);
    a = H::ParseOp8(500, 0);
    CHECK(a.kind == H::Op8::test && a.secs == H::kTestMaxS);
    a = H::ParseOp8(-1, 1);
    CHECK(a.kind == H::Op8::rehearse && a.on);
    a = H::ParseOp8(-1, 0);
    CHECK(a.kind == H::Op8::rehearse && !a.on);
    CHECK(H::ParseOp8(-1, 2).kind == H::Op8::bad);
    CHECK(H::ParseOp8(-2, 1).kind == H::Op8::bad);
    CHECK(H::ParseOp8(nan, 0).kind == H::Op8::bad);
    CHECK(H::ParseOp8(10, nan).kind == H::Op8::bad);
}

void TestPlan() {
    const double dur = 14000;
    CHECK(!H::TestHuntAt(-1, dur).up);
    H::TestFrame f = H::TestHuntAt(0, dur);
    CHECK(f.up && !f.pulled && f.level == 0 && f.hands == 0);
    f = H::TestHuntAt(1999, dur);
    CHECK(f.up && !f.pulled);
    f = H::TestHuntAt(2000, dur);
    CHECK(f.up && f.pulled && f.level == H::kTestLevel && f.hands == H::kTestHands && f.strength == H::kTestStrength);
    f = H::TestHuntAt(5999, dur);
    CHECK(f.pulled);
    f = H::TestHuntAt(6000, dur);
    CHECK(f.up && !f.pulled);
    CHECK(!H::TestHuntAt(dur, dur).up);
    CHECK(!H::TestHuntAt(4000, 3000).pulled);  // a test shorter than the pull window: nothing past its end
    // the pulled window covers at least three 1 Hz TROUBLE censuses whatever their phase
    int censuses = 0;
    for (double t = 0.37 * 1000; t < dur; t += 1000) censuses += H::TestHuntAt(t, dur).pulled ? 1 : 0;
    CHECK(censuses >= 3);
}

// The test plan through the sender's HuntTracker (10 Hz reads) and the receiver's HuntView: one UP line, one PULLED line (level
// 1.10, deep), one CLEAR line with the episode's length; the wire round trip of each message.
void TestPlanThroughTracker() {
    R::HuntTracker tr;
    R::HuntView view;
    const double dur = 14000, t0 = 50000;
    int ups = 0, pulls = 0, clears = 0, sent = 0, edges = 0;
    std::uint8_t last_flags = 0xFF;
    double clear_secs = -1;
    for (double t = 0; t <= dur + 1000; t += 50) {  // bt_hunt.cpp reads at 20 Hz
        const H::TestFrame f = H::TestHuntAt(t, dur);
        R::HuntIn in;
        if (f.up) {
            in.up = true, in.test = true, in.pulled = f.pulled, in.level = f.level, in.strength = f.strength, in.hands = f.hands;
            in.members = H::kTestMembers, in.radius = H::kTestRadiusM;
            in.center[0] = -1046, in.center[1] = 36, in.center[2] = 120;
        }
        R::BtsHunt m;
        const std::uint8_t k = tr.Step(in, t0 + t, &m);
        if (!k) continue;
        ++sent;
        std::uint8_t b[R::kBtsHuntSize];
        const std::size_t n = R::EncodeHunt(static_cast<std::uint32_t>(sent), m, b, sizeof(b));
        R::BtsHunt d;
        CHECK(n == R::kBtsHuntSize && R::DecodeHunt(b, n, d));
        CHECK((d.flags & R::kHfTest) != 0 && d.episode == 1);
        if (d.kind == R::kHuntUp) {
            CHECK(d.radius == H::kTestRadiusM && d.members == H::kTestMembers);
            if (d.flags != last_flags) ++edges, last_flags = d.flags;
        }
        const R::HuntView::Out o = view.Take(d, t0 + t);
        ups += o.up ? 1 : 0;
        if (o.pulled) {
            ++pulls;
            CHECK(std::fabs(d.level - 1.10f) < 1e-6f && (d.flags & R::kHfDeep) && d.hands == 1);
        }
        if (o.clear) ++clears, clear_secs = o.secs;
    }
    CHECK(ups == 1 && pulls == 1 && clears == 1);
    CHECK(edges == 3);                                   // UP, PULLED (deep), un-pulled
    CHECK(std::fabs(clear_secs - dur / 1000.0) < 0.15);  // the CLEAR at 14 s
    CHECK(sent >= 50 && sent <= 62);                     // about 4 Hz for 14 s, plus the edges and the CLEAR
    CHECK(!view.up && !tr.active);
}

void TestGazerRuleAndDist() {
    CHECK(!H::SpotGazerBlock(false, -1));  // none / unknown
    CHECK(H::SpotGazerBlock(false, 0) && H::SpotGazerBlock(false, 150));
    CHECK(!H::SpotGazerBlock(false, 150.5));
    CHECK(!H::SpotGazerBlock(true, 10));  // after the first spot: its own rain armed them
    const double a[3] = {0, 0, 0}, b[3] = {3, 4, 100};
    CHECK(H::Dist2D(a, b) == 5.0);
    CHECK(H::Dist2D(a, b) <= R::Dist3(a, b));
}

void TestBook() {
    H::SpotBook bk;
    const double p[3] = {-1046, 36, 120}, q[3] = {-900, 0, 100};
    const std::uint8_t hunt = 0, catcher = 1;
    CHECK(bk.Want(hunt, p, R::kSpotHuntRadiusM, 1000) == 0);
    CHECK(bk.Want(hunt, q, R::kSpotHuntRadiusM, 1250) == 0);  // the same user keeps its slot, the centre moves
    CHECK(bk.s[0].pos[0] == -900 && bk.s[0].want_ms == 1250);
    CHECK(bk.Want(catcher, p, 130, 1300) == 1);
    CHECK(bk.Count(H::SlotState::live) == 2);
    CHECK(bk.Due(0, 1300));  // never evaluated
    bk.s[0].eval_ms = 1300;
    CHECK(!bk.Due(0, 3299) && bk.Due(0, 3300));
    CHECK(bk.SourceLive(0, 4250));
    CHECK(!bk.SourceLive(0, 4251) && bk.s[0].ended && bk.s[0].end_why && std::strstr(bk.s[0].end_why, "quiet"));
    CHECK(bk.Due(0, 4251));  // an ended source is due at once
    bk.Want(hunt, p, R::kSpotHuntRadiusM, 4300);  // wanted again before its drop: live again
    CHECK(bk.SourceLive(0, 4300) && !bk.s[0].ended);
    bk.End(hunt, "the hunt ended");
    CHECK(!bk.SourceLive(0, 4400) && std::strcmp(bk.s[0].end_why, "the hunt ended") == 0);
    bk.End(hunt, "other");  // the first reason stays
    CHECK(std::strcmp(bk.s[0].end_why, "the hunt ended") == 0);
    // release: a slot that never made a spot is free at once, one that did drains
    bk.Release(0, 4400);
    CHECK(bk.s[0].st == H::SlotState::free);
    bk.s[1].made_once = true;
    bk.Release(1, 4400);
    CHECK(bk.s[1].st == H::SlotState::draining && bk.s[1].drop_ms == 4400);
    // a draining slot's owner key is never handed out again before it is free
    CHECK(bk.Want(catcher, p, 130, 4500) == 0);
    bk.Free(0);
    for (std::uint8_t u = 10; u < 10 + R::kSpotSlots - 1; ++u) CHECK(bk.Want(u, p, 50, 5000) >= 0);
    CHECK(bk.Count(H::SlotState::live) == R::kSpotSlots - 1 && bk.Count(H::SlotState::draining) == 1);
    CHECK(bk.Want(99, p, 50, 5000) == -1);  // all 8 taken (7 live, 1 draining)
    bk.Free(1);
    CHECK(bk.Want(99, p, 50, 5000) == 1);
    bk.Clear();
    CHECK(bk.Count(H::SlotState::free) == R::kSpotSlots && bk.Find(99) == -1);
}

// A "partner gone" (or the link / relay) mid-hunt: the live slots are let go into draining (watched until the game drops them), never
// freed while their spot may still be listed; the slot booked again by the next UP is another one, and the gazer rule does not read
// the gazers our own (draining) spot armed as this game's rain. Gate run 20261128-011308: the old Clear() dropped the watch.
void TestLetGo() {
    H::SpotBook bk;
    const double p[3] = {-1040, 40, 464.8}, far[3] = {-700, 40, 464.8};
    CHECK(bk.LetGo(100) == 0);
    CHECK(bk.Want(0, p, R::kSpotHuntRadiusM, 1000) == 0);
    bk.s[0].made_once = true, bk.s[0].made_ms = 1000;
    CHECK(bk.Want(1, far, 60, 1000) == 1);  // a catcher slot that never made a spot
    CHECK(bk.LetGo(2200) == 2);
    CHECK(bk.s[0].st == H::SlotState::draining && bk.s[0].drop_ms == 2200 && bk.s[1].st == H::SlotState::free);
    CHECK(bk.Count(H::SlotState::live) == 0 && bk.Count(H::SlotState::draining) == 1);
    CHECK(bk.LetGo(2300) == 0 && bk.s[0].st == H::SlotState::draining && bk.s[0].drop_ms == 2200);  // a second event keeps the watch
    // the next UP books another slot (slot 0's key is still listed)
    const int i = bk.Want(0, p, R::kSpotHuntRadiusM, 2325);
    CHECK(i == 1);
    // our draining spot covers the new slot's centre: the gazers it armed do not hold the new slot
    CHECK(bk.OwnSpotCovers(static_cast<unsigned>(i), p));
    CHECK(!H::SpotGazerBlock(bk.s[i].made_once || bk.OwnSpotCovers(static_cast<unsigned>(i), p), 20.0));
    CHECK(!bk.OwnSpotCovers(static_cast<unsigned>(i), far));                 // 340 m off: not ours
    CHECK(!bk.OwnSpotCovers(0, p));                                         // slot 1 never made a spot: not counted
    CHECK(H::SpotGazerBlock(bk.OwnSpotCovers(static_cast<unsigned>(i), far), 20.0));  // an armed gazer elsewhere still holds
    // the drain ends as before: gone once the list no longer names slot 0's key
    CHECK(H::DrainVerdict(12300, bk.s[0].drop_ms, false) == H::Drain::gone);
    bk.Free(0);
    CHECK(!bk.OwnSpotCovers(static_cast<unsigned>(i), p));
}

void TestDrain() {
    CHECK(H::DrainVerdict(1000, 0, false) == H::Drain::gone);
    CHECK(H::DrainVerdict(15000, 0, true) == H::Drain::wait);
    CHECK(H::DrainVerdict(15001, 0, true) == H::Drain::stale);
}

// The receiver's spot life over a 14 s test hunt (sandboxed: life 10 s): evaluated every 2 s from the first UP, refreshed while
// live, dropped at the CLEAR; the spot (the game's countdown, probe-findings 4) is gone, and read gone at the 1 s poll, within
// 12 s of the CLEAR (the gate's pass line).
void TestSpotScheduleSandboxed() {
    H::SpotBook bk;
    const double c[3] = {-1046, 36, 120};
    const double clear_ms = 14000;
    double game_life_end = -1, gone_read = -1, last_refresh = -1;
    int refreshes = 0;
    for (double t = 100; t < 40000; t += 33) {
        if (t < clear_ms && std::fmod(t, 250.0) < 33) bk.Want(0, c, R::kSpotHuntRadiusM, t);  // HUNT at 4 Hz
        if (t >= clear_ms && t < clear_ms + 33) bk.End(0, "the hunt ended");
        for (unsigned i = 0; i < R::kSpotSlots; ++i) {
            H::SpotSlot& x = bk.s[i];
            if (x.st == H::SlotState::live && (bk.Due(i, t) || !bk.SourceLive(i, t))) {
                const bool live = bk.SourceLive(i, t);
                x.eval_ms = t;
                R::RainSpotIn in;
                in.bt_rain = 1, in.source_live = live, in.radius = x.radius, in.my_dist_m = 7, in.has_slot = true;
                const R::SpotVerdict v = R::RainSpotGate(in);
                if (v.call == R::SpotCall::make) {
                    game_life_end = t + 1000.0 * R::RainSpotLife(false), last_refresh = t, ++refreshes;
                    x.made_once = true, x.made_ms = t;
                } else if (v.call == R::SpotCall::drop) {
                    CHECK(!live);
                    bk.Release(i, t);
                }
            } else if (x.st == H::SlotState::draining && t - x.poll_ms >= H::kGonePollMs) {
                x.poll_ms = t;
                const bool listed = t < game_life_end;
                if (H::DrainVerdict(t, x.drop_ms, listed) != H::Drain::wait) {
                    gone_read = t;
                    bk.Free(i);
                }
            }
        }
    }
    CHECK(refreshes >= 7 && refreshes <= 8);  // 0, 2, ..., 12 s (+ 14 s at the edge)
    CHECK(last_refresh >= clear_ms - 2100 && last_refresh < clear_ms);
    CHECK(gone_read > clear_ms && gone_read - clear_ms <= 12000);
    CHECK(bk.Count(H::SlotState::free) == R::kSpotSlots);
}

// A real-save receiver (or the bt_sync 8 -1 1 rehearsal). Review (safety 1): a rain spot is timefall there (her cargo, vehicles
// and structures in it are world state in her save), so none is made at any distance; walking in at a truck's 30 m/s from 400 m,
// every evaluation drops.
void TestSpotRealSave() {
    CHECK(R::RainSpotLife(true) == 3.0f && R::RainSpotLife(false) == 10.0f);
    const float r = R::kSpotHuntRadiusM;
    for (double t = 0; t < 20000; t += 2000) {  // the evaluations, every 2 s
        R::RainSpotIn in;
        in.bt_rain = 1, in.source_live = true, in.real_save = true, in.radius = r, in.has_slot = true;
        in.my_dist_m = std::max(0.0, 400.0 - 30.0 * t / 1000.0);
        const R::SpotVerdict v = R::RainSpotGate(in);
        CHECK(v.call == R::SpotCall::drop && v.why && std::strstr(v.why, "timefall there"));
    }
    R::RainSpotIn in;
    in.bt_rain = 1, in.source_live = true, in.real_save = true, in.radius = r, in.has_slot = true, in.my_dist_m = -1;
    CHECK(R::RainSpotGate(in).call == R::SpotCall::drop);  // her distance unknown
    in.my_dist_m = 500, in.carried = true;
    CHECK(R::RainSpotGate(in).call == R::SpotCall::drop);  // a passenger
    in.carried = false;
    CHECK(R::RainSpotGate(in).call == R::SpotCall::drop);  // far away: still none
    in.real_save = false;
    CHECK(R::RainSpotGate(in).call == R::SpotCall::make);  // a sandboxed save makes it
    in.not_playing = "a menu";
    CHECK(R::RainSpotGate(in).call == R::SpotCall::hold);
    in.not_playing = nullptr, in.bt_rain = 0;
    CHECK(R::RainSpotGate(in).call == R::SpotCall::drop);
}

void TestRing() {
    CHECK(H::TarRingRadius(12, false, 0) == 12);
    CHECK(H::TarRingRadius(0, false, 0) == H::kRingDefaultM);
    CHECK(H::TarRingRadius(std::numeric_limits<float>::quiet_NaN(), false, 0) == H::kRingDefaultM);
    CHECK(H::TarRingRadius(500, false, 0) == H::kRingMaxM && H::TarRingRadius(1, false, 0) == H::kRingMinM);
    const float a = H::TarRingRadius(12, true, 0.6f), b = H::TarRingRadius(12, true, 1.2f), c = H::TarRingRadius(12, true, 5.0f);
    CHECK(a < 12 && b < a && b == c && std::fabs(b - 12 * 0.4f) < 1e-5f);
}

int main() {
    TestHello();
    TestSendGate();
    TestCrowdAndPull();
    TestOp8();
    TestPlan();
    TestPlanThroughTracker();
    TestGazerRuleAndDist();
    TestBook();
    TestDrain();
    TestLetGo();
    TestSpotScheduleSandboxed();
    TestSpotRealSave();
    TestRing();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
