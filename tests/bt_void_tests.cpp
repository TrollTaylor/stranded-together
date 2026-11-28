// Round 31 stage 6 (bt_void): the pure part's checks (native/mod/bt_void_rules.hpp; the shared codecs in bt_rules.hpp). No game.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bt_void_rules.hpp"

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

static_assert(st::btrules::voidout::kStage == 6, "the stage");

namespace R = st::btrules;
namespace V = st::btrules::voidout;

bool Has(const char* s, const char* sub) { return s && sub && std::strstr(s, sub) != nullptr; }

void TestHello() {
    R::BtsHello h;
    h.level = 2, h.role = 1, h.caps = static_cast<std::uint16_t>(R::kCapField | R::kCapVoid2 | R::kCapWorldApply);
    std::uint8_t b[R::kBtsHelloSize] = {};
    const std::size_t n = R::EncodeHello(1, h, b, sizeof(b));
    R::BtsHello o;
    CHECK(n == R::kBtsHelloSize && R::DecodeHello(b, n, o) && o.caps == h.caps && o.level == 2 && o.role == 1);
}

void TestCaps() {
    CHECK(V::Caps(0, 2, 1, false) == 0);
    CHECK(V::Caps(1, 0, 1, false) == 0);
    CHECK(V::Caps(1, 2, 0, false) == 0);  // solo / echo
    CHECK(V::Caps(1, 1, 2, false) == R::kCapVoid2);
    CHECK(V::Caps(1, 2, 1, false) == R::kCapVoid2);  // bt_void 1: the blast only
    // bt_void 2 offers craters only while CR is not dropped (kCraterDropped: never this round)
    const std::uint16_t cr = V::CraterDropped() ? R::kCapVoid2 : static_cast<std::uint16_t>(R::kCapVoid2 | R::kCapWorldApply);
    CHECK(V::Caps(2, 2, 1, false) == cr);
    CHECK(V::Caps(2, 2, 2, false) == cr);  // a sandboxed guest
    CHECK(V::Caps(2, 2, 2, true) == R::kCapVoid2);                         // a real-save guest: never
    CHECK(V::Caps(2, 1, 1, false) == R::kCapVoid2);                        // bt_sync 1: sends only
    CHECK(V::SendOk(1, 1, 1, true, R::kCapVoid2));
    CHECK(!V::SendOk(1, 1, 1, false, R::kCapVoid2));
    CHECK(!V::SendOk(1, 2, 2, true, R::kCapVoid));  // the partner understands only VOID v1
    CHECK(!V::SendOk(0, 2, 2, true, R::kCapVoid2));
    CHECK(!V::SendOk(1, 0, 2, true, R::kCapVoid2));
    CHECK(!V::SendOk(1, 2, 0, true, R::kCapVoid2));
}

void TestSources() {
    CHECK(Has(V::SourceName(V::kSrcCatcher), "catcher"));
    CHECK(std::strcmp(V::SourceName(R::kVoidSrcTest), "test") == 0);
    CHECK(std::strcmp(V::SourceName(R::kVoidSrcCatch), R::VoidSourceName(R::kVoidSrcCatch)) == 0);
}

R::BlastIn GoodBlast() {
    R::BlastIn in;
    in.bt_void = 1, in.pos_ok = true, in.age_ms = 100, in.has_resource = true, in.dist_m = 600, in.far_m = 100;
    return in;
}
void TestBlastFor() {
    R::BlastIn in = GoodBlast();
    R::BlastVerdict v = V::BlastFor(2, in);
    CHECK(v.call == R::BlastCall::play && v.far_res);
    v = V::BlastFor(1, in);
    CHECK(v.call == R::BlastCall::drop && Has(v.why, "bt_sync is 1"));
    in.emergency = true;
    CHECK(V::BlastFor(2, in).call == R::BlastCall::drop);
    in = GoodBlast();
    in.dist_m = 50;
    v = V::BlastFor(2, in);
    CHECK(v.call == R::BlastCall::play && !v.far_res);  // near: the near resource
    // a real-save guest: from 300 m only (the near blast untested)
    in.real_save = true;
    v = V::BlastFor(2, in);
    CHECK(v.call == R::BlastCall::drop && Has(v.why, "within 300 m"));
    in.dist_m = -1;
    CHECK(V::BlastFor(2, in).call == R::BlastCall::drop);
    in.dist_m = 300;
    CHECK(V::BlastFor(2, in).call == R::BlastCall::play);
    in = GoodBlast();
    in.blast_active = true;
    v = V::BlastFor(2, in);
    CHECK(v.call == R::BlastCall::hold && Has(v.why, "another"));
    in = GoodBlast();
    in.not_playing = "a menu";
    CHECK(V::BlastFor(2, in).call == R::BlastCall::hold);
    in = GoodBlast();
    in.age_ms = R::kBlastQueueMs + 1;
    CHECK(V::BlastFor(2, in).call == R::BlastCall::drop);
    in = GoodBlast();
    in.loading = true;
    CHECK(V::BlastFor(2, in).call == R::BlastCall::drop);
    in = GoodBlast();
    in.has_resource = false;
    CHECK(V::BlastFor(2, in).call == R::BlastCall::hold);
    in = GoodBlast();
    in.bt_void = 0;
    CHECK(V::BlastFor(2, in).call == R::BlastCall::drop);
}

R::CraterApplyIn GoodCrater(int role) {
    R::CraterApplyIn in;
    in.bt_void = 2, in.role = role, in.sandboxed = true, in.sender_sandboxed = true, in.world_equal = true, in.records_equal = true;
    in.flags = static_cast<std::uint8_t>(R::kV2Catcher | R::kV2CraterWanted);
    in.age_ms = 100;
    return in;
}
void TestCraterGateRows() {
    CHECK(V::CraterGateRows(2, GoodCrater(2)).call == R::KillCall::apply);
    CHECK(V::CraterGateRows(2, GoodCrater(1)).call == R::KillCall::apply);
    CHECK(V::CraterGateRows(1, GoodCrater(2)).call == R::KillCall::refuse);  // bt_sync 1
    // the gazer row: refused for good (PF 17), whatever the caller says
    R::CraterApplyIn in = GoodCrater(2);
    in.flags = static_cast<std::uint8_t>(R::kV2Gazer | R::kV2CraterWanted);
    in.gazer_crater_ok = true;
    R::KillVerdict v = V::CraterGateRows(2, in);
    CHECK(v.call == R::KillCall::refuse && Has(v.why, "not a catcher"));
    in.role = 1;
    CHECK(V::CraterGateRows(2, in).call == R::KillCall::refuse);
    // a real-save guest: never
    in = GoodCrater(2);
    in.sandboxed = false;
    v = V::CraterGateRows(2, in);
    CHECK(v.call == R::KillCall::refuse && Has(v.why, "this game plays on its own real save"));
    // the host: a real-save guest's crater stays in her world; another world; other records
    in = GoodCrater(1);
    in.sender_sandboxed = false;
    CHECK(V::CraterGateRows(2, in).call == R::KillCall::refuse);
    in = GoodCrater(1);
    in.world_equal = false;
    CHECK(V::CraterGateRows(2, in).call == R::KillCall::refuse);
    in = GoodCrater(1);
    in.records_equal = false;
    v = V::CraterGateRows(2, in);
    CHECK(v.call == R::KillCall::refuse && Has(v.why, "records"));
    // no crater wanted; bt_void 1; not a two-game role
    in = GoodCrater(2);
    in.flags = R::kV2Catcher;
    CHECK(V::CraterGateRows(2, in).call == R::KillCall::refuse);
    in = GoodCrater(2);
    in.bt_void = 1;
    CHECK(V::CraterGateRows(2, in).call == R::KillCall::refuse);
    in = GoodCrater(0);
    CHECK(V::CraterGateRows(2, in).call == R::KillCall::refuse);
    // a test crater lands only on a sandboxed save (the host's real save never takes one)
    in = GoodCrater(1);
    in.flags = static_cast<std::uint8_t>(in.flags | R::kV2Test);
    in.sandboxed = false;
    v = V::CraterGateRows(2, in);
    CHECK(v.call == R::KillCall::refuse && Has(v.why, "test crater"));
    in.sandboxed = true;
    CHECK(V::CraterGateRows(2, in).call == R::KillCall::apply);
    // holds and expiry
    in = GoodCrater(2);
    in.not_playing = "a cutscene";
    CHECK(V::CraterGateRows(2, in).call == R::KillCall::hold);
    in = GoodCrater(2);
    in.carried = true;
    CHECK(V::CraterGateRows(2, in).call == R::KillCall::hold);
    in = GoodCrater(2);
    in.age_ms = R::kCraterHoldMs + 1;
    CHECK(V::CraterGateRows(2, in).call == R::KillCall::refuse);
    in = GoodCrater(2);
    in.loading = true;
    CHECK(V::CraterGateRows(2, in).call == R::KillCall::refuse);
}

// CR dropped: CraterGate refuses whatever the rows would apply or hold, and keeps the rows' own refusals (a real-save guest's line).
void TestCraterDropped() {
    CHECK(V::CraterDropped());
    R::KillVerdict v = V::CraterGate(2, GoodCrater(2));
    CHECK(v.call == R::KillCall::refuse && Has(v.why, "CR dropped") && Has(v.why, "otgc_set_mission_progress"));
    CHECK(V::CraterGate(2, GoodCrater(1)).call == R::KillCall::refuse);
    R::CraterApplyIn in = GoodCrater(2);
    in.flags = static_cast<std::uint8_t>(in.flags | R::kV2Test);
    CHECK(V::CraterGate(2, in).call == R::KillCall::refuse);  // the test crater too
    in = GoodCrater(2);
    in.not_playing = "a cutscene";
    CHECK(V::CraterGate(2, in).call == R::KillCall::refuse);  // no hold either
    in = GoodCrater(2);
    in.sandboxed = false;
    v = V::CraterGate(2, in);
    CHECK(v.call == R::KillCall::refuse && Has(v.why, "this game plays on its own real save"));
    v = V::CraterGate(1, GoodCrater(2));
    CHECK(v.call == R::KillCall::refuse && Has(v.why, "bt_sync is 1"));
}

void TestVoid2FromLocal() {
    V::LocalIn in;
    in.pos[0] = 1000, in.pos[1] = -2000, in.pos[2] = 300;
    in.pos_ok = true, in.caused = true, in.source = R::kVoidSrcCatch, in.far_dist = 100;
    in.me[0] = 1000, in.me[1] = -2000, in.me[2] = 300, in.me_ok = true;
    in.sender_ms = 5, in.void_id = 7, in.world = 0xABCD;
    R::BtsVoid2 m;
    CHECK(V::Void2FromLocal(in, m));
    CHECK(m.flags == R::kV2Gazer && m.crater_r == 0 && m.class1 == 0 && m.player_dist == 0 && m.void_id == 7 && m.world == 0xABCD);
    CHECK(!(m.flags & R::kV2CraterWanted));  // a FUN_14299B1B0 voidout: the blast only (PF 17)
    in.me[0] = 1200, in.baggage = true;
    CHECK(V::Void2FromLocal(in, m) && (m.flags & R::kV2Far) && (m.flags & R::kV2Baggage) && std::fabs(m.player_dist - 200.0f) < 0.01f);
    in.me_ok = false;
    CHECK(V::Void2FromLocal(in, m) && m.player_dist == -1.0f && !(m.flags & R::kV2Far));
    in.pos_ok = false;
    CHECK(!V::Void2FromLocal(in, m));
    // the wire round trip
    in.pos_ok = true, in.me_ok = true;
    CHECK(V::Void2FromLocal(in, m));
    std::uint8_t b[R::kBtsVoid2Size] = {};
    const std::size_t n = R::EncodeVoid2(3, m, b, sizeof(b));
    R::BtsVoid2 o;
    CHECK(n == R::kBtsVoid2Size && R::DecodeVoid2(b, n, o) && o.flags == m.flags && o.pos[1] == -2000 && o.void_id == 7 && o.source == R::kVoidSrcCatch);
}

void TestVoid2FromCatcher() {
    V::CatcherIn in;
    in.pos[0] = 10, in.pos[1] = 20, in.pos[2] = 30, in.pos_ok = true, in.class1 = 2, in.radius = 0;
    R::BtsVoid2 m;
    CHECK(V::Void2FromCatcher(in, m));
    CHECK(m.source == V::kSrcCatcher && m.flags == (R::kV2Catcher | R::kV2CraterWanted) && m.crater_r == R::kCraterRadiusDefault && m.class1 == 2);
    in.radius = 60;
    CHECK(V::Void2FromCatcher(in, m) && m.crater_r == 60);
    in.class1 = 99;
    CHECK(V::Void2FromCatcher(in, m) && m.class1 == 0);
    in.pos_ok = false;
    CHECK(!V::Void2FromCatcher(in, m));
    // a gazer could never ask for one through this path: only the catcher path sets kV2CraterWanted
    V::LocalIn l;
    l.pos_ok = true, l.caused = true;
    CHECK(V::Void2FromLocal(l, m) && !(m.flags & R::kV2CraterWanted));
}

void TestParse() {
    V::TestArgs a;
    CHECK(V::ParseTest(600, 1, a) && a.m == 600 && a.from_partner && !a.crater && !a.standin);
    CHECK(V::ParseTest(150, 3, a) && a.from_partner && a.crater);
    CHECK(V::ParseTest(10, 0, a) && !a.from_partner && !a.crater);
    CHECK(V::ParseTest(10, std::nan(""), a) && !a.from_partner);  // the mode left out: 0
    CHECK(V::ParseTest(1, 4, a) && a.standin && a.standin_on);
    CHECK(V::ParseTest(0, 4, a) && a.standin && !a.standin_on);
    CHECK(!V::ParseTest(2, 4, a));
    CHECK(!V::ParseTest(-1, 1, a));
    CHECK(!V::ParseTest(3001, 1, a));
    CHECK(!V::ParseTest(std::nan(""), 1, a));
    CHECK(!V::ParseTest(10, 5, a));
    CHECK(!V::ParseTest(10, 1.5, a));
}

void TestSpot() {
    const double me[3] = {100, 0, 10}, partner[3] = {0, 0, 50};
    double out[3] = {};
    CHECK(V::TestSpot(me, 0, 1, partner, true, 600, true, out));
    CHECK(std::fabs(out[0] - 600) < 1e-9 && std::fabs(out[1]) < 1e-9 && out[2] == 50);
    CHECK(std::fabs(R::Dist3(partner, out) - 600) < 1e-6);
    CHECK(V::TestSpot(me, 0, 1, partner, true, 0, true, out) && out[0] == 0 && out[2] == 50);  // at the partner
    CHECK(!V::TestSpot(me, 0, 1, partner, false, 600, true, out));
    // on top of each other: my facing
    const double same[3] = {100, 0.1, 10};
    CHECK(V::TestSpot(me, 0, 1, same, true, 30, true, out) && std::fabs(out[1] - 30.1) < 1e-9 && out[2] == 10);
    // ahead of me
    CHECK(V::TestSpot(me, 1, 0, nullptr, false, 50, false, out) && out[0] == 150 && out[2] == 10);
    R::BtsVoid2 m;
    CHECK(V::Void2Test(out, 50, true, 1, 2, 3, m) && m.source == R::kVoidSrcTest && m.reason == R::kVoidReasonTest);
    CHECK(m.flags == (R::kV2Test | R::kV2Catcher | R::kV2CraterWanted) && m.crater_r == V::kTestCraterR);
    CHECK(V::Void2Test(out, -1, false, 1, 2, 3, m) && m.flags == R::kV2Test && m.crater_r == 0 && m.player_dist == -1.0f);
}

void TestRepeatFifo() {
    const double a[3] = {0, 0, 0}, b[3] = {50, 50, 0}, c[3] = {200, 0, 0};
    CHECK(V::Repeat(a, 1000, b, 5000));
    CHECK(!V::Repeat(a, 1000, b, 6001));
    CHECK(!V::Repeat(a, 1000, c, 1500));
    CHECK(V::Repeat(b, 5000, a, 1000));  // either order
    V::Fifo<int, 3> f;
    CHECK(f.Push(1) && f.Push(2) && f.Push(3) && !f.Push(4) && f.n == 3);
    f.PopFront();
    CHECK(f.n == 2 && f.e[0] == 2 && f.e[1] == 3);
    f.Clear();
    CHECK(f.n == 0);
    f.PopFront();
    CHECK(f.n == 0);
}

void TestFade() {
    const V::Fade ours = V::FadeFor(150);
    CHECK(ours.start == 150 && ours.range == 450);
    CHECK(V::FadeFor(-5).start == 150 && V::FadeFor(99999).start == 5000);
    const V::Fade shipped;
    CHECK(shipped.start == 11500 && shipped.range == 11800);
    // what to save: what is there, unless it is ours already (a restore that failed) or junk
    CHECK(V::SameFade(V::FadeToSave(shipped, ours), shipped));
    CHECK(V::SameFade(V::FadeToSave(ours, ours), shipped));
    CHECK(V::SameFade(V::FadeToSave(V::Fade{std::nanf(""), 1}, ours), shipped));
    CHECK(V::SameFade(V::FadeToSave(V::Fade{9000, 9100}, ours), V::Fade{9000, 9100}));
    // the restore: ours there -> write; the saved there -> done; else leave it
    CHECK(V::RestoreFor(ours, ours, shipped) == V::RestoreCall::write);
    CHECK(V::RestoreFor(shipped, ours, shipped) == V::RestoreCall::done);
    CHECK(V::RestoreFor(V::Fade{1, 2}, ours, shipped) == V::RestoreCall::changed);
    // the flash at the gate's distances: 600 m faded out, 150 m full
    CHECK(R::FlashFade(600, ours.start, ours.range) == 0.0f);
    CHECK(R::FlashFade(150, ours.start, ours.range) == 1.0f);
}

void TestNearCheck() {
    V::NearRead a;
    a.health = 100, a.flow_ok = true, a.flow_calls = 4, a.hm_ok = true, a.hm140 = 0;
    V::NearRead b = a;
    char out[200] = {};
    CHECK(V::NearCheck(a, b, out, sizeof(out)) && std::strcmp(out, "unchanged") == 0);
    b.health = 100.3f;
    CHECK(V::NearCheck(a, b, out, sizeof(out)));
    b.health = 60;
    CHECK(!V::NearCheck(a, b, out, sizeof(out)) && Has(out, "CHANGED: ") && Has(out, "health 100.0 -> 60.0"));
    b = a, b.flow_calls = 5;
    CHECK(!V::NearCheck(a, b, out, sizeof(out)) && Has(out, "flow calls 4 -> 5"));
    b = a, b.hm140 = 0x1234;
    CHECK(!V::NearCheck(a, b, out, sizeof(out)) && Has(out, "hm+0x140"));
    b = a, b.game_over = true;
    CHECK(!V::NearCheck(a, b, out, sizeof(out)) && Has(out, "GameOver"));
    b = a, b.health = 10, b.game_over = true, b.flow_calls = 9;
    CHECK(!V::NearCheck(a, b, out, sizeof(out)) && Has(out, "health") && Has(out, ", flow calls") && Has(out, ", GameOver"));
    b = a, b.flow_ok = false, b.flow_calls = 0;  // not hooked: not compared
    CHECK(V::NearCheck(a, b, out, sizeof(out)));
    CHECK(V::NearCheck(a, a, nullptr, 0));
}

void TestSameCrater() {
    const double a[3] = {10, 20, 30}, b[3] = {10.5, 20, 30}, c[3] = {12, 20, 30};
    CHECK(V::SameCrater(a, 40, b, 40.2f));
    CHECK(!V::SameCrater(a, 40, c, 40));
    CHECK(!V::SameCrater(a, 40, b, 41));
    CHECK(!V::SameCrater(nullptr, 40, b, 40));
    CHECK(R::CraterFlags(30, -1, 40) == 1u && R::CraterFlags(51, -1, 40) == 0u && R::CraterFlags(-1, -1, 40) == 0u);
}

void TestLogBucket() {
    V::LogBucket b;
    int ok = 0;
    for (int i = 0; i < 40; ++i) ok += b.Take(1000) ? 1 : 0;
    CHECK(ok == static_cast<int>(V::kLogBurst) && b.dropped == 40u - static_cast<unsigned>(V::kLogBurst));
    CHECK(!b.Take(1500));
    CHECK(b.Take(2600));  // 1.6 s: one more
    CHECK(!b.Take(2600));
    for (int i = 0; i < 100; ++i) b.Take(1e9);
    CHECK(b.tokens >= 0 && b.tokens <= V::kLogBurst);
}

int main() {
    TestHello();
    TestCaps();
    TestSources();
    TestBlastFor();
    TestCraterGateRows();
    TestCraterDropped();
    TestVoid2FromLocal();
    TestVoid2FromCatcher();
    TestParse();
    TestSpot();
    TestRepeatFifo();
    TestFade();
    TestNearCheck();
    TestSameCrater();
    TestLogBucket();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
