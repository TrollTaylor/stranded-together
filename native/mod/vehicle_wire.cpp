// Shared real vehicles, the pure part. See vehicle_wire.hpp.
#include "vehicle_wire.hpp"

#include <cmath>

namespace st::vwire {
namespace {

constexpr std::uint8_t kMagic[4] = {'S', 'V', 'M', '1'};

// ---------------------------------------------------------------- little-endian writer / reader
class W {
public:
    W(std::uint8_t* out, std::size_t cap) noexcept : p_(out), cap_(cap) {}
    void u8(std::uint8_t v) noexcept { put(&v, 1); }
    void i8(std::int8_t v) noexcept { u8(static_cast<std::uint8_t>(v)); }
    void u16(std::uint16_t v) noexcept {
        const std::uint8_t b[2] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8)};
        put(b, 2);
    }
    void u32(std::uint32_t v) noexcept {
        const std::uint8_t b[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v >> 16),
                                   static_cast<std::uint8_t>(v >> 24)};
        put(b, 4);
    }
    void u64(std::uint64_t v) noexcept {
        u32(static_cast<std::uint32_t>(v));
        u32(static_cast<std::uint32_t>(v >> 32));
    }
    void f32(float v) noexcept {
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        u32(u);
    }
    void f64(double v) noexcept {
        std::uint64_t u;
        std::memcpy(&u, &v, 8);
        u64(u);
    }
    void bytes(const void* b, std::size_t n) noexcept { put(b, n); }
    void zeros(std::size_t n) noexcept {
        for (std::size_t i = 0; i < n; ++i) u8(0);
    }
    bool ok() const noexcept { return ok_; }
    std::size_t size() const noexcept { return n_; }

private:
    void put(const void* b, std::size_t n) noexcept {
        if (!ok_ || !p_ || n_ + n > cap_) {
            ok_ = false;
            return;
        }
        std::memcpy(p_ + n_, b, n);
        n_ += n;
    }
    std::uint8_t* p_;
    std::size_t cap_, n_ = 0;
    bool ok_ = true;
};

class R {
public:
    R(const std::uint8_t* in, std::size_t size) noexcept : p_(in), size_(size) {}
    std::uint8_t u8() noexcept {
        std::uint8_t v = 0;
        get(&v, 1);
        return v;
    }
    std::int8_t i8() noexcept { return static_cast<std::int8_t>(u8()); }
    std::uint16_t u16() noexcept {
        std::uint8_t b[2] = {};
        get(b, 2);
        return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
    }
    std::uint32_t u32() noexcept {
        std::uint8_t b[4] = {};
        get(b, 4);
        return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) | (static_cast<std::uint32_t>(b[2]) << 16) |
               (static_cast<std::uint32_t>(b[3]) << 24);
    }
    std::uint64_t u64() noexcept {
        const std::uint64_t lo = u32();
        const std::uint64_t hi = u32();
        return lo | (hi << 32);
    }
    float f32() noexcept {
        const std::uint32_t u = u32();
        float v;
        std::memcpy(&v, &u, 4);
        return v;
    }
    double f64() noexcept {
        const std::uint64_t u = u64();
        double v;
        std::memcpy(&v, &u, 8);
        return v;
    }
    void bytes(void* out, std::size_t n) noexcept { get(out, n); }
    // Reserved bytes: all must be zero.
    bool zeros(std::size_t n) noexcept {
        bool z = true;
        for (std::size_t i = 0; i < n; ++i)
            if (u8() != 0) z = false;
        return z;
    }
    bool ok() const noexcept { return ok_; }
    bool done() const noexcept { return ok_ && n_ == size_; }

private:
    void get(void* out, std::size_t n) noexcept {
        if (!ok_ || n_ + n > size_) {
            ok_ = false;
            std::memset(out, 0, n);
            return;
        }
        std::memcpy(out, p_ + n_, n);
        n_ += n;
    }
    const std::uint8_t* p_;
    std::size_t size_, n_ = 0;
    bool ok_ = true;
};

void Header(W& w, MsgType t) noexcept {
    w.bytes(kMagic, 4);
    w.u8(static_cast<std::uint8_t>(t));
    w.u8(kVersion);
    w.u16(0);
}
bool ReadHeader(R& r, MsgType want) noexcept {
    std::uint8_t m[4];
    r.bytes(m, 4);
    const std::uint8_t t = r.u8(), v = r.u8();
    const std::uint16_t res = r.u16();
    return r.ok() && std::memcmp(m, kMagic, 4) == 0 && t == static_cast<std::uint8_t>(want) && v == kVersion && res == 0;
}

bool Finite(double v, double lim) noexcept { return std::isfinite(v) && std::fabs(v) <= lim; }

void PutKey(W& w, const VKey& k) noexcept {
    w.u32(k.id);
    w.u8(k.kind);
    w.bytes(k.guid, 16);
    w.zeros(3);
}
bool GetKey(R& r, VKey& k) noexcept {
    k.id = r.u32();
    k.kind = r.u8();
    r.bytes(k.guid, 16);
    return r.zeros(3) && r.ok() && KeyValid(k);
}
// An all-zero key (no vehicle) for a RESULT that spawned nothing.
bool KeyZero(const VKey& k) noexcept { return k.id == 0 && k.kind == 0 && !k.HasGuid(); }

bool PosOk(const double (&p)[3]) noexcept { return Finite(p[0], 1e7) && Finite(p[1], 1e7) && Finite(p[2], 1e7); }
bool RotOk(const float (&m)[9]) noexcept {
    for (float v : m)
        if (!Finite(v, 1.5)) return false;
    return true;
}
bool AllZero(const double (&p)[3]) noexcept { return p[0] == 0 && p[1] == 0 && p[2] == 0; }
bool AllZero(const float (&m)[9]) noexcept {
    for (float v : m)
        if (v != 0) return false;
    return true;
}

void PutEntry(W& w, const SnapEntry& e) noexcept {
    w.u32(e.key.id);
    w.u8(e.key.kind);
    w.u8(e.state);
    w.u8(e.eflags);
    w.u8(0);
    w.bytes(e.key.guid, 16);
    for (double v : e.pos) w.f64(v);
    for (float v : e.rot) w.f32(v);
    w.u32(e.flags);
    w.bytes(e.flag_bytes, 4);
    w.u16(e.area);
    w.u16(e.rec2);
    w.f32(e.life);
    w.f32(e.battery);
    w.u32(e.rider);
}
bool GetEntry(R& r, SnapEntry& e) noexcept {
    e.key.id = r.u32();
    e.key.kind = r.u8();
    e.state = r.u8();
    e.eflags = r.u8();
    const bool res = r.zeros(1);
    r.bytes(e.key.guid, 16);
    for (double& v : e.pos) v = r.f64();
    for (float& v : e.rot) v = r.f32();
    e.flags = r.u32();
    r.bytes(e.flag_bytes, 4);
    e.area = r.u16();
    e.rec2 = r.u16();
    e.life = r.f32();
    e.battery = r.f32();
    e.rider = r.u32();
    if (!r.ok() || !res || !KeyValid(e.key) || (e.eflags & ~kEntAllBits) != 0) return false;
    if (e.Has(kEntXf)) {
        if (!PosOk(e.pos) || !RotOk(e.rot)) return false;
    } else if (!AllZero(e.pos) || !AllZero(e.rot)) {
        return false;  // no transform: none sent
    }
    if (!Finite(e.life, 1e7) || !Finite(e.battery, 1e3)) return false;
    if (e.rider > kNoRider) return false;
    if (((e.rider != kNoRider) != e.Has(kEntRidden))) return false;
    return true;
}

bool NameOk(const char* n, std::size_t cap) noexcept {
    std::size_t i = 0;
    for (; i < cap && n[i]; ++i)
        if (static_cast<unsigned char>(n[i]) < 0x20 || static_cast<unsigned char>(n[i]) > 0x7E) return false;
    for (; i < cap; ++i)
        if (n[i]) return false;  // NUL padded
    return true;
}

}  // namespace

// ---------------------------------------------------------------- names
const char* KindName(std::uint8_t kind) noexcept {
    switch (kind) {
        case 1: return "roadster";
        case 2: return "truck";
        case 3: return "trike";
        default: return "?";
    }
}
std::uint32_t ActorTypeOfKind(std::uint8_t kind) noexcept { return kind == 1 ? 0x20u : kind == 2 ? 0x10u : kind == 3 ? 0x0Fu : 0u; }
std::uint8_t KindOfActorId(std::uint32_t id) noexcept {
    const std::uint32_t t = id >> 13;
    return t == 0x20 ? 1 : t == 0x10 ? 2 : t == 0x0F ? 3 : 0;
}
bool KeyValid(const VKey& k) noexcept { return k.kind >= 1 && k.kind <= 3 && KindOfActorId(k.id) == k.kind; }

const char* MsgTypeName(std::uint8_t type) noexcept {
    switch (type) {
        case 1: return "SNAP_REQ";
        case 2: return "SNAP";
        case 3: return "ADD";
        case 4: return "REM";
        case 5: return "REQ";
        case 6: return "RESULT";
        case 7: return "NEED";
        default: return IsExternalType(type) ? "external" : "?";
    }
}
const char* ReqResultName(std::uint8_t r) noexcept {
    switch (r) {
        case 0: return "spawned";
        case 1: return "refused";
        case 2: return "failed";
        case 3: return "timeout";
        case 4: return "duplicate";
        default: return "?";
    }
}
const char* BindKindName(BindKind k) noexcept {
    switch (k) {
        case BindKind::bound: return "bound";
        case BindKind::bound_unverified: return "bound (unverified)";
        case BindKind::replace: return "re-place";
        case BindKind::remove_then_mirror: return "remove + mirror";
        case BindKind::remove_local: return "remove (host removed it)";
        case BindKind::mirror: return "mirror";
        case BindKind::wait: return "wait";
        case BindKind::gone: return "gone";
    }
    return "?";
}

// ---------------------------------------------------------------- messages
bool PeekType(const std::uint8_t* in, std::size_t size, std::uint8_t* type) noexcept {
    if (!in || size < kHeaderSize || size > kMaxMessage) return false;
    if (std::memcmp(in, kMagic, 4) != 0 || in[5] != kVersion || in[6] != 0 || in[7] != 0) return false;
    if (in[4] == 0) return false;
    if (type) *type = in[4];
    return true;
}
std::size_t WriteHeader(std::uint8_t type, std::uint8_t* out, std::size_t cap) noexcept {
    if (!IsExternalType(type)) return 0;
    W w(out, cap);
    Header(w, static_cast<MsgType>(type));
    return w.ok() ? w.size() : 0;
}

std::size_t EncodeSnapReq(const SnapReq& m, std::uint8_t* out, std::size_t cap) noexcept {
    W w(out, cap);
    Header(w, MsgType::snap_req);
    w.u32(m.req_id);
    w.u32(m.load_counter);
    w.u8(m.reason);
    w.zeros(3);
    return w.ok() ? w.size() : 0;
}
bool DecodeSnapReq(const std::uint8_t* in, std::size_t size, SnapReq& out) noexcept {
    R r(in, size);
    if (!ReadHeader(r, MsgType::snap_req)) return false;
    SnapReq m;
    m.req_id = r.u32();
    m.load_counter = r.u32();
    m.reason = r.u8();
    if (!r.zeros(3) || !r.done() || m.reason > 2) return false;
    out = m;
    return true;
}

std::size_t EncodeSnapChunk(const SnapChunk& m, std::uint8_t* out, std::size_t cap) noexcept {
    if (m.count == 0 || m.count > kSnapMaxChunks || m.index >= m.count || m.entries.size() > kSnapPerChunk || m.total > kSnapMaxEntries ||
        m.entries.size() > m.total)
        return 0;
    for (const SnapEntry& e : m.entries)
        if (!KeyValid(e.key)) return 0;
    W w(out, cap);
    Header(w, MsgType::snap);
    w.u32(m.snap_id);
    w.u32(m.req_id);
    w.u32(m.host_load_counter);
    w.u16(m.host_area);
    w.u8(m.index);
    w.u8(m.count);
    w.u16(m.total);
    w.u8(static_cast<std::uint8_t>(m.entries.size()));
    w.u8(0);
    for (const SnapEntry& e : m.entries) PutEntry(w, e);
    return w.ok() ? w.size() : 0;
}
bool DecodeSnapChunk(const std::uint8_t* in, std::size_t size, SnapChunk& out) noexcept {
    R r(in, size);
    if (!ReadHeader(r, MsgType::snap)) return false;
    SnapChunk m;
    m.snap_id = r.u32();
    m.req_id = r.u32();
    m.host_load_counter = r.u32();
    m.host_area = r.u16();
    m.index = r.u8();
    m.count = r.u8();
    m.total = r.u16();
    const std::uint8_t n = r.u8();
    if (!r.zeros(1) || !r.ok()) return false;
    if (m.count == 0 || m.count > kSnapMaxChunks || m.index >= m.count || n > kSnapPerChunk || m.total > kSnapMaxEntries || n > m.total) return false;
    if (size != kHeaderSize + kSnapChunkHeader + n * kSnapEntrySize) return false;
    m.entries.resize(n);
    for (SnapEntry& e : m.entries)
        if (!GetEntry(r, e)) return false;
    if (!r.done()) return false;
    out = std::move(m);
    return true;
}

std::size_t EncodeAdd(const Add& m, std::uint8_t* out, std::size_t cap) noexcept {
    if (!KeyValid(m.unit.key) || static_cast<std::uint8_t>(m.source) > 2 || m.p4 > 1) return 0;
    if ((m.rec0 & 0xF) != m.unit.key.kind) return 0;
    W w(out, cap);
    Header(w, MsgType::add);
    w.u32(m.token);
    w.u8(static_cast<std::uint8_t>(m.source));
    w.u8(m.has_entry ? 1 : 0);
    w.u16(m.rec0);
    w.u8(m.p4);
    w.zeros(3);
    PutEntry(w, m.unit);
    if (m.has_entry) w.bytes(m.entry, kEntrySize);
    else w.zeros(kEntrySize);
    return w.ok() ? w.size() : 0;
}
bool DecodeAdd(const std::uint8_t* in, std::size_t size, Add& out) noexcept {
    R r(in, size);
    if (!ReadHeader(r, MsgType::add)) return false;
    Add m;
    m.token = r.u32();
    const std::uint8_t src = r.u8(), has = r.u8();
    m.rec0 = r.u16();
    m.p4 = r.u8();
    if (!r.zeros(3) || src > 2 || has > 1 || m.p4 > 1) return false;
    m.source = static_cast<AddSource>(src);
    m.has_entry = has != 0;
    if (!GetEntry(r, m.unit)) return false;
    r.bytes(m.entry, kEntrySize);
    if (!r.done()) return false;
    if ((m.rec0 & 0xF) != m.unit.key.kind) return false;
    if (m.source == AddSource::guest_request ? m.token == 0 : m.token != 0) return false;
    if (!m.has_entry) {
        for (std::uint8_t b : m.entry)
            if (b) return false;
    } else {
        // the entry's own id (+0x2C) is the unit's
        std::uint32_t eid = 0;
        std::memcpy(&eid, m.entry + 0x2C, 4);
        if (eid != m.unit.key.id) return false;
    }
    out = m;
    return true;
}

std::size_t EncodeRem(const Rem& m, std::uint8_t* out, std::size_t cap) noexcept {
    if (!KeyValid(m.key) || static_cast<std::uint8_t>(m.reason) > 4 || m.from > 1) return 0;
    W w(out, cap);
    Header(w, MsgType::rem);
    w.u8(static_cast<std::uint8_t>(m.reason));
    w.u8(m.from);
    w.u8(m.has_pos ? 1 : 0);
    w.u8(0);
    PutKey(w, m.key);
    for (int i = 0; i < 3; ++i) w.f64(m.has_pos ? m.pos[i] : 0.0);
    w.u32(m.world);
    return w.ok() ? w.size() : 0;
}
bool DecodeRem(const std::uint8_t* in, std::size_t size, Rem& out) noexcept {
    R r(in, size);
    if (!ReadHeader(r, MsgType::rem)) return false;
    Rem m;
    const std::uint8_t reason = r.u8();
    m.from = r.u8();
    const std::uint8_t has = r.u8();
    if (!r.zeros(1) || reason > 4 || m.from > 1 || has > 1) return false;
    m.reason = static_cast<RemReason>(reason);
    m.has_pos = has != 0;
    if (!GetKey(r, m.key)) return false;
    for (double& v : m.pos) v = r.f64();
    m.world = r.u32();
    if (!r.done()) return false;
    if (m.has_pos ? !PosOk(m.pos) : !AllZero(m.pos)) return false;
    out = m;
    return true;
}

std::size_t EncodeReq(const Req& m, std::uint8_t* out, std::size_t cap) noexcept {
    if (m.kind < 1 || m.kind > 3 || m.garage_slot < -1 || m.garage_slot > 8 || static_cast<std::uint8_t>(m.reason) > 3 || m.token == 0) return 0;
    // the name up to its NUL (printable ASCII), NUL padded
    char name[kNameChars] = {};
    for (std::size_t i = 0; i < kNameChars && m.name[i]; ++i) name[i] = m.name[i];
    // a real transform: not the origin, rotation rows of a rotation (an unreadable request is never sent as zeros)
    if (!PosOk(m.pos) || AllZero(m.pos) || !RotOk(m.rot) || !RotRowsOk(m.rot) || !NameOk(name, kNameChars)) return 0;
    W w(out, cap);
    Header(w, MsgType::req);
    w.u32(m.token);
    w.u8(m.kind);
    w.i8(m.garage_slot);
    w.u8(static_cast<std::uint8_t>(m.reason));
    w.u8(0);
    w.u32(m.req_flags);
    w.u16(m.rec2);
    w.u16(0);
    w.u64(m.rec14);
    for (double v : m.pos) w.f64(v);
    for (float v : m.rot) w.f32(v);
    w.bytes(name, kNameChars);
    return w.ok() ? w.size() : 0;
}
bool DecodeReq(const std::uint8_t* in, std::size_t size, Req& out) noexcept {
    R r(in, size);
    if (!ReadHeader(r, MsgType::req)) return false;
    Req m;
    m.token = r.u32();
    m.kind = r.u8();
    m.garage_slot = r.i8();
    const std::uint8_t reason = r.u8();
    if (!r.zeros(1)) return false;
    m.req_flags = r.u32();
    m.rec2 = r.u16();
    if (!r.zeros(2)) return false;
    m.rec14 = r.u64();
    for (double& v : m.pos) v = r.f64();
    for (float& v : m.rot) v = r.f32();
    r.bytes(m.name, kNameChars);
    m.name[kNameChars] = 0;
    if (!r.done() || m.token == 0 || m.kind < 1 || m.kind > 3 || m.garage_slot < -1 || m.garage_slot > 8 || reason > 3) return false;
    m.reason = static_cast<ReqReason>(reason);
    if (!PosOk(m.pos) || AllZero(m.pos) || !RotOk(m.rot) || !RotRowsOk(m.rot) || !NameOk(m.name, kNameChars)) return false;
    out = m;
    return true;
}

std::size_t EncodeResult(const Result& m, std::uint8_t* out, std::size_t cap) noexcept {
    if (m.token == 0 || static_cast<std::uint8_t>(m.result) > 4 || m.path > 2) return 0;
    if (m.result == ReqResult::spawned ? !KeyValid(m.key) : !KeyZero(m.key)) return 0;
    W w(out, cap);
    Header(w, MsgType::req_result);
    w.u32(m.token);
    w.u8(static_cast<std::uint8_t>(m.result));
    w.u8(m.path);
    w.u16(0);
    if (m.result == ReqResult::spawned) PutKey(w, m.key);
    else w.zeros(24);
    return w.ok() ? w.size() : 0;
}
bool DecodeResult(const std::uint8_t* in, std::size_t size, Result& out) noexcept {
    R r(in, size);
    if (!ReadHeader(r, MsgType::req_result)) return false;
    Result m;
    m.token = r.u32();
    const std::uint8_t res = r.u8();
    m.path = r.u8();
    if (!r.zeros(2) || m.token == 0 || res > 4 || m.path > 2) return false;
    m.result = static_cast<ReqResult>(res);
    if (m.result == ReqResult::spawned) {
        if (!GetKey(r, m.key)) return false;
    } else if (!r.zeros(24)) {
        return false;
    }
    if (!r.done()) return false;
    out = m;
    return true;
}

std::size_t EncodeNeed(const Need& m, std::uint8_t* out, std::size_t cap) noexcept {
    if (!KeyValid(m.key) || m.reason > 1) return 0;
    W w(out, cap);
    Header(w, MsgType::need);
    PutKey(w, m.key);
    w.u8(m.reason);
    w.zeros(3);
    return w.ok() ? w.size() : 0;
}
bool DecodeNeed(const std::uint8_t* in, std::size_t size, Need& out) noexcept {
    R r(in, size);
    if (!ReadHeader(r, MsgType::need)) return false;
    Need m;
    if (!GetKey(r, m.key)) return false;
    m.reason = r.u8();
    if (!r.zeros(3) || !r.done() || m.reason > 1) return false;
    out = m;
    return true;
}

std::vector<std::vector<std::uint8_t>> EncodeSnapshot(std::uint32_t snap_id, std::uint32_t req_id, std::uint32_t load_counter, std::uint16_t area,
                                                      const std::vector<SnapEntry>& entries) {
    std::vector<std::vector<std::uint8_t>> out;
    std::vector<SnapEntry> valid;
    for (const SnapEntry& e : entries)
        if (KeyValid(e.key) && valid.size() < kSnapMaxEntries) valid.push_back(e);
    const std::size_t count = valid.empty() ? 1 : (valid.size() + kSnapPerChunk - 1) / kSnapPerChunk;
    if (count > kSnapMaxChunks) return out;
    for (std::size_t c = 0; c < count; ++c) {
        SnapChunk m;
        m.snap_id = snap_id;
        m.req_id = req_id;
        m.host_load_counter = load_counter;
        m.host_area = area;
        m.index = static_cast<std::uint8_t>(c);
        m.count = static_cast<std::uint8_t>(count);
        m.total = static_cast<std::uint16_t>(valid.size());
        for (std::size_t i = c * kSnapPerChunk; i < valid.size() && i < (c + 1) * kSnapPerChunk; ++i) m.entries.push_back(valid[i]);
        std::vector<std::uint8_t> buf(kMaxMessage);
        const std::size_t n = EncodeSnapChunk(m, buf.data(), buf.size());
        if (!n) return {};
        buf.resize(n);
        out.push_back(std::move(buf));
    }
    return out;
}

void SnapAssembler::Reset() noexcept {
    active_ = complete_ = false;
    snap_id_ = req_id_ = load_counter_ = 0;
    area_ = total_ = 0;
    count_ = 0;
    have_ = 0;
    entries_.clear();
}

SnapAssembler::Step SnapAssembler::Add(const SnapChunk& c) noexcept {
    if (c.count == 0 || c.count > kSnapMaxChunks || c.index >= c.count) return Step::rejected;
    if (!active_ || c.snap_id != snap_id_ || complete_) {
        // a new snapshot (or the next one after a complete one)
        if (active_ && c.snap_id == snap_id_ && complete_) return Step::rejected;  // a duplicate of a complete snapshot's chunk
        Reset();
        active_ = true;
        snap_id_ = c.snap_id;
        req_id_ = c.req_id;
        load_counter_ = c.host_load_counter;
        area_ = c.host_area;
        total_ = c.total;
        count_ = c.count;
    } else if (c.count != count_ || c.total != total_ || c.req_id != req_id_ || c.host_load_counter != load_counter_ || c.host_area != area_) {
        Reset();
        return Step::rejected;
    }
    const std::uint32_t bit = 1u << c.index;
    if (have_ & bit) {
        Reset();
        return Step::rejected;
    }
    if (entries_.size() + c.entries.size() > total_) {
        Reset();
        return Step::rejected;
    }
    have_ |= bit;
    entries_.insert(entries_.end(), c.entries.begin(), c.entries.end());
    const std::uint32_t all = count_ >= 32 ? 0xFFFFFFFFu : ((1u << count_) - 1u);
    if (have_ == all) {
        if (entries_.size() != total_) {
            Reset();
            return Step::rejected;
        }
        complete_ = true;
        return Step::complete;
    }
    return Step::partial;
}

// ---------------------------------------------------------------- request flags
std::uint32_t RequestFlagsFromRecord(std::uint16_t rec0, std::uint8_t p4, std::uint8_t p6) noexcept {
    const std::uint32_t u = rec0;
    const std::uint32_t hi = ((((u >> 13) & 1u) | (static_cast<std::uint32_t>(p6 & 1u) << 1)) << 1 | ((u >> 12) & 1u)) << 12;
    const std::uint32_t lo = (((u >> 2) & 0xCu) | (u & 0xFC0u)) >> 2;
    return hi | (static_cast<std::uint32_t>(p4 & 1u) << 2) | lo;
}
bool RecordFromRequestFlags(std::uint32_t flags, std::uint8_t pool_type, std::uint16_t* rec0, std::uint8_t* p4, std::uint8_t* p6) noexcept {
    if (pool_type < 1 || pool_type > 3) return false;
    if (flags & ~0x73F7u) return false;  // bits 3, 10, 11, 15+: never from 0x142F830A0
    const std::uint32_t r = pool_type | ((flags & 3u) << 4) | (((flags >> 4) & 0x3Fu) << 6) | (((flags >> 12) & 3u) << 12);
    if (rec0) *rec0 = static_cast<std::uint16_t>(r);
    if (p4) *p4 = static_cast<std::uint8_t>((flags >> 2) & 1u);
    if (p6) *p6 = static_cast<std::uint8_t>((flags >> 14) & 1u);
    return true;
}

Gate DecideGate(const GateIn& in) noexcept {
    if (in.flags & kReqOnline) return Gate::drop_online;
    if (in.mod_call || in.host) return Gate::pass;
    if (in.load_window || !in.host_real) return Gate::pass;  // the host shares no real vehicles (yet): the game's own spawns
    return in.can_forward ? Gate::forward : Gate::drop;
}

// ---------------------------------------------------------------- binding
double Dist(const double (&a)[3], const double (&b)[3]) noexcept {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

BindPlan PlanBinding(const std::vector<SnapEntry>& host, const std::vector<LocalUnit>& local, const BindParams& p) {
    BindPlan plan;
    std::vector<char> claimed(local.size(), 0);
    auto find = [&](auto&& pred) -> int {
        for (std::size_t i = 0; i < local.size(); ++i)
            if (!claimed[i] && pred(local[i])) return static_cast<int>(i);
        return -1;
    };
    auto dist_of = [&](const SnapEntry& h, const LocalUnit& u) -> double {
        return h.Has(kEntXf) && u.has_pos ? Dist(h.pos, u.pos) : -1.0;
    };
    // What to do with the host's vehicle when the guest has nothing for it.
    auto absent = [&](const SnapEntry& h) -> BindKind {
        if (h.Has(kEntRemoved)) return BindKind::gone;
        if (!p.mirrors_allowed || !h.Has(kEntXf) || h.area != p.current_area) return BindKind::wait;
        return BindKind::mirror;
    };
    // The guest's unit `u` (same id) is another vehicle than the host's `h`.
    auto conflict = [&](const SnapEntry& h, const LocalUnit& u) -> BindKind {
        if (h.Has(kEntRemoved)) return u.removed ? BindKind::gone : (u.ridden ? BindKind::wait : BindKind::remove_local);
        if (u.ridden) return BindKind::wait;
        // the same vehicle (kind, GUID or none on both) whose actor is not spawned here (out of streaming range): its unit's
        // own place is old, so it is not another vehicle; bound, and re-placed once its actor is back (M2.5: streaming is
        // not removal)
        if (!u.has_entity && !u.removed && u.kind == h.key.kind && std::memcmp(u.guid, h.key.guid, 16) == 0) return BindKind::bound_unverified;
        if (p.allow_replace && !u.HasGuid() && !h.key.HasGuid() && u.has_entity && h.Has(kEntXf) && u.kind == h.key.kind) return BindKind::replace;
        return BindKind::remove_then_mirror;
    };
    // The guest's unit `u` is the host's `h`: bound, or removed when the host removed it.
    auto same = [&](const SnapEntry& h, const LocalUnit& u, double d) -> BindKind {
        if (h.Has(kEntRemoved) && !u.removed) return u.ridden ? BindKind::wait : BindKind::remove_local;
        if (h.Has(kEntRemoved)) return BindKind::gone;
        if (u.removed) return BindKind::remove_then_mirror;  // removed here, alive on the host: mirrored again (the removal is done)
        if (d < 0) return BindKind::bound_unverified;
        return BindKind::bound;
    };

    plan.steps.resize(host.size());
    // Pass 1: earlier id mappings and GUID matches claim their units first (a GUID is the same vehicle in both games).
    std::vector<char> done(host.size(), 0);
    for (std::size_t hi = 0; hi < host.size(); ++hi) {
        const SnapEntry& h = host[hi];
        BindStep& s = plan.steps[hi];
        s.host = static_cast<int>(hi);
        int li = find([&](const LocalUnit& u) { return u.bound_host_id == h.key.id && u.kind == h.key.kind && u.id != h.key.id; });
        // the mod's own mirror of this host vehicle (a mirror never carries the host's placement GUID: it is still that one)
        if (li < 0) li = find([&](const LocalUnit& u) { return u.mirrored_for == h.key.id && u.kind == h.key.kind && !u.removed; });
        bool by_guid = false;
        if (li < 0 && h.key.HasGuid()) {
            li = find([&](const LocalUnit& u) { return u.kind == h.key.kind && std::memcmp(u.guid, h.key.guid, 16) == 0; });
            by_guid = li >= 0;
        }
        if (li < 0) continue;
        claimed[li] = 1;
        done[hi] = 1;
        s.local = li;
        s.dist = dist_of(h, local[li]);
        if (s.dist > p.radius_m && !h.Has(kEntRemoved)) {
            // the same vehicle (GUID or mapping), elsewhere: put back where the host has it, when it can be (not while
            // somebody rides it on either side: a driven vehicle moves, the drive module follows it)
            s.kind = local[li].ridden || h.Has(kEntRidden) ? BindKind::bound_unverified
                     : local[li].has_entity && p.allow_replace ? BindKind::replace
                                                               : BindKind::bound_unverified;
        } else {
            s.kind = same(h, local[li], s.dist);
            if (by_guid && s.kind == BindKind::bound_unverified) s.kind = BindKind::bound;  // the GUID is the proof
        }
    }
    // Pass 2: the same id.
    for (std::size_t hi = 0; hi < host.size(); ++hi) {
        if (done[hi]) continue;
        const SnapEntry& h = host[hi];
        BindStep& s = plan.steps[hi];
        const int li = find([&](const LocalUnit& u) { return u.id == h.key.id; });
        if (li < 0) {
            s.kind = absent(h);
            continue;
        }
        const LocalUnit& u = local[li];
        claimed[li] = 1;
        s.local = li;
        s.dist = dist_of(h, u);
        const bool guid_differs = std::memcmp(u.guid, h.key.guid, 16) != 0;  // one has a GUID the other has not, or two GUIDs
        if (u.kind != h.key.kind || guid_differs) s.kind = conflict(h, u);
        else if (s.dist > p.radius_m && h.Has(kEntRidden) && !h.Has(kEntRemoved)) s.kind = BindKind::bound_unverified;  // driven on the host
        else if (s.dist > p.radius_m) s.kind = conflict(h, u);
        else s.kind = same(h, u, s.dist);
    }
    for (const BindStep& s : plan.steps) {
        switch (s.kind) {
            case BindKind::bound: ++plan.bound; break;
            case BindKind::bound_unverified: ++plan.unverified; break;
            case BindKind::replace: ++plan.replace; break;
            case BindKind::remove_then_mirror: ++plan.remove_mirror; break;
            case BindKind::remove_local: ++plan.remove_local; break;
            case BindKind::mirror: ++plan.mirror; break;
            case BindKind::wait: ++plan.wait; break;
            case BindKind::gone: ++plan.gone; break;
        }
    }
    // Pass 3: the guest's units nobody claimed.
    for (std::size_t i = 0; i < local.size(); ++i) {
        if (claimed[i]) continue;
        ExtraStep e;
        e.local = static_cast<int>(i);
        if (local[i].removed) e.kind = ExtraKind::already_removed;
        else if (local[i].pending_mirror) e.kind = ExtraKind::keep_pending;
        else if (local[i].ridden) e.kind = ExtraKind::keep_ridden;
        else e.kind = ExtraKind::remove;
        if (e.kind == ExtraKind::remove) ++plan.extra_remove;
        else if (e.kind != ExtraKind::already_removed) ++plan.extra_kept;
        plan.extras.push_back(e);
    }
    return plan;
}

// ---------------------------------------------------------------- host rules
bool HostRemovalAllowed(const HostUnitView& unit, const VKey& key, bool has_pos, const double (&pos)[3], double radius_m) noexcept {
    if (!KeyValid(key) || !unit.in_use || unit.removed) return false;
    if (unit.id != key.id || unit.kind != key.kind) return false;
    if (std::memcmp(unit.guid, key.guid, 16) != 0) return false;
    if (unit.has_pos && (!has_pos || Dist(unit.pos, pos) > radius_m)) return false;
    return true;
}

bool SharedActive(const ActiveInfo& a, double now_ms) noexcept {
    return a.partner_riding || now_ms - a.last_near_ms < kActiveNearMs || now_ms - a.last_drove_ms < kActiveDroveMs;
}

bool RateLimit::Allow(double now_ms) noexcept {
    if (burst_ == 0) return false;
    // the oldest of the last `burst_` must be older than the window
    unsigned oldest = next_;
    double t = times_[oldest % burst_];
    if (now_ms - t < window_) return false;
    times_[next_ % burst_] = now_ms;
    next_ = (next_ + 1) % burst_;
    return true;
}

// ================================================================ stage 2: remote driving and ownership
namespace {
bool QuatOk(const float (&q)[4]) noexcept {
    for (float v : q)
        if (!Finite(v, 1.5)) return false;
    const double n = static_cast<double>(q[0]) * q[0] + static_cast<double>(q[1]) * q[1] + static_cast<double>(q[2]) * q[2] +
                     static_cast<double>(q[3]) * q[3];
    return n > 0.81 && n < 1.21;  // a unit quaternion within 10 %
}
bool Vec3Ok(const float (&v)[3], double lim) noexcept { return Finite(v[0], lim) && Finite(v[1], lim) && Finite(v[2], lim); }
}  // namespace

bool InputsValid(const std::uint8_t (&in)[kDriveInputBytes]) noexcept {
    for (int i = 0; i < 11; ++i) {
        float f;
        std::memcpy(&f, in + 4 * i, 4);
        if (!Finite(f, 4.0)) return false;
    }
    for (int i = 44; i < 47; ++i)
        if (in[i] > 1) return false;
    return in[47] == 0;
}

std::size_t EncodeDrive(const DriveFrame& f, std::uint8_t* out, std::size_t cap) noexcept {
    W w(out, cap);
    w.bytes(kDriveMagic, 4);
    w.u8(kDriveVersion);
    w.u8(static_cast<std::uint8_t>(f.type));
    w.u8(f.flags);
    w.u8(f.ride_state);
    w.u32(f.seq);
    w.u32(f.sender_ms);
    w.u32(f.epoch);
    PutKey(w, f.key);
    for (double v : f.pos) w.f64(v);
    for (float v : f.quat) w.f32(v);
    for (float v : f.vel) w.f32(v);
    for (float v : f.avel) w.f32(v);
    w.f32(f.speed_kmh);
    w.f32(f.life);
    w.f32(f.life_max);
    w.f32(f.battery);
    if (f.type == DriveType::drive) w.bytes(f.input, kDriveInputBytes);
    if (!w.ok()) return 0;
    // the encoder only writes what the decoder accepts
    DriveFrame back;
    return DecodeDrive(out, w.size(), back) ? w.size() : 0;
}

bool LooksLikeDrive(const std::uint8_t* in, std::size_t size) noexcept {
    return in && (size == kDriveParkSize || size == kDriveFrameSize) && std::memcmp(in, kDriveMagic, 4) == 0;
}

bool DecodeDrive(const std::uint8_t* in, std::size_t size, DriveFrame& out) noexcept {
    if (!LooksLikeDrive(in, size)) return false;
    R r(in, size);
    std::uint8_t m[4];
    r.bytes(m, 4);
    DriveFrame f;
    const std::uint8_t ver = r.u8(), type = r.u8();
    f.flags = r.u8();
    f.ride_state = r.u8();
    f.seq = r.u32();
    f.sender_ms = r.u32();
    f.epoch = r.u32();
    if (!GetKey(r, f.key)) return false;
    for (double& v : f.pos) v = r.f64();
    for (float& v : f.quat) v = r.f32();
    for (float& v : f.vel) v = r.f32();
    for (float& v : f.avel) v = r.f32();
    f.speed_kmh = r.f32();
    f.life = r.f32();
    f.life_max = r.f32();
    f.battery = r.f32();
    if (ver != kDriveVersion || (type != 1 && type != 2)) return false;
    f.type = static_cast<DriveType>(type);
    if ((f.type == DriveType::drive) != (size == kDriveFrameSize)) return false;
    if (f.type == DriveType::drive) r.bytes(f.input, kDriveInputBytes);
    if (!r.done()) return false;
    if ((f.flags & ~kDrvAllFlags) != 0) return false;
    if ((f.type == DriveType::drive) != f.Has(kDrvSeated)) return false;  // seated exactly on DRIVE
    if (f.ride_state > 15) return false;
    if (!PosOk(f.pos) || !QuatOk(f.quat) || !Vec3Ok(f.vel, 200.0) || !Vec3Ok(f.avel, 50.0) || !Finite(f.speed_kmh, 1000.0)) return false;
    if (!Finite(f.life_max, 1e6) || !Finite(f.life, 1e6) || f.life < 0 || f.life_max < 0 || f.life > f.life_max) return false;
    if (f.Has(kDrvBattery)) {
        if (!Finite(f.battery, 1e6) || f.battery < 0) return false;
    } else if (f.battery != 0) {
        return false;
    }
    if (f.type == DriveType::drive && !InputsValid(f.input)) return false;
    out = f;
    return true;
}

const char* OwnTypeName(std::uint8_t t) noexcept {
    switch (t) {
        case 16: return "CLAIM";
        case 17: return "GRANT";
        case 18: return "DENY";
        case 19: return "RELEASE";
        default: return "?";
    }
}

std::size_t EncodeOwn(const Own& m, std::uint8_t* out, std::size_t cap) noexcept {
    W w(out, cap);
    Header(w, static_cast<MsgType>(m.type));
    PutKey(w, m.key);
    w.u8(m.owner);
    w.u8(m.reason);
    w.u8(m.has_xf ? 1 : 0);
    w.u8(0);
    w.u32(m.epoch);
    w.u32(m.sender_ms);
    for (double v : m.pos) w.f64(m.has_xf ? v : 0.0);
    for (int i = 0; i < 4; ++i) w.f32(m.has_xf ? m.quat[i] : 0.0f);
    for (float v : m.vel) w.f32(m.has_xf ? v : 0.0f);
    w.u32(m.world);
    if (!w.ok()) return 0;
    Own back;
    return DecodeOwn(out, w.size(), back) ? w.size() : 0;
}

bool DecodeOwn(const std::uint8_t* in, std::size_t size, Own& out) noexcept {
    std::uint8_t type = 0;
    if (!PeekType(in, size, &type) || type < 16 || type > 19 || size != kHeaderSize + kOwnBodySize) return false;
    R r(in, size);
    if (!ReadHeader(r, static_cast<MsgType>(type))) return false;
    Own m;
    m.type = static_cast<OwnType>(type);
    if (!GetKey(r, m.key)) return false;
    m.owner = r.u8();
    m.reason = r.u8();
    const std::uint8_t has = r.u8();
    const bool res = r.zeros(1);
    m.epoch = r.u32();
    m.sender_ms = r.u32();
    for (double& v : m.pos) v = r.f64();
    for (float& v : m.quat) v = r.f32();
    for (float& v : m.vel) v = r.f32();
    m.world = r.u32();
    if (!r.done() || !res || has > 1 || m.owner > 1) return false;
    m.has_xf = has == 1;
    switch (m.type) {
        case OwnType::claim:
            if (m.owner != kOwnerGuest || m.reason > kClaimPush || m.has_xf) return false;
            break;
        case OwnType::grant:
            if (m.reason > kGrantTimeout || m.has_xf) return false;
            break;
        case OwnType::deny:
            if (m.reason > kDenyNotThere || m.has_xf || m.owner != kOwnerHost) return false;
            break;
        case OwnType::release:
            if (m.reason > kReleaseAtRest) return false;
            break;
    }
    if (m.has_xf) {
        if (!PosOk(m.pos) || !QuatOk(m.quat) || !Vec3Ok(m.vel, 200.0)) return false;
    } else {
        const float zq[4] = {0, 0, 0, 0};
        if (!AllZero(m.pos) || std::memcmp(m.quat, zq, sizeof(zq)) != 0 || m.vel[0] != 0 || m.vel[1] != 0 || m.vel[2] != 0) return false;
    }
    out = m;
    return true;
}

OwnStep HostOnClaim(const OwnState& s, std::uint8_t claim_reason, bool host_sam_on_it, double host_drove_ms_ago) noexcept {
    OwnStep st;
    st.next = s;
    st.send = true;
    if (host_sam_on_it || (host_drove_ms_ago >= 0 && host_drove_ms_ago < 300)) {
        // the host wins every tie: it stays (or becomes) the host's
        if (s.owner != kOwnerHost) {
            st.next.owner = kOwnerHost;
            ++st.next.epoch;
        }
        st.type = OwnType::deny;
        st.reason = host_sam_on_it ? kDenyHostDrives : kDenyHostFirst;
        return st;
    }
    (void)claim_reason;
    if (s.owner == kOwnerGuest) {
        st.type = OwnType::grant;  // again (a repeated claim): the same epoch
        st.reason = kGrantClaim;
        return st;
    }
    st.next.owner = kOwnerGuest;
    ++st.next.epoch;
    st.type = OwnType::grant;
    st.reason = kGrantClaim;
    return st;
}

OwnStep HostOnHostTakes(const OwnState& s) noexcept {
    OwnStep st;
    st.next = s;
    if (s.owner == kOwnerHost) return st;
    st.next.owner = kOwnerHost;
    ++st.next.epoch;
    st.send = true;
    st.type = OwnType::grant;
    st.reason = kGrantHostTook;
    return st;
}

OwnStep HostOnRelease(const OwnState& s, std::uint8_t release_reason, std::uint32_t epoch) noexcept {
    OwnStep st;
    st.next = s;
    if (s.owner != kOwnerGuest || epoch != s.epoch || release_reason != kReleaseAtRest) return st;  // an old epoch, or not at rest yet
    st.next.owner = kOwnerHost;
    ++st.next.epoch;
    st.send = true;
    st.type = OwnType::grant;
    st.reason = kGrantHandback;
    return st;
}

OwnStep HostOnSilence(const OwnState& s, double silent_ms) noexcept {
    OwnStep st;
    st.next = s;
    if (s.owner != kOwnerGuest || silent_ms < 10000.0) return st;
    st.next.owner = kOwnerHost;
    ++st.next.epoch;
    st.send = true;
    st.type = OwnType::grant;
    st.reason = kGrantTimeout;
    return st;
}

OwnState GuestOnAnswer(const OwnState& s, const Own& m, bool* changed) noexcept {
    if (changed) *changed = false;
    if ((m.type != OwnType::grant && m.type != OwnType::deny) || m.epoch < s.epoch) return s;
    OwnState n;
    n.owner = m.type == OwnType::deny ? kOwnerHost : m.owner;
    n.epoch = m.epoch;
    if (changed) *changed = n.owner != s.owner || n.epoch != s.epoch;
    return n;
}

OwnState GuestOnHostFrame(const OwnState& s, std::uint32_t frame_epoch, bool* changed) noexcept {
    if (changed) *changed = false;
    if (frame_epoch <= s.epoch) return s;
    OwnState n;
    n.owner = kOwnerHost;
    n.epoch = frame_epoch;
    if (changed) *changed = true;
    return n;
}

bool HostAcceptsGuestFrame(const OwnState& s, std::uint32_t frame_epoch) noexcept { return s.owner == kOwnerGuest && frame_epoch == s.epoch; }

bool ClaimNear(const Footprint& vehicle, bool have_partner, const double (&partner)[3], std::uint8_t claim_reason) noexcept {
    if (!have_partner) return false;
    for (double v : partner)
        if (!std::isfinite(v)) return false;
    return PointInFootprint(vehicle, partner, claim_reason == kClaimPush ? kPushNearM : kClaimNearM);
}

bool FrameInReach(const double (&frame)[3], bool have_unit, const double (&unit)[3], bool have_partner, const double (&partner)[3]) noexcept {
    if (!PosOk(frame)) return false;
    if (have_unit && PosOk(unit) && Dist(frame, unit) <= kFrameReachM) return true;
    if (have_partner && PosOk(partner) && Dist(frame, partner) <= kFrameReachM) return true;
    return false;
}

double ParkedTeleportGapMs(unsigned failed) noexcept {
    if (failed < 2) return 2000.0;
    const unsigned e = failed - 1 > 5 ? 5 : failed - 1;  // 2 -> 4 s, 3 -> 8 s, ... 6+ -> 64 s, capped at 60 s
    const double gap = 2000.0 * static_cast<double>(1u << e);
    return gap > 60000.0 ? 60000.0 : gap;
}

bool RotRowsOk(const float (&r)[9]) noexcept {
    for (float v : r)
        if (!std::isfinite(v)) return false;
    for (int a = 0; a < 3; ++a) {
        const double n = static_cast<double>(r[a * 3]) * r[a * 3] + static_cast<double>(r[a * 3 + 1]) * r[a * 3 + 1] + static_cast<double>(r[a * 3 + 2]) * r[a * 3 + 2];
        if (n < 0.81 || n > 1.21) return false;
        for (int b = a + 1; b < 3; ++b) {
            const double d = static_cast<double>(r[a * 3]) * r[b * 3] + static_cast<double>(r[a * 3 + 1]) * r[b * 3 + 1] + static_cast<double>(r[a * 3 + 2]) * r[b * 3 + 2];
            if (std::fabs(d) > 0.1) return false;
        }
    }
    return true;
}

bool RestDetector::Update(double speed_mps, double now_ms) noexcept {
    if (!std::isfinite(speed_mps) || speed_mps >= 0.2) {
        still_since_ = -1;
        at_rest_ = false;
        return false;
    }
    if (still_since_ < 0) still_since_ = now_ms;
    at_rest_ = now_ms - still_since_ >= 1000.0;
    return at_rest_;
}

const char* BandName(Band b) noexcept {
    switch (b) {
        case Band::none: return "none";
        case Band::impulse: return "impulse";
        case Band::interp: return "interpolated teleport";
        case Band::teleport: return "teleport";
    }
    return "?";
}

Band ChooseBand(double err_m, double err_deg, const BandLimits& l) noexcept {
    if (!std::isfinite(err_m) || !std::isfinite(err_deg)) return Band::teleport;
    if (err_m < l.ignore_m && err_deg < l.ignore_deg) return Band::none;
    if (err_m < l.impulse_m && err_deg < l.impulse_deg) return Band::impulse;
    if (err_m <= l.interp_m) return Band::interp;
    return Band::teleport;
}

void NudgeVelocity(const double (&err)[3], const float (&measured)[3], double tau_s, double max_mps, float (&out)[3]) noexcept {
    if (!(tau_s > 0)) tau_s = 0.5;
    double add[3] = {err[0] / tau_s, err[1] / tau_s, err[2] / tau_s};
    const double n = std::sqrt(add[0] * add[0] + add[1] * add[1] + add[2] * add[2]);
    if (n > max_mps && n > 0)
        for (double& a : add) a *= max_mps / n;
    for (int i = 0; i < 3; ++i) {
        const double m = std::isfinite(measured[i]) ? measured[i] : 0.0;
        out[i] = static_cast<float>(m + add[i]);
    }
}

// ---------------------------------------------------------------- the driven follower (v0.21)
namespace {
bool VelOk(const float (&v)[3]) noexcept {
    for (float c : v)
        if (!std::isfinite(c) || std::fabs(c) > 100.0f) return false;
    return true;
}
double Len3(const double (&v)[3]) noexcept { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
void ClampLen(double (&v)[3], double max) noexcept {
    const double n = Len3(v);
    if (n > max && n > 0)
        for (double& c : v) c *= max / n;
}
}  // namespace

void HermitePos(const double (&pa)[3], const float (&va)[3], const double (&pb)[3], const float (&vb)[3], double h_s, double f,
                double (&out)[3]) noexcept {
    if (!(f >= 0)) f = 0;
    if (f > 1) f = 1;
    const bool cubic = VelOk(va) && VelOk(vb) && h_s > 0 && h_s < 1.0;
    if (!cubic) {
        for (int i = 0; i < 3; ++i) out[i] = pa[i] + (pb[i] - pa[i]) * f;
        return;
    }
    const double f2 = f * f, f3 = f2 * f;
    const double h00 = 2 * f3 - 3 * f2 + 1, h10 = f3 - 2 * f2 + f, h01 = -2 * f3 + 3 * f2, h11 = f3 - f2;
    for (int i = 0; i < 3; ++i) out[i] = h00 * pa[i] + h10 * h_s * va[i] + h01 * pb[i] + h11 * h_s * vb[i];
}

void Extrapolate(const double (&p)[3], const float (&vel)[3], double ahead_ms, double (&out)[3]) noexcept {
    double t = ahead_ms;
    if (!(t > 0)) t = 0;
    if (t > kExtrapolateMaxMs) t = kExtrapolateMaxMs;
    const bool ok = VelOk(vel);
    for (int i = 0; i < 3; ++i) out[i] = p[i] + (ok ? vel[i] * t / 1000.0 : 0.0);
}

double WrapPi(double a) noexcept {
    if (!std::isfinite(a)) return 0;
    while (a > 3.141592653589793) a -= 6.283185307179586;
    while (a < -3.141592653589793) a += 6.283185307179586;
    return a;
}
double YawOfRows(const float (&rows)[9]) noexcept { return std::atan2(static_cast<double>(rows[4]), static_cast<double>(rows[3])); }

void FollowVelocity(const FollowIn& in, const FollowGains& g, double (&bias)[3], FollowOut* out) noexcept {
    FollowOut o;
    double dt = in.dt_s;
    if (!(dt > 0.001)) dt = 0.001;
    if (dt > 0.1) dt = 0.1;
    double up[3] = {in.up[0], in.up[1], in.up[2]};
    const double un = Len3(up);
    if (!(un > 0.5) || !std::isfinite(un)) up[0] = 0, up[1] = 0, up[2] = 1;
    else
        for (double& c : up) c /= un;
    auto horiz = [&](const double (&v)[3], double (&h)[3]) {
        const double d = v[0] * up[0] + v[1] * up[1] + v[2] * up[2];
        for (int i = 0; i < 3; ++i) h[i] = v[i] - d * up[i];
    };
    double e[3] = {in.err[0], in.err[1], in.err[2]};
    for (double& c : e)
        if (!std::isfinite(c)) c = 0;
    // our forward and right axes in the plane of our up axis
    double f[3] = {in.fwd[0], in.fwd[1], in.fwd[2]};
    horiz(f, f);
    double fn = Len3(f);
    if (!(fn > 0.3) || !std::isfinite(fn)) f[0] = 0, f[1] = 1, f[2] = 0, fn = 1;
    for (double& c : f) c /= fn;
    const double right[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    auto dot = [](const double (&x)[3], const double (&y)[3]) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    const double en = Len3(e);
    const double gain = en > g.far_m ? g.far_gain : 1.0;
    const double beta = 1.0 - std::exp(-dt / (g.tau_s > 0.001 ? g.tau_s : 0.001));
    double vn[3], vt[3];
    for (int i = 0; i < 3; ++i) {
        vn[i] = std::isfinite(in.v_now[i]) ? in.v_now[i] : 0.0;
        vt[i] = std::isfinite(in.v_target[i]) ? in.v_target[i] : 0.0;
    }
    const double e_al = dot(e, f), e_lat = dot(e, right);
    const double v_al = dot(vn, f), v_lat = dot(vn, right), t_al = dot(vt, f), t_lat = dot(vt, right);
    // along (what the wheels roll): the owner's speed plus the error closed in 1 / kp, an integral for what the tyres hold
    if (in.still) {
        bias[0] = bias[1] = bias[2] = 0;
    } else {
        bias[0] += g.ki * e_al * dt;
        if (bias[0] > g.max_bias_mps) bias[0] = g.max_bias_mps;
        if (bias[0] < -g.max_bias_mps) bias[0] = -g.max_bias_mps;
        bias[1] = bias[2] = 0;
    }
    double c_al = g.kp * gain * e_al + bias[0];
    const double mc = g.max_corr_mps * gain;
    if (c_al > mc) c_al = mc;
    if (c_al < -mc) c_al = -mc;
    double dv_al = beta * (t_al + c_al - v_al);
    const double mal = g.max_dv_mps * gain;
    if (dv_al > mal) dv_al = mal;
    if (dv_al < -mal) dv_al = -mal;
    // sideways only a little: the tyres hold a truck's side, and a sideways push at its centre of mass tips it over
    // (hidden run 20261120-161614: 0.6 m/s a tick rolled the follower 40 degrees onto the road's barrier); the side error
    // is steered away through the yaw below
    double dv_lat = beta * (t_lat + g.kp_lat * e_lat - v_lat);
    if (dv_lat > g.max_dv_lat_mps) dv_lat = g.max_dv_lat_mps;
    if (dv_lat < -g.max_dv_lat_mps) dv_lat = -g.max_dv_lat_mps;
    for (int i = 0; i < 3; ++i) {
        const double d = dv_al * f[i] + dv_lat * right[i];
        o.dv[i] = static_cast<float>(d);
        o.v_set[i] = static_cast<float>(vn[i] + d);
    }
    // the sideways error steered away through the yaw (pure pursuit)
    double yaw_err = WrapPi(in.yaw_err);
    if (std::fabs(v_al) > g.lat_min_speed_mps) {
        const double ahead = (std::max)(g.lat_min_m, std::fabs(v_al) * g.lat_ahead_s);
        double th = std::atan2(e_lat, ahead);
        if (th > g.lat_max_rad) th = g.lat_max_rad;
        if (th < -g.lat_max_rad) th = -g.lat_max_rad;
        th *= g.lat_sign;
        yaw_err -= v_al > 0 ? th : -th;  // target to the right: turn right (negative yaw) going forward
    }
    double yc = g.kyaw * gain * yaw_err;
    if (yc > g.max_yaw_corr) yc = g.max_yaw_corr;
    if (yc < -g.max_yaw_corr) yc = -g.max_yaw_corr;
    const double wn = std::isfinite(in.w_now_z) ? in.w_now_z : 0.0, wt = std::isfinite(in.w_target_z) ? in.w_target_z : 0.0;
    double dw = in.yaw ? beta * (wt + yc - wn) : 0.0;
    const double mdw = g.max_dw_rad * gain;
    if (dw > mdw) dw = mdw;
    if (dw < -mdw) dw = -mdw;
    o.dw = static_cast<float>(dw);
    o.w_set_z = static_cast<float>(wn + dw);
    o.yaw_cmd = yaw_err;
    *out = o;
}

double SteerTrim(double yaw_cmd, double v_along, const FollowGains& g) noexcept {
    if (!std::isfinite(yaw_cmd) || !std::isfinite(v_along) || std::fabs(v_along) < g.lat_min_speed_mps) return 0;
    double t = -g.k_steer * yaw_cmd * (v_along > 0 ? 1.0 : -1.0);
    if (t > g.max_steer) t = g.max_steer;
    if (t < -g.max_steer) t = -g.max_steer;
    return t;
}

const char* DriveTpName(DriveTp t) noexcept {
    switch (t) {
        case DriveTp::none: return "none";
        case DriveTp::too_far: return "far";
        case DriveTp::angle: return "angle";
        case DriveTp::stuck: return "stuck";
        case DriveTp::stopped: return "stopped";
    }
    return "?";
}

DriveTp DriveTeleport(const DriveTpIn& in) noexcept {
    if (in.since_last_tp_ms < kTpGapMs) return DriveTp::none;
    if (in.err_m >= (in.carried ? kTpFarCarriedM : kTpFarM)) return DriveTp::too_far;
    if (in.ang_deg >= (in.carried ? kTpAngleCarriedDeg : kTpAngleDeg)) return DriveTp::angle;
    if (in.carried) {
        // a standstill re-seat (v0.21): both stand, the owner's for kTpParkedMs, ours still kTpStoppedM or kTpParkedDeg off
        if (in.still_ms >= kTpStoppedMs && in.owner_still_ms >= kTpParkedMs && (in.err_m >= kTpStoppedM || in.ang_deg >= kTpParkedDeg))
            return DriveTp::stopped;
        return DriveTp::none;
    }
    if (in.stuck_ms >= kTpStuckMs && in.err_m >= kTpStuckM && in.err_m >= kTpStuckFall * in.stuck_err0) return DriveTp::stuck;
    if (in.owner_still_ms >= kTpParkedMs && (in.err_m >= kTpParkedM || in.ang_deg >= kTpParkedDeg)) return DriveTp::stopped;
    if (in.still_ms >= kTpStoppedMs && in.err_m >= kTpStoppedM && in.sam_dist_m > kTpStoppedNearM) return DriveTp::stopped;
    return DriveTp::none;
}

void KindHalfExtents(std::uint8_t kind, float (&half)[3]) noexcept {
    // the drivable art's size plus a margin (right, forward, up)
    switch (kind) {
        case 1: half[0] = 1.1f, half[1] = 2.4f, half[2] = 1.0f; break;   // roadster
        case 2: half[0] = 1.5f, half[1] = 4.4f, half[2] = 2.0f; break;   // truck
        case 3: half[0] = 0.6f, half[1] = 1.4f, half[2] = 0.9f; break;   // trike
        default: half[0] = 1.5f, half[1] = 4.4f, half[2] = 2.0f; break;
    }
}

bool PointInFootprint(const Footprint& f, const double (&p)[3], double margin_m) noexcept {
    const double d[3] = {p[0] - f.c[0], p[1] - f.c[1], p[2] - f.c[2]};
    for (int a = 0; a < 3; ++a) {
        const double along = d[0] * f.rot[a * 3 + 0] + d[1] * f.rot[a * 3 + 1] + d[2] * f.rot[a * 3 + 2];
        if (std::fabs(along) > f.half[a] + margin_m) return false;
    }
    return true;
}

bool FootprintsOverlap(const Footprint& a, const Footprint& b) noexcept {
    // heights (world z) first: centre +- the up extent
    if (std::fabs(a.c[2] - b.c[2]) > static_cast<double>(a.half[2]) + b.half[2]) return false;
    // ground plane: the right / forward axes of both, projected to x / y
    auto axis = [](const Footprint& f, int k, double (&o)[2]) {
        o[0] = f.rot[k * 3 + 0], o[1] = f.rot[k * 3 + 1];
        const double n = std::sqrt(o[0] * o[0] + o[1] * o[1]);
        if (n > 1e-6) o[0] /= n, o[1] /= n;
        else o[0] = k == 0 ? 1 : 0, o[1] = k == 0 ? 0 : 1;
    };
    double ax[4][2];
    axis(a, 0, ax[0]), axis(a, 1, ax[1]), axis(b, 0, ax[2]), axis(b, 1, ax[3]);
    const double d[2] = {b.c[0] - a.c[0], b.c[1] - a.c[1]};
    for (const auto& n : ax) {
        auto radius = [&](const Footprint& f) {
            double r = 0;
            for (int k = 0; k < 2; ++k) {
                double e[2];
                axis(f, k, e);
                r += f.half[k] * std::fabs(e[0] * n[0] + e[1] * n[1]);
            }
            return r;
        };
        if (std::fabs(d[0] * n[0] + d[1] * n[1]) > radius(a) + radius(b)) return false;
    }
    return true;
}

bool TeleportClear(const Footprint& target, bool have_sam, const double (&sam)[3], const Footprint* others, std::size_t n_others) noexcept {
    if (have_sam && PointInFootprint(target, sam, 0.6)) return false;
    for (std::size_t i = 0; i < n_others; ++i)
        if (FootprintsOverlap(target, others[i])) return false;
    return true;
}

void QuatFromRows(const float (&r)[9], float (&q)[4]) noexcept {
    const float m[3][3] = {{r[0], r[1], r[2]}, {r[3], r[4], r[5]}, {r[6], r[7], r[8]}};
    float x, y, z, w;
    const float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        w = 0.25f * s, x = (m[2][1] - m[1][2]) / s, y = (m[0][2] - m[2][0]) / s, z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        w = (m[2][1] - m[1][2]) / s, x = 0.25f * s, y = (m[0][1] + m[1][0]) / s, z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        w = (m[0][2] - m[2][0]) / s, x = (m[0][1] + m[1][0]) / s, y = 0.25f * s, z = (m[1][2] + m[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        w = (m[1][0] - m[0][1]) / s, x = (m[0][2] + m[2][0]) / s, y = (m[1][2] + m[2][1]) / s, z = 0.25f * s;
    }
    const float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(n > 1e-6f) || !std::isfinite(n)) {
        q[0] = q[1] = q[2] = 0, q[3] = 1;
        return;
    }
    q[0] = x / n, q[1] = y / n, q[2] = z / n, q[3] = w / n;
}

void RowsFromQuat(const float (&in)[4], float (&m)[9]) noexcept {
    float n = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2] + in[3] * in[3]);
    if (!(n > 1e-6f) || !std::isfinite(n)) n = 1;
    const float x = in[0] / n, y = in[1] / n, z = in[2] / n, w = in[3] / n;
    const float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    m[0] = 1 - 2 * (yy + zz), m[1] = 2 * (xy - wz), m[2] = 2 * (xz + wy);
    m[3] = 2 * (xy + wz), m[4] = 1 - 2 * (xx + zz), m[5] = 2 * (yz - wx);
    m[6] = 2 * (xz - wy), m[7] = 2 * (yz + wx), m[8] = 1 - 2 * (xx + yy);
}

double AngleBetweenRows(const float (&a)[9], const float (&b)[9]) noexcept {
    // trace(A * B^T) = 1 + 2 cos(angle) for rotation matrices given as rows
    double tr = 0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) tr += static_cast<double>(a[r * 3 + c]) * b[r * 3 + c];
    double c = (tr - 1.0) * 0.5;
    if (c > 1) c = 1;
    if (c < -1) c = -1;
    return std::acos(c) * 57.29577951308232;
}

}  // namespace st::vwire
