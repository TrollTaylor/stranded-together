#include "texture_wire.hpp"

#include <cstring>

namespace st::twire {
namespace {

class W {
public:
    W(std::uint8_t* out, std::size_t cap) : p_(out), cap_(cap) {}
    void U8(std::uint8_t v) {
        if (n_ + 1 <= cap_) p_[n_] = v;
        n_ += 1;
    }
    void U16(std::uint16_t v) {
        U8(static_cast<std::uint8_t>(v));
        U8(static_cast<std::uint8_t>(v >> 8));
    }
    void U32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) U8(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void Bytes(const std::uint8_t* b, std::size_t k) {
        if (n_ + k <= cap_ && k) std::memcpy(p_ + n_, b, k);
        n_ += k;
    }
    void Head(Kind k) {
        Bytes(reinterpret_cast<const std::uint8_t*>("STT1"), 4);
        U8(kVersion);
        U8(static_cast<std::uint8_t>(k));
    }
    std::size_t Done() const { return n_ <= cap_ && n_ <= kMaxMessage ? n_ : 0; }

private:
    std::uint8_t* p_;
    std::size_t cap_, n_ = 0;
};

class R {
public:
    R(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}
    bool U8(std::uint8_t& v) {
        if (i_ + 1 > n_) return ok_ = false;
        v = p_[i_++];
        return true;
    }
    bool U16(std::uint16_t& v) {
        std::uint8_t a = 0, b = 0;
        if (!U8(a) || !U8(b)) return false;
        v = static_cast<std::uint16_t>(a | (b << 8));
        return true;
    }
    bool U32(std::uint32_t& v) {
        v = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint8_t b = 0;
            if (!U8(b)) return false;
            v |= static_cast<std::uint32_t>(b) << (8 * i);
        }
        return true;
    }
    bool Bytes(std::uint8_t* out, std::size_t k) {
        if (i_ + k > n_) return ok_ = false;
        std::memcpy(out, p_ + i_, k);
        i_ += k;
        return true;
    }
    const std::uint8_t* Here() const { return p_ + i_; }
    bool Skip(std::size_t k) {
        if (i_ + k > n_) return ok_ = false;
        i_ += k;
        return true;
    }
    bool End() const { return ok_ && i_ == n_; }

private:
    const std::uint8_t* p_;
    std::size_t n_, i_ = 0;
    bool ok_ = true;
};

bool Head(R& r, const std::uint8_t* p, std::size_t n, Kind want) {
    return n >= 6 && n <= kMaxMessage && PeekKind(p, n) == want && r.Skip(6);
}

}  // namespace

const char* CodeName(std::uint8_t code) {
    switch (code) {
    case kOk: return "ok";
    case kHashMismatch: return "hash mismatch";
    case kInvalid: return "invalid blob";
    case kTooLarge: return "too large";
    case kRefused: return "refused by setting";
    case kUnknownSlot: return "unknown slot or table";
    case kBudget: return "budget";
    default: return "?";
    }
}

Kind PeekKind(const std::uint8_t* p, std::size_t n) {
    if (!p || n < 6 || n > kMaxMessage || std::memcmp(p, "STT1", 4) != 0 || p[4] != kVersion) return Kind::none;
    if (p[5] < 1 || p[5] > 7) return Kind::none;
    return static_cast<Kind>(p[5]);
}

std::size_t EncodeLook(const Look& m, std::uint8_t* out, std::size_t cap) {
    if (m.n > kMaxEntries) return 0;
    W w(out, cap);
    w.Head(Kind::look);
    w.U32(m.epoch);
    w.U32(m.table);
    w.U8(m.flags);
    w.U8(m.colour);
    w.U8(m.boots);
    w.U8(m.worn);
    w.U8(m.n);
    for (unsigned i = 0; i < m.n; ++i) {
        if (m.e[i].size == 0 || m.e[i].size > kMaxBlob) return 0;
        w.U8(m.e[i].slot);
        w.Bytes(m.e[i].id, 32);
        w.U32(m.e[i].size);
    }
    return w.Done();
}

bool DecodeLook(const std::uint8_t* p, std::size_t n, Look& out) {
    R r(p, n);
    out = Look{};
    if (!Head(r, p, n, Kind::look)) return false;
    if (!r.U32(out.epoch) || !r.U32(out.table) || !r.U8(out.flags) || !r.U8(out.colour) || !r.U8(out.boots) || !r.U8(out.worn) || !r.U8(out.n)) return false;
    if (out.n > kMaxEntries || (out.flags & ~3u)) return false;
    for (unsigned i = 0; i < out.n; ++i) {
        if (!r.U8(out.e[i].slot) || !r.Bytes(out.e[i].id, 32) || !r.U32(out.e[i].size)) return false;
        if (out.e[i].size == 0 || out.e[i].size > kMaxBlob) return false;
        for (unsigned j = 0; j < i; ++j)
            if (out.e[j].slot == out.e[i].slot) return false;  // one blob per slot
    }
    return r.End();
}

std::size_t EncodeIds(Kind kind, const IdList& m, std::uint8_t* out, std::size_t cap) {
    if ((kind != Kind::have && kind != Kind::want) || m.n > kMaxEntries) return 0;
    W w(out, cap);
    w.Head(kind);
    w.U32(m.epoch);
    w.U8(m.n);
    for (unsigned i = 0; i < m.n; ++i) {
        w.Bytes(m.id[i], 32);
        if (kind == Kind::want) w.U32(m.offset[i]);
    }
    return w.Done();
}

bool DecodeIds(const std::uint8_t* p, std::size_t n, Kind kind, IdList& out) {
    R r(p, n);
    out = IdList{};
    if ((kind != Kind::have && kind != Kind::want) || !Head(r, p, n, kind)) return false;
    if (!r.U32(out.epoch) || !r.U8(out.n) || out.n > kMaxEntries) return false;
    for (unsigned i = 0; i < out.n; ++i) {
        if (!r.Bytes(out.id[i], 32)) return false;
        if (kind == Kind::want && (!r.U32(out.offset[i]) || out.offset[i] >= kMaxBlob)) return false;
    }
    return r.End();
}

std::size_t EncodeChunk(const Chunk& m, std::uint8_t* out, std::size_t cap) {
    if (m.len == 0 || m.len > kChunkData || !m.data || m.offset >= kMaxBlob) return 0;
    W w(out, cap);
    w.Head(Kind::chunk);
    w.Bytes(m.prefix, 8);
    w.U32(m.offset);
    w.U16(m.len);
    w.Bytes(m.data, m.len);
    return w.Done();
}

bool DecodeChunk(const std::uint8_t* p, std::size_t n, Chunk& out) {
    R r(p, n);
    out = Chunk{};
    if (!Head(r, p, n, Kind::chunk)) return false;
    if (!r.Bytes(out.prefix, 8) || !r.U32(out.offset) || !r.U16(out.len)) return false;
    if (out.len == 0 || out.len > kChunkData || out.offset >= kMaxBlob) return false;
    out.data = r.Here();
    if (!r.Skip(out.len)) return false;
    return r.End();
}

std::size_t EncodeResult(const Result& m, std::uint8_t* out, std::size_t cap) {
    if (m.code > kBudget) return 0;
    W w(out, cap);
    w.Head(Kind::result);
    w.Bytes(m.id, 32);
    w.U8(m.code);
    return w.Done();
}

bool DecodeResult(const std::uint8_t* p, std::size_t n, Result& out) {
    R r(p, n);
    out = Result{};
    if (!Head(r, p, n, Kind::result)) return false;
    if (!r.Bytes(out.id, 32) || !r.U8(out.code) || out.code > kBudget) return false;
    return r.End();
}

std::size_t EncodeCancel(std::uint32_t epoch, std::uint8_t* out, std::size_t cap) {
    W w(out, cap);
    w.Head(Kind::cancel);
    w.U32(epoch);
    return w.Done();
}

bool DecodeCancel(const std::uint8_t* p, std::size_t n, std::uint32_t& epoch) {
    R r(p, n);
    epoch = 0;
    if (!Head(r, p, n, Kind::cancel)) return false;
    return r.U32(epoch) && r.End();
}

bool Assembly::Init(std::uint32_t size) {
    size_ = 0;
    received_ = 0;
    have_ = 0;
    bytes_.clear();
    pieces_.clear();
    if (size == 0 || size > kMaxBlob) return false;
    size_ = size;
    pieces_.assign((size + kChunkData - 1) / kChunkData, 0);
    return true;
}

bool Assembly::Add(std::uint32_t offset, const std::uint8_t* data, std::size_t len) {
    if (!size_ || !data || offset % kChunkData != 0 || offset >= size_) return false;
    const std::size_t idx = offset / kChunkData;
    const std::size_t want = (size_ - offset) < kChunkData ? (size_ - offset) : kChunkData;
    if (len != want || idx >= pieces_.size() || pieces_[idx]) return false;
    if (bytes_.size() != size_) bytes_.assign(size_, 0);  // allocated with the first piece (a LOOK alone costs nothing)
    std::memcpy(bytes_.data() + offset, data, len);
    pieces_[idx] = 1;
    ++have_;
    received_ += static_cast<std::uint32_t>(len);
    return true;
}

std::uint32_t Assembly::ResumeOffset() const {
    for (std::size_t i = 0; i < pieces_.size(); ++i)
        if (!pieces_[i]) return static_cast<std::uint32_t>(i * kChunkData);
    return size_;
}

std::vector<std::uint8_t> Assembly::Take() {
    std::vector<std::uint8_t> out;
    out.swap(bytes_);
    size_ = 0;
    pieces_.clear();
    have_ = 0;
    received_ = 0;
    return out;
}

bool Bucket::Take(double now_ms, double rate, double burst, std::size_t bytes) {
    if (last_ < 0) last_ = now_ms;
    const double dt = now_ms > last_ ? now_ms - last_ : 0;
    last_ = now_ms;
    tokens_ += dt * rate / 1000.0;
    if (tokens_ > burst) tokens_ = burst;
    if (tokens_ < static_cast<double>(bytes)) return false;
    tokens_ -= static_cast<double>(bytes);
    return true;
}

namespace {
bool RecipeOk(const Recipe& m) {
    if (m.n < 1 || m.n > kMaxEntries || m.generator == 0) return false;
    for (unsigned i = 0; i < m.n; ++i) {
        const RecipeEntry& e = m.e[i];
        if (e.colour < 1 || e.colour > 16 || e.variant >= kMetalVariants) return false;
        for (unsigned k = 0; k < i; ++k)
            if (m.e[k].slot == e.slot) return false;
    }
    return true;
}
}  // namespace

std::size_t EncodeRecipe(const Recipe& m, std::uint8_t* out, std::size_t cap) {
    if (!RecipeOk(m)) return 0;
    W w(out, cap);
    w.Head(Kind::recipe);
    w.U32(m.epoch);
    w.U8(m.generator);
    w.U16(m.encoder);
    w.Bytes(m.originals, 32);
    w.U8(m.n);
    for (unsigned i = 0; i < m.n; ++i) {
        w.U8(m.e[i].slot);
        w.U8(m.e[i].colour);
        w.U8(m.e[i].variant);
        w.Bytes(m.e[i].id, 32);
    }
    return w.Done();
}

bool DecodeRecipe(const std::uint8_t* p, std::size_t n, Recipe& m) {
    m = Recipe{};
    R r(p, n);
    if (!Head(r, p, n, Kind::recipe)) return false;
    if (!r.U32(m.epoch) || !r.U8(m.generator) || !r.U16(m.encoder) || !r.Bytes(m.originals, 32) || !r.U8(m.n)) return false;
    if (m.n < 1 || m.n > kMaxEntries) return false;
    for (unsigned i = 0; i < m.n; ++i)
        if (!r.U8(m.e[i].slot) || !r.U8(m.e[i].colour) || !r.U8(m.e[i].variant) || !r.Bytes(m.e[i].id, 32)) return false;
    return r.End() && RecipeOk(m);
}

}  // namespace st::twire
