// Pure tests: pose codec, rotation helpers, DST1 join codes, and a full link v2
// handshake + DATA exchange between two in-process sessions.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "body_table.hpp"
#include "pose_wire.hpp"
#include "stcoop/link_crypto.hpp"
#include "stcoop/link_protocol.hpp"
#include "stcoop/link_session.hpp"
#include "stcoop/steam_join_code.hpp"

namespace {
int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

using namespace st;

void TestPoseRoundTrip() {
    wire::Pose p;
    p.seq = 42;
    p.sender_ms = 0xFFFFFF00u;
    p.flags = wire::kPoseValid;
    p.x = -1234.5678901;
    p.y = 98765.4321;
    p.z = 321.0;
    p.rot = wire::Normalize(wire::Quat{0.1f, 0.2f, 0.3f, 0.9f});
    p.speed = 4.5f;
    std::uint8_t buf[wire::kPoseFrameSize];
    CHECK(wire::EncodePose(p, buf) == wire::kPoseFrameSize);
    wire::Pose q;
    CHECK(wire::DecodePose(buf, sizeof(buf), q));
    CHECK(q.seq == p.seq && q.sender_ms == p.sender_ms && q.flags == p.flags);
    CHECK(q.x == p.x && q.y == p.y && q.z == p.z);
    CHECK(std::fabs(q.rot.w - p.rot.w) < 1e-6f);
    // Rejections
    CHECK(!wire::DecodePose(buf, sizeof(buf) - 1, q));
    std::uint8_t bad[wire::kPoseFrameSize];
    std::memcpy(bad, buf, sizeof(bad));
    bad[0] = 'X';
    CHECK(!wire::DecodePose(bad, sizeof(bad), q));
    wire::Pose nan = p;
    nan.x = std::nan("");
    wire::EncodePose(nan, bad);
    CHECK(!wire::DecodePose(bad, sizeof(bad), q));
    wire::Pose zeroq = p;
    zeroq.rot = wire::Quat{0, 0, 0, 0};
    wire::EncodePose(zeroq, bad);
    CHECK(!wire::DecodePose(bad, sizeof(bad), q));
}

void TestRotation() {
    const float yaws[] = {0.0f, 0.7f, 1.57f, 3.1f, -2.0f};
    for (float a : yaws) {
        // Rotation about Z (Decima's up axis).
        float m[3][3] = {{std::cos(a), -std::sin(a), 0}, {std::sin(a), std::cos(a), 0}, {0, 0, 1}};
        const wire::Quat q = wire::QuatFromMatrix(m);
        float back[3][3];
        wire::MatrixFromQuat(q, back);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) CHECK(std::fabs(back[r][c] - m[r][c]) < 1e-4f);
    }
    // Slerp midpoint between 0 and 90 deg about Z is 45 deg.
    float m0[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float m90[3][3] = {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}};
    const wire::Quat h = wire::Slerp(wire::QuatFromMatrix(m0), wire::QuatFromMatrix(m90), 0.5f);
    float mh[3][3];
    wire::MatrixFromQuat(h, mh);
    CHECK(std::fabs(mh[0][0] - std::cos(0.785398f)) < 1e-3f);
    CHECK(std::fabs(mh[1][0] - std::sin(0.785398f)) < 1e-3f);
}

void TestJoinCode() {
    const char secret[stcoop::link::kSecretChars] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K',
                                                    'M', 'N', 'P', 'Q', 'R', 'S', 'T', 'V', 'W', 'X'};
    const std::uint64_t host = 76561198000000001ull;
    char code[stcoop::steam_code::kCodeChars + 1] = {};
    CHECK(stcoop::steam_code::Format(secret, host, code, sizeof(code)));
    CHECK(std::strncmp(code, "DST1-", 5) == 0);
    stcoop::steam_code::Code parsed{};
    CHECK(stcoop::steam_code::Parse(code, std::strlen(code), parsed) == stcoop::steam_code::Status::ok);
    CHECK(parsed.host == host);
    CHECK(std::memcmp(parsed.secret, secret, sizeof(secret)) == 0);
    // A typo in the secret is caught by the check characters.
    code[7] = code[7] == 'A' ? 'B' : 'A';
    CHECK(stcoop::steam_code::Parse(code, std::strlen(code), parsed) != stcoop::steam_code::Status::ok);
    std::printf("example code: %.*s...\n", 10, code);
}

// Two sessions wired back to back through in-memory queues.
struct Wire {
    struct Packet {
        std::vector<std::uint8_t> bytes;
        stcoop::lab::Endpoint from;
    };
    std::deque<Packet> to_host, to_guest;
};
Wire g_wire;
const stcoop::lab::Endpoint kHostEp{0x0A000001, 1000};
const stcoop::lab::Endpoint kGuestEp{0x0A000002, 2000};

bool SendFromHost(void*, const stcoop::lab::Endpoint&, const std::uint8_t* d, std::size_t n) noexcept {
    g_wire.to_guest.push_back({std::vector<std::uint8_t>(d, d + n), kHostEp});
    return true;
}
bool SendFromGuest(void*, const stcoop::lab::Endpoint&, const std::uint8_t* d, std::size_t n) noexcept {
    g_wire.to_host.push_back({std::vector<std::uint8_t>(d, d + n), kGuestEp});
    return true;
}
bool Rand(void*, std::uint8_t* out, std::size_t n) noexcept {
    static std::uint32_t s = 12345;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        out[i] = static_cast<std::uint8_t>(s >> 24);
    }
    return true;
}

void TestLinkHandshake() {
    namespace lk = stcoop::link;
    lk::Session host, guest;
    char secret[lk::kSecretChars];
    std::memcpy(secret, lk::kLabSecret, lk::kSecretChars);
    lk::SessionConfig hc{}, gc{};
    hc.role = lk::Role::host;
    hc.play = true;
    hc.exact_filter = false;
    std::strncpy(hc.build, "test", lk::kBuildSize);
    lk::DerivePsk(secret, hc.psk);
    gc = hc;
    gc.role = lk::Role::guest;
    gc.peer = kHostEp;
    std::uint64_t now = 1000;
    host.Start(hc, lk::SessionIo{nullptr, &SendFromHost, &Rand}, now);
    guest.Start(gc, lk::SessionIo{nullptr, &SendFromGuest, &Rand}, now);
    int poses_at_host = 0;
    for (int step = 0; step < 400 && !(host.CanSendData() && guest.CanSendData() && poses_at_host > 3); ++step) {
        now += 10;
        host.OnTimer(now);
        guest.OnTimer(now);
        while (!g_wire.to_host.empty()) {
            auto p = g_wire.to_host.front();
            g_wire.to_host.pop_front();
            const lk::Delivery d = host.OnDatagram(p.bytes.data(), p.bytes.size(), p.from, now);
            if (d.data) {
                lk::MessageReader r(d.body, d.size);
                lk::Message m{};
                while (r.Next(m)) {
                    wire::Pose pose;
                    if (m.type == static_cast<std::uint8_t>(lk::MsgType::frame) && wire::DecodePose(m.data, m.size, pose))
                        ++poses_at_host;
                }
            }
        }
        while (!g_wire.to_guest.empty()) {
            auto p = g_wire.to_guest.front();
            g_wire.to_guest.pop_front();
            guest.OnDatagram(p.bytes.data(), p.bytes.size(), p.from, now);
        }
        if (guest.CanSendData()) {
            wire::Pose pose;
            pose.seq = static_cast<std::uint32_t>(step);
            pose.flags = wire::kPoseValid;
            std::uint8_t frame[wire::kPoseFrameSize];
            wire::EncodePose(pose, frame);
            std::uint8_t body[lk::kLinkMaxBody];
            lk::BodyWriter w(body, sizeof(body));
            CHECK(w.Add(lk::MsgType::frame, frame, sizeof(frame)));
            CHECK(guest.SendData(body, w.size(), now));
        }
    }
    CHECK(host.state() == lk::LinkState::connected);
    CHECK(guest.state() == lk::LinkState::connected);
    CHECK(poses_at_host > 3);
    std::printf("link: host=%s guest=%s poses_at_host=%d\n", lk::LinkStateName(host.state()),
                lk::LinkStateName(guest.state()), poses_at_host);

    // Wrong secret must never connect.
    lk::Session h2, g2;
    g_wire = Wire{};
    lk::SessionConfig bad = gc;
    char other[lk::kSecretChars];
    std::memcpy(other, lk::kLabSecret, lk::kSecretChars);
    other[0] = other[0] == 'A' ? 'B' : 'A';
    lk::DerivePsk(other, bad.psk);
    h2.Start(hc, lk::SessionIo{nullptr, &SendFromHost, &Rand}, now);
    g2.Start(bad, lk::SessionIo{nullptr, &SendFromGuest, &Rand}, now);
    for (int step = 0; step < 300; ++step) {
        now += 10;
        h2.OnTimer(now);
        g2.OnTimer(now);
        while (!g_wire.to_host.empty()) {
            auto p = g_wire.to_host.front();
            g_wire.to_host.pop_front();
            h2.OnDatagram(p.bytes.data(), p.bytes.size(), p.from, now);
        }
        while (!g_wire.to_guest.empty()) {
            auto p = g_wire.to_guest.front();
            g_wire.to_guest.pop_front();
            g2.OnDatagram(p.bytes.data(), p.bytes.size(), p.from, now);
        }
    }
    CHECK(h2.state() != lk::LinkState::connected);
    CHECK(g2.state() != lk::LinkState::connected);
}

}  // namespace

// STJ1 joint frames (docs/real-partner-body.md).
wire::Quat RandQuat(unsigned& seed) {
    auto r = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>((seed >> 8) & 0xFFFF) / 32767.5f - 1.0f;
    };
    return wire::Normalize(wire::Quat{r(), r(), r(), r()});
}

void TestJointFrames() {
    wire::JointFrame f;
    f.seq = 7;
    f.sender_ms = 123456;
    f.joint_count = 77;
    f.body_id = 3;
    f.flags = wire::kJointsValid;
    unsigned seed = 12345;
    for (unsigned j = 0; j < 77; ++j) f.q[j] = RandQuat(seed);
    // edge cases: identity, w ~ 0, components at +-1/sqrt2, a negated quaternion
    f.q[0] = wire::Quat{0, 0, 0, 1};
    f.q[1] = wire::Quat{1, 0, 0, 0};
    f.q[2] = wire::Quat{0.70710678f, 0.70710678f, 0, 0};
    f.q[3] = wire::Quat{-0.70710678f, 0, 0.70710678f, 0};
    f.q[4] = wire::Quat{0.5f, -0.5f, 0.5f, -0.5f};
    f.q[5] = wire::Quat{-0.1f, -0.2f, -0.3f, -0.9273618f};
    f.t_count = 2;
    f.t_joint[0] = 0;
    f.t_joint[1] = 1;
    f.t[0][0] = 0.01f, f.t[0][1] = -0.02f, f.t[0][2] = 0.0f;
    f.t[1][0] = 0.0123f, f.t[1][1] = 0.0f, f.t[1][2] = 0.919f;
    std::uint8_t buf[wire::kMaxJointFrameSize];
    const std::size_t n = wire::EncodeJoints(f, buf, sizeof(buf));
    CHECK(n == wire::JointFrameSize(77, 2));
    CHECK(n == 497);
    wire::JointFrame g;
    CHECK(wire::DecodeJoints(buf, n, g));
    CHECK(g.seq == 7 && g.sender_ms == 123456 && g.joint_count == 77 && g.body_id == 3 && g.layout_hash == wire::kSamLayoutHash);
    float worst = 0;
    for (unsigned j = 0; j < 77; ++j) {
        const float a = wire::AngleBetween(f.q[j], g.q[j]);
        if (a > worst) worst = a;
    }
    CHECK(worst < 0.02f);
    CHECK(g.t_count == 2 && g.t_joint[1] == 1 && std::fabs(g.t[1][2] - 0.919f) < 1e-4f && std::fabs(g.t[0][1] + 0.02f) < 1e-4f);
    // clamp
    f.t[1][2] = 5.0f;
    CHECK(wire::EncodeJoints(f, buf, sizeof(buf)) == n);
    CHECK(wire::DecodeJoints(buf, n, g) && std::fabs(g.t[1][2] - 3.2767f) < 1e-3f);
    f.t[1][2] = 0.919f;
    // rejects
    CHECK(!wire::DecodeJoints(buf, n - 1, g));
    CHECK(!wire::DecodeJoints(buf, n + 1, g));
    std::uint8_t bad[wire::kMaxJointFrameSize];
    std::memcpy(bad, buf, n);
    bad[3] = '2';
    CHECK(!wire::DecodeJoints(bad, n, g));
    std::memcpy(bad, buf, n);
    bad[16] = 0;  // joint_count 0
    CHECK(!wire::DecodeJoints(bad, n, g));
    std::memcpy(bad, buf, n);
    bad[16] = 200;  // too many joints
    CHECK(!wire::DecodeJoints(bad, n, g));
    std::memcpy(bad, buf, n);
    bad[20 + 77 * 6 + 1] = 77;  // translation joint out of range
    CHECK(!wire::DecodeJoints(bad, n, g));
    CHECK(!wire::DecodeJoints(buf, 10, g));
    CHECK(!wire::DecodeJoints(nullptr, n, g));
    wire::JointFrame nan = f;
    nan.q[9].x = std::nanf("");
    CHECK(wire::EncodeJoints(nan, buf, sizeof(buf)) == 0);
    CHECK(wire::EncodeJoints(f, buf, 100) == 0);  // too small
    // blend: halfway between q and -q of the same rotation is that rotation; t lerps
    wire::JointFrame a = f, b = f;
    for (unsigned j = 0; j < 77; ++j) b.q[j] = wire::Quat{-f.q[j].x, -f.q[j].y, -f.q[j].z, -f.q[j].w};
    b.t[1][2] = 1.019f;
    wire::JointFrame m;
    wire::BlendJoints(a, b, 0.5f, m);
    worst = 0;
    for (unsigned j = 0; j < 77; ++j) worst = std::fmax(worst, wire::AngleBetween(m.q[j], f.q[j]));
    CHECK(worst < 0.01f);
    CHECK(std::fabs(m.t[1][2] - 0.969f) < 1e-5f);
    // blend of two different rotations about z: halfway angle
    a.q[0] = wire::Quat{0, 0, 0, 1};
    b.q[0] = wire::Quat{0, 0, std::sin(0.5f), std::cos(0.5f)};  // 60 degrees... (1 rad)
    wire::BlendJoints(a, b, 0.5f, a);  // aliasing allowed
    CHECK(std::fabs(wire::AngleBetween(a.q[0], wire::Quat{0, 0, 0, 1}) - 57.29578f * 0.5f) < 0.5f);
    CHECK(std::fabs(wire::AngleBetween(wire::Quat{0, 0, 0, 1}, wire::Quat{0, 0, 1, 0}) - 180.0f) < 0.01f);
}

void TestBodyTable() {
    std::uint8_t g[16] = {};
    CHECK(bodies::ParseGuid("336f4328-1381-4947-8e6d-7def29e654bd", g));
    const std::uint8_t fgl[16] = {0x28, 0x43, 0x6f, 0x33, 0x81, 0x13, 0x47, 0x49, 0x8e, 0x6d, 0x7d, 0xef, 0x29, 0xe6, 0x54, 0xbd};
    CHECK(std::memcmp(g, fgl, 16) == 0);
    CHECK(bodies::ParseGuid("514b5705-aaf0-a447-a82e-0bb83b855201", g));
    const std::uint8_t sam[16] = {0x05, 0x57, 0x4b, 0x51, 0xf0, 0xaa, 0x47, 0xa4, 0xa8, 0x2e, 0x0b, 0xb8, 0x3b, 0x85, 0x52, 0x01};
    CHECK(std::memcmp(g, sam, 16) == 0);
    CHECK(!bodies::ParseGuid("336f4328-1381-4947-8e6d-7def29e654b", g));
    CHECK(!bodies::ParseGuid("336f4328+1381-4947-8e6d-7def29e654bd", g));
    CHECK(!bodies::ParseGuid("336f4328-1381-4947-8e6d-7def29e654bx", g));
    CHECK(!bodies::ParseGuid(nullptr, g));
    CHECK(bodies::Count() >= 5);
    for (std::size_t i = 0; i < bodies::Count(); ++i) {
        const bodies::Body& b = bodies::At(i);
        CHECK(b.id == i + 1);  // ids are wire values: dense and append-only
        CHECK(bodies::ParseGuid(b.guid, g));
        CHECK(std::strncmp(b.path, "ds/entities/cinematics/demo_", 28) == 0);
        CHECK(bodies::FindByKey(b.key) == &b && bodies::FindById(b.id) == &b);
    }
    CHECK(bodies::FindByKey("FRAGILE") && bodies::FindByKey("FRAGILE")->id == 3);
    CHECK(!bodies::FindByKey("frag") && !bodies::FindByKey("") && !bodies::FindById(0));
    CHECK(bodies::Default().id == 1);
}

int main() {
    TestPoseRoundTrip();
    TestJointFrames();
    TestBodyTable();
    TestRotation();
    TestJoinCode();
    TestLinkHandshake();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all net tests passed\n");
    return 0;
}
