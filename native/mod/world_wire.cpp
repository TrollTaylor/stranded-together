// Shared structures, the pure part (world_wire.hpp; docs/world-sync.md).
#include "world_wire.hpp"

#include <algorithm>
#include <cmath>

namespace st::wwire {
namespace {

constexpr std::uint8_t kMagic[4] = {'S', 'W', 'S', '1'};

struct W {
    std::uint8_t* p;
    std::size_t cap, n = 0;
    bool ok = true;
    W(std::uint8_t* out, std::size_t c) : p(out), cap(c) {}
    void Bytes(const void* v, std::size_t k) {
        if (!ok || n + k > cap) {
            ok = false;
            return;
        }
        std::memcpy(p + n, v, k);
        n += k;
    }
    void U8(std::uint8_t v) { Bytes(&v, 1); }
    void U16(std::uint16_t v) { Bytes(&v, 2); }
    void U32(std::uint32_t v) { Bytes(&v, 4); }
    void U64(std::uint64_t v) { Bytes(&v, 8); }
    void F64(double v) { Bytes(&v, 8); }
    void F32(float v) { Bytes(&v, 4); }
    std::size_t Done() const { return ok ? n : 0; }
};
struct R {
    const std::uint8_t* p;
    std::size_t size, n = 0;
    bool ok = true;
    R(const std::uint8_t* in, std::size_t s) : p(in), size(s) {}
    void Bytes(void* v, std::size_t k) {
        if (!ok || n + k > size) {
            ok = false;
            std::memset(v, 0, k);
            return;
        }
        std::memcpy(v, p + n, k);
        n += k;
    }
    std::uint8_t U8() { std::uint8_t v; Bytes(&v, 1); return v; }
    std::uint16_t U16() { std::uint16_t v; Bytes(&v, 2); return v; }
    std::uint32_t U32() { std::uint32_t v; Bytes(&v, 4); return v; }
    std::uint64_t U64() { std::uint64_t v; Bytes(&v, 8); return v; }
    double F64() { double v; Bytes(&v, 8); return v; }
    float F32() { float v; Bytes(&v, 4); return v; }
    bool End() const { return ok && n == size; }
};

bool Finite(double v) noexcept { return std::isfinite(v); }
bool PosOk(const double (&p)[3]) noexcept {
    for (double v : p)
        if (!Finite(v)) return false;
    return std::fabs(p[0]) <= kMaxCoord && std::fabs(p[1]) <= kMaxCoord && std::fabs(p[2]) <= kMaxZ;
}
bool RotOk(const float (&r)[9]) noexcept {
    bool any = false;
    for (float v : r) {
        if (!std::isfinite(v) || std::fabs(v) > 1.5f) return false;
        if (v != 0) any = true;
    }
    if (!any) return false;
    // each row roughly unit length (a rotation, possibly with the game's tiny scale noise)
    for (int i = 0; i < 3; ++i) {
        const double l = static_cast<double>(r[i * 3]) * r[i * 3] + static_cast<double>(r[i * 3 + 1]) * r[i * 3 + 1] +
                         static_cast<double>(r[i * 3 + 2]) * r[i * 3 + 2];
        if (l < 0.8 || l > 1.2) return false;
    }
    return true;
}

void PutXf(W& w, const Xf& x) {
    for (double v : x.pos) w.F64(v);
    for (float v : x.rot) w.F32(v);
}
void GetXf(R& r, Xf& x) {
    for (double& v : x.pos) v = r.F64();
    for (float& v : x.rot) v = r.F32();
}
void PutReq(W& w, const ReqId& q) {
    w.U32(q.nonce);
    w.U16(q.n);
}
ReqId GetReq(R& r) {
    ReqId q;
    q.nonce = r.U32();
    q.n = r.U16();
    return q;
}
void PutEntry(W& w, const Entry& e) {
    w.U32(e.hid);
    w.U8(e.type);
    w.U8(e.state);
    w.U8(e.level);
    w.U8(e.flags);
    for (double v : e.pos) w.F64(v);
}
bool GetEntry(R& r, Entry& e) {
    e.hid = r.U32();
    e.type = r.U8();
    e.state = r.U8();
    e.level = r.U8();
    e.flags = r.U8();
    for (double& v : e.pos) v = r.F64();
    return r.ok && e.hid != 0 && e.type <= 45 && e.state <= 3 && e.level <= 5 && (e.flags & ~kEntAllBits) == 0 && PosOk(e.pos);
}

bool Begin(const std::uint8_t* in, std::size_t n, Msg t, Header* h) noexcept {
    return ReadHeader(in, n, h) && h->type == static_cast<std::uint8_t>(t);
}
std::size_t Start(W& w, const Header& h, Msg t) {
    Header hh = h;
    hh.type = static_cast<std::uint8_t>(t);
    const std::size_t k = WriteHeader(hh, w.p, w.cap);
    if (!k) w.ok = false;
    w.n = k;
    return k;
}

std::uint64_t Fnv(std::uint64_t h, const void* p, std::size_t n) noexcept {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}
constexpr std::uint64_t kFnvBasis = 14695981039346656037ull;

}  // namespace

const char* MsgName(std::uint8_t t) noexcept {
    switch (static_cast<Msg>(t)) {
        case Msg::hello: return "HELLO";
        case Msg::snap_req: return "SNAP_REQ";
        case Msg::sum: return "SUM";
        case Msg::bucket_req: return "BUCKET_REQ";
        case Msg::digest: return "DIGEST";
        case Msg::need: return "NEED";
        case Msg::add: return "ADD";
        case Msg::gbuild: return "GBUILD";
        case Msg::rej: return "REJ";
        case Msg::rem: return "REM";
        case Msg::remreq: return "REMREQ";
        case Msg::mats: return "MATS";
        case Msg::state: return "STATE";
        case Msg::dur: return "DUR";
        case Msg::check: return "CHECK";
        case Msg::lost: return "LOST";
    }
    return "?";
}
const char* RejName(std::uint8_t r) noexcept {
    switch (static_cast<RejReason>(r)) {
        case RejReason::invalid: return "invalid";
        case RejReason::type: return "type";
        case RejReason::disabled: return "disabled";
        case RejReason::too_far: return "far";
        case RejReason::not_ready: return "not_ready";
        case RejReason::host_real: return "host_real";
    }
    return "?";
}
const char* RemName(std::uint8_t r) noexcept {
    switch (static_cast<RemReason>(r)) {
        case RemReason::manual: return "manual";
        case RemReason::evicted: return "evicted";
        case RemReason::decay: return "decay";
        case RemReason::area: return "area";
        case RemReason::pickup: return "pickup";
        case RemReason::guest: return "guest";
    }
    return "?";
}

std::size_t WriteHeader(const Header& h, std::uint8_t* out, std::size_t cap) noexcept {
    if (!out || cap < kHeaderSize || h.type < 1 || h.type > 16 || (h.flags & ~kFlagAll)) return 0;
    std::memcpy(out, kMagic, 4);
    out[4] = h.type;
    out[5] = kVersion;
    std::memcpy(out + 6, &h.flags, 2);
    std::memcpy(out + 8, &h.host_world, 4);
    std::memcpy(out + 12, &h.seq, 4);
    return kHeaderSize;
}
bool ReadHeader(const std::uint8_t* in, std::size_t size, Header* h) noexcept {
    if (!in || !h || size < kHeaderSize || size > kMaxMessage || std::memcmp(in, kMagic, 4) != 0 || in[5] != kVersion) return false;
    h->type = in[4];
    std::memcpy(&h->flags, in + 6, 2);
    std::memcpy(&h->host_world, in + 8, 4);
    std::memcpy(&h->seq, in + 12, 4);
    return h->type >= 1 && h->type <= 16 && (h->flags & ~kFlagAll) == 0 && h->seq != 0;
}

// ---------------------------------------------------------------- Record
int RecordOffset(std::size_t info_off) noexcept {
    std::size_t at = 0;
    for (const Range& r : kRanges) {
        if (info_off >= r.off && info_off < static_cast<std::size_t>(r.off) + r.len) return static_cast<int>(at + (info_off - r.off));
        at += r.len;
    }
    return -1;
}
void Record::Pos(double (&out)[3]) const noexcept { std::memcpy(out, b + RecordOffset(0x08), 24); }
void Record::SetPos(const double (&p)[3]) noexcept { std::memcpy(b + RecordOffset(0x08), p, 24); }
void Record::Rot(float (&out)[9]) const noexcept { std::memcpy(out, b + RecordOffset(0x20), 36); }
std::uint32_t Record::Level() const noexcept {
    std::uint32_t v;
    std::memcpy(&v, b + RecordOffset(0x4C), 4);
    return v;
}
std::uint8_t Record::State() const noexcept { return b[RecordOffset(0x70)]; }
float Record::Durability() const noexcept {
    float v;
    std::memcpy(&v, b + RecordOffset(0x7C), 4);
    return v;
}
std::uint8_t Record::Flag(std::size_t info_off) const noexcept {
    const int o = RecordOffset(info_off);
    return o >= 0 ? b[o] : 0;
}
void RecordFromInfo(const std::uint8_t* info, Record* out) noexcept {
    std::size_t at = 0;
    for (const Range& r : kRanges) {
        std::memcpy(out->b + at, info + r.off, r.len);
        at += r.len;
    }
}
void RecordToInfo(const Record& rec, std::uint8_t* info) noexcept {
    std::size_t at = 0;
    for (const Range& r : kRanges) {
        std::memcpy(info + r.off, rec.b + at, r.len);
        at += r.len;
    }
}
bool RecordValid(const Record& r) noexcept {
    if (r.Type() > 45) return false;
    double p[3];
    float rot[9];
    r.Pos(p);
    r.Rot(rot);
    if (!PosOk(p) || !RotOk(rot)) return false;
    if (r.Level() > 5 || r.State() > 3 || !std::isfinite(r.Durability()) || r.Durability() < 0) return false;
    return SubclassValid(r);
}
// W3: the ladder / rope fields the subclass InitFromInfo reads (ds.exe, decompiled):
//   ladder 0x142D2DD00: +0x578 i32 mode (0 / other: two geometry branches), +0x57C / +0x580 f32 lengths (<= 0: 9.65 m),
//                       +0x584 / +0x588 i32 flags (== 1)
//   rope   0x142D24530: +0x578 f32 length (clamped 1..30 m), +0x57C..+0x584 f32[3] the rope's direction, +0x588 f32 the
//                       longest length (<= 0: 30 m)
// Finite and in a sane range (the game clamps the rest itself).
bool SubclassValid(const Record& r) noexcept {
    const int t = r.Type();
    auto f = [&](std::size_t off) {
        float v = 0;
        std::memcpy(&v, r.b + RecordOffset(off), 4);
        return v;
    };
    if (t >= 24 && t <= 26) {
        const float a = f(0x57C), b = f(0x580);
        return std::isfinite(a) && std::isfinite(b) && a >= -1.0f && a <= 100.0f && b >= -1.0f && b <= 100.0f;  // the flags: any value (== 1 is read)
    }
    if (t >= 27 && t <= 29) {
        for (std::size_t off = 0x578; off <= 0x588; off += 4)
            if (!std::isfinite(f(off)) || f(off) < -1000.0f || f(off) > 1000.0f) return false;
        return f(0x578) <= 100.0f && f(0x588) <= 100.0f;
    }
    if (t >= 21 && t <= 23) {
        // W4: the zipline's ExportInfo 0x142CE5A40 writes cp+0x520 (a byte: 0 / 2 from its update) as a u32
        std::uint32_t v = 0;
        std::memcpy(&v, r.b + RecordOffset(0x578), 4);
        return v <= 255u;
    }
    return true;
}
bool XfValid(const Xf& x) noexcept { return PosOk(x.pos) && RotOk(x.rot); }

// ---------------------------------------------------------------- hashing
int BucketOf(std::uint32_t hid) noexcept { return static_cast<int>(hid & (kBuckets - 1)); }
std::uint64_t EntryHash(const Entry& e) noexcept {
    std::uint64_t h = kFnvBasis;
    h = Fnv(h, &e.hid, 4);
    h = Fnv(h, &e.type, 1);
    // W3: not a ladder's / rope's position: the game re-attaches it to the ground in each world (a cm apart, run
    // 20261121-175940: the join check healed every 10 s); the DIGEST binding compares them with the guard's tolerance.
    if (e.type >= 24 && e.type <= 29) return h;
    for (double v : e.pos) {
        const long long cm = std::llround(v * 100.0);
        h = Fnv(h, &cm, 8);
    }
    return h;
}
void BucketHashes(const std::vector<Entry>& entries, std::uint64_t (&buckets)[kBuckets]) noexcept {
    std::vector<const Entry*> sorted;
    sorted.reserve(entries.size());
    for (const Entry& e : entries) sorted.push_back(&e);
    std::sort(sorted.begin(), sorted.end(), [](const Entry* a, const Entry* b) { return a->hid < b->hid; });
    for (auto& b : buckets) b = kFnvBasis;
    for (const Entry* e : sorted) {
        const std::uint64_t eh = EntryHash(*e);
        auto& b = buckets[BucketOf(e->hid)];
        b = Fnv(b, &eh, 8);
    }
}
std::uint64_t CheckHash(const std::uint64_t (&buckets)[kBuckets]) noexcept { return Fnv(kFnvBasis, buckets, sizeof(buckets)); }

// ---------------------------------------------------------------- encoders
std::size_t EncodeHello(const Header& h, const Hello& b, std::uint8_t* out, std::size_t cap) noexcept {
    W w(out, cap);
    Start(w, h, Msg::hello);
    w.U16(b.proto);
    w.U8(b.on);
    w.U8(b.parts);
    w.U8(b.anim_mode);
    w.U8(b.ready);
    w.U32(b.world);
    w.U32(b.sender);
    return b.anim_mode <= 3 && (b.parts & ~15u) == 0 && b.on <= 1 && b.ready <= 1 ? w.Done() : 0;
}
std::size_t EncodeSnapReq(const Header& h, const SnapReq& b, std::uint8_t* out, std::size_t cap) noexcept {
    W w(out, cap);
    Start(w, h, Msg::snap_req);
    w.U32(b.guest_world);
    w.U8(b.reason);
    return b.reason <= 2 ? w.Done() : 0;
}
std::size_t EncodeSum(const Header& h, const Sum& b, std::uint8_t* out, std::size_t cap) noexcept {
    W w(out, cap);
    Start(w, h, Msg::sum);
    w.U32(b.count);
    for (std::uint64_t v : b.buckets) w.U64(v);
    return b.count <= 3140 ? w.Done() : 0;
}
std::size_t EncodeBucketReq(const Header& h, const BucketReq& b, std::uint8_t* out, std::size_t cap) noexcept {
    W w(out, cap);
    Start(w, h, Msg::bucket_req);
    w.U64(b.mask);
    return b.mask ? w.Done() : 0;
}
std::size_t EncodeDigest(const Header& h, const Digest& b, std::uint8_t* out, std::size_t cap) noexcept {
    if (b.entries.size() > kDigestMax || b.parts == 0 || b.part >= b.parts) return 0;
    W w(out, cap);
    Start(w, h, Msg::digest);
    w.U16(b.serial);
    w.U8(b.part);
    w.U8(b.parts);
    w.U64(b.mask);
    w.U8(static_cast<std::uint8_t>(b.entries.size()));
    for (const Entry& e : b.entries) PutEntry(w, e);
    return w.Done();
}
std::size_t EncodeNeed(const Header& h, const Need& b, std::uint8_t* out, std::size_t cap) noexcept {
    if (b.hids.empty() || b.hids.size() > kNeedMax) return 0;
    W w(out, cap);
    Start(w, h, Msg::need);
    w.U8(static_cast<std::uint8_t>(b.hids.size()));
    for (std::uint32_t v : b.hids) w.U32(v);
    return w.Done();
}
std::size_t EncodeAdd(const Header& h, const Add& b, std::uint8_t* out, std::size_t cap) noexcept {
    if (!b.hid || static_cast<std::uint8_t>(b.origin) > 2 || !RecordValid(b.rec) || (b.has_pcc && !XfValid(b.pcc))) return 0;
    W w(out, cap);
    Start(w, h, Msg::add);
    w.U32(b.hid);
    w.U8(static_cast<std::uint8_t>(b.origin));
    PutReq(w, b.req);
    w.U8(b.has_pcc ? 1 : 0);
    PutXf(w, b.has_pcc ? b.pcc : Xf{});
    w.Bytes(b.rec.b, kRecordSize);
    return w.Done();
}
std::size_t EncodeGBuild(const Header& h, const GBuild& b, std::uint8_t* out, std::size_t cap) noexcept {
    if (b.req.Empty() || !RecordValid(b.rec) || (b.has_pcc && !XfValid(b.pcc)) || !PosOk(b.body)) return 0;
    W w(out, cap);
    Start(w, h, Msg::gbuild);
    PutReq(w, b.req);
    w.U8(b.has_pcc ? 1 : 0);
    PutXf(w, b.has_pcc ? b.pcc : Xf{});
    for (double v : b.body) w.F64(v);
    w.Bytes(b.rec.b, kRecordSize);
    return w.Done();
}
std::size_t EncodeRej(const Header& h, const Rej& b, std::uint8_t* out, std::size_t cap) noexcept {
    W w(out, cap);
    Start(w, h, Msg::rej);
    PutReq(w, b.req);
    w.U8(static_cast<std::uint8_t>(b.reason));
    const auto r = static_cast<std::uint8_t>(b.reason);
    return r >= 1 && r <= 6 ? w.Done() : 0;
}
std::size_t EncodeRem(const Header& h, const Rem& b, std::uint8_t* out, std::size_t cap) noexcept {
    const auto t = static_cast<Msg>(h.type);
    const auto r = static_cast<std::uint8_t>(b.reason);
    if ((t != Msg::rem && t != Msg::remreq) || !b.hid || r < 1 || r > 6 || b.type > 45 || !PosOk(b.pos)) return 0;
    W w(out, cap);
    Start(w, h, t);
    w.U32(b.hid);
    w.U8(b.type);
    for (double v : b.pos) w.F64(v);
    w.U8(r);
    return w.Done();
}
std::size_t EncodeCheck(const Header& h, const Check& b, std::uint8_t* out, std::size_t cap) noexcept {
    W w(out, cap);
    Start(w, h, Msg::check);
    w.U32(b.count);
    w.U64(b.hash);
    return b.count <= 3140 ? w.Done() : 0;
}
std::size_t EncodeLost(const Header& h, const Lost& b, std::uint8_t* out, std::size_t cap) noexcept {
    if (!b.hid) return 0;
    W w(out, cap);
    Start(w, h, Msg::lost);
    w.U32(b.hid);
    return w.Done();
}

// ---- W2
bool MatsValid(const std::int32_t (&m)[6], std::int32_t max, bool need_positive) noexcept {
    std::int64_t sum = 0;
    for (std::int32_t v : m) {
        if (v < 0 || v > max) return false;
        sum += v;
    }
    return !need_positive || sum > 0;
}
bool DurValid(float d) noexcept { return std::isfinite(d) && d >= -1000.0f && d <= 1.0e8f; }
namespace {
void PutMats(W& w, const std::int32_t (&m)[6]) {
    for (std::int32_t v : m) w.U32(static_cast<std::uint32_t>(v));
}
void GetMats(R& r, std::int32_t (&m)[6]) {
    for (std::int32_t& v : m) v = static_cast<std::int32_t>(r.U32());
}
bool MatsOk(const Mats& b) noexcept {
    return b.hid && b.type <= 45 && PosOk(b.pos) && static_cast<std::uint8_t>(b.kind) <= 1 && MatsValid(b.m, kMaxMat, true);
}
bool StateOk(const State& b) noexcept {
    return b.hid && b.type <= 45 && PosOk(b.pos) && b.level <= kMaxLevelWire && b.max_level <= kMaxLevelWire && static_cast<std::uint8_t>(b.levelup) <= 2 &&
           static_cast<std::uint8_t>(b.why) <= 3 && MatsValid(b.own, kMaxTotal, false) && MatsValid(b.invested, kMaxTotal, false) &&
           MatsValid(b.repair, kMaxTotal, false) && MatsValid(b.excess, kMaxMat, false) && DurValid(b.dur);
}
}  // namespace
std::size_t EncodeMats(const Header& h, const Mats& b, std::uint8_t* out, std::size_t cap) noexcept {
    if (!MatsOk(b)) return 0;
    W w(out, cap);
    Start(w, h, Msg::mats);
    PutReq(w, b.req);
    w.U32(b.hid);
    w.U8(b.type);
    for (double v : b.pos) w.F64(v);
    w.U8(static_cast<std::uint8_t>(b.kind));
    PutMats(w, b.m);
    return w.Done();
}
std::size_t EncodeState(const Header& h, const State& b, std::uint8_t* out, std::size_t cap) noexcept {
    if (!StateOk(b)) return 0;
    W w(out, cap);
    Start(w, h, Msg::state);
    w.U32(b.hid);
    w.U8(b.type);
    for (double v : b.pos) w.F64(v);
    w.U8(b.level);
    w.U8(b.max_level);
    w.U8(static_cast<std::uint8_t>(b.levelup));
    w.U8(static_cast<std::uint8_t>(b.why));
    PutMats(w, b.own);
    PutMats(w, b.invested);
    PutMats(w, b.repair);
    w.F32(b.dur);
    PutReq(w, b.req);
    PutMats(w, b.excess);
    return w.Done();
}
std::size_t EncodeDur(const Header& h, const Dur& b, std::uint8_t* out, std::size_t cap) noexcept {
    if (b.e.empty() || b.e.size() > kDurMax) return 0;
    for (const DurEnt& d : b.e)
        if (!d.hid || !DurValid(d.dur)) return 0;
    W w(out, cap);
    Start(w, h, Msg::dur);
    w.U8(static_cast<std::uint8_t>(b.e.size()));
    for (const DurEnt& d : b.e) {
        w.U32(d.hid);
        w.F32(d.dur);
    }
    return w.Done();
}

// ---------------------------------------------------------------- decoders
bool DecodeHello(const std::uint8_t* in, std::size_t n, Header* h, Hello* b) noexcept {
    if (!Begin(in, n, Msg::hello, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->proto = r.U16();
    b->on = r.U8();
    b->parts = r.U8();
    b->anim_mode = r.U8();
    b->ready = r.U8();
    b->world = r.U32();
    b->sender = r.U32();
    return r.End() && b->proto >= 1 && b->on <= 1 && (b->parts & ~15u) == 0 && b->anim_mode <= 3 && b->ready <= 1;
}
bool DecodeSnapReq(const std::uint8_t* in, std::size_t n, Header* h, SnapReq* b) noexcept {
    if (!Begin(in, n, Msg::snap_req, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->guest_world = r.U32();
    b->reason = r.U8();
    return r.End() && b->reason <= 2;
}
bool DecodeSum(const std::uint8_t* in, std::size_t n, Header* h, Sum* b) noexcept {
    if (!Begin(in, n, Msg::sum, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->count = r.U32();
    for (std::uint64_t& v : b->buckets) v = r.U64();
    return r.End() && b->count <= 3140;
}
bool DecodeBucketReq(const std::uint8_t* in, std::size_t n, Header* h, BucketReq* b) noexcept {
    if (!Begin(in, n, Msg::bucket_req, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->mask = r.U64();
    return r.End() && b->mask != 0;
}
bool DecodeDigest(const std::uint8_t* in, std::size_t n, Header* h, Digest* b) noexcept {
    if (!Begin(in, n, Msg::digest, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->serial = r.U16();
    b->part = r.U8();
    b->parts = r.U8();
    b->mask = r.U64();
    const std::uint8_t k = r.U8();
    if (!r.ok || k > kDigestMax || b->parts == 0 || b->part >= b->parts || b->mask == 0) return false;
    b->entries.assign(k, Entry{});
    for (Entry& e : b->entries) {
        if (!GetEntry(r, e)) return false;
        if (!((b->mask >> BucketOf(e.hid)) & 1u)) return false;  // an entry outside the answered buckets
    }
    return r.End();
}
bool DecodeNeed(const std::uint8_t* in, std::size_t n, Header* h, Need* b) noexcept {
    if (!Begin(in, n, Msg::need, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    const std::uint8_t k = r.U8();
    if (!r.ok || k == 0 || k > kNeedMax) return false;
    b->hids.assign(k, 0);
    for (std::uint32_t& v : b->hids) {
        v = r.U32();
        if (!v) return false;
    }
    return r.End();
}
bool DecodeAdd(const std::uint8_t* in, std::size_t n, Header* h, Add* b) noexcept {
    if (!Begin(in, n, Msg::add, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->hid = r.U32();
    const std::uint8_t o = r.U8();
    b->req = GetReq(r);
    const std::uint8_t pc = r.U8();
    GetXf(r, b->pcc);
    r.Bytes(b->rec.b, kRecordSize);
    if (!r.End() || !b->hid || o > 2 || pc > 1 || !RecordValid(b->rec)) return false;
    b->origin = static_cast<Origin>(o);
    b->has_pcc = pc == 1;
    if (b->has_pcc && !XfValid(b->pcc)) return false;
    if (!b->has_pcc)
        for (double v : b->pcc.pos)
            if (v != 0) return false;
    return true;
}
bool DecodeGBuild(const std::uint8_t* in, std::size_t n, Header* h, GBuild* b) noexcept {
    if (!Begin(in, n, Msg::gbuild, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->req = GetReq(r);
    const std::uint8_t pc = r.U8();
    GetXf(r, b->pcc);
    for (double& v : b->body) v = r.F64();
    r.Bytes(b->rec.b, kRecordSize);
    if (!r.End() || b->req.Empty() || pc > 1 || !RecordValid(b->rec) || !PosOk(b->body)) return false;
    b->has_pcc = pc == 1;
    return !b->has_pcc || XfValid(b->pcc);
}
bool DecodeRej(const std::uint8_t* in, std::size_t n, Header* h, Rej* b) noexcept {
    if (!Begin(in, n, Msg::rej, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->req = GetReq(r);
    const std::uint8_t k = r.U8();
    if (!r.End() || k < 1 || k > 6) return false;
    b->reason = static_cast<RejReason>(k);
    return true;
}
bool DecodeRem(const std::uint8_t* in, std::size_t n, Header* h, Rem* b) noexcept {
    if (!ReadHeader(in, n, h) || (h->type != static_cast<std::uint8_t>(Msg::rem) && h->type != static_cast<std::uint8_t>(Msg::remreq))) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->hid = r.U32();
    b->type = r.U8();
    for (double& v : b->pos) v = r.F64();
    const std::uint8_t k = r.U8();
    if (!r.End() || !b->hid || b->type > 45 || k < 1 || k > 6 || !PosOk(b->pos)) return false;
    b->reason = static_cast<RemReason>(k);
    return true;
}
bool DecodeCheck(const std::uint8_t* in, std::size_t n, Header* h, Check* b) noexcept {
    if (!Begin(in, n, Msg::check, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->count = r.U32();
    b->hash = r.U64();
    return r.End() && b->count <= 3140;
}
bool DecodeLost(const std::uint8_t* in, std::size_t n, Header* h, Lost* b) noexcept {
    if (!Begin(in, n, Msg::lost, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->hid = r.U32();
    return r.End() && b->hid != 0;
}
bool DecodeMats(const std::uint8_t* in, std::size_t n, Header* h, Mats* b) noexcept {
    if (!Begin(in, n, Msg::mats, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->req = GetReq(r);
    b->hid = r.U32();
    b->type = r.U8();
    for (double& v : b->pos) v = r.F64();
    const std::uint8_t k = r.U8();
    b->kind = static_cast<MatsKind>(k);
    GetMats(r, b->m);
    return r.End() && k <= 1 && MatsOk(*b);
}
bool DecodeState(const std::uint8_t* in, std::size_t n, Header* h, State* b) noexcept {
    if (!Begin(in, n, Msg::state, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    b->hid = r.U32();
    b->type = r.U8();
    for (double& v : b->pos) v = r.F64();
    b->level = r.U8();
    b->max_level = r.U8();
    const std::uint8_t lu = r.U8(), why = r.U8();
    b->levelup = static_cast<LevelUp>(lu);
    b->why = static_cast<StateWhy>(why);
    GetMats(r, b->own);
    GetMats(r, b->invested);
    GetMats(r, b->repair);
    b->dur = r.F32();
    b->req = GetReq(r);
    GetMats(r, b->excess);
    return r.End() && lu <= 2 && why <= 3 && StateOk(*b);
}
bool DecodeDur(const std::uint8_t* in, std::size_t n, Header* h, Dur* b) noexcept {
    if (!Begin(in, n, Msg::dur, h)) return false;
    R r(in + kHeaderSize, n - kHeaderSize);
    const std::uint8_t k = r.U8();
    if (!r.ok || k == 0 || k > kDurMax) return false;
    b->e.resize(k);
    for (DurEnt& d : b->e) {
        d.hid = r.U32();
        d.dur = r.F32();
        if (!r.ok || !d.hid || !DurValid(d.dur)) return false;
    }
    return r.End();
}

// ---------------------------------------------------------------- digest chunks
std::vector<Digest> ChunkDigest(const std::vector<Entry>& entries, std::uint64_t mask, std::uint16_t serial) {
    std::vector<Entry> in;
    for (const Entry& e : entries)
        if ((mask >> BucketOf(e.hid)) & 1u) in.push_back(e);
    std::sort(in.begin(), in.end(), [](const Entry& a, const Entry& b) { return a.hid < b.hid; });
    std::size_t parts = (in.size() + kDigestMax - 1) / kDigestMax;
    if (parts == 0) parts = 1;
    if (parts > 255) parts = 255;  // 3140 / 24 < 255
    std::vector<Digest> out(parts);
    for (std::size_t i = 0; i < parts; ++i) {
        out[i].serial = serial;
        out[i].part = static_cast<std::uint8_t>(i);
        out[i].parts = static_cast<std::uint8_t>(parts);
        out[i].mask = mask;
        for (std::size_t k = i * kDigestMax; k < in.size() && k < (i + 1) * kDigestMax; ++k) out[i].entries.push_back(in[k]);
    }
    return out;
}

void DigestAssembler::Reset() noexcept {
    active_ = false;
    serial_ = 0;
    parts_ = 0;
    mask_ = 0;
    got_.clear();
    entries_.clear();
}
bool DigestAssembler::Add(const Digest& d) noexcept {
    if (!active_ || d.serial != serial_ || d.parts != parts_ || d.mask != mask_) {
        Reset();
        active_ = true;
        serial_ = d.serial;
        parts_ = d.parts;
        mask_ = d.mask;
        got_.assign(parts_, false);
    }
    if (got_[d.part]) return false;
    got_[d.part] = true;
    entries_.insert(entries_.end(), d.entries.begin(), d.entries.end());
    return true;
}
bool DigestAssembler::Complete() const noexcept {
    if (!active_) return false;
    for (bool g : got_)
        if (!g) return false;
    return true;
}

}  // namespace st::wwire
