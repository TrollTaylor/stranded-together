#include "game_font.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace st::gamefont {

const std::uint8_t kMenuFontGuid[16] = {0xA0, 0x69, 0xF8, 0x94, 0x23, 0xDB, 0x46, 0x3D,
                                        0xA6, 0x9A, 0xC9, 0x20, 0x36, 0x32, 0x77, 0x56};

namespace {

constexpr std::uint32_t kSeed = 42;
constexpr std::uint32_t kSalt1[4] = {0x0FA3A9443u, 0x0F41CAB62u, 0x0F376811Cu, 0x0D2A89E3Eu};
constexpr std::uint32_t kSalt2[4] = {0x06C084A37u, 0x07E159D95u, 0x03D5AF7E8u, 0x018AA7D3Fu};
constexpr std::uint32_t kMagicPlain = 0x20304050u, kMagicEnc = 0x21304050u;
// The archive that holds the menu font in the Director's Cut (searched first; then all of them).
constexpr const wchar_t* kMenuFontArchive = L"7017f9bb9d52fc1c4433599203cc51b1.bin";

// Limits (a corrupt or unexpected archive is refused, never trusted).
constexpr std::uint64_t kMaxEntries = 4u << 20, kMaxChunks = 4u << 20;
constexpr std::uint32_t kMaxChunkBytes = 64u << 20;
constexpr std::uint32_t kMaxCoreBytes = 16u << 20;

void Why(char* why, std::size_t cap, const char* fmt, ...) {
    if (!why || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}

inline std::uint64_t Rotl(std::uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline std::uint64_t Fmix(std::uint64_t k) {
    k ^= k >> 33;
    k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 33;
    k *= 0xC4CEB9FE1A85EC53ull;
    k ^= k >> 33;
    return k;
}

template <class T>
T Get(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// 16-byte table mask of one key word.
void Mask(std::uint32_t key, std::uint8_t out[16]) {
    const std::uint32_t in[4] = {key, kSalt1[1], kSalt1[2], kSalt1[3]};
    std::uint64_t h[2];
    Murmur3x64_128(in, sizeof(in), kSeed, h);
    std::memcpy(out, h, 16);
}

void XorRow(std::uint8_t* row, std::uint32_t key0, std::uint32_t key1) {
    std::uint8_t m[32];
    Mask(key0, m);
    Mask(key1, m + 16);
    for (int i = 0; i < 32; ++i) row[i] ^= m[i];
}

struct Md5 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    ~Md5() {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    bool Open() { return alg || BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0)); }
    bool Hash(const std::uint8_t* data, ULONG n, std::uint8_t out[16]) {
        BCRYPT_HASH_HANDLE h = nullptr;
        if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0))) return false;
        const bool ok = BCRYPT_SUCCESS(BCryptHashData(h, const_cast<PUCHAR>(data), n, 0)) &&
                        BCRYPT_SUCCESS(BCryptFinishHash(h, out, 16, 0));
        BCryptDestroyHash(h);
        return ok;
    }
};

using OodleDecompressFn = std::intptr_t(__stdcall*)(const void* src, std::intptr_t src_len, void* dst, std::intptr_t dst_len, int fuzz,
                                                     int crc, int verbosity, void* dec_base, std::intptr_t dec_size, void* cb,
                                                     void* cb_user, void* dec_mem, std::intptr_t dec_mem_size, int thread_phase);

// ds.exe links Oodle statically and never loads the oo2core DLL that ships in its folder: the reader loads that file
// (by full path, from the game folder only) for the read and frees it again (*loaded).
OodleDecompressFn FindOodle(const std::wstring& game_dir, bool allow_load, HMODULE* loaded) {
    static const wchar_t* const kNames[] = {L"oo2core_7_win64.dll", L"oo2core_8_win64.dll", L"oo2core_9_win64.dll",
                                            L"oo2core_6_win64.dll", L"oo2core_5_win64.dll"};
    HMODULE m = nullptr;
    for (const wchar_t* n : kNames)
        if ((m = GetModuleHandleW(n)) != nullptr) break;
    if (!m && allow_load) {
        for (const wchar_t* n : kNames) {
            const std::wstring p = game_dir + L"\\" + n;
            if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            if ((m = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) != nullptr) {
                *loaded = m;
                break;
            }
        }
    }
    return m ? reinterpret_cast<OodleDecompressFn>(reinterpret_cast<void*>(GetProcAddress(m, "OodleLZ_Decompress"))) : nullptr;
}

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~File() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    bool Open(const std::wstring& p) {
        h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        return h != INVALID_HANDLE_VALUE;
    }
    std::uint64_t Size() const {
        LARGE_INTEGER s{};
        return GetFileSizeEx(h, &s) ? static_cast<std::uint64_t>(s.QuadPart) : 0;
    }
    bool ReadAt(std::uint64_t off, void* dst, std::size_t n) const {
        auto* p = static_cast<std::uint8_t*>(dst);
        while (n) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(off);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(n, 1u << 24));
            DWORD got = 0;
            if (!ReadFile(h, p, want, &got, &ov) || got == 0) return false;
            p += got, off += got, n -= got;
        }
        return true;
    }
};

struct Chunk {
    std::uint64_t uoff = 0, coff = 0;
    std::uint32_t usize = 0, csize = 0;
    std::uint8_t key16[16] = {};
};

// One packfile: finds the entry of a path hash and reads its bytes.
class Archive {
public:
    bool Open(const std::wstring& path, char* why, std::size_t cap) {
        if (!f_.Open(path)) return Why(why, cap, "cannot open (error %lu)", GetLastError()), false;
        file_size_ = f_.Size();
        std::uint8_t hdr[40];
        if (file_size_ < 40 || !f_.ReadAt(0, hdr, 40)) return Why(why, cap, "header unreadable"), false;
        const std::uint32_t magic = Get<std::uint32_t>(hdr), key = Get<std::uint32_t>(hdr + 4);
        if (magic != kMagicPlain && magic != kMagicEnc) return Why(why, cap, "bad magic %08X", magic), false;
        enc_ = magic == kMagicEnc;
        std::uint8_t body[32];
        std::memcpy(body, hdr + 8, 32);
        if (enc_) XorRow(body, key, key + 1);
        entries_ = Get<std::uint64_t>(body + 16);
        chunks_ = Get<std::uint32_t>(body + 24);
        if (entries_ > kMaxEntries || chunks_ > kMaxChunks || 40 + 32 * (entries_ + chunks_) > file_size_)
            return Why(why, cap, "tables out of range (%llu entries, %llu chunks)", static_cast<unsigned long long>(entries_),
                       static_cast<unsigned long long>(chunks_)),
                   false;
        return true;
    }

    // 1 found, 0 not in this archive, -1 error.
    int Find(std::uint64_t hash, std::uint64_t* off, std::uint32_t* size, char* why, std::size_t cap) {
        std::vector<std::uint8_t> t(static_cast<std::size_t>(32 * entries_));
        if (!t.empty() && !f_.ReadAt(40, t.data(), t.size())) return Why(why, cap, "entry table unreadable"), -1;
        for (std::uint64_t i = 0; i < entries_; ++i) {
            std::uint8_t row[32];
            std::memcpy(row, &t[static_cast<std::size_t>(i * 32)], 32);
            if (enc_) {
                // The hash sits in the first 16 bytes (key0's mask): the second half only on a match.
                std::uint8_t m[16];
                Mask(Get<std::uint32_t>(row + 4), m);
                if ((Get<std::uint64_t>(row + 8) ^ Get<std::uint64_t>(m + 8)) != hash) continue;
                XorRow(row, Get<std::uint32_t>(row + 4), Get<std::uint32_t>(row + 28));
            } else if (Get<std::uint64_t>(row + 8) != hash) {
                continue;
            }
            *off = Get<std::uint64_t>(row + 16);
            *size = Get<std::uint32_t>(row + 24);
            return 1;
        }
        return 0;
    }

    bool Read(std::uint64_t off, std::uint32_t size, OodleDecompressFn oodle, Md5& md5, std::vector<std::uint8_t>* out, char* why,
              std::size_t cap) {
        if (!size || size > kMaxCoreBytes) return Why(why, cap, "entry size %u out of range", size), false;
        if (!LoadChunks(why, cap)) return false;
        // The first chunk at or before off (sorted by uncompressed offset).
        std::size_t lo = 0, hi = chunk_.size();
        while (hi - lo > 1) {
            const std::size_t mid = (lo + hi) / 2;
            if (chunk_[mid].uoff <= off) lo = mid;
            else hi = mid;
        }
        out->clear();
        out->reserve(size);
        const std::uint64_t end = off + size;
        std::vector<std::uint8_t> comp, plain;
        for (std::size_t i = lo; i < chunk_.size() && chunk_[i].uoff < end; ++i) {
            const Chunk& c = chunk_[i];
            if (c.uoff + c.usize <= off) continue;
            if (!c.usize || c.usize > kMaxChunkBytes || !c.csize || c.csize > kMaxChunkBytes || c.coff + c.csize > file_size_)
                return Why(why, cap, "chunk %zu out of range", i), false;
            comp.resize(c.csize);
            if (!f_.ReadAt(c.coff, comp.data(), comp.size())) return Why(why, cap, "chunk %zu unreadable", i), false;
            if (enc_) {
                std::uint64_t h[2];
                Murmur3x64_128(c.key16, 16, kSeed, h);
                std::uint32_t iv[4];
                std::memcpy(iv, h, 16);
                for (int k = 0; k < 4; ++k) iv[k] ^= kSalt2[k];
                std::uint8_t mask[16];
                if (!md5.Hash(reinterpret_cast<const std::uint8_t*>(iv), 16, mask)) return Why(why, cap, "MD5 failed"), false;
                for (std::size_t k = 0; k < comp.size(); ++k) comp[k] ^= mask[k & 15];
            }
            if (c.csize == c.usize) {
                plain.swap(comp);
            } else {
                plain.assign(static_cast<std::size_t>(c.usize) + 64, 0);
                const std::intptr_t got = oodle(comp.data(), static_cast<std::intptr_t>(comp.size()), plain.data(),
                                                static_cast<std::intptr_t>(c.usize), 1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
                if (got != static_cast<std::intptr_t>(c.usize))
                    return Why(why, cap, "Oodle gave %lld of %u bytes", static_cast<long long>(got), c.usize), false;
            }
            const std::uint64_t a = std::max(off, c.uoff) - c.uoff, b = std::min(end, c.uoff + c.usize) - c.uoff;
            out->insert(out->end(), plain.begin() + static_cast<std::ptrdiff_t>(a), plain.begin() + static_cast<std::ptrdiff_t>(b));
        }
        if (out->size() != size) return Why(why, cap, "short read %zu of %u", out->size(), size), false;
        return true;
    }

private:
    bool LoadChunks(char* why, std::size_t cap) {
        if (!chunk_.empty() || !chunks_) return !chunk_.empty() || (Why(why, cap, "no chunks"), false);
        std::vector<std::uint8_t> t(static_cast<std::size_t>(32 * chunks_));
        if (!f_.ReadAt(40 + 32 * entries_, t.data(), t.size())) return Why(why, cap, "chunk table unreadable"), false;
        chunk_.resize(static_cast<std::size_t>(chunks_));
        for (std::size_t i = 0; i < chunk_.size(); ++i) {
            std::uint8_t row[32];
            std::memcpy(row, &t[i * 32], 32);
            const std::uint32_t k0 = Get<std::uint32_t>(row + 12);
            if (enc_) XorRow(row, k0, Get<std::uint32_t>(row + 28));
            Chunk& c = chunk_[i];
            c.uoff = Get<std::uint64_t>(row);
            c.usize = Get<std::uint32_t>(row + 8);
            c.coff = Get<std::uint64_t>(row + 16);
            c.csize = Get<std::uint32_t>(row + 24);
            std::memcpy(c.key16, &c.uoff, 8);
            std::memcpy(c.key16 + 8, &c.usize, 4);
            std::memcpy(c.key16 + 12, &k0, 4);
            if (i && c.uoff < chunk_[i - 1].uoff) return Why(why, cap, "chunk table not sorted"), false;
        }
        return true;
    }

    File f_;
    bool enc_ = false;
    std::uint64_t file_size_ = 0, entries_ = 0, chunks_ = 0;
    std::vector<Chunk> chunk_;
};

// Bounded reader over one object body.
struct Cursor {
    const std::uint8_t* p;
    std::size_t n, pos = 0;
    bool ok = true;
    bool Need(std::size_t k) {
        if (!ok || k > n - pos) ok = false;
        return ok;
    }
    std::uint32_t U32() {
        if (!Need(4)) return 0;
        const std::uint32_t v = Get<std::uint32_t>(p + pos);
        pos += 4;
        return v;
    }
    float F32() {
        if (!Need(4)) return 0;
        const float v = Get<float>(p + pos);
        pos += 4;
        return v;
    }
    const std::uint8_t* Bytes(std::size_t k) {
        if (!Need(k)) return nullptr;
        const std::uint8_t* r = p + pos;
        pos += k;
        return r;
    }
};

bool Finite(float v) { return std::isfinite(v) && std::fabs(v) < 1e6f; }

}  // namespace

void Murmur3x64_128(const void* data, std::size_t n, std::uint32_t seed, std::uint64_t out[2]) noexcept {
    const auto* d = static_cast<const std::uint8_t*>(data);
    const std::uint64_t c1 = 0x87C37B91114253D5ull, c2 = 0x4CF5AD432745937Full;
    std::uint64_t h1 = seed, h2 = seed;
    const std::size_t nblocks = n / 16;
    for (std::size_t i = 0; i < nblocks; ++i) {
        std::uint64_t k1 = Get<std::uint64_t>(d + i * 16), k2 = Get<std::uint64_t>(d + i * 16 + 8);
        k1 *= c1, k1 = Rotl(k1, 31), k1 *= c2, h1 ^= k1;
        h1 = Rotl(h1, 27), h1 += h2, h1 = h1 * 5 + 0x52DCE729;
        k2 *= c2, k2 = Rotl(k2, 33), k2 *= c1, h2 ^= k2;
        h2 = Rotl(h2, 31), h2 += h1, h2 = h2 * 5 + 0x38495AB5;
    }
    const std::uint8_t* tail = d + nblocks * 16;
    const std::size_t t = n & 15;
    std::uint64_t k1 = 0, k2 = 0;
    for (std::size_t i = t; i > 8; --i) k2 = (k2 << 8) | tail[i - 1];
    if (t > 8) k2 *= c2, k2 = Rotl(k2, 33), k2 *= c1, h2 ^= k2;
    for (std::size_t i = std::min<std::size_t>(t, 8); i > 0; --i) k1 = (k1 << 8) | tail[i - 1];
    if (t > 0) k1 *= c1, k1 = Rotl(k1, 31), k1 *= c2, h1 ^= k1;
    h1 ^= n, h2 ^= n;
    h1 += h2, h2 += h1;
    h1 = Fmix(h1), h2 = Fmix(h2);
    h1 += h2, h2 += h1;
    out[0] = h1, out[1] = h2;
}

std::uint64_t PathHash(const char* path) noexcept {
    char buf[512];
    std::size_t n = 0;
    for (; path[n] && n + 1 < sizeof(buf); ++n) {
        char c = path[n] == '\\' ? '/' : path[n];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        buf[n] = c;
    }
    buf[n] = 0;
    std::uint64_t h[2];
    Murmur3x64_128(buf, n + 1, kSeed, h);  // the terminating NUL is hashed too
    return h[0];
}

const Glyph* Font::Find(std::uint32_t cp) const noexcept {
    const auto it = std::lower_bound(glyphs.begin(), glyphs.end(), cp, [](const Glyph& g, std::uint32_t c) { return g.cp < c; });
    return it != glyphs.end() && it->cp == cp ? &*it : nullptr;
}

bool ParseFontCore(const std::uint8_t* data, std::size_t n, const std::uint8_t* guid, Font* out, char* why, std::size_t cap) noexcept {
    try {
        *out = Font{};
        std::size_t pos = 0;
        while (pos + 12 <= n) {
            const std::uint64_t tid = Get<std::uint64_t>(data + pos);
            const std::uint32_t size = Get<std::uint32_t>(data + pos + 8);
            if (size > n - pos - 12) return Why(why, cap, "object at %zu overruns the file", pos), false;
            const std::uint8_t* body = data + pos + 12;
            pos += 12 + static_cast<std::size_t>(size);
            if (tid != kFontResourceTypeId) continue;
            Cursor c{body, size};
            const std::uint8_t* g = c.Bytes(16);
            if (!g || (guid && std::memcmp(g, guid, 16) != 0)) return Why(why, cap, "the FontResource has another ObjectUUID"), false;
            const std::uint32_t name_len = c.U32();
            if (name_len > 256) return Why(why, cap, "typeface name length %u", name_len), false;
            if (name_len) {
                c.U32();  // crc32c
                const std::uint8_t* s = c.Bytes(name_len);
                if (s) out->typeface.assign(reinterpret_cast<const char*>(s), name_len);
            }
            out->height = c.F32(), out->ascent = c.F32(), out->descent = c.F32(), out->em_height = c.F32();
            if (!c.ok || !Finite(out->height) || out->height <= 0 || !Finite(out->ascent) || !Finite(out->descent))
                return Why(why, cap, "bad metrics"), false;
            const std::uint32_t ng = c.U32();
            if (ng > 0x10000) return Why(why, cap, "%u glyphs", ng), false;
            out->glyphs.reserve(ng);
            for (std::uint32_t i = 0; i < ng && c.ok; ++i) {
                Glyph gl;
                gl.advance = c.F32();
                gl.cp = c.U32();
                c.Bytes(16);  // BoundingBox2 (recomputed from the points below)
                const std::uint32_t nc = c.U32();
                if (!c.ok || nc > 512 || !Finite(gl.advance)) return Why(why, cap, "glyph %u: %u contours", i, nc), false;
                gl.first_contour = static_cast<std::uint32_t>(out->contours.size());
                gl.n_contours = nc;
                gl.min_x = gl.min_y = 1e30f, gl.max_x = gl.max_y = -1e30f;
                for (std::uint32_t k = 0; k < nc; ++k) {
                    Contour ct;
                    ct.n_cmd = c.U32();
                    if (ct.n_cmd > 0x10000) return Why(why, cap, "glyph U+%04X: %u commands", gl.cp, ct.n_cmd), false;
                    const std::uint8_t* cmds = c.Bytes(ct.n_cmd);
                    ct.n_pt = c.U32();
                    if (!c.ok || ct.n_pt > 0x10000) return Why(why, cap, "glyph U+%04X: %u points", gl.cp, ct.n_pt), false;
                    const std::uint8_t* pts = c.Bytes(static_cast<std::size_t>(ct.n_pt) * 8);
                    if (!c.ok) break;
                    std::uint32_t used = 1;
                    for (std::uint32_t q = 0; q < ct.n_cmd; ++q) {
                        const std::uint32_t type = cmds[q] >> 6, kk = cmds[q] & 63u;
                        if (type > 1 || kk == 0) return Why(why, cap, "glyph U+%04X: command %02X", gl.cp, cmds[q]), false;
                        used += kk;
                    }
                    if (ct.n_pt == 0 || used != ct.n_pt)
                        return Why(why, cap, "glyph U+%04X: %u points for %u used", gl.cp, ct.n_pt, used), false;
                    ct.first_cmd = static_cast<std::uint32_t>(out->cmds.size());
                    ct.first_pt = static_cast<std::uint32_t>(out->pts.size() / 2);
                    out->cmds.insert(out->cmds.end(), cmds, cmds + ct.n_cmd);
                    for (std::uint32_t q = 0; q < ct.n_pt; ++q) {
                        const float x = Get<float>(pts + q * 8), y = Get<float>(pts + q * 8 + 4);
                        if (!Finite(x) || !Finite(y)) return Why(why, cap, "glyph U+%04X: bad point", gl.cp), false;
                        out->pts.push_back(x), out->pts.push_back(y);
                        gl.min_x = std::min(gl.min_x, x), gl.max_x = std::max(gl.max_x, x);
                        gl.min_y = std::min(gl.min_y, y), gl.max_y = std::max(gl.max_y, y);
                    }
                    out->contours.push_back(ct);
                }
                if (!nc) gl.min_x = gl.min_y = gl.max_x = gl.max_y = 0;
                if (out->pts.size() > (8u << 20)) return Why(why, cap, "too many points"), false;
                out->glyphs.push_back(gl);
            }
            const std::uint32_t nk = c.U32();
            if (!c.ok || nk > 0x100000 || !c.Bytes(static_cast<std::size_t>(nk) * 12))
                return Why(why, cap, "kerning table out of range"), false;
            out->kerning_pairs = nk;
            if (c.pos != size) return Why(why, cap, "the FontResource parse ended at %zu of %u bytes", c.pos, size), false;
            std::sort(out->glyphs.begin(), out->glyphs.end(), [](const Glyph& a, const Glyph& b) { return a.cp < b.cp; });
            for (std::size_t i = 1; i < out->glyphs.size(); ++i)
                if (out->glyphs[i].cp == out->glyphs[i - 1].cp) return Why(why, cap, "code point U+%04X twice", out->glyphs[i].cp), false;
            if (out->glyphs.empty()) return Why(why, cap, "no glyphs"), false;
            return true;
        }
        return Why(why, cap, "no FontResource object in the core file"), false;
    } catch (...) {
        *out = Font{};
        return Why(why, cap, "out of memory"), false;
    }
}

bool ReadCore(const std::wstring& game_dir, const char* path, const wchar_t* preferred_archive, bool allow_load,
              std::vector<std::uint8_t>* out, char* why, std::size_t cap) noexcept {
    try {
        struct Unload {
            HMODULE m = nullptr;
            ~Unload() {
                if (m) FreeLibrary(m);
            }
        } unload;
        const OodleDecompressFn oodle = FindOodle(game_dir, allow_load, &unload.m);
        if (!oodle) return Why(why, cap, "no OodleLZ_Decompress (the game folder's oo2core_*_win64.dll)"), false;
        Md5 md5;
        if (!md5.Open()) return Why(why, cap, "BCrypt MD5 unavailable"), false;
        const std::uint64_t hash = PathHash(path);
        const std::wstring data_dir = game_dir + L"\\data\\";
        std::vector<std::wstring> names;
        if (preferred_archive) names.push_back(preferred_archive);
        WIN32_FIND_DATAW fd;
        const HANDLE f = FindFirstFileW((data_dir + L"*.bin").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            std::vector<std::wstring> all;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) all.push_back(fd.cFileName);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
            std::sort(all.begin(), all.end());
            for (auto& a : all)
                if (!preferred_archive || _wcsicmp(a.c_str(), preferred_archive) != 0) names.push_back(a);
        }
        char last[160] = "no archives in the data folder";
        for (const std::wstring& name : names) {
            Archive ar;
            char w[160] = {};
            if (!ar.Open(data_dir + name, w, sizeof(w))) {
                std::snprintf(last, sizeof(last), "%ls: %s", name.c_str(), w);
                continue;
            }
            std::uint64_t off = 0;
            std::uint32_t size = 0;
            const int r = ar.Find(hash, &off, &size, w, sizeof(w));
            if (r < 0) std::snprintf(last, sizeof(last), "%ls: %s", name.c_str(), w);
            if (r <= 0) continue;
            if (ar.Read(off, size, oodle, md5, out, w, sizeof(w))) return true;
            return Why(why, cap, "%ls: %s", name.c_str(), w), false;
        }
        return Why(why, cap, "%s not found (%s)", path, last), false;
    } catch (...) {
        return Why(why, cap, "out of memory"), false;
    }
}

bool LoadMenuFont(const std::wstring& game_dir, bool allow_load, Font* out, char* why, std::size_t cap) noexcept {
    std::vector<std::uint8_t> core;
    if (!ReadCore(game_dir, kMenuFontPath, kMenuFontArchive, allow_load, &core, why, cap)) return false;
    const bool ok = ParseFontCore(core.data(), core.size(), kMenuFontGuid, out, why, cap);
    // The raw core bytes are dropped here: only the parsed outlines stay (in memory).
    std::fill(core.begin(), core.end(), std::uint8_t{0});
    return ok;
}

}  // namespace st::gamefont
