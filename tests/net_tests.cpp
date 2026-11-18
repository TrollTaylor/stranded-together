// Pure tests: pose codec, rotation helpers, DST1 join codes, and a full link v2
// handshake + DATA exchange between two in-process sessions.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <string>
#include <vector>

#include "body_choice.hpp"
#include "body_table.hpp"
#include "game_font.hpp"
#include "nametag_math.hpp"
#include "pose_wire.hpp"
#include "self_voice_setting.hpp"
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
    bad[8] = 1;  // version 1 (v0.7 builds)
    CHECK(!wire::DecodeGear(bad, n, g));
    std::memcpy(bad, buf, n);
    bad[9] = 57;  // count over the cap (and the size no longer matches)
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
    bad[10] = 0xF1;  // unknown flag bits are cleared (bits 1-3, the parts blocks, are known: no blocks, so not set here)
    CHECK(wire::DecodeGear(bad, n, g) && g.flags == wire::kGearValid);
    std::memcpy(bad, buf, n);
    bad[8] = 3;  // version 3 (v0.9 - v0.11 builds)
    CHECK(!wire::DecodeGear(bad, n, g));
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
    // Version 4: the boots item (kind 7, joint 0, no parent) and its parts block (docs/real-partner-body.md section 21).
    {
        wire::GearFrame b = f;
        b.flags |= wire::kGearBoots;
        b.boots_apply = 0x0000FF0Fu, b.boots_visible = 0x00000103u;
        wire::GearItem& it = b.items[b.count++];
        it.kind = wire::GearKind::boots;
        it.joint = 0;
        it.parent = wire::kGearNoParent;
        std::uint8_t bb[wire::kMaxGearFrameSize];
        const std::size_t bn = wire::EncodeGear(b, bb, sizeof(bb));
        CHECK(bn == wire::GearFrameSize(6, false, false, true) && bn == 16 + 8 + 6 * 18);
        CHECK(wire::DecodeGear(bb, bn, g) && g.count == 6 && (g.flags & wire::kGearBoots) && g.boots_apply == 0x0000FF0Fu &&
              g.boots_visible == 0x00000103u && g.items[5].kind == wire::GearKind::boots && g.items[5].joint == 0 &&
              g.items[5].res == wire::kGearNone);
        // the decoder refuses: visible bits outside apply, the item without the block, the block without the item
        std::uint8_t x[wire::kMaxGearFrameSize];
        std::memcpy(x, bb, bn);
        x[16 + 4] |= 0x10;  // visible bit 4, not in apply
        CHECK(!wire::DecodeGear(x, bn, g));
        std::memcpy(x, bb, 16);
        x[10] = static_cast<std::uint8_t>(x[10] & ~wire::kGearBoots);
        std::memcpy(x + 16, bb + 24, bn - 24);  // the same items, no block
        CHECK(!wire::DecodeGear(x, bn - 8, g));
        std::memcpy(x, buf, 16);  // the five-item frame without boots, a block put in
        x[10] = static_cast<std::uint8_t>(x[10] | wire::kGearBoots);
        std::memset(x + 16, 0, 8);
        std::memcpy(x + 24, buf + 16, n - 16);
        CHECK(!wire::DecodeGear(x, n + 8, g));
        // the encoder refuses: no block, a block and no item, two boots items, boots on another joint or an item,
        // visible outside apply
        wire::GearFrame e2 = b;
        e2.flags = static_cast<std::uint8_t>(e2.flags & ~wire::kGearBoots);
        CHECK(wire::EncodeGear(e2, bb, sizeof(bb)) == 0);
        e2 = f;
        e2.flags |= wire::kGearBoots;
        CHECK(wire::EncodeGear(e2, bb, sizeof(bb)) == 0);
        e2 = b;
        e2.items[e2.count++] = it;
        CHECK(wire::EncodeGear(e2, bb, sizeof(bb)) == 0);
        e2 = b;
        e2.items[5].joint = 63;
        CHECK(wire::EncodeGear(e2, bb, sizeof(bb)) == 0);
        e2 = b;
        e2.items[5].parent = 0;
        CHECK(wire::EncodeGear(e2, bb, sizeof(bb)) == 0);
        e2 = b;
        e2.boots_visible = 0x80000000u;
        CHECK(wire::EncodeGear(e2, bb, sizeof(bb)) == 0);
        // the boots item is part of the layout
        CHECK(wire::GearLayoutHash(b) != wire::GearLayoutHash(f));
    }
    // The full cap fits a link body next to nothing else (1148 bytes) and decodes.
    wire::GearFrame full;
    full.flags = wire::kGearValid | wire::kGearParts | wire::kGearPackParts | wire::kGearBoots;
    full.parts_apply[0] = ~0ull, full.parts_apply[1] = 0xFFFF;
    full.pack_apply = 0xFFFFFF;
    full.boots_apply = ~0u, full.boots_visible = 0x00F0F0F0u;
    for (unsigned i = 0; i < wire::kMaxGearItems; ++i) {
        wire::GearItem& it = full.items[full.count++];
        const bool boots = i + 1 == wire::kMaxGearItems;
        it.kind = i == 0 ? wire::GearKind::backpack : boots ? wire::GearKind::boots : wire::GearKind::cargo;
        it.model = i == 0 || boots ? wire::kGearNone : static_cast<std::uint8_t>(1 + i % 6);
        it.joint = boots ? 0 : 4;
        it.parent = i == 0 || boots ? wire::kGearNoParent : static_cast<std::uint8_t>(i - 1);
        it.p[2] = boots ? 0.0f : 0.3f;
    }
    const std::size_t fn = wire::EncodeGear(full, buf, sizeof(buf));
    CHECK(fn == wire::kMaxGearFrameSize && fn <= 1148 - 4);
    CHECK(wire::DecodeGear(buf, fn, g) && g.count == wire::kMaxGearItems && g.items[39].parent == 38 && g.boots_visible == 0x00F0F0F0u);
}

// STC1 version 3 (entity and weapon items, the parts block), STR1 resource frames, prop reuse across layouts and
// the retry schedule of failed props (docs/real-partner-body.md section 12).
// STV1 (gear looks) and STK1 (the backpack's skinned pose), docs/real-partner-body.md section 13.
void TestGearLook() {
    wire::GearLook l;
    l.item = 7;
    l.layout = 0xBEEF;
    l.kind = wire::GearKind::entity;
    l.res = 12;
    l.nvar = 2;
    for (unsigned k = 0; k < 16; ++k) l.guid[0][k] = static_cast<std::uint8_t>(k), l.guid[1][k] = static_cast<std::uint8_t>(0xF0 + k);
    l.nsv = 3;
    l.sv[0].hash = 0x3A3714EDu, l.sv[0].part = wire::kLookAllParts, l.sv[0].count = 1, l.sv[0].v[0] = 9.0f;
    l.sv[1].hash = 0x4AB95B99u, l.sv[1].part = 5, l.sv[1].count = 3, l.sv[1].v[0] = 0.1f, l.sv[1].v[1] = 0.2f, l.sv[1].v[2] = 0.3f;
    l.sv[1].v[3] = 77.0f;  // beyond count: not sent
    l.sv[2].hash = 0x42661929u, l.sv[2].part = 0, l.sv[2].count = 4;
    std::uint8_t buf[wire::kMaxGearLookFrameSize];
    const std::size_t n = wire::EncodeGearLook(l, buf, sizeof(buf));
    CHECK(n == wire::GearLookSize(2, 3) && n == 28 + 32 + 72);
    wire::GearLook m;
    CHECK(wire::DecodeGearLook(buf, n, m));
    CHECK(m.item == 7 && m.layout == 0xBEEF && m.kind == wire::GearKind::entity && m.res == 12 && m.nvar == 2 && m.nsv == 3);
    CHECK(std::memcmp(m.guid[1], l.guid[1], 16) == 0);
    CHECK(m.sv[0].hash == 0x3A3714EDu && m.sv[0].part == wire::kLookAllParts && m.sv[0].count == 1 && m.sv[0].v[0] == 9.0f);
    CHECK(m.sv[1].part == 5 && m.sv[1].count == 3 && m.sv[1].v[2] == 0.3f && m.sv[1].v[3] == 0.0f);
    // v[3] beyond count is not sent: equal once the original drops it too
    l.sv[1].v[3] = 0.0f;
    CHECK(wire::GearLookHash(m) == wire::GearLookHash(l));
    // item / layout are not part of the hash; a value change is (beyond 1/256)
    m.item = 3, m.layout = 1;
    CHECK(wire::GearLookHash(m) == wire::GearLookHash(l));
    m.sv[0].v[0] = 9.01f;
    CHECK(wire::GearLookHash(m) != wire::GearLookHash(l));
    m.sv[0].v[0] = 9.001f;
    CHECK(wire::GearLookHash(m) == wire::GearLookHash(l));
    // a variation's order counts
    wire::GearLook o = l;
    std::memcpy(o.guid[0], l.guid[1], 16), std::memcpy(o.guid[1], l.guid[0], 16);
    CHECK(wire::GearLookHash(o) != wire::GearLookHash(l));
    // a non-entity kind sends no resource id
    o = l;
    o.kind = wire::GearKind::backpack;
    CHECK(wire::DecodeGearLook(buf, wire::EncodeGearLook(o, buf, sizeof(buf)), m) && m.res == wire::kGearNone);
    // the largest look fits a link body
    o = l;
    o.nvar = wire::kMaxLookVariations, o.nsv = wire::kMaxLookShaderVars;
    for (unsigned i = 0; i < o.nsv; ++i) o.sv[i].count = 4, o.sv[i].part = static_cast<std::uint16_t>(i);
    CHECK(wire::EncodeGearLook(o, buf, sizeof(buf)) == 924);
    static_assert(wire::kMaxGearLookFrameSize <= 1148 - 8, "STV1 fits a link body");
    // refused: caps, kinds, counts, parts, non-finite values, sizes, magic, version
    o.nvar = wire::kMaxLookVariations + 1;
    CHECK(wire::EncodeGearLook(o, buf, sizeof(buf)) == 0);
    o = l;
    o.item = wire::kMaxGearItems;
    CHECK(wire::EncodeGearLook(o, buf, sizeof(buf)) == 0);
    o = l;
    o.kind = static_cast<wire::GearKind>(0);
    CHECK(wire::EncodeGearLook(o, buf, sizeof(buf)) == 0);
    o = l;
    o.sv[0].count = 0;
    CHECK(wire::EncodeGearLook(o, buf, sizeof(buf)) == 0);
    o.sv[0].count = 5;
    CHECK(wire::EncodeGearLook(o, buf, sizeof(buf)) == 0);
    o = l;
    o.sv[2].part = 0x200;
    CHECK(wire::EncodeGearLook(o, buf, sizeof(buf)) == 0);
    o = l;
    o.sv[1].v[1] = std::nanf("");
    CHECK(wire::EncodeGearLook(o, buf, sizeof(buf)) == 0);
    o.sv[1].v[1] = 0.2f;
    o.sv[1].v[3] = std::nanf("");  // beyond count: not sent, fine
    CHECK(wire::EncodeGearLook(o, buf, sizeof(buf)) == n);
    const std::size_t n2 = wire::EncodeGearLook(l, buf, sizeof(buf));
    CHECK(!wire::DecodeGearLook(buf, n2 - 1, m) && !wire::DecodeGearLook(buf, n2 + 1, m));
    std::uint8_t bad[wire::kMaxGearLookFrameSize + 4];
    std::memcpy(bad, buf, n2);
    bad[3] = '2';
    CHECK(!wire::DecodeGearLook(bad, n2, m));
    std::memcpy(bad, buf, n2);
    bad[4] = 2;  // version
    CHECK(!wire::DecodeGearLook(bad, n2, m));
    std::memcpy(bad, buf, n2);
    bad[5] = static_cast<std::uint8_t>(wire::kMaxGearItems);  // item
    CHECK(!wire::DecodeGearLook(bad, n2, m));
    std::memcpy(bad, buf, n2);
    bad[10] = 3;  // nvar 3: the size no longer matches
    CHECK(!wire::DecodeGearLook(bad, n2, m));
    std::memcpy(bad, buf, n2);
    bad[28 + 32 + 24 + 6] = 0;  // the second variable's count 0
    CHECK(!wire::DecodeGearLook(bad, n2, m));
    std::memcpy(bad, buf, n2);
    const float inf = std::numeric_limits<float>::infinity();
    std::memcpy(bad + 28 + 32 + 8, &inf, 4);  // the first variable's value
    CHECK(!wire::DecodeGearLook(bad, n2, m));
    // other frames are not looks, and a look is none of them
    wire::GearRes r;
    wire::GearFrame gf;
    CHECK(!wire::DecodeGearRes(buf, n2, r) && !wire::DecodeGear(buf, n2, gf));
    // an empty look (a case without a variation) is valid
    o = wire::GearLook{};
    o.kind = wire::GearKind::cargo;
    CHECK(wire::EncodeGearLook(o, buf, sizeof(buf)) == 28 && wire::DecodeGearLook(buf, 28, m) && m.nvar == 0 && m.nsv == 0);
    // the parts: shown bits outside apply are dropped; a part change is a look change
    o.parts_apply = 0x1FF, o.parts_visible = 0x1002;
    CHECK(wire::DecodeGearLook(buf, wire::EncodeGearLook(o, buf, sizeof(buf)), m) && m.parts_apply == 0x1FF && m.parts_visible == 0x2);
    wire::GearLook o2 = m;
    o2.parts_visible = 0x4;
    CHECK(wire::GearLookHash(o2) != wire::GearLookHash(m));
    o2.parts_visible = 0x2 | 0x1000;  // outside apply: the same look
    CHECK(wire::GearLookHash(o2) == wire::GearLookHash(m));

    // STK1
    wire::GearPose p;
    p.item = 0;
    p.layout = 0x1234;
    p.skeleton = 0x81671A7Du;
    p.n = 42;
    for (unsigned j = 0; j < p.n; ++j) {
        const float a = 0.05f * static_cast<float>(j);
        p.q[j] = wire::Normalize(wire::Quat{std::sin(a), 0.1f, 0.0f, std::cos(a)});
        p.t[j][0] = 0.001f * static_cast<float>(j), p.t[j][1] = -0.25f, p.t[j][2] = j == 5 ? 3.19f : 0.5f;
    }
    std::uint8_t pb[wire::kMaxGearPoseFrameSize];
    const std::size_t pn = wire::EncodeGearPose(p, pb, sizeof(pb));
    CHECK(pn == wire::GearPoseSize(42) && pn == 16 + 42 * 12);
    wire::GearPose q;
    CHECK(wire::DecodeGearPose(pb, pn, q));
    CHECK(q.item == 0 && q.layout == 0x1234 && q.skeleton == 0x81671A7Du && q.n == 42);
    float dm = 1, dd = 1;
    CHECK(wire::GearPoseDiff(p, q, &dm, &dd) && dm < 1e-4f && dd < 0.05f);
    CHECK(std::fabs(q.t[5][2] - 3.19f) < 1e-4f);
    q.t[3][1] += 0.01f;
    CHECK(wire::GearPoseDiff(p, q, &dm, &dd) && std::fabs(dm - 0.01f) < 2e-4f);
    q.skeleton = 1;
    CHECK(!wire::GearPoseDiff(p, q, &dm, &dd));
    static_assert(wire::kMaxGearPoseFrameSize == 784 && wire::kMaxGearPoseFrameSize <= 1148 - 8, "STK1 fits a link body");
    wire::GearPose b = p;
    b.t[0][0] = 3.3f;  // beyond +-3.2 m
    CHECK(wire::EncodeGearPose(b, pb, sizeof(pb)) == 0);
    b = p;
    b.q[1].x = std::nanf("");
    CHECK(wire::EncodeGearPose(b, pb, sizeof(pb)) == 0);
    b = p;
    b.n = 0;
    CHECK(wire::EncodeGearPose(b, pb, sizeof(pb)) == 0);
    b.n = wire::kMaxGearPoseJoints + 1;
    CHECK(wire::EncodeGearPose(b, pb, sizeof(pb)) == 0);
    b = p;
    b.item = wire::kMaxGearItems;
    CHECK(wire::EncodeGearPose(b, pb, sizeof(pb)) == 0);
    const std::size_t pn2 = wire::EncodeGearPose(p, pb, sizeof(pb));
    CHECK(!wire::DecodeGearPose(pb, pn2 - 1, q) && !wire::DecodeGearPose(pb, pn2 + 12, q));
    std::uint8_t pbad[wire::kMaxGearPoseFrameSize];
    std::memcpy(pbad, pb, pn2);
    pbad[12] = 41;  // the joint count no longer matches the size
    CHECK(!wire::DecodeGearPose(pbad, pn2, q));
    std::memcpy(pbad, pb, pn2);
    pbad[4] = 9;  // version
    CHECK(!wire::DecodeGearPose(pbad, pn2, q));
    CHECK(!wire::DecodeGearLook(pb, pn2, m) && !wire::DecodeGear(pb, pn2, gf));
}

void TestGearV2() {
    wire::GearFrame f;
    f.seq = 9;
    f.flags = wire::kGearValid | wire::kGearParts;
    f.parts_apply[0] = 0x0000000F00000004ull, f.parts_apply[1] = 0x1;
    f.parts_visible[0] = 0x0000000500000004ull | 0x100, f.parts_visible[1] = 0x3;  // bits outside apply are dropped
    auto add = [&f](wire::GearKind k, std::uint8_t res, std::uint8_t joint, std::uint8_t parent) {
        wire::GearItem& it = f.items[f.count++];
        it.kind = k;
        it.res = res;
        it.joint = joint;
        it.parent = parent;
        it.p[1] = -0.1f;
    };
    add(wire::GearKind::backpack, wire::kGearNone, 4, wire::kGearNoParent);
    add(wire::GearKind::entity, 0, 0, 0);                        // on the backpack
    add(wire::GearKind::entity, 63, 9, wire::kGearNoParent);     // in the right hand
    add(wire::GearKind::cargo, 7, 1, wire::kGearNoParent);       // a cargo item's res is not sent
    f.items[3].model = 2;
    std::uint8_t buf[wire::kMaxGearFrameSize];
    const std::size_t n = wire::EncodeGear(f, buf, sizeof(buf));
    CHECK(n == wire::GearFrameSize(4, true) && n == 16 + 32 + 4 * 18);
    wire::GearFrame g;
    CHECK(wire::DecodeGear(buf, n, g));
    CHECK(g.flags == (wire::kGearValid | wire::kGearParts) && g.count == 4);
    CHECK(g.parts_apply[0] == f.parts_apply[0] && g.parts_apply[1] == 1);
    CHECK(g.parts_visible[0] == 0x0000000500000004ull && g.parts_visible[1] == 1);
    CHECK(g.items[1].kind == wire::GearKind::entity && g.items[1].res == 0 && g.items[1].parent == 0);
    CHECK(g.items[2].res == 63 && g.items[2].joint == 9);
    CHECK(g.items[3].res == wire::kGearNone && g.items[0].res == wire::kGearNone);
    // The resource id is part of the layout; the parts are not.
    wire::GearFrame o = f;
    o.items[2].res = 5;
    CHECK(wire::GearLayoutHash(o) != wire::GearLayoutHash(f));
    o = f;
    o.parts_visible[0] = 0;
    CHECK(wire::GearLayoutHash(o) == wire::GearLayoutHash(f));
    // An entity item without a valid id is refused both ways.
    o = f;
    o.items[2].res = 64;
    CHECK(wire::EncodeGear(o, buf, sizeof(buf)) == 0);
    std::uint8_t bad[wire::kMaxGearFrameSize];
    std::memcpy(bad, buf, n);
    bad[48 + 18 * 2 + 5] = 64;
    CHECK(!wire::DecodeGear(bad, n, g));
    // The parts flag without the block (or the reverse) does not size up.
    std::memcpy(bad, buf, n);
    bad[10] = wire::kGearValid;
    CHECK(!wire::DecodeGear(bad, n, g));
    // Without parts: 16 + 18 n.
    o = f;
    o.flags = wire::kGearValid;
    CHECK(wire::EncodeGear(o, buf, sizeof(buf)) == wire::GearFrameSize(4));
    // The backpack's parts block (after the parts block; alone too).
    o = f;
    o.flags |= wire::kGearPackParts;
    o.pack_apply = 0xFFFFFF;
    o.pack_visible = 0x1C0021 | (1ull << 40);  // a bit outside apply is dropped
    std::size_t on = wire::EncodeGear(o, buf, sizeof(buf));
    CHECK(on == wire::GearFrameSize(4, true, true) && on == 16 + 32 + 16 + 4 * 18);
    CHECK(wire::DecodeGear(buf, on, g) && g.pack_apply == 0xFFFFFF && g.pack_visible == 0x1C0021 && g.parts_apply[1] == 1);
    o.flags = wire::kGearValid | wire::kGearPackParts;
    on = wire::EncodeGear(o, buf, sizeof(buf));
    CHECK(on == wire::GearFrameSize(4, false, true) && wire::DecodeGear(buf, on, g) && g.pack_visible == 0x1C0021 && g.parts_apply[0] == 0);
    CHECK(!wire::DecodeGear(buf, on - 1, g));

    // Version 3: the weapon kind (docs/real-partner-body.md section 17) keeps its resource id (part of the
    // layout), its EDSWeaponId (model) and its level (variation); a weapon without a valid id, a kind above
    // weapon and a version 2 frame are refused.
    CHECK(wire::GearHasRes(wire::GearKind::weapon) && wire::GearHasRes(wire::GearKind::entity) && !wire::GearHasRes(wire::GearKind::cargo));
    o = f;
    o.flags = wire::kGearValid;
    o.items[2].kind = wire::GearKind::weapon;
    o.items[2].model = 45;
    o.items[2].variation = 4;
    o.items[2].res = 24;
    o.items[2].joint = 26;
    on = wire::EncodeGear(o, buf, sizeof(buf));
    CHECK(on == wire::GearFrameSize(4) && buf[8] == wire::kGearVersion && wire::kGearVersion == 4);
    CHECK(wire::DecodeGear(buf, on, g) && g.items[2].kind == wire::GearKind::weapon && g.items[2].res == 24 && g.items[2].model == 45 &&
          g.items[2].variation == 4 && g.items[2].joint == 26 && g.items[2].parent == wire::kGearNoParent);
    wire::GearFrame w2 = o;
    w2.items[2].res = 25;
    CHECK(wire::GearLayoutHash(w2) != wire::GearLayoutHash(o));
    w2 = o;
    w2.items[2].res = 64;
    CHECK(wire::EncodeGear(w2, buf, sizeof(buf)) == 0);
    w2 = o;
    w2.items[2].kind = static_cast<wire::GearKind>(8);  // unknown since version 4 (7 is boots)
    CHECK(wire::EncodeGear(w2, buf, sizeof(buf)) == 0);
    on = wire::EncodeGear(o, buf, sizeof(buf));
    buf[8] = 2;
    CHECK(!wire::DecodeGear(buf, on, g));
    // a weapon's look carries its resource id too
    wire::GearLook wl;
    wl.item = 2;
    wl.kind = wire::GearKind::weapon;
    wl.res = 24;
    std::uint8_t lb[wire::kMaxGearLookFrameSize];
    const std::size_t ln = wire::EncodeGearLook(wl, lb, sizeof(lb));
    wire::GearLook wl2;
    CHECK(ln && wire::DecodeGearLook(lb, ln, wl2) && wl2.kind == wire::GearKind::weapon && wl2.res == 24);

    // STR1
    wire::GearRes r;
    r.id = 17;
    for (int i = 0; i < 16; ++i) r.guid[i] = static_cast<std::uint8_t>(0xA0 + i);
    std::snprintf(r.path, sizeof(r.path), "%s", "ds/entities/player/ds_player_inventory");
    std::uint8_t rb[wire::kMaxGearResFrameSize];
    const std::size_t rn = wire::EncodeGearRes(r, rb, sizeof(rb));
    CHECK(rn == 24 + std::strlen(r.path));
    wire::GearRes rr;
    CHECK(wire::DecodeGearRes(rb, rn, rr) && rr.id == 17 && std::memcmp(rr.guid, r.guid, 16) == 0 && std::strcmp(rr.path, r.path) == 0);
    CHECK(!wire::DecodeGearRes(rb, rn - 1, rr) && !wire::DecodeGearRes(rb, rn + 1, rr));
    std::uint8_t rbad[wire::kMaxGearResFrameSize];
    std::memcpy(rbad, rb, rn);
    rbad[5] = 64;  // id over the table
    CHECK(!wire::DecodeGearRes(rbad, rn, rr));
    std::memcpy(rbad, rb, rn);
    rbad[24] = 'x';  // not under ds/entities/
    CHECK(!wire::DecodeGearRes(rbad, rn, rr));
    std::memcpy(rbad, rb, rn);
    rbad[30] = '\n';  // a control byte in the path
    CHECK(!wire::DecodeGearRes(rbad, rn, rr));
    CHECK(wire::GearResPathOk("ds/entities/weapons/weapon_collection") && !wire::GearResPathOk("ds/entities/../x") &&
          !wire::GearResPathOk("ds/entities/Weapons/x") && !wire::GearResPathOk("ds/entities/") && !wire::GearResPathOk("models/x"));
    {
        // the receiver's check before loading (review finding 1): proven cores only, refused entities never
        const std::uint8_t any[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        // b5041f35-b5a5-564c-8b84-76587a06f97a (the BB pod entity), stored as the game keeps it
        const std::uint8_t pod[16] = {0x35, 0x1f, 0x04, 0xb5, 0xa5, 0xb5, 0x4c, 0x56, 0x8b, 0x84, 0x76, 0x58, 0x7a, 0x06, 0xf9, 0x7a};
        const std::uint8_t cyl[16] = {0x27, 0x36, 0x03, 0xf5, 0xf0, 0x24, 0x41, 0x14, 0xa7, 0xd4, 0xe7, 0xf1, 0xe9, 0x0c, 0x68, 0xfa};
        char txt[37];
        wire::GearGuidText(pod, txt);
        CHECK(std::strcmp(txt, "b5041f35-b5a5-564c-8b84-76587a06f97a") == 0);
        const char* why = nullptr;
        CHECK(wire::GearResProven("ds/entities/weapons/weapon_collection", any, false, &why));
        CHECK(wire::GearResProven("ds/entities/item/backpack/backpack_temp", any, false, &why));
        CHECK(wire::GearResProven("ds/entities/item/backpack/other_item", any, false, &why));
        CHECK(wire::GearResProven("ds/entities/player/ds_player_inventory", any, false, &why));
        CHECK(!wire::GearResProven("ds/entities/player/ds_odradek", any, false, &why) && why);
        CHECK(!wire::GearResProven("ds/entities/weapons/builder", any, false, &why));
        CHECK(!wire::GearResProven("ds/entities/weapons/weapon_collection2", any, false, &why));
        // a weapon's own resource: never loaded at any mirror level (the three proven entities of the folder are not weapons)
        CHECK(wire::GearResWeaponOnly("ds/entities/weapons/assault_rifle"));
        CHECK(wire::GearResWeaponOnly("ds/entities/weapons/builder"));
        CHECK(wire::GearResWeaponOnly("ds/entities/weapons/weapon_collection2"));
        CHECK(!wire::GearResWeaponOnly("ds/entities/weapons/weapon_collection"));
        CHECK(!wire::GearResWeaponOnly("ds/entities/weapons/sam_cant"));
        CHECK(!wire::GearResWeaponOnly("ds/entities/weapons/sam_boot"));
        CHECK(!wire::GearResWeaponOnly("ds/entities/item/backpack/backpack_temp"));
        CHECK(!wire::GearResWeaponOnly(nullptr));
        CHECK(!wire::GearResProven("ds/entities/player/ds_player_inventory", pod, false, &why));
        CHECK(wire::GearResProven("ds/entities/player/ds_player_inventory", pod, true, &why));
        CHECK(!wire::GearResProven("ds/entities/item/backpack/backpack_temp", cyl, true, &why));
        CHECK(!wire::GearResProven("ds/entities/../entities/item/backpack/x", any, false, &why));
    }
    std::snprintf(r.path, sizeof(r.path), "%s", "c:/windows");
    CHECK(wire::EncodeGearRes(r, rb, sizeof(rb)) == 0);
    // An STR1 frame is no other frame, and no other frame is an STR1 frame.
    wire::GearFrame gf2;
    wire::Pose pose;
    CHECK(!wire::DecodeGear(rb, rn, gf2) && !wire::DecodePose(rb, rn, pose) && !wire::DecodeGearRes(buf, n, rr));

    // Prop reuse: one piece removed from the middle keeps every later prop (moved, not respawned).
    {
        const int old_keys[6] = {0, 9, 10, 11, 9, -1};
        const int want[5] = {0, 10, 11, 9, 12};  // item 1 (key 9) gone, a new key 12 at the end
        int map[5];
        wire::GearAssignProps(old_keys, 6, want, 5, map);
        CHECK(map[0] == 0 && map[1] == 2 && map[2] == 3 && (map[3] == 1 || map[3] == 4) && map[4] == -1);
        // each old slot used once
        const int want2[3] = {9, 9, 9};
        int map2[3];
        wire::GearAssignProps(old_keys, 6, want2, 3, map2);
        CHECK(map2[1] == 1 && map2[0] == 4 && map2[2] == -1);  // index 1 keeps its own, item 0 takes the other
        // the same index wins when its key still matches
        const int want3[2] = {9, 0};
        int map3[2];
        wire::GearAssignProps(old_keys, 6, want3, 2, map3);
        CHECK(map3[0] == 1 && map3[1] == 0);
        // no prop wanted: -1
        const int want4[1] = {-1};
        int map4[1];
        wire::GearAssignProps(old_keys, 6, want4, 1, map4);
        CHECK(map4[0] == -1);
    }
    // Retries: 2, 4, 8, 16, 30, 30 s.
    CHECK(wire::GearRetryDelayMs(0) == 0 && wire::GearRetryDelayMs(1) == 2000 && wire::GearRetryDelayMs(2) == 4000 &&
          wire::GearRetryDelayMs(4) == 16000 && wire::GearRetryDelayMs(5) == 30000 && wire::GearRetryDelayMs(40) == 30000);
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

// The game-font parser (game_font.cpp) on a synthetic FontResource (made-up outlines, no game data):
// layout, validation of counts and point use, the exact-end rule, GUID check, and the path hash.
static void PutU32(std::vector<std::uint8_t>& b, std::uint32_t v) { b.insert(b.end(), reinterpret_cast<std::uint8_t*>(&v), reinterpret_cast<std::uint8_t*>(&v) + 4); }
static void PutF(std::vector<std::uint8_t>& b, float v) { b.insert(b.end(), reinterpret_cast<std::uint8_t*>(&v), reinterpret_cast<std::uint8_t*>(&v) + 4); }
static std::vector<std::uint8_t> SyntheticFont(std::uint32_t pts_in_square, bool extra_byte) {
    std::vector<std::uint8_t> body;
    for (int i = 0; i < 16; ++i) body.push_back(static_cast<std::uint8_t>(i + 1));  // ObjectUUID
    PutU32(body, 4), PutU32(body, 0), body.insert(body.end(), {'T', 'e', 's', 't'});  // TypefaceName (len, crc, bytes)
    PutF(body, 1000), PutF(body, 800), PutF(body, 200), PutF(body, 700);
    PutU32(body, 2);
    // 'B' (sorted after ' ' by the parser): a square (4 line-to) and a curve contour (one 3-point spline)
    PutF(body, 600), PutU32(body, 'B');
    for (int i = 0; i < 4; ++i) PutF(body, 0);
    PutU32(body, 2);
    PutU32(body, 1), body.push_back(0x04);  // type 0, k 4
    PutU32(body, pts_in_square);
    for (std::uint32_t i = 0; i < pts_in_square; ++i) PutF(body, static_cast<float>(i % 2 ? 700 : 100)), PutF(body, static_cast<float>(i < 2 ? 0 : 700));
    PutU32(body, 1), body.push_back(0x43);  // type 1, k 3: two controls + end
    PutU32(body, 4);
    for (int i = 0; i < 4; ++i) PutF(body, 200.0f + 50 * i), PutF(body, -100.0f + 10 * i);
    PutF(body, 250), PutU32(body, ' ');
    for (int i = 0; i < 4; ++i) PutF(body, 0);
    PutU32(body, 0);
    PutU32(body, 1), PutU32(body, 'A'), PutU32(body, 'B'), PutF(body, -20);  // kerning
    if (extra_byte) body.push_back(0);
    std::vector<std::uint8_t> core;
    const std::uint64_t other = 0x1111111111111111ull, tid = gamefont::kFontResourceTypeId;
    core.insert(core.end(), reinterpret_cast<const std::uint8_t*>(&other), reinterpret_cast<const std::uint8_t*>(&other) + 8);
    PutU32(core, 3), core.insert(core.end(), {9, 9, 9});  // an unrelated object first
    core.insert(core.end(), reinterpret_cast<const std::uint8_t*>(&tid), reinterpret_cast<const std::uint8_t*>(&tid) + 8);
    PutU32(core, static_cast<std::uint32_t>(body.size()));
    core.insert(core.end(), body.begin(), body.end());
    return core;
}

void TestGameFontParser() {
    char why[200] = {};
    gamefont::Font f;
    std::vector<std::uint8_t> ok = SyntheticFont(5, false);
    CHECK(gamefont::ParseFontCore(ok.data(), ok.size(), nullptr, &f, why, sizeof(why)));
    CHECK(f.typeface == "Test" && f.height == 1000 && f.ascent == 800 && f.descent == 200);
    CHECK(f.glyphs.size() == 2 && f.glyphs[0].cp == ' ' && f.glyphs[1].cp == 'B' && f.kerning_pairs == 1);
    const gamefont::Glyph* b = f.Find('B');
    CHECK(b && b->n_contours == 2 && b->advance == 600 && b->min_y == -100 && b->max_x == 700 && b->max_y == 700);
    CHECK(f.contours.size() == 2 && f.contours[1].n_pt == 4 && f.cmds.size() == 2 && f.pts.size() == 2 * 9);
    CHECK(!f.Find('A') && f.Find(' ') && f.Find(' ')->n_contours == 0);
    std::uint8_t guid[16];
    for (int i = 0; i < 16; ++i) guid[i] = static_cast<std::uint8_t>(i + 1);
    CHECK(gamefont::ParseFontCore(ok.data(), ok.size(), guid, &f, why, sizeof(why)));
    CHECK(!gamefont::ParseFontCore(ok.data(), ok.size(), gamefont::kMenuFontGuid, &f, why, sizeof(why)));
    std::vector<std::uint8_t> bad_pts = SyntheticFont(6, false);  // 6 points for 1 + 4 used
    CHECK(!gamefont::ParseFontCore(bad_pts.data(), bad_pts.size(), nullptr, &f, why, sizeof(why)) && f.glyphs.empty());
    std::vector<std::uint8_t> extra = SyntheticFont(5, true);  // the parse must end exactly at the object's size
    CHECK(!gamefont::ParseFontCore(extra.data(), extra.size(), nullptr, &f, why, sizeof(why)));
    for (std::size_t cut = 0; cut < ok.size(); cut += 7)  // every truncation is refused, never read past
        CHECK(!gamefont::ParseFontCore(ok.data(), cut, nullptr, &f, why, sizeof(why)));
    // Decima path hashes (the same as tools/re/ds_archive.py hash).
    CHECK(gamefont::PathHash("interface/fonts/ds/expsmediumneon.core") == 0x8B1154249961D97Aull);
    CHECK(gamefont::PathHash("Interface\\Fonts\\DS\\ExpsMediumNeon.core") == 0x8B1154249961D97Aull);
}

// session.ini self_voice (docs/audio-research.md section 9, Phase 1): the values, and who is muted under auto.
void TestSelfVoiceSetting() {
    using selfvoice::Mode;
    Mode m = Mode::sam;
    CHECK(selfvoice::ParseMode("auto", &m) && m == Mode::automatic);
    CHECK(selfvoice::ParseMode("mute", &m) && m == Mode::mute);
    CHECK(selfvoice::ParseMode("sam", &m) && m == Mode::sam);
    for (const char* bad : {"", "Auto", "MUTE", "on", "1", "fragile", "auto ", " sam", "mute\n"}) {
        m = Mode::mute;
        CHECK(!selfvoice::ParseMode(bad, &m) && m == Mode::mute);  // refused, the value untouched
    }
    CHECK(selfvoice::ParseMode("auto", nullptr) && !selfvoice::ParseMode("x", nullptr));
    for (Mode x : {Mode::automatic, Mode::sam, Mode::mute}) CHECK(selfvoice::ParseMode(selfvoice::ModeName(x), &m) && m == x);
    // Sam's keys (any case, sam_* variants); unset counts as Sam (the mod sends sam).
    CHECK(selfvoice::IsSamCharacter("sam") && selfvoice::IsSamCharacter("Sam") && selfvoice::IsSamCharacter("sam_mule"));
    CHECK(selfvoice::IsSamCharacter("") && selfvoice::IsSamCharacter(nullptr));
    CHECK(!selfvoice::IsSamCharacter("fragile_suit") && !selfvoice::IsSamCharacter("fragile") && !selfvoice::IsSamCharacter("samurai") &&
          !selfvoice::IsSamCharacter("sa") && !selfvoice::IsSamCharacter("s") && !selfvoice::IsSamCharacter("porter"));
    // auto: muted only for a character other than Sam; sam never; mute always.
    CHECK(selfvoice::ResolveMute(Mode::automatic, "fragile_suit") && selfvoice::ResolveMute(Mode::automatic, "mama"));
    CHECK(!selfvoice::ResolveMute(Mode::automatic, "sam") && !selfvoice::ResolveMute(Mode::automatic, "sam_mule") &&
          !selfvoice::ResolveMute(Mode::automatic, ""));
    CHECK(!selfvoice::ResolveMute(Mode::sam, "fragile_suit") && selfvoice::ResolveMute(Mode::mute, "sam"));
    // Every offered body except Sam's is muted under auto (the character list the launcher offers).
    for (std::size_t i = 0; i < bodies::OfferedCount(); ++i) {
        const bodies::Body& b = bodies::OfferedAt(i);
        const bool sam = std::strncmp(b.key, "sam", 3) == 0;
        CHECK(selfvoice::ResolveMute(Mode::automatic, b.key) == !sam);
    }
    // What the mod resolves: ChooseOwn makes the host Sam whatever my_character says.
    CHECK(!selfvoice::ResolveMute(Mode::automatic, bodies::ChooseOwn(true, "fragile_suit").body->key));
    CHECK(selfvoice::ResolveMute(Mode::automatic, bodies::ChooseOwn(false, "fragile_suit").body->key));
    CHECK(!selfvoice::ResolveMute(Mode::automatic, bodies::ChooseOwn(false, "gandalf").body->key));  // unknown: sam
}

// ---- partner voice: STS1 frame (docs/audio-research.md sections 7 and 15)
// Partner sounds (docs/audio-research.md section 16): STA1 round trip, every refusal, the size limit, and the other
// decoders refuse it (and it refuses theirs).
void TestSoundFrames() {
    wire::SoundFrame a;
    a.seq = 65535;
    a.sender_ms = 0xFFFFFF00u;
    a.n = 4;
    wire::SoundItem& post = a.item[0];
    post.op = 0, post.a = 0x0B509EF4u, post.b = 1234, post.slot = 1, post.tag = 7, post.category = 1, post.age_ms = 17;
    post.off[0] = -12, post.off[1] = 20, post.off[2] = 5;
    post.nrtpc = 2;
    post.rtpc_id[0] = 0x1111u, post.rtpc_value[0] = 0.5f, post.rtpc_id[1] = 0x2222u, post.rtpc_value[1] = -3.25f;
    wire::SoundItem& stop = a.item[1];
    stop.op = 1, stop.b = 1234, stop.fade_ms = 300, stop.category = 2;
    wire::SoundItem& act = a.item[2];
    act.op = 2, act.a = 0x6D69D583u, act.slot = 3, act.tag = 9, act.fade_ms = 100;
    wire::SoundItem& rt = a.item[3];
    rt.op = 3, rt.slot = 6, rt.tag = 7, rt.nrtpc = 1, rt.rtpc_id[0] = 0x3333u, rt.rtpc_value[0] = 42.0f, rt.fade_ms = 50;
    const std::size_t want = wire::kSoundHeaderSize + 4 * wire::kSoundItemSize + 3 * wire::kSoundRtpcSize;
    CHECK(wire::SoundFrameSize(a) == want && want == 132);
    // one item checked alone (the sender drops an invalid item, not the whole frame)
    CHECK(wire::SoundItemValid(post) && wire::SoundItemValid(stop) && wire::SoundItemValid(act) && wire::SoundItemValid(rt));
    {
        wire::SoundItem bad = post;
        bad.op = 9;
        CHECK(!wire::SoundItemValid(bad));
        bad = stop;
        bad.b = 0;  // a stop without a playing id
        CHECK(!wire::SoundItemValid(bad));
        bad = post;
        bad.rtpc_value[0] = std::numeric_limits<float>::quiet_NaN();
        CHECK(!wire::SoundItemValid(bad));
    }
    std::uint8_t buf[wire::kMaxSoundFrameSize + 16];
    const std::size_t size = wire::EncodeSound(a, buf, sizeof(buf));
    CHECK(size == want);
    CHECK(std::memcmp(buf, "STA1", 4) == 0 && buf[4] == 1 && buf[5] == 4);
    static wire::SoundFrame b;
    CHECK(wire::DecodeSound(buf, size, b));
    CHECK(b.n == 4 && b.seq == 65535 && b.sender_ms == 0xFFFFFF00u);
    CHECK(b.item[0].op == 0 && b.item[0].a == 0x0B509EF4u && b.item[0].b == 1234 && b.item[0].slot == 1 && b.item[0].tag == 7 && b.item[0].category == 1 &&
          b.item[0].age_ms == 17 && b.item[0].off[0] == -12 && b.item[0].off[1] == 20 && b.item[0].off[2] == 5 && b.item[0].nrtpc == 2 &&
          b.item[0].rtpc_id[1] == 0x2222u && b.item[0].rtpc_value[1] == -3.25f);
    CHECK(b.item[1].op == 1 && b.item[1].b == 1234 && b.item[1].fade_ms == 300 && b.item[1].category == 2);
    CHECK(b.item[2].op == 2 && b.item[2].a == 0x6D69D583u && b.item[2].slot == 3 && b.item[2].tag == 9 && b.item[2].fade_ms == 100);
    CHECK(b.item[3].op == 3 && b.item[3].nrtpc == 1 && b.item[3].rtpc_value[0] == 42.0f);
    // sizes: one byte short / long, the header alone, null
    CHECK(!wire::DecodeSound(buf, size - 1, b));
    buf[size] = 0;
    CHECK(!wire::DecodeSound(buf, size + 1, b));
    CHECK(!wire::DecodeSound(buf, wire::kSoundHeaderSize, b));
    CHECK(!wire::DecodeSound(nullptr, size, b));
    // a refused frame leaves the output alone
    b.seq = 99;
    std::uint8_t t[sizeof(buf)];
    auto refused = [&](std::size_t at, std::uint8_t v) {
        std::memcpy(t, buf, size);
        t[at] = v;
        return !wire::DecodeSound(t, size, b) && b.seq == 99;
    };
    const std::size_t i0 = wire::kSoundHeaderSize;        // item 0 (post, 2 RTPCs)
    const std::size_t i1 = i0 + wire::SoundItemSize(2);  // item 1 (stop)
    const std::size_t i2 = i1 + wire::kSoundItemSize;    // item 2 (action)
    const std::size_t i3 = i2 + wire::kSoundItemSize;    // item 3 (rtpc)
    CHECK(refused(3, static_cast<std::uint8_t>('2')));  // magic
    CHECK(refused(4, 2));                               // version
    CHECK(refused(5, 0));                               // n = 0
    CHECK(refused(5, 3));                               // n does not match the size
    CHECK(refused(5, 33));                              // n > 32
    CHECK(refused(i0 + 8, 4));                          // unknown op
    CHECK(refused(i0 + 9, 7));                          // slot 7
    CHECK(refused(i0 + 11, 13));                        // nrtpc 13
    std::memcpy(t, buf, size);
    std::memset(t + i0, 0, 4);  // post without an event id
    CHECK(!wire::DecodeSound(t, size, b));
    std::memcpy(t, buf, size);
    std::memset(t + i0 + 4, 0, 4);  // post without a playing id
    CHECK(!wire::DecodeSound(t, size, b));
    std::memcpy(t, buf, size);
    t[i0 + 12] = 0xD1, t[i0 + 13] = 0x07;  // age 2001
    CHECK(!wire::DecodeSound(t, size, b));
    CHECK(refused(i0 + 14, 1));  // a post with a fade
    CHECK(refused(i0 + 22, 5));  // category 5
    CHECK(refused(i0 + 23, 1));  // the reserved byte
    std::memcpy(t, buf, size);
    std::memset(t + i0 + 24, 0, 4);  // RTPC id 0
    CHECK(!wire::DecodeSound(t, size, b));
    const float bad[3] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 2e7f};
    for (float v : bad) {
        std::memcpy(t, buf, size);
        std::memcpy(t + i0 + 28, &v, 4);  // RTPC value
        CHECK(!wire::DecodeSound(t, size, b));
    }
    // stop: fields it does not use must be 0, a playing id is needed, fade <= 10 s
    CHECK(refused(i1 + 0, 1));  // a stop with an event id
    std::memcpy(t, buf, size);
    std::memset(t + i1 + 4, 0, 4);
    CHECK(!wire::DecodeSound(t, size, b));
    CHECK(refused(i1 + 9, 1));   // slot
    CHECK(refused(i1 + 10, 1));  // tag
    CHECK(refused(i1 + 16, 1));  // offset
    std::memcpy(t, buf, size);
    t[i1 + 14] = 0x11, t[i1 + 15] = 0x27;  // fade 10001
    CHECK(!wire::DecodeSound(t, size, b));
    // action: type <= 4, no offset; rtpc: no ids, a slot < 7
    CHECK(refused(i2 + 9, 5));
    CHECK(refused(i2 + 18, 1));
    CHECK(refused(i3 + 0, 1));
    CHECK(refused(i3 + 9, 7));
    // the encoder: nothing invalid, at most 512 bytes, a small buffer, n = 0
    wire::SoundFrame c = a;
    c.item[1].a = 5;  // a stop with an event id
    CHECK(wire::EncodeSound(c, buf, sizeof(buf)) == 0 && wire::SoundFrameSize(c) == 0);
    c = a;
    c.n = 0;
    CHECK(wire::EncodeSound(c, buf, sizeof(buf)) == 0);
    c = a;
    c.item[3].nrtpc = 0;  // an RTPC update without values
    CHECK(wire::EncodeSound(c, buf, sizeof(buf)) == 0);
    CHECK(wire::EncodeSound(a, buf, want - 1) == 0);
    static wire::SoundFrame big;
    big.n = wire::kMaxSoundItems;
    for (unsigned i = 0; i < big.n; ++i) {
        big.item[i] = post;
        big.item[i].nrtpc = 0;
    }
    CHECK(wire::SoundFrameSize(big) == wire::kSoundHeaderSize + 32 * wire::kSoundItemSize);  // 780: too big to send
    CHECK(wire::EncodeSound(big, buf, sizeof(buf)) == 0);
    big.n = 20;  // 12 + 20 x 24 = 492
    CHECK(wire::EncodeSound(big, buf, sizeof(buf)) == 492 && wire::DecodeSound(buf, 492, b) && b.n == 20);
    big.item[0].nrtpc = 12;  // + 96 = 588
    CHECK(wire::EncodeSound(big, buf, sizeof(buf)) == 0);
    // the other decoders refuse it, and it refuses theirs
    const std::size_t sn = wire::EncodeSound(a, buf, sizeof(buf));
    wire::Pose pose;
    static wire::JointFrame jf;
    wire::NameFrame nf;
    static wire::GearFrame gf;
    wire::GearRes gr;
    static wire::GearLook gl;
    static wire::GearPose gp;
    wire::Speech sp;
    CHECK(sn && !wire::DecodePose(buf, sn, pose) && !wire::DecodeJoints(buf, sn, jf) && !wire::DecodeName(buf, sn, nf) && !wire::DecodeGear(buf, sn, gf) &&
          !wire::DecodeGearRes(buf, sn, gr) && !wire::DecodeGearLook(buf, sn, gl) && !wire::DecodeGearPose(buf, sn, gp) && !wire::DecodeSpeech(buf, sn, sp));
    wire::Speech s;
    s.kind = 1, s.seq = 3;
    std::uint8_t sb[wire::kMaxSpeechFrameSize];
    const std::size_t ssz = wire::EncodeSpeech(s, sb, sizeof(sb));
    std::uint8_t pb[wire::kPoseFrameSize];
    wire::Pose pp;
    pp.flags = wire::kPoseValid;
    CHECK(ssz && !wire::DecodeSound(sb, ssz, b) && !wire::DecodeSound(pb, wire::EncodePose(pp, pb), b));
}

void TestSpeechFrames() {

    wire::Speech a;
    a.kind = 0;
    a.flags = wire::kSpeechFromHost;
    a.n = 2;
    a.seq = 7;
    a.sender_ms = 123456;
    a.situation = 0x2CF2EAD9u;  // Jumping
    for (int i = 0; i < 16; ++i) a.guid[0][i] = static_cast<std::uint8_t>(i + 1), a.guid[1][i] = static_cast<std::uint8_t>(0xF0 - i);
    std::uint8_t buf[wire::kMaxSpeechFrameSize + 8];
    const std::size_t size = wire::EncodeSpeech(a, buf, sizeof(buf));
    CHECK(size == wire::SpeechFrameSize(2) && size == 52);
    CHECK(std::memcmp(buf, "STS1", 4) == 0 && buf[4] == 1 && buf[5] == 0 && buf[6] == 1 && buf[7] == 2);
    wire::Speech b;
    CHECK(wire::DecodeSpeech(buf, size, b));
    CHECK(b.kind == 0 && b.flags == wire::kSpeechFromHost && b.n == 2 && b.seq == 7 && b.sender_ms == 123456 && b.situation == 0x2CF2EAD9u);
    CHECK(std::memcmp(b.guid, a.guid, 32) == 0);
    // the size must match n exactly
    CHECK(!wire::DecodeSpeech(buf, size - 1, b));
    buf[size] = 0;
    CHECK(!wire::DecodeSpeech(buf, size + 1, b));
    CHECK(!wire::DecodeSpeech(buf, wire::kSpeechHeaderSize - 1, b));
    CHECK(!wire::DecodeSpeech(nullptr, size, b));
    // magic, version, kind, count
    std::uint8_t t[sizeof(buf)];
    std::memcpy(t, buf, size);
    t[3] = '2';
    CHECK(!wire::DecodeSpeech(t, size, b));
    std::memcpy(t, buf, size);
    t[4] = 2;
    CHECK(!wire::DecodeSpeech(t, size, b));
    std::memcpy(t, buf, size);
    t[5] = 3;  // kind 3
    CHECK(!wire::DecodeSpeech(t, size, b));
    std::memcpy(t, buf, size);
    t[7] = 5;  // n 5 (and the size of 2)
    CHECK(!wire::DecodeSpeech(t, size, b));
    // seq 0, an all-zero GUID
    std::memcpy(t, buf, size);
    std::memset(t + 8, 0, 4);
    CHECK(!wire::DecodeSpeech(t, size, b));
    std::memcpy(t, buf, size);
    std::memset(t + 20 + 16, 0, 16);
    CHECK(!wire::DecodeSpeech(t, size, b));
    // unknown flag bits are cleared
    std::memcpy(t, buf, size);
    t[6] = 0xFF;
    CHECK(wire::DecodeSpeech(t, size, b) && b.flags == wire::kSpeechKnown);
    // kind 1 (muted) carries no GUIDs; kind 0 / 2 need at least one
    wire::Speech m;
    m.kind = 1;
    m.seq = 8;
    CHECK(wire::EncodeSpeech(m, buf, sizeof(buf)) == wire::kSpeechHeaderSize);
    CHECK(wire::DecodeSpeech(buf, wire::kSpeechHeaderSize, b) && b.kind == 1 && b.n == 0 && b.seq == 8);
    m.n = 1;
    std::memset(m.guid[0], 0x11, 16);
    CHECK(wire::EncodeSpeech(m, buf, sizeof(buf)) == 0);  // muted with a GUID
    wire::Speech z = a;
    z.n = 0;
    CHECK(wire::EncodeSpeech(z, buf, sizeof(buf)) == 0);  // played without a sentence
    z.n = 1, z.kind = 2;
    CHECK(wire::EncodeSpeech(z, buf, sizeof(buf)) == wire::SpeechFrameSize(1));
    CHECK(wire::DecodeSpeech(buf, wire::SpeechFrameSize(1), b) && b.kind == 2 && b.n == 1);
    // a hand-made kind 0 frame with n 0 and a kind 1 frame with n 1 are refused
    std::memcpy(t, buf, wire::kSpeechHeaderSize);
    t[5] = 0, t[7] = 0;
    CHECK(!wire::DecodeSpeech(t, wire::kSpeechHeaderSize, b));
    std::memcpy(t, buf, wire::SpeechFrameSize(1));
    t[5] = 1;
    CHECK(!wire::DecodeSpeech(t, wire::SpeechFrameSize(1), b));
    // encoder limits: 4 sentences at most, seq 0, a small buffer
    wire::Speech big = a;
    big.n = 4;
    for (int k = 2; k < 4; ++k) std::memset(big.guid[k], 0x40 + k, 16);
    CHECK(wire::EncodeSpeech(big, buf, sizeof(buf)) == wire::kMaxSpeechFrameSize);
    CHECK(wire::DecodeSpeech(buf, wire::kMaxSpeechFrameSize, b) && b.n == 4 && b.guid[3][0] == 0x43);
    big.n = 5;
    CHECK(wire::EncodeSpeech(big, buf, sizeof(buf)) == 0);
    big.n = 1, big.seq = 0;
    CHECK(wire::EncodeSpeech(big, buf, sizeof(buf)) == 0);
    big.seq = 1;
    CHECK(wire::EncodeSpeech(big, buf, wire::SpeechFrameSize(1) - 1) == 0);
    // the other decoders refuse it, and it refuses theirs
    const std::size_t sn = wire::EncodeSpeech(a, buf, sizeof(buf));
    wire::Pose pose;
    static wire::JointFrame jf;
    wire::NameFrame nf;
    static wire::GearFrame gf;
    wire::GearRes gr;
    static wire::GearLook gl;
    static wire::GearPose gp;
    CHECK(!wire::DecodePose(buf, sn, pose) && !wire::DecodeJoints(buf, sn, jf) && !wire::DecodeName(buf, sn, nf) && !wire::DecodeGear(buf, sn, gf) &&
          !wire::DecodeGearRes(buf, sn, gr) && !wire::DecodeGearLook(buf, sn, gl) && !wire::DecodeGearPose(buf, sn, gp));
    nf = wire::NameFrame{};
    std::snprintf(nf.name, sizeof(nf.name), "%s", "x");
    nf.len = 1;
    std::uint8_t nb[wire::kMaxNameFrameSize];
    const std::size_t nn = wire::EncodeName(nf, nb, sizeof(nb));
    CHECK(nn && !wire::DecodeSpeech(nb, nn, b));

    // The reliable channel carries STS1 in order, exactly once, through a lost first transmission.
    namespace lk = stcoop::link;
    static lk::ReliableSender tx;
    static lk::ReliableReceiver rx;
    std::uint64_t now = 1000;
    for (std::uint32_t q = 1; q <= 3; ++q) {
        wire::Speech s = a;
        s.seq = q;
        const std::size_t n = wire::EncodeSpeech(s, buf, sizeof(buf));
        CHECK(tx.Queue(buf, n, lk::ReliableMode::append) == lk::ReliableResult::queued);
    }
    std::vector<std::uint32_t> got;
    bool lost_once = false;
    for (int step = 0; step < 200 && got.size() < 3; ++step) {
        now += 20;
        lk::ReliableSender::Pick pick{};
        while (tx.NextDue(now, 100, 4096, pick)) {
            tx.MarkSent(pick.id, now);
            if (pick.id == 1 && !lost_once) {  // the second message's first transmission is lost
                lost_once = true;
                continue;
            }
            rx.Accept(pick.id, pick.payload, pick.size, now);
            rx.Accept(pick.id, pick.payload, pick.size, now);  // a duplicate is stored once
        }
        std::uint8_t out[lk::kMaxReliableMessage];
        std::size_t outn = 0;
        while (rx.Drain(out, sizeof(out), outn, now)) {
            wire::Speech d;
            CHECK(wire::DecodeSpeech(out, outn, d));
            got.push_back(d.seq);
        }
        std::uint16_t next = 0;
        std::uint32_t bits = 0;
        std::uint8_t free_slots = 0;
        rx.AckState(next, bits, free_slots);
        tx.OnRelAck(next, bits, free_slots, now);
    }
    CHECK(lost_once && got.size() == 3 && got[0] == 1 && got[1] == 2 && got[2] == 3);
    CHECK(tx.idle() && tx.retransmits() >= 1);
}

// STM1 vehicle frames (docs/real-partner-body.md section 22).
void TestVehicleFrames() {
    // the allow-list: GUID text -> raw bytes_le, kinds and layouts as decoded from the game data
    const wire::VehicleResource* res = wire::VehicleResources();
    CHECK(res != nullptr);
    const std::uint8_t casp[16] = {0x5d, 0x81, 0x17, 0xff, 0x07, 0x4a, 0x40, 0x08, 0x9b, 0x23, 0xd3, 0x0f, 0x30, 0x26, 0x9e, 0x7f};
    const wire::VehicleResource* r = wire::FindVehicleResource(casp);
    CHECK(r && r->kind == wire::VehicleKind::speedcar && r->layout == 0xECF43324u && std::strcmp(r->key, "roadster") == 0);
    std::uint8_t other[16] = {};
    std::memcpy(other, casp, 16);
    other[15] ^= 1;
    CHECK(wire::FindVehicleResource(other) == nullptr);
    unsigned kinds[4] = {};
    for (unsigned i = 0; i < wire::kVehicleResourceCount; ++i) ++kinds[static_cast<unsigned>(res[i].kind)];
    CHECK(kinds[0] == 0 && kinds[1] == 1 && kinds[2] == 2 && kinds[3] == 2);

    static wire::VehicleFrame f, back;
    f = wire::VehicleFrame{};
    f.seq = 7;
    f.sender_ms = 0xFFFFFFF0u;
    f.kind = 1;
    f.state = 2;
    f.flags = wire::kVehSeated | wire::kVehJoints;
    std::memcpy(f.guid, casp, 16);
    f.x = -1100.123456789, f.y = 274.5, f.z = 352.25;
    f.rot = wire::Normalize(wire::Quat{0.1f, -0.2f, 0.7f, 0.68f});
    f.speed = 12.5f;
    f.seat[0] = 0.312f, f.seat[1] = -0.25f, f.seat[2] = 0.851f;
    f.seat_rot = wire::Normalize(wire::Quat{0.0f, 0.0f, 0.05f, 1.0f});
    f.layout = 0xECF43324u;
    f.n = 23;
    for (unsigned j = 0; j < f.n; ++j) {
        f.q[j] = wire::Normalize(wire::Quat{0.01f * static_cast<float>(j), 0.3f, -0.1f, 0.9f});
        f.t[j][0] = 0.001f * static_cast<float>(j), f.t[j][1] = -1.5f, f.t[j][2] = 2.25f;
    }
    std::uint8_t buf[wire::kMaxVehicleFrameSize];
    const std::size_t n = wire::EncodeVehicle(f, buf, sizeof(buf));
    CHECK(n == wire::VehicleFrameSize(23) && n == 84 + 23 * 12);
    CHECK(wire::DecodeVehicle(buf, n, back));
    CHECK(back.seq == 7 && back.sender_ms == 0xFFFFFFF0u && back.kind == 1 && back.state == 2 && back.flags == f.flags);
    CHECK(std::memcmp(back.guid, casp, 16) == 0 && back.x == f.x && back.y == f.y && back.z == f.z && back.speed == 12.5f);
    CHECK(wire::AngleBetween(back.rot, f.rot) < 0.02f && wire::AngleBetween(back.seat_rot, f.seat_rot) < 0.02f);
    CHECK(std::fabs(back.seat[0] - 0.312f) < 0.0006f && std::fabs(back.seat[1] + 0.25f) < 0.0006f && std::fabs(back.seat[2] - 0.851f) < 0.0006f);
    CHECK(back.layout == 0xECF43324u && back.n == 23);
    float worst = 0;
    for (unsigned j = 0; j < 23; ++j) {
        const float a = wire::AngleBetween(back.q[j], f.q[j]);
        if (a > worst) worst = a;
        for (unsigned k = 0; k < 3; ++k) CHECK(std::fabs(back.t[j][k] - f.t[j][k]) < 0.0006f);
    }
    CHECK(worst < 0.02f);
    // a frame without joints (parked, not seated)
    {
        wire::VehicleFrame p = f;
        p.flags = 0, p.n = 0, p.state = 0;
        p.seat[0] = p.seat[1] = p.seat[2] = 0;
        const std::size_t pn = wire::EncodeVehicle(p, buf, sizeof(buf));
        CHECK(pn == wire::kVehicleHeaderSize && wire::DecodeVehicle(buf, pn, back) && back.n == 0 && back.flags == 0);
        p.seat[0] = 0.5f;  // a seat while not seated
        CHECK(wire::EncodeVehicle(p, buf, sizeof(buf)) == 0);
    }
    // the encoder refuses what the decoder would
    auto refused = [&](void (*mut)(wire::VehicleFrame&)) {
        static wire::VehicleFrame m;
        m = f;
        mut(m);
        return wire::EncodeVehicle(m, buf, sizeof(buf)) == 0;
    };
    CHECK(refused([](wire::VehicleFrame& m) { m.kind = 0; }));
    CHECK(refused([](wire::VehicleFrame& m) { m.kind = 4; }));
    CHECK(refused([](wire::VehicleFrame& m) { m.kind = 3; }));  // a roadster GUID with the trike's kind
    CHECK(refused([](wire::VehicleFrame& m) { m.state = 16; }));
    CHECK(refused([](wire::VehicleFrame& m) { m.flags |= 4; }));
    CHECK(refused([](wire::VehicleFrame& m) { m.flags &= static_cast<std::uint8_t>(~wire::kVehJoints); }));
    CHECK(refused([](wire::VehicleFrame& m) { m.n = 65; }));
    CHECK(refused([](wire::VehicleFrame& m) { m.layout = 0; }));
    CHECK(refused([](wire::VehicleFrame& m) { m.guid[0] ^= 0xFF; }));
    CHECK(refused([](wire::VehicleFrame& m) { m.x = std::numeric_limits<double>::quiet_NaN(); }));
    CHECK(refused([](wire::VehicleFrame& m) { m.y = 2e7; }));
    CHECK(refused([](wire::VehicleFrame& m) { m.speed = std::numeric_limits<float>::infinity(); }));
    CHECK(refused([](wire::VehicleFrame& m) { m.seat[2] = 4.5f; }));
    CHECK(refused([](wire::VehicleFrame& m) { m.t[3][1] = 40.0f; }));
    CHECK(refused([](wire::VehicleFrame& m) { m.q[0].w = std::numeric_limits<float>::quiet_NaN(); }));
    std::uint8_t small[wire::kVehicleHeaderSize];
    CHECK(wire::EncodeVehicle(f, small, sizeof(small)) == 0);
    // the decoder: sizes, magic, version, reserved byte, bytes it must refuse
    CHECK(!wire::DecodeVehicle(buf, n - 1, back));
    CHECK(!wire::DecodeVehicle(buf, n - 12, back));
    CHECK(!wire::DecodeVehicle(buf, 40, back));
    auto bad_at = [&](std::size_t off, std::uint8_t v) {
        std::uint8_t c[wire::kMaxVehicleFrameSize];
        std::memcpy(c, buf, n);
        c[off] = v;
        return !wire::DecodeVehicle(c, n, back);
    };
    CHECK(bad_at(0, 'X'));     // magic
    CHECK(bad_at(4, 2));       // version
    CHECK(bad_at(5, 0));       // kind 0
    CHECK(bad_at(5, 9));       // kind 9
    CHECK(bad_at(6, 200));     // state
    CHECK(bad_at(7, 0x81));    // unknown flag
    CHECK(bad_at(7, wire::kVehSeated));  // joints without the flag
    CHECK(bad_at(16, 0x00));   // GUID byte 0 (0x5d)
    CHECK(bad_at(83, 1));      // reserved
    CHECK(bad_at(82, 24));     // n = 24 with 23 joints' bytes
    {
        // a NaN position
        std::uint8_t c[wire::kMaxVehicleFrameSize];
        std::memcpy(c, buf, n);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        std::memcpy(c + 32, &nan, 8);
        CHECK(!wire::DecodeVehicle(c, n, back));
        // a seat x of 5 m (5000 mm at offset 66)
        std::memcpy(c, buf, n);
        const std::int16_t big = 5000;
        std::memcpy(c + 66, &big, 2);
        CHECK(!wire::DecodeVehicle(c, n, back));
    }
    // other frames are never taken for a vehicle frame, and a vehicle frame for nothing else
    wire::Pose pose;
    CHECK(!wire::DecodePose(buf, n, pose));
    wire::GearPose gp;
    CHECK(!wire::DecodeGearPose(buf, n, gp));
}

int main() {    TestPoseRoundTrip();
    TestJointFrames();
    TestJointOutfit();
    TestNameFrames();
    TestNameTagMath();
    TestGearFrames();
    TestGearV2();
    TestGearLook();
    TestBodyTable();
    TestBodyChoice();
    TestRotation();
    TestJoinCode();
    TestLinkHandshake();
    TestGameFontParser();
    TestSelfVoiceSetting();
    TestSpeechFrames();
    TestSoundFrames();
    TestVehicleFrames();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all net tests passed\n");
    return 0;
}
