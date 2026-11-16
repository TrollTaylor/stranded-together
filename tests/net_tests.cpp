// Pure tests: pose codec, rotation helpers, DST1 join codes, and a full link v2
// handshake + DATA exchange between two in-process sessions.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

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

int main() {
    TestPoseRoundTrip();
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
