// Enemy sync P1a (native/mod/enemy_wire.*): the SEN1 / SEP1 round trips and every decoder rejection, and the enemy SFX1 frame
// (shot_wire: actor-id shooters, a mixed frame refused). No game, no link.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "enemy_wire.hpp"
#include "shot_wire.hpp"

using namespace st;

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
constexpr std::uint32_t kMule = 0x2000u | 37u;  // type 1, slot 37

ewire::Enemy MakeEnemy(unsigned i) {
    ewire::Enemy e;
    e.actor_id = 0x2000u | (i + 1);
    e.owner_epoch = 1;
    e.state = static_cast<std::uint8_t>(ewire::kRealized | (i & 1 ? ewire::kPosed : 0));
    e.phase = 0xFF;
    e.flags = i & 1 ? ewire::kMoving : 0;
    e.pos[0] = 1517.9123 + i, e.pos[1] = 992.0004 - i * 0.5, e.pos[2] = 172.0711;
    e.yaw = static_cast<float>(-kPi + 0.1 * i);
    e.vel[0] = 1.11f, e.vel[1] = -0.37f, e.vel[2] = 0.02f;
    return e;
}

double YawErrDeg(float a, float b) {
    double d = std::fmod(static_cast<double>(a) - b + 3 * kPi, 2 * kPi) - kPi;
    return std::fabs(d) * 180.0 / kPi;
}

void TestSenRoundTrip() {
    for (unsigned n : {1u, 32u}) {
        ewire::EnemyFrame f;
        f.n = static_cast<std::uint8_t>(n);
        f.seq = 77, f.sender_ms = 0xFFFFFF00u, f.gen_tag = 0;
        for (unsigned i = 0; i < n; ++i) f.e[i] = MakeEnemy(i);
        std::uint8_t buf[ewire::kSenMaxSize];
        const std::size_t size = ewire::EncodeEnemies(f, buf, sizeof(buf));
        CHECK(size == ewire::kSenHeader + ewire::kSenEntry * n);
        ewire::EnemyFrame g;
        CHECK(ewire::DecodeEnemies(buf, size, g));
        CHECK(g.n == n && g.seq == 77 && g.sender_ms == 0xFFFFFF00u);
        for (unsigned i = 0; i < n; ++i) {
            const ewire::Enemy& a = f.e[i];
            const ewire::Enemy& b = g.e[i];
            CHECK(b.actor_id == a.actor_id && b.state == a.state && b.flags == a.flags && b.phase == 0xFF && b.owner_epoch == 1);
            for (int k = 0; k < 3; ++k) {
                CHECK(std::fabs(b.pos[k] - a.pos[k]) <= 0.0005);
                CHECK(std::fabs(b.vel[k] - a.vel[k]) <= 0.005f);
            }
            CHECK(YawErrDeg(a.yaw, b.yaw) < 0.006);
        }
    }
    static_assert(ewire::kSenMaxSize == 916, "SEN1 max");
    static_assert(ewire::kSenMaxSize <= 1024 && ewire::kSepMaxSize <= 1024, "SEN1 / SEP1 fit one combat FRAME (link.cpp kCombatFrameMax)");
}

void TestSenRejects() {
    ewire::EnemyFrame f;
    f.n = 2;
    f.e[0] = MakeEnemy(0), f.e[1] = MakeEnemy(1);
    std::uint8_t buf[ewire::kSenMaxSize + 8];
    const std::size_t size = ewire::EncodeEnemies(f, buf, sizeof(buf));
    CHECK(size > 0);
    ewire::EnemyFrame g;
    std::uint8_t c[sizeof(buf)];
    std::memcpy(c, buf, size);
    c[0] = 'X';
    CHECK(!ewire::DecodeEnemies(c, size, g));  // magic
    std::memcpy(c, buf, size);
    c[4] = 2;
    CHECK(!ewire::DecodeEnemies(c, size, g));  // version
    std::memcpy(c, buf, size);
    c[5] = 0;
    CHECK(!ewire::DecodeEnemies(c, size, g));  // n 0
    c[5] = 33;
    CHECK(!ewire::DecodeEnemies(c, ewire::kSenHeader + 33 * ewire::kSenEntry, g));  // n 33
    CHECK(!ewire::DecodeEnemies(buf, size - 1, g));                                  // size -1
    CHECK(!ewire::DecodeEnemies(buf, size + 1, g));                                  // size +1
    // a non-enemy actor id (type 2), and the none id
    std::memcpy(c, buf, size);
    c[ewire::kSenHeader + 1] = 0x40;  // id 0x4001
    CHECK(!ewire::DecodeEnemies(c, size, g));
    ewire::EnemyFrame h = f;
    h.e[1].actor_id = 0x1FFFFF;
    CHECK(ewire::EncodeEnemies(h, buf, sizeof(buf)) == 0);
    h = f;
    h.e[0].pos[0] = 2.5e6;  // > 2000 km
    CHECK(ewire::EncodeEnemies(h, buf, sizeof(buf)) == 0);
    h = f;
    h.e[0].pos[2] = std::nan("");
    CHECK(ewire::EncodeEnemies(h, buf, sizeof(buf)) == 0);
    h = f;
    h.e[0].vel[1] = std::nanf("");
    CHECK(ewire::EncodeEnemies(h, buf, sizeof(buf)) == 0);
    h = f;
    h.n = 0;
    CHECK(ewire::EncodeEnemies(h, buf, sizeof(buf)) == 0);
    // a big coordinate on the wire (the i32 mm reaches +-2147 km): refused
    std::memcpy(c, buf, size);
    const std::int32_t big = 2100000000;  // 2100 km in mm
    std::memcpy(c + ewire::kSenHeader + 8, &big, 4);
    CHECK(!ewire::DecodeEnemies(c, size, g));
    // the velocity clamp
    h = f;
    h.e[0].vel[0] = 1000.0f;
    CHECK(ewire::EncodeEnemies(h, buf, sizeof(buf)) > 0);
    CHECK(ewire::DecodeEnemies(buf, ewire::kSenHeader + 2 * ewire::kSenEntry, g) && g.e[0].vel[0] <= 327.01f);
}

wire::JointFrame MakeJoints() {
    wire::JointFrame j;
    j.joint_count = 77;
    j.layout_hash = wire::kSamLayoutHash;
    j.flags = wire::kJointsValid;
    for (unsigned k = 0; k < 77; ++k) {
        const double a = 0.01 * k;
        j.q[k] = wire::Normalize(wire::Quat{static_cast<float>(std::sin(a)), 0.1f, static_cast<float>(-0.05 * std::cos(a)), static_cast<float>(std::cos(a))});
    }
    j.t_count = 2;
    j.t_joint[0] = 0, j.t_joint[1] = 1;
    j.t[0][0] = 0, j.t[0][1] = 0, j.t[0][2] = 0;
    j.t[1][0] = 0.01f, j.t[1][1] = -0.02f, j.t[1][2] = 1.02f;
    return j;
}

void TestSep() {
    ewire::EnemyPose p;
    p.slot = 5;
    p.actor_id = kMule;
    p.sender_ms = 123456;
    p.joints = MakeJoints();
    std::uint8_t buf[ewire::kSepMaxSize];
    const std::size_t size = ewire::EncodeEnemyPose(p, buf, sizeof(buf));
    CHECK(size == ewire::kSepHeader + wire::JointFrameSize(77, 2));
    CHECK(size <= 1024);  // one combat FRAME (link.cpp kCombatFrameMax)
    ewire::EnemyPose q;
    CHECK(ewire::DecodeEnemyPose(buf, size, q));
    CHECK(q.slot == 5 && q.actor_id == kMule && q.sender_ms == 123456 && !q.foreign_layout && q.joints.joint_count == 77);
    CHECK(wire::AngleBetween(q.joints.q[40], p.joints.q[40]) < 0.1f);
    CHECK(q.joints.t_count == 2 && std::fabs(q.joints.t[1][2] - 1.02f) < 0.001f);
    // a foreign layout: decoded, flagged
    ewire::EnemyPose f = p;
    f.joints.layout_hash = 0x12345678u;
    const std::size_t fs = ewire::EncodeEnemyPose(f, buf, sizeof(buf));
    CHECK(fs > 0 && ewire::DecodeEnemyPose(buf, fs, q) && q.foreign_layout);
    // rejects
    ewire::EnemyPose bad = p;
    bad.slot = 8;
    CHECK(ewire::EncodeEnemyPose(bad, buf, sizeof(buf)) == 0);
    bad = p;
    bad.actor_id = 0x4001;
    CHECK(ewire::EncodeEnemyPose(bad, buf, sizeof(buf)) == 0);
    const std::size_t ok = ewire::EncodeEnemyPose(p, buf, sizeof(buf));
    std::uint8_t c[ewire::kSepMaxSize];
    std::memcpy(c, buf, ok);
    c[3] = '2';
    CHECK(!ewire::DecodeEnemyPose(c, ok, q));
    std::memcpy(c, buf, ok);
    c[6] = 1;
    CHECK(!ewire::DecodeEnemyPose(c, ok, q));  // reserved
    std::memcpy(c, buf, ok);
    c[5] = 9;
    CHECK(!ewire::DecodeEnemyPose(c, ok, q));  // slot
    CHECK(!ewire::DecodeEnemyPose(buf, ok - 1, q));
    CHECK(!ewire::DecodeEnemyPose(buf, ewire::kSepHeader, q));
}

void TestEnemyShots() {
    shotw::Frame f;
    f.flags = shotw::kEnemyShooters;
    f.n = 2;
    for (unsigned i = 0; i < 2; ++i) {
        shotw::Shot& s = f.shots[i];
        s.shot_id = 10 + i;
        s.shooter = kMule + i;
        s.attack_id = 0x56;
        s.weapon_id = 12, s.magazine_id = 34;
        s.origin[0] = 1500, s.origin[1] = 990, s.origin[2] = 173;
        s.dir[0] = 1, s.dir[1] = 0, s.dir[2] = 0;
        s.speed = 300;
        s.flags = shotw::kMuzzle | shotw::kHandRel;
        s.hand_rel[0] = 0.4f;
    }
    std::uint8_t buf[shotw::kMaxFrameSize];
    const std::size_t size = shotw::EncodeShots(f, buf, sizeof(buf));
    CHECK(size == shotw::kHeaderSize + 2 * shotw::kShotSize);
    shotw::Frame g;
    CHECK(shotw::DecodeShots(buf, size, g) && g.flags == shotw::kEnemyShooters && g.shots[1].shooter == kMule + 1 && g.shots[0].weapon_id == 12 &&
          g.shots[0].magazine_id == 34);
    // a Sam shooter in an enemy frame, an enemy in a players' frame: refused both ways
    shotw::Frame m = f;
    m.shots[1].shooter = shotw::kShooterSam;
    CHECK(shotw::EncodeShots(m, buf, sizeof(buf)) == 0);
    m = f;
    m.flags = 0;
    CHECK(shotw::EncodeShots(m, buf, sizeof(buf)) == 0);
    // patched on the wire
    const std::size_t ok = shotw::EncodeShots(f, buf, sizeof(buf));
    std::uint8_t c[shotw::kMaxFrameSize];
    std::memcpy(c, buf, ok);
    c[6] = 0;  // the frame flag cleared: enemy ids in a players' frame
    CHECK(!shotw::DecodeShots(c, ok, g));
    CHECK(shotw::ShooterOk(shotw::kEnemyShooters, kMule) && !shotw::ShooterOk(shotw::kEnemyShooters, shotw::kShooterSam) &&
          !shotw::ShooterOk(shotw::kEnemyShooters, shotw::kNoActor) && shotw::ShooterOk(0, shotw::kShooterSam) && !shotw::ShooterOk(0, kMule));
}


// ---------------------------------------------------------------- SCB1 (P2)
ewire::Hit MakeHit(unsigned i) {
    ewire::Hit h;
    h.key = kMule + i;
    h.hit_seq = 100 + i;
    h.attack = 0x47;
    h.kind = ewire::kKindBullet;
    h.flags = ewire::kHitAttackerSam | ewire::kHitPartValid | ewire::kHitNormalValid;
    h.serial = 0x1234 + i;
    h.b12E = 3, h.b180 = 1, h.depth = static_cast<std::uint8_t>(i & 3);
    h.v130 = 7, h.v184 = 0x3F800000u, h.v188 = 9;
    h.part = i == 0 ? 0xFFFFFFFFu : 12;
    h.amount = 25.5f, h.mult = 1.0f, h.a20 = 1.0f;
    h.point[0] = 0.25f, h.point[1] = -0.5f, h.point[2] = 1.2f + 0.01f * static_cast<float>(i);
    h.normal[0] = 0.6f, h.normal[1] = -0.8f, h.normal[2] = 0.0f;
    h.impulse[0] = 12.34f, h.impulse[1] = -500.0f, h.impulse[2] = 0.5f;  // -500 clamps to -327
    h.t_ms = 123456u + i;
    h.her_dist_m = 9.87f;
    return h;
}
bool HitRefused(void (*patch)(ewire::Hit&)) {
    ewire::ScbHeader hd;
    ewire::Hit h = MakeHit(1);
    patch(h);
    std::uint8_t buf[ewire::kScbMaxSize];
    return ewire::EncodeHits(hd, &h, 1, buf, sizeof(buf)) == 0;
}
void TestScbHits() {
    ewire::ScbHeader hd;
    hd.guids_fp = 0x55AD7620u, hd.seq = 7;
    for (unsigned n : {1u, 12u}) {
        ewire::Hit in[ewire::kHitMax];
        for (unsigned i = 0; i < n; ++i) in[i] = MakeHit(i);
        std::uint8_t buf[ewire::kScbMaxSize];
        const std::size_t size = ewire::EncodeHits(hd, in, n, buf, sizeof(buf));
        CHECK(size == ewire::kScbHeader + n * ewire::kHitSize);
        ewire::ScbHeader g;
        ewire::Hit out[ewire::kHitMax];
        CHECK(ewire::DecodeHits(buf, size, g, out));
        CHECK(g.type == ewire::kScbHit && g.n == n && g.guids_fp == 0x55AD7620u && g.seq == 7);
        bool same = true;
        for (unsigned i = 0; i < n; ++i) {
            const ewire::Hit& a = in[i];
            const ewire::Hit& b = out[i];
            same = same && a.key == b.key && a.hit_seq == b.hit_seq && a.attack == b.attack && a.kind == b.kind && a.flags == b.flags && a.serial == b.serial &&
                   a.b12E == b.b12E && a.b180 == b.b180 && a.depth == b.depth && a.v130 == b.v130 && a.v184 == b.v184 && a.v188 == b.v188 && a.part == b.part &&
                   a.amount == b.amount && a.mult == b.mult && a.a20 == b.a20 && a.t_ms == b.t_ms;
            for (int k = 0; k < 3; ++k) {
                same = same && a.point[k] == b.point[k] && std::fabs(a.normal[k] - b.normal[k]) < 1e-4f;
                const float want = a.impulse[k] < -327.0f ? -327.0f : a.impulse[k];
                same = same && std::fabs(want - b.impulse[k]) < 0.006f;
            }
            same = same && std::fabs(b.her_dist_m - 9.9f) < 0.051f;
        }
        CHECK(same);
    }
    ewire::Hit one[1] = {MakeHit(0)};
    std::uint8_t buf[ewire::kScbMaxSize];
    ewire::ScbHeader g;
    ewire::Hit out[ewire::kHitMax];
    // encoder refusals: n 0 / 13, a non-enemy key, attacks 0 / 0x180 / 0x18E, NaN, a point beyond 8 m, a bad kind
    CHECK(ewire::EncodeHits(hd, one, 0, buf, sizeof(buf)) == 0);
    ewire::Hit many[13];
    for (unsigned i = 0; i < 13; ++i) many[i] = MakeHit(i);
    CHECK(ewire::EncodeHits(hd, many, 13, buf, sizeof(buf)) == 0);
    CHECK(HitRefused([](ewire::Hit& h) { h.key = 0x4000u | 3u; }));
    CHECK(HitRefused([](ewire::Hit& h) { h.attack = 0; }));
    CHECK(HitRefused([](ewire::Hit& h) { h.attack = 0x180; }));
    CHECK(HitRefused([](ewire::Hit& h) { h.attack = 0x18E; }));
    CHECK(HitRefused([](ewire::Hit& h) { h.amount = std::nanf(""); }));
    CHECK(HitRefused([](ewire::Hit& h) { h.point[1] = 8.01f; }));
    CHECK(HitRefused([](ewire::Hit& h) { h.kind = 0; }));
    CHECK(HitRefused([](ewire::Hit& h) { h.kind = 5; }));
    CHECK(!HitRefused([](ewire::Hit& h) { h.attack = 0x18D; }));
    // decoder refusals on the wire bytes
    const std::size_t ok = ewire::EncodeHits(hd, one, 1, buf, sizeof(buf));
    CHECK(ok == ewire::kScbHeader + ewire::kHitSize && ewire::DecodeHits(buf, ok, g, out));
    auto bad = [&](std::size_t at, std::uint8_t v) {
        std::uint8_t c[ewire::kScbMaxSize];
        std::memcpy(c, buf, ok);
        c[at] = v;
        ewire::ScbHeader h2;
        return !ewire::DecodeHits(c, ok, h2, out);
    };
    CHECK(bad(0, 'X'));               // magic
    CHECK(bad(4, 9));                 // type
    CHECK(bad(4, ewire::kScbPhase));  // PHASE is reserved
    CHECK(bad(5, 2));                 // version
    CHECK(bad(6, 0));                 // n 0
    CHECK(bad(6, 2));                 // n vs the size
    CHECK(bad(7, 1));                 // reserved
    CHECK(bad(16 + 2, 0x10));         // the key's type byte: not an enemy
    CHECK(bad(16 + 19, 1));           // reserved
    CHECK(bad(16 + 78, 1));           // reserved
    CHECK(!ewire::DecodeHits(buf, ok - 1, g, out));  // size
    {
        std::uint8_t big[ewire::kScbMaxSize + 4] = {};
        std::memcpy(big, buf, ok);
        CHECK(!ewire::DecodeHits(big, ok + 1, g, out));
    }
    {
        // attack 0x180 and 0x247 written raw: refused
        std::uint8_t c[ewire::kScbMaxSize];
        std::memcpy(c, buf, ok);
        c[16 + 8] = 0x80, c[16 + 9] = 0x01;
        ewire::ScbHeader h2;
        CHECK(!ewire::DecodeHits(c, ok, h2, out));
        c[16 + 8] = 0x47, c[16 + 9] = 0x02;
        CHECK(!ewire::DecodeHits(c, ok, h2, out));
        c[16 + 8] = 0x00, c[16 + 9] = 0x00;
        CHECK(!ewire::DecodeHits(c, ok, h2, out));
    }
    {
        // a point of 9 m, and a NaN, written raw: refused
        std::uint8_t c[ewire::kScbMaxSize];
        std::memcpy(c, buf, ok);
        const float nine = 9.0f;
        std::memcpy(c + 16 + 48, &nine, 4);
        ewire::ScbHeader h2;
        CHECK(!ewire::DecodeHits(c, ok, h2, out));
        const float nan = std::nanf("");
        std::memcpy(c + 16 + 48, &nan, 4);
        CHECK(!ewire::DecodeHits(c, ok, h2, out));
    }
}
void TestScbOthers() {
    ewire::ScbHeader hd;
    hd.guids_fp = 0xABCDEF01u, hd.seq = 99;
    std::uint8_t buf[2048];
    // HITRES, n 64
    ewire::HitRes r[ewire::kHitResMax];
    for (unsigned i = 0; i < ewire::kHitResMax; ++i)
        r[i] = ewire::HitRes{kMule + i, 1000 + i, static_cast<std::uint8_t>(i % (ewire::kHitResultMax + 1)), static_cast<std::uint8_t>(i % 4), static_cast<std::uint8_t>(i % 6)};
    std::size_t size = ewire::EncodeHitRes(hd, r, ewire::kHitResMax, buf, sizeof(buf));
    CHECK(size == ewire::kScbHeader + ewire::kHitResMax * ewire::kHitResSize);
    ewire::ScbHeader g;
    ewire::HitRes ro[ewire::kHitResMax];
    CHECK(ewire::DecodeHitRes(buf, size, g, ro) && g.n == ewire::kHitResMax && g.seq == 99);
    bool same = true;
    for (unsigned i = 0; i < ewire::kHitResMax; ++i)
        same = same && ro[i].key == r[i].key && ro[i].hit_seq == r[i].hit_seq && ro[i].result == r[i].result && ro[i].owner == r[i].owner && ro[i].defeat == r[i].defeat;
    CHECK(same);
    CHECK(ewire::EncodeHitRes(hd, r, 65, buf, sizeof(buf)) == 0);
    ewire::HitRes rb = r[0];
    rb.result = ewire::kHitResultMax + 1;
    CHECK(ewire::EncodeHitRes(hd, &rb, 1, buf, sizeof(buf)) == 0);
    size = ewire::EncodeHitRes(hd, r, 1, buf, sizeof(buf));
    buf[16 + 8] = ewire::kHitResultMax + 1;  // a result beyond the last
    CHECK(!ewire::DecodeHitRes(buf, size, g, ro));
    // OUTCOME, n 32
    ewire::Outcome o[ewire::kOutcomeMax];
    for (unsigned i = 0; i < ewire::kOutcomeMax; ++i) {
        o[i].key = kMule + i, o[i].defeat = static_cast<std::uint8_t>(i % 6), o[i].cause = static_cast<std::uint8_t>(i % 4);
        o[i].ver = static_cast<std::uint16_t>(65530 + i);
        o[i].state = static_cast<std::uint8_t>(1 + i % 5), o[i].downbits = static_cast<std::uint8_t>(i & 0x3F), o[i].phase = 0xFF;
        o[i].health = 1000.0f - static_cast<float>(i), o[i].stamina = -3.5f, o[i].last_seq = i, o[i].epoch = static_cast<std::uint8_t>(250 + i);
    }
    size = ewire::EncodeOutcomes(hd, o, ewire::kOutcomeMax, buf, sizeof(buf));
    CHECK(size == ewire::kScbHeader + ewire::kOutcomeMax * ewire::kOutcomeSize);
    ewire::Outcome oo[ewire::kOutcomeMax];
    CHECK(ewire::DecodeOutcomes(buf, size, g, oo) && g.n == ewire::kOutcomeMax);
    same = true;
    for (unsigned i = 0; i < ewire::kOutcomeMax; ++i)
        same = same && oo[i].key == o[i].key && oo[i].defeat == o[i].defeat && oo[i].cause == o[i].cause && oo[i].ver == o[i].ver && oo[i].state == o[i].state &&
               oo[i].downbits == o[i].downbits && oo[i].phase == 0xFF && oo[i].health == o[i].health && oo[i].stamina == o[i].stamina && oo[i].last_seq == o[i].last_seq &&
               oo[i].epoch == o[i].epoch;
    CHECK(same);
    ewire::Outcome ob = o[0];
    ob.defeat = 6;
    CHECK(ewire::EncodeOutcomes(hd, &ob, 1, buf, sizeof(buf)) == 0);
    ob = o[0];
    ob.cause = 4;
    CHECK(ewire::EncodeOutcomes(hd, &ob, 1, buf, sizeof(buf)) == 0);
    CHECK(ewire::EncodeOutcomes(hd, o, 33, buf, sizeof(buf)) == 0);
    // RESYNC: n 0, the header alone
    size = ewire::EncodeResync(hd, buf, sizeof(buf));
    CHECK(size == ewire::kScbHeader);
    CHECK(ewire::DecodeScbHeader(buf, size, g) && g.type == ewire::kScbResync && g.n == 0 && g.guids_fp == 0xABCDEF01u);
    ewire::Hit hh[ewire::kHitMax];
    CHECK(!ewire::DecodeHits(buf, size, g, hh));     // the wrong decoder for a type
    CHECK(!ewire::DecodeOutcomes(buf, size, g, oo));
    buf[6] = 1;
    CHECK(!ewire::DecodeScbHeader(buf, size, g));    // RESYNC with n 1
    // SEN1's defeat bits
    CHECK(ewire::DefeatBits(4) == 0x10 && ewire::DefeatOfState(static_cast<std::uint8_t>(ewire::kRealized | ewire::DefeatBits(5) | ewire::kPosed)) == 5);
}
// P1b: OWN / OWNACK / OWNRESET (SCB1 types 3, 9, 10) and SEN1's ownership bits
void TestScbOwn() {
    ewire::ScbHeader hd;
    hd.guids_fp = 0x55AD7620u, hd.seq = 9;
    ewire::Own o[ewire::kOwnMax];
    for (unsigned i = 0; i < ewire::kOwnMax; ++i) {
        o[i].gen = 7, o[i].id = 0x2000u | (i + 1), o[i].epoch = static_cast<std::uint16_t>(65530 + i), o[i].owner = i & 1 ? 1 : 2, o[i].reason = i & 1 ? 9 : 1;
        o[i].phase = i == 3 ? 0xFF : static_cast<std::uint8_t>(i % 6), o[i].flags = static_cast<std::uint8_t>(ewire::kOwnHasPlace | (i & 2 ? ewire::kOwnDamaged : 0));
        o[i].pos[0] = 1564.061 + i, o[i].pos[1] = 1012.084, o[i].pos[2] = -171.5;
        o[i].yaw = static_cast<float>(-kPi + 0.2 * i);
        o[i].vel[0] = 1.5f, o[i].vel[1] = -0.25f, o[i].vel[2] = 0;
        o[i].health = i ? 812.5f : std::nanf("");
    }
    std::uint8_t buf[ewire::kScbHeader + ewire::kOwnMax * ewire::kOwnSize];
    std::size_t size = ewire::EncodeOwns(hd, ewire::kScbOwn, o, ewire::kOwnMax, buf, sizeof(buf));
    CHECK(size == ewire::kScbHeader + ewire::kOwnMax * ewire::kOwnSize);
    ewire::ScbHeader g;
    ewire::Own oo[ewire::kOwnMax];
    CHECK(ewire::DecodeOwns(buf, size, g, oo) && g.type == ewire::kScbOwn && g.n == ewire::kOwnMax && g.guids_fp == 0x55AD7620u && g.seq == 9);
    bool same = true;
    for (unsigned i = 0; i < ewire::kOwnMax; ++i) {
        same = same && oo[i].gen == 7 && oo[i].id == o[i].id && oo[i].epoch == o[i].epoch && oo[i].owner == o[i].owner && oo[i].reason == o[i].reason &&
               oo[i].phase == o[i].phase && oo[i].flags == o[i].flags && oo[i].result == 0 && oo[i].refusal == 0;
        for (int k = 0; k < 3; ++k) same = same && std::fabs(oo[i].pos[k] - o[i].pos[k]) <= 0.0006 && std::fabs(oo[i].vel[k] - o[i].vel[k]) <= 0.006f;
        same = same && YawErrDeg(oo[i].yaw, o[i].yaw) < 0.01;
        same = same && (i ? oo[i].health == o[i].health : std::isnan(oo[i].health));
    }
    CHECK(same);
    CHECK(oo[1].epoch == 65531 && ewire::DecodeOwns(buf, size, g, oo));
    // an OWNACK: reason 6 and the result word
    ewire::Own a = o[0];
    a.reason = 6, a.result = ewire::kOwnRefused, a.refusal = 8;
    size = ewire::EncodeOwns(hd, ewire::kScbOwnAck, &a, 1, buf, sizeof(buf));
    CHECK(size == ewire::kScbHeader + ewire::kOwnSize && buf[16 + 14] == 8 && buf[16 + 15] == 2);
    CHECK(ewire::DecodeOwns(buf, size, g, oo) && g.type == ewire::kScbOwnAck && oo[0].result == 2 && oo[0].refusal == 8);
    // encoder refusals
    auto enc_bad = [&](std::uint8_t type, void (*f)(ewire::Own&)) {
        ewire::Own x = o[1];
        if (type == ewire::kScbOwnAck) x.reason = 6;
        f(x);
        return ewire::EncodeOwns(hd, type, &x, 1, buf, sizeof(buf)) == 0;
    };
    CHECK(enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.id = 0x4000u | 5; }));       // not an enemy
    CHECK(enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.owner = 3; }));
    CHECK(enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.owner = 0; }));
    CHECK(enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.reason = 0; }));
    CHECK(enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.reason = 13; }));
    CHECK(enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.reason = 6; }));            // the ack's reason on an OWN
    CHECK(enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.result = 1; }));            // a result on an OWN
    CHECK(enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.phase = 6; }));
    CHECK(!enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.phase = 0xFF; }));
    CHECK(enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.flags = 8; }));
    CHECK(enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.pos[0] = std::nan(""); }));
    CHECK(enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.pos[2] = 2.0e6; }));
    CHECK(enc_bad(ewire::kScbOwn, [](ewire::Own& x) { x.yaw = std::nanf(""); }));
    CHECK(enc_bad(ewire::kScbOwnAck, [](ewire::Own& x) { x.reason = 1; }));
    CHECK(enc_bad(ewire::kScbOwnAck, [](ewire::Own& x) { x.result = 3; }));
    CHECK(enc_bad(ewire::kScbOwnAck, [](ewire::Own& x) { x.refusal = 10; }));
    CHECK(!enc_bad(ewire::kScbOwnAck, [](ewire::Own& x) { x.result = 2, x.refusal = 9; }));
    CHECK(ewire::EncodeOwns(hd, ewire::kScbHit, o, 1, buf, sizeof(buf)) == 0);                   // the wrong type
    CHECK(ewire::EncodeOwns(hd, ewire::kScbOwn, o, ewire::kOwnMax + 1, buf, sizeof(buf)) == 0);  // n
    CHECK(ewire::EncodeOwns(hd, ewire::kScbOwn, o, 0, buf, sizeof(buf)) == 0);
    // decoder refusals on the wire bytes
    const std::size_t ok = ewire::EncodeOwns(hd, ewire::kScbOwn, o, 1, buf, sizeof(buf));
    CHECK(ok == ewire::kScbHeader + ewire::kOwnSize && ewire::DecodeOwns(buf, ok, g, oo));
    auto bad = [&](std::size_t at, std::uint8_t v) {
        std::uint8_t c[ewire::kScbHeader + ewire::kOwnSize];
        std::memcpy(c, buf, ok);
        c[at] = v;
        ewire::ScbHeader h2;
        ewire::Own x[ewire::kOwnMax];
        return !ewire::DecodeOwns(c, ok, h2, x);
    };
    CHECK(bad(0, 'X'));         // magic
    CHECK(bad(4, ewire::kScbHit));  // another type (the size)
    CHECK(bad(5, 2));           // version
    CHECK(bad(6, 0));           // n
    CHECK(bad(7, 1));           // reserved
    CHECK(bad(16 + 6, 0x10));   // the id's type byte
    CHECK(bad(16 + 10, 3));     // owner
    CHECK(bad(16 + 11, 6));     // the ack's reason on an OWN
    CHECK(bad(16 + 12, 7));     // phase
    CHECK(bad(16 + 13, 0x80));  // flags
    CHECK(bad(16 + 15, 1));     // a result on an OWN
    CHECK(!ewire::DecodeOwns(buf, ok - 1, g, oo));
    // OWNRESET
    size = ewire::EncodeOwnReset(hd, 0xDEADBEEFu, buf, sizeof(buf));
    std::uint32_t gen = 0;
    CHECK(size == ewire::kScbHeader + ewire::kOwnResetSize && ewire::DecodeOwnReset(buf, size, g, &gen) && gen == 0xDEADBEEFu && g.type == ewire::kScbOwnReset);
    CHECK(!ewire::DecodeOwns(buf, size, g, oo));
    buf[6] = 2;
    CHECK(!ewire::DecodeScbHeader(buf, size, g));  // OWNRESET with n 2
    // P2's types stay decodable with the shared header
    ewire::Hit h;
    h.key = kMule, h.hit_seq = 1, h.attack = 0x47, h.kind = ewire::kKindBullet;
    size = ewire::EncodeHits(hd, &h, 1, buf, sizeof(buf));
    ewire::Hit hh[ewire::kHitMax];
    CHECK(size && ewire::DecodeHits(buf, size, g, hh) && !ewire::DecodeOwns(buf, size, g, oo));
    // SEN1: the guest's frame bits, the damaged flag, the phase byte
    ewire::EnemyFrame f;
    f.n = 1, f.flags = ewire::kFromGuest | ewire::kSenderCutscene, f.seq = 3, f.sender_ms = 77, f.gen_tag = 12;
    f.e[0] = MakeEnemy(0);
    f.e[0].owner_epoch = 0xFF, f.e[0].phase = 5, f.e[0].flags = static_cast<std::uint8_t>(ewire::kOwnerGuest | ewire::kDamaged | ewire::kMoving);
    std::uint8_t sb[ewire::kSenMaxSize];
    size = ewire::EncodeEnemies(f, sb, sizeof(sb));
    ewire::EnemyFrame fg;
    CHECK(size && ewire::DecodeEnemies(sb, size, fg) && fg.flags == (ewire::kFromGuest | ewire::kSenderCutscene) && fg.gen_tag == 12 && fg.e[0].owner_epoch == 0xFF &&
          fg.e[0].phase == 5 && fg.e[0].flags == (ewire::kOwnerGuest | ewire::kDamaged | ewire::kMoving));
    // the enemy shots' mask (shot_wire)
    CHECK(shotw::EnemyCaptureOn(1) && !shotw::EnemyReplayOn(1) && !shotw::EnemyCaptureOn(2) && shotw::EnemyReplayOn(2) && shotw::EnemyCaptureOn(3) &&
          shotw::EnemyReplayOn(3) && !shotw::EnemyCaptureOn(0) && !shotw::EnemyReplayOn(0));
}
}  // namespace

int main() {
    TestScbOwn();
    TestSenRoundTrip();
    TestSenRejects();
    TestSep();
    TestEnemyShots();
    TestScbHits();
    TestScbOthers();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
