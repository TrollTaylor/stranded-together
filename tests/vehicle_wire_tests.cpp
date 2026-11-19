// SVM1 (native/mod/vehicle_wire.*), the real shared vehicles' pure part: every message round trip and every decoder
// rejection, the snapshot's chunking and reassembly (in order, out of order, duplicates, a newer snapshot), the spawn
// request flags against 0x142F830A0's own formula, the guest's gate, the binding rules (M2.2), the host's removal guard
// (M2.4) and eviction refusal (M2.5), and the request rate limit. No game, no link.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "vehicle_wire.hpp"

using namespace st::vwire;

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

using Bytes = std::vector<std::uint8_t>;

VKey Key(std::uint32_t id, std::uint8_t kind, std::uint8_t g0 = 0) {
    VKey k;
    k.id = id;
    k.kind = kind;
    if (g0) {
        for (int i = 0; i < 16; ++i) k.guid[i] = static_cast<std::uint8_t>(g0 + i);
    }
    return k;
}

SnapEntry Entry(std::uint32_t id, std::uint8_t kind, double x, double y, double z, std::uint8_t g0 = 0) {
    SnapEntry e;
    e.key = Key(id, kind, g0);
    e.state = 2;
    e.eflags = kEntEntity | kEntXf;
    e.pos[0] = x, e.pos[1] = y, e.pos[2] = z;
    const float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::memcpy(e.rot, rot, sizeof(rot));
    e.flags = 0x10411100;
    e.flag_bytes[0] = 0x01;
    e.area = 200;
    e.rec2 = 7;
    e.life = 1000.0f;
    e.battery = 0.75f;
    e.rider = kNoRider;
    return e;
}

bool SameEntry(const SnapEntry& a, const SnapEntry& b) {
    return a.key == b.key && a.state == b.state && a.eflags == b.eflags && std::memcmp(a.pos, b.pos, sizeof(a.pos)) == 0 &&
           std::memcmp(a.rot, b.rot, sizeof(a.rot)) == 0 && a.flags == b.flags && std::memcmp(a.flag_bytes, b.flag_bytes, 4) == 0 && a.area == b.area &&
           a.rec2 == b.rec2 && a.life == b.life && a.battery == b.battery && a.rider == b.rider;
}

template <class M, class Enc>
Bytes Encode(const M& m, Enc enc) {
    Bytes b(kMaxMessage);
    const std::size_t n = enc(m, b.data(), b.size());
    b.resize(n);
    return b;
}

// ---------------------------------------------------------------- identity
void TestIdentity() {
    CHECK(KindOfActorId(0x040000) == 1);  // roadster (0x20 << 13)
    CHECK(KindOfActorId(0x020005) == 2);  // truck (0x10 << 13)
    CHECK(KindOfActorId(0x01E001) == 3);  // trike (0x0F << 13)
    CHECK(KindOfActorId(0x01FFFF) == 3);
    CHECK(KindOfActorId(0x1FFFFF) == 0);  // nobody
    CHECK(KindOfActorId(0) == 0);
    CHECK(KindOfActorId(0x050000) == 0);
    CHECK(ActorTypeOfKind(1) == 0x20 && ActorTypeOfKind(2) == 0x10 && ActorTypeOfKind(3) == 0x0F && ActorTypeOfKind(0) == 0 && ActorTypeOfKind(4) == 0);
    CHECK(KeyValid(Key(0x01E007, 3)));
    CHECK(!KeyValid(Key(0x01E007, 2)));  // a truck id is not a trike
    CHECK(!KeyValid(Key(0x020000, 0)));
    CHECK(!KeyValid(Key(0x020000, 4)));
    CHECK(Key(0x020000, 2, 9).HasGuid() && !Key(0x020000, 2).HasGuid());
    CHECK(Key(0x020000, 2, 9) != Key(0x020000, 2, 8));
    CHECK(std::strcmp(KindName(1), "roadster") == 0 && std::strcmp(KindName(3), "trike") == 0 && std::strcmp(KindName(9), "?") == 0);
    CHECK(std::strcmp(MsgTypeName(2), "SNAP") == 0 && std::strcmp(MsgTypeName(40), "external") == 0);
}

// ---------------------------------------------------------------- round trips
void TestRoundTrips() {
    {
        SnapReq m;
        m.req_id = 7, m.load_counter = 3, m.reason = 1;
        const Bytes b = Encode(m, EncodeSnapReq);
        CHECK(b.size() == kHeaderSize + 12);
        SnapReq o;
        CHECK(DecodeSnapReq(b.data(), b.size(), o) && o.req_id == 7 && o.load_counter == 3 && o.reason == 1);
        std::uint8_t t = 0;
        CHECK(PeekType(b.data(), b.size(), &t) && t == static_cast<std::uint8_t>(MsgType::snap_req));
    }
    {
        SnapChunk c;
        c.snap_id = 11, c.req_id = 7, c.host_load_counter = 3, c.host_area = 200, c.index = 1, c.count = 3, c.total = 20;
        for (int i = 0; i < 9; ++i) c.entries.push_back(Entry(0x020000u + static_cast<std::uint32_t>(i), 2, -1100.5 + i, 300.25, 360.0, i == 2 ? 0x40 : 0));
        c.entries[3].eflags = kEntRemoved;  // no transform: none sent
        std::memset(c.entries[3].pos, 0, sizeof(c.entries[3].pos));
        std::memset(c.entries[3].rot, 0, sizeof(c.entries[3].rot));
        c.entries[4].eflags |= kEntRidden;
        c.entries[4].rider = 0x000123;
        const Bytes b = Encode(c, EncodeSnapChunk);
        CHECK(b.size() == kHeaderSize + kSnapChunkHeader + 9 * kSnapEntrySize);
        CHECK(b.size() <= kMaxMessage);
        SnapChunk o;
        CHECK(DecodeSnapChunk(b.data(), b.size(), o));
        CHECK(o.snap_id == 11 && o.req_id == 7 && o.host_load_counter == 3 && o.host_area == 200 && o.index == 1 && o.count == 3 && o.total == 20);
        CHECK(o.entries.size() == 9);
        bool same = o.entries.size() == c.entries.size();
        for (std::size_t i = 0; same && i < c.entries.size(); ++i) same = SameEntry(o.entries[i], c.entries[i]);
        CHECK(same);
    }
    {
        Add a;
        a.token = 5;
        a.source = AddSource::guest_request;
        a.rec0 = static_cast<std::uint16_t>(3 | (1 << 6));
        a.p4 = 1;
        a.unit = Entry(0x01E009, 3, 10, 20, 30);
        a.has_entry = true;
        for (std::size_t i = 0; i < kEntrySize; ++i) a.entry[i] = static_cast<std::uint8_t>(i * 7);
        const std::uint32_t id = 0x01E009;
        std::memcpy(a.entry + 0x2C, &id, 4);
        const Bytes b = Encode(a, EncodeAdd);
        CHECK(b.size() == kHeaderSize + 12 + kSnapEntrySize + kEntrySize);
        Add o;
        CHECK(DecodeAdd(b.data(), b.size(), o));
        CHECK(o.token == 5 && o.source == AddSource::guest_request && o.rec0 == a.rec0 && o.p4 == 1 && o.has_entry && SameEntry(o.unit, a.unit) &&
              std::memcmp(o.entry, a.entry, kEntrySize) == 0);
        Add n = a;
        n.has_entry = false;
        n.token = 0;
        n.source = AddSource::host_own;
        std::memset(n.entry, 0x55, sizeof(n.entry));  // not sent without has_entry
        const Bytes b2 = Encode(n, EncodeAdd);
        CHECK(DecodeAdd(b2.data(), b2.size(), o) && !o.has_entry && o.source == AddSource::host_own && o.entry[0] == 0);
    }
    {
        Rem r;
        r.key = Key(0x020007, 2, 0x11);
        r.reason = RemReason::removed_flag;
        r.from = 1;
        r.has_pos = true;
        r.pos[0] = -1221.2, r.pos[1] = 316.01, r.pos[2] = 359.47;
        r.world = 7;
        const Bytes b = Encode(r, EncodeRem);
        CHECK(b.size() == kHeaderSize + 56);
        Rem o;
        CHECK(DecodeRem(b.data(), b.size(), o) && o.key == r.key && o.reason == RemReason::removed_flag && o.from == 1 && o.has_pos && o.pos[1] == 316.01 &&
              o.world == 7);
    }
    {
        Req q;
        q.token = 9, q.kind = 3, q.garage_slot = 4, q.reason = ReqReason::garage, q.req_flags = 0x5014, q.rec2 = 3, q.rec14 = 0x1122334455667788ull;
        q.pos[0] = 1, q.pos[1] = 2, q.pos[2] = 3;
        const float rot[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
        std::memcpy(q.rot, rot, sizeof(rot));
        std::snprintf(q.name, sizeof(q.name), "Trike Mk2");
        const Bytes b = Encode(q, EncodeReq);
        CHECK(b.size() == kHeaderSize + 116);
        Req o;
        CHECK(DecodeReq(b.data(), b.size(), o) && o.token == 9 && o.kind == 3 && o.garage_slot == 4 && o.reason == ReqReason::garage &&
              o.req_flags == 0x5014 && o.rec2 == 3 && o.rec14 == 0x1122334455667788ull && o.pos[2] == 3 && o.rot[3] == -1 &&
              std::strcmp(o.name, "Trike Mk2") == 0);
        q.garage_slot = -1;
        q.name[0] = 0;
        const Bytes b2 = Encode(q, EncodeReq);
        CHECK(DecodeReq(b2.data(), b2.size(), o) && o.garage_slot == -1 && o.name[0] == 0);
    }
    {
        Result r;
        r.token = 4, r.result = ReqResult::spawned, r.path = 1, r.key = Key(0x01E00A, 3);
        const Bytes b = Encode(r, EncodeResult);
        CHECK(b.size() == kHeaderSize + 32);
        Result o;
        CHECK(DecodeResult(b.data(), b.size(), o) && o.token == 4 && o.result == ReqResult::spawned && o.path == 1 && o.key == r.key);
        Result f;
        f.token = 4, f.result = ReqResult::timeout, f.path = 2;
        const Bytes b2 = Encode(f, EncodeResult);
        CHECK(DecodeResult(b2.data(), b2.size(), o) && o.result == ReqResult::timeout && o.key.id == 0);
        CHECK(std::strcmp(ReqResultName(4), "duplicate") == 0);
    }
    {
        Need m;
        m.key = Key(0x040000, 1), m.reason = 1;
        const Bytes b = Encode(m, EncodeNeed);
        CHECK(b.size() == kHeaderSize + 28);
        Need o;
        CHECK(DecodeNeed(b.data(), b.size(), o) && o.key == m.key && o.reason == 1);
    }
    {
        // the drive module's external messages: a header of their own, not decoded here
        std::uint8_t b[32] = {};
        CHECK(WriteHeader(40, b, sizeof(b)) == kHeaderSize);
        b[kHeaderSize] = 0xAB;
        std::uint8_t t = 0;
        CHECK(PeekType(b, kHeaderSize + 1, &t) && t == 40 && IsExternalType(t));
        CHECK(WriteHeader(5, b, sizeof(b)) == 0);   // ours
        CHECK(WriteHeader(64, b, sizeof(b)) == 0);  // out of range
        CHECK(WriteHeader(40, b, 4) == 0);          // no room
    }
}

// ---------------------------------------------------------------- rejections
void TestRejections() {
    SnapReq sr;
    sr.req_id = 1;
    const Bytes good = Encode(sr, EncodeSnapReq);
    SnapReq o;
    CHECK(DecodeSnapReq(good.data(), good.size(), o));
    {
        Bytes b = good;
        b[0] = 'X';  // magic
        CHECK(!DecodeSnapReq(b.data(), b.size(), o) && !PeekType(b.data(), b.size(), nullptr));
        b = good;
        b[5] = 2;  // version
        CHECK(!DecodeSnapReq(b.data(), b.size(), o) && !PeekType(b.data(), b.size(), nullptr));
        b = good;
        b[6] = 1;  // reserved header
        CHECK(!DecodeSnapReq(b.data(), b.size(), o));
        b = good;
        b[4] = static_cast<std::uint8_t>(MsgType::snap);  // another type
        CHECK(!DecodeSnapReq(b.data(), b.size(), o));
        b = good;
        b.back() = 1;  // reserved body
        CHECK(!DecodeSnapReq(b.data(), b.size(), o));
        b = good;
        b.push_back(0);  // too long
        CHECK(!DecodeSnapReq(b.data(), b.size(), o));
        b = good;
        b.pop_back();  // too short
        CHECK(!DecodeSnapReq(b.data(), b.size(), o));
        b = good;
        b[kHeaderSize + 8] = 3;  // reason
        CHECK(!DecodeSnapReq(b.data(), b.size(), o));
        CHECK(!DecodeSnapReq(nullptr, 0, o));
        CHECK(!PeekType(good.data(), kHeaderSize - 1, nullptr));
        Bytes z = good;
        z[4] = 0;  // type 0
        CHECK(!PeekType(z.data(), z.size(), nullptr));
    }
    // snapshot chunk
    SnapChunk c;
    c.snap_id = 1, c.count = 1, c.index = 0, c.total = 2;
    c.entries = {Entry(0x020000, 2, 1, 2, 3), Entry(0x01E000, 3, 4, 5, 6, 0x20)};
    const Bytes cg = Encode(c, EncodeSnapChunk);
    SnapChunk co;
    CHECK(DecodeSnapChunk(cg.data(), cg.size(), co));
    auto entry_at = [&](std::size_t i) { return kHeaderSize + kSnapChunkHeader + i * kSnapEntrySize; };
    {
        Bytes b = cg;
        b[entry_at(0) + 4] = 3;  // the truck id as a trike
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        b[entry_at(0) + 4] = 0;  // kind 0
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        b[entry_at(0) + 7] = 1;  // reserved
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        b[entry_at(0) + 6] = 0x40;  // unknown eflag
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        std::memcpy(b.data() + entry_at(0) + 24, &nan, 8);  // a NaN position
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        const double far = 2e7;
        std::memcpy(b.data() + entry_at(0) + 32, &far, 8);  // out of the world
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        const float big = 2.0f;
        std::memcpy(b.data() + entry_at(1) + 48, &big, 4);  // a rotation element out of range
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        b[entry_at(0) + 6] = kEntEntity;  // no transform flag, but a transform is there
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        b[entry_at(0) + 6] = kEntEntity | kEntXf | kEntRidden;  // ridden without a rider
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        const std::uint32_t rider = 0x200000;  // above 0x1FFFFF
        std::memcpy(b.data() + entry_at(0) + 104, &rider, 4);
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        b[kHeaderSize + 14] = 1;  // index 1 of 1
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        b[kHeaderSize + 15] = 0;  // count 0
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        b[kHeaderSize + 15] = 33;  // count over the cap
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        b[kHeaderSize + 18] = 3;  // three entries announced, two sent
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        b[kHeaderSize + 16] = 1;  // total 1 < 2 entries
        b[kHeaderSize + 17] = 0;
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        b[kHeaderSize + 19] = 1;  // reserved
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        b = cg;
        b.resize(b.size() - 1);
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
        const float nanf = std::numeric_limits<float>::quiet_NaN();
        b = cg;
        std::memcpy(b.data() + entry_at(0) + 96, &nanf, 4);  // life
        CHECK(!DecodeSnapChunk(b.data(), b.size(), co));
    }
    {
        // an encoder refuses what a decoder would refuse
        SnapChunk bad = c;
        bad.entries[0].key.kind = 3;
        Bytes buf(kMaxMessage);
        CHECK(EncodeSnapChunk(bad, buf.data(), buf.size()) == 0);
        bad = c;
        bad.index = 1;
        CHECK(EncodeSnapChunk(bad, buf.data(), buf.size()) == 0);
        bad = c;
        for (int i = 0; i < 9; ++i) bad.entries.push_back(Entry(0x020010u + static_cast<std::uint32_t>(i), 2, 0, 0, 1));
        bad.total = 11;
        CHECK(EncodeSnapChunk(bad, buf.data(), buf.size()) == 0);  // 11 > 9 per chunk
        CHECK(EncodeSnapChunk(c, buf.data(), 100) == 0);           // no room
    }
    // ADD
    Add a;
    a.unit = Entry(0x01E003, 3, 1, 2, 3);
    a.rec0 = 3;
    a.p4 = 1;
    const Bytes ag = Encode(a, EncodeAdd);
    Add ao;
    CHECK(DecodeAdd(ag.data(), ag.size(), ao));
    {
        Bytes b = ag;
        b[kHeaderSize + 4] = 1;  // guest_request without a token
        CHECK(!DecodeAdd(b.data(), b.size(), ao));
        b = ag;
        b[kHeaderSize + 0] = 1;  // host_own with a token
        CHECK(!DecodeAdd(b.data(), b.size(), ao));
        b = ag;
        b[kHeaderSize + 4] = 3;  // unknown source
        CHECK(!DecodeAdd(b.data(), b.size(), ao));
        b = ag;
        b[kHeaderSize + 6] = 2;  // rec0 says truck, the unit is a trike
        CHECK(!DecodeAdd(b.data(), b.size(), ao));
        b = ag;
        b[kHeaderSize + 8] = 2;  // p4 2
        CHECK(!DecodeAdd(b.data(), b.size(), ao));
        b = ag;
        b[kHeaderSize + 9] = 1;  // reserved
        CHECK(!DecodeAdd(b.data(), b.size(), ao));
        b = ag;
        b.back() = 1;  // entry bytes without has_entry
        CHECK(!DecodeAdd(b.data(), b.size(), ao));
        b = ag;
        b[kHeaderSize + 5] = 1;  // has_entry, but the entry's id (+0x2C) is 0
        CHECK(!DecodeAdd(b.data(), b.size(), ao));
        Add bad = a;
        bad.rec0 = 2;
        Bytes buf(kMaxMessage);
        CHECK(EncodeAdd(bad, buf.data(), buf.size()) == 0);
    }
    // REM
    Rem r;
    r.key = Key(0x020001, 2);
    const Bytes rg = Encode(r, EncodeRem);
    Rem ro;
    CHECK(DecodeRem(rg.data(), rg.size(), ro) && !ro.has_pos);
    {
        Bytes b = rg;
        b[kHeaderSize + 0] = 5;  // reason
        CHECK(!DecodeRem(b.data(), b.size(), ro));
        b = rg;
        b[kHeaderSize + 1] = 2;  // from
        CHECK(!DecodeRem(b.data(), b.size(), ro));
        b = rg;
        b[kHeaderSize + 2] = 2;  // has_pos
        CHECK(!DecodeRem(b.data(), b.size(), ro));
        b = rg;
        b[b.size() - 5] = 0x40;  // a position without has_pos (the last byte of z; the world follows)
        CHECK(!DecodeRem(b.data(), b.size(), ro));
        b = rg;
        b[kHeaderSize + 4 + 4] = 1;  // kind roadster for a truck id
        CHECK(!DecodeRem(b.data(), b.size(), ro));
        b = rg;
        b[kHeaderSize + 4 + 21] = 1;  // key padding
        CHECK(!DecodeRem(b.data(), b.size(), ro));
    }
    // REQ
    Req q;
    q.token = 1, q.kind = 2;
    q.pos[0] = -1200, q.pos[1] = 300, q.pos[2] = 360;
    q.rot[0] = 1, q.rot[4] = 1, q.rot[8] = 1;
    const Bytes qg = Encode(q, EncodeReq);
    Req qo;
    CHECK(DecodeReq(qg.data(), qg.size(), qo));
    {
        // an unreadable request is never a vehicle at the world origin or with a degenerate rotation
        Req z = q;
        std::memset(z.pos, 0, sizeof(z.pos));
        Bytes buf(kMaxMessage);
        CHECK(EncodeReq(z, buf.data(), buf.size()) == 0);
        z = q;
        std::memset(z.rot, 0, sizeof(z.rot));
        CHECK(EncodeReq(z, buf.data(), buf.size()) == 0);
        z = q;
        z.rot[1] = 0.5f;  // rows not orthogonal
        CHECK(EncodeReq(z, buf.data(), buf.size()) == 0);
        Bytes b = qg;
        std::memset(b.data() + kHeaderSize + 24, 0, 24);  // the position zeroed on the wire
        CHECK(!DecodeReq(b.data(), b.size(), qo));
        b = qg;
        std::memset(b.data() + kHeaderSize + 48, 0, 36);  // the rotation zeroed on the wire
        CHECK(!DecodeReq(b.data(), b.size(), qo));
    }
    {
        Bytes b = qg;
        std::memset(b.data() + kHeaderSize, 0, 4);  // token 0
        CHECK(!DecodeReq(b.data(), b.size(), qo));
        b = qg;
        b[kHeaderSize + 4] = 4;  // kind
        CHECK(!DecodeReq(b.data(), b.size(), qo));
        b = qg;
        b[kHeaderSize + 5] = 9;  // garage slot 9
        CHECK(!DecodeReq(b.data(), b.size(), qo));
        b = qg;
        b[kHeaderSize + 5] = static_cast<std::uint8_t>(-2);
        CHECK(!DecodeReq(b.data(), b.size(), qo));
        b = qg;
        b[kHeaderSize + 6] = 3;  // reason
        CHECK(!DecodeReq(b.data(), b.size(), qo));
        b = qg;
        b[kHeaderSize + 7] = 1;  // reserved
        CHECK(!DecodeReq(b.data(), b.size(), qo));
        b = qg;
        b[kHeaderSize + 14] = 1;  // reserved
        CHECK(!DecodeReq(b.data(), b.size(), qo));
        b = qg;
        b[b.size() - kNameChars] = 0x07;  // a control character in the name
        CHECK(!DecodeReq(b.data(), b.size(), qo));
        b = qg;
        b[b.size() - kNameChars + 1] = 'A';  // a character after the NUL
        CHECK(!DecodeReq(b.data(), b.size(), qo));
        Req bad = q;
        bad.token = 0;
        Bytes buf(kMaxMessage);
        CHECK(EncodeReq(bad, buf.data(), buf.size()) == 0);
        bad = q;
        bad.pos[0] = std::numeric_limits<double>::infinity();
        CHECK(EncodeReq(bad, buf.data(), buf.size()) == 0);
    }
    // RESULT
    {
        Result s;
        s.token = 2, s.result = ReqResult::spawned, s.key = Key(0x020002, 2);
        Bytes b = Encode(s, EncodeResult);
        Result so;
        CHECK(DecodeResult(b.data(), b.size(), so));
        b[kHeaderSize + 4] = static_cast<std::uint8_t>(ReqResult::refused);  // refused with a key
        CHECK(!DecodeResult(b.data(), b.size(), so));
        Result f;
        f.token = 2, f.result = ReqResult::refused;
        Bytes fb = Encode(f, EncodeResult);
        fb[kHeaderSize + 4] = 0;  // spawned without a key
        CHECK(!DecodeResult(fb.data(), fb.size(), so));
        fb = Encode(f, EncodeResult);
        fb[kHeaderSize + 5] = 3;  // path
        CHECK(!DecodeResult(fb.data(), fb.size(), so));
        Bytes buf(64);
        f.key = Key(0x020002, 2);
        CHECK(EncodeResult(f, buf.data(), buf.size()) == 0);  // a key on a refusal
        s.key = VKey{};
        CHECK(EncodeResult(s, buf.data(), buf.size()) == 0);  // no key on a spawn
    }
    // NEED
    {
        Need n;
        n.key = Key(0x020002, 2);
        Bytes b = Encode(n, EncodeNeed);
        Need no;
        CHECK(DecodeNeed(b.data(), b.size(), no));
        b[kHeaderSize + 24] = 2;
        CHECK(!DecodeNeed(b.data(), b.size(), no));
        b = Encode(n, EncodeNeed);
        b[kHeaderSize + 25] = 1;
        CHECK(!DecodeNeed(b.data(), b.size(), no));
        // a message of another type is never one of these
        CHECK(!DecodeSnapReq(b.data(), b.size(), sr) && !DecodeAdd(b.data(), b.size(), ao) && !DecodeRem(b.data(), b.size(), ro));
    }
}

// ---------------------------------------------------------------- snapshot chunking
void TestSnapshot() {
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{9}, std::size_t{10}, std::size_t{24}, std::size_t{256}, std::size_t{300}}) {
        std::vector<SnapEntry> es;
        for (std::size_t i = 0; i < n; ++i)
            es.push_back(Entry(0x020000u + static_cast<std::uint32_t>(i % 0x2000), 2, static_cast<double>(i), 1, 2, i % 5 == 0 ? 0x30 : 0));
        const auto chunks = EncodeSnapshot(42, 7, 3, 200, es);
        const std::size_t kept = n > kSnapMaxEntries ? kSnapMaxEntries : n;
        const std::size_t want = kept == 0 ? 1 : (kept + kSnapPerChunk - 1) / kSnapPerChunk;
        CHECK(chunks.size() == want);
        SnapAssembler as;
        SnapAssembler::Step last = SnapAssembler::Step::rejected;
        bool sizes_ok = true;
        // out of order: odd chunks first
        std::vector<std::size_t> order;
        for (std::size_t i = 1; i < chunks.size(); i += 2) order.push_back(i);
        for (std::size_t i = 0; i < chunks.size(); i += 2) order.push_back(i);
        for (std::size_t k = 0; k < order.size(); ++k) {
            const auto& b = chunks[order[k]];
            if (b.size() > kMaxMessage) sizes_ok = false;
            SnapChunk c;
            CHECK(DecodeSnapChunk(b.data(), b.size(), c));
            last = as.Add(c);
            if (k + 1 < order.size()) CHECK(last == SnapAssembler::Step::partial);
        }
        CHECK(sizes_ok);
        CHECK(last == SnapAssembler::Step::complete && as.Complete());
        CHECK(as.Entries().size() == kept && as.SnapId() == 42 && as.ReqId() == 7 && as.LoadCounter() == 3 && as.Area() == 200);
        // every entry arrived exactly once
        std::vector<int> seen(kept, 0);
        for (const SnapEntry& e : as.Entries()) {
            const std::size_t i = static_cast<std::size_t>(e.pos[0]);
            if (i < kept) ++seen[i];
        }
        bool once = true;
        for (int s : seen)
            if (s != 1) once = false;
        CHECK(once);
    }
    // duplicates, a newer snapshot, an inconsistent header
    std::vector<SnapEntry> es;
    for (int i = 0; i < 20; ++i) es.push_back(Entry(0x01E000u + static_cast<std::uint32_t>(i), 3, i, 0, 0));
    const auto a = EncodeSnapshot(1, 0, 1, 200, es);
    const auto b = EncodeSnapshot(2, 0, 1, 200, es);
    CHECK(a.size() == 3 && b.size() == 3);
    auto dec = [](const std::vector<std::uint8_t>& m) {
        SnapChunk c;
        DecodeSnapChunk(m.data(), m.size(), c);
        return c;
    };
    SnapAssembler as;
    CHECK(as.Add(dec(a[0])) == SnapAssembler::Step::partial);
    CHECK(as.Add(dec(a[0])) == SnapAssembler::Step::rejected);  // a duplicate chunk: starts over
    CHECK(!as.Complete());
    CHECK(as.Add(dec(a[0])) == SnapAssembler::Step::partial);
    CHECK(as.Add(dec(a[1])) == SnapAssembler::Step::partial);
    CHECK(as.Add(dec(b[0])) == SnapAssembler::Step::partial);  // a newer snapshot: starts over with it
    CHECK(as.SnapId() == 2);
    CHECK(as.Add(dec(b[1])) == SnapAssembler::Step::partial);
    CHECK(as.Add(dec(b[2])) == SnapAssembler::Step::complete);
    CHECK(as.Entries().size() == 20);
    CHECK(as.Add(dec(b[2])) == SnapAssembler::Step::rejected);  // a chunk of the complete snapshot again
    SnapAssembler as2;
    SnapChunk c0 = dec(a[0]);
    SnapChunk c1 = dec(a[1]);
    c1.req_id = 9;  // disagrees with chunk 0
    CHECK(as2.Add(c0) == SnapAssembler::Step::partial);
    CHECK(as2.Add(c1) == SnapAssembler::Step::rejected);
    SnapAssembler as3;
    SnapChunk big = dec(a[0]);
    big.total = 5;  // 9 entries in a snapshot of 5
    CHECK(as3.Add(big) == SnapAssembler::Step::rejected);
    SnapAssembler as4;
    SnapChunk one = dec(a[2]);
    one.count = 1, one.index = 0;  // a single chunk of 2 entries announcing 20
    CHECK(as4.Add(one) == SnapAssembler::Step::rejected);
    // an empty table is one chunk, complete at once
    const auto e = EncodeSnapshot(5, 0, 1, 200, {});
    SnapAssembler as5;
    CHECK(e.size() == 1 && as5.Add(dec(e[0])) == SnapAssembler::Step::complete && as5.Entries().empty());
}

// ---------------------------------------------------------------- spawn request flags
// 0x142F830A0's own expression (decompiled), for every record bit pattern it can see.
std::uint32_t GameFormula(std::uint16_t uVar1, std::uint8_t param_4, std::uint8_t param_6) {
    const std::uint32_t local_58 = 0;
    return ((((uVar1 >> 0xd) & 1u) | static_cast<std::uint32_t>(param_6) * 2u) * 2u | ((uVar1 >> 0xc) & 1u)) << 0xc |
           static_cast<std::uint32_t>(param_4) << 2 | (local_58 & 0xffff8c08u) | (((uVar1 >> 2) & 0xcu) | (uVar1 & 0xfc0u)) >> 2;
}

void TestRequestFlags() {
    bool all = true, inverse = true;
    for (std::uint32_t pool = 1; pool <= 3; ++pool)
        for (std::uint32_t hi = 0; hi < (1u << 10); ++hi)  // record bits 4..13
            for (std::uint8_t p4 = 0; p4 <= 1; ++p4)
                for (std::uint8_t p6 = 0; p6 <= 1; ++p6) {
                    const std::uint16_t rec0 = static_cast<std::uint16_t>(pool | (hi << 4));
                    const std::uint32_t f = RequestFlagsFromRecord(rec0, p4, p6);
                    if (f != GameFormula(rec0, p4, p6)) all = false;
                    std::uint16_t r = 0;
                    std::uint8_t a = 9, b = 9;
                    if (!RecordFromRequestFlags(f, static_cast<std::uint8_t>(pool), &r, &a, &b) || r != rec0 || a != p4 || b != p6) inverse = false;
                }
    CHECK(all);
    CHECK(inverse);
    CHECK((RequestFlagsFromRecord(0x0FFF, 1, 1) & 0x800) == 0);  // 0x142F830A0 can never set the online flag
    std::uint16_t r = 0;
    CHECK(!RecordFromRequestFlags(0xC0A, 2, &r, nullptr, nullptr));  // an online request
    CHECK(!RecordFromRequestFlags(0x8, 2, &r, nullptr, nullptr));
    CHECK(!RecordFromRequestFlags(0x10000, 2, &r, nullptr, nullptr));
    CHECK(!RecordFromRequestFlags(0, 0, &r, nullptr, nullptr));
    CHECK(!RecordFromRequestFlags(0, 4, &r, nullptr, nullptr));
    CHECK(RecordFromRequestFlags(0, 3, &r, nullptr, nullptr) && r == 3);
}

// ---------------------------------------------------------------- the gate
void TestGate() {
    GateIn in;
    in.flags = 0xC0A;  // online: dropped on both games, even the mod's own and in the load window
    in.host = true;
    CHECK(DecideGate(in) == Gate::drop_online);
    in.host = false, in.mod_call = true, in.load_window = true;
    CHECK(DecideGate(in) == Gate::drop_online);
    in = GateIn{};
    in.host = true;
    CHECK(DecideGate(in) == Gate::pass);  // the host's own world
    in = GateIn{};
    in.mod_call = true;
    CHECK(DecideGate(in) == Gate::pass);  // the guest's mirror
    in = GateIn{};
    in.load_window = true;
    CHECK(DecideGate(in) == Gate::pass);  // the save's own spawns replay on the guest
    in = GateIn{};
    in.can_forward = true;
    in.flags = 0x5014;
    CHECK(DecideGate(in) == Gate::forward);
    in.can_forward = false;
    CHECK(DecideGate(in) == Gate::drop);
    // the host's game shares no real vehicles (no snapshot yet, or silent): the guest's own spawns pass; online ones still drop
    in.host_real = false;
    CHECK(DecideGate(in) == Gate::pass);
    in.can_forward = true;
    CHECK(DecideGate(in) == Gate::pass);
    in.flags = kReqOnline;
    CHECK(DecideGate(in) == Gate::drop_online);
}

// ---------------------------------------------------------------- binding
LocalUnit Local(std::uint32_t id, std::uint8_t kind, double x, double y, double z, std::uint8_t g0 = 0) {
    LocalUnit u;
    u.id = id;
    u.kind = kind;
    u.has_entity = true;
    u.has_pos = true;
    u.pos[0] = x, u.pos[1] = y, u.pos[2] = z;
    if (g0)
        for (int i = 0; i < 16; ++i) u.guid[i] = static_cast<std::uint8_t>(g0 + i);
    return u;
}

void TestBinding() {
    BindParams p;
    p.current_area = 200;
    {
        // the proven case: the same save, the same ids and GUIDs within 0.08 m; one runtime truck 349.5 m away under the same id;
        // one guest-only runtime trike
        std::vector<SnapEntry> host = {Entry(0x020007, 2, -1221.20, 316.01, 359.47), Entry(0x020000, 2, -1170.26, 336.49, 365.0),
                                       Entry(0x02000F, 2, 0, 0, 0, 0x40), Entry(0x01E001, 3, -666.34, 292.87, 513.74)};
        host[2].eflags = 0;  // a placement vehicle not spawned: no transform
        std::memset(host[2].pos, 0, sizeof(host[2].pos));
        std::vector<LocalUnit> local = {Local(0x020007, 2, -1221.28, 316.01, 359.47), Local(0x020000, 2, -1143.28, 683.98, 399.24),
                                        Local(0x02000F, 2, 0, 0, 0, 0x40), Local(0x01E001, 3, -666.34, 292.87, 513.74), Local(0x01E005, 3, -1191.14, 243.0, 362.08)};
        local[2].has_pos = false, local[2].has_entity = false;
        const BindPlan plan = PlanBinding(host, local, p);
        CHECK(plan.steps.size() == 4);
        CHECK(plan.steps[0].kind == BindKind::bound && plan.steps[0].local == 0 && plan.steps[0].dist < 0.1);
        CHECK(plan.steps[1].kind == BindKind::replace && plan.steps[1].local == 1 && plan.steps[1].dist > 300);
        CHECK(plan.steps[2].kind == BindKind::bound && plan.steps[2].local == 2);  // GUID match, no positions
        CHECK(plan.steps[3].kind == BindKind::bound);
        CHECK(plan.extras.size() == 1 && plan.extras[0].local == 4 && plan.extras[0].kind == ExtraKind::remove);
        CHECK(plan.bound == 3 && plan.replace == 1 && plan.extra_remove == 1);
        // without re-placing: removed, then mirrored
        BindParams q = p;
        q.allow_replace = false;
        CHECK(PlanBinding(host, local, q).steps[1].kind == BindKind::remove_then_mirror);
        // ridden by the guest's Sam: left alone (never pulled from under him)
        local[1].ridden = true;
        local[4].ridden = true;
        const BindPlan r = PlanBinding(host, local, p);
        CHECK(r.steps[1].kind == BindKind::wait && r.extras[0].kind == ExtraKind::keep_ridden && r.extra_remove == 0 && r.extra_kept == 1);
    }
    {
        // a GUID vehicle under another id on the guest: bound by GUID; the guest's unit with the host's id is another vehicle
        std::vector<SnapEntry> host = {Entry(0x020011, 2, 10, 10, 0, 0x50)};
        std::vector<LocalUnit> local = {Local(0x020011, 2, 300, 10, 0), Local(0x020012, 2, 10, 10.5, 0, 0x50)};
        const BindPlan plan = PlanBinding(host, local, p);
        CHECK(plan.steps[0].kind == BindKind::bound && plan.steps[0].local == 1);
        CHECK(plan.extras.size() == 1 && plan.extras[0].local == 0 && plan.extras[0].kind == ExtraKind::remove);
        // the same GUID vehicle far away: re-placed (it is the same vehicle)
        local[1].pos[0] = 200;
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::replace);
        // ... unless the host's is driven (the drive module follows it)
        host[0].eflags |= kEntRidden;
        host[0].rider = 0x123;
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::bound_unverified);
    }
    {
        // a GUID on one side only under the same id: another vehicle, removed then mirrored (a placement is never re-placed)
        std::vector<SnapEntry> host = {Entry(0x020003, 2, 10, 10, 0)};
        std::vector<LocalUnit> local = {Local(0x020003, 2, 10, 10, 0, 0x70)};
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::remove_then_mirror);
    }
    {
        // positions missing on one side: bound, unverified
        std::vector<SnapEntry> host = {Entry(0x020004, 2, 10, 10, 0)};
        std::vector<LocalUnit> local = {Local(0x020004, 2, 0, 0, 0)};
        local[0].has_pos = false;
        const BindPlan plan = PlanBinding(host, local, p);
        CHECK(plan.steps[0].kind == BindKind::bound_unverified && plan.steps[0].dist < 0 && plan.unverified == 1);
    }
    {
        // the guest has nothing for it: wait in the grace time, then mirror (this area only, with a place)
        std::vector<SnapEntry> host = {Entry(0x01E00A, 3, 5, 5, 0)};
        std::vector<LocalUnit> local;
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::wait);
        BindParams m = p;
        m.mirrors_allowed = true;
        CHECK(PlanBinding(host, local, m).steps[0].kind == BindKind::mirror);
        host[0].area = 100;
        CHECK(PlanBinding(host, local, m).steps[0].kind == BindKind::wait);
        host[0].area = 200;
        host[0].eflags = 0;
        std::memset(host[0].pos, 0, sizeof(host[0].pos));
        CHECK(PlanBinding(host, local, m).steps[0].kind == BindKind::wait);  // no place to mirror it to
    }
    {
        // removed on the host
        std::vector<SnapEntry> host = {Entry(0x020005, 2, 1, 1, 1)};
        host[0].eflags |= kEntRemoved;
        std::vector<LocalUnit> local = {Local(0x020005, 2, 1, 1, 1)};
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::remove_local);
        local[0].ridden = true;
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::wait);
        local[0].ridden = false;
        local[0].removed = true;
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::gone);
        local.clear();
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::gone);
    }
    {
        // removed here, alive on the host: mirrored again
        std::vector<SnapEntry> host = {Entry(0x020006, 2, 1, 1, 1)};
        std::vector<LocalUnit> local = {Local(0x020006, 2, 1, 1, 1)};
        local[0].removed = true;
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::remove_then_mirror);
    }
    {
        // a fallback mirror's id mapping: the host's 0x01E00B is our 0x01E00C
        std::vector<SnapEntry> host = {Entry(0x01E00B, 3, 50, 50, 0)};
        std::vector<LocalUnit> local = {Local(0x01E00C, 3, 50.2, 50, 0)};
        local[0].bound_host_id = 0x01E00B;
        const BindPlan plan = PlanBinding(host, local, p);
        CHECK(plan.steps[0].kind == BindKind::bound && plan.steps[0].local == 0 && plan.extras.empty());
        // a new unit from a pending fallback mirror is left alone
        std::vector<LocalUnit> l2 = {Local(0x01E00D, 3, 50, 50, 0)};
        l2[0].pending_mirror = true;
        const BindPlan p2 = PlanBinding({}, l2, p);
        CHECK(p2.extras.size() == 1 && p2.extras[0].kind == ExtraKind::keep_pending && p2.extra_remove == 0);
        // an extra removed already
        l2[0].pending_mirror = false;
        l2[0].removed = true;
        const BindPlan p3 = PlanBinding({}, l2, p);
        CHECK(p3.extras[0].kind == ExtraKind::already_removed && p3.extra_remove == 0 && p3.extra_kept == 0);
    }
    {
        // the same id driven on the host, far from ours: bound (the drive module follows), not re-placed
        std::vector<SnapEntry> host = {Entry(0x020008, 2, 100, 0, 0)};
        host[0].eflags |= kEntRidden;
        host[0].rider = 0x55;
        std::vector<LocalUnit> local = {Local(0x020008, 2, 0, 0, 0)};
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::bound_unverified);
    }
    {
        // one local unit is never claimed twice
        std::vector<SnapEntry> host = {Entry(0x020009, 2, 0, 0, 0, 0x60), Entry(0x020009, 2, 0, 0, 0)};
        host[1].key.id = 0x02000A;
        std::vector<LocalUnit> local = {Local(0x020009, 2, 0, 0, 0, 0x60)};
        const BindPlan plan = PlanBinding(host, local, p);
        CHECK(plan.steps[0].kind == BindKind::bound && plan.steps[0].local == 0);
        CHECK(plan.steps[1].local == -1 && plan.steps[1].kind == BindKind::wait);
    }
    {
        // the same vehicle (no GUID on either side) whose actor is not spawned here, its unit's own place old: bound, not removed
        // (streaming is not removal); with its actor back it is re-placed
        std::vector<SnapEntry> host = {Entry(0x020010, 2, 500, 0, 0)};
        std::vector<LocalUnit> local = {Local(0x020010, 2, 0, 0, 0)};
        local[0].has_entity = false;
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::bound_unverified);
        local[0].has_entity = true;
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::replace);
        // another kind under that id without an actor: still another vehicle
        local[0].has_entity = false;
        local[0].kind = 3;
        local[0].id = 0x020010;
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::remove_then_mirror);
    }
    {
        // the mod's own mirror of a placement vehicle carries no GUID: it is still that vehicle (no remove / mirror loop)
        std::vector<SnapEntry> host = {Entry(0x020013, 2, 10, 10, 0, 0x30)};
        std::vector<LocalUnit> local = {Local(0x020013, 2, 10.1, 10, 0)};
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::remove_then_mirror);  // unknown: another vehicle
        local[0].mirrored_for = 0x020013;
        const BindPlan plan = PlanBinding(host, local, p);
        CHECK(plan.steps[0].kind == BindKind::bound && plan.steps[0].local == 0 && plan.extras.empty());
        local[0].pos[0] = 200;  // moved away: re-placed, not removed
        CHECK(PlanBinding(host, local, p).steps[0].kind == BindKind::replace);
        local[0].removed = true;  // a removed mirror is not claimed by it
        CHECK(PlanBinding(host, local, p).steps[0].kind != BindKind::bound);
    }
    CHECK(std::strcmp(BindKindName(BindKind::replace), "re-place") == 0);
}

// ---------------------------------------------------------------- the host's checks on the guest's word (review fixes)
void TestReviewRules() {
    // CLAIM proximity: the partner body at the truck (its footprint +3 m for a get-on, +6 m for a push)
    Footprint truck;
    truck.c[0] = 100, truck.c[1] = 50, truck.c[2] = 10;
    KindHalfExtents(2, truck.half);  // 1.5 x 4.4 x 2.0
    const double beside[3] = {100 + 1.5 + 2.0, 50, 10}, five[3] = {100 + 1.5 + 5.0, 50, 10}, far[3] = {400, 50, 10};
    CHECK(ClaimNear(truck, true, beside, kClaimDrive));
    CHECK(!ClaimNear(truck, true, five, kClaimDrive));
    CHECK(ClaimNear(truck, true, five, kClaimPush));
    CHECK(!ClaimNear(truck, true, far, kClaimPush));
    CHECK(!ClaimNear(truck, false, beside, kClaimDrive));  // no partner body: refused
    const double nan3[3] = {std::numeric_limits<double>::quiet_NaN(), 0, 0};
    CHECK(!ClaimNear(truck, true, nan3, kClaimDrive));
    // a guest frame within reach of our unit or of the partner body
    const double unit[3] = {0, 0, 0}, partner[3] = {1000, 0, 0};
    const double f1[3] = {30, 0, 0}, f2[3] = {1030, 0, 0}, f3[3] = {500, 0, 0};
    CHECK(FrameInReach(f1, true, unit, true, partner));
    CHECK(FrameInReach(f2, true, unit, true, partner));
    CHECK(!FrameInReach(f3, true, unit, true, partner));
    CHECK(!FrameInReach(f2, true, unit, false, partner));
    CHECK(!FrameInReach(f1, false, unit, false, partner));
    // the parked teleport backoff
    CHECK(ParkedTeleportGapMs(0) == 2000 && ParkedTeleportGapMs(1) == 2000 && ParkedTeleportGapMs(2) == 4000 && ParkedTeleportGapMs(3) == 8000);
    CHECK(ParkedTeleportGapMs(6) == 60000 && ParkedTeleportGapMs(100) == 60000);
    // rotation rows
    const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, yaw[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1}, zero[9] = {}, skew[9] = {1, 0.5f, 0, 0, 1, 0, 0, 0, 1};
    CHECK(RotRowsOk(id) && RotRowsOk(yaw) && !RotRowsOk(zero) && !RotRowsOk(skew));
    // the host's removal guard: the unit's place given and near
    HostUnitView u;
    u.in_use = true, u.id = 0x020007, u.kind = 2, u.has_pos = true;
    const double near[3] = {1, 0, 0}, none[3] = {};
    CHECK(HostRemovalAllowed(u, Key(0x020007, 2), true, near));
    CHECK(!HostRemovalAllowed(u, Key(0x020007, 2), false, none));
}

// ---------------------------------------------------------------- host rules
void TestHostRules() {
    HostUnitView u;
    u.in_use = true, u.id = 0x020007, u.kind = 2, u.has_pos = true;
    u.pos[0] = 10, u.pos[1] = 0, u.pos[2] = 0;
    const double near[3] = {12, 0, 0}, far[3] = {30, 0, 0};
    CHECK(HostRemovalAllowed(u, Key(0x020007, 2), true, near));
    CHECK(!HostRemovalAllowed(u, Key(0x020007, 2), false, far));  // no position given for a unit that has one: refused
    HostUnitView np = u;
    np.has_pos = false;
    CHECK(HostRemovalAllowed(np, Key(0x020007, 2), false, far));  // neither has one: id, kind and GUID decide
    CHECK(!HostRemovalAllowed(u, Key(0x020007, 2), true, far));  // another place: another vehicle
    CHECK(!HostRemovalAllowed(u, Key(0x020008, 2), true, near));
    CHECK(!HostRemovalAllowed(u, Key(0x020007, 2, 0x10), true, near));  // a GUID the unit has not
    HostUnitView g = u;
    for (int i = 0; i < 16; ++i) g.guid[i] = static_cast<std::uint8_t>(0x10 + i);
    CHECK(HostRemovalAllowed(g, Key(0x020007, 2, 0x10), true, near));
    CHECK(!HostRemovalAllowed(g, Key(0x020007, 2), true, near));
    HostUnitView r = u;
    r.removed = true;
    CHECK(!HostRemovalAllowed(r, Key(0x020007, 2), true, near));
    HostUnitView f = u;
    f.in_use = false;
    CHECK(!HostRemovalAllowed(f, Key(0x020007, 2), true, near));
    CHECK(!HostRemovalAllowed(u, Key(0x020007, 3), true, near));  // an invalid key
    ActiveInfo a;
    CHECK(!SharedActive(a, 1e6));
    a.partner_riding = true;
    CHECK(SharedActive(a, 1e6));
    a = ActiveInfo{};
    a.last_near_ms = 1e6 - 59000;
    CHECK(SharedActive(a, 1e6));
    a.last_near_ms = 1e6 - 61000;
    CHECK(!SharedActive(a, 1e6));
    a.last_drove_ms = 1e6 - 9 * 60000;
    CHECK(SharedActive(a, 1e6));
    a.last_drove_ms = 1e6 - 11 * 60000;
    CHECK(!SharedActive(a, 1e6));
    RateLimit rl(3, 6000);
    CHECK(rl.Allow(0) && rl.Allow(100) && rl.Allow(200));
    CHECK(!rl.Allow(300));
    CHECK(!rl.Allow(5999));
    CHECK(rl.Allow(6001));  // the first one is 6 s old
    CHECK(!rl.Allow(6050));
    CHECK(rl.Allow(6200));
    RateLimit none(0, 1000);
    CHECK(!none.Allow(0));
    const double a3[3] = {0, 3, 4}, b3[3] = {0, 0, 0};
    CHECK(std::fabs(Dist(a3, b3) - 5.0) < 1e-12);
}

// ---------------------------------------------------------------- stage 2: STD1, VOWN, ownership, bands, clearance
DriveFrame Drive(DriveType t) {
    DriveFrame f;
    f.type = t;
    f.flags = static_cast<std::uint8_t>((t == DriveType::drive ? kDrvSeated : 0) | kDrvEngine | kDrvBattery);
    f.ride_state = t == DriveType::drive ? 2 : 0;
    f.seq = 77;
    f.sender_ms = 123456;
    f.epoch = 3;
    f.key = Key(0x020005, 2, 0x40);
    f.pos[0] = -1212.27, f.pos[1] = 280.51, f.pos[2] = 351.25;
    const float rows[9] = {0.8660254f, 0.5f, 0, -0.5f, 0.8660254f, 0, 0, 0, 1};
    QuatFromRows(rows, f.quat);
    f.vel[0] = 4.5f, f.vel[1] = -1.25f, f.vel[2] = 0.1f;
    f.avel[2] = 0.3f;
    f.speed_kmh = 17.5f;
    f.life = 421.0f, f.life_max = 1000.0f;
    f.battery = 812.5f;
    if (t == DriveType::drive) {
        const float in[11] = {0.784f, -0.36f, 0, 1, 1, 1, 0, 0, 0, 1, 0};
        std::memcpy(f.input, in, sizeof(in));
        f.input[44] = 1;
    }
    return f;
}

bool SameDrive(const DriveFrame& a, const DriveFrame& b) {
    return a.type == b.type && a.flags == b.flags && a.ride_state == b.ride_state && a.seq == b.seq && a.sender_ms == b.sender_ms && a.epoch == b.epoch &&
           a.key == b.key && std::memcmp(a.pos, b.pos, sizeof(a.pos)) == 0 && std::memcmp(a.quat, b.quat, sizeof(a.quat)) == 0 &&
           std::memcmp(a.vel, b.vel, sizeof(a.vel)) == 0 && std::memcmp(a.avel, b.avel, sizeof(a.avel)) == 0 && a.speed_kmh == b.speed_kmh &&
           a.life == b.life && a.life_max == b.life_max && a.battery == b.battery && std::memcmp(a.input, b.input, sizeof(a.input)) == 0;
}

void TestDriveFrames() {
    std::uint8_t buf[256];
    for (DriveType t : {DriveType::drive, DriveType::park}) {
        const DriveFrame f = Drive(t);
        const std::size_t n = EncodeDrive(f, buf, sizeof(buf));
        CHECK(n == (t == DriveType::drive ? kDriveFrameSize : kDriveParkSize));
        CHECK(LooksLikeDrive(buf, n));
        DriveFrame back;
        CHECK(DecodeDrive(buf, n, back));
        CHECK(SameDrive(f, back));
        CHECK(EncodeDrive(f, buf, n - 1) == 0);  // no room
        // every truncation and one byte too many
        for (std::size_t k = 0; k < n; ++k) CHECK(!DecodeDrive(buf, k, back));
        std::uint8_t big[257] = {};
        std::memcpy(big, buf, n);
        CHECK(!DecodeDrive(big, n + 1, back));
    }
    auto rejects = [&](DriveFrame f, const char* what) {
        std::uint8_t b[256];
        // the encoder refuses what the decoder would (it checks its own output)
        const bool enc = EncodeDrive(f, b, sizeof(b)) != 0;
        if (enc) std::printf("  (encoder accepted: %s)\n", what);
        CHECK(!enc);
    };
    {
        DriveFrame f = Drive(DriveType::park);
        f.flags |= kDrvSeated;
        rejects(f, "seated on a park frame");
        f = Drive(DriveType::drive);
        f.flags = static_cast<std::uint8_t>(f.flags & ~kDrvSeated);
        rejects(f, "a drive frame without seated");
        f = Drive(DriveType::park);
        f.flags |= 64;
        rejects(f, "unknown flag");
        f = Drive(DriveType::park);
        f.ride_state = 16;
        rejects(f, "ride state 16");
        f = Drive(DriveType::park);
        f.key = Key(0x020005, 3);
        rejects(f, "kind not the id's");
        f = Drive(DriveType::park);
        f.pos[1] = std::numeric_limits<double>::quiet_NaN();
        rejects(f, "NaN position");
        f = Drive(DriveType::park);
        f.pos[0] = 2e7;
        rejects(f, "far position");
        f = Drive(DriveType::park);
        f.quat[0] = f.quat[1] = f.quat[2] = f.quat[3] = 0;
        rejects(f, "zero quaternion");
        f = Drive(DriveType::park);
        f.quat[3] = 1.5f;
        rejects(f, "non-unit quaternion");
        f = Drive(DriveType::park);
        f.vel[0] = 250;
        rejects(f, "fast");
        f = Drive(DriveType::park);
        f.avel[1] = 60;
        rejects(f, "spinning");
        f = Drive(DriveType::park);
        f.speed_kmh = std::numeric_limits<float>::infinity();
        rejects(f, "inf speed");
        f = Drive(DriveType::park);
        f.life = 1001;
        rejects(f, "life over max");
        f = Drive(DriveType::park);
        f.life = -1;
        rejects(f, "negative life");
        f = Drive(DriveType::park);
        f.flags = static_cast<std::uint8_t>(f.flags & ~kDrvBattery);
        rejects(f, "battery without its flag");
        f = Drive(DriveType::park);
        f.battery = -2;
        rejects(f, "negative battery");
        f = Drive(DriveType::drive);
        f.input[47] = 1;
        rejects(f, "padding byte");
        f = Drive(DriveType::drive);
        f.input[45] = 2;
        rejects(f, "input byte 2");
        f = Drive(DriveType::drive);
        const float big = 5.0f;
        std::memcpy(f.input + 4, &big, 4);
        rejects(f, "input axis 5");
        f = Drive(DriveType::drive);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(f.input, &nan, 4);
        rejects(f, "NaN input");
    }
    // raw corruptions the encoder cannot make
    {
        const DriveFrame f = Drive(DriveType::drive);
        const std::size_t n = EncodeDrive(f, buf, sizeof(buf));
        DriveFrame back;
        std::uint8_t c[256];
        std::memcpy(c, buf, n);
        c[0] = 'X';
        CHECK(!DecodeDrive(c, n, back));
        std::memcpy(c, buf, n);
        c[4] = 2;  // version
        CHECK(!DecodeDrive(c, n, back));
        std::memcpy(c, buf, n);
        c[5] = 2;  // type park with the drive size
        CHECK(!DecodeDrive(c, n, back));
        std::memcpy(c, buf, n);
        c[5] = 3;
        CHECK(!DecodeDrive(c, n, back));
        std::memcpy(c, buf, n);
        c[20 + 21] = 1;  // the key's reserved bytes
        CHECK(!DecodeDrive(c, n, back));
        // a park frame with the drive type byte
        const DriveFrame p = Drive(DriveType::park);
        const std::size_t pn = EncodeDrive(p, c, sizeof(c));
        CHECK(pn == kDriveParkSize);
        c[5] = 1;
        CHECK(!DecodeDrive(c, pn, back));
    }
    // InputsValid alone
    {
        std::uint8_t in[kDriveInputBytes] = {};
        CHECK(InputsValid(in));
        in[44] = 1, in[45] = 1, in[46] = 1;
        CHECK(InputsValid(in));
        const float m4 = -4.0f;
        std::memcpy(in + 40, &m4, 4);
        CHECK(InputsValid(in));
        const float m41 = -4.01f;
        std::memcpy(in + 40, &m41, 4);
        CHECK(!InputsValid(in));
    }
}

void TestOwnMessages() {
    std::uint8_t buf[256];
    Own c;
    c.type = OwnType::claim;
    c.key = Key(0x020005, 2, 0x40);
    c.owner = kOwnerGuest;
    c.reason = kClaimPush;
    c.epoch = 4;
    c.sender_ms = 99;
    std::size_t n = EncodeOwn(c, buf, sizeof(buf));
    CHECK(n == kHeaderSize + kOwnBodySize);
    std::uint8_t type = 0;
    CHECK(PeekType(buf, n, &type) && type == 16 && IsExternalType(type));
    Own back;
    CHECK(DecodeOwn(buf, n, back));
    CHECK(back.type == OwnType::claim && back.key == c.key && back.owner == kOwnerGuest && back.reason == kClaimPush && back.epoch == 4 &&
          back.sender_ms == 99 && !back.has_xf && back.world == 0);
    c.world = 3;  // the sender's world travels with it
    n = EncodeOwn(c, buf, sizeof(buf));
    CHECK(n == kHeaderSize + kOwnBodySize && DecodeOwn(buf, n, back) && back.world == 3);
    c.world = 0;
    // a release with its transform
    Own r;
    r.type = OwnType::release;
    r.key = Key(0x01E001, 3);
    r.owner = kOwnerGuest;
    r.reason = kReleaseAtRest;
    r.epoch = 5;
    r.has_xf = true;
    r.pos[0] = 10.5, r.pos[1] = -20.25, r.pos[2] = 300.125;
    const float rows[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
    QuatFromRows(rows, r.quat);
    r.vel[1] = 0.05f;
    n = EncodeOwn(r, buf, sizeof(buf));
    CHECK(n == kHeaderSize + kOwnBodySize);
    CHECK(DecodeOwn(buf, n, back));
    CHECK(back.type == OwnType::release && back.has_xf && back.pos[2] == 300.125 && back.vel[1] == 0.05f && back.reason == kReleaseAtRest);
    // grant / deny
    Own g;
    g.type = OwnType::grant;
    g.key = c.key;
    g.owner = kOwnerGuest;
    g.reason = kGrantClaim;
    g.epoch = 5;
    CHECK(EncodeOwn(g, buf, sizeof(buf)) != 0);
    g.reason = 9;
    CHECK(EncodeOwn(g, buf, sizeof(buf)) == 0);
    Own d;
    d.type = OwnType::deny;
    d.key = c.key;
    d.owner = kOwnerGuest;  // a deny always names the host
    CHECK(EncodeOwn(d, buf, sizeof(buf)) == 0);
    d.owner = kOwnerHost;
    d.reason = kDenyHostDrives;
    CHECK(EncodeOwn(d, buf, sizeof(buf)) != 0);
    d.reason = kDenyNotThere;  // the host does not see the guest's Sam at it
    CHECK(EncodeOwn(d, buf, sizeof(buf)) != 0);
    d.reason = kDenyNotThere + 1;
    CHECK(EncodeOwn(d, buf, sizeof(buf)) == 0);
    // a claim must come from the guest, carries no transform
    Own bad = c;
    bad.owner = kOwnerHost;
    CHECK(EncodeOwn(bad, buf, sizeof(buf)) == 0);
    bad = c;
    bad.has_xf = true;
    CHECK(EncodeOwn(bad, buf, sizeof(buf)) == 0);
    bad = r;
    bad.quat[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(EncodeOwn(bad, buf, sizeof(buf)) == 0);
    bad = c;
    bad.key = Key(0x123456, 2);
    CHECK(EncodeOwn(bad, buf, sizeof(buf)) == 0);
    // raw: a stage-1 type, sizes, reserved byte, has_xf 2, values without has_xf
    n = EncodeOwn(c, buf, sizeof(buf));
    std::uint8_t x[256];
    std::memcpy(x, buf, n);
    x[4] = 20;
    CHECK(!DecodeOwn(x, n, back));
    std::memcpy(x, buf, n);
    CHECK(!DecodeOwn(x, n - 1, back));
    std::memcpy(x, buf, n);
    x[kHeaderSize + 27] = 1;  // reserved
    CHECK(!DecodeOwn(x, n, back));
    std::memcpy(x, buf, n);
    x[kHeaderSize + 26] = 2;  // has_xf
    CHECK(!DecodeOwn(x, n, back));
    std::memcpy(x, buf, n);
    x[kHeaderSize + 40] = 1;  // a position byte without has_xf
    CHECK(!DecodeOwn(x, n, back));
    CHECK(std::strcmp(OwnTypeName(17), "GRANT") == 0 && std::strcmp(OwnTypeName(3), "?") == 0);
}

void TestOwnership() {
    OwnState s;  // the host's, epoch 0
    // the guest claims a free vehicle: granted, epoch 1
    OwnStep st = HostOnClaim(s, kClaimDrive, false, -1);
    CHECK(st.send && st.type == OwnType::grant && st.next.owner == kOwnerGuest && st.next.epoch == 1);
    s = st.next;
    // a repeated claim: granted again, same epoch
    st = HostOnClaim(s, kClaimDrive, false, -1);
    CHECK(st.send && st.type == OwnType::grant && st.next.owner == kOwnerGuest && st.next.epoch == 1);
    // the guest's frames count with that epoch only
    CHECK(HostAcceptsGuestFrame(s, 1) && !HostAcceptsGuestFrame(s, 0) && !HostAcceptsGuestFrame(s, 2));
    // the host's Sam gets on (both pressed X): the host takes it, epoch 2
    st = HostOnHostTakes(s);
    CHECK(st.send && st.type == OwnType::grant && st.reason == kGrantHostTook && st.next.owner == kOwnerHost && st.next.epoch == 2);
    s = st.next;
    CHECK(!HostAcceptsGuestFrame(s, 2));
    // nothing to take from itself
    st = HostOnHostTakes(s);
    CHECK(!st.send && st.next.epoch == 2);
    // a claim while the host drives: denied
    st = HostOnClaim(s, kClaimDrive, true, 0);
    CHECK(st.send && st.type == OwnType::deny && st.reason == kDenyHostDrives && st.next.owner == kOwnerHost && st.next.epoch == 2);
    // a claim 100 ms after the host got on (it is still getting on): the host first
    st = HostOnClaim(s, kClaimDrive, false, 100);
    CHECK(st.send && st.type == OwnType::deny && st.reason == kDenyHostFirst);
    // a claim long after: granted
    st = HostOnClaim(s, kClaimPush, false, 5000);
    CHECK(st.type == OwnType::grant && st.next.owner == kOwnerGuest && st.next.epoch == 3);
    s = st.next;
    // a claim of a guest-owned vehicle while the host's Sam is on it: back to the host
    OwnStep tie = HostOnClaim(s, kClaimDrive, true, 0);
    CHECK(tie.type == OwnType::deny && tie.next.owner == kOwnerHost && tie.next.epoch == 4);
    // release: got off keeps it with the guest; at rest with the wrong epoch does nothing; at rest hands it back
    st = HostOnRelease(s, kReleaseGotOff, 3);
    CHECK(!st.send && st.next.owner == kOwnerGuest);
    st = HostOnRelease(s, kReleaseAtRest, 2);
    CHECK(!st.send && st.next.owner == kOwnerGuest);
    st = HostOnRelease(s, kReleaseAtRest, 3);
    CHECK(st.send && st.type == OwnType::grant && st.reason == kGrantHandback && st.next.owner == kOwnerHost && st.next.epoch == 4);
    // silence: 9 s nothing, 10 s back to the host
    st = HostOnSilence(s, 9000);
    CHECK(!st.send);
    st = HostOnSilence(s, 10000);
    CHECK(st.send && st.reason == kGrantTimeout && st.next.owner == kOwnerHost && st.next.epoch == 4);
    // the guest's side
    OwnState gs;
    Own m;
    m.type = OwnType::grant;
    m.owner = kOwnerGuest;
    m.epoch = 3;
    bool ch = false;
    gs = GuestOnAnswer(gs, m, &ch);
    CHECK(ch && gs.owner == kOwnerGuest && gs.epoch == 3);
    gs = GuestOnAnswer(gs, m, &ch);
    CHECK(!ch && gs.owner == kOwnerGuest);
    m.owner = kOwnerHost;
    m.epoch = 2;  // older: ignored
    gs = GuestOnAnswer(gs, m, &ch);
    CHECK(!ch && gs.owner == kOwnerGuest && gs.epoch == 3);
    m.type = OwnType::deny;
    m.epoch = 4;
    gs = GuestOnAnswer(gs, m, &ch);
    CHECK(ch && gs.owner == kOwnerHost && gs.epoch == 4);
    m.type = OwnType::claim;  // not an answer
    m.epoch = 9;
    gs = GuestOnAnswer(gs, m, &ch);
    CHECK(!ch && gs.epoch == 4);
    gs = GuestOnHostFrame(gs, 4, &ch);
    CHECK(!ch);
    OwnState g2;
    g2.owner = kOwnerGuest;
    g2.epoch = 5;
    g2 = GuestOnHostFrame(g2, 6, &ch);
    CHECK(ch && g2.owner == kOwnerHost && g2.epoch == 6);
    // the rest detector
    RestDetector rd;
    CHECK(!rd.Update(0.1, 0));
    CHECK(!rd.Update(0.1, 999));
    CHECK(rd.Update(0.19, 1000));
    CHECK(!rd.Update(0.3, 1100));
    CHECK(!rd.Update(0.0, 1200));
    CHECK(rd.Update(0.0, 2200));
    CHECK(!rd.Update(std::numeric_limits<double>::quiet_NaN(), 2300));
}

void TestBandsAndClearance() {
    CHECK(ChooseBand(0.05, 1.0) == Band::none);
    CHECK(ChooseBand(0.05, 3.5) == Band::impulse);
    CHECK(ChooseBand(0.5, 0) == Band::impulse);
    CHECK(ChooseBand(1.99, 10) == Band::impulse);
    CHECK(ChooseBand(1.0, 50) == Band::interp);
    CHECK(ChooseBand(2.0, 0) == Band::interp);
    CHECK(ChooseBand(5.0, 0) == Band::interp);
    CHECK(ChooseBand(5.01, 0) == Band::teleport);
    CHECK(ChooseBand(std::numeric_limits<double>::quiet_NaN(), 0) == Band::teleport);
    BandLimits tight;
    tight.ignore_m = 0.3;
    CHECK(ChooseBand(0.25, 1) == Band::impulse && ChooseBand(0.25, 1, tight) == Band::none);
    CHECK(std::strcmp(BandName(Band::interp), "interpolated teleport") == 0);
    // the nudge: clamped at 2 m/s, added to the measured velocity
    const double err[3] = {3.0, 4.0, 0.0};
    const float meas[3] = {10, 0, 0};
    float out[3];
    NudgeVelocity(err, meas, 0.5, 2.0, out);
    CHECK(std::fabs(out[0] - 11.2f) < 1e-4f && std::fabs(out[1] - 1.6f) < 1e-4f && out[2] == 0);
    const double small[3] = {0.1, 0, 0};
    NudgeVelocity(small, meas, 0.5, 2.0, out);
    CHECK(std::fabs(out[0] - 10.2f) < 1e-4f);
    // footprints: a truck at the origin facing +y
    Footprint t;
    KindHalfExtents(2, t.half);
    CHECK(t.half[1] > t.half[0]);
    const double inside[3] = {0.5, 3.0, 0.5}, beside[3] = {2.5, 0, 0}, near_edge[3] = {1.9, 0, 0}, above[3] = {0, 0, 4.0};
    CHECK(PointInFootprint(t, inside, 0));
    CHECK(!PointInFootprint(t, beside, 0.6));
    CHECK(PointInFootprint(t, near_edge, 0.6));
    CHECK(!PointInFootprint(t, above, 0.6));
    // rotated 90 deg: the long axis along x
    Footprint r = t;
    const float rot90[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
    std::memcpy(r.rot, rot90, sizeof(rot90));
    const double along_x[3] = {4.0, 0, 0};
    CHECK(PointInFootprint(r, along_x, 0) && !PointInFootprint(t, along_x, 0));
    // two trucks side by side 3.5 m apart (parallel): clear; 2.5 m: overlap; one rotated across: overlap
    Footprint o = t;
    o.c[0] = 3.5;
    CHECK(!FootprintsOverlap(t, o));
    o.c[0] = 2.5;
    CHECK(FootprintsOverlap(t, o));
    Footprint across = r;
    across.c[1] = 4.0;
    CHECK(FootprintsOverlap(t, across));
    across.c[1] = 6.5;
    CHECK(!FootprintsOverlap(t, across));
    // one above the other (a bridge): clear
    Footprint up = t;
    up.c[2] = 5.0;
    CHECK(!FootprintsOverlap(t, up));
    // the teleport rule
    const double sam_far[3] = {20, 0, 0}, sam_in[3] = {0, 1, 0.5};
    Footprint far = t;
    far.c[0] = 30;
    CHECK(TeleportClear(t, true, sam_far, &far, 1));
    CHECK(!TeleportClear(t, true, sam_in, &far, 1));
    CHECK(TeleportClear(t, false, sam_in, nullptr, 0));
    o.c[0] = 2.5;
    CHECK(!TeleportClear(t, true, sam_far, &o, 1));
    // quaternion round trips and angles
    const float rows[9] = {0.8660254f, 0.5f, 0, -0.5f, 0.8660254f, 0, 0, 0, 1};
    float q[4], back[9];
    QuatFromRows(rows, q);
    RowsFromQuat(q, back);
    for (int i = 0; i < 9; ++i) CHECK(std::fabs(back[i] - rows[i]) < 1e-5f);
    const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    CHECK(std::fabs(AngleBetweenRows(rows, ident) - 30.0) < 1e-3);
    CHECK(AngleBetweenRows(ident, ident) < 1e-3);
    CHECK(std::fabs(AngleBetweenRows(rot90, ident) - 90.0) < 1e-3);
    const float zero[4] = {0, 0, 0, 0};
    RowsFromQuat(zero, back);  // degenerate: no NaN
    for (float v : back) CHECK(std::isfinite(v));
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestDriveFrames();
    TestOwnMessages();
    TestOwnership();
    TestBandsAndClearance();
    TestIdentity();
    TestRoundTrips();
    TestRejections();
    TestSnapshot();
    TestRequestFlags();
    TestGate();
    TestBinding();
    TestHostRules();
    TestReviewRules();
    if (g_fail) {
        std::printf("st_vehicle_wire_tests: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    std::printf("st_vehicle_wire_tests: all %d checks passed\n", g_checks);
    return 0;
}
