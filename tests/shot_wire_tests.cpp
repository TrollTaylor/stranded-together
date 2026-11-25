// Shots (native/mod/shot_wire.*), the pure part of ship A: the SFX1 round trip and every decoder rejection, the SEH1 beacon,
// the record flag byte, the replay set, the dedupe window, the hand anchor, the play gate, pulls, the rate cap and the id
// checks. No game, no link.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "shot_wire.hpp"

using namespace st::shotw;

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

constexpr double kPi = 3.14159265358979323846;

Shot MakeShot(std::uint32_t id) {
    Shot s;
    s.shot_id = id;
    s.shooter = kShooterSam;
    s.attack_id = 0x56;
    s.weapon_id = 45;
    s.magazine_id = 7;
    s.origin[0] = 1517.9123, s.origin[1] = 992.0004, s.origin[2] = 173.5;
    const double l = std::sqrt(0.3 * 0.3 + 0.9 * 0.9 + 0.1 * 0.1);
    s.dir[0] = static_cast<float>(0.3 / l), s.dir[1] = static_cast<float>(0.9 / l), s.dir[2] = static_cast<float>(-0.1 / l);
    s.speed = 450.3f;
    s.target_actor = kNoActor;
    s.rec04 = 1, s.rec05 = 2, s.rec06 = 3, s.rec58 = 4, s.rec5A = 5, s.recD0 = 0xE5, s.recD1 = 0x02;
    s.flags = kMuzzle | kHandRel;
    s.age_ms = 17;
    s.rec84 = 0xDEADBEEF;
    s.rec94 = 1.25f;
    s.hand_rel[0] = 0.512f, s.hand_rel[1] = -0.033f, s.hand_rel[2] = 0.041f;
    s.yaw = 1.0f;
    return s;
}

double AngleDeg(const float (&a)[3], const float (&b)[3]) {
    const double d = static_cast<double>(a[0]) * b[0] + static_cast<double>(a[1]) * b[1] + static_cast<double>(a[2]) * b[2];
    return std::acos(d > 1 ? 1 : d < -1 ? -1 : d) * 180.0 / kPi;
}

void TestRoundTrip() {
    for (unsigned n : {1u, kMaxShots}) {
        Frame f;
        f.n = static_cast<std::uint8_t>(n);
        f.flags = 0;
        f.seq = 77, f.sender_ms = 0xFFFFFF00u, f.world = 3;
        for (unsigned i = 0; i < n; ++i) f.shots[i] = MakeShot(1000 + i);
        std::uint8_t buf[kMaxFrameSize];
        const std::size_t size = EncodeShots(f, buf, sizeof(buf));
        CHECK(size == kHeaderSize + kShotSize * n);
        CHECK(LooksLikeShotFrame(buf, size));
        Frame g;
        CHECK(DecodeShots(buf, size, g));
        CHECK(g.n == n && g.seq == 77 && g.sender_ms == 0xFFFFFF00u && g.world == 3);
        for (unsigned i = 0; i < n; ++i) {
            const Shot &a = f.shots[i], &b = g.shots[i];
            CHECK(b.shot_id == a.shot_id && b.shooter == a.shooter && b.attack_id == a.attack_id && b.weapon_id == 45 && b.magazine_id == 7);
            for (int k = 0; k < 3; ++k) CHECK(std::fabs(b.origin[k] - a.origin[k]) <= 0.0005);
            CHECK(AngleDeg(a.dir, b.dir) < 0.01);
            CHECK(std::fabs(b.speed - 450.3f) < 0.06f);
            CHECK(b.rec04 == 1 && b.rec05 == 2 && b.rec06 == 3 && b.rec58 == 4 && b.rec5A == 5 && b.recD0 == 0xE5 && b.recD1 == 2);
            CHECK(b.flags == (kMuzzle | kHandRel) && b.age_ms == 17 && b.rec84 == 0xDEADBEEF && b.rec94 == 1.25f);
            for (int k = 0; k < 3; ++k) CHECK(std::fabs(b.hand_rel[k] - a.hand_rel[k]) <= 0.0006f);
            CHECK(std::fabs(b.yaw - 1.0f) < 0.0002f);
            CHECK(b.target_actor == kNoActor);
        }
    }
    // a negative yaw wraps into 0..2pi
    {
        Frame f;
        f.n = 1;
        f.shots[0] = MakeShot(1);
        f.shots[0].yaw = -1.0f;
        std::uint8_t buf[kMaxFrameSize];
        Frame g;
        CHECK(DecodeShots(buf, EncodeShots(f, buf, sizeof(buf)), g));
        CHECK(std::fabs(g.shots[0].yaw - static_cast<float>(2 * kPi - 1.0)) < 0.0002f);
    }
    // a target actor and its offset (cm)
    {
        Frame f;
        f.n = 1;
        f.shots[0] = MakeShot(1);
        f.shots[0].target_actor = 0x2005;
        f.shots[0].target_off[0] = 0.12f, f.shots[0].target_off[1] = -1.5f, f.shots[0].target_off[2] = 1.33f;
        std::uint8_t buf[kMaxFrameSize];
        Frame g;
        CHECK(DecodeShots(buf, EncodeShots(f, buf, sizeof(buf)), g));
        CHECK(g.shots[0].target_actor == 0x2005 && std::fabs(g.shots[0].target_off[1] + 1.5f) < 0.006f && std::fabs(g.shots[0].target_off[2] - 1.33f) < 0.006f);
    }
}

void TestRejects() {
    Frame f;
    f.n = 2;
    f.shots[0] = MakeShot(1), f.shots[1] = MakeShot(2);
    std::uint8_t buf[kMaxFrameSize];
    const std::size_t size = EncodeShots(f, buf, sizeof(buf));
    CHECK(size);
    Frame g;
    std::uint8_t c[kMaxFrameSize];
    std::memcpy(c, buf, size);
    c[0] = 'X';
    CHECK(!DecodeShots(c, size, g));
    std::memcpy(c, buf, size);
    c[4] = 2;
    CHECK(!DecodeShots(c, size, g));
    std::memcpy(c, buf, size);
    c[5] = 0;
    CHECK(!DecodeShots(c, size, g));
    c[5] = static_cast<std::uint8_t>(kMaxShots + 1);
    CHECK(!DecodeShots(c, size, g));
    CHECK(!DecodeShots(buf, size - 1, g));
    CHECK(!DecodeShots(buf, size + 1, g));
    CHECK(!DecodeShots(nullptr, size, g));
    // a zero direction
    std::memcpy(c, buf, size);
    std::memset(c + kHeaderSize + 24, 0, 6);
    CHECK(!DecodeShots(c, size, g));
    // speed 0
    std::memcpy(c, buf, size);
    c[kHeaderSize + 30] = 0, c[kHeaderSize + 31] = 0;
    CHECK(!DecodeShots(c, size, g));
    // attack id 0 and out of range
    std::memcpy(c, buf, size);
    c[kHeaderSize + 8] = 0, c[kHeaderSize + 9] = 0;
    CHECK(!DecodeShots(c, size, g));
    c[kHeaderSize + 8] = 0x8D, c[kHeaderSize + 9] = 0x01;
    CHECK(!DecodeShots(c, size, g));
    c[kHeaderSize + 8] = 0x95, c[kHeaderSize + 9] = 0x01;  // 0x195: beyond the wide range
    CHECK(!DecodeShots(c, size, g));
    c[kHeaderSize + 8] = 0x91, c[kHeaderSize + 9] = 0x01;  // 0x191 (weapon 92, the MULE fixed gun): decoded since round 21
    CHECK(DecodeShots(c, size, g));
    // a NaN rec94
    std::memcpy(c, buf, size);
    c[kHeaderSize + 56] = 0xFF, c[kHeaderSize + 57] = 0xFF, c[kHeaderSize + 58] = 0xC0, c[kHeaderSize + 59] = 0x7F;
    CHECK(!DecodeShots(c, size, g));
    // the encoder refuses what the decoder would
    Frame bad = f;
    bad.shots[1].speed = 0;
    CHECK(!EncodeShots(bad, buf, sizeof(buf)));
    bad = f;
    bad.shots[0].dir[0] = bad.shots[0].dir[1] = bad.shots[0].dir[2] = 0;
    CHECK(!EncodeShots(bad, buf, sizeof(buf)));
    bad = f;
    bad.shots[0].dir[0] = NAN;
    CHECK(!EncodeShots(bad, buf, sizeof(buf)));
    bad = f;
    bad.shots[0].origin[0] = 3.0e6;
    CHECK(!EncodeShots(bad, buf, sizeof(buf)));
    bad = f;
    bad.n = 0;
    CHECK(!EncodeShots(bad, buf, sizeof(buf)));
    bad.n = static_cast<std::uint8_t>(kMaxShots + 1);
    CHECK(!EncodeShots(bad, buf, sizeof(buf)));
    CHECK(!EncodeShots(f, buf, kHeaderSize + kShotSize));  // too small a buffer
    // a frame of another kind is not a shot frame
    const std::uint8_t sen[] = {'S', 'E', 'N', '1', 1, 1};
    CHECK(!LooksLikeShotFrame(sen, sizeof(sen)));
    CHECK(HasMagic(sen, sizeof(sen), "SEN1") && !HasMagic(sen, sizeof(sen), "SEP1"));
}

void TestBeacon() {
    Beacon b;
    b.parts = kPartFire | kPartMirror;
    b.enemy_count = 150;
    b.ids_fp = 0x40624894, b.guids_fp = 0x55AD7620, b.world = 9;
    b.freeze_mode = 1, b.role = 2;
    std::uint8_t buf[kBeaconSize];
    CHECK(EncodeBeacon(b, buf, sizeof(buf)) == kBeaconSize);
    Beacon c;
    CHECK(DecodeBeacon(buf, sizeof(buf), c));
    CHECK(c.parts == b.parts && c.enemy_count == 150 && c.ids_fp == b.ids_fp && c.guids_fp == b.guids_fp && c.world == 9 && c.freeze_mode == 1 && c.role == 2);
    CHECK(!DecodeBeacon(buf, sizeof(buf) - 1, c));
    std::uint8_t d[kBeaconSize];
    std::memcpy(d, buf, sizeof(d));
    d[4] = 2;
    CHECK(!DecodeBeacon(d, sizeof(d), c));
    std::memcpy(d, buf, sizeof(d));
    d[20] = 5;
    CHECK(!DecodeBeacon(d, sizeof(d), c));
    std::memcpy(d, buf, sizeof(d));
    d[23] = 1;
    CHECK(!DecodeBeacon(d, sizeof(d), c));
    CHECK(!EncodeBeacon(b, buf, kBeaconSize - 1));
    CHECK(BothHave(kPartFire, kPartFire | kPartMirror, kPartFire));
    CHECK(!BothHave(kPartFire, kPartMirror, kPartFire));
    CHECK(!BothHave(0, kPartFire, kPartFire));
}

void TestD0() {
    for (unsigned v = 0; v < 32; ++v) {
        D0Args a;
        a.p8 = v & 1, a.p9 = (v >> 1) & 1, a.p10 = (v >> 2) & 1, a.p11 = (v >> 3) & 1, a.p12 = (v >> 4) & 1;
        const std::uint8_t d0 = PackD0(a);
        CHECK((d0 & kD0NotSettable) == 0);
        const D0Args b = UnpackD0(d0);
        CHECK(b.p8 == a.p8 && b.p9 == a.p9 && b.p10 == a.p10 && b.p11 == a.p11 && b.p12 == a.p12);
    }
    D0Args a;
    a.p8 = 1;
    CHECK(PackD0(a) == 0x01);
    a = D0Args{};
    a.p9 = 1;
    CHECK(PackD0(a) == 0x04);
    a = D0Args{};
    a.p12 = 1;
    CHECK(PackD0(a) == 0x20);
    a = D0Args{};
    a.p11 = 1;
    CHECK(PackD0(a) == 0x40);
    a = D0Args{};
    a.p10 = 1;
    CHECK(PackD0(a) == 0x80);
    // bits 1, 3, 4 are kept from the record, never set by the call
    CHECK(PackD0(D0Args{}, 0xFF) == 0x1A);
    CHECK(PackD0(UnpackD0(0xFF), 0) == 0xE5);
}

void TestReplaySet() {
    ReplaySet s;
    const std::uint64_t r1[] = {0x1000, 0x2000};
    s.Add(r1, 2, 100.0, 1);
    std::uint8_t kind = 0;
    int how = -1, slot = -1;
    CHECK(s.Find(0x1000, 200.0, &kind, &how, &slot) && kind == 1 && how == 0 && slot == 0);
    CHECK(s.Find(0x2020, 200.0, &kind, &how, &slot) && how == 1 && slot == 1);
    CHECK(s.Find(0x0FE0, 200.0, &kind, &how) && how == 2);
    CHECK(!s.Find(0x3000, 200.0));
    CHECK(!s.Find(0, 200.0));
    CHECK(!s.Find(0x1000, 100.0 + kReplayTtlMs + 1));  // expired
    CHECK(s.Find(0x1000, 100.0 + kReplayTtlMs - 1));
    // the ring: kReplaySetSize newer entries push it out
    for (unsigned i = 0; i < kReplaySetSize; ++i) {
        const std::uint64_t r = 0x100000 + i * 0x100;
        s.Add(&r, 1, 150.0, 3);
    }
    CHECK(!s.Find(0x1000, 200.0));
    CHECK(s.Find(0x100000, 200.0, &kind) && kind == 3);
    CHECK(s.Find(0x100000 + 127 * 0x100, 200.0));
    CHECK(s.Newest() == 150.0);
}

// The replay set's room and its per-entry lifetime (round 15 review): a replay never pushes out a live entry.
void TestReplaySetRoom() {
    static ReplaySet s;  // 2048 entries: off the stack
    CHECK(s.HasRoom(0.0));
    const std::uint64_t r = 0x5000;
    for (unsigned i = 0; i < kReplaySetSize; ++i) {
        CHECK(s.HasRoom(100.0));
        s.Add(&r, 1, 100.0, 1);
    }
    CHECK(!s.HasRoom(100.0));                     // full of live entries
    CHECK(!s.HasRoom(100.0 + kReplayTtlMs - 1));
    CHECK(s.HasRoom(100.0 + kReplayTtlMs + 1));   // the oldest has expired
    // the lifetime follows the speed: 150 m / speed + 1 s, 4 s .. 10 s
    CHECK(ReplayTtlMs(150.0f) == kReplayTtlMs);
    CHECK(ReplayTtlMs(450.0f) == kReplayTtlMs);
    CHECK(std::fabs(ReplayTtlMs(20.0f) - 8500.0) < 1e-6);
    CHECK(ReplayTtlMs(10.0f) == kReplayTtlMaxMs);
    CHECK(ReplayTtlMs(0.0f) == kReplayTtlMaxMs);
    ReplaySet t;
    const std::uint64_t slow = 0x7000;
    t.Add(&slow, 1, 1000.0, 1, ReplayTtlMs(20.0f));
    CHECK(t.Find(0x7000, 1000.0 + 8000.0));        // still flying after 8 s
    CHECK(!t.Find(0x7000, 1000.0 + 8600.0));
}

// The bullet category and the attack ids (round 15 review): what the receiver lets reach the Enqueue.
void TestCategory() {
    CHECK(AttackReplayable(0x47));
    CHECK(AttackReplayable(0x5E) && AttackReplayable(0x61));
    CHECK(!AttackReplayable(0x5F) && !AttackReplayable(0x60));  // CreateBullet's projectile-entity path
    CHECK(!AttackReplayable(0) && !AttackReplayable(kMaxAttackId + 1) && AttackReplayable(kMaxAttackId));
    CHECK(MaskD1(0xFF) == 0x01 && MaskD1(0x02) == 0 && MaskD1(0x01) == 1);
    CHECK(CategoryOk(0, 0, false));        // no category: the Enqueue looks nothing up
    CHECK(CategoryOk(0, 1, false));
    CHECK(!CategoryOk(0, 2, false));       // the context path looks the category up even when it is 0
    CHECK(!CategoryOk(71, 0, false));      // a category the registry does not list: list[-1] under the lock
    CHECK(CategoryOk(71, 0, true) && CategoryOk(71, 2, true));
    CHECK(!CategoryOk(200, MaskD1(0xFF), false));
    // the attack from the category (run 20261123-204612: category 71, D1 0x00 -> 0x47)
    CHECK(CategoryAttackOf(0x47, 0x48, 0x00) == 0x47);
    CHECK(CategoryAttackOf(0x47, 0x48, 0x01) == 0x48);
    CHECK(CategoryAttackOf(0x47, 0x48, 0x02) == 0x47);
    CHECK(RecordAttack(0, 71, true, 0x47, 0x48, 0x00) == 0x47);
    CHECK(RecordAttack(0, 71, true, 0x47, 0x48, 0x01) == 0x48);
    CHECK(RecordAttack(0x56, 71, true, 0x47, 0x48, 0x00) == 0x56);  // the record's own attack wins
    CHECK(RecordAttack(0, 71, false, 0x47, 0x48, 0x00) == 0);        // not listed: no shot
    CHECK(RecordAttack(0, 0, true, 0x47, 0x48, 0x00) == 0);
    // never encoded, never decoded
    Frame f;
    f.n = 1;
    f.shots[0] = MakeShot(1);
    std::uint8_t buf[kMaxFrameSize];
    f.shots[0].attack_id = 0x5F;
    CHECK(EncodeShots(f, buf, sizeof(buf)) == 0);
    f.shots[0].attack_id = 0x60;
    CHECK(EncodeShots(f, buf, sizeof(buf)) == 0);
    f.shots[0].attack_id = 0x61;
    const std::size_t size = EncodeShots(f, buf, sizeof(buf));
    CHECK(size);
    Frame g;
    buf[kHeaderSize + 8] = 0x60, buf[kHeaderSize + 9] = 0;
    CHECK(!DecodeShots(buf, size, g));
    buf[kHeaderSize + 8] = 0x5F;
    CHECK(!DecodeShots(buf, size, g));
    buf[kHeaderSize + 8] = 0x61;
    CHECK(DecodeShots(buf, size, g) && g.shots[0].attack_id == 0x61);
}

void TestDedupe() {
    Dedupe d;
    CHECK(d.Accept(10));
    CHECK(!d.Accept(10));
    CHECK(d.Accept(12));
    CHECK(d.Accept(11));   // reordered
    CHECK(!d.Accept(11));
    CHECK(d.Accept(300));
    CHECK(!d.Accept(12));  // older than the window
    CHECK(d.Accept(45));   // 255 behind: inside the window, not seen
    CHECK(!d.Accept(44));  // 256 behind: outside
    CHECK(!d.Accept(300));
    CHECK(d.Accept(299));
    CHECK(d.Accept(300 + 64));
    CHECK(!d.Accept(299));
    CHECK(!d.Accept(300));
    CHECK(d.Accept(301));
    // the u32 wrap
    Dedupe w;
    CHECK(w.Accept(0xFFFFFFFEu));
    CHECK(w.Accept(1));
    CHECK(!w.Accept(0xFFFFFFFEu));
    CHECK(w.Accept(0xFFFFFFFFu));
    CHECK(w.Accept(0));
    CHECK(!w.Accept(0));
}

void TestAnchor() {
    for (double yaw_deg : {0.0, 90.0, 179.0, -179.0, 180.0, -90.0, 33.0}) {
        const float yaw = static_cast<float>(yaw_deg * kPi / 180.0);
        const double hand[3] = {100.0, 200.0, 50.0};
        const double origin[3] = {100.4, 199.7, 50.2};
        const double delta[3] = {origin[0] - hand[0], origin[1] - hand[1], origin[2] - hand[2]};
        float rel[3];
        ToYawFrame(delta, yaw, rel);
        double back[3];
        FromYawFrame(rel, yaw, hand, back);
        for (int k = 0; k < 3; ++k) CHECK(std::fabs(back[k] - origin[k]) < 1e-5);
    }
    // forward along the heading is +x in the yaw frame
    {
        const float fwd[3] = {0, 1, 0};
        const float yaw = YawOf(fwd);
        CHECK(std::fabs(yaw - static_cast<float>(kPi / 2)) < 1e-6f);
        const double d[3] = {0, 2, 0};
        float rel[3];
        ToYawFrame(d, yaw, rel);
        CHECK(std::fabs(rel[0] - 2) < 1e-5f && std::fabs(rel[1]) < 1e-5f);
    }
    // the body turned: the same hand-relative offset follows it
    {
        Shot s = MakeShot(1);
        s.flags = kHandRel;
        s.hand_rel[0] = 0.5f, s.hand_rel[1] = 0, s.hand_rel[2] = 0;
        s.origin[0] = 10.5, s.origin[1] = 0, s.origin[2] = 1;
        const double hand[3] = {10, 0, 1};
        double out[3];
        CHECK(AnchoredOrigin(s, true, hand, 0.0f, 1.5, out));
        CHECK(std::fabs(out[0] - 10.5) < 1e-6 && std::fabs(out[1]) < 1e-6);
        CHECK(AnchoredOrigin(s, true, hand, static_cast<float>(kPi / 2), 1.5, out));
        CHECK(std::fabs(out[0] - 10.0) < 1e-6 && std::fabs(out[1] - 0.5) < 1e-6);
        // too far from the absolute origin: the absolute one
        const double far_hand[3] = {20, 0, 1};
        CHECK(!AnchoredOrigin(s, true, far_hand, 0.0f, 1.5, out));
        CHECK(out[0] == 10.5);
        // no hand, or no hand_rel in the shot
        CHECK(!AnchoredOrigin(s, false, hand, 0.0f, 1.5, out));
        s.flags = 0;
        CHECK(!AnchoredOrigin(s, true, hand, 0.0f, 1.5, out));
    }
}

void TestPlayGate() {
    CHECK(PlayGate(false, 0, 1000) == Gate::now_no_body);
    CHECK(PlayGate(true, 900, 1000) == Gate::hold);
    CHECK(PlayGate(true, 1000, 1000) == Gate::fire);
    CHECK(PlayGate(true, 1400, 1000) == Gate::fire);
    CHECK(PlayGate(true, 1501, 1000) == Gate::late);
    CHECK(PlayGate(true, 1000, 3100) == Gate::now_ahead);
    CHECK(PlayGate(true, 1200, 3100) == Gate::hold);
    // unwrap
    CHECK(UnwrapNear(5000u, 4000.0) == 5000.0);
    CHECK(UnwrapNear(0xFFFFFF00u, 4294967300.0 + 10) == 4294967040.0);
    CHECK(UnwrapNear(10u, 4294967290.0) == 4294967306.0);
}

void TestPulls() {
    PullGroup p;
    CHECK(p.Next(1000, 45));
    CHECK(!p.Next(1005, 45));   // the same pull (pellets)
    CHECK(!p.Next(1015, 45));
    CHECK(p.Next(1040, 45));    // the next pull
    CHECK(p.Next(1041, 46));    // another weapon
    CHECK(p.Next(1000, 46));    // time went back
}

void TestRateCap() {
    RateCap c;
    for (unsigned i = 0; i < kCapShots; ++i) CHECK(c.Take(1000 + i));
    CHECK(!c.Take(1050));
    CHECK(c.Take(1100.5));  // the first left the window
    CHECK(!c.Take(1100.6));
    CHECK(c.Take(1250));
}

// Round 21 (P1c 3b): the wide attack range, gated per partner and by the game's own registry.
void TestAttackWide() {
    CHECK(AttackReplayable(0x57) && AttackReplayable(0x18C));
    CHECK(!AttackReplayable(0x191));          // narrow: an old partner
    CHECK(AttackReplayable(0x191, true));     // wide
    CHECK(AttackReplayable(0x194, true) && !AttackReplayable(0x195, true));
    for (bool wide : {false, true}) {
        CHECK(!AttackReplayable(0, wide));
        CHECK(!AttackReplayable(kAttackPlaceholder, wide));
        CHECK(!AttackReplayable(kAttackEntityA, wide) && !AttackReplayable(kAttackEntityB, wide));
    }
    std::uint8_t listed[64] = {};
    auto set = [&](std::uint16_t a) { listed[a >> 3] |= static_cast<std::uint8_t>(1u << (a & 7)); };
    // no registry read: the narrow range alone (the old behaviour)
    CHECK(AttackAllowed(0x57, true, nullptr) && !AttackAllowed(0x191, true, nullptr));
    // read: the narrow range as before whatever it lists (review rv-correctness R8); above 0x18C only what it lists, within the
    // wire bound
    CHECK(AttackAllowed(0x57, true, listed) && AttackAllowed(0x58, true, listed) && AttackAllowed(0x18C, true, listed));
    CHECK(!AttackAllowed(0x191, true, listed));
    set(0x57), set(0x191), set(0x18D), set(0x195), set(0x5F);
    CHECK(AttackAllowed(0x57, true, listed) && AttackAllowed(0x191, true, listed));
    CHECK(!AttackAllowed(0x191, false, listed));  // listed, but the partner is narrow
    CHECK(!AttackAllowed(0x18D, true, listed) && !AttackAllowed(0x195, true, listed) && !AttackAllowed(0x5F, true, listed));
    CHECK(AttackAllowed(0x58, true, listed) && !AttackAllowed(0x192, true, listed));
    CHECK(!AttackAllowed(0, true, listed) && !AttackAllowed(0x60, true, nullptr));
}

void TestIds() {
    IdCheck c;
    CHECK(ValidId(45, c) == 0);  // not listed
    c.listed = true;
    CHECK(ValidId(45, c) == 45);
    c.child_ok = false;
    CHECK(ValidId(45, c) == 0);  // a weapon without its child list
    c.child_ok = true;
    CHECK(ValidId(0, c) == 0);
}

// Round 16: the drop at the bullet, the receiver classes, the aimed test's checks.
// P2 (docs/enemy-sync.md 9): a bullet on a frozen mirror: her own round passes to Entity OnHit only while the hits are captured
// and the receiver is an entity receiver whose vt+0x10 is OnHit; everything else is dropped as before.
void TestMirrorBulletRule() {
    CHECK(MirrorBulletRule(false, true, true, true) == MirrorBullet::pass_capture);    // hers, hits on, an entity receiver
    CHECK(MirrorBulletRule(false, true, true, false) == MirrorBullet::drop_no_entity); // hers on a part with no entity receiver
    CHECK(MirrorBulletRule(false, true, false, true) == MirrorBullet::drop);           // hits off: dropped as in P1a
    CHECK(MirrorBulletRule(true, true, true, true) == MirrorBullet::drop);             // a replay (never hers)
    CHECK(MirrorBulletRule(true, false, true, true) == MirrorBullet::drop);
    CHECK(MirrorBulletRule(false, false, true, true) == MirrorBullet::drop);           // this world's round (another shooter)
    CHECK(MirrorBulletRule(false, false, false, false) == MirrorBullet::drop);
}

void TestBulletRule() {
    CHECK(BulletRule(true, true, false, false) == BulletVerdict::drop);   // a replay into anything but our Sam
    CHECK(BulletRule(true, true, true, false) == BulletVerdict::pass);    // our Sam: the Sam filter decides
    CHECK(BulletRule(true, true, false, true) == BulletVerdict::pass);    // the hidden-run control
    CHECK(BulletRule(true, false, false, false) == BulletVerdict::pass);  // a reused reference on a bullet of the game's own
    CHECK(BulletRule(false, true, false, false) == BulletVerdict::pass);  // not a replay (the game's scripted CreateBullet)
    CHECK(BulletRule(false, false, false, false) == BulletVerdict::pass);
}

// A hit on our Sam (or an NPC) is a replay's only with a replay-set match AND the bullet system's dummy as its attacker (review
// rv-behaviour 1): a reused reference on one of the game's own rounds is kept; an unknown dummy (0) matches nothing.
void TestReplayHit() {
    const std::uint64_t dummy = 0x6130A65E000ull;
    CHECK(ReplayHit(true, dummy, dummy));
    CHECK(ReplayHit(true, dummy + 0x20, dummy));   // the entity / its reference
    CHECK(ReplayHit(true, dummy - 0x20, dummy));
    CHECK(!ReplayHit(true, 0x6130A65F000ull, dummy));  // a MULE's round reusing a replay's reference
    CHECK(!ReplayHit(false, dummy, dummy));            // the dummy alone is never a reason (the game's scripted bullets)
    CHECK(!ReplayHit(true, dummy, 0));                 // the dummy unknown (after a load)
    CHECK(!ReplayHit(true, 0, 0));
    CHECK(!ReplayHit(true, 0, dummy));
}

// A load clears the set: nothing matches, every slot is free again.
void TestReplaySetClear() {
    static ReplaySet s;
    const std::uint64_t r[2] = {0x1000, 0x2000};
    for (unsigned i = 0; i < kReplaySetSize; ++i) s.Add(r, 2, 100.0, kReplayKindPartner);
    CHECK(s.Find(0x1000, 200.0) && !s.HasRoom(200.0));
    s.Clear();
    CHECK(!s.Find(0x1000, 200.0) && !s.Find(0x2020, 200.0) && s.HasRoom(200.0) && s.head == 0);
    std::uint8_t kind = 0;
    s.Add(r, 1, 300.0, kReplayKindEnemy);
    CHECK(s.Find(0x1000, 400.0, &kind) && kind == kReplayKindEnemy && !s.Find(0x2000, 400.0));
}

void TestHitClass() {
    CHECK(ClassifyHit("VehicleEntity") == HitClass::vehicle);
    CHECK(ClassifyHit("DSVehicleMotorbike") == HitClass::vehicle);
    CHECK(ClassifyHit("DSVehicleHitchCarrierMover") == HitClass::vehicle);
    CHECK(ClassifyHit("DSHoveringCartEntity") == HitClass::vehicle);
    CHECK(ClassifyHit("DSBaggage2Entity") == HitClass::cargo);
    CHECK(ClassifyHit("DSPlacableEntity") == HitClass::structure);
    CHECK(ClassifyHit("DSLadderMover") == HitClass::structure);
    CHECK(ClassifyHit("Destructibility") == HitClass::structure);
    CHECK(ClassifyHit("DSGazerGameActor") == HitClass::bt);
    CHECK(ClassifyHit("DSCatcherEntity") == HitClass::bt);
    CHECK(ClassifyHit("Humanoid") == HitClass::npc);
    CHECK(ClassifyHit("DSMuleEntity") == HitClass::npc);
    CHECK(ClassifyHit("DSControlledEntity") == HitClass::npc);  // a MULE's entity (run 20261124-092442)
    CHECK(ClassifyHit("Entity") == HitClass::other);            // a part: its parent decides (shot_sync.cpp)
    CHECK(ClassifyHit("DSPlayerEntity") == HitClass::player);
    CHECK(ClassifyHit("DSSharedSubstanceBreakableWeak") == HitClass::other);
    CHECK(ClassifyHit("Property") == HitClass::other);  // "rope" in lower case is not a rope
    CHECK(ClassifyHit("") == HitClass::other);
    CHECK(ClassifyHit(nullptr) == HitClass::other);
    CHECK(std::strcmp(HitClassName(HitClass::structure), "structure") == 0);
    CHECK(std::strcmp(HitClassName(static_cast<HitClass>(99)), "other") == 0);
}

void TestAimed() {
    CHECK(ValueUnchanged(1000.0f, 1000.0f, 0.5f));
    CHECK(ValueUnchanged(1000.0f, 999.8f, 0.5f));
    CHECK(!ValueUnchanged(1000.0f, 990.0f, 0.5f));
    CHECK(!ValueUnchanged(std::nanf(""), 1.0f, 0.5f));
    const double t[3] = {10, 0, 5}, from[3] = {20, 0, 0};
    double o[3];
    AimedOrigin(t, from, 6, 1, o);
    CHECK(std::fabs(o[0] - 16) < 1e-9 && std::fabs(o[1]) < 1e-9 && std::fabs(o[2] - 6) < 1e-9);
    AimedOrigin(t, t, 6, 0, o);  // from the target itself: any horizontal side, never a NaN
    CHECK(std::isfinite(o[0]) && std::isfinite(o[1]) && std::fabs(std::hypot(o[0] - 10, o[1]) - 6) < 1e-9);
}

}  // namespace

int main() {
    TestRoundTrip();
    TestRejects();
    TestBeacon();
    TestD0();
    TestReplaySet();
    TestReplaySetRoom();
    TestCategory();
    TestDedupe();
    TestAnchor();
    TestPlayGate();
    TestPulls();
    TestRateCap();
    TestIds();
    TestAttackWide();
    TestBulletRule();
    TestMirrorBulletRule();
    TestReplayHit();
    TestReplaySetClear();
    TestHitClass();
    TestAimed();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
