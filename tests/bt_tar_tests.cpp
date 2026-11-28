// Round 31 stage 4 (bt_tar): the pure part's checks (native/mod/bt_tar_rules.hpp; the shared codecs in bt_rules.hpp). No game.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bt_tar_rules.hpp"

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

namespace R = st::btrules;
namespace T = st::btrules::tar;

static_assert(T::kStage == 4, "the stage");
static_assert(!T::kRealSaveOk, "a real-save guest stays refused until the gate's cargo read (build spec stage 4)");

// The shared BTS1 header round trip.
void TestHello() {
    R::BtsHello h;
    h.level = 2, h.role = 1, h.caps = R::kCapField;
    std::uint8_t b[R::kBtsHelloSize] = {};
    const std::size_t n = R::EncodeHello(1, h, b, sizeof(b));
    R::BtsHello o;
    CHECK(n == R::kBtsHelloSize && R::DecodeHello(b, n, o) && o.caps == h.caps && o.level == 2 && o.role == 1);
}

// A gate input that lets the record in.
R::TarFeelIn Open() {
    R::TarFeelIn in;
    in.bt_tar = 1, in.hunt_up = true, in.since_hunt_ms = 100, in.hm_ready = true, in.dist_m = 10, in.count = 0;
    return in;
}

void TestGate() {
    R::TarFeelIn in = Open();
    T::FeelVerdict v = T::FeelGate(in, true);
    CHECK(v.call == R::TarCall::in && v.why == T::Why::none);
    // each refusal, in TarFeelGate's order
    in = Open(), in.emergency = true, in.bt_tar = 0;
    CHECK(T::FeelGate(in, true).why == T::Why::emergency);
    in = Open(), in.bt_tar = 0;
    CHECK(T::FeelGate(in, true).why == T::Why::off && T::FeelGate(in, true).call == R::TarCall::out);
    in = Open();
    CHECK(T::FeelGate(in, false).why == T::Why::no_source);
    in = Open(), in.hunt_up = false;
    CHECK(T::FeelGate(in, true).why == T::Why::cleared);
    in = Open(), in.since_hunt_ms = 3001;
    CHECK(T::FeelGate(in, true).why == T::Why::quiet);
    in = Open(), in.since_hunt_ms = 3000;
    CHECK(T::FeelGate(in, true).call == R::TarCall::in);
    in = Open(), in.loading = true, in.carried = true;
    CHECK(T::FeelGate(in, true).why == T::Why::load);
    in = Open(), in.carried = true, in.real_save = true;
    CHECK(T::FeelGate(in, true).why == T::Why::carried);
    in = Open(), in.real_save = true, in.hm_ready = false;
    v = T::FeelGate(in, true);
    CHECK(v.call == R::TarCall::out && v.why == T::Why::real_save);
    CHECK(std::strstr(T::WhyText(v.why), "this game plays on its own real save") == T::WhyText(v.why));
    in = Open(), in.real_save = true, in.real_save_ok = true;
    CHECK(T::FeelGate(in, true).call == R::TarCall::in);
    in = Open(), in.hm_ready = false;
    CHECK(T::FeelGate(in, true).why == T::Why::hm_not_ready);
    in = Open(), in.dist_m = 300.5;
    CHECK(T::FeelGate(in, true).why == T::Why::too_far);
    in = Open(), in.dist_m = -1;
    CHECK(T::FeelGate(in, true).why == T::Why::too_far);
    in = Open(), in.dist_m = 300;
    CHECK(T::FeelGate(in, true).call == R::TarCall::in);
    in = Open(), in.not_playing = "a menu";
    v = T::FeelGate(in, true);
    CHECK(v.call == R::TarCall::hold && v.why == T::Why::not_playing && std::strcmp(v.not_playing, "a menu") == 0);
    in = Open(), in.count = 10;
    v = T::FeelGate(in, true);
    CHECK(v.call == R::TarCall::hold && v.why == T::Why::list_full && std::strcmp(T::WhyText(v.why), "list full (10): not added") == 0);
    in = Open(), in.count = 10, in.ours_in = true;
    CHECK(T::FeelGate(in, true).call == R::TarCall::in);
    // the same calls as the shared TarFeelGate wherever the source is there and the real-save rule does not decide
    for (int k = 0; k < 9; ++k) {
        R::TarFeelIn a = Open();
        if (k == 1) a.bt_tar = 0;
        if (k == 2) a.hunt_up = false;
        if (k == 3) a.since_hunt_ms = 5000;
        if (k == 4) a.loading = true;
        if (k == 5) a.carried = true;
        if (k == 6) a.hm_ready = false;
        if (k == 7) a.dist_m = 1000;
        if (k == 8) a.count = 10;
        CHECK(T::FeelGate(a, true).call == R::TarFeelGate(a).call);
    }
    // an out that ends the episode needs no re-add hold-off
    CHECK(T::EndsEpisode(T::Why::cleared) && T::EndsEpisode(T::Why::quiet) && T::EndsEpisode(T::Why::off) && T::EndsEpisode(T::Why::emergency));
    CHECK(!T::EndsEpisode(T::Why::load) && !T::EndsEpisode(T::Why::gone_from_list) && !T::EndsEpisode(T::Why::too_far) && !T::EndsEpisode(T::Why::link_lost));
    for (int w = 0; w <= static_cast<int>(T::Why::hm_changed); ++w)
        CHECK((w == 0) == (T::WhyText(static_cast<T::Why>(w))[0] == 0));
}

void TestInside() {
    const double pos[3] = {-1040.0, 40.0, 464.8};
    const double in1[3] = {-1040.0, 45.4, 464.0};  // 5.4 m: inside r 6 (5.5)
    const double out1[3] = {-1040.0, 45.6, 464.0};
    const double high[3] = {-1040.0, 40.0, 466.9};  // above pos.z + 2
    const double low[3] = {-1040.0, 40.0, 400.0};   // no lower bound
    T::Inside o = T::InsideTest(in1, pos, 6.0f, 2.0f);
    CHECK(o.in && std::fabs(o.rin - 5.5) < 1e-9 && std::fabs(o.top - 466.8) < 1e-9);
    CHECK(!T::InsideTest(out1, pos, 6.0f, 2.0f).in);
    CHECK(!T::InsideTest(high, pos, 6.0f, 2.0f).in);
    CHECK(T::InsideTest(low, pos, 6.0f, 2.0f).in);
    // a small radius keeps the 1 m floor
    const double close[3] = {-1040.0, 40.9, 464.0};
    CHECK(T::InsideTest(close, pos, 1.0f, 2.0f).in && std::fabs(T::InsideTest(close, pos, 1.0f, 2.0f).rin - 1.0) < 1e-9);
    CHECK(!T::InsideTest(nullptr, pos, 6.0f, 2.0f).in);
    // probe 26's out sample: 2D 30.0 m vs 5.5, z 482.3 vs top 466.8
    const double away[3] = {-1010.0, 40.0, 482.3};
    o = T::InsideTest(away, pos, 6.0f, 2.0f);
    CHECK(!o.in && std::fabs(o.d2 - 30.0) < 1e-9);
    // the neutral z: never inside
    const double neutral[3] = {-1040.0, 40.0, T::kNeutralZ};
    CHECK(!T::InsideTest(in1, neutral, 6.0f, 2.0f).in);
}

void TestRecord() {
    R::BtsHunt m;
    m.kind = R::kHuntUp, m.episode = 3, m.center[0] = -1040.0f, m.center[1] = 40.0f, m.center[2] = 464.75f, m.radius = 12.0f;
    double pos[3];
    float r = 0;
    CHECK(T::RecordFrom(m, pos, r) && r == 12.0f && pos[0] == -1040.0 && pos[2] == 464.75);
    m.radius = 0.2f;
    CHECK(T::RecordFrom(m, pos, r) && r == T::kRadiusMinM);
    m.radius = 900.0f;
    CHECK(T::RecordFrom(m, pos, r) && r == T::kRadiusMaxM);
    m.center[1] = std::nanf("");
    CHECK(!T::RecordFrom(m, pos, r));
    m.center[1] = 1e8f;
    CHECK(!T::RecordFrom(m, pos, r));
    const double a[3] = {0, 0, 0}, b[3] = {0.3, 0, 0}, c[3] = {0.5, 0, 0};
    CHECK(!T::NeedsRefresh(a, 6.0f, b, 6.1f));
    CHECK(T::NeedsRefresh(a, 6.0f, c, 6.0f));
    CHECK(T::NeedsRefresh(a, 6.0f, b, 6.25f));
    // a HUNT through the codec keeps the centre and radius the record takes
    R::BtsHunt u;
    u.kind = R::kHuntUp, u.flags = R::kHfTest, u.episode = T::kTestEpisode0, u.center[0] = -1040.0f, u.center[1] = 40.0f, u.center[2] = 464.8f;
    u.radius = T::kTestRadiusM;
    std::uint8_t buf[R::kBtsHuntSize];
    const std::size_t n = R::EncodeHunt(7, u, buf, sizeof(buf));
    R::BtsHunt d;
    CHECK(n == R::kBtsHuntSize && R::DecodeHunt(buf, n, d) && d.episode == T::kTestEpisode0 && d.radius == T::kTestRadiusM && d.center[2] == 464.8f);
    CHECK(T::RecordFrom(d, pos, r) && r == 6.0f);
}

void TestList() {
    std::int32_t n = 0;
    std::uintptr_t a[R::kTarListMax] = {};
    const std::uintptr_t ours = 0x1000;
    // the game's two crowds, then ours
    CHECK(T::ListAdd(&n, a, 0xA0) == 1 && T::ListAdd(&n, a, 0xB0) == 1 && T::ListAdd(&n, a, ours) == 1 && n == 3 && a[2] == ours);
    CHECK(T::ListHas(n, a, ours) == 1 && T::ListHas(n, a, 0xC0) == 0 && T::ListHas(11, a, ours) == -1 && T::ListHas(-1, a, ours) == -1);
    CHECK(T::ListAdd(&n, a, 0xC0) == 1 && n == 4);
    // removal by pointer: the later entries move down, the freed tail zeroed
    CHECK(T::ListRemove(&n, a, ours) == 1 && n == 3 && a[0] == 0xA0 && a[1] == 0xB0 && a[2] == 0xC0 && a[3] == 0);
    CHECK(T::ListRemove(&n, a, ours) == 0 && n == 3);
    CHECK(T::ListHas(n, a, ours) == 0);
    // full: not added
    while (n < R::kTarListMax) T::ListAdd(&n, a, 0x2000 + static_cast<std::uintptr_t>(n));
    CHECK(T::ListAdd(&n, a, ours) == 0 && n == 10);
    // the last entry
    std::int32_t m = 1;
    std::uintptr_t one[R::kTarListMax] = {ours};
    CHECK(T::ListRemove(&m, one, ours) == 1 && m == 0 && one[0] == 0);
    // a bad count is never edited
    std::int32_t bad = -3;
    CHECK(T::ListAdd(&bad, a, ours) == 0 && bad == -3 && T::ListRemove(&bad, a, ours) == 0);
    bad = 12;
    CHECK(T::ListAdd(&bad, a, ours) == 0);
}

void TestCargo() {
    CHECK(T::CarriedPiece(1, -1, 0x10, 1.0));
    CHECK(!T::CarriedPiece(0, -1, 0x10, 1.0));
    CHECK(!T::CarriedPiece(9, -1, 0x10, 1.0) && !T::CarriedPiece(10, -1, 0x10, 1.0) && T::CarriedPiece(11, -1, 0x10, 1.0));
    CHECK(!T::CarriedPiece(1, 5, 0x10, 1.0) && !T::CarriedPiece(1, -1, 0, 1.0) && !T::CarriedPiece(1, -1, 0x10, 2.6));
    T::CargoSnap a, b;
    CHECK(!T::CargoDiff(a, b).ok);
    a.ok = b.ok = true;
    a.Add(4, 1.0f), a.Add(9, 0.5f), a.Add(12, 0.75f);
    b.Add(9, 0.5f), b.Add(4, 1.0f), b.Add(12, 0.75f);
    T::CargoDelta d = T::CargoDiff(a, b);
    CHECK(d.ok && d.pieces == 3 && d.lost == 0 && d.d == 0);
    char t[96];
    T::CargoText(d, nullptr, t, sizeof(t));
    CHECK(std::strcmp(t, "cargo d 0.000 (3 pieces)") == 0);
    b = T::CargoSnap{};
    b.ok = true;
    b.Add(4, 0.875f), b.Add(12, 0.75f);
    d = T::CargoDiff(a, b);
    CHECK(d.ok && d.pieces == 2 && d.lost == 1 && std::fabs(d.d + 0.125) < 1e-9);
    T::CargoText(d, nullptr, t, sizeof(t));
    CHECK(std::strcmp(t, "cargo d -0.125 (2 pieces, 1 no longer carried)") == 0);
    // the worn tail: which piece fell most (run 20261128-020913's -632.983 over 22 pieces said nothing about the spread)
    CHECK(d.worn == 1 && d.worst_idx == 4 && d.worst_before == 1.0f && d.worst_after == 0.875f);
    T::CargoWornText(d, t, sizeof(t));
    CHECK(std::strncmp(t, "; worn 1 of 2, the most #4 1.0 -> 0.", 36) == 0);
    {
        T::CargoSnap w0, w1;
        w0.ok = w1.ok = true;
        w0.Add(1, 900.0f), w0.Add(2, 900.0f), w0.Add(3, 500.0f);
        w1.Add(1, 880.0f), w1.Add(2, 850.0f), w1.Add(3, 501.0f);  // a gain is not wear
        const T::CargoDelta w = T::CargoDiff(w0, w1);
        CHECK(w.worn == 2 && w.worst_idx == 2 && w.worst_before == 900.0f && w.worst_after == 850.0f && std::fabs(w.d + 69.0) < 1e-6);
        T::CargoWornText(w, t, sizeof(t));
        CHECK(std::strcmp(t, "; worn 2 of 3, the most #2 900.0 -> 850.0") == 0);
        T::CargoText(w, nullptr, t, sizeof(t));
        CHECK(std::strcmp(t, "cargo d -69.000 (3 pieces)") == 0);
    }
    // a tiny float difference reads exactly 0 (never "-0.000")
    b = a;
    b.life[0] = 1.0f - 1e-8f;
    d = T::CargoDiff(a, b);
    T::CargoText(d, nullptr, t, sizeof(t));
    CHECK(d.d == 0 && std::strncmp(t, "cargo d 0.000", 13) == 0);
    T::CargoWornText(d, t, sizeof(t));
    CHECK(t[0] == 0);  // no wear: no tail
    T::CargoText(T::CargoDelta{}, "no baggage manager", t, sizeof(t));
    CHECK(std::strcmp(t, "cargo d n/a (no baggage manager)") == 0);
    // no carried piece: a read of 0 pieces (it decides nothing: the gate needs pieces)
    T::CargoSnap e1, e2;
    e1.ok = e2.ok = true;
    T::CargoText(T::CargoDiff(e1, e2), nullptr, t, sizeof(t));
    CHECK(std::strcmp(t, "cargo d 0.000 (0 pieces)") == 0);
    // the cap
    T::CargoSnap big;
    for (int i = 0; i < T::kCargoMax + 3; ++i) big.Add(static_cast<std::uint32_t>(i), 1.0f);
    CHECK(big.n == T::kCargoMax && big.more == 3);
    big.Add(99, std::nanf(""));
    CHECK(big.more == 3);
}

void TestArgs() {
    CHECK(T::TestArgs(0, 0) == T::TestMode::ahead);
    CHECK(T::TestArgs(-1040, 40) == T::TestMode::at);
    CHECK(T::TestArgs(12, 0) == T::TestMode::at);
    CHECK(T::TestArgs(0, 1) == T::TestMode::real_on && T::TestArgs(0, 2) == T::TestMode::real_off && T::TestArgs(0, 3) == T::TestMode::stop);
    CHECK(T::TestArgs(0, 4) == T::TestMode::bad && T::TestArgs(0, -1) == T::TestMode::bad);
    CHECK(T::TestArgs(std::nan(""), 0) == T::TestMode::bad && T::TestArgs(2e6, 0) == T::TestMode::bad);
}

void TestLimit() {
    T::LineLimit l;
    int dropped = -1;
    for (int i = 0; i < T::kLogBurst; ++i) CHECK(l.Allow(1000.0 + i, &dropped) && dropped == 0);
    CHECK(!l.Allow(1100.0, &dropped) && !l.Allow(1200.0, &dropped));
    CHECK(l.Allow(1000.0 + T::kLogWindowMs, &dropped) && dropped == 2);
    CHECK(l.Allow(1000.0 + T::kLogWindowMs + 1, &dropped) && dropped == 0);
    // a clock going back starts a new window
    CHECK(l.Allow(5.0, &dropped));
}

// The view bt_tar feeds (bt_rules.hpp HuntView): UP, the 3 s rule (SinceMs), CLEAR.
void TestView() {
    R::HuntView v;
    R::BtsHunt m;
    m.kind = R::kHuntUp, m.episode = T::kTestEpisode0 + 1, m.radius = 6;
    v.Take(m, 1000);
    CHECK(v.up && v.episode == T::kTestEpisode0 + 1 && v.SinceMs(1250) == 250);
    R::TarFeelIn in = Open();
    in.hunt_up = v.up, in.since_hunt_ms = v.SinceMs(4100);
    CHECK(T::FeelGate(in, true).why == T::Why::quiet);
    m.kind = R::kHuntClear;
    const R::HuntView::Out o = v.Take(m, 2000);
    CHECK(o.clear && !v.up);
    in.hunt_up = v.up, in.since_hunt_ms = v.SinceMs(2000);
    CHECK(T::FeelGate(in, true).why == T::Why::cleared);
}

int main() {
    TestHello();
    TestGate();
    TestInside();
    TestRecord();
    TestList();
    TestCargo();
    TestArgs();
    TestLimit();
    TestView();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
