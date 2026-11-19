// STX1, the host-held save session's save transfer: the pure part (save_xfer.hpp). No game calls, no file I/O.
#include "save_xfer.hpp"

#include <cstring>

namespace st::stx {
namespace {

// ---- little-endian helpers ------------------------------------------------------------------------------
class Writer {
public:
    Writer(std::uint8_t* out, std::size_t cap) noexcept : p_(out), cap_(cap) {}
    void U8(std::uint8_t v) noexcept {
        if (n_ + 1 <= cap_) p_[n_] = v;
        n_ += 1;
    }
    void U16(std::uint16_t v) noexcept {
        U8(static_cast<std::uint8_t>(v));
        U8(static_cast<std::uint8_t>(v >> 8));
    }
    void U32(std::uint32_t v) noexcept {
        for (int i = 0; i < 4; ++i) U8(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void Bytes(const std::uint8_t* b, std::size_t len) noexcept {
        if (n_ + len <= cap_ && len) std::memcpy(p_ + n_, b, len);
        n_ += len;
    }
    void Str(const char* s, std::size_t field) noexcept {
        std::size_t len = 0;
        while (len < field && s[len]) ++len;
        Bytes(reinterpret_cast<const std::uint8_t*>(s), len);
        for (std::size_t i = len; i < field; ++i) U8(0);
    }
    std::size_t Done() const noexcept { return n_ <= cap_ ? n_ : 0; }

private:
    std::uint8_t* p_;
    std::size_t cap_, n_ = 0;
};

class Reader {
public:
    Reader(const std::uint8_t* p, std::size_t n) noexcept : p_(p), n_(n) {}
    std::uint8_t U8() noexcept { return at_ < n_ ? p_[at_++] : (bad_ = true, 0); }
    std::uint16_t U16() noexcept {
        const std::uint16_t a = U8();
        return static_cast<std::uint16_t>(a | (U8() << 8));
    }
    std::uint32_t U32() noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(U8()) << (8 * i);
        return v;
    }
    void Bytes(std::uint8_t* out, std::size_t len) noexcept {
        for (std::size_t i = 0; i < len; ++i) out[i] = U8();
    }
    // Printable ASCII, NUL padded (nothing after the first NUL); `empty_ok` decides about an all-NUL field.
    bool Str(char* out, std::size_t field, bool empty_ok) noexcept {
        bool ended = false, ok = true;
        std::size_t len = 0;
        for (std::size_t i = 0; i < field; ++i) {
            const std::uint8_t c = U8();
            if (ended) {
                ok = ok && c == 0;
            } else if (c == 0) {
                ended = true;
            } else if (c < 0x20 || c > 0x7E) {
                ok = false;
            } else {
                out[len++] = static_cast<char>(c);
            }
        }
        out[len] = 0;
        return ok && (empty_ok || len > 0);
    }
    const std::uint8_t* Here() const noexcept { return p_ + at_; }
    void Skip(std::size_t len) noexcept {
        if (at_ + len > n_) bad_ = true;
        else at_ += len;
    }
    bool AtEnd() const noexcept { return !bad_ && at_ == n_; }
    bool bad() const noexcept { return bad_; }

private:
    const std::uint8_t* p_;
    std::size_t n_, at_ = 0;
    bool bad_ = false;
};

bool BuildOk(const char* s) noexcept {
    std::size_t len = 0;
    for (; s[len]; ++len) {
        if (len >= kBuildChars) return false;
        if (s[len] < 0x20 || s[len] > 0x7E) return false;
    }
    return len > 0;
}

void Head(Writer& w, Kind kind, std::uint32_t xfer_id) noexcept {
    w.U8(kMagic);
    w.U8(kVersion);
    w.U8(static_cast<std::uint8_t>(kind));
    w.U8(0);
    w.U32(xfer_id);
}

void PutVersion(Writer& w, const GameVersion& v) noexcept {
    w.Str(v.build, kBuildChars);
    w.U32(v.savegame_version);
    w.U32(v.save_flags);
}

bool GetVersion(Reader& r, GameVersion& v) noexcept {
    const bool ok = r.Str(v.build, kBuildChars, false);
    v.savegame_version = r.U32();
    v.save_flags = r.U32();
    return ok;
}

bool GetBool(Reader& r, bool& out) noexcept {
    const std::uint8_t b = r.U8();
    out = b == 1;
    return b <= 1;
}

constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t Rotr(std::uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }

}  // namespace

// ---- names --------------------------------------------------------------------------------------------------
const char* KindName(Kind k) noexcept {
    switch (k) {
        case Kind::state: return "STATE";
        case Kind::gstate: return "GSTATE";
        case Kind::offer: return "OFFER";
        case Kind::want: return "WANT";
        case Kind::chunk: return "CHUNK";
        case Kind::end: return "END";
        case Kind::result: return "RESULT";
        case Kind::abort: return "ABORT";
    }
    return "?";
}

const char* ResultCodeName(ResultCode c) noexcept {
    switch (c) {
        case ResultCode::ok: return "ok";
        case ResultCode::sha_mismatch: return "sha_mismatch";
        case ResultCode::parse: return "parse";
        case ResultCode::version: return "version";
        case ResultCode::disk: return "disk";
        case ResultCode::load_refused: return "load_refused";
        case ResultCode::load_failed: return "load_failed";
    }
    return "?";
}

const char* AbortCodeName(AbortCode c) noexcept {
    switch (c) {
        case AbortCode::superseded: return "superseded";
        case AbortCode::cancelled: return "cancelled";
        case AbortCode::read_failed: return "read_failed";
        case AbortCode::protocol: return "protocol";
        case AbortCode::session_end: return "session_end";
    }
    return "?";
}

const char* ReasonName(Reason r) noexcept {
    switch (r) {
        case Reason::title_continue: return "title_continue";
        case Reason::title_load: return "title_load";
        case Reason::new_game_first_save: return "new_game_first_save";
        case Reason::in_world_load: return "in_world_load";
        case Reason::retry: return "retry";
        case Reason::late_join: return "late_join";
    }
    return "?";
}

// ---- SHA-256 ---------------------------------------------------------------------------------------------------
void Sha256Ctx::Reset() noexcept {
    static constexpr std::uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, kInit, sizeof(h_));
    used_ = 0;
    total_ = 0;
}

void Sha256Ctx::Block(const std::uint8_t* p) noexcept {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<std::uint32_t>(p[4 * i]) << 24) | (static_cast<std::uint32_t>(p[4 * i + 1]) << 16) |
               (static_cast<std::uint32_t>(p[4 * i + 2]) << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256Ctx::Update(const void* data, std::size_t size) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    total_ += size;
    if (used_) {
        const std::size_t take = size < 64 - used_ ? size : 64 - used_;
        std::memcpy(buf_ + used_, p, take);
        used_ += take;
        p += take;
        size -= take;
        if (used_ < 64) return;
        Block(buf_);
        used_ = 0;
    }
    while (size >= 64) {
        Block(p);
        p += 64;
        size -= 64;
    }
    if (size) {
        std::memcpy(buf_, p, size);
        used_ = size;
    }
}

Sha256 Sha256Ctx::Final() noexcept {
    const std::uint64_t bits = total_ * 8;
    const std::uint8_t pad = 0x80;
    Update(&pad, 1);
    const std::uint8_t zero = 0;
    while (used_ != 56) Update(&zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    Update(len, 8);
    Sha256 out{};
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
        out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
        out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
        out[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
    }
    Reset();
    return out;
}

Sha256 Sha256Of(const void* data, std::size_t size) noexcept {
    Sha256Ctx c;
    c.Update(data, size);
    return c.Final();
}

std::string ShaHex(const Sha256& sha) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (std::uint8_t b : sha) {
        s += kHex[b >> 4];
        s += kHex[b & 15];
    }
    return s;
}

bool ShaIsZero(const Sha256& sha) noexcept {
    for (std::uint8_t b : sha)
        if (b) return false;
    return true;
}

// ---- fields ---------------------------------------------------------------------------------------------------
bool SetBuild(GameVersion& v, const char* text) noexcept {
    if (!text || !BuildOk(text)) return false;
    std::memset(v.build, 0, sizeof(v.build));
    std::memcpy(v.build, text, std::strlen(text));
    return true;
}

bool SameVersion(const GameVersion& a, const GameVersion& b) noexcept {
    return std::strncmp(a.build, b.build, kBuildChars) == 0 && a.savegame_version == b.savegame_version && a.save_flags == b.save_flags;
}

bool IsSlotName(const char* name) noexcept {
    if (!name) return false;
    for (const char* prefix : {"autosave", "manualsave", "quicksave"}) {
        const std::size_t n = std::strlen(prefix);
        if (std::strncmp(name, prefix, n) != 0) continue;
        const char* d = name + n;
        std::size_t digits = 0;
        while (d[digits] >= '0' && d[digits] <= '9') ++digits;
        if (d[digits] != 0 || digits == 0 || digits > 3) return false;
        return !(digits > 1 && d[0] == '0');
    }
    return false;
}

bool SetSlotName(Offer& o, const char* name) noexcept {
    if (!IsSlotName(name) || std::strlen(name) > kSlotNameChars) return false;
    std::memset(o.slot_name, 0, sizeof(o.slot_name));
    std::memcpy(o.slot_name, name, std::strlen(name));
    return true;
}

// ---- encode -----------------------------------------------------------------------------------------------------
std::size_t EncodeState(const State& s, std::uint8_t* out, std::size_t cap) noexcept {
    if (static_cast<std::uint8_t>(s.phase) > 3 || !BuildOk(s.version.build)) return 0;
    Writer w(out, cap);
    Head(w, Kind::state, 0);
    w.U8(static_cast<std::uint8_t>(s.phase));
    w.U32(s.load_counter);
    w.Bytes(s.current_save_sha.data(), 32);
    w.U32(s.offer_xfer_id);
    PutVersion(w, s.version);
    w.U8(s.new_game_pending ? 1 : 0);
    return w.Done();
}

std::size_t EncodeGState(const GState& g, std::uint8_t* out, std::size_t cap) noexcept {
    if (static_cast<std::uint8_t>(g.phase) > 4 || g.have_bytes > kMaxSaveBytes || g.refuse_reason > 6) return 0;
    Writer w(out, cap);
    Head(w, Kind::gstate, 0);
    w.U8(static_cast<std::uint8_t>(g.phase));
    w.U32(g.loaded_load_counter);
    w.Bytes(g.loaded_sha.data(), 32);
    w.U32(g.have_bytes);
    w.U16(g.refuse_reason);
    return w.Done();
}

std::size_t EncodeOffer(std::uint32_t xfer_id, const Offer& o, std::uint8_t* out, std::size_t cap) noexcept {
    if (!xfer_id || !IsSlotName(o.slot_name) || o.size == 0 || o.size > kMaxSaveBytes || static_cast<std::uint8_t>(o.reason) > 5 ||
        !BuildOk(o.version.build))
        return 0;
    Writer w(out, cap);
    Head(w, Kind::offer, xfer_id);
    w.U32(o.load_counter);
    w.U8(o.slot_type);
    w.U8(static_cast<std::uint8_t>(o.slot_index));
    w.Str(o.slot_name, kSlotNameChars);
    w.U32(o.size);
    w.Bytes(o.sha.data(), 32);
    w.U8(o.approx ? 1 : 0);
    w.U8(static_cast<std::uint8_t>(o.reason));
    PutVersion(w, o.version);
    return w.Done();
}

std::size_t EncodeWant(std::uint32_t xfer_id, const Want& want, std::uint8_t* out, std::size_t cap) noexcept {
    if (!xfer_id || want.next_offset > kMaxSaveBytes) return 0;
    Writer w(out, cap);
    Head(w, Kind::want, xfer_id);
    w.U32(want.next_offset);
    w.Bytes(want.sha.data(), 32);
    return w.Done();
}

std::size_t EncodeChunk(std::uint32_t xfer_id, std::uint32_t offset, const std::uint8_t* data, std::size_t len, std::uint8_t* out,
                        std::size_t cap) noexcept {
    if (!xfer_id || !data || len == 0 || len > kMaxChunk || offset > kMaxSaveBytes || len > kMaxSaveBytes - offset) return 0;
    Writer w(out, cap);
    Head(w, Kind::chunk, xfer_id);
    w.U32(offset);
    w.U16(static_cast<std::uint16_t>(len));
    w.Bytes(data, len);
    return w.Done();
}

std::size_t EncodeEnd(std::uint32_t xfer_id, const End& e, std::uint8_t* out, std::size_t cap) noexcept {
    if (!xfer_id || e.size == 0 || e.size > kMaxSaveBytes) return 0;
    Writer w(out, cap);
    Head(w, Kind::end, xfer_id);
    w.U32(e.size);
    w.Bytes(e.sha.data(), 32);
    return w.Done();
}

std::size_t EncodeResult(std::uint32_t xfer_id, const Result& r, std::uint8_t* out, std::size_t cap) noexcept {
    if (!xfer_id || static_cast<std::uint16_t>(r.code) > 6 || r.ok != (r.code == ResultCode::ok)) return 0;
    Writer w(out, cap);
    Head(w, Kind::result, xfer_id);
    w.U8(r.ok ? 1 : 0);
    w.U16(static_cast<std::uint16_t>(r.code));
    return w.Done();
}

std::size_t EncodeAbort(std::uint32_t xfer_id, AbortCode code, std::uint8_t* out, std::size_t cap) noexcept {
    const std::uint16_t c = static_cast<std::uint16_t>(code);
    if (!xfer_id || c < 1 || c > 5) return 0;
    Writer w(out, cap);
    Head(w, Kind::abort, xfer_id);
    w.U16(c);
    return w.Done();
}

// ---- decode ------------------------------------------------------------------------------------------------------
bool Decode(const std::uint8_t* data, std::size_t size, Message& out) noexcept {
    if (!data || size < kHeaderSize || size > kMaxMessage) return false;
    out = Message{};
    Reader r(data, size);
    if (r.U8() != kMagic || r.U8() != kVersion) return false;
    const std::uint8_t kind = r.U8();
    out.h.flags = r.U8();
    out.h.xfer_id = r.U32();
    if (kind < 1 || kind > 8 || out.h.flags != 0) return false;
    out.h.kind = static_cast<Kind>(kind);
    const std::size_t body = size - kHeaderSize;
    const bool control = out.h.kind == Kind::state || out.h.kind == Kind::gstate;
    if (control ? out.h.xfer_id != 0 : out.h.xfer_id == 0) return false;
    switch (out.h.kind) {
        case Kind::state: {
            if (body != kStateBody) return false;
            State& s = out.state;
            const std::uint8_t phase = r.U8();
            s.load_counter = r.U32();
            r.Bytes(s.current_save_sha.data(), 32);
            s.offer_xfer_id = r.U32();
            const bool ver = GetVersion(r, s.version);
            const bool pending = GetBool(r, s.new_game_pending);
            if (phase > 3 || !ver || !pending) return false;
            s.phase = static_cast<HostPhase>(phase);
            break;
        }
        case Kind::gstate: {
            if (body != kGStateBody) return false;
            GState& g = out.gstate;
            const std::uint8_t phase = r.U8();
            g.loaded_load_counter = r.U32();
            r.Bytes(g.loaded_sha.data(), 32);
            g.have_bytes = r.U32();
            g.refuse_reason = r.U16();
            if (phase > 4 || g.have_bytes > kMaxSaveBytes || g.refuse_reason > 6) return false;
            g.phase = static_cast<GuestPhase>(phase);
            break;
        }
        case Kind::offer: {
            if (body != kOfferBody) return false;
            Offer& o = out.offer;
            o.load_counter = r.U32();
            o.slot_type = r.U8();
            o.slot_index = static_cast<std::int8_t>(r.U8());
            const bool name = r.Str(o.slot_name, kSlotNameChars, false);
            o.size = r.U32();
            r.Bytes(o.sha.data(), 32);
            const bool approx = GetBool(r, o.approx);
            const std::uint8_t reason = r.U8();
            const bool ver = GetVersion(r, o.version);
            if (!name || !IsSlotName(o.slot_name) || o.size == 0 || o.size > kMaxSaveBytes || !approx || reason > 5 || !ver) return false;
            o.reason = static_cast<Reason>(reason);
            break;
        }
        case Kind::want:
            if (body != kWantBody) return false;
            out.want.next_offset = r.U32();
            r.Bytes(out.want.sha.data(), 32);
            if (out.want.next_offset > kMaxSaveBytes) return false;
            break;
        case Kind::chunk: {
            if (body < kChunkHead + 1) return false;
            out.chunk.offset = r.U32();
            out.chunk.len = r.U16();
            if (out.chunk.len == 0 || out.chunk.len > kMaxChunk || body != kChunkHead + out.chunk.len) return false;
            if (out.chunk.offset > kMaxSaveBytes || out.chunk.len > kMaxSaveBytes - out.chunk.offset) return false;
            out.chunk.data = r.Here();
            r.Skip(out.chunk.len);
            break;
        }
        case Kind::end:
            if (body != kEndBody) return false;
            out.end.size = r.U32();
            r.Bytes(out.end.sha.data(), 32);
            if (out.end.size == 0 || out.end.size > kMaxSaveBytes) return false;
            break;
        case Kind::result: {
            if (body != kResultBody) return false;
            const std::uint8_t ok = r.U8();
            const std::uint16_t code = r.U16();
            if (ok > 1 || code > 6 || (ok == 1) != (code == 0)) return false;
            out.result.ok = ok == 1;
            out.result.code = static_cast<ResultCode>(code);
            break;
        }
        case Kind::abort: {
            if (body != kAbortBody) return false;
            const std::uint16_t code = r.U16();
            if (code < 1 || code > 5) return false;
            out.abort.code = static_cast<AbortCode>(code);
            break;
        }
    }
    return r.AtEnd();
}

// ---- checkpoint.dat ---------------------------------------------------------------------------------------------
const char* CheckpointErrorName(CheckpointError e) noexcept {
    switch (e) {
        case CheckpointError::ok: return "ok";
        case CheckpointError::too_small: return "too_small";
        case CheckpointError::too_large: return "too_large";
        case CheckpointError::bad_size_field: return "bad_size_field";
        case CheckpointError::size_mismatch: return "size_mismatch";
        case CheckpointError::bad_magic: return "bad_magic";
        case CheckpointError::bad_ini: return "bad_ini";
    }
    return "?";
}

namespace {

// 1..7 decimal digits (no leading zero unless "0"), then NULs only.
bool SizeField(const std::uint8_t* p, std::uint32_t& out) noexcept {
    std::uint32_t v = 0;
    int digits = 0;
    int i = 0;
    for (; i < 8 && p[i] >= '0' && p[i] <= '9'; ++i, ++digits) v = v * 10 + static_cast<std::uint32_t>(p[i] - '0');
    if (digits == 0 || digits > 7 || (digits > 1 && p[0] == '0')) return false;
    // A NUL ends the number; the game leaves whatever was in its buffer after it (run 20261119-074751: a fresh autosave
    // began "410\0p\x05\0\0"), so the bytes after the NUL are not checked.
    if (p[i] != 0) return false;
    out = v;
    return true;
}

std::string TrimAscii(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

}  // namespace

CheckpointError ValidateCheckpoint(const std::uint8_t* data, std::size_t size, CheckpointInfo* info) {
    if (!data || size < 24 + 8 + 4) return CheckpointError::too_small;
    if (size > kMaxSaveBytes) return CheckpointError::too_large;
    std::uint32_t ini = 0, png = 0, dat = 0;
    if (!SizeField(data, ini) || !SizeField(data + 8, png) || !SizeField(data + 16, dat)) return CheckpointError::bad_size_field;
    if (static_cast<std::uint64_t>(24) + ini + png + dat != size) return CheckpointError::size_mismatch;
    if (dat < 4 || ini < 8) return CheckpointError::size_mismatch;
    const std::uint8_t* d = data + 24 + ini + png;
    const std::uint32_t magic = static_cast<std::uint32_t>(d[0]) | (static_cast<std::uint32_t>(d[1]) << 8) |
                                (static_cast<std::uint32_t>(d[2]) << 16) | (static_cast<std::uint32_t>(d[3]) << 24);
    if (magic != kCheckpointMagic) return CheckpointError::bad_magic;
    // The [Slot] ini: printable text, CR/LF/TAB, trailing NULs allowed.
    std::string text(reinterpret_cast<const char*>(data + 24), ini);
    while (!text.empty() && text.back() == 0) text.pop_back();
    for (unsigned char c : text)
        if (c != '\r' && c != '\n' && c != '\t' && (c < 0x20 || c > 0x7E)) return CheckpointError::bad_ini;
    CheckpointInfo got;
    got.ini_size = ini;
    got.png_size = png;
    got.data_size = dat;
    bool header = false, have_time = false;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = TrimAscii(line);
        if (line.empty()) continue;
        if (!header) {
            if (line != "[Slot]") return CheckpointError::bad_ini;
            header = true;
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) return CheckpointError::bad_ini;
        const std::string key = TrimAscii(line.substr(0, eq)), value = TrimAscii(line.substr(eq + 1));
        for (char c : key)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return CheckpointError::bad_ini;
        if (key == "ModificationTime") {
            if (value.empty() || value.size() > 20) return CheckpointError::bad_ini;
            std::uint64_t t = 0;
            for (char c : value) {
                if (c < '0' || c > '9') return CheckpointError::bad_ini;
                t = t * 10 + static_cast<std::uint64_t>(c - '0');
            }
            got.modification_time = t;
            have_time = true;
        } else if (key == "Title") {
            got.title = value;
        } else if (key == "SubTitle") {
            got.subtitle = value;
        }
    }
    if (!header || !have_time) return CheckpointError::bad_ini;
    if (info) *info = got;
    return CheckpointError::ok;
}

// ---- Sender -----------------------------------------------------------------------------------------------------
const char* Sender::PhaseName(Phase p) noexcept {
    switch (p) {
        case Phase::idle: return "idle";
        case Phase::offered: return "offered";
        case Phase::streaming: return "streaming";
        case Phase::ended: return "ended";
        case Phase::done: return "done";
        case Phase::failed: return "failed";
        case Phase::aborted: return "aborted";
    }
    return "?";
}

std::uint32_t Sender::NewId() noexcept {
    const std::uint32_t id = next_id_;
    next_id_ = next_id_ + 1 ? next_id_ + 1 : 1;
    return id;
}

bool Sender::Offer(const st::stx::Offer& offer, const std::uint8_t* data, std::size_t size) noexcept {
    if (!data || size == 0 || size > kMaxSaveBytes) return false;
    st::stx::Offer o = offer;
    o.size = static_cast<std::uint32_t>(size);
    o.sha = Sha256Of(data, size);
    std::uint8_t probe[kMaxMessage];
    if (!EncodeOffer(1, o, probe, sizeof(probe))) return false;
    if (phase_ != Phase::idle) {
        if (o.load_counter < offer_.load_counter) return false;
        if (o.load_counter == offer_.load_counter) {
            if (o.sha != offer_.sha) return false;
            if (busy() || phase_ == Phase::done) {
                Reannounce();
                return true;
            }
            // failed / aborted: the same save once more, as a new transfer
        } else if (busy()) {
            // Superseded: the guest drops the old part; only the newest offer is ever loaded.
            control_.clear();
            std::uint8_t buf[kHeaderSize + kAbortBody];
            const std::size_t n = EncodeAbort(xfer_id_, AbortCode::superseded, buf, sizeof(buf));
            control_.emplace_back(buf, buf + n);
        }
    }
    try {
        data_.assign(data, data + size);
    } catch (...) {
        return false;
    }
    offer_ = o;
    xfer_id_ = NewId();
    send_offset_ = 0;
    end_pending_ = false;
    result_ = ResultCode::ok;
    phase_ = Phase::offered;
    std::uint8_t buf[kMaxMessage];
    const std::size_t n = EncodeOffer(xfer_id_, offer_, buf, sizeof(buf));
    control_.emplace_back(buf, buf + n);
    return true;
}

void Sender::Reannounce() noexcept {
    if (!busy() && phase_ != Phase::done) return;
    // A new link session lost whatever was in flight: the guest answers with WANT <bytes it has> (or RESULT ok when
    // it already loaded this one).
    control_.clear();
    std::uint8_t buf[kMaxMessage];
    const std::size_t n = EncodeOffer(xfer_id_, offer_, buf, sizeof(buf));
    control_.emplace_back(buf, buf + n);
    phase_ = Phase::offered;
    end_pending_ = false;
}

void Sender::Cancel(AbortCode code) noexcept {
    if (!busy()) return;
    control_.clear();
    std::uint8_t buf[kHeaderSize + kAbortBody];
    const std::size_t n = EncodeAbort(xfer_id_, code, buf, sizeof(buf));
    if (n) control_.emplace_back(buf, buf + n);
    abort_code_ = code;
    phase_ = Phase::aborted;
}

void Sender::OnMessage(const Message& m) noexcept {
    if (m.h.kind != Kind::want && m.h.kind != Kind::result && m.h.kind != Kind::abort) return;
    if (phase_ == Phase::idle || m.h.xfer_id != xfer_id_) {
        ++stale;
        return;
    }
    switch (m.h.kind) {
        case Kind::want:
            if (!busy()) {
                ++stale;
                return;
            }
            if (m.want.sha != offer_.sha || m.want.next_offset > offer_.size) {
                Cancel(AbortCode::protocol);
                return;
            }
            if (m.want.next_offset > 0) ++resumes;
            else if (phase_ == Phase::streaming || phase_ == Phase::ended) ++restarts;
            send_offset_ = m.want.next_offset;
            end_pending_ = true;
            phase_ = Phase::streaming;
            break;
        case Kind::result:
            result_ = m.result.code;
            phase_ = m.result.ok ? Phase::done : Phase::failed;
            control_.clear();
            end_pending_ = false;
            break;
        case Kind::abort:
            abort_code_ = m.abort.code;
            phase_ = Phase::aborted;
            control_.clear();
            end_pending_ = false;
            break;
        default: break;
    }
}

bool Sender::has_output() const noexcept { return !control_.empty() || (phase_ == Phase::streaming && end_pending_); }

std::size_t Sender::Build(std::uint8_t* out, std::size_t cap) const noexcept {
    if (!control_.empty()) {
        const std::vector<std::uint8_t>& m = control_.front();
        if (m.size() > cap) return 0;
        std::memcpy(out, m.data(), m.size());
        return m.size();
    }
    if (phase_ != Phase::streaming || !end_pending_) return 0;
    if (send_offset_ < offer_.size) {
        const std::size_t left = offer_.size - send_offset_;
        const std::size_t len = left < kMaxChunk ? left : kMaxChunk;
        return EncodeChunk(xfer_id_, send_offset_, data_.data() + send_offset_, len, out, cap);
    }
    End e;
    e.size = offer_.size;
    e.sha = offer_.sha;
    return EncodeEnd(xfer_id_, e, out, cap);
}

void Sender::Advance() noexcept {
    if (!control_.empty()) {
        control_.pop_front();
        return;
    }
    if (phase_ != Phase::streaming || !end_pending_) return;
    if (send_offset_ < offer_.size) {
        const std::uint32_t left = offer_.size - send_offset_;
        const std::uint32_t len = left < kMaxChunk ? left : static_cast<std::uint32_t>(kMaxChunk);
        send_offset_ += len;
        ++chunks_sent;
        bytes_sent += len;
        return;
    }
    end_pending_ = false;   // END sent: the guest's RESULT (or a WANT 0 retry) comes next
    phase_ = Phase::ended;
}

// ---- Receiver ---------------------------------------------------------------------------------------------------
bool MemoryPartStore::Open(const Sha256& s, std::uint32_t size, std::uint32_t& have) noexcept {
    if (!open || s != sha) bytes.clear();
    sha = s;
    open = true;
    if (bytes.size() > size) bytes.clear();
    have = static_cast<std::uint32_t>(bytes.size());
    return true;
}

bool MemoryPartStore::Append(std::uint32_t offset, const std::uint8_t* data, std::size_t len) noexcept {
    if (!open || fail_append || offset != bytes.size()) return false;
    try {
        bytes.insert(bytes.end(), data, data + len);
    } catch (...) {
        return false;
    }
    return true;
}

bool MemoryPartStore::ReadAll(std::vector<std::uint8_t>& out) noexcept {
    if (!open) return false;
    try {
        out = bytes;
    } catch (...) {
        return false;
    }
    return true;
}

void MemoryPartStore::Remove() noexcept {
    bytes.clear();
    open = false;
    ++removes;
}

const char* Receiver::PhaseName(Phase p) noexcept {
    switch (p) {
        case Phase::waiting: return "waiting";
        case Phase::receiving: return "receiving";
        case Phase::complete: return "complete";
        case Phase::loading: return "loading";
        case Phase::loaded: return "loaded";
        case Phase::refused: return "refused";
        case Phase::failed: return "failed";
    }
    return "?";
}

void Receiver::Queue(std::size_t n, const std::uint8_t* buf) noexcept {
    if (!n) return;
    try {
        out_.emplace_back(buf, buf + n);
    } catch (...) {
    }
}

void Receiver::SendWant() noexcept {
    Want w;
    w.next_offset = have_;
    w.sha = offer_.sha;
    std::uint8_t buf[kHeaderSize + kWantBody];
    Queue(EncodeWant(xfer_, w, buf, sizeof(buf)), buf);
}

void Receiver::SendResult(std::uint32_t xfer, ResultCode code) noexcept {
    Result r;
    r.ok = code == ResultCode::ok;
    r.code = code;
    std::uint8_t buf[kHeaderSize + kResultBody];
    Queue(EncodeResult(xfer, r, buf, sizeof(buf)), buf);
}

void Receiver::Drop() noexcept {
    store_.Remove();
    complete_.clear();
    have_ = 0;
    xfer_ = 0;
    retries_ = 0;
}

void Receiver::Fail(ResultCode code) noexcept {
    SendResult(xfer_, code);
    Drop();
    error_ = code;
    phase_ = Phase::failed;   // the guest stays on its waiting screen until the next offer
}

void Receiver::Reset() noexcept {
    if (xfer_) store_.Remove();
    complete_.clear();
    out_.clear();
    have_ = 0;
    xfer_ = 0;
    loading_xfer_ = 0;
    loading_offer_ = st::stx::Offer{};
    retries_ = 0;
    phase_ = Phase::waiting;
    error_ = ResultCode::ok;
    have_state_ = false;
    version_mismatch_ = false;
    loaded_ = false;
    loaded_counter_ = 0;
    loaded_sha_ = Sha256{};
}

void Receiver::OnMessage(const Message& m) noexcept {
    switch (m.h.kind) {
        case Kind::state:
            state_ = m.state;
            have_state_ = true;
            version_mismatch_ = have_local_ && !SameVersion(m.state.version, local_);
            if (version_mismatch_ && phase_ != Phase::receiving && phase_ != Phase::loading) {
                phase_ = Phase::refused;
                error_ = ResultCode::version;
            }
            return;
        case Kind::offer: {
            const st::stx::Offer& o = m.offer;
            if (!have_local_ || !SameVersion(o.version, local_)) {
                // Never stored: the two games' saves are not interchangeable.
                version_mismatch_ = true;
                SendResult(m.h.xfer_id, ResultCode::version);
                if (phase_ != Phase::loading && phase_ != Phase::receiving) {
                    phase_ = Phase::refused;
                    error_ = ResultCode::version;
                }
                return;
            }
            if (xfer_ && m.h.xfer_id == xfer_ && o.sha == offer_.sha) {
                // The running transfer offered again (a new link session): carry on where it stopped.
                if (phase_ == Phase::receiving) {
                    ++resumes;
                    SendWant();
                }
                return;
            }
            if (loaded_ && o.sha == loaded_sha_ && o.load_counter == loaded_counter_ && phase_ != Phase::receiving) {
                SendResult(m.h.xfer_id, ResultCode::ok);   // already this world
                return;
            }
            if (xfer_ && o.load_counter < offer_.load_counter) {
                ++stale;   // an older offer: only the newest is ever loaded
                return;
            }
            // New (or superseding) transfer. The same file may keep its bytes (resume); another one starts over.
            if (xfer_ && o.sha != offer_.sha) store_.Remove();
            complete_.clear();
            xfer_ = m.h.xfer_id;
            offer_ = o;
            retries_ = 0;
            error_ = ResultCode::ok;
            std::uint32_t have = 0;
            if (!store_.Open(o.sha, o.size, have)) {
                Fail(ResultCode::disk);
                return;
            }
            if (have > o.size) {
                store_.Remove();
                if (!store_.Open(o.sha, o.size, have) || have > o.size) {
                    Fail(ResultCode::disk);
                    return;
                }
            }
            if (have) ++resumes;
            have_ = have;
            phase_ = Phase::receiving;
            SendWant();
            return;
        }
        case Kind::chunk:
            if (!xfer_ || m.h.xfer_id != xfer_ || phase_ != Phase::receiving) {
                ++stale;
                return;
            }
            if (m.chunk.offset != have_) {
                ++dropped_chunks;   // sent before our WANT reached the host: the stream restarts at have_
                return;
            }
            if (static_cast<std::uint64_t>(m.chunk.offset) + m.chunk.len > offer_.size) {
                std::uint8_t buf[kHeaderSize + kAbortBody];
                Queue(EncodeAbort(xfer_, AbortCode::protocol, buf, sizeof(buf)), buf);
                Drop();
                error_ = ResultCode::parse;
                phase_ = Phase::failed;
                return;
            }
            if (!store_.Append(m.chunk.offset, m.chunk.data, m.chunk.len)) {
                Fail(ResultCode::disk);
                return;
            }
            have_ += m.chunk.len;
            return;
        case Kind::end: {
            if (!xfer_ || m.h.xfer_id != xfer_ || phase_ != Phase::receiving) {
                ++stale;
                return;
            }
            if (m.end.size != offer_.size || m.end.sha != offer_.sha) {
                std::uint8_t buf[kHeaderSize + kAbortBody];
                Queue(EncodeAbort(xfer_, AbortCode::protocol, buf, sizeof(buf)), buf);
                Drop();
                error_ = ResultCode::parse;
                phase_ = Phase::failed;
                return;
            }
            std::vector<std::uint8_t> bytes;
            const bool whole = have_ == offer_.size && store_.ReadAll(bytes) && bytes.size() == offer_.size;
            if (!whole || Sha256Of(bytes.data(), bytes.size()) != offer_.sha) {
                // Delete and ask again from the start, at most kMaxShaRetries times.
                ++sha_retries;
                store_.Remove();
                if (++retries_ > kMaxShaRetries) {
                    Fail(ResultCode::sha_mismatch);
                    return;
                }
                std::uint32_t have = 0;
                if (!store_.Open(offer_.sha, offer_.size, have)) {
                    Fail(ResultCode::disk);
                    return;
                }
                have_ = 0;
                if (have) {   // a store that kept bytes after Remove: never trust them
                    Fail(ResultCode::disk);
                    return;
                }
                SendWant();
                return;
            }
            if (ValidateCheckpoint(bytes.data(), bytes.size()) != CheckpointError::ok) {
                Fail(ResultCode::parse);
                return;
            }
            complete_ = std::move(bytes);
            phase_ = Phase::complete;
            return;
        }
        case Kind::abort:
            if (!xfer_ || m.h.xfer_id != xfer_) {
                ++stale;
                return;
            }
            if (phase_ == Phase::loading) return;   // the game is loading it already; its RESULT follows
            Drop();
            phase_ = Phase::waiting;
            return;
        default: return;
    }
}

void Receiver::NewSession() noexcept {
    // A load the game is running keeps its RESULT (sent on the new session); the save already loaded stays (GSTATE says
    // which world the guest is in). Everything about the running transfer is forgotten except the part's bytes on disk:
    // the same file offered again resumes (the store keeps a part of the same SHA), any other offer is taken whatever its
    // load_counter (a restarted host counts from 1 again).
    xfer_ = 0;
    have_ = 0;
    retries_ = 0;
    complete_.clear();
    out_.clear();
    have_state_ = false;
    version_mismatch_ = false;
    error_ = ResultCode::ok;
    if (loading_xfer_) phase_ = Phase::loading;
    else if (phase_ != Phase::loaded) phase_ = Phase::waiting;
}

bool Receiver::TakeComplete(std::vector<std::uint8_t>& data, st::stx::Offer& offer) noexcept {
    // One load at a time: a save that arrived while the game loads the previous one waits for that load's LoadDone (its
    // RESULT must name the transfer that was loaded, never the newer one).
    if (phase_ != Phase::complete || loading_xfer_) return false;
    data = std::move(complete_);
    complete_.clear();
    offer = offer_;
    loading_offer_ = offer_;
    loading_xfer_ = xfer_;
    store_.Remove();   // the bytes are with the game side now (it writes the slot)
    phase_ = Phase::loading;
    return true;
}

void Receiver::LoadDone(ResultCode code) noexcept {
    if (!loading_xfer_) return;
    SendResult(loading_xfer_, code);
    // A newer offer may have started while the game loaded this one: it keeps running.
    const bool newer = xfer_ != 0 && xfer_ != loading_xfer_;   // (0: forgotten by NewSession, nothing newer runs)
    if (code == ResultCode::ok) {
        loaded_ = true;
        loaded_counter_ = loading_offer_.load_counter;
        loaded_sha_ = loading_offer_.sha;
    } else {
        error_ = code;
    }
    if (!newer) {
        phase_ = code == ResultCode::ok ? Phase::loaded : Phase::failed;
        xfer_ = 0;
        have_ = 0;
    }
    loading_xfer_ = 0;
}

std::size_t Receiver::Build(std::uint8_t* out, std::size_t cap) const noexcept {
    if (out_.empty() || out_.front().size() > cap) return 0;
    std::memcpy(out, out_.front().data(), out_.front().size());
    return out_.front().size();
}

void Receiver::Advance() noexcept {
    if (!out_.empty()) out_.pop_front();
}

GState Receiver::Status() const noexcept {
    GState g;
    switch (phase_) {
        case Phase::receiving: g.phase = GuestPhase::receiving; break;
        case Phase::complete:
        case Phase::loading: g.phase = GuestPhase::loading; break;
        case Phase::loaded: g.phase = GuestPhase::world; break;
        case Phase::refused: g.phase = GuestPhase::refused; break;
        default: g.phase = GuestPhase::waiting; break;
    }
    g.loaded_load_counter = loaded_ ? loaded_counter_ : 0;
    g.loaded_sha = loaded_ ? loaded_sha_ : Sha256{};
    g.have_bytes = phase_ == Phase::receiving ? have_ : 0;
    g.refuse_reason = phase_ == Phase::refused || phase_ == Phase::failed ? static_cast<std::uint16_t>(error_) : 0;
    return g;
}

// ---- guest paths --------------------------------------------------------------------------------------------------
namespace {

std::vector<std::wstring> SplitRel(const std::wstring& rel) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : rel) {
        if (c == L'\\') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

bool AllDigitsW(const std::wstring& s, std::size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (wchar_t c : s)
        if (c < L'0' || c > L'9') return false;
    return true;
}

}  // namespace

GuestPath ClassifyGuestPath(const std::wstring& rel) noexcept {
    try {
        if (rel.empty() || rel.size() > 200) return GuestPath::invalid;
        for (wchar_t c : rel)
            if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'.' || c == L'\\'))
                return GuestPath::invalid;
        const std::vector<std::wstring> p = SplitRel(rel);
        if (p.size() == 2 && p[0] == L"incoming") {
            const std::wstring& f = p[1];
            if (f.size() != 64 + 5 || f.compare(64, 5, L".part") != 0) return GuestPath::invalid;
            for (std::size_t i = 0; i < 64; ++i)
                if (!((f[i] >= L'0' && f[i] <= L'9') || (f[i] >= L'a' && f[i] <= L'f'))) return GuestPath::invalid;
            return GuestPath::incoming_part;
        }
        if (p.size() == 4 && p[0] == L"DeathStrandingDC" && AllDigitsW(p[1], 10) && p[1] != L"0" && p[1][0] != L'0' && p[3] == L"checkpoint.dat") {
            const std::wstring& s = p[2];
            if (s.size() > 10 && s.compare(0, 10, L"manualsave") == 0) {
                const std::wstring n = s.substr(10);
                if (AllDigitsW(n, 3) && !(n.size() > 1 && n[0] == L'0')) return GuestPath::manual_slot;
            }
        }
        return GuestPath::invalid;
    } catch (...) {
        return GuestPath::invalid;
    }
}

std::wstring IncomingPartRel(const Sha256& sha) {
    const std::string hex = ShaHex(sha);
    return L"incoming\\" + std::wstring(hex.begin(), hex.end()) + L".part";
}

std::wstring ManualSlotRel(std::uint32_t account, int slot) {
    if (!account || slot < 0 || slot > 999) return {};
    return L"DeathStrandingDC\\" + std::to_wstring(account) + L"\\manualsave" + std::to_wstring(slot) + L"\\checkpoint.dat";
}

}  // namespace st::stx
