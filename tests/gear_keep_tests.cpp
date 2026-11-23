// Gear kept across loads and the private room, the pure part (native/mod/gear_keep.hpp): the BB pod under the self view
// (the game's rule, the cross-check, the fallback) and what the private room took. No game.
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "gear_keep.hpp"

using namespace st::gearkeep;

namespace {
int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

void TestPodAsked() {
    PodRead r;  // the defaults: shown
    CHECK(PodAsked(r));
    r.part_mapped = true;
    CHECK(PodAsked(r));  // mapped and asked
    r.part_asked = false;
    CHECK(!PodAsked(r));  // the part not asked: hidden
    r.part_mapped = false;
    CHECK(PodAsked(r));  // not mapped: the part does not count
    r = PodRead{};
    r.slot_hidden = true;
    CHECK(!PodAsked(r));
    r = PodRead{};
    r.sam_visible = false;
    CHECK(!PodAsked(r));
    r = PodRead{};
    r.forced_hide = true;
    CHECK(!PodAsked(r));
    r = PodRead{};
    r.mask_k = true;
    CHECK(!PodAsked(r));
    // the inventory gate off: the game shows it whatever else says (0x1426C7BAC: mov bl,1)
    r = PodRead{};
    r.slot_hidden = true, r.sam_visible = false, r.forced_hide = true, r.mask_k = true;
    r.gate_ready = false;
    CHECK(PodAsked(r));
    r.gate_ready = true, r.gate_on = false;
    CHECK(PodAsked(r));
    r.gate_on = true;
    CHECK(!PodAsked(r));
}

PodTick Tick(bool self, bool have, bool ask, bool flag, bool plain, double now) {
    PodTick t;
    t.self_shown = self, t.have_ask = have, t.ask = ask, t.flag = flag, t.plain = plain, t.now_ms = now;
    return t;
}

// Round 10's bug: the pod hidden by a load while the self view was off latched hidden for the session.
void TestPodAfterLoad() {
    PodState s;
    // plain play, self view off: the flag, the rule agrees
    PodOut o = DecidePod(s, Tick(false, true, true, true, true, 0));
    CHECK(o.vis && o.src == PodSource::flag && o.ev == PodEvent::none);
    // the self view on: the rule
    o = DecidePod(s, Tick(true, true, true, false, true, 100));
    CHECK(o.vis && o.src == PodSource::ask);
    // a load: the self view off, the game has the pod hidden (the rule says so too)
    o = DecidePod(s, Tick(false, true, false, false, false, 200));
    CHECK(!o.vis && o.src == PodSource::flag && o.ev == PodEvent::none);
    // back in plain play, the self view on: the rule shows it (before round 11 the hidden reading stood)
    o = DecidePod(s, Tick(true, true, true, false, true, 5000));
    CHECK(o.vis && o.src == PodSource::ask);
    CHECK(s.compared == 2 && s.matched == 2 && s.mismatched == 0 && !s.disproved);
}

void TestPodFallback() {
    // no rule (sites off or `gear 30 1`): the memo, taken as worn after an off window without plain play
    PodState s;
    PodOut o = DecidePod(s, Tick(false, false, false, true, true, 0));
    CHECK(o.vis && o.src == PodSource::flag);
    o = DecidePod(s, Tick(true, false, false, false, true, 100));
    CHECK(o.vis && o.src == PodSource::memo);
    // off in a load (hidden there), never in plain play: back under the self view as worn
    o = DecidePod(s, Tick(false, false, false, false, false, 200));
    CHECK(!o.vis);
    o = DecidePod(s, Tick(true, false, false, false, true, 300));
    CHECK(o.vis && o.src == PodSource::worn);
    // off in plain play with the pod really off (BB in the incubator outside a load): that reading stands
    o = DecidePod(s, Tick(false, false, false, false, true, 400));
    CHECK(!o.vis && o.src == PodSource::flag);
    o = DecidePod(s, Tick(true, false, false, false, true, 500));
    CHECK(!o.vis && o.src == PodSource::memo);
    // the memo reading of plain play stands through later non-plain reads in the same off window
    o = DecidePod(s, Tick(false, false, false, true, true, 600));
    o = DecidePod(s, Tick(false, false, false, false, false, 700));
    o = DecidePod(s, Tick(true, false, false, false, true, 800));
    CHECK(o.vis && o.src == PodSource::memo);
}

void TestPodCrossCheck() {
    PodState s;
    // a one-tick lag at a transition: a mismatch that ends, never disproved
    PodOut o = DecidePod(s, Tick(false, true, false, true, true, 0));
    CHECK(o.ev == PodEvent::mismatch_started && s.mismatch);
    o = DecidePod(s, Tick(false, true, false, false, true, 100));
    CHECK(o.ev == PodEvent::mismatch_ended && !s.mismatch && !s.disproved);
    // a long mismatch outside plain play (a load) never disproves
    for (int i = 0; i < 100; ++i) o = DecidePod(s, Tick(false, true, true, false, false, 1000.0 + 100 * i));
    CHECK(!s.disproved && s.mismatch);
    // in plain play it does after kPodDisproveMs (the timer starts at the first plain mismatch)
    o = DecidePod(s, Tick(false, true, true, false, true, 20000));
    CHECK(o.ev == PodEvent::none && !s.disproved);
    o = DecidePod(s, Tick(false, true, true, false, true, 20000 + kPodDisproveMs - 1));
    CHECK(!s.disproved);
    o = DecidePod(s, Tick(false, true, true, false, true, 20000 + kPodDisproveMs));
    CHECK(o.ev == PodEvent::disproved && s.disproved);
    // disproved: the self view takes the fallback, not the rule (the last plain reading was hidden)
    o = DecidePod(s, Tick(true, true, true, false, true, 30000));
    CHECK(!o.vis && o.src == PodSource::memo);
    CHECK(s.mismatched >= 100 && s.matched == 1);
    // a plain mismatch interrupted by a match restarts the timer
    PodState t;
    DecidePod(t, Tick(false, true, true, false, true, 0));
    DecidePod(t, Tick(false, true, true, false, true, 1500));
    DecidePod(t, Tick(false, true, true, true, true, 1600));
    DecidePod(t, Tick(false, true, true, false, true, 1700));
    o = DecidePod(t, Tick(false, true, true, false, true, 3600));
    CHECK(!t.disproved);
    o = DecidePod(t, Tick(false, true, true, false, true, 3700));
    CHECK(t.disproved && o.ev == PodEvent::disproved);
    // the rule unreadable for a read: the open mismatch is closed (no stale timer)
    PodState u;
    DecidePod(u, Tick(false, true, true, false, true, 0));
    DecidePod(u, Tick(false, false, false, false, true, 1000));
    CHECK(!u.mismatch && u.plain_mismatch_ms < 0);
    DecidePod(u, Tick(false, true, true, false, true, 2500));
    CHECK(!u.disproved);
}

// The fallback in the private room: hidden (BB is in the incubator), whatever the memo says; the rule, when read, wins.
void TestPodRoom() {
    PodState s;
    PodTick t = Tick(false, false, false, true, true, 0);
    PodOut o = DecidePod(s, t);  // plain play, the pod worn
    CHECK(o.vis && o.src == PodSource::flag);
    t = Tick(false, false, false, false, false, 100);  // the room load: not plain
    t.in_room = true;
    o = DecidePod(s, t);
    CHECK(!o.vis && o.src == PodSource::flag);
    t = Tick(true, false, false, false, false, 200);  // the self view back in the room, no rule: hidden there
    t.in_room = true;
    o = DecidePod(s, t);
    CHECK(!o.vis && o.src == PodSource::room);
    t = Tick(true, true, true, false, false, 300);  // the rule read in the room: it decides
    t.in_room = true;
    o = DecidePod(s, t);
    CHECK(o.vis && o.src == PodSource::ask);
    t = Tick(true, true, false, false, false, 400);
    t.in_room = true;
    o = DecidePod(s, t);
    CHECK(!o.vis && o.src == PodSource::ask);
    // the rule disproved: the room's fallback again
    s.disproved = true;
    t = Tick(true, true, true, false, false, 500);
    t.in_room = true;
    o = DecidePod(s, t);
    CHECK(!o.vis && o.src == PodSource::room);
    // out of the room (after its load, no plain play yet): taken as worn
    s = PodState{};
    o = DecidePod(s, Tick(false, false, false, false, false, 600));
    o = DecidePod(s, Tick(true, false, false, false, false, 700));
    CHECK(o.vis && o.src == PodSource::worn);
}

void TestPodSourceNames() {
    for (int i = 0; i < 5; ++i) CHECK(PodSourceName(static_cast<PodSource>(i))[0] != '?');
}

constexpr unsigned N = 5;
RegainEvent Step(RoomRegain<N>& s, bool room, bool plain, double now, std::initializer_list<int> c, int (&missing)[N], int* n) {
    int counts[N] = {};
    unsigned i = 0;
    for (int v : c) counts[i++] = v;
    return RegainTick(s, room, plain, now, counts, missing, n);
}

void TestRoomRegain() {
    RoomRegain<N> s;
    int miss[N] = {}, n = -1;
    // field: the last plain reading is kept; a terminal (not plain) does not replace it
    CHECK(Step(s, false, true, 0, {1, 1, 1, 0, 2}, miss, &n) == RegainEvent::none);
    CHECK(Step(s, false, false, 100, {0, 0, 0, 0, 0}, miss, &n) == RegainEvent::none);
    CHECK(s.have_before && s.before[0] == 1 && s.before[4] == 2);
    // the room: entered once
    CHECK(Step(s, true, false, 200, {0, 0, 1, 0, 0}, miss, &n) == RegainEvent::entered);
    CHECK(Step(s, true, false, 300, {0, 0, 1, 0, 0}, miss, &n) == RegainEvent::none);
    // left at 1000: nothing before kRegainWaitMs, nothing while not plain, then kRegainSettleMs of plain play
    CHECK(Step(s, false, false, 1000, {1, 1, 0, 0, 2}, miss, &n) == RegainEvent::none);
    CHECK(Step(s, false, true, 2000, {1, 1, 0, 0, 2}, miss, &n) == RegainEvent::none);
    CHECK(Step(s, false, false, 5000, {1, 1, 0, 0, 2}, miss, &n) == RegainEvent::none);  // a terminal again: settle restarts
    CHECK(Step(s, false, true, 8000, {1, 1, 0, 0, 2}, miss, &n) == RegainEvent::none);
    CHECK(Step(s, false, true, 1000 + kRegainWaitMs, {1, 1, 0, 0, 2}, miss, &n) == RegainEvent::none);  // only 3 s settled
    CHECK(Step(s, false, true, 8000 + kRegainSettleMs, {1, 1, 0, 0, 3}, miss, &n) == RegainEvent::report);
    // key 2 (the cap) not back; key 4 gained one (not counted)
    CHECK(n == 1 && miss[2] == 1 && miss[0] == 0 && miss[1] == 0 && miss[3] == 0 && miss[4] == 0);
    CHECK(s.before[2] == 1);  // before still names it for the log
    // back to field: the next plain read replaces before
    CHECK(Step(s, false, true, 20000, {1, 1, 0, 0, 3}, miss, &n) == RegainEvent::none);
    CHECK(s.before[2] == 0 && s.before[4] == 3);
    // a second visit where everything comes back
    CHECK(Step(s, true, false, 21000, {0, 0, 0, 0, 0}, miss, &n) == RegainEvent::entered);
    CHECK(Step(s, false, true, 22000, {1, 1, 0, 0, 3}, miss, &n) == RegainEvent::none);
    CHECK(Step(s, false, true, 22000 + kRegainWaitMs, {1, 1, 0, 0, 3}, miss, &n) == RegainEvent::report);
    CHECK(n == 0);
    // re-entering the room before the report: no report, one more entered only after a report
    CHECK(Step(s, true, false, 40000, {1, 1, 0, 0, 3}, miss, &n) == RegainEvent::entered);
    CHECK(Step(s, false, true, 41000, {1, 1, 0, 0, 3}, miss, &n) == RegainEvent::none);
    CHECK(Step(s, true, false, 42000, {1, 1, 0, 0, 3}, miss, &n) == RegainEvent::none);
    CHECK(Step(s, false, true, 43000, {1, 1, 0, 0, 3}, miss, &n) == RegainEvent::none);
    CHECK(Step(s, false, true, 43000 + kRegainWaitMs, {1, 0, 0, 0, 3}, miss, &n) == RegainEvent::report);
    CHECK(n == 1 && miss[1] == 1);
    // straight into the room (a load into it): no reading before, the report names nothing
    RoomRegain<N> d;
    CHECK(Step(d, true, false, 0, {0, 0, 0, 0, 0}, miss, &n) == RegainEvent::entered);
    CHECK(!d.have_before);
    CHECK(Step(d, false, true, 1000, {0, 0, 0, 0, 0}, miss, &n) == RegainEvent::none);
    CHECK(Step(d, false, true, 1000 + kRegainWaitMs, {0, 0, 0, 0, 0}, miss, &n) == RegainEvent::report);
    CHECK(n == 0);
}

}  // namespace

int main() {
    TestPodAsked();
    TestPodAfterLoad();
    TestPodFallback();
    TestPodRoom();
    TestPodCrossCheck();
    TestPodSourceNames();
    TestRoomRegain();
    std::printf("gear_keep_tests: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
