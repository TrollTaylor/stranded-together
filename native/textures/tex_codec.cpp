// Stranded Together textures: the pure pixel code (tex_codec.hpp). Project-authored decoders and encoders written
// from the public BCn format descriptions (no third-party code).
#include "tex_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace st::tex {

// ---- Formats ---------------------------------------------------------------------------------------------
const char* FormatName(std::uint8_t fmt) {
    switch (fmt) {
    case kRGBA8: return "RGBA8";
    case kBC1: return "BC1";
    case kBC2: return "BC2";
    case kBC3: return "BC3";
    case kBC4U: return "BC4U";
    case kBC4S: return "BC4S";
    case kBC5U: return "BC5U";
    case kBC5S: return "BC5S";
    case kBC6U: return "BC6U";
    case kBC6S: return "BC6S";
    case kBC7: return "BC7";
    default: return "fmt?";
    }
}

int BlockBytes(std::uint8_t fmt) {
    switch (fmt) {
    case kBC1:
    case kBC4U:
    case kBC4S: return 8;
    case kBC2:
    case kBC3:
    case kBC5U:
    case kBC5S:
    case kBC6U:
    case kBC6S:
    case kBC7: return 16;
    default: return 0;
    }
}

bool CanDecode(std::uint8_t fmt) {
    return fmt == kRGBA8 || fmt == kBC1 || fmt == kBC3 || fmt == kBC4U || fmt == kBC5U || fmt == kBC7;
}
bool CanEncode(std::uint8_t fmt) { return fmt == kBC1 || fmt == kBC3 || fmt == kBC4U || fmt == kBC5U; }

std::uint64_t MipBytes(std::uint8_t fmt, std::uint32_t w, std::uint32_t h) {
    if (fmt == kRGBA8) return static_cast<std::uint64_t>(std::max(1u, w)) * std::max(1u, h) * 4u;
    const int bb = BlockBytes(fmt);
    if (!bb) return 0;
    const std::uint64_t bw = std::max(1u, (w + 3u) / 4u), bh = std::max(1u, (h + 3u) / 4u);
    return bw * bh * static_cast<std::uint64_t>(bb);
}

std::uint64_t ChainBytes(std::uint8_t fmt, std::uint32_t w, std::uint32_t h, std::uint32_t mips) {
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < mips; ++i) {
        sum += MipBytes(fmt, w, h);
        w = std::max(1u, w / 2u);
        h = std::max(1u, h / 2u);
    }
    return sum;
}

std::uint32_t FullMipCount(std::uint32_t w, std::uint32_t h) {
    std::uint32_t n = 1, m = std::max(w, h);
    while (m > 1) {
        m /= 2;
        ++n;
    }
    return n;
}

// ---- Decoders --------------------------------------------------------------------------------------------
namespace {

inline std::uint16_t Rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
inline std::uint32_t Rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

void Expand565(std::uint16_t c, std::uint8_t out[3]) {
    const unsigned r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    out[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
    out[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
}

// 16 RGBA pixels of one BC1 colour block. `force4`: BC3's colour half (always 4-colour).
void DecodeBC1Block(const std::uint8_t* b, std::uint8_t px[16][4], bool force4) {
    const std::uint16_t c0 = Rd16(b), c1 = Rd16(b + 2);
    const std::uint32_t bits = Rd32(b + 4);
    std::uint8_t pal[4][4];
    Expand565(c0, pal[0]);
    Expand565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (force4 || c0 > c1) {
        for (int c = 0; c < 3; ++c) {
            pal[2][c] = static_cast<std::uint8_t>((2 * pal[0][c] + pal[1][c]) / 3);
            pal[3][c] = static_cast<std::uint8_t>((pal[0][c] + 2 * pal[1][c]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int c = 0; c < 3; ++c) {
            pal[2][c] = static_cast<std::uint8_t>((pal[0][c] + pal[1][c]) / 2);
            pal[3][c] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    for (int k = 0; k < 16; ++k) std::memcpy(px[k], pal[(bits >> (2 * k)) & 3u], 4);
}

void DecodeBC4Block(const std::uint8_t* b, std::uint8_t out[16]) {
    const unsigned a0 = b[0], a1 = b[1];
    unsigned pal[8];
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (unsigned k = 1; k < 7; ++k) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
    } else {
        for (unsigned k = 1; k < 5; ++k) pal[k + 1] = ((5 - k) * a0 + k * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
    for (int k = 0; k < 16; ++k) out[k] = static_cast<std::uint8_t>(pal[(bits >> (3 * k)) & 7u]);
}

// --- BC7 (all 8 modes)
constexpr std::uint16_t kP2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660,
    0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22};
constexpr const char* kP3[64] = {
    "0011001102212222", "0001001122112221", "0000200122112211", "0222002200110111", "0000000011221122", "0011001100220022",
    "0022002211111111", "0011001122112211", "0000000011112222", "0000111111112222", "0000111122222222", "0012001200120012",
    "0112011201120112", "0122012201220122", "0011011211221222", "0011200122002220", "0001001101121122", "0111001120012200",
    "0000112211221122", "0022002200221111", "0111011102220222", "0001000122212221", "0000001101220122", "0000110022102210",
    "0122012200110000", "0012001211222222", "0110122112210110", "0000011012211221", "0022110211020022", "0110011020022222",
    "0011012201220011", "0000200022112221", "0000000211221222", "0222002200120011", "0011001200220222", "0120012001200120",
    "0000111122220000", "0120120120120120", "0120201212010120", "0011220011220011", "0011112222000011", "0101010122222222",
    "0000000021212121", "0022112200221122", "0022001100220011", "0220122102201221", "0101222222220101", "0000212121212121",
    "0101010101012222", "0222011102220111", "0002111200021112", "0000211221122112", "0222011101110222", "0002111211120002",
    "0110011001102222", "0000000021122112", "0110011022222222", "0022001100110022", "0022112211220022", "0000000000002112",
    "0002000100020001", "0222122202221222", "0101222222222222", "0111201122012220"};
constexpr std::uint8_t kA2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
                                  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8,  2, 2,
                                  2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15};
constexpr std::uint8_t kA3a[64] = {3, 3,  15, 15, 8, 3,  15, 15, 8,  8,  6,  6, 6,  5,  3,  3,  3,  3,  8,  15, 3, 3,
                                   6, 10, 5,  8,  8, 6,  8,  5,  15, 15, 8,  15, 3, 5,  6,  10, 8,  15, 15, 3,  15, 5,
                                   15, 15, 15, 15, 3, 15, 5,  5,  5,  8,  5,  10, 5, 10, 8,  13, 15, 12, 3,  3};
constexpr std::uint8_t kA3b[64] = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
                                   15, 8,  3,  15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
                                   3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};
constexpr std::uint8_t kW2[4] = {0, 21, 43, 64};
constexpr std::uint8_t kW3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::uint8_t kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
struct Bc7Mode {
    int ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2;
};
constexpr Bc7Mode kModes[8] = {{3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
                               {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
                               {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};

struct BitReader {
    std::uint64_t lo, hi;
    int pos = 0;
    unsigned Get(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; ++i, ++pos) {
            const std::uint64_t word = pos < 64 ? lo : hi;
            const int bit = pos < 64 ? pos : pos - 64;
            v |= static_cast<unsigned>((word >> bit) & 1u) << i;
        }
        return v;
    }
};

const std::uint8_t* Weights(int bits) { return bits == 2 ? kW2 : bits == 3 ? kW3 : kW4; }

int Subset(int ns, int part, int i) {
    if (ns == 1) return 0;
    if (ns == 2) return (kP2[part] >> i) & 1;
    return kP3[part][i] - '0';
}
int Anchor(int ns, int part, int subset) {
    if (subset == 0) return 0;
    if (ns == 2) return kA2[part];
    return subset == 1 ? kA3a[part] : kA3b[part];
}

void DecodeBC7Block(const std::uint8_t* b, std::uint8_t px[16][4]) {
    BitReader r{0, 0};
    for (int i = 0; i < 8; ++i) r.lo |= static_cast<std::uint64_t>(b[i]) << (8 * i);
    for (int i = 0; i < 8; ++i) r.hi |= static_cast<std::uint64_t>(b[8 + i]) << (8 * i);
    int m = 0;
    while (m < 8 && r.Get(1) == 0) ++m;
    if (m == 8) {
        std::memset(px, 0, 16 * 4);
        return;
    }
    const Bc7Mode& md = kModes[m];
    const int part = static_cast<int>(r.Get(md.pb));
    const unsigned rot = r.Get(md.rb);
    const unsigned isel = r.Get(md.isb);
    const int ne = md.ns * 2;
    unsigned ep[6][4];
    for (int e = 0; e < 6; ++e) ep[e][0] = ep[e][1] = ep[e][2] = 0, ep[e][3] = 255;
    for (int c = 0; c < 3; ++c)
        for (int e = 0; e < ne; ++e) ep[e][c] = r.Get(md.cb);
    if (md.ab)
        for (int e = 0; e < ne; ++e) ep[e][3] = r.Get(md.ab);
    int cb = md.cb, ab = md.ab;
    if (md.epb) {
        for (int e = 0; e < ne; ++e) {
            const unsigned p = r.Get(1);
            for (int c = 0; c < 3; ++c) ep[e][c] = (ep[e][c] << 1) | p;
            if (md.ab) ep[e][3] = (ep[e][3] << 1) | p;
        }
        ++cb;
        if (ab) ++ab;
    }
    if (md.spb) {
        for (int s = 0; s < md.ns; ++s) {
            const unsigned p = r.Get(1);
            for (int e = 2 * s; e <= 2 * s + 1; ++e)
                for (int c = 0; c < 3; ++c) ep[e][c] = (ep[e][c] << 1) | p;
        }
        ++cb;
    }
    for (int e = 0; e < ne; ++e) {
        for (int c = 0; c < 3; ++c) {
            const unsigned x = ep[e][c] << (8 - cb);
            ep[e][c] = (x | (x >> cb)) & 255u;
        }
        if (ab) {
            const unsigned x = ep[e][3] << (8 - ab);
            ep[e][3] = (x | (x >> ab)) & 255u;
        }
    }
    unsigned idx[16], idx2[16] = {};
    for (int i = 0; i < 16; ++i) {
        const int s = Subset(md.ns, part, i);
        const int n = i == Anchor(md.ns, part, s) ? md.ib - 1 : md.ib;
        idx[i] = r.Get(n);
    }
    if (md.ib2)
        for (int i = 0; i < 16; ++i) idx2[i] = r.Get(i == 0 ? md.ib2 - 1 : md.ib2);
    const std::uint8_t* w1 = Weights(md.ib);
    const std::uint8_t* w2 = md.ib2 ? Weights(md.ib2) : nullptr;
    for (int i = 0; i < 16; ++i) {
        const int s = Subset(md.ns, part, i);
        const unsigned* e0 = ep[2 * s];
        const unsigned* e1 = ep[2 * s + 1];
        unsigned wc, wa;
        if (!w2) {
            wc = wa = w1[idx[i]];
        } else if (!isel) {
            wc = w1[idx[i]];
            wa = w2[idx2[i]];
        } else {
            wc = w2[idx2[i]];
            wa = w1[idx[i]];
        }
        unsigned v[4];
        for (int c = 0; c < 3; ++c) v[c] = ((64 - wc) * e0[c] + wc * e1[c] + 32) >> 6;
        v[3] = ((64 - wa) * e0[3] + wa * e1[3] + 32) >> 6;
        if (rot == 1) std::swap(v[0], v[3]);
        else if (rot == 2) std::swap(v[1], v[3]);
        else if (rot == 3) std::swap(v[2], v[3]);
        for (int c = 0; c < 4; ++c) px[i][c] = static_cast<std::uint8_t>(v[c]);
    }
}

template <typename F>
void ForBlocks(std::uint32_t w, std::uint32_t h, F&& f) {
    const std::uint32_t bw = std::max(1u, (w + 3u) / 4u), bh = std::max(1u, (h + 3u) / 4u);
    for (std::uint32_t by = 0; by < bh; ++by)
        for (std::uint32_t bx = 0; bx < bw; ++bx) f(bx, by, static_cast<std::size_t>(by) * bw + bx);
}

void Store(Image& out, std::uint32_t bx, std::uint32_t by, const std::uint8_t px[16][4]) {
    for (int k = 0; k < 16; ++k) {
        const std::uint32_t x = bx * 4u + static_cast<std::uint32_t>(k & 3), y = by * 4u + static_cast<std::uint32_t>(k >> 2);
        if (x < out.w && y < out.h) std::memcpy(out.At(x, y), px[k], 4);
    }
}

}  // namespace

bool Decode(std::uint8_t fmt, const std::uint8_t* data, std::size_t size, std::uint32_t w, std::uint32_t h, Image& out, std::string* error) {
    if (!CanDecode(fmt)) {
        if (error) *error = std::string("format ") + FormatName(fmt) + " is not decoded";
        return false;
    }
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        if (error) *error = "bad size";
        return false;
    }
    const std::uint64_t need = MipBytes(fmt, w, h);
    if (size < need) {
        if (error) *error = "short data: " + std::to_string(size) + " of " + std::to_string(need) + " bytes";
        return false;
    }
    out.Resize(w, h);
    if (fmt == kRGBA8) {
        std::memcpy(out.rgba.data(), data, out.rgba.size());
        return true;
    }
    std::uint8_t px[16][4];
    switch (fmt) {
    case kBC1:
        ForBlocks(w, h, [&](std::uint32_t bx, std::uint32_t by, std::size_t i) {
            DecodeBC1Block(data + i * 8, px, false);
            Store(out, bx, by, px);
        });
        break;
    case kBC3:
        ForBlocks(w, h, [&](std::uint32_t bx, std::uint32_t by, std::size_t i) {
            std::uint8_t a[16];
            DecodeBC4Block(data + i * 16, a);
            DecodeBC1Block(data + i * 16 + 8, px, true);
            for (int k = 0; k < 16; ++k) px[k][3] = a[k];
            Store(out, bx, by, px);
        });
        break;
    case kBC4U:
        ForBlocks(w, h, [&](std::uint32_t bx, std::uint32_t by, std::size_t i) {
            std::uint8_t v[16];
            DecodeBC4Block(data + i * 8, v);
            for (int k = 0; k < 16; ++k) px[k][0] = px[k][1] = px[k][2] = v[k], px[k][3] = 255;
            Store(out, bx, by, px);
        });
        break;
    case kBC5U:
        ForBlocks(w, h, [&](std::uint32_t bx, std::uint32_t by, std::size_t i) {
            std::uint8_t r[16], g[16];
            DecodeBC4Block(data + i * 16, r);
            DecodeBC4Block(data + i * 16 + 8, g);
            for (int k = 0; k < 16; ++k) px[k][0] = r[k], px[k][1] = g[k], px[k][2] = 0, px[k][3] = 255;
            Store(out, bx, by, px);
        });
        break;
    case kBC7:
        ForBlocks(w, h, [&](std::uint32_t bx, std::uint32_t by, std::size_t i) {
            DecodeBC7Block(data + i * 16, px);
            Store(out, bx, by, px);
        });
        break;
    default: return false;
    }
    return true;
}

void RebuildNormalZ(Image& img) {
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        const double x = img.rgba[i] / 127.5 - 1.0, y = img.rgba[i + 1] / 127.5 - 1.0;
        const double z = std::sqrt(std::max(0.0, 1.0 - x * x - y * y));
        img.rgba[i + 2] = static_cast<std::uint8_t>(std::min(255.0, z * 127.5 + 127.5 + 0.5));
        img.rgba[i + 3] = 255;
    }
}

// ---- Encoders --------------------------------------------------------------------------------------------
namespace {

// The 16 pixels of block (bx, by); edge blocks repeat the last row / column.
void Gather(const Image& img, std::uint32_t bx, std::uint32_t by, std::uint8_t px[16][4]) {
    for (int k = 0; k < 16; ++k) {
        const std::uint32_t x = std::min(img.w - 1, bx * 4u + static_cast<std::uint32_t>(k & 3));
        const std::uint32_t y = std::min(img.h - 1, by * 4u + static_cast<std::uint32_t>(k >> 2));
        std::memcpy(px[k], img.At(x, y), 4);
    }
}

std::uint16_t To565(int r, int g, int b) {
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    return static_cast<std::uint16_t>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

// Squared error of choosing the best of the 4 palette entries for every pixel; fills idx.
int ChooseIndices(const std::uint8_t px[16][4], std::uint16_t c0, std::uint16_t c1, unsigned idx[16]) {
    std::uint8_t p0[3], p1[3];
    Expand565(c0, p0);
    Expand565(c1, p1);
    int pal[4][3];
    for (int c = 0; c < 3; ++c) {
        pal[0][c] = p0[c];
        pal[1][c] = p1[c];
        pal[2][c] = (2 * p0[c] + p1[c]) / 3;
        pal[3][c] = (p0[c] + 2 * p1[c]) / 3;
    }
    int total = 0;
    for (int k = 0; k < 16; ++k) {
        int best = 0, best_d = 1 << 30;
        for (int j = 0; j < 4; ++j) {
            const int dr = px[k][0] - pal[j][0], dg = px[k][1] - pal[j][1], db = px[k][2] - pal[j][2];
            const int d = dr * dr + dg * dg + db * db;
            if (d < best_d) best_d = d, best = j;
        }
        idx[k] = static_cast<unsigned>(best);
        total += best_d;
    }
    return total;
}

// Writes an 8-byte 4-colour BC1 block (c0 > c1; equal endpoints -> all index 0).
void EmitBC1(std::uint8_t* out, std::uint16_t c0, std::uint16_t c1, const unsigned idx_in[16]) {
    unsigned idx[16];
    std::memcpy(idx, idx_in, sizeof(idx));
    if (c0 < c1) {
        std::swap(c0, c1);
        static constexpr unsigned kSwap[4] = {1, 0, 3, 2};
        for (unsigned& i : idx) i = kSwap[i];
    } else if (c0 == c1) {
        for (unsigned& i : idx) i = 0;
    }
    std::uint32_t bits = 0;
    for (int k = 0; k < 16; ++k) bits |= idx[k] << (2 * k);
    out[0] = static_cast<std::uint8_t>(c0 & 0xFF);
    out[1] = static_cast<std::uint8_t>(c0 >> 8);
    out[2] = static_cast<std::uint8_t>(c1 & 0xFF);
    out[3] = static_cast<std::uint8_t>(c1 >> 8);
    for (int i = 0; i < 4; ++i) out[4 + i] = static_cast<std::uint8_t>(bits >> (8 * i));
}

void EncodeBC1Block(const std::uint8_t px[16][4], std::uint8_t* out) {
    // Mean and covariance (integers: exact).
    int sum[3] = {0, 0, 0};
    for (int k = 0; k < 16; ++k)
        for (int c = 0; c < 3; ++c) sum[c] += px[k][c];
    std::int64_t cov[6] = {0, 0, 0, 0, 0, 0};   // rr rg rb gg gb bb, scaled by 16*16
    for (int k = 0; k < 16; ++k) {
        const std::int64_t r = px[k][0] * 16 - sum[0], g = px[k][1] * 16 - sum[1], b = px[k][2] * 16 - sum[2];
        cov[0] += r * r, cov[1] += r * g, cov[2] += r * b, cov[3] += g * g, cov[4] += g * b, cov[5] += b * b;
    }
    // Principal axis by power iteration (doubles from exact integers; the same steps on every CPU).
    double ax[3] = {1.0, 1.0, 1.0};
    for (int it = 0; it < 8; ++it) {
        const double n0 = cov[0] * ax[0] + cov[1] * ax[1] + cov[2] * ax[2];
        const double n1 = cov[1] * ax[0] + cov[3] * ax[1] + cov[4] * ax[2];
        const double n2 = cov[2] * ax[0] + cov[4] * ax[1] + cov[5] * ax[2];
        const double m = std::max({std::fabs(n0), std::fabs(n1), std::fabs(n2)});
        if (m <= 0.0) break;
        ax[0] = n0 / m, ax[1] = n1 / m, ax[2] = n2 / m;
    }
    // Extremes projected on the axis.
    int lo = 0, hi = 0;
    double dmin = 1e30, dmax = -1e30;
    for (int k = 0; k < 16; ++k) {
        const double d = px[k][0] * ax[0] + px[k][1] * ax[1] + px[k][2] * ax[2];
        if (d < dmin) dmin = d, lo = k;
        if (d > dmax) dmax = d, hi = k;
    }
    std::uint16_t c0 = To565(px[hi][0], px[hi][1], px[hi][2]);
    std::uint16_t c1 = To565(px[lo][0], px[lo][1], px[lo][2]);
    unsigned idx[16];
    int err = ChooseIndices(px, c0, c1, idx);
    // Least-squares refinement of the two endpoints for the chosen indices (2 passes).
    for (int pass = 0; pass < 2 && err > 0; ++pass) {
        static constexpr int kW0[4] = {3, 0, 2, 1};   // weight of c0, in thirds
        int a2 = 0, b2 = 0, ab = 0;
        int ax0[3] = {0, 0, 0}, bx0[3] = {0, 0, 0};
        for (int k = 0; k < 16; ++k) {
            const int a = kW0[idx[k]], b = 3 - a;
            a2 += a * a, b2 += b * b, ab += a * b;
            for (int c = 0; c < 3; ++c) ax0[c] += a * px[k][c] * 3, bx0[c] += b * px[k][c] * 3;   // x scaled by 3 to match
        }
        const int det = a2 * b2 - ab * ab;
        if (det == 0) break;
        int e0[3], e1[3];
        for (int c = 0; c < 3; ++c) {
            // Solve [a2 ab; ab b2] [e0 e1] = [ax bx] (weights in thirds, so x*3*3 / 3 = x*3: the 3 cancels).
            const double v0 = (static_cast<double>(ax0[c]) * b2 - static_cast<double>(bx0[c]) * ab) / det;
            const double v1 = (static_cast<double>(bx0[c]) * a2 - static_cast<double>(ax0[c]) * ab) / det;
            e0[c] = static_cast<int>(std::lround(v0));
            e1[c] = static_cast<int>(std::lround(v1));
        }
        const std::uint16_t n0 = To565(e0[0], e0[1], e0[2]), n1 = To565(e1[0], e1[1], e1[2]);
        unsigned nidx[16];
        const int nerr = ChooseIndices(px, n0, n1, nidx);
        if (nerr >= err) break;
        c0 = n0, c1 = n1, err = nerr;
        std::memcpy(idx, nidx, sizeof(idx));
    }
    EmitBC1(out, c0, c1, idx);
}

void EncodeBC4Block(const std::uint8_t v[16], std::uint8_t* out) {
    int mn = 255, mx = 0;
    for (int k = 0; k < 16; ++k) mn = std::min(mn, static_cast<int>(v[k])), mx = std::max(mx, static_cast<int>(v[k]));
    std::uint64_t bits = 0;
    if (mx > mn) {
        // 8-value mode: a0 = max > a1 = min; palette index 0 = max, 1 = min, 2..7 between.
        int pal[8];
        pal[0] = mx;
        pal[1] = mn;
        for (int k = 1; k < 7; ++k) pal[k + 1] = ((7 - k) * mx + k * mn) / 7;
        for (int k = 0; k < 16; ++k) {
            int best = 0, bd = 1 << 30;
            for (int j = 0; j < 8; ++j) {
                const int d = std::abs(pal[j] - v[k]);
                if (d < bd) bd = d, best = j;
            }
            bits |= static_cast<std::uint64_t>(best) << (3 * k);
        }
    }
    out[0] = static_cast<std::uint8_t>(mx);
    out[1] = static_cast<std::uint8_t>(mx > mn ? mn : mx);
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<std::uint8_t>(bits >> (8 * i));
}

}  // namespace

void EncodeBC1(const Image& img, std::vector<std::uint8_t>& out) {
    out.assign(static_cast<std::size_t>(MipBytes(kBC1, img.w, img.h)), 0);
    std::uint8_t px[16][4];
    ForBlocks(img.w, img.h, [&](std::uint32_t bx, std::uint32_t by, std::size_t i) {
        Gather(img, bx, by, px);
        EncodeBC1Block(px, out.data() + i * 8);
    });
}

void EncodeBC4(const Image& img, int channel, std::vector<std::uint8_t>& out) {
    out.assign(static_cast<std::size_t>(MipBytes(kBC4U, img.w, img.h)), 0);
    std::uint8_t px[16][4], v[16];
    ForBlocks(img.w, img.h, [&](std::uint32_t bx, std::uint32_t by, std::size_t i) {
        Gather(img, bx, by, px);
        for (int k = 0; k < 16; ++k) v[k] = px[k][channel & 3];
        EncodeBC4Block(v, out.data() + i * 8);
    });
}

void EncodeBC3(const Image& img, std::vector<std::uint8_t>& out) {
    out.assign(static_cast<std::size_t>(MipBytes(kBC3, img.w, img.h)), 0);
    std::uint8_t px[16][4], v[16];
    ForBlocks(img.w, img.h, [&](std::uint32_t bx, std::uint32_t by, std::size_t i) {
        Gather(img, bx, by, px);
        for (int k = 0; k < 16; ++k) v[k] = px[k][3];
        EncodeBC4Block(v, out.data() + i * 16);
        EncodeBC1Block(px, out.data() + i * 16 + 8);
    });
}

void EncodeBC5(const Image& img, std::vector<std::uint8_t>& out) {
    out.assign(static_cast<std::size_t>(MipBytes(kBC5U, img.w, img.h)), 0);
    std::uint8_t px[16][4], v[16];
    ForBlocks(img.w, img.h, [&](std::uint32_t bx, std::uint32_t by, std::size_t i) {
        Gather(img, bx, by, px);
        for (int k = 0; k < 16; ++k) v[k] = px[k][0];
        EncodeBC4Block(v, out.data() + i * 16);
        for (int k = 0; k < 16; ++k) v[k] = px[k][1];
        EncodeBC4Block(v, out.data() + i * 16 + 8);
    });
}

bool Encode(std::uint8_t fmt, const Image& img, std::vector<std::uint8_t>& out, std::string* error) {
    if (img.w == 0 || img.h == 0 || img.rgba.size() != static_cast<std::size_t>(img.w) * img.h * 4u) {
        if (error) *error = "empty image";
        return false;
    }
    switch (fmt) {
    case kBC1: EncodeBC1(img, out); return true;
    case kBC3: EncodeBC3(img, out); return true;
    case kBC4U: EncodeBC4(img, 0, out); return true;
    case kBC5U: EncodeBC5(img, out); return true;
    default:
        if (error) *error = std::string("format ") + FormatName(fmt) + " is not encoded";
        return false;
    }
}

// ---- Mips and resizing -------------------------------------------------------------------------------------
namespace {
struct SrgbTable {
    float to_lin[256];
    SrgbTable() {
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            to_lin[i] = static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
    }
};
const SrgbTable& Srgb() {
    static const SrgbTable t;
    return t;
}
std::uint8_t Round8(double v) { return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 255)); }
}  // namespace

float SrgbToLinear(std::uint8_t v) { return Srgb().to_lin[v]; }
std::uint8_t SrgbToLinear8(std::uint8_t v) { return Round8(Srgb().to_lin[v] * 255.0); }
std::uint8_t LinearToSrgb8(float v) {
    const double c = std::clamp(static_cast<double>(v), 0.0, 1.0);
    const double s = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return Round8(s * 255.0);
}

namespace {
// Weighted average of n source pixels into `out` per the kind.
void Average(const std::uint8_t* const* src, const double* wts, int n, MipKind kind, std::uint8_t out[4]) {
    double acc[4] = {0, 0, 0, 0}, wsum = 0;
    for (int i = 0; i < n; ++i) {
        const std::uint8_t* p = src[i];
        const double w = wts[i];
        wsum += w;
        if (kind == MipKind::srgb_rgb) {
            for (int c = 0; c < 3; ++c) acc[c] += w * SrgbToLinear(p[c]);
        } else if (kind == MipKind::normal_rg) {
            // B is ignored (an edited PNG's blue may be anything): z rebuilt per pixel, the 3D vectors averaged.
            const double nx = p[0] / 127.5 - 1.0, ny = p[1] / 127.5 - 1.0;
            acc[0] += w * nx;
            acc[1] += w * ny;
            acc[2] += w * std::sqrt(std::max(0.0, 1.0 - nx * nx - ny * ny));
        } else {
            for (int c = 0; c < 3; ++c) acc[c] += w * p[c];
        }
        acc[3] += w * p[3];
    }
    if (wsum <= 0) wsum = 1;
    for (double& a : acc) a /= wsum;
    if (kind == MipKind::srgb_rgb) {
        for (int c = 0; c < 3; ++c) out[c] = LinearToSrgb8(static_cast<float>(acc[c]));
    } else if (kind == MipKind::normal_rg) {
        double x = acc[0], y = acc[1];
        const double l = std::sqrt(x * x + y * y + acc[2] * acc[2]);
        if (l < 0.1) x = y = 0;   // opposite normals cancel: straight up
        else x /= l, y /= l;
        out[0] = Round8(x * 127.5 + 127.5);
        out[1] = Round8(y * 127.5 + 127.5);
        const double z = std::sqrt(std::max(0.0, 1.0 - x * x - y * y));
        out[2] = Round8(z * 127.5 + 127.5);
    } else {
        for (int c = 0; c < 3; ++c) out[c] = Round8(acc[c]);
    }
    out[3] = Round8(acc[3]);
}
}  // namespace

Image Downsample(const Image& src, MipKind kind) {
    Image out;
    const std::uint32_t w = std::max(1u, src.w / 2u), h = std::max(1u, src.h / 2u);
    out.Resize(w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t x0 = std::min(src.w - 1, x * 2u), x1 = std::min(src.w - 1, x * 2u + 1u);
            const std::uint32_t y0 = std::min(src.h - 1, y * 2u), y1 = std::min(src.h - 1, y * 2u + 1u);
            const std::uint8_t* p[4] = {src.At(x0, y0), src.At(x1, y0), src.At(x0, y1), src.At(x1, y1)};
            const double wts[4] = {1, 1, 1, 1};
            Average(p, wts, 4, kind, out.At(x, y));
        }
    }
    return out;
}

Image Resize(const Image& src, std::uint32_t w, std::uint32_t h, MipKind kind) {
    if (src.w == w && src.h == h) return src;
    Image cur = src;
    // Halve while at least twice as large on both axes (box filter), then a bilinear pass to the exact size.
    while (cur.w >= w * 2u && cur.h >= h * 2u) cur = Downsample(cur, kind);
    if (cur.w == w && cur.h == h) return cur;
    Image out;
    out.Resize(w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
        const double fy = std::clamp((y + 0.5) * cur.h / h - 0.5, 0.0, static_cast<double>(cur.h - 1));
        const std::uint32_t y0 = static_cast<std::uint32_t>(fy), y1 = std::min(cur.h - 1, y0 + 1u);
        const double ty = fy - y0;
        for (std::uint32_t x = 0; x < w; ++x) {
            const double fx = std::clamp((x + 0.5) * cur.w / w - 0.5, 0.0, static_cast<double>(cur.w - 1));
            const std::uint32_t x0 = static_cast<std::uint32_t>(fx), x1 = std::min(cur.w - 1, x0 + 1u);
            const double tx = fx - x0;
            const std::uint8_t* p[4] = {cur.At(x0, y0), cur.At(x1, y0), cur.At(x0, y1), cur.At(x1, y1)};
            const double wts[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
            Average(p, wts, 4, kind, out.At(x, y));
        }
    }
    return out;
}

// ---- Packing ---------------------------------------------------------------------------------------------
PackChannel Unpack(std::uint32_t packing, int channel) {
    const std::uint8_t b = static_cast<std::uint8_t>(packing >> (8 * (channel & 3)));
    PackChannel p;
    p.type = b & 0x0Fu;
    p.source = static_cast<std::uint8_t>((b >> 4) & 3u);
    p.single = (b & 0x80u) != 0;
    return p;
}

const char* SetTypeName(std::uint8_t type) {
    static const char* const kNames[] = {"Invalid", "Color", "Alpha", "Normal", "Reflectance", "AO", "Roughness", "Height",
                                         "Mask", "Mask_Alpha", "Incandescence", "Translucency_Diffusion", "Translucency_Amount",
                                         "Misc_01", "Curvature", "Luminance"};
    return type < 16 ? kNames[type] : "?";
}

const char* MapFileName(std::uint8_t type) {
    switch (type) {
    case kTypeColor: return "Colour";
    case kTypeAlpha: return "Alpha";
    case kTypeNormal: return "Normal";
    case kTypeReflectance: return "Reflectance";
    case kTypeAO: return "Ambient occlusion";
    case kTypeRoughness: return "Roughness";
    case kTypeHeight: return "Height";
    case kTypeMask: return "Mask";
    case kTypeMaskAlpha: return "Mask alpha";
    case kTypeIncandescence: return "Glow";
    case kTypeMisc01: return "Misc";
    case 11: return "Translucency";
    case 12: return "Translucency amount";
    case 14: return "Curvature";
    case 15: return "Luminance";
    default: return "Map";
    }
}

std::string PackedMap::Component() const {
    std::string s;
    for (int c : channels) s += "RGBA"[c & 3];
    return s;
}

std::vector<PackedMap> MapsOf(std::uint32_t packing) {
    std::vector<PackedMap> maps;
    for (int ch = 0; ch < 4; ++ch) {
        const PackChannel p = Unpack(packing, ch);
        if (p.type == 0) continue;
        auto it = std::find_if(maps.begin(), maps.end(), [&](const PackedMap& m) { return m.type == p.type; });
        if (it == maps.end()) {
            maps.push_back(PackedMap{p.type, {}});
            it = maps.end() - 1;
        }
        it->channels.push_back(ch);
    }
    for (PackedMap& m : maps)
        std::stable_sort(m.channels.begin(), m.channels.end(),
                         [&](int a, int b) { return Unpack(packing, a).source < Unpack(packing, b).source; });
    return maps;
}

Image ExtractChannels(const Image& src, const std::vector<int>& channels) {
    Image out;
    out.Resize(src.w, src.h);
    const std::size_t n = static_cast<std::size_t>(src.w) * src.h;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* s = &src.rgba[i * 4];
        std::uint8_t* d = &out.rgba[i * 4];
        if (channels.size() == 1) {
            d[0] = d[1] = d[2] = s[channels[0] & 3];
        } else {
            for (std::size_t c = 0; c < 3; ++c) d[c] = c < channels.size() ? s[channels[c] & 3] : 0;
        }
        d[3] = 255;
    }
    return out;
}

bool InsertChannels(Image& dst, const Image& map, const std::vector<int>& channels) {
    if (dst.w != map.w || dst.h != map.h || channels.empty() || channels.size() > 4) return false;
    const std::size_t n = static_cast<std::size_t>(dst.w) * dst.h;
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t c = 0; c < channels.size(); ++c) dst.rgba[i * 4 + (channels[c] & 3)] = map.rgba[i * 4 + std::min<std::size_t>(c, 2)];
    return true;
}

// ---- Measures --------------------------------------------------------------------------------------------
double Psnr(const Image& a, const Image& b, int mask) {
    if (a.w != b.w || a.h != b.h || a.rgba.size() != b.rgba.size()) return 0.0;
    double se = 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        if (!((mask >> (i & 3)) & 1)) continue;
        const double d = static_cast<double>(a.rgba[i]) - b.rgba[i];
        se += d * d;
        ++n;
    }
    if (n == 0) return 0.0;
    if (se == 0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / (se / static_cast<double>(n)));
}

void MeanRgb(const Image& img, double out[3], bool (*keep)(const std::uint8_t* px)) {
    double s[3] = {0, 0, 0};
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        if (keep && !keep(&img.rgba[i])) continue;
        for (int c = 0; c < 3; ++c) s[c] += img.rgba[i + static_cast<std::size_t>(c)];
        ++n;
    }
    for (int c = 0; c < 3; ++c) out[c] = n ? s[c] / static_cast<double>(n) : 0.0;
}

}  // namespace st::tex
