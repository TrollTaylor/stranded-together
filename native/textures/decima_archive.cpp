// Stranded Together textures: the read-only Decima packfile reader (decima_archive.hpp).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "decima_archive.hpp"
#include "tex_hash.hpp"

namespace st::tex {
namespace {

constexpr std::uint32_t kSeed = 42;
constexpr std::uint32_t kSalt1[4] = {0x0FA3A9443u, 0x0F41CAB62u, 0x0F376811Cu, 0x0D2A89E3Eu};
constexpr std::uint32_t kSalt2[4] = {0x06C084A37u, 0x07E159D95u, 0x03D5AF7E8u, 0x018AA7D3Fu};
constexpr std::uint32_t kMagicPlain = 0x20304050u;
constexpr std::uint32_t kMagicEnc = 0x21304050u;

inline std::uint64_t Rotl(std::uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline std::uint64_t Fmix(std::uint64_t k) {
    k ^= k >> 33;
    k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 33;
    k *= 0xC4CEB9FE1A85EC53ull;
    k ^= k >> 33;
    return k;
}
inline std::uint64_t Ld64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
inline std::uint32_t Ld32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

}  // namespace

void Murmur3x64_128(const void* data, std::size_t size, std::uint32_t seed, std::uint64_t out[2]) {
    const auto* d = static_cast<const std::uint8_t*>(data);
    const std::uint64_t c1 = 0x87C37B91114253D5ull, c2 = 0x4CF5AD432745937Full;
    std::uint64_t h1 = seed, h2 = seed;
    const std::size_t nblocks = size / 16;
    for (std::size_t i = 0; i < nblocks; ++i) {
        std::uint64_t k1 = Ld64(d + i * 16), k2 = Ld64(d + i * 16 + 8);
        k1 *= c1;
        k1 = Rotl(k1, 31);
        k1 *= c2;
        h1 ^= k1;
        h1 = Rotl(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52DCE729;
        k2 *= c2;
        k2 = Rotl(k2, 33);
        k2 *= c1;
        h2 ^= k2;
        h2 = Rotl(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495AB5;
    }
    const std::uint8_t* tail = d + nblocks * 16;
    const std::size_t t = size & 15;
    std::uint64_t k1 = 0, k2 = 0;
    for (std::size_t i = t; i > 8; --i) k2 = (k2 << 8) | tail[i - 1];
    if (t > 8) {
        k2 *= c2;
        k2 = Rotl(k2, 33);
        k2 *= c1;
        h2 ^= k2;
    }
    for (std::size_t i = std::min<std::size_t>(t, 8); i > 0; --i) k1 = (k1 << 8) | tail[i - 1];
    if (t > 0) {
        k1 *= c1;
        k1 = Rotl(k1, 31);
        k1 *= c2;
        h1 ^= k1;
    }
    h1 ^= size;
    h2 ^= size;
    h1 += h2;
    h2 += h1;
    h1 = Fmix(h1);
    h2 = Fmix(h2);
    h1 += h2;
    h2 += h1;
    out[0] = h1;
    out[1] = h2;
}

std::string NormalizeCorePath(const std::string& path) {
    std::string p;
    p.reserve(path.size() + 5);
    for (char c : path) p += c == '\\' ? '/' : static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    auto ends = [&](const char* s) {
        const std::size_t n = std::strlen(s);
        return p.size() >= n && p.compare(p.size() - n, n, s) == 0;
    };
    if (!ends(".core") && !ends(".stream")) p += ".core";
    return p;
}

std::uint64_t CorePathHash(const std::string& path) {
    std::string p = NormalizeCorePath(path);
    p.push_back('\0');
    std::uint64_t h[2];
    Murmur3x64_128(p.data(), p.size(), kSeed, h);
    return h[0];
}

void TableMask(std::uint32_t key, std::uint8_t out[16]) {
    const std::uint32_t in[4] = {key, kSalt1[1], kSalt1[2], kSalt1[3]};
    std::uint64_t h[2];
    Murmur3x64_128(in, sizeof(in), kSeed, h);
    std::memcpy(out, h, 16);
}

void DecryptTableRow(std::uint8_t* row, std::uint32_t key0, std::uint32_t key1) {
    std::uint8_t m0[16], m1[16];
    TableMask(key0, m0);
    TableMask(key1, m1);
    for (int i = 0; i < 16; ++i) row[i] ^= m0[i];
    for (int i = 0; i < 16; ++i) row[16 + i] ^= m1[i];
}

bool ChunkMask(const std::uint8_t key16[16], std::uint8_t out[16]) {
    std::uint64_t h[2];
    Murmur3x64_128(key16, 16, kSeed, h);
    std::uint8_t iv[16];
    std::memcpy(iv, h, 16);
    for (int i = 0; i < 4; ++i) {
        std::uint32_t w;
        std::memcpy(&w, iv + i * 4, 4);
        w ^= kSalt2[i];
        std::memcpy(iv + i * 4, &w, 4);
    }
    Md5 md{};
    if (!HashMd5(iv, 16, md)) return false;
    std::memcpy(out, md.data(), 16);
    return true;
}

std::uint32_t ArchiveOpenAccess() { return GENERIC_READ; }
std::uint32_t ArchiveShareMode() { return FILE_SHARE_READ | FILE_SHARE_WRITE; }

// ---- Archive -------------------------------------------------------------------------------------------------
Archive::~Archive() {
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
}

bool Archive::ReadRaw(std::uint64_t offset, void* dst, std::uint32_t size) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    return ReadFile(static_cast<HANDLE>(file_), dst, size, &got, &ov) && got == size;
}

bool Archive::Open(const std::wstring& path, std::string& error) {
    path_ = path;
    HANDLE f = CreateFileW(path.c_str(), ArchiveOpenAccess(), ArchiveShareMode(), nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = "cannot open (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    file_ = f;
    std::uint8_t hdr[40];
    if (!ReadRaw(0, hdr, 40)) return error = "short header", false;
    const std::uint32_t magic = Ld32(hdr), key = Ld32(hdr + 4);
    if (magic != kMagicPlain && magic != kMagicEnc) return error = "not a packfile (magic)", false;
    encrypted_ = magic == kMagicEnc;
    std::uint8_t body[32];
    std::memcpy(body, hdr + 8, 32);
    if (encrypted_) DecryptTableRow(body, key, key + 1);
    entry_count_ = Ld64(body + 16);
    chunk_count_ = Ld32(body + 24);
    LARGE_INTEGER fsize{};
    GetFileSizeEx(f, &fsize);
    const std::uint64_t tables = 40 + (entry_count_ + chunk_count_) * 32ull;
    if (entry_count_ > 10000000ull || chunk_count_ > 10000000u || tables > static_cast<std::uint64_t>(fsize.QuadPart))
        return error = "table sizes do not fit the file", false;
    etab_.resize(static_cast<std::size_t>(entry_count_ * 32));
    if (!etab_.empty() && !ReadRaw(40, etab_.data(), static_cast<std::uint32_t>(etab_.size()))) return error = "short entry table", false;
    return true;
}

void Archive::Find(const std::set<std::uint64_t>& wanted, std::vector<ArchiveEntry>& found) const {
    std::uint8_t row[32];
    for (std::uint64_t i = 0; i < entry_count_; ++i) {
        std::memcpy(row, etab_.data() + i * 32, 32);
        if (encrypted_) DecryptTableRow(row, Ld32(row + 4), Ld32(row + 28));
        const std::uint64_t h = Ld64(row + 8);
        if (!wanted.count(h)) continue;
        ArchiveEntry e;
        e.hash = h;
        e.offset = Ld64(row + 16);
        e.size = Ld32(row + 24);
        found.push_back(e);
    }
}

bool Archive::LoadChunks(std::string& error) {
    if (!chunks_.empty() || chunk_count_ == 0) return true;
    ctab_.resize(static_cast<std::size_t>(chunk_count_) * 32);
    if (!ReadRaw(40 + entry_count_ * 32, ctab_.data(), static_cast<std::uint32_t>(ctab_.size()))) return error = "short chunk table", false;
    chunks_.resize(chunk_count_);
    std::uint8_t row[32];
    for (std::uint32_t i = 0; i < chunk_count_; ++i) {
        const std::uint8_t* raw = ctab_.data() + static_cast<std::size_t>(i) * 32;
        std::memcpy(row, raw, 32);
        const std::uint32_t k0 = Ld32(raw + 12);
        if (encrypted_) DecryptTableRow(row, k0, Ld32(raw + 28));
        Chunk& c = chunks_[i];
        c.uoff = Ld64(row);
        c.usize = Ld32(row + 8);
        c.coff = Ld64(row + 16);
        c.csize = Ld32(row + 24);
        std::memcpy(c.key16, row, 12);
        std::memcpy(c.key16 + 12, &k0, 4);
    }
    ctab_.clear();
    ctab_.shrink_to_fit();
    return true;
}

bool Archive::Read(std::uint64_t offset, std::uint32_t size, const Decompressor& dec, std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    if (!LoadChunks(error)) return false;
    if (chunks_.empty()) return error = "no chunks", false;
    // The last chunk starting at or before offset (the table is sorted by uncompressed offset).
    auto it = std::upper_bound(chunks_.begin(), chunks_.end(), offset, [](std::uint64_t o, const Chunk& c) { return o < c.uoff; });
    if (it != chunks_.begin()) --it;
    const std::uint64_t end = offset + size;
    out.reserve(size);
    std::vector<std::uint8_t> comp, plain;
    for (; it != chunks_.end() && it->uoff < end; ++it) {
        if (it->csize > (64u << 20) || it->usize > (64u << 20)) return error = "chunk too large", false;
        comp.resize(it->csize);
        if (!ReadRaw(it->coff, comp.data(), it->csize)) return error = "short chunk read", false;
        if (encrypted_) {
            std::uint8_t mask[16];
            if (!ChunkMask(it->key16, mask)) return error = "MD5 unavailable", false;
            for (std::size_t i = 0; i < comp.size(); ++i) comp[i] ^= mask[i & 15];
        }
        if (it->csize == it->usize) {
            plain.swap(comp);
        } else {
            plain.resize(it->usize);
            if (!dec || !dec(comp.data(), comp.size(), plain.data(), plain.size())) return error = "decompression failed", false;
        }
        const std::uint64_t a = std::max(offset, it->uoff) - it->uoff;
        const std::uint64_t b = std::min(end, it->uoff + it->usize) - it->uoff;
        if (b > a) out.insert(out.end(), plain.begin() + static_cast<std::ptrdiff_t>(a), plain.begin() + static_cast<std::ptrdiff_t>(b));
    }
    if (out.size() != size) return error = "short read " + std::to_string(out.size()) + " of " + std::to_string(size), false;
    return true;
}

// ---- ArchiveSet ----------------------------------------------------------------------------------------------
bool ArchiveSet::Open(const std::wstring& data_dir, std::string& error) {
    archives_.clear();
    where_.clear();
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((data_dir + L"\\*.bin").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return error = "no .bin archives in the game's data folder", false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.emplace_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // Sorted like Python's sorted() on str (code-point order), so duplicates resolve the same way.
    std::sort(names.begin(), names.end());
    for (const std::wstring& n : names) {
        auto a = std::make_unique<Archive>();
        std::string err;
        if (!a->Open(data_dir + L"\\" + n, err)) continue;   // a damaged extra file is skipped
        archives_.push_back(std::move(a));
    }
    if (archives_.empty()) return error = "no readable archives", false;
    return true;
}

void ArchiveSet::Index(const std::set<std::uint64_t>& wanted) {
    for (std::size_t i = 0; i < archives_.size(); ++i) {
        std::vector<ArchiveEntry> found;
        archives_[i]->Find(wanted, found);
        for (const ArchiveEntry& e : found)
            if (!where_.count(e.hash)) where_[e.hash] = {i, e};
    }
}

bool ArchiveSet::ReadFile(std::uint64_t hash, const Decompressor& dec, std::vector<std::uint8_t>& out, std::string& error) {
    auto it = where_.find(hash);
    if (it == where_.end()) return error = "not in any archive", false;
    return archives_[it->second.first]->Read(it->second.second.offset, it->second.second.size, dec, out, error);
}

}  // namespace st::tex
