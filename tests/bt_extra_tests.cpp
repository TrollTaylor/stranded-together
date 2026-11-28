// Round 31 stage 8 (bt_extra): the pure part's checks (native/mod/bt_extra_rules.hpp; the shared codecs in bt_rules.hpp). No game.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bt_extra_rules.hpp"

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
namespace X = st::btrules::extra;

static_assert(X::kStage == 8, "the stage");
static_assert(X::kKillLookReason == 0x5414860Du, "the kill look reason (PF 21: stats d 0 / 0)");
static_assert(!X::kSkyRealSaveOk, "SKY waits for bt-sky-sun on a real-save guest");

// The shared BTS1 header round trip.
void TestHello() {
    R::BtsHello h;
    h.level = 2, h.role = 1, h.caps = R::kCapField | R::kCapWorldApply;
    std::uint8_t b[R::kBtsHelloSize] = {};
    const std::size_t n = R::EncodeHello(1, h, b, sizeof(b));
    R::BtsHello o;
    CHECK(n == R::kBtsHelloSize && R::DecodeHello(b, n, o) && o.caps == h.caps && o.level == 2 && o.role == 1);
}

void TestKillLook() {
    CHECK(X::KillLookReason(0, true, false, false) == 0);
    CHECK(X::KillLookReason(1, true, false, false) == 0x5414860Du);
    CHECK(X::KillLookReason(5, true, false, false) == 0x5414860Du);
    CHECK(X::KillLookReason(1, false, false, false) == 0);  // the stats checks failed
    CHECK(X::KillLookReason(1, true, true, false) == 0);    // a real-save guest
    CHECK(X::KillLookReason(1, true, false, true) == 0);    // stopped
    CHECK(X::KillLookReason(1, true, false, false) != R::kKillStatsA && X::KillLookReason(1, true, false, false) != R::kKillStatsB);
    // the window: armed, extended by each applied kill, due 5 s after the last; 15 s after the arm with none
    X::KillWindow w;
    CHECK(!w.Due(1e9));
    w.Arm(1000);
    CHECK(w.open && !w.Due(15999) && w.Due(16000));
    w.Arm(5000);  // already open: the first arm stays
    CHECK(w.arm_ms == 1000);
    w.Applied(2000, true);
    CHECK(w.applied == 1 && w.realized == 1 && !w.Due(6999) && w.Due(7000));
    w.Applied(6000, false);
    CHECK(w.applied == 2 && w.realized == 1 && !w.Due(10999) && w.Due(11000));
    w.Close();
    CHECK(!w.open && !w.Due(1e9));
    w.Applied(20000, true);  // closed: ignored
    CHECK(w.applied == 2);
    w.Arm(30000);
    CHECK(w.open && w.applied == 0 && w.last_ms < 0);
    // the stats delta wraps
    CHECK(X::StatDelta(214, 215) == 1 && X::StatDelta(215, 215) == 0 && X::StatDelta(0xFFFFFFFFu, 0) == 1 && X::StatDelta(5, 3) == -2);
    // realized: flag 0x1 (state 2 at the update) or state 2 now; the dead bit 0x8 alone is not
    CHECK(X::Realized(0x00090A01u, 2) && X::Realized(0x1u, 0) && X::Realized(0, 2));
    CHECK(!X::Realized(0x0001000Cu, 0) && !X::Realized(0x00010004u, 0));
}

void TestRespawnCodec() {
    R::BtsRespawn m;
    m.scene = 0x390B7DE4u, m.field = 0x390B7DE4u, m.sender_ms = 123456;
    std::uint8_t b[R::kBtsRespawnSize];
    const std::size_t n = R::EncodeRespawn(7, m, b, sizeof(b));
    CHECK(n == R::kBtsRespawnSize);
    R::BtsRespawn o;
    CHECK(R::DecodeRespawn(b, n, o) && o.scene == m.scene && o.field == m.field && o.sender_ms == m.sender_ms);
    CHECK(!R::DecodeRespawn(b, n - 1, o));
    R::BtsRespawn z;  // scene 0 (the GUID unreadable): never encoded
    CHECK(R::EncodeRespawn(7, z, b, sizeof(b)) == 0);
    // GetUUIDHashOfGazerScene 0x1429A71E0 = crc32c(crc32c(0, lo64), hi64), the census's field id
    std::uint8_t g[16];
    for (int i = 0; i < 16; ++i) g[i] = static_cast<std::uint8_t>(i * 17 + 3);
    CHECK(R::GuidHash(g) == R::Crc32cU64(R::Crc32cU64(0, R::Le64(g)), R::Le64(g + 8)));
}

void TestRespawnSend() {
    X::RespawnSendIn in;
    in.bt_respawn = 1, in.role = 1, in.sync_level = 2, in.peer = true, in.peer_role = 2, in.peer_level = 2, in.checks_ok = true, in.field = 0x390B7DE4u;
    in.peer_caps = 0x2000;
    CHECK(X::RespawnSendWhyNot(in) == nullptr);
    X::RespawnSendIn t = in;
    // review (wire 2): an older build (v0.44 caps 0xFF) or a guest without the respawn cap gets no type 16
    t.peer_caps = 0xFF;
    CHECK(X::RespawnSendWhyNot(t) != nullptr && std::strstr(X::RespawnSendWhyNot(t), "no cap 0x2000"));
    t.peer_caps = 0xDFFF;
    CHECK(X::RespawnSendWhyNot(t) != nullptr);
    t.peer_caps = 0x20FF;
    CHECK(X::RespawnSendWhyNot(t) == nullptr);
    t = in;
    t.bt_respawn = 0;
    CHECK(X::RespawnSendWhyNot(t) != nullptr);
    t = in, t.checks_ok = false;
    CHECK(X::RespawnSendWhyNot(t) != nullptr);
    t = in, t.role = 2;
    CHECK(X::RespawnSendWhyNot(t) != nullptr);
    t = in, t.sync_level = 1;
    CHECK(X::RespawnSendWhyNot(t) != nullptr);
    t = in, t.peer = false;
    CHECK(X::RespawnSendWhyNot(t) != nullptr);
    t = in, t.peer_role = 1;
    CHECK(X::RespawnSendWhyNot(t) != nullptr);
    t = in, t.peer_level = 1;
    CHECK(X::RespawnSendWhyNot(t) != nullptr);
    t = in, t.field = 0;
    CHECK(X::RespawnSendWhyNot(t) != nullptr);
    t = in, t.since_last_ms = 4999;
    CHECK(X::RespawnSendWhyNot(t) != nullptr);
    t = in, t.since_last_ms = 5000;
    CHECK(X::RespawnSendWhyNot(t) == nullptr);
    // the field ring
    X::FieldRing r;
    CHECK(r.Since(1, 100) >= 1e17);
    r.Add(1, 100);
    r.Add(2, 200);
    CHECK(r.Since(1, 600) == 500 && r.Since(2, 600) == 400 && r.Since(3, 600) >= 1e17 && r.Since(0, 600) >= 1e17);
    r.Add(1, 500);
    CHECK(r.Since(1, 600) == 100);
    for (int i = 0; i < 8; ++i) r.Add(9, 700);  // the oldest are overwritten
    CHECK(r.Since(1, 800) >= 1e17 && r.Since(9, 800) == 100);
    r.Clear();
    CHECK(r.Since(9, 800) >= 1e17);
    // the detour's packed slot
    std::uint32_t f = 0;
    bool ok = false;
    CHECK(!X::UnpackCall(0, &f, &ok));
    CHECK(X::UnpackCall(X::PackCall(0x390B7DE4u, true), &f, &ok) && f == 0x390B7DE4u && ok);
    CHECK(X::UnpackCall(X::PackCall(0, false), &f, &ok) && f == 0 && !ok);
    CHECK(X::PackCall(0, false) != 0);
}

void TestRespawnApply() {
    // the scene match: the field, +0x3A8 = the locator's scene, its GUID hash = the field
    X::SceneSeen s;
    s.field = 0x390B7DE4u, s.owner = 0x1000, s.loc_scene = 0x1000, s.owner_hash = 0x390B7DE4u;
    CHECK(X::SceneMatches(s, 0x390B7DE4u));
    CHECK(!X::SceneMatches(s, 0));
    CHECK(!X::SceneMatches(s, 0x12345678u));
    X::SceneSeen t = s;
    t.owner = 0;
    CHECK(!X::SceneMatches(t, 0x390B7DE4u));
    t = s, t.loc_scene = 0x2000;
    CHECK(!X::SceneMatches(t, 0x390B7DE4u));
    t = s, t.owner_hash = 0;
    CHECK(!X::SceneMatches(t, 0x390B7DE4u));
    // bt_rules.hpp's RespawnApplyGate as this module feeds it (sandboxed = redirected and not the real-save stand-in)
    R::RespawnIn in;
    in.bt_respawn = 1, in.role = 2, in.sandboxed = true, in.scene_here = true;
    CHECK(R::RespawnApplyGate(in).call == R::KillCall::apply);
    R::RespawnIn r = in;
    r.sandboxed = false;
    const R::KillVerdict v = R::RespawnApplyGate(r);
    CHECK(v.call == R::KillCall::refuse && v.why && std::strstr(v.why, "this game plays on its own real save") != nullptr);
    r = in, r.role = 1;
    CHECK(R::RespawnApplyGate(r).call == R::KillCall::refuse);
    r = in, r.bt_respawn = 0;
    CHECK(R::RespawnApplyGate(r).call == R::KillCall::refuse);
    r = in, r.scene_here = false;
    CHECK(R::RespawnApplyGate(r).call == R::KillCall::hold);
    r = in, r.not_playing = "a menu";
    CHECK(R::RespawnApplyGate(r).call == R::KillCall::hold);
    r = in, r.loading = true;
    CHECK(R::RespawnApplyGate(r).call == R::KillCall::refuse);
    r = in, r.age_ms = 60001;
    CHECK(R::RespawnApplyGate(r).call == R::KillCall::refuse);
}

void TestSky() {
    X::SkyIn in;
    in.bt_sky = 1, in.checks_ok = true, in.sync_level = 2, in.peer = true, in.partner_trouble = R::kTrHunted, in.partner_ok = true, in.me_ok = true;
    in.dist_m = 40;
    CHECK(X::SkyWhyNot(in) == nullptr);
    for (std::uint8_t k : {R::kTrHunted, R::kTrPulled, R::kTrCaught, R::kTrCatcher}) {
        X::SkyIn t = in;
        t.partner_trouble = k;
        CHECK(X::SkyWhyNot(t) == nullptr);
    }
    X::SkyIn t = in;
    t.partner_trouble = R::kTrClear;
    CHECK(X::SkyWhyNot(t) != nullptr);
    t = in, t.partner_trouble = 0;
    CHECK(X::SkyWhyNot(t) != nullptr);
    t = in, t.bt_sky = 0;
    CHECK(X::SkyWhyNot(t) != nullptr);
    t = in, t.checks_ok = false;
    CHECK(X::SkyWhyNot(t) != nullptr);
    t = in, t.real_save = true;
    CHECK(X::SkyWhyNot(t) != nullptr);  // until bt-sky-sun (kSkyRealSaveOk)
    t = in, t.peer = false;
    CHECK(X::SkyWhyNot(t) != nullptr);
    t = in, t.loading = true;
    CHECK(X::SkyWhyNot(t) != nullptr);
    t = in, t.not_playing = "a cutscene";
    CHECK(X::SkyWhyNot(t) != nullptr);
    t = in, t.carried = true;
    CHECK(X::SkyWhyNot(t) != nullptr);
    t = in, t.dist_m = 150;
    CHECK(X::SkyWhyNot(t) == nullptr);
    t = in, t.dist_m = 150.5;
    CHECK(X::SkyWhyNot(t) != nullptr);
    t = in, t.dist_m = -1;
    CHECK(X::SkyWhyNot(t) != nullptr);
    t = in, t.partner_ok = false;
    CHECK(X::SkyWhyNot(t) != nullptr);
    t = in, t.stopped = true;
    CHECK(X::SkyWhyNot(t) != nullptr);
    CHECK(X::SkyDue(100, 100) && !X::SkyDue(99.9, 100) && X::kSkyPeriodMs == 100.0);
}

void TestCommand() {
    CHECK(X::TestSubOf(0) == X::kSubRespawn && X::TestSubOf(1) == X::kSubSky && X::TestSubOf(2) == X::kSubRealSave);
    CHECK(X::TestSubOf(3) == X::kSubBad && X::TestSubOf(-1) == X::kSubBad && X::TestSubOf(0.5) == X::kSubBad);
    CHECK(X::SkySeconds(0) == 10 && X::SkySeconds(10) == 10 && X::SkySeconds(120) == 120 && X::SkySeconds(121) < 0 && X::SkySeconds(0.5) < 0);
    std::uint32_t f = 1;
    CHECK(X::FieldArg(0, &f) && f == 0);
    CHECK(X::FieldArg(957054436.0, &f) && f == 0x390B7DE4u);
    CHECK(X::FieldArg(4294967295.0, &f) && f == 0xFFFFFFFFu);
    CHECK(!X::FieldArg(-1, &f) && !X::FieldArg(4294967296.0, &f) && !X::FieldArg(1.5, &f));
}

void TestBudget() {
    X::LineBudget b;
    int d = -1;
    for (int i = 0; i < X::LineBudget::kLines; ++i) CHECK(b.Take(1000, &d) && d == 0);
    CHECK(!b.Take(1001, &d) && !b.Take(1002, &d));
    CHECK(b.Take(11000, &d) && d == 2);  // a new window: the two dropped are reported once
    CHECK(b.Take(11001, &d) && d == 0);
}

int main() {
    TestHello();
    TestKillLook();
    TestRespawnCodec();
    TestRespawnSend();
    TestRespawnApply();
    TestSky();
    TestCommand();
    TestBudget();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
