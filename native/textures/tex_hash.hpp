// Stranded Together textures: SHA-256 and MD5 through Windows CNG (BCrypt), and CRC-32. Used by the extractor (MD5 for
// the archive chunk masks, SHA-256 for the manifest) and later by the mod's loader. The launcher's own hash.hpp has
// the same SHA-256 with its file reader; this one stays free of the launcher's files so the mod can link it.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace st::tex {

using Sha256 = std::array<std::uint8_t, 32>;
using Md5 = std::array<std::uint8_t, 16>;

bool HashSha256(const void* data, std::size_t size, Sha256& out);
bool HashMd5(const void* data, std::size_t size, Md5& out);
// SHA-256 of a whole file (read in 1 MiB pieces, shared read). False when it cannot be read.
bool HashFileSha256(const std::wstring& path, Sha256& out, std::uint64_t* size = nullptr);

std::string Hex(const std::uint8_t* data, std::size_t size);   // lowercase
template <std::size_t N>
std::string Hex(const std::array<std::uint8_t, N>& d) { return Hex(d.data(), N); }

// CRC-32 (IEEE, reflected 0xEDB88320), usable at compile time (tex_slots.hpp's table version).
constexpr std::uint32_t Crc32(const char* text, std::size_t size, std::uint32_t crc = 0) {
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint8_t>(text[i]);
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

}  // namespace st::tex
