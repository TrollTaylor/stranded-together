// Stranded Together textures: Fragile's ready-made suit colours (suit_defaults.hpp). Integers only.
#include "suit_defaults.hpp"

#include <algorithm>
#include <cstring>

#include "suit_colour_math.hpp"

namespace st::tex::defaults {

namespace {

// Gold / silver per variant (section 6 of the round-8 design; variant 0 is the product).
struct MetalRow {
    Metal silver, gold;
};
constexpr MetalRow kMetals[kMetalVariants] = {
    // 0, the product since generator 2 (round 9): the round-8 proof's polished metal, from Sam's own metal pixels (Colour
    // mean (39,36,31), Reflectance mean (231,184,117)); on her suit a lighter Roughness picture looks shinier, so x1.15 is
    // glossier than the prototype's x0.75. Side by side in the game it is clearly the closer one to Sam's metal.
    {{{40, 40, 41}, {230, 231, 231}, 1150}, {{40, 36, 31}, {232, 185, 118}, 1150}},
    {{{55, 55, 57}, {226, 227, 228}, 700}, {{52, 47, 46}, {240, 192, 120}, 700}},   // 1: Sam's hue
    {{{50, 50, 52}, {218, 219, 219}, 750}, {{50, 46, 45}, {249, 198, 124}, 750}},   // 2: Sam measured
    // 3: the round-7 prototype (a satin metal), generator 1's product: kept for side-by-side comparisons (tex 11 3). Its
    // output is byte for byte what generator 1 made for 15 / 16 (the old golden hashes are its golden hashes now).
    {{{58, 58, 60}, {235, 235, 235}, 750}, {{58, 51, 48}, {240, 190, 100}, 750}},
};

inline std::uint32_t Lum(std::uint32_t r, std::uint32_t g, std::uint32_t b) { return 2126u * r + 7152u * g + 722u * b; }

void Fail(std::string* why, const char* text) {
    if (why) *why = text;
}

// One horizontal box pass of the blur: out = (side * (left + right) + centre * x + 2^23) >> 24, edges replicated.
void BlurRow(const std::uint8_t* in, std::uint8_t* out, std::uint32_t n, std::size_t stride) {
    for (std::uint32_t x = 0; x < n; ++x) {
        const std::uint64_t l = in[(x ? x - 1 : 0) * stride], c = in[x * stride], r = in[(x + 1 < n ? x + 1 : n - 1) * stride];
        out[x * stride] = static_cast<std::uint8_t>((kBlurSide * (l + r) + kBlurCentre * c + (1u << 23)) >> 24);
    }
}

// One 2:1 Lanczos line: `n` input samples (stride `si`) into n/2 outputs (stride `so`).
void LanczosLine(const std::uint8_t* in, std::size_t si, std::uint32_t n, std::uint8_t* out, std::size_t so) {
    const std::uint32_t m = n / 2;
    auto put = [&](std::uint32_t x, std::int64_t s) {
        s >>= 22;
        out[x * so] = static_cast<std::uint8_t>(s < 0 ? 0 : s > 255 ? 255 : s);
    };
    const std::int32_t* edge[3] = {kLanczosEdge0, kLanczosEdge1, kLanczosEdge2};
    const std::uint32_t edge_n[3] = {7, 9, 11};
    for (std::uint32_t x = 0; x < m; ++x) {
        std::int64_t s = 1 << 21;
        if (x < 3) {
            for (std::uint32_t i = 0; i < edge_n[x]; ++i) s += static_cast<std::int64_t>(in[i * si]) * edge[x][i];
        } else if (x + 3 >= m) {
            const std::uint32_t k = m - 1 - x;   // mirrored: the table of x = k, reversed, ending at the last input
            const std::uint32_t len = edge_n[k], start = n - len;
            for (std::uint32_t i = 0; i < len; ++i) s += static_cast<std::int64_t>(in[(start + i) * si]) * edge[k][len - 1 - i];
        } else {
            const std::uint32_t start = 2 * x - 5;
            for (std::uint32_t i = 0; i < 12; ++i) s += static_cast<std::int64_t>(in[(start + i) * si]) * kLanczosInterior[i];
        }
        put(x, s);
    }
}

void PlaneHash(const Image& img, bool rgb, std::vector<std::uint8_t>& buf, Sha256& out) {
    const std::size_t n = static_cast<std::size_t>(img.w) * img.h;
    buf.resize(n * (rgb ? 3 : 1));
    for (std::size_t i = 0; i < n; ++i) {
        if (rgb) {
            buf[i * 3] = img.rgba[i * 4], buf[i * 3 + 1] = img.rgba[i * 4 + 1], buf[i * 3 + 2] = img.rgba[i * 4 + 2];
        } else {
            buf[i] = img.rgba[i * 4];
        }
    }
    HashSha256(buf.data(), buf.size(), out);
}

}  // namespace

const std::int32_t kLanczosInterior[12] = {15473, 63150, -142601, -279497, 568350, 1872276, 1872276, 568350, -279497, -142601, 63150, 15473};
const std::int32_t kLanczosEdge0[7] = {1978344, 1978344, 600549, -295331, -150679, 66728, 16350};
const std::int32_t kLanczosEdge1[9] = {-275298, 559811, 1844147, 1844147, 559811, -275298, -140458, 62201, 15241};
const std::int32_t kLanczosEdge2[11] = {63384, -143129, -280532, 570455, 1879209, 1879209, 570455, -280532, -143129, 63384, 15531};

unsigned ChangedMaps(int colour) {
    if (colour >= 1 && colour <= 14) return kColour;
    if (colour == 15 || colour == 16) return kColour | kReflectance | kRoughness;
    return 0;
}
bool HasDefault(int colour) { return colour >= 1 && colour <= 16; }

Metal MetalOf(int colour, int variant) {
    if (variant < 0 || variant >= kMetalVariants) variant = 0;
    return colour == 15 ? kMetals[variant].silver : kMetals[variant].gold;
}

bool IsFabric(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const int d = static_cast<int>(b) - static_cast<int>(r);
    const std::uint32_t L = Lum(r, g, b);
    const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
    return d >= 10 && d <= 70 && b < 190 && L > 350000u && L < 1750000u && mx - mn < 90;
}

void Median5Binary(const std::uint8_t* in, std::uint8_t* out, std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> col(w);
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint32_t rows[5];
        for (int d = -2; d <= 2; ++d) {
            const std::int64_t yy = static_cast<std::int64_t>(y) + d;
            rows[d + 2] = static_cast<std::uint32_t>(yy < 0 ? 0 : yy >= static_cast<std::int64_t>(h) ? h - 1 : yy);
        }
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t c = 0;
            for (std::uint32_t r : rows) c += in[static_cast<std::size_t>(r) * w + x] == 255 ? 1 : 0;
            col[x] = c;
        }
        // a sliding sum of the five clamped columns: col[clamp(x-2)] .. col[clamp(x+2)]
        std::uint32_t s = 0;
        for (int d = -2; d <= 2; ++d) {
            const std::int64_t xx = d;
            s += col[static_cast<std::size_t>(xx < 0 ? 0 : xx >= static_cast<std::int64_t>(w) ? w - 1 : xx)];
        }
        for (std::uint32_t x = 0; x < w; ++x) {
            out[static_cast<std::size_t>(y) * w + x] = s >= 13 ? 255 : 0;
            // move the window one to the right: drop clamp(x-2), add clamp(x+3)
            const std::int64_t drop = static_cast<std::int64_t>(x) - 2, add = static_cast<std::int64_t>(x) + 3;
            s -= col[static_cast<std::size_t>(drop < 0 ? 0 : drop)];
            s += col[static_cast<std::size_t>(add >= static_cast<std::int64_t>(w) ? w - 1 : add)];
        }
    }
}

void PilGaussianBlur12(std::uint8_t* plane, std::uint32_t w, std::uint32_t h) {
    const std::uint32_t m = std::max(w, h);
    std::vector<std::uint8_t> a(m), b(m);
    // 3 horizontal passes, then 3 vertical ones (Pillow: the same passes on the transposed image), a u8 store each
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint8_t* row = plane + static_cast<std::size_t>(y) * w;
        BlurRow(row, a.data(), w, 1);
        BlurRow(a.data(), b.data(), w, 1);
        BlurRow(b.data(), row, w, 1);
    }
    for (std::uint32_t x = 0; x < w; ++x) {
        for (std::uint32_t y = 0; y < h; ++y) a[y] = plane[static_cast<std::size_t>(y) * w + x];
        BlurRow(a.data(), b.data(), h, 1);
        BlurRow(b.data(), a.data(), h, 1);
        BlurRow(a.data(), b.data(), h, 1);
        for (std::uint32_t y = 0; y < h; ++y) plane[static_cast<std::size_t>(y) * w + x] = b[y];
    }
}

void Lanczos3Half(const std::uint8_t* in, std::uint32_t w, std::uint32_t h, std::uint8_t* out) {
    const std::uint32_t ow = w / 2, oh = h / 2;
    std::vector<std::uint8_t> t(static_cast<std::size_t>(ow) * h);
    for (std::uint32_t y = 0; y < h; ++y) LanczosLine(in + static_cast<std::size_t>(y) * w, 1, w, t.data() + static_cast<std::size_t>(y) * ow, 1);
    for (std::uint32_t x = 0; x < ow; ++x) LanczosLine(t.data() + x, ow, h, out + x, ow);
    (void)oh;
}

bool Analyse(const Image& colour, Analysis& a, std::string* why) {
    a = Analysis{};
    const std::uint32_t w = colour.w, h = colour.h;
    if (w < 14 || h < 14 || (w & 1) || (h & 1) || colour.rgba.size() != static_cast<std::size_t>(w) * h * 4) {
        Fail(why, "her Colour picture has an unexpected size (it must be even and at least 14 pixels a side)");
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(w) * h;
    std::vector<std::uint8_t> m0(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* p = &colour.rgba[i * 4];
        if (!IsFabric(p[0], p[1], p[2])) continue;
        m0[i] = 255;
        ++a.n;
        a.sum[0] += p[0], a.sum[1] += p[1], a.sum[2] += p[2];
        a.sum_lum += Lum(p[0], p[1], p[2]);
    }
    if (a.n < n / 100 || a.sum_lum == 0 || !a.sum[0] || !a.sum[1] || !a.sum[2]) {
        Fail(why, "her suit's navy fabric was not found in her Colour picture: the game's pictures changed");
        a = Analysis{};
        return false;
    }
    a.w = w, a.h = h;
    a.mask.resize(n);
    Median5Binary(m0.data(), a.mask.data(), w, h);
    PilGaussianBlur12(a.mask.data(), w, h);
    // q = min(255, rel16 >> 9): the prototype's uint8(rel * 128)
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* p = &colour.rgba[i * 4];
        std::uint64_t rel16 = static_cast<std::uint64_t>(Lum(p[0], p[1], p[2])) * a.n * 65536u / a.sum_lum;
        rel16 = std::clamp<std::uint64_t>(rel16, 29491u, 104857u);
        m0[i] = static_cast<std::uint8_t>(std::min<std::uint64_t>(255u, rel16 >> 9));
    }
    const std::size_t ns = static_cast<std::size_t>(w / 2) * (h / 2);
    a.mask_s.resize(ns);
    a.rel_s.resize(ns);
    Lanczos3Half(a.mask.data(), w, h, a.mask_s.data());
    Lanczos3Half(m0.data(), w, h, a.rel_s.data());
    return true;
}

bool Make(int colour, int variant, const Analysis& a, const Image& C, const Image& F, const Image& R, Output& out, std::string* why) {
    out = Output{};
    if (!HasDefault(colour)) return Fail(why, "no ready-made colour for this colour number"), false;
    if (variant < 0 || variant >= kMetalVariants) return Fail(why, "unknown metal variant"), false;
    if (a.w == 0 || C.w != a.w || C.h != a.h || C.rgba.size() != static_cast<std::size_t>(a.w) * a.h * 4)
        return Fail(why, "her Colour picture is not the analysed one"), false;
    const std::size_t n = static_cast<std::size_t>(a.w) * a.h;
    out.maps = ChangedMaps(colour);
    out.colour.Resize(a.w, a.h);
    if (colour <= 14) {
        const suitcol::Rgb t = suitcol::kLut[colour];
        const std::uint64_t T[3] = {t.r, t.g, t.b};
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t m = a.mask[i];
            const std::uint8_t* c = &C.rgba[i * 4];
            std::uint8_t* o = &out.colour.rgba[i * 4];
            for (int k = 0; k < 3; ++k) {
                const std::uint64_t S = a.sum[k];
                const std::uint64_t v = c[k] * ((255u - m) * S + m * T[k] * a.n) / (255u * S);
                o[k] = static_cast<std::uint8_t>(std::min<std::uint64_t>(255u, v));
            }
            o[3] = 255;
        }
        return true;
    }
    // metals
    const std::uint32_t ws = a.w / 2, hs = a.h / 2;
    if (F.w != ws || F.h != hs || F.rgba.size() != static_cast<std::size_t>(ws) * hs * 4)
        return Fail(why, "her Reflectance picture is not half the Colour picture's size"), false;
    if (R.w != a.w || R.h != a.h || R.rgba.size() != n * 4) return Fail(why, "her Roughness picture is not the Colour picture's size"), false;
    const Metal mt = MetalOf(colour, variant);
    out.roughness.Resize(a.w, a.h);
    // Roughness x rough_permille / 1000 where the fabric mask is full: r * (255000 - (1000 - p) * m) / 255000, signed (p may be
    // above 1000), clamped to 0..255. For p <= 1000 the same integers as before (the golden hashes).
    const std::int64_t rough_den = 255000, rough_k = 1000 - static_cast<std::int64_t>(mt.rough_permille);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* c = &C.rgba[i * 4];
        std::uint64_t rel16 = static_cast<std::uint64_t>(Lum(c[0], c[1], c[2])) * a.n * 65536u / a.sum_lum;
        rel16 = std::clamp<std::uint64_t>(rel16, 29491u, 104857u);
        const std::uint64_t m = a.mask[i];
        std::uint8_t* o = &out.colour.rgba[i * 4];
        for (int k = 0; k < 3; ++k) {
            const std::uint64_t v = (static_cast<std::uint64_t>(c[k]) * (255u - m) * 65536u + mt.base[k] * rel16 * m) / (255u * 65536u);
            o[k] = static_cast<std::uint8_t>(std::min<std::uint64_t>(255u, v));
        }
        o[3] = 255;
        const std::int64_t r = R.rgba[i * 4];
        const std::int64_t rv = r * (rough_den - rough_k * static_cast<std::int64_t>(m)) / rough_den;
        const std::uint8_t ro = static_cast<std::uint8_t>(rv < 0 ? 0 : rv > 255 ? 255 : rv);
        std::uint8_t* q = &out.roughness.rgba[i * 4];
        q[0] = q[1] = q[2] = ro;
        q[3] = 255;
    }
    out.reflectance.Resize(ws, hs);
    const std::size_t ns = static_cast<std::size_t>(ws) * hs;
    for (std::size_t i = 0; i < ns; ++i) {
        const std::uint64_t ms = a.mask_s[i];
        const std::uint64_t v = std::clamp<std::uint64_t>(10880u + 15u * a.rel_s[i], 8960u, 14080u);
        const std::uint8_t* f = &F.rgba[i * 4];
        std::uint8_t* o = &out.reflectance.rgba[i * 4];
        for (int k = 0; k < 3; ++k) {
            const std::uint64_t x = (static_cast<std::uint64_t>(f[k]) * (255u - ms) * 12800u + mt.shine[k] * v * ms) / (255u * 12800u);
            o[k] = static_cast<std::uint8_t>(std::min<std::uint64_t>(255u, x));
        }
        o[3] = 255;
    }
    return true;
}

Sha256 OriginalsId(const Image& colour, const Image& refl, const Image& rough, const Image& ao) {
    std::string text = "stx-originals|1|";
    std::vector<std::uint8_t> buf;
    const Image* imgs[4] = {&colour, &refl, &rough, &ao};
    for (int i = 0; i < 4; ++i) {
        const Image& img = *imgs[i];
        std::uint8_t wh[8];
        for (int k = 0; k < 4; ++k) wh[k] = static_cast<std::uint8_t>(img.w >> (8 * k)), wh[4 + k] = static_cast<std::uint8_t>(img.h >> (8 * k));
        text.append(reinterpret_cast<const char*>(wh), 8);
        Sha256 ph{};
        if (img.rgba.size() == static_cast<std::size_t>(img.w) * img.h * 4) PlaneHash(img, i < 2, buf, ph);
        text.append(reinterpret_cast<const char*>(ph.data()), ph.size());
    }
    Sha256 out{};
    HashSha256(text.data(), text.size(), out);
    return out;
}

}  // namespace st::tex::defaults
