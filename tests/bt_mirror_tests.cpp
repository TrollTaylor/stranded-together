// Round 31 stage 5 (bt_mirror): the pure part's checks (native/mod/bt_mirror_rules.hpp; the shared codecs in bt_rules.hpp). No game.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bt_mirror_rules.hpp"

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

static_assert(st::btrules::mirror::kStage == 5, "the stage");

namespace R = st::btrules;
namespace M = st::btrules::mirror;

// The shared BTS1 header round trip.
void TestHello() {
    R::BtsHello h;
    h.level = 2, h.role = 1, h.caps = R::kCapField | R::kCapGzst2;
    std::uint8_t b[R::kBtsHelloSize] = {};
    const std::size_t n = R::EncodeHello(1, h, b, sizeof(b));
    R::BtsHello o;
    CHECK(n == R::kBtsHelloSize && R::DecodeHello(b, n, o) && o.caps == h.caps && o.level == 2 && o.role == 1);
}

// Round 31 PF fact 2: 0x1 realized, 0x8 dead (the stage-5 flag fix in bt_rules.hpp).
void TestFlags() {
    char t[160];
    R::FlagsText(R::kFlagRealized, t, sizeof(t));
    CHECK(std::strcmp(t, " realized") == 0);
    R::FlagsText(R::kFlagDead, t, sizeof(t));
    CHECK(std::strcmp(t, " dead") == 0);
    CHECK((R::GzBits(R::kFlagRealized, 2) & R::kGzDead) == 0);                       // realized, not dead
    CHECK((R::GzBits(R::kFlagRealized | R::kFlagDead, 2) & R::kGzDead) == R::kGzDead);  // realized and dead
    CHECK((R::GzBits(0x00090A01u, 2) & R::kGzDead) == 0);                             // the census row of a realized idle gazer
    CHECK((R::GzBits(0x00090211u, 2) & R::kGzDead) == 0);                             // kind 2's react bit 0x10 (PF 7), not dead
    CHECK((R::GzBits(0x00011200u, 0) & R::kGzNear) == R::kGzNear && (R::GzBits(0x00011200u, 0) & R::kGzDead) == 0);    // NEAR, state 0
}

M::GateIn Good() {  // a realized copy 40 m away, the partner's gazer sensing her (the mirror leg)
    M::GateIn in;
    in.key = 1, in.sync_level = 2, in.partner_bits = R::kGzNear, in.since_ms = 250, in.resolved = true, in.state = 2, in.comp_ok = true;
    in.local_dist_m = 40, in.trigger_m = 20, in.drops_ok = true;
    return in;
}
void TestGate() {
    M::GateIn in = Good();
    M::GateOut o = M::Gate(in);
    CHECK(o.call == R::GzCall::mirror && !o.turn_only && !o.held);
    // state 0 here: held (the old design's position: this game's player beyond about 50 m)
    in = Good(), in.state = 0, in.comp_ok = false;
    o = M::Gate(in);
    CHECK(o.call == R::GzCall::none && o.held && std::strncmp(o.why, "not realized here", 17) == 0);
    in.mirrored = true, in.mirrored_n = 1;
    o = M::Gate(in);
    CHECK(o.call == R::GzCall::release && !o.held);  // a mirrored one is released, never "held"
    // no component: held
    in = Good(), in.comp_ok = false;
    CHECK(M::Gate(in).held && M::Gate(in).call == R::GzCall::none);
    // the hysteresis: on above trigger + 5, off below trigger + 2
    in = Good(), in.local_dist_m = 24.9;
    CHECK(M::Gate(in).call == R::GzCall::none);
    in.local_dist_m = 25.1;
    CHECK(M::Gate(in).call == R::GzCall::mirror);
    in = Good(), in.mirrored = true, in.mirrored_n = 1, in.local_dist_m = 23;
    CHECK(M::Gate(in).call == R::GzCall::mirror);  // kept between the two
    in.local_dist_m = 21.9;
    o = M::Gate(in);
    CHECK(o.call == R::GzCall::release && std::strstr(o.why, "+ 2 m") != nullptr);
    // quiet 2 s
    in = Good(), in.mirrored = true, in.mirrored_n = 1, in.since_ms = 2001;
    o = M::Gate(in);
    CHECK(o.call == R::GzCall::release && std::strcmp(o.why, "no GZST2 for 2 s") == 0);
    in.since_ms = 1e18;  // no entry at all
    CHECK(M::Gate(in).call == R::GzCall::release);
    // the partner's gazer stops sensing her, or is dead there
    in = Good(), in.partner_bits = R::kGzRealized;
    CHECK(M::Gate(in).call == R::GzCall::none);
    in.partner_bits = R::kGzNear | R::kGzDead;
    CHECK(M::Gate(in).call == R::GzCall::none);
    in.partner_bits = R::kGzReact;
    CHECK(M::Gate(in).call == R::GzCall::mirror);
    // the real save: turn only; the drop hooks not in: turn only
    in = Good(), in.real_save = true;
    o = M::Gate(in);
    CHECK(o.call == R::GzCall::mirror && o.turn_only);
    in = Good(), in.drops_ok = false;
    o = M::Gate(in);
    CHECK(o.call == R::GzCall::mirror && o.turn_only);
    // at most 6
    in = Good(), in.mirrored_n = 6;
    CHECK(M::Gate(in).call == R::GzCall::none);
    in.mirrored_n = 5;
    CHECK(M::Gate(in).call == R::GzCall::mirror);
    in = Good(), in.mirrored = true, in.mirrored_n = 6;
    CHECK(M::Gate(in).call == R::GzCall::mirror);  // a mirrored one counts itself
    // beyond 60 m: held (the game releases a realized gazer there)
    in = Good(), in.local_dist_m = 60.5;
    o = M::Gate(in);
    CHECK(o.call == R::GzCall::none && o.held);
    // dead here, caught here, unknown distance, not resolved, a load, not playing, the key, the level, EmergencyStop
    in = Good(), in.dead = true;
    CHECK(M::Gate(in).call == R::GzCall::none && !M::Gate(in).held);
    in = Good(), in.caught_here = true;
    CHECK(M::Gate(in).call == R::GzCall::none);
    in = Good(), in.local_dist_m = -1;
    CHECK(M::Gate(in).call == R::GzCall::none);
    in = Good(), in.trigger_m = -1;
    CHECK(M::Gate(in).call == R::GzCall::none);
    in = Good(), in.resolved = false;
    CHECK(M::Gate(in).call == R::GzCall::none && !M::Gate(in).held);
    in = Good(), in.loading = true;
    CHECK(M::Gate(in).call == R::GzCall::none);
    in = Good(), in.not_playing = "a menu";
    CHECK(M::Gate(in).call == R::GzCall::none && std::strcmp(M::Gate(in).why, "a menu") == 0);
    in = Good(), in.key = 0, in.mirrored = true;
    CHECK(M::Gate(in).call == R::GzCall::release);
    in = Good(), in.sync_level = 1;
    CHECK(M::Gate(in).call == R::GzCall::none);
    in = Good(), in.emergency = true, in.mirrored = true;
    CHECK(M::Gate(in).call == R::GzCall::release);
}

void TestSender() {
    CHECK(M::SenderAgit(R::kGzNear, true, 0.25f) == 0.25f);
    CHECK(M::SenderAgit(R::kGzNear, true, 3.0f) == 1.0f);
    CHECK(M::SenderAgit(R::kGzNear, false, -1.0f) == 1.0f);  // state 0: no graph, 1.0 while it senses
    CHECK(M::SenderAgit(R::kGzNear, true, std::nanf("")) == 1.0f);
    CHECK(M::SenderAgit(R::kGzRealized, false, -1.0f) == 0.0f);
    CHECK(M::SenderAgit(R::kGzNear | R::kGzDead, false, -1.0f) == 0.0f);
    CHECK(M::SenderWants(R::kGzNear) && M::SenderWants(R::kGzReact) && M::SenderWants(R::kGzCaught));
    CHECK(!M::SenderWants(R::kGzRealized) && !M::SenderWants(R::kGzNear | R::kGzDead) && !M::SenderWants(0));
    CHECK(M::SenderEdges(0, R::kGzReact) == R::kGeFound && M::SenderEdges(R::kGzReact, R::kGzReact) == 0);
    CHECK(M::SenderEdges(R::kGzReact, R::kGzReact | R::kGzCaught) == R::kGeCaught);
    CHECK(M::SenderEdges(0, R::kGzReact | R::kGzCaught) == (R::kGeFound | R::kGeCaught));
    CHECK(M::SenderEdges(R::kGzCaught, 0) == 0);
    CHECK(!M::AgitWanted(127) && M::AgitWanted(128) && M::AgitWanted(255) && R::Unit8(0.5f) >= 128);
    CHECK(M::DeadHere(R::kFlagDead, 1.0f) && M::DeadHere(0, 0.0f) && M::DeadHere(0, -3.0f));
    CHECK(!M::DeadHere(R::kFlagRealized, 1.0f) && !M::DeadHere(0, std::nanf("")));
}

void TestWrites() {
    M::Writes w = M::WritesFor(false, 1, 255);  // the host, agitated
    CHECK(w.kind && w.agit);
    w = M::WritesFor(false, 1, 100);
    CHECK(w.kind && !w.agit);
    w = M::WritesFor(false, 2, 200);  // a sandboxed guest
    CHECK(w.kind && w.agit);
    w = M::WritesFor(true, 2, 255);  // turn only: nothing
    CHECK(!w.kind && !w.agit);
    w = M::WritesFor(true, 1, 255);
    CHECK(!w.kind && !w.agit);
    w = M::WritesFor(false, 1, 255, false);  // kAgitOnHost false: never on the host
    CHECK(w.kind && !w.agit);
    w = M::WritesFor(false, 2, 255, false);
    CHECK(w.kind && w.agit);
}

void TestYaw() {
    const double pi = 3.141592653589793;
    const double a[3] = {0, 0, 0}, east[3] = {10, 0, 5}, north[3] = {0, 10, 0}, near[3] = {0.3, 0.3, 0};
    double y = 9;
    CHECK(M::YawToward(a, east, &y) && std::fabs(y) < 1e-9);
    CHECK(M::YawToward(a, north, &y) && std::fabs(y - pi / 2) < 1e-9);
    CHECK(!M::YawToward(a, near, &y));
    CHECK(std::fabs(M::YawDiff(0, pi / 2) - pi / 2) < 1e-9);
    CHECK(std::fabs(M::YawDiff(170 / M::kDeg, -170 / M::kDeg) - 20 / M::kDeg) < 1e-9);  // the short way round
    CHECK(std::fabs(M::YawDiff(-170 / M::kDeg, 170 / M::kDeg) + 20 / M::kDeg) < 1e-9);
    CHECK(std::fabs(M::YawDt(1000, 0) - 0.033) < 1e-9);
    CHECK(std::fabs(M::YawDt(1100, 1000) - 0.1) < 1e-9);
    CHECK(std::fabs(M::YawDt(5000, 1000) - 0.25) < 1e-9);
    // a 90 deg/s step over 0.1 s is 9 deg
    CHECK(std::fabs(R::kGzYawRateRad * M::YawDt(1100, 1000) * M::kDeg - 9.0) < 1e-6);
    float m[3][3] = {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}};  // forward (row 1) along +x: yaw 0
    CHECK(std::fabs(M::RowYaw(m)) < 1e-6);
    M::RotateRows(m, pi / 4);
    CHECK(std::fabs(M::RowYaw(m) - pi / 4) < 1e-6);
    CHECK(std::fabs(m[2][2] - 1.0f) < 1e-6f && std::fabs(m[2][0]) < 1e-6f);  // up stays up
    M::RotateRows(m, pi / 4);
    CHECK(std::fabs(M::RowYaw(m) - pi / 2) < 1e-6);
    CHECK(std::fabs(m[0][0] * m[1][0] + m[0][1] * m[1][1]) < 1e-6f);  // the rows stay orthogonal
    // op 23's proof: 90 -> 135
    float n[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    CHECK(std::fabs(M::RowYaw(n) * M::kDeg - 90) < 1e-4);
    M::RotateRows(n, 45 / M::kDeg);
    CHECK(std::fabs(M::RowYaw(n) * M::kDeg - 135) < 1e-4);
}

void TestStale() {
    const std::uintptr_t actor = 0x20000000, ent = 0x30000000, back = actor + 0xE8;
    CHECK(M::StaleCheck(true, 7, 7, 2, ent, ent, back, actor) == M::kFresh);
    CHECK(M::StaleCheck(false, 7, 7, 2, ent, ent, back, actor) == M::kStaleId);
    CHECK(M::StaleCheck(true, 8, 7, 2, ent, ent, back, actor) == M::kStaleId);
    CHECK(M::StaleCheck(true, 7, 7, 2, ent, ent, back, 0) == M::kStaleId);
    CHECK(M::StaleCheck(true, 7, 7, 0, ent, ent, back, actor) == M::kStaleState);
    CHECK(M::StaleCheck(true, 7, 7, 2, ent + 0x100, ent, back, actor) == M::kStaleEntity);
    CHECK(M::StaleCheck(true, 7, 7, 2, 0, 0, back, actor) == M::kStaleEntity);
    CHECK(M::StaleCheck(true, 7, 7, 2, ent, ent, back + 8, actor) == M::kStaleBack);
    CHECK(M::StaleCheck(true, 7, 7, 2, ent, ent, 0x10, actor) == M::kStaleBack);
    CHECK(std::strcmp(M::StaleText(M::kStaleState), "state != 2: its entity and component were released") == 0);
    CHECK(M::BackOk(0, 0.0f, 0, 0.0f) && M::BackOk(1, 2.5f, 1, 2.505f));
    CHECK(!M::BackOk(0, 0.0f, 1, 0.0f) && !M::BackOk(0, 0.0f, 0, 10.0f) && !M::BackOk(0, 0.0f, 0, std::nanf("")));
}

void TestDrops() {
    CHECK(M::DropAction(0x6221E6CBu) && M::DropAction(0x5A7E4B1Du));
    CHECK(!M::DropAction(0x68A02F9Fu) && !M::DropAction(0x682DFB3Bu) && !M::DropAction(0x3752F65Eu) && !M::DropAction(0));
    CHECK(M::DropMessage(0x682DFB3Bu) && !M::DropMessage(0x674A8C04u) && !M::DropMessage(0x6221E6CBu));
}

void TestLines() {
    CHECK(M::NextLineMs(0, 0) == 1000 && M::NextLineMs(0, 9000) == 10000 && M::NextLineMs(0, 10000) == 20000);
}

void TestGzst2() {
    R::BtsGzst2 m;
    m.sender_ms = 123456, m.n = 2;
    R::Gz2Entry& a = m.e[0];
    a.field = 0x1234ABCDu, a.loc = 0x40AB1A93u, a.state = 0, a.mode = 3, a.bits = R::GzBits(0x00011200u, 0), a.kind = 2;
    a.action = 0x6221E6CBu, a.blend = R::Unit8(0.4f), a.agit = R::Unit8(M::SenderAgit(a.bits, false, -1)), a.edges = R::kGeFound;
    a.pos[0] = -987.5f, a.pos[1] = 61.25f, a.pos[2] = 120.0f, a.yaw = R::YawCdeg(-2.0), a.dist_dm = R::DistDm(9.3);
    R::Gz2Entry& b = m.e[1];
    b.field = 0x1234ABCDu, b.loc = 0x11112222u, b.state = 2, b.bits = R::kGzReact | R::kGzNear, b.agit = R::Unit8(0.75f);
    b.pos[0] = 1, b.pos[1] = 2, b.pos[2] = 3, b.yaw = 18000, b.dist_dm = R::kGzDistUnknown;
    std::uint8_t buf[R::kBtsMaxSize];
    const std::size_t n = R::EncodeGzst2(9, m, buf, sizeof(buf));
    CHECK(n == R::kBtsGzst2Base + 2 * R::kBtsGzst2Entry);
    R::BtsGzst2 o;
    CHECK(R::DecodeGzst2(buf, n, o) && o.n == 2 && o.sender_ms == 123456);
    CHECK(o.e[0].loc == 0x40AB1A93u && o.e[0].kind == 2 && o.e[0].agit == 255 && o.e[0].edges == R::kGeFound && o.e[0].dist_dm == 93);
    CHECK(o.e[0].action == 0x6221E6CBu && o.e[0].mode == 3 && o.e[0].yaw == R::YawCdeg(-2.0) && o.e[0].pos[1] == 61.25f);
    CHECK(o.e[1].state == 2 && o.e[1].agit == R::Unit8(0.75f) && o.e[1].yaw == 18000 && o.e[1].dist_dm == R::kGzDistUnknown);
    CHECK(M::AgitWanted(o.e[0].agit) && M::SenderWants(o.e[0].bits));
    // the six-entry maximum fits one BTS1 message
    R::BtsGzst2 six;
    six.n = 6;
    CHECK(R::EncodeGzst2(1, six, buf, sizeof(buf)) == R::kBtsGzst2Base + 6 * R::kBtsGzst2Entry);
    six.n = 7;
    CHECK(R::EncodeGzst2(1, six, buf, sizeof(buf)) == 0);
    // a bad entry refuses the message
    m.e[1].bits = 0x40;
    CHECK(R::EncodeGzst2(9, m, buf, sizeof(buf)) == 0);
    CHECK(!R::DecodeGzst2(buf, n - 1, o));
}

// The sender's reach (runs 20261128-012446 / -012712: the host's NEAR bit stayed set 113 m away, so the mirror never ended).
void TestReach() {
    CHECK(M::SenderReachM(20.0) == 30.0 && M::SenderReachM(-1.0) == M::kReachNoTriggerM && M::SenderReachM(std::nan("")) == M::kReachNoTriggerM);
    CHECK(M::SenderInReach(9.3, 9.3, 20.0));                 // the mirror leg: the host 9.3 m from 40AB1A93
    CHECK(M::SenderInReach(30.0, 9.3, 20.0));                // the edge counts
    CHECK(!M::SenderInReach(113.0, 9.3, 20.0));              // the host left: our distance wins over a stale +0x204
    CHECK(!M::SenderInReach(65.0, 65.0, 20.0));              // the round 30 bt-field leave (65 m)
    CHECK(M::SenderInReach(-1.0, 12.0, 20.0));               // our distance unknown: the game's
    CHECK(!M::SenderInReach(-1.0, 45.0, 20.0));
    CHECK(!M::SenderInReach(-1.0, -1.0, 20.0));              // both unknown: not in reach
    CHECK(!M::SenderInReach(std::nan(""), std::nan(""), 20.0));
    CHECK(M::SenderInReach(25.0, -1.0, -1.0) && !M::SenderInReach(31.0, -1.0, -1.0));  // the radius unknown: 30 m
    // the bits acted on: the sensing bits cleared out of reach (no GZST2 entry), kept in reach; realized / dead untouched
    CHECK(M::SenderBits(R::kGzNear, true) == R::kGzNear && M::SenderWants(M::SenderBits(R::kGzNear, true)));
    CHECK(M::SenderBits(R::kGzNear, false) == 0 && !M::SenderWants(M::SenderBits(R::kGzNear, false)));
    CHECK(M::SenderBits(R::kGzNear | R::kGzReact | R::kGzCaught | R::kGzRealized, false) == R::kGzRealized);
    CHECK(M::SenderBits(R::kGzDead | R::kGzNear, false) == R::kGzDead);
    // the return is a new rise: FOUND goes out again
    CHECK(M::SenderEdges(M::SenderBits(R::kGzReact, false), M::SenderBits(R::kGzReact, true)) == R::kGeFound);
}

int main() {
    TestHello();
    TestFlags();
    TestGate();
    TestSender();
    TestReach();
    TestWrites();
    TestYaw();
    TestStale();
    TestDrops();
    TestLines();
    TestGzst2();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
