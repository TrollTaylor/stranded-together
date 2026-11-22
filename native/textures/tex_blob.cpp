// Stranded Together textures: the blob (tex_blob.hpp).
#include "tex_blob.hpp"

#include <cstring>

namespace st::tex {
namespace {

void Put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void Put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void Put64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint16_t Get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t Get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint64_t Get64(const std::uint8_t* p) { return static_cast<std::uint64_t>(Get32(p)) | static_cast<std::uint64_t>(Get32(p + 4)) << 32; }

bool Fail(std::string* why, const char* text) {
    if (why) *why = text;
    return false;
}

int Hex1(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

int SlotIndex(const char* id) {
    if (!id) return -1;
    for (std::size_t i = 0; i < kSlotCount; ++i)
        if (std::strcmp(kSlots[i].id, id) == 0) return static_cast<int>(i);
    return -1;
}

MipKind SlotMipKind(const Slot& s) {
    if ((s.packing & 0x0Fu) == kTypeNormal) return MipKind::normal_rg;
    return s.srgb ? MipKind::srgb_rgb : MipKind::linear;
}

std::size_t BlobSize(const Slot& s) { return kBlobHeader + static_cast<std::size_t>(ChainBytes(s.clone_fmt, s.w, s.h, s.mips)); }

bool GuidBytes(const char* t, std::uint8_t out[16]) {
    // 8-4-4-4-12 hex digits
    std::uint8_t raw[16] = {};
    int k = 0;
    for (const char* p = t; p && *p; ++p) {
        if (*p == '-') continue;
        const int h = Hex1(*p);
        if (h < 0 || k >= 32) return false;
        raw[k / 2] = static_cast<std::uint8_t>(raw[k / 2] | (k % 2 ? h : h << 4));
        ++k;
    }
    if (k != 32) return false;
    // The text's first three groups are little-endian numbers (Windows GUID order).
    out[0] = raw[3], out[1] = raw[2], out[2] = raw[1], out[3] = raw[0];
    out[4] = raw[5], out[5] = raw[4];
    out[6] = raw[7], out[7] = raw[6];
    for (int i = 8; i < 16; ++i) out[i] = raw[i];
    return true;
}

bool BlobGuid(const std::uint8_t* p, std::size_t n, std::uint8_t out[16]) {
    if (!p || n < kBlobHeader) return false;
    std::vector<std::uint8_t> copy(p, p + n);
    std::memset(copy.data() + 0x10, 0, 16);
    Sha256 h{};
    if (!HashSha256(copy.data(), copy.size(), h)) return false;
    out[0] = 'S', out[1] = 'T', out[2] = 'T', out[3] = 'X';
    std::memcpy(out + 4, h.data(), 12);
    return true;
}

bool BuildBlob(const Slot& s, const Image& packed, std::vector<std::uint8_t>& blob, std::string* error) {
    if (!CanEncode(s.clone_fmt)) {
        if (error) *error = std::string("slot ") + s.id + ": format " + FormatName(s.clone_fmt) + " is not encoded";
        return false;
    }
    if (packed.w == 0 || packed.h == 0) {
        if (error) *error = "empty image";
        return false;
    }
    const MipKind kind = SlotMipKind(s);
    Image mip = (packed.w == s.w && packed.h == s.h) ? packed : Resize(packed, s.w, s.h, kind);
    const std::size_t total = BlobSize(s);
    blob.assign(total, 0);
    std::uint8_t* h = blob.data();
    h[0] = 0;
    Put16(h + 2, s.w);
    Put16(h + 4, s.h);
    h[8] = s.mips;
    h[9] = s.clone_fmt;
    h[0x0B] = s.srgb;
    h[0x0D] = 0xA9;
    h[0x0E] = 0;
    const std::uint32_t embedded = static_cast<std::uint32_t>(total - kBlobHeader);
    Put32(h + 0x20, 20u + embedded);
    Put32(h + 0x24, embedded);
    Put32(h + 0x28, 0);
    Put32(h + 0x2C, 0);
    Put64(h + 0x30, s.views);
    std::size_t at = kBlobHeader;
    std::vector<std::uint8_t> enc;
    for (std::uint32_t m = 0; m < s.mips; ++m) {
        if (m) mip = Downsample(mip, kind);
        if (!Encode(s.clone_fmt, mip, enc, error)) return false;
        const std::uint64_t want = MipBytes(s.clone_fmt, mip.w, mip.h);
        if (enc.size() != want || at + enc.size() > total) {
            if (error) *error = "mip size mismatch";
            return false;
        }
        std::memcpy(blob.data() + at, enc.data(), enc.size());
        at += enc.size();
    }
    if (at != total) {
        if (error) *error = "chain size mismatch";
        return false;
    }
    std::uint8_t g[16];
    if (!BlobGuid(blob.data(), blob.size(), g)) {
        if (error) *error = "hash failed";
        return false;
    }
    std::memcpy(h + 0x10, g, 16);
    return true;
}

bool ValidateBlob(const Slot& s, const std::uint8_t* p, std::size_t n, const Sha256* id, std::string* why) {
    if (!p || n < kBlobHeader) return Fail(why, "too short");
    if (n > kBlobMax) return Fail(why, "too large");
    if (n != BlobSize(s)) return Fail(why, "size differs from the slot's");
    if (p[0] != 0) return Fail(why, "not a 2D texture");
    if (p[1] != 0) return Fail(why, "header byte 1");
    if (Get16(p + 2) != s.w || Get16(p + 4) != s.h) return Fail(why, "dimensions differ from the slot's");
    if (Get16(p + 6) != 0) return Fail(why, "slices");
    if (p[8] != s.mips) return Fail(why, "mip count");
    if (p[9] != s.clone_fmt) return Fail(why, "format");
    if (p[9] != kBC1 && p[9] != kBC3 && p[9] != kBC4U && p[9] != kBC5U) return Fail(why, "format not allowed");
    if (p[0x0A] != 0) return Fail(why, "header byte 0x0A");
    if (p[0x0B] != s.srgb) return Fail(why, "colour space");
    if (p[0x0C] != 0) return Fail(why, "header byte 0x0C");
    if (p[0x0D] != 0xA9) return Fail(why, "flags");
    if (p[0x0E] != 0) return Fail(why, "streaming mips");
    if (p[0x0F] != 0) return Fail(why, "header byte 0x0F");
    if (p[0x10] != 'S' || p[0x11] != 'T' || p[0x12] != 'T' || p[0x13] != 'X') return Fail(why, "not one of ours (GUID)");
    const std::uint32_t embedded = static_cast<std::uint32_t>(n - kBlobHeader);
    if (Get32(p + 0x20) != 20u + embedded) return Fail(why, "data size");
    if (Get32(p + 0x24) != embedded) return Fail(why, "embedded size");
    if (ChainBytes(s.clone_fmt, s.w, s.h, s.mips) != embedded) return Fail(why, "chain size");
    if (Get32(p + 0x28) != 0) return Fail(why, "streamed bytes");
    if (Get32(p + 0x2C) != 0) return Fail(why, "first embedded mip");
    if (Get64(p + 0x30) != s.views) return Fail(why, "extra views");
    std::uint8_t g[16];
    if (!BlobGuid(p, n, g)) return Fail(why, "hash failed");
    if (std::memcmp(g, p + 0x10, 16) != 0) return Fail(why, "GUID does not match the content");
    if (id) {
        Sha256 h{};
        if (!HashSha256(p, n, h)) return Fail(why, "hash failed");
        if (h != *id) return Fail(why, "SHA-256 differs from the announced id");
    }
    return true;
}

}  // namespace st::tex
