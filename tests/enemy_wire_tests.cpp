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

}  // namespace

int main() {
    TestSenRoundTrip();
    TestSenRejects();
    TestSep();
    TestEnemyShots();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
