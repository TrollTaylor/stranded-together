// Pure tests: pose codec, rotation helpers, DST1 join codes, and a full link v2
// handshake + DATA exchange between two in-process sessions.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "body_choice.hpp"
#include "body_table.hpp"
#include "nametag_math.hpp"
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

// ---- name tags: STN1 frame + sanitizer (docs/real-partner-body.md section 10)
void TestNameFrames() {
    char out[wire::kMaxNameBytes + 1];
    // plain, trimmed, inner runs of space collapsed
    CHECK(wire::SanitizeName("  Tag  Host ", 12, out, sizeof(out)) == 8 && std::strcmp(out, "Tag Host") == 0);
    // controls, DEL, bidi overrides and zero-width characters dropped
    const char evil[] = "A\x01" "B\x7F" "C\xE2\x80\xAE" "D\xE2\x81\xA6" "E\xE2\x80\x8B" "F\xEF\xBB\xBF" "G\xC2\x85" "H";
    CHECK(wire::SanitizeName(evil, sizeof(evil) - 1, out, sizeof(out)) == 8 && std::strcmp(out, "ABCDEFGH") == 0);
    // invalid UTF-8: stray continuation, overlong, surrogate, truncated sequence -> dropped
    const char bad[] = "a\x80" "b\xC0\xAF" "c\xED\xA0\x80" "d\xE3\x81";
    CHECK(wire::SanitizeName(bad, sizeof(bad) - 1, out, sizeof(out)) == 4 && std::strcmp(out, "abcd") == 0);
    // valid multi-byte text is kept: "Sam" + U+00E9 + U+30B5 (katakana SA) + U+1F4E6 (package)
    const char utf[] = "Sam\xC3\xA9\xE3\x82\xB5\xF0\x9F\x93\xA6";
    CHECK(wire::SanitizeName(utf, sizeof(utf) - 1, out, sizeof(out)) == sizeof(utf) - 1 && std::strcmp(out, utf) == 0);
    // 32 code points at most, cut at a code-point boundary
    std::string many;
    for (int i = 0; i < 40; ++i) many += "\xC3\xA9";  // 40 x U+00E9, 80 bytes
    std::size_t n = wire::SanitizeName(many.c_str(), many.size(), out, sizeof(out));
    CHECK(n == 64 && std::memcmp(out, many.c_str(), 64) == 0);  // 32 code points = 64 bytes
    std::string kana;
    for (int i = 0; i < 30; ++i) kana += "\xE3\x82\xB5";  // 90 bytes: the 64-byte cap cuts at 21 code points
    n = wire::SanitizeName(kana.c_str(), kana.size(), out, sizeof(out));
    CHECK(n == 63 && std::memcmp(out, kana.c_str(), 63) == 0);
    char small[6];
    CHECK(wire::SanitizeName("\xC3\xA9\xC3\xA9\xC3\xA9", 6, small, sizeof(small)) == 4);  // never half a sequence
    CHECK(wire::SanitizeName(nullptr, 3, out, sizeof(out)) == 0 && out[0] == 0);
    CHECK(wire::SanitizeName(" \t\n ", 4, out, sizeof(out)) == 0 && out[0] == 0);

    // round trip
    wire::NameFrame f;
    f.flags = wire::kNameFromHost;
    std::snprintf(f.name, sizeof(f.name), "%s", "TagHost");
    f.len = 7;
    std::uint8_t buf[wire::kMaxNameFrameSize];
    const std::size_t size = wire::EncodeName(f, buf, sizeof(buf));
    CHECK(size == wire::kNameHeaderSize + 7);
    CHECK(std::memcmp(buf, "STN1", 4) == 0 && buf[4] == 1 && buf[5] == 1 && buf[6] == 7 && buf[7] == 0);
    wire::NameFrame g;
    CHECK(wire::DecodeName(buf, size, g) && g.len == 7 && std::strcmp(g.name, "TagHost") == 0 && g.flags == wire::kNameFromHost);
    // the other frame decoders refuse it (a receiver tries pose, joints, then name)
    wire::Pose pose;
    static wire::JointFrame jf;
    CHECK(!wire::DecodePose(buf, size, pose));
    CHECK(!wire::DecodeJoints(buf, size, jf));
    // and the name decoder refuses theirs
    std::uint8_t pbuf[wire::kPoseFrameSize];
    wire::Pose p;
    wire::EncodePose(p, pbuf);
    CHECK(!wire::DecodeName(pbuf, sizeof(pbuf), g));
    // size, version, length checks
    CHECK(!wire::DecodeName(buf, size - 1, g));
    CHECK(!wire::DecodeName(buf, wire::kNameHeaderSize - 1, g));
    std::uint8_t bad2[wire::kMaxNameFrameSize + 4];
    std::memcpy(bad2, buf, size);
    bad2[4] = 2;
    CHECK(!wire::DecodeName(bad2, size, g));
    std::memcpy(bad2, buf, size);
    bad2[6] = 65;
    CHECK(!wire::DecodeName(bad2, wire::kNameHeaderSize + 65, g));
    // unknown flag bits are cleared; an empty name is valid
    std::memcpy(bad2, buf, size);
    bad2[5] = 0xF1;
    CHECK(wire::DecodeName(bad2, size, g) && g.flags == wire::kNameFromHost);
    wire::NameFrame empty;
    CHECK(wire::EncodeName(empty, buf, sizeof(buf)) == wire::kNameHeaderSize);
    CHECK(wire::DecodeName(buf, wire::kNameHeaderSize, g) && g.len == 0 && g.name[0] == 0);
    // a hostile sender's bytes come out sanitized
    const std::uint8_t raw[] = {'S', 'T', 'N', '1', 1, 0, 6, 0, 'x', 0x1B, '[', '2', 'J', 'y'};
    CHECK(wire::DecodeName(raw, sizeof(raw), g) && std::strcmp(g.name, "x[2Jy") == 0 && g.len == 5);
    // no room
    CHECK(wire::EncodeName(f, buf, 10) == 0);
}

void TestGearFrames() {
    // A backpack on Sam's spine, two cases stacked on it, a hand-held case, the Odradek.
    wire::GearFrame f;
    f.seq = 77;
    f.flags = wire::kGearValid;
    auto add = [&f](wire::GearKind k, std::uint8_t model, std::uint8_t var, std::uint8_t joint, std::uint8_t parent, float x, float y, float z,
                    float deg) {
        wire::GearItem& it = f.items[f.count++];
        it.kind = k;
        it.model = model;
        it.variation = var;
        it.joint = joint;
        it.parent = parent;
        it.p[0] = x, it.p[1] = y, it.p[2] = z;
        const float h = deg * 0.0087266f;
        it.q = wire::Quat{0, 0, std::sin(h), std::cos(h)};
    };
    add(wire::GearKind::backpack, wire::kGearNone, wire::kGearNone, 4, wire::kGearNoParent, 0.0f, -0.25f, 0.05f, 0);
    add(wire::GearKind::cargo, 2, 7, 0, 0, 0.01f, -0.10f, 0.32f, 90);
    add(wire::GearKind::cargo, 1, wire::kGearNone, 0, 0, -0.02f, -0.12f, 0.71f, -12.5f);
    add(wire::GearKind::cargo, 3, 3, 9, wire::kGearNoParent, 0.10f, 0.0f, -0.30f, 180);
    add(wire::GearKind::odradek, wire::kGearNone, wire::kGearNone, 35, wire::kGearNoParent, 0.05f, -0.2f, 0.11f, 33);
    std::uint8_t buf[wire::kMaxGearFrameSize];
    const std::size_t n = wire::EncodeGear(f, buf, sizeof(buf));
    CHECK(n == wire::GearFrameSize(5) && n == 16 + 5 * 18);
    wire::GearFrame g;
    CHECK(wire::DecodeGear(buf, n, g));
    CHECK(g.seq == 77 && g.count == 5 && g.flags == wire::kGearValid && g.dropped == 0);
    CHECK(wire::GearLayoutHash(g) == wire::GearLayoutHash(f));
    for (unsigned i = 0; i < 5; ++i) {
        CHECK(g.items[i].kind == f.items[i].kind && g.items[i].model == f.items[i].model && g.items[i].variation == f.items[i].variation);
        CHECK(g.items[i].parent == f.items[i].parent);
        if (f.items[i].parent == wire::kGearNoParent) CHECK(g.items[i].joint == f.items[i].joint);
        for (int a = 0; a < 3; ++a) CHECK(std::fabs(g.items[i].p[a] - f.items[i].p[a]) <= 0.0005f);  // 1 mm units
        CHECK(wire::AngleBetween(g.items[i].q, f.items[i].q) < 0.02f);
    }
    float dm = 1, dd = 1;
    CHECK(wire::GearPoseDelta(f, g, &dm, &dd) && dm <= 0.0005f && dd < 0.02f);
    // A moved piece changes the poses, not the layout.
    wire::GearFrame moved = f;
    moved.items[2].p[2] += 0.05f;
    CHECK(wire::GearLayoutHash(moved) == wire::GearLayoutHash(f));
    CHECK(wire::GearPoseDelta(f, moved, &dm, &dd) && std::fabs(dm - 0.05f) < 1e-4f);
    // Another model / variation / anchor / count is another layout.
    wire::GearFrame other = f;
    other.items[1].model = 3;
    CHECK(wire::GearLayoutHash(other) != wire::GearLayoutHash(f) && !wire::GearPoseDelta(f, other, &dm, &dd));
    other = f;
    other.items[1].variation = 8;
    CHECK(wire::GearLayoutHash(other) != wire::GearLayoutHash(f));
    other = f;
    other.items[3].joint = 38;
    CHECK(wire::GearLayoutHash(other) != wire::GearLayoutHash(f));
    other = f;
    other.count = 4;
    CHECK(wire::GearLayoutHash(other) != wire::GearLayoutHash(f));
    // Decoder rejects: size, magic, version, count over the cap, a forward parent, a bad joint,
    // an unknown kind, a hash that does not match.
    CHECK(!wire::DecodeGear(buf, n - 1, g));
    CHECK(!wire::DecodeGear(buf, n + 1, g));
    std::uint8_t bad[wire::kMaxGearFrameSize];
    std::memcpy(bad, buf, n);
    bad[3] = '2';
    CHECK(!wire::DecodeGear(bad, n, g));
    std::memcpy(bad, buf, n);
    bad[8] = 2;  // version
    CHECK(!wire::DecodeGear(bad, n, g));
    std::memcpy(bad, buf, n);
    bad[9] = 41;  // count over the cap (and the size no longer matches)
    CHECK(!wire::DecodeGear(bad, n, g));
    std::memcpy(bad, buf, n);
    bad[16 + 18 * 1 + 4] = 3;  // item 1 sits on item 3 (not an earlier item)
    CHECK(!wire::DecodeGear(bad, n, g));
    std::memcpy(bad, buf, n);
    bad[16 + 18 * 3 + 3] = 77;  // joint 77 of 77
    CHECK(!wire::DecodeGear(bad, n, g));
    std::memcpy(bad, buf, n);
    bad[16 + 18 * 2 + 0] = 9;  // unknown kind
    CHECK(!wire::DecodeGear(bad, n, g));
    std::memcpy(bad, buf, n);
    bad[16 + 18 * 2 + 1] = 5;  // model changed without the hash: rejected
    CHECK(!wire::DecodeGear(bad, n, g));
    std::memcpy(bad, buf, n);
    bad[10] = 0xFF;  // unknown flag bits are cleared
    CHECK(wire::DecodeGear(bad, n, g) && g.flags == wire::kGearValid);
    // Other frames' magic is not a gear frame, and a gear frame is none of theirs.
    wire::Pose pose;
    wire::JointFrame jf;
    wire::NameFrame nf;
    CHECK(!wire::DecodePose(buf, n, pose) && !wire::DecodeJoints(buf, n, jf) && !wire::DecodeName(buf, n, nf));
    std::uint8_t pbuf[wire::kPoseFrameSize];
    wire::EncodePose(pose, pbuf);
    CHECK(!wire::DecodeGear(pbuf, sizeof(pbuf), g));
    // Encoder refuses: over the cap, a forward parent, a bad joint, NaN, beyond +-32 m, a small buffer.
    wire::GearFrame e = f;
    e.count = static_cast<std::uint8_t>(wire::kMaxGearItems + 1);
    CHECK(wire::EncodeGear(e, buf, sizeof(buf)) == 0);
    e = f;
    e.items[1].parent = 1;
    CHECK(wire::EncodeGear(e, buf, sizeof(buf)) == 0);
    e = f;
    e.items[0].joint = 77;
    CHECK(wire::EncodeGear(e, buf, sizeof(buf)) == 0);
    e = f;
    e.items[2].p[1] = std::nanf("");
    CHECK(wire::EncodeGear(e, buf, sizeof(buf)) == 0);
    e = f;
    e.items[2].p[1] = 40.0f;
    CHECK(wire::EncodeGear(e, buf, sizeof(buf)) == 0);
    CHECK(wire::EncodeGear(f, buf, n - 1) == 0);
    // The full cap fits a link body next to nothing else (1148 bytes) and decodes.
    wire::GearFrame full;
    full.flags = wire::kGearValid;
    for (unsigned i = 0; i < wire::kMaxGearItems; ++i) {
        wire::GearItem& it = full.items[full.count++];
        it.kind = i == 0 ? wire::GearKind::backpack : wire::GearKind::cargo;
        it.model = i == 0 ? wire::kGearNone : static_cast<std::uint8_t>(1 + i % 6);
        it.joint = 4;
        it.parent = i == 0 ? wire::kGearNoParent : static_cast<std::uint8_t>(i - 1);
        it.p[2] = 0.3f;
    }
    const std::size_t fn = wire::EncodeGear(full, buf, sizeof(buf));
    CHECK(fn == wire::kMaxGearFrameSize && fn <= 1148 - 4);
    CHECK(wire::DecodeGear(buf, fn, g) && g.count == wire::kMaxGearItems && g.items[39].parent == 38);
}

void TestNameTagMath() {
    // Camera at the origin looking along +Y (Z up), 90 deg vertical fov, 16:9.
    nametag::View v;
    v.axis[0][0] = 1;  // right
    v.axis[1][1] = 1;  // forward
    v.axis[2][2] = 1;  // up
    v.near_plane = 0.1f;
    v.far_plane = 1000;
    v.top = 0.1f;  // tan(45 deg) * near
    v.right = v.top * 16.0f / 9.0f;
    const double ahead[3] = {0, 10, 0};
    nametag::Projected p = nametag::Project(v, ahead, 1920, 1080);
    CHECK(p.visible && std::fabs(p.x - 960) < 0.01f && std::fabs(p.y - 540) < 0.01f && std::fabs(p.dist - 10) < 1e-4f);
    // 10 m ahead, 5 m up: half way to the top edge (tan = 0.5)
    const double up[3] = {0, 10, 5};
    p = nametag::Project(v, up, 1920, 1080);
    CHECK(p.visible && std::fabs(p.x - 960) < 0.01f && std::fabs(p.y - 270) < 0.01f);
    // to the right: x grows; on the right edge exactly at x = depth * right / near
    const double right_edge[3] = {10.0 * 16.0 / 9.0, 10, 0};
    p = nametag::Project(v, right_edge, 1920, 1080);
    CHECK(p.visible && std::fabs(p.x - 1920) < 0.05f);
    const double left[3] = {-3, 10, 0};
    p = nametag::Project(v, left, 1920, 1080);
    CHECK(p.visible && p.x < 960);
    // behind the camera, on the near plane, beyond the edges: not visible
    const double behind[3] = {0, -10, 0};
    CHECK(!nametag::Project(v, behind, 1920, 1080).visible);
    const double at_near[3] = {0, 0.05, 0};
    CHECK(!nametag::Project(v, at_near, 1920, 1080).visible);
    const double off_right[3] = {30, 10, 0};
    CHECK(!nametag::Project(v, off_right, 1920, 1080).visible);
    const double off_top[3] = {0, 10, 11};
    CHECK(!nametag::Project(v, off_top, 1920, 1080).visible);
    // a camera far from the origin (world coordinates in the thousands, doubles)
    nametag::View far = v;
    far.pos[0] = -1100.25, far.pos[1] = 273.5, far.pos[2] = 352.75;
    const double far_pt[3] = {-1100.25, 283.5, 352.75};
    p = nametag::Project(far, far_pt, 2560, 1440);
    CHECK(p.visible && std::fabs(p.x - 1280) < 0.01f && std::fabs(p.y - 720) < 0.01f);
    // a rotated camera: looking along +X (right = -Y)
    nametag::View rot = v;
    rot.axis[0][0] = 0, rot.axis[0][1] = -1;
    rot.axis[1][1] = 0, rot.axis[1][0] = 1;
    const double along_x[3] = {10, -2, 0};  // 2 m to the camera's right
    p = nametag::Project(rot, along_x, 1920, 1080);
    CHECK(p.visible && p.x > 960 && std::fabs(p.y - 540) < 0.01f);
    // unusable views
    nametag::View ortho = v;
    ortho.perspective = false;
    CHECK(!nametag::Project(ortho, ahead, 1920, 1080).visible);
    nametag::View zero = v;
    zero.axis[1][1] = 0;
    CHECK(!nametag::ViewUsable(zero));
    nametag::View nan = v;
    nan.pos[0] = std::nan("");
    CHECK(!nametag::ViewUsable(nan));
    // letterbox: a 21:9 view on a 16:9 buffer -> bars top and bottom, centre unchanged
    nametag::View wide = v;
    wide.right = v.top * 21.0f / 9.0f;
    const nametag::Viewport vp = nametag::FitViewport(wide.right / wide.top, 1920, 1080);
    CHECK(vp.fitted && std::fabs(vp.w - 1920) < 0.01f && std::fabs(vp.h - 1920 * 9.0f / 21.0f) < 0.05f && vp.y > 0);
    p = nametag::Project(wide, up, 1920, 1080);
    CHECK(p.visible && std::fabs(p.y - (540 - 0.25f * vp.h)) < 0.05f);
    CHECK(!nametag::FitViewport(16.0f / 9.0f, 1920, 1080).fitted);
    CHECK(!nametag::FitViewport(1.7700f, 1920, 1080).fitted);  // within 1 %
    const nametag::Viewport pil = nametag::FitViewport(4.0f / 3.0f, 1920, 1080);
    CHECK(pil.fitted && std::fabs(pil.w - 1440) < 0.01f && std::fabs(pil.x - 240) < 0.01f);

    // style by distance
    nametag::Style s = nametag::StyleFor(3, 1);
    CHECK(s.visible && s.size_px == nametag::kNearPx && s.alpha == 1 && !s.show_distance);
    s = nametag::StyleFor(60, 1);
    CHECK(s.visible && s.size_px == nametag::kFarPx && s.show_distance);
    s = nametag::StyleFor(32.5f, 2);
    CHECK(s.visible && std::fabs(s.size_px - 2 * (nametag::kNearPx + nametag::kFarPx) / 2) < 0.01f);
    s = nametag::StyleFor(225, 1);
    CHECK(s.visible && std::fabs(s.alpha - 0.5f) < 1e-4f);
    CHECK(!nametag::StyleFor(300, 1).visible && !nametag::StyleFor(0.5f, 1).visible && !nametag::StyleFor(std::nanf(""), 1).visible);

    // labels
    char out[96];
    CHECK(nametag::ComposeLabel("Alice", "Bob", true, false, out, sizeof(out)) == 5 && std::strcmp(out, "Alice") == 0);
    nametag::ComposeLabel("Sam", "Sam", true, false, out, sizeof(out));  // one Steam account, two games
    CHECK(std::strcmp(out, "Sam (host)") == 0);
    nametag::ComposeLabel("Sam", "Sam", false, false, out, sizeof(out));
    CHECK(std::strcmp(out, "Sam (guest)") == 0);
    nametag::ComposeLabel("", "Bob", false, false, out, sizeof(out));
    CHECK(std::strcmp(out, "Guest") == 0);
    nametag::ComposeLabel(nullptr, "Bob", true, false, out, sizeof(out));
    CHECK(std::strcmp(out, "Host") == 0);
    nametag::ComposeLabel(nullptr, "Bob", false, true, out, sizeof(out));
    CHECK(std::strcmp(out, "Bob (echo)") == 0);
    nametag::ComposeLabel(nullptr, "", false, true, out, sizeof(out));
    CHECK(std::strcmp(out, "You (echo)") == 0);
    char tiny[6];
    CHECK(nametag::ComposeLabel("ab\xE3\x82\xB5" "cd", "x", true, false, tiny, sizeof(tiny)) == 5);  // cut after the full sequence
    CHECK(nametag::ComposeLabel("abc\xE3\x82\xB5", "x", true, false, tiny, sizeof(tiny)) == 3 && std::strcmp(tiny, "abc") == 0);
    CHECK(nametag::ComposeLabel("abcd\xC3\xA9", "x", true, false, tiny, sizeof(tiny)) == 4 && std::strcmp(tiny, "abcd") == 0);
    CHECK(nametag::ComposeLabel("abc\xC3\xA9", "x", true, false, tiny, sizeof(tiny)) == 5 && std::strcmp(tiny, "abc\xC3\xA9") == 0);
    CHECK(nametag::NameHash("") == 2166136261u && nametag::NameHash("a") != nametag::NameHash("b"));
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

// The optional outfit block (flags kJointsOutfit): 8 bytes after the translations.
void TestJointOutfit() {
    wire::JointFrame f;
    f.joint_count = 77;
    f.body_id = 1;
    f.flags = wire::kJointsValid | wire::kJointsOutfit;
    f.t_count = 2;
    f.t_joint[1] = 1;
    f.outfit = wire::Outfit{0, 33, 16, 24, 1, 2, 3, 4};
    std::uint8_t buf[wire::kMaxJointFrameSize];
    const std::size_t n = wire::EncodeJoints(f, buf, sizeof(buf));
    CHECK(n == wire::JointFrameSize(77, 2, true) && n == 505);
    CHECK(buf[n - 8] == 0 && buf[n - 7] == 33 && buf[n - 6] == 16 && buf[n - 1] == 4);
    wire::JointFrame g;
    CHECK(wire::DecodeJoints(buf, n, g));
    CHECK((g.flags & wire::kJointsOutfit) && g.outfit == f.outfit && g.outfit.glasses_b_frame == 4 && g.outfit.exos == 33);
    CHECK(!wire::DecodeJoints(buf, n - 8, g));  // flag set, block missing
    CHECK(!wire::DecodeJoints(buf, n - 1, g));
    std::uint8_t bad[wire::kMaxJointFrameSize];
    std::memcpy(bad, buf, n);
    bad[18] = wire::kJointsValid;  // block present, flag clear
    CHECK(!wire::DecodeJoints(bad, n, g));
    CHECK(wire::DecodeJoints(bad, n - 8, g) && !(g.flags & wire::kJointsOutfit) && g.outfit == wire::Outfit{});
    // without the flag the frame is the plain 497-byte one
    f.flags = wire::kJointsValid;
    CHECK(wire::EncodeJoints(f, buf, sizeof(buf)) == 497);
    // the blend carries the newer frame's outfit
    wire::JointFrame a = f, b = f, m;
    a.outfit.exos = 1;
    b.outfit.exos = 2;
    b.flags = wire::kJointsValid | wire::kJointsOutfit;
    wire::BlendJoints(a, b, 0.25f, m);
    CHECK(m.outfit.exos == 2 && (m.flags & wire::kJointsOutfit));
    // a newer build's unknown flag with a longer frame: decoded, the extra block ignored
    f.flags = wire::kJointsValid | wire::kJointsOutfit;
    const std::size_t n2 = wire::EncodeJoints(f, buf, sizeof(buf));
    std::uint8_t longer[wire::kMaxJointFrameSize + 16] = {};
    std::memcpy(longer, buf, n2);
    longer[18] = static_cast<std::uint8_t>(wire::kJointsValid | wire::kJointsOutfit | 0x04);
    CHECK(wire::DecodeJoints(longer, n2 + 12, g) && g.outfit == f.outfit && g.flags == (wire::kJointsValid | wire::kJointsOutfit));
    CHECK(wire::DecodeJoints(longer, n2, g));       // the unknown block may be empty
    CHECK(!wire::DecodeJoints(longer, n2 - 1, g));  // but the known blocks must be complete
    // the exos index counts only for the porter suit (variant 0)
    CHECK(wire::PorterExos(wire::Outfit{0, 31, 16, 0, 0, 0, 0, 0}) == 31);
    CHECK(wire::PorterExos(wire::Outfit{2, 31, 16, 0, 0, 0, 0, 0}) == 0xFF);
    CHECK(wire::PorterExos(wire::Outfit{0, 44, 16, 0, 0, 0, 0, 0}) == 0xFF);
    CHECK(wire::PorterExos(wire::Outfit{0xFF, 3, 16, 0, 0, 0, 0, 0}) == 0xFF);
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
    // offered: exactly the bodies proven in game (ids 1-16, real-body-tour-all), in id order; nothing else
    CHECK(bodies::OfferedCount() == 16);
    for (std::size_t i = 0; i < bodies::OfferedCount(); ++i) CHECK(bodies::OfferedAt(i).id == i + 1 && bodies::OfferedAt(i).offered);
    CHECK(&bodies::OfferedAt(99) == &bodies::Default());
    for (std::uint8_t id = 1; id <= 16; ++id) CHECK(bodies::IsOffered(id));
    CHECK(!bodies::IsOffered(0) && !bodies::IsOffered(17) && !bodies::IsOffered(200));
    std::size_t offered = 0;
    for (std::size_t i = 0; i < bodies::Count(); ++i) offered += bodies::At(i).offered ? 1 : 0;
    CHECK(offered == bodies::OfferedCount());
}

void TestBodyChoice() {
    // sender: my_character -> STJ1 body_id
    bodies::OwnChoice o = bodies::ChooseOwn(false, "fragile");
    CHECK(o.body->id == 3 && !o.host_forced && !o.unknown_key);
    o = bodies::ChooseOwn(false, "Cliff");
    CHECK(o.body->id == 5);
    o = bodies::ChooseOwn(false, "");
    CHECK(o.body == &bodies::Default() && !o.unknown_key);
    o = bodies::ChooseOwn(false, nullptr);
    CHECK(o.body == &bodies::Default() && !o.unknown_key);
    o = bodies::ChooseOwn(false, "gandalf");
    CHECK(o.body == &bodies::Default() && o.unknown_key);
    o = bodies::ChooseOwn(false, "heartman");
    CHECK(o.body->id == 8 && !o.unknown_key);
    // A body appended to the table but not proven yet (none today) is sent as asked; the receiver shows sam.
    const bodies::Body* unproven = nullptr;
    for (std::size_t i = 0; i < bodies::Count() && !unproven; ++i)
        if (!bodies::At(i).offered) unproven = &bodies::At(i);
    if (unproven) {
        o = bodies::ChooseOwn(false, unproven->key);
        CHECK(o.body == unproven && !o.unknown_key);
    }
    // the host is always Sam
    o = bodies::ChooseOwn(true, "fragile");
    CHECK(o.body == &bodies::Default() && o.host_forced);
    o = bodies::ChooseOwn(true, "sam");
    CHECK(o.body == &bodies::Default() && !o.host_forced);
    o = bodies::ChooseOwn(true, "");
    CHECK(o.body == &bodies::Default() && !o.host_forced && !o.unknown_key);
    o = bodies::ChooseOwn(true, "gandalf");
    CHECK(o.body == &bodies::Default() && !o.host_forced && o.unknown_key);

    // receiver: the peer's id when offered, else sam; partner_character overrides
    bodies::PartnerChoice p = bodies::ChoosePartner("", true, 3);
    CHECK(p.body->id == 3 && !p.from_override && !p.peer_not_offered);
    p = bodies::ChoosePartner(nullptr, true, 5);
    CHECK(p.body->id == 5);
    p = bodies::ChoosePartner("", false, 3);  // no frame yet: the peer's id is not known
    CHECK(p.body == &bodies::Default() && !p.peer_not_offered);
    p = bodies::ChoosePartner("", true, 8);
    CHECK(p.body->id == 8 && !p.peer_not_offered);
    if (unproven) {  // known but not offered
        p = bodies::ChoosePartner("", true, unproven->id);
        CHECK(p.body == &bodies::Default() && p.peer_not_offered);
        p = bodies::ChoosePartner(unproven->key, true, 3);  // debug tours may show unproven bodies
        CHECK(p.body == unproven && p.from_override);
    }
    p = bodies::ChoosePartner("", true, 0);
    CHECK(p.body == &bodies::Default() && p.peer_not_offered);
    p = bodies::ChoosePartner("", true, 250);  // unknown id (a newer build's body)
    CHECK(p.body == &bodies::Default() && p.peer_not_offered);
    p = bodies::ChoosePartner("diehardman", true, 3);
    CHECK(p.body->id == 4 && p.from_override);
    p = bodies::ChoosePartner("heartman", true, 3);
    CHECK(p.body->id == 8 && p.from_override);
    p = bodies::ChoosePartner("PORTER", false, 0);
    CHECK(p.body->id == 2 && p.from_override);
    p = bodies::ChoosePartner("gandalf", true, 3);  // unknown override: ignored, the peer's choice
    CHECK(p.body->id == 3 && !p.from_override && p.unknown_override);

    // debounce: the first id at once, a change only after kStableFrames frames in a row
    bodies::StableId s;
    CHECK(!s.Known() && s.Shown() == 0);
    CHECK(s.Push(3) && s.Known() && s.Shown() == 3);
    CHECK(!s.Push(3) && s.Shown() == 3);
    for (unsigned i = 0; i + 1 < bodies::kStableFrames; ++i) CHECK(!s.Push(5));
    CHECK(s.Shown() == 3);
    CHECK(s.Push(5) && s.Shown() == 5);  // the 30th frame
    // flapping never switches: the shown id in between restarts the count
    for (unsigned round = 0; round < 50; ++round) {
        for (unsigned i = 0; i < bodies::kStableFrames - 1; ++i) CHECK(!s.Push(1));
        CHECK(!s.Push(5));
    }
    CHECK(s.Shown() == 5);
    // alternating between two other ids never switches either
    for (unsigned i = 0; i < 200; ++i) CHECK(!s.Push(i % 2 ? 2 : 4));
    CHECK(s.Shown() == 5);
    // a run of one id interrupted by another restarts the count
    for (unsigned i = 0; i < 20; ++i) CHECK(!s.Push(2));
    CHECK(!s.Push(4));
    for (unsigned i = 0; i + 1 < bodies::kStableFrames; ++i) CHECK(!s.Push(2));
    CHECK(s.Push(2) && s.Shown() == 2);
    s.Reset();
    CHECK(!s.Known() && s.Push(4) && s.Shown() == 4);
    bodies::StableId quick(1);
    CHECK(quick.Push(1) && quick.Push(2) && quick.Shown() == 2 && !quick.Push(2));

    // an unknown body_id travels intact through STJ1 (the receiver decides what to show)
    wire::JointFrame f;
    f.joint_count = 77;
    f.body_id = 250;
    f.flags = wire::kJointsValid;
    std::uint8_t buf[wire::kMaxJointFrameSize];
    const std::size_t n = wire::EncodeJoints(f, buf, sizeof(buf));
    wire::JointFrame g;
    CHECK(n && wire::DecodeJoints(buf, n, g) && g.body_id == 250 && g.joint_count == 77);
    CHECK(bodies::ChoosePartner("", true, g.body_id).body == &bodies::Default());
    f.body_id = 0;
    CHECK(wire::EncodeJoints(f, buf, sizeof(buf)) == n && wire::DecodeJoints(buf, n, g) && g.body_id == 0);
}

int main() {
    TestPoseRoundTrip();
    TestJointFrames();
    TestJointOutfit();
    TestNameFrames();
    TestNameTagMath();
    TestGearFrames();
    TestBodyTable();
    TestBodyChoice();
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
