// Vehicle hits (native/mod/tackle_rules.*), the pure part of M4: the path per kind, the gate, the sphere, the hit filter,
// the damage cap, the rate limit, the crash sphere, the STH1 round trip and every decoder rejection, the partner capsule's
// vehicle rules. No game, no link.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "tackle_rules.hpp"
#include "vehicle_wire.hpp"

using namespace st::trules;

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

bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

void TestPaths() {
    // auto: every kind takes the direct path (only our Sam is hit, D4-3): the truck with its MULE slot 0x157, trike and
    // roadster 0x158
    CHECK(PathFor(2, true, true, true, 0) == Path::direct);
    CHECK(AttackOf(Path::direct, true) == kAttackMule);
    CHECK(PathFor(3, false, true, true, 0) == Path::direct);
    CHECK(PathFor(1, false, true, true, 0) == Path::direct);
    CHECK(AttackOf(Path::direct, false) == kAttackPlayer);
    CHECK(AttackOf(Path::bracket, false) == kAttackPlayer);
    CHECK(AttackOf(Path::bracket, true) == kAttackPlayer);  // the bracket always selects slot +0xA00
    // a truck without the MULE slot (flag 0x8000000 at its add-to-world): the direct path with 0x158
    CHECK(PathFor(2, false, true, true, 0) == Path::direct);
    // the direct path's checks failed: the bracket everywhere
    CHECK(PathFor(2, true, false, true, 0) == Path::bracket);
    // only the direct path passed: it serves every kind (0x158 without the MULE slot)
    CHECK(PathFor(3, false, true, false, 0) == Path::direct);
    CHECK(AttackOf(Path::direct, false) == kAttackPlayer);
    CHECK(PathFor(2, true, false, false, 0) == Path::none);
    CHECK(AttackOf(Path::none, true) == 0);
    // forced (tests)
    CHECK(PathFor(3, false, true, true, 1) == Path::direct);
    CHECK(PathFor(2, true, true, true, 2) == Path::bracket);
    CHECK(PathFor(2, true, false, true, 1) == Path::none);
    CHECK(PathFor(2, true, true, false, 2) == Path::none);
    // no kind
    CHECK(PathFor(0, true, true, true, 0) == Path::none);
    CHECK(PathFor(4, true, true, true, 0) == Path::none);
    CHECK(std::strcmp(PathName(Path::direct), "direct") == 0);
}

GateIn Pass() {
    GateIn g;
    g.feature_on = true;
    g.friendly_fire = true;
    g.have_vehicle = true;
    g.partner_drives = true;
    g.local_ok = g.local_awake = g.local_on_foot = true;
    g.dist_m = 6;
    g.speed_kmh = 25;
    g.now_ms = 10000;
    g.hold_off_until_ms = 0;
    g.sam_to_sphere_m = 2.0;
    g.sphere_radius_m = 1.8;
    g.path = Path::direct;
    return g;
}

void TestGate() {
    CHECK(Gate(Pass()) == Block::ok);
    GateIn g = Pass();
    g.feature_on = false;
    CHECK(Gate(g) == Block::off);
    g = Pass(), g.friendly_fire = false;
    CHECK(Gate(g) == Block::friendly_fire);
    g = Pass(), g.have_vehicle = false;
    CHECK(Gate(g) == Block::no_vehicle);
    g = Pass(), g.partner_drives = false;
    CHECK(Gate(g) == Block::not_driven);
    g = Pass(), g.local_ok = false;
    CHECK(Gate(g) == Block::no_local);
    g = Pass(), g.local_awake = false;
    CHECK(Gate(g) == Block::asleep);
    g = Pass(), g.local_on_foot = false;
    CHECK(Gate(g) == Block::not_on_foot);
    g = Pass(), g.carried = true;
    CHECK(Gate(g) == Block::carried);
    g = Pass(), g.game_blocked = true;
    CHECK(Gate(g) == Block::game_state);
    // 50 m: the game's own MULE gate
    g = Pass(), g.dist_m = 50.0;
    CHECK(Gate(g) == Block::ok);
    g.dist_m = 50.01;
    CHECK(Gate(g) == Block::too_far);
    g.dist_m = std::nan("");
    CHECK(Gate(g) == Block::too_far);
    // speed over 15 km/h (the game's gate is strict)
    g = Pass(), g.speed_kmh = 15.0;
    CHECK(Gate(g) == Block::slow);
    g.speed_kmh = 15.01;
    CHECK(Gate(g) == Block::ok);
    g.speed_kmh = std::nan("");
    CHECK(Gate(g) == Block::slow);
    // the hold-off after a hit
    g = Pass(), g.hold_off_until_ms = 10000.5;
    CHECK(Gate(g) == Block::hold_off);
    g.hold_off_until_ms = 10000;
    CHECK(Gate(g) == Block::ok);
    // the path
    g = Pass(), g.path = Path::none;
    CHECK(Gate(g) == Block::no_path);
    // near the sphere: within its radius + 2 m
    g = Pass(), g.sam_to_sphere_m = 3.8;
    CHECK(Gate(g) == Block::ok);
    g.sam_to_sphere_m = 3.81;
    CHECK(Gate(g) == Block::not_near);
    // the bracket (no callback of ours: the game's sphere hits whatever it catches): only with our Sam's centre inside it
    g = Pass(), g.path = Path::bracket, g.sam_to_sphere_m = 1.8;
    CHECK(Gate(g) == Block::ok);
    g.sam_to_sphere_m = 1.81;
    CHECK(Gate(g) == Block::not_near);
    g.sam_to_sphere_m = 3.0;
    CHECK(Gate(g) == Block::not_near);
    g.sam_to_sphere_m = std::nan("");
    CHECK(Gate(g) == Block::not_near);
    // order: friendly fire off wins over everything after it
    g = Pass(), g.friendly_fire = false, g.carried = true, g.speed_kmh = 0;
    CHECK(Gate(g) == Block::friendly_fire);
    for (unsigned b = 0; b < static_cast<unsigned>(Block::count); ++b) CHECK(std::strcmp(BlockName(static_cast<Block>(b)), "?") != 0);
    CHECK(std::strcmp(BlockName(Block::count), "?") == 0);
}

void TestSphere() {
    // identity rotation: right x, forward y, up z
    const double pos[3] = {100, 200, 10};
    const float rows[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float local[3] = {0.0f, 2.5f, 1.0f};
    double c[3];
    SphereCentre(pos, rows, local, 5.0f, c);
    CHECK(Near(c[0], 100) && Near(c[1], 202.5) && Near(c[2], 11));
    // reversing: forward component negated (the sphere at the back)
    SphereCentre(pos, rows, local, -0.5f, c);
    CHECK(Near(c[0], 100) && Near(c[1], 197.5) && Near(c[2], 11));
    // speed 0 counts as forward (the game's compare is "less than 0")
    SphereCentre(pos, rows, local, 0.0f, c);
    CHECK(Near(c[1], 202.5));
    // yawed 90 deg: forward = -x, right = +y
    const float yaw[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
    SphereCentre(pos, yaw, local, 3.0f, c);
    CHECK(Near(c[0], 97.5) && Near(c[1], 200) && Near(c[2], 11));
    const float side[3] = {1.0f, 0.0f, 0.0f};
    SphereCentre(pos, yaw, side, 3.0f, c);
    CHECK(Near(c[0], 100) && Near(c[1], 201));
}

void TestHitFilter() {
    const double root[3] = {10, 20, 5};
    double sc[3];
    SamCentre(root, sc);
    CHECK(Near(sc[2], 6.0));
    const double at[3] = {10, 20, 6};
    CHECK(HitIsSams(at, sc));
    const double feet[3] = {10, 20, 5};  // 1 m below the centre
    CHECK(HitIsSams(feet, sc));
    const double cargo[3] = {10, 20.3, 7.4};  // a tall cargo stack on his back
    CHECK(HitIsSams(cargo, sc));
    const double edge[3] = {11.5, 20, 6};
    CHECK(HitIsSams(edge, sc));
    const double npc[3] = {11.51, 20, 6};
    CHECK(!HitIsSams(npc, sc));
    const double far[3] = {30, 20, 6};
    CHECK(!HitIsSams(far, sc));
    const double bad[3] = {std::nan(""), 20, 6};
    CHECK(!HitIsSams(bad, sc));
    const double inf[3] = {10, HUGE_VAL, 6};
    CHECK(!HitIsSams(inf, sc));
}

void TestCap() {
    CHECK(CapAmount(10000.0f) == kHitAmount);
    CHECK(CapAmount(100.0f) == 100.0f);
    CHECK(CapAmount(100.5f) == kHitAmount);
    CHECK(CapAmount(40.0f) == 40.0f);
    CHECK(CapAmount(0.0f) == 0.0f);
    CHECK(CapAmount(-1.0f) == -1.0f);  // the attack's own damage (100 / 400) then
    CHECK(std::isnan(CapAmount(std::nanf(""))));
    CHECK(BracketSpeed(29.0f) < 0);
    CHECK(BracketSpeed(30.0f) < 0);  // the game's compare: 10000 only above 30
    CHECK(BracketSpeed(30.1f) == kBracketSpeedKmh);
    CHECK(BracketSpeed(80.0f) == kBracketSpeedKmh);
    static_assert(kBracketSpeedKmh > kMinSpeedKmh && kBracketSpeedKmh <= kLethalSpeedKmh, "the held speed still attacks, below the lethal amount");
}

void TestCrashSphere() {
    const double root[3] = {0, 0, 0};
    const double veh[3] = {3, 4, 0.5};  // 5 m away horizontally
    double c[3];
    CrashSphere(root, veh, c);
    CHECK(Near(c[0], 0.6 * 3 / 5) && Near(c[1], 0.6 * 4 / 5) && Near(c[2], kSamCentreUpM));
    // the vehicle right above (no horizontal direction): centred on Sam
    const double over[3] = {0, 0, 3};
    CrashSphere(root, over, c);
    CHECK(Near(c[0], 0) && Near(c[1], 0) && Near(c[2], kSamCentreUpM));
    // the crash sphere always holds Sam's centre (radius 1 m > 0.6 m)
    double sc[3];
    SamCentre(root, sc);
    const double d = std::sqrt((c[0] - sc[0]) * (c[0] - sc[0]) + (c[1] - sc[1]) * (c[1] - sc[1]) + (c[2] - sc[2]) * (c[2] - sc[2]));
    CHECK(d <= kCrashSphereM);
    static_assert(kCrashSphereBackM < kCrashSphereM, "the crash sphere holds Sam's centre");
}

void TestLimiter() {
    HitLimiter l;
    CHECK(!l.InPass() && l.Passes() == 0 && l.HoldOffUntil() == 0);
    l.Called(1000);
    l.Called(1016);
    l.Called(1033);
    CHECK(l.InPass() && l.CallsThisPass() == 3 && l.Passes() == 1 && l.PassStartMs() == 1000);
    l.HitSeen(1040);
    CHECK(!l.InPass() && l.Hits() == 1 && Near(l.HoldOffUntil(), 1040 + kHoldOffMs));
    // the gate holds off until then
    GateIn g = Pass();
    g.now_ms = 2000, g.hold_off_until_ms = l.HoldOffUntil();
    CHECK(Gate(g) == Block::hold_off);
    g.now_ms = 2540;
    CHECK(Gate(g) == Block::ok);
    // a second pass
    l.Called(2540);
    CHECK(l.InPass() && l.CallsThisPass() == 1 && l.Passes() == 2);
    l.HitSeen(2560);
    CHECK(l.Hits() == 2);
    // a pass without a hit (Sam left the sphere) stays open; the next call continues it
    l.Called(5000);
    l.Called(5016);
    CHECK(l.Passes() == 3 && l.CallsThisPass() == 2);
    // ... but not for ever: no call for kPassIdleMs ends it without a hit (a later hit is not this pass's)
    CHECK(!l.Expire(5016 + kPassIdleMs));
    CHECK(l.InPass());
    CHECK(l.Expire(5016 + kPassIdleMs + 1));
    CHECK(!l.InPass() && l.Hits() == 2);
    CHECK(!l.Expire(9000));
    l.Called(9000);
    CHECK(l.InPass() && l.Passes() == 4 && l.CallsThisPass() == 1);
    l.Reset();
    CHECK(l.Passes() == 0 && l.Hits() == 0 && !l.InPass());
}

HitMsg Hit() {
    HitMsg m;
    m.from = 1;
    m.kind = 2;
    m.path = 1;
    m.crash = 0;
    m.vehicle_id = 0x020005;
    m.attack = kAttackMule;
    m.world = 7;
    m.sender_ms = 123456;
    m.speed_kmh = 24.5f;
    m.health_before = 1000.0f;
    m.health_after = 881.0f;
    return m;
}
bool Same(const HitMsg& a, const HitMsg& b) {
    return a.from == b.from && a.kind == b.kind && a.path == b.path && a.crash == b.crash && a.vehicle_id == b.vehicle_id && a.attack == b.attack &&
           a.world == b.world && a.sender_ms == b.sender_ms && a.speed_kmh == b.speed_kmh && a.health_before == b.health_before &&
           a.health_after == b.health_after;
}

void TestMessage() {
    std::uint8_t buf[64];
    HitMsg m = Hit();
    const std::size_t n = EncodeHit(m, buf, sizeof(buf));
    CHECK(n == kMessageSize && n == 40);
    HitMsg o;
    CHECK(DecodeHit(buf, n, o) && Same(m, o));
    std::uint8_t type = 0;
    CHECK(st::vwire::PeekType(buf, n, &type) && type == kSvmType);
    static_assert(kSvmType != 48, "the ride messages' type");
    // the trike, the bracket, a crash
    m.kind = 3, m.path = 2, m.attack = kAttackPlayer, m.crash = 1, m.from = 0;
    CHECK(EncodeHit(m, buf, sizeof(buf)) == kMessageSize && DecodeHit(buf, kMessageSize, o) && Same(m, o));
    // the encoder refuses what the decoder would
    m = Hit(), m.kind = 0;
    CHECK(EncodeHit(m, buf, sizeof(buf)) == 0);
    m = Hit(), m.kind = 4;
    CHECK(EncodeHit(m, buf, sizeof(buf)) == 0);
    m = Hit(), m.from = 2;
    CHECK(EncodeHit(m, buf, sizeof(buf)) == 0);
    m = Hit(), m.path = 3;
    CHECK(EncodeHit(m, buf, sizeof(buf)) == 0);
    m = Hit(), m.crash = 2;
    CHECK(EncodeHit(m, buf, sizeof(buf)) == 0);
    m = Hit(), m.attack = 0x159;
    CHECK(EncodeHit(m, buf, sizeof(buf)) == 0);
    m = Hit(), m.speed_kmh = std::nanf("");
    CHECK(EncodeHit(m, buf, sizeof(buf)) == 0);
    m = Hit(), m.health_after = HUGE_VALF;
    CHECK(EncodeHit(m, buf, sizeof(buf)) == 0);
    m = Hit();
    CHECK(EncodeHit(m, buf, kMessageSize - 1) == 0);
    CHECK(EncodeHit(m, nullptr, 64) == 0);
    // decoder rejections
    CHECK(EncodeHit(m, buf, sizeof(buf)) == kMessageSize);
    CHECK(!DecodeHit(buf, kMessageSize - 1, o));
    CHECK(!DecodeHit(buf, kMessageSize + 1, o));
    CHECK(!DecodeHit(nullptr, kMessageSize, o));
    auto bad = [&](std::size_t at, std::uint8_t v) {
        std::uint8_t c[64];
        std::memcpy(c, buf, kMessageSize);
        c[at] = v;
        HitMsg x;
        return !DecodeHit(c, kMessageSize, x);
    };
    CHECK(bad(0, 'X'));                       // SVM1 magic
    CHECK(bad(4, 48));                        // another type
    CHECK(bad(kSvmHeaderSize + 0, 'X'));      // STH1 magic
    CHECK(bad(kSvmHeaderSize + 4, 2));        // from
    CHECK(bad(kSvmHeaderSize + 5, 0));        // kind
    CHECK(bad(kSvmHeaderSize + 6, 9));        // path
    CHECK(bad(kSvmHeaderSize + 7, 5));        // crash
    CHECK(bad(kSvmHeaderSize + 12, 0x59));    // attack 0x159
    {
        std::uint8_t c[64];
        std::memcpy(c, buf, kMessageSize);
        const float nan = std::nanf("");
        std::memcpy(c + kSvmHeaderSize + 24, &nan, 4);
        HitMsg x;
        CHECK(!DecodeHit(c, kMessageSize, x));
    }
    // the decoder leaves `out` alone on a rejection
    HitMsg keep = Hit();
    keep.vehicle_id = 0xABCDEF;
    std::uint8_t c[64];
    std::memcpy(c, buf, kMessageSize);
    c[kSvmHeaderSize + 5] = 0;
    CHECK(!DecodeHit(c, kMessageSize, keep) && keep.vehicle_id == 0xABCDEF);
}

VehicleBox Box(double x, double y, double speed) {
    VehicleBox b;
    b.c[0] = x, b.c[1] = y, b.c[2] = 1.0;
    b.half[0] = 1.2f, b.half[1] = 3.0f, b.half[2] = 1.5f;
    b.speed_mps = speed;
    return b;
}

void TestCapsule() {
    const double body[3] = {0, 0, 0};
    // inside the box: 0
    VehicleBox b = Box(0, 0, 0);
    CHECK(Near(DistanceToBox(b, body), 0.0));
    // 2 m to the right of a box 1.2 m wide: 0.8 m
    b = Box(2.0, 0, 0);
    CHECK(Near(DistanceToBox(b, body), 0.8, 1e-6));
    // a corner
    b = Box(4.2, 7.0, 0);  // 3 m in x from the side, 4 m in y from the end
    const double p0[3] = {0, 0, 1.0};
    CHECK(Near(DistanceToBox(b, p0), 5.0, 1e-6));
    // rotated 90 deg: half extents swap in the world
    b = Box(0, 4.0, 0);
    const float yaw[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
    std::memcpy(b.rot, yaw, sizeof(yaw));
    const double p1[3] = {0, 0, 1.0};
    CHECK(Near(DistanceToBox(b, p1), 4.0 - 1.2, 1e-6));

    // a moving vehicle within 5 m: off
    VehicleBox boxes[2] = {Box(6.0, 0, 3.0), Box(30, 0, 0)};
    CHECK(CapsuleVehicleBlock(body, boxes, 2, false) != nullptr);  // 4.8 m from its side, 3 m/s
    boxes[0].speed_mps = 1.0;  // not over 1 m/s
    CHECK(CapsuleVehicleBlock(body, boxes, 2, false) == nullptr);
    boxes[0].speed_mps = 1.01;
    CHECK(CapsuleVehicleBlock(body, boxes, 2, false) != nullptr);
    boxes[0] = Box(6.3, 0, 8.0);  // 5.1 m: far enough
    CHECK(CapsuleVehicleBlock(body, boxes, 2, false) == nullptr);
    // a parked vehicle overlapping the capsule refuses the enable only
    boxes[0] = Box(1.5, 0, 0);  // its side 0.3 m from the body point
    CHECK(CapsuleVehicleBlock(body, boxes, 2, false) == nullptr);
    CHECK(CapsuleVehicleBlock(body, boxes, 2, true) != nullptr);
    boxes[0] = Box(1.7, 0, 0);  // 0.5 m: clear of the 0.4 m capsule
    CHECK(CapsuleVehicleBlock(body, boxes, 2, true) == nullptr);
    // a vehicle above the body point (the capsule is tall): checked at 1 m and 2 m up
    boxes[0] = Box(0, 0, 0);
    boxes[0].c[2] = 3.2;  // its bottom at 1.7 m
    CHECK(CapsuleVehicleBlock(body, boxes, 1, true) != nullptr);
    boxes[0].c[2] = 4.0;  // its bottom at 2.5 m: over the 2 m point by 0.5
    CHECK(CapsuleVehicleBlock(body, boxes, 1, true) == nullptr);
    CHECK(CapsuleVehicleBlock(body, nullptr, 3, true) == nullptr);
    CHECK(CapsuleVehicleBlock(body, boxes, 0, true) == nullptr);

    // the moving flag's hysteresis: on over 1 m/s, off at 0.6 m/s or below
    CHECK(!Moving(false, 1.0));
    CHECK(Moving(false, 1.01));
    CHECK(Moving(true, 0.61));
    CHECK(!Moving(true, 0.6));
    CHECK(Moving(true, std::nan("")) && !Moving(false, std::nan("")));
    // a moving box at 0.8 m/s still counts (it was over 1 m/s), a stopped one does not
    boxes[0] = Box(6.0, 0, 0.8);
    boxes[0].moving = true;
    CHECK(CapsuleVehicleBlock(body, boxes, 1, false) != nullptr);
    boxes[0].moving = false;
    CHECK(CapsuleVehicleBlock(body, boxes, 1, false) == nullptr);

    // speeds from the drive module's polls: a vehicle at 1.2 m/s sampled every 100 ms poll reads 1.2 m/s every time (no
    // swing between 0 and twice its speed), and keeps the moving flag
    double sp = 0;
    bool have = false, moving = false;
    double place[3] = {0, 0, 0};
    for (int i = 0; i < 20; ++i) {
        const double next[3] = {place[0] + 0.12, 0, 0};
        sp = SpeedSample(sp, have, place, next, 100.0);
        have = true;
        moving = Moving(moving, sp);
        std::memcpy(place, next, sizeof(place));
        CHECK(Near(sp, 1.2, 1e-9));
        CHECK(moving);
    }
    // a bad sample keeps the estimate
    const double far[3] = {1000, 0, 0};
    CHECK(Near(SpeedSample(1.2, true, place, far, 100.0), 1.2));
    CHECK(Near(SpeedSample(1.2, true, place, place, 0.0), 1.2));
    CHECK(Near(SpeedSample(0.0, false, place, place, 0.0), 0.0));
    // halves: from 2 m/s to standing
    CHECK(Near(SpeedSample(2.0, true, place, place, 100.0), 1.0));
}
}  // namespace

int main() {
    TestPaths();
    TestGate();
    TestSphere();
    TestHitFilter();
    TestCap();
    TestCrashSphere();
    TestLimiter();
    TestMessage();
    TestCapsule();
    std::printf("tackle_rules_tests: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
