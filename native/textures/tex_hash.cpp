// Stranded Together textures: hashes through BCrypt (tex_hash.hpp).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <vector>

#include "tex_hash.hpp"

namespace st::tex {
namespace {

class Hasher {
public:
    explicit Hasher(LPCWSTR algorithm) {
        if (BCryptOpenAlgorithmProvider(&alg_, algorithm, nullptr, 0) < 0) {
            alg_ = nullptr;
            return;
        }
        ok_ = BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    ~Hasher() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;
    bool Update(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        while (ok_ && size) {
            const ULONG piece = size > 0x40000000u ? 0x40000000u : static_cast<ULONG>(size);
            ok_ = BCryptHashData(hash_, const_cast<PUCHAR>(p), piece, 0) >= 0;
            p += piece;
            size -= piece;
        }
        return ok_;
    }
    bool Finish(std::uint8_t* out, ULONG size) { return ok_ && BCryptFinishHash(hash_, out, size, 0) >= 0; }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool ok_ = false;
};

}  // namespace

bool HashSha256(const void* data, std::size_t size, Sha256& out) {
    Hasher h(BCRYPT_SHA256_ALGORITHM);
    return h.Update(data, size) && h.Finish(out.data(), static_cast<ULONG>(out.size()));
}

bool HashMd5(const void* data, std::size_t size, Md5& out) {
    Hasher h(BCRYPT_MD5_ALGORITHM);
    return h.Update(data, size) && h.Finish(out.data(), static_cast<ULONG>(out.size()));
}

bool HashFileSha256(const std::wstring& path, Sha256& out, std::uint64_t* size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    Hasher h(BCRYPT_SHA256_ALGORITHM);
    std::vector<std::uint8_t> buf(1u << 20);
    std::uint64_t total = 0;
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        total += got;
        if (!h.Update(buf.data(), got)) {
            ok = false;
            break;
        }
    }
    CloseHandle(f);
    if (!ok || !h.Finish(out.data(), static_cast<ULONG>(out.size()))) return false;
    if (size) *size = total;
    return true;
}

std::string Hex(const std::uint8_t* data, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        s += kDigits[data[i] >> 4];
        s += kDigits[data[i] & 15];
    }
    return s;
}

}  // namespace st::tex
