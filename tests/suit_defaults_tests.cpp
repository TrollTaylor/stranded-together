// Fragile's ready-made suit colours (native/textures/suit_defaults.hpp, docs/textures.md): the integer generator against
// golden hashes computed by its Python model (which matches Pillow's MedianFilter(5), GaussianBlur(1.2) and LANCZOS bit
// for bit) on a synthetic input. No game data is read or written.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "png_wic.hpp"
#include "ready_made_rules.hpp"
#include "suit_colour_math.hpp"
#include "suit_defaults.hpp"
#include "tex_hash.hpp"

using namespace st::tex;
namespace df = st::tex::defaults;

namespace {
int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

std::string HashOf(const std::vector<std::uint8_t>& b) {
    Sha256 d{};
    HashSha256(b.data(), b.size(), d);
    return Hex(d);
}
// The raw planes the Python model hashes: RGB rows, or one channel.
std::vector<std::uint8_t> Rgb(const Image& img) {
    std::vector<std::uint8_t> b;
    for (std::size_t i = 0; i < static_cast<std::size_t>(img.w) * img.h; ++i) b.insert(b.end(), &img.rgba[i * 4], &img.rgba[i * 4] + 3);
    return b;
}
std::vector<std::uint8_t> Grey(const Image& img) {
    std::vector<std::uint8_t> b;
    for (std::size_t i = 0; i < static_cast<std::size_t>(img.w) * img.h; ++i) b.push_back(img.rgba[i * 4]);
    return b;
}

// golden.py's synthetic input: an LCG (seed "STSK"), about 11/16 of the pixels "her navy fabric", varied.
struct Lcg {
    std::uint64_t x;
    std::uint64_t Next() { return x = x * 6364136223846793005ull + 1442695040888963407ull; }
};
void Synth(std::uint32_t W, std::uint32_t H, Image& C, Image& F, Image& R) {
    Lcg g{0x5354534Bull};
    C.Resize(W, H);
    R.Resize(W, H);
    F.Resize(W / 2, H / 2);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::uint64_t v = g.Next();
            std::uint8_t* p = C.At(x, y);
            if ((v >> 60) < 11) {
                p[0] = static_cast<std::uint8_t>(50 + ((v >> 8) & 31));
                p[1] = static_cast<std::uint8_t>(55 + ((v >> 16) & 31));
                p[2] = static_cast<std::uint8_t>(p[0] + 15 + ((v >> 24) & 31));
            } else {
                p[0] = static_cast<std::uint8_t>(v >> 32), p[1] = static_cast<std::uint8_t>(v >> 40), p[2] = static_cast<std::uint8_t>(v >> 48);
            }
            p[3] = 255;
            std::uint8_t* r = R.At(x, y);
            r[0] = r[1] = r[2] = static_cast<std::uint8_t>(v >> 4);
            r[3] = 255;
        }
    for (std::uint32_t y = 0; y < H / 2; ++y)
        for (std::uint32_t x = 0; x < W / 2; ++x) {
            const std::uint64_t v = g.Next();
            std::uint8_t* p = F.At(x, y);
            p[0] = static_cast<std::uint8_t>(v >> 8), p[1] = static_cast<std::uint8_t>(v >> 16), p[2] = static_cast<std::uint8_t>(v >> 24), p[3] = 255;
        }
}

Image Uniform(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    Image img;
    img.Resize(w, h);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) img.rgba[i * 4] = r, img.rgba[i * 4 + 1] = g, img.rgba[i * 4 + 2] = b, img.rgba[i * 4 + 3] = 255;
    return img;
}

void Fabric() {
    // L = 2126 r + 7152 g + 722 b; d = b - r; spread = max - min
    CHECK(df::IsFabric(60, 70, 90));
    CHECK(!df::IsFabric(60, 70, 69) && df::IsFabric(60, 70, 70) && df::IsFabric(60, 70, 130) && !df::IsFabric(60, 70, 131));   // d 9/10/70/71
    CHECK(df::IsFabric(140, 150, 189) && !df::IsFabric(140, 150, 190));                                                        // b 189/190
    // L: (r, g, b) = (0, 47, 15): 336144 + 10830 = 346974 (no); exact boundaries 350000 / 1750000 with integers
    // 2126*r + 7152*g + 722*b == 350000: r=10, g=40, b=? -> 21260 + 286080 = 307340; 42660 / 722 is not whole: use spreads
    CHECK(!df::IsFabric(20, 30, 40));   // L = 42520 + 214560 + 28880 = 285960
    CHECK(df::IsFabric(30, 40, 50));    // L = 63780 + 286080 + 36100 = 385960
    // L == 350000 exactly: 2126*25 + 7152*38 + 722*... -> search a whole triple in the test itself
    int lo_eq = 0, lo_eq_bad = 0, lo_above = 0, lo_above_bad = 0, hi_below = 0, hi_below_bad = 0;
    for (int r = 0; r < 256; ++r)
        for (int g = 0; g < 256; ++g)
            for (int b = r + 10; b <= r + 70 && b < 190; ++b) {
                const int L = 2126 * r + 7152 * g + 722 * b;
                const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
                if (mx - mn >= 90) continue;
                const bool f = df::IsFabric(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b));
                if (L == 350000) ++lo_eq, lo_eq_bad += f ? 1 : 0;
                if (L > 350000 && L < 352000) ++lo_above, lo_above_bad += f ? 0 : 1;
                if (L >= 1750000) hi_below_bad += f ? 1 : 0;   // never fabric at or over 1750000
                if (L > 1748000 && L < 1750000) ++hi_below, hi_below_bad += f ? 0 : 1;
            }
    CHECK(lo_eq > 0 && lo_eq_bad == 0);         // every L == 350000 pixel refused
    CHECK(lo_above > 0 && lo_above_bad == 0);   // every pixel just above taken
    CHECK(hi_below > 0 && hi_below_bad == 0);   // just below 1750000 taken, at or over refused (no whole pixel is exactly 1750000)
    CHECK(df::IsFabric(60, 100, 120) && !df::IsFabric(60, 150, 120));   // spread 60 / 90 (89/90 below)
    CHECK(df::IsFabric(60, 148, 120) && !df::IsFabric(59, 149, 120) == true);   // 148-60 = 88; 149-59 = 90
    CHECK(df::IsFabric(60, 149, 120) && !df::IsFabric(60, 150, 125));   // 89 / 90
}

void Median() {
    const std::uint32_t w = 9, h = 9;
    std::vector<std::uint8_t> in(w * h, 0), out(w * h, 0);
    in[4 * w + 4] = 255;   // a lone pixel goes
    df::Median5Binary(in.data(), out.data(), w, h);
    bool any = false;
    for (std::uint8_t v : out) any |= v != 0;
    CHECK(!any);
    // 12 of the 25 around the centre: off; 13: on
    std::fill(in.begin(), in.end(), std::uint8_t{0});
    int k = 0;
    for (std::uint32_t y = 2; y <= 6 && k < 12; ++y)
        for (std::uint32_t x = 2; x <= 6 && k < 12; ++x) in[y * w + x] = 255, ++k;
    df::Median5Binary(in.data(), out.data(), w, h);
    CHECK(out[4 * w + 4] == 0);
    in[6 * w + 6] = 255;   // the 13th
    df::Median5Binary(in.data(), out.data(), w, h);
    CHECK(out[4 * w + 4] == 255);
    // a corner: the replicated edge counts 9 times
    std::fill(in.begin(), in.end(), std::uint8_t{0});
    in[0] = 255, in[1] = 255;   // (0,0): 9 + 6 = 15 of 25 with replication? (0,0) x3 rows x3 cols = 9, (1,0) x3 rows = 3: 12
    df::Median5Binary(in.data(), out.data(), w, h);
    CHECK(out[0] == 0);
    in[w] = 255;   // (0,1): x3 cols = 3 more: 15
    df::Median5Binary(in.data(), out.data(), w, h);
    CHECK(out[0] == 255);
}

void Blur() {
    // the constants from Pillow 12's float formula (_gaussian_blur_radius(1.2, 3), then 1<<24 / (2r+1))
    const float r = 1.2f;
    const float s2 = r * r / 3.0f;
    const float L = std::sqrt(s2 * 12.0f + 1.0f);
    const float l = std::floor((L - 1.0f) / 2.0f);
    float a = (2.0f * l + 1.0f) * (l * (l + 1.0f) - 3.0f * s2);
    a = a / (6.0f * (s2 - (l + 1.0f) * (l + 1.0f)));
    const float radius = l + a;
    const std::uint32_t ww = static_cast<std::uint32_t>(static_cast<float>(1 << 24) / (radius * 2.0f + 1.0f));
    const std::uint32_t ri = static_cast<std::uint32_t>(radius);
    const std::uint32_t fw = ((1u << 24) - (ri * 2 + 1) * ww) / 2;
    CHECK(ww == df::kBlurCentre && fw == df::kBlurSide);
    // an impulse: symmetric, sums to about the input
    const std::uint32_t n = 15;
    std::vector<std::uint8_t> p(n * n, 0);
    p[7 * n + 7] = 255;
    df::PilGaussianBlur12(p.data(), n, n);
    int sum = 0;
    bool sym = true;
    for (std::uint32_t y = 0; y < n; ++y)
        for (std::uint32_t x = 0; x < n; ++x) {
            sum += p[y * n + x];
            sym = sym && p[y * n + x] == p[(n - 1 - y) * n + x] && p[y * n + x] == p[y * n + (n - 1 - x)];
        }
    std::printf("blur impulse: centre %u, +1 %u, +2 %u, +3 %u, sum %d\n", p[7 * n + 7], p[7 * n + 8], p[7 * n + 9], p[7 * n + 10], sum);
    CHECK(sym && std::abs(sum - 255) <= 40 && p[7 * n + 7] > p[7 * n + 8] && p[7 * n + 8] >= p[7 * n + 9]);
    // a constant stays constant (edges replicated)
    std::vector<std::uint8_t> c(n * n, 200);
    df::PilGaussianBlur12(c.data(), n, n);
    bool same = true;
    for (std::uint8_t v : c) same = same && v == 200;
    CHECK(same);
}

void Lanczos() {
    auto sum = [](const std::int32_t* t, int n) {
        std::int64_t s = 0;
        for (int i = 0; i < n; ++i) s += t[i];
        return s;
    };
    CHECK(std::llabs(sum(df::kLanczosInterior, 12) - (1 << 22)) <= 3 && std::llabs(sum(df::kLanczosEdge0, 7) - (1 << 22)) <= 3 &&
          std::llabs(sum(df::kLanczosEdge1, 9) - (1 << 22)) <= 3 && std::llabs(sum(df::kLanczosEdge2, 11) - (1 << 22)) <= 3);
    bool mirror = true;
    for (int i = 0; i < 6; ++i) mirror = mirror && df::kLanczosInterior[i] == df::kLanczosInterior[11 - i];
    CHECK(mirror);
    // the tables from Pillow's formula (double maths, rounded to 22 bits)
    auto coeffs = [](int insize, int outsize, int xx, std::vector<std::int32_t>& k, int& xmin) {
        const double scale = static_cast<double>(insize) / outsize, support = 3.0 * scale, ss = 1.0 / scale;
        const double pi = 3.14159265358979323846;
        auto sinc = [&](double x) { return x == 0.0 ? 1.0 : std::sin(x * pi) / (x * pi); };
        auto lz = [&](double x) { return (x >= -3.0 && x < 3.0) ? sinc(x) * sinc(x / 3.0) : 0.0; };
        const double center = (xx + 0.5) * scale;
        xmin = static_cast<int>(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        const int xmax = std::min(static_cast<int>(center + support + 0.5), insize) - xmin;
        std::vector<double> kd;
        double ws = 0;
        for (int x = 0; x < xmax; ++x) kd.push_back(lz((x + xmin - center + 0.5) * ss)), ws += kd.back();
        k.clear();
        for (double v : kd) {
            v = ws != 0 ? v / ws : v;
            k.push_back(static_cast<std::int32_t>(v * (1 << 22) + (v >= 0 ? 0.5 : -0.5)));
        }
    };
    std::vector<std::int32_t> k;
    int xmin = 0;
    coeffs(2048, 1024, 0, k, xmin);
    CHECK(xmin == 0 && k == std::vector<std::int32_t>(df::kLanczosEdge0, df::kLanczosEdge0 + 7));
    coeffs(2048, 1024, 1, k, xmin);
    CHECK(xmin == 0 && k == std::vector<std::int32_t>(df::kLanczosEdge1, df::kLanczosEdge1 + 9));
    coeffs(2048, 1024, 2, k, xmin);
    CHECK(xmin == 0 && k == std::vector<std::int32_t>(df::kLanczosEdge2, df::kLanczosEdge2 + 11));
    coeffs(2048, 1024, 500, k, xmin);
    CHECK(xmin == 995 && k == std::vector<std::int32_t>(df::kLanczosInterior, df::kLanczosInterior + 12));
    coeffs(2048, 1024, 1023, k, xmin);
    CHECK(xmin == 2041);
    std::vector<std::int32_t> rev(df::kLanczosEdge0, df::kLanczosEdge0 + 7);
    std::reverse(rev.begin(), rev.end());
    CHECK(k == rev);
    // a constant stays constant
    const std::uint32_t w = 32, h = 20;
    std::vector<std::uint8_t> c(w * h, 77), o(w / 2 * h / 2, 0);
    df::Lanczos3Half(c.data(), w, h, o.data());
    bool same = true;
    for (std::uint8_t v : o) same = same && v == 77;
    CHECK(same);
}

void Golden() {
    Image C, F, R;
    Synth(64, 64, C, F, R);
    {
        std::vector<std::uint8_t> all = Rgb(C);
        const std::vector<std::uint8_t> f = Rgb(F), r = Grey(R);
        all.insert(all.end(), f.begin(), f.end());
        all.insert(all.end(), r.begin(), r.end());
        CHECK(HashOf(all) == "e650a019ca9a689eeabf1769f15484da5a09b98ebaf9dc2926ed1e48eae5a5b7");   // proves the LCG
    }
    df::Analysis a;
    std::string why;
    CHECK(df::Analyse(C, a, &why));
    CHECK(a.n == 2882 && a.sum[0] == 190015 && a.sum[1] == 205397 && a.sum[2] == 278619 && a.sum_lum == 2074134152ull);
    CHECK(HashOf(a.mask) == "4d60b866e7107191505d1311830e2a93cbcd3e148da3fb02c7bec535a929c291");
    CHECK(HashOf(a.mask_s) == "f7e17ff59c7bf2dd996be23e9e3be844317f903e6621343617af26392eb94869");
    CHECK(HashOf(a.rel_s) == "a544b35ed7997cbf02f4e483f311c6432d5511692228c812416c462569afd274");
    struct G {
        int c;
        const char *colour, *refl, *rough;
    };
    const char* pal_refl = "d99e266cb561882412788b117a6409682e6144304d53b9808fd236da38626718";
    const char* pal_rough = "1ef14331186fd15955522ea5f99eb27fce2f972af111f9db0ef45969f845ea9c";
    // generator 2 (round 9): variant 0 is the polished metal (Roughness x1.15, signed and clamped). These hashes come from
    // the independent numpy integer model (round-9 scratch verify/golden9.py, the round-8 Pillow-exact median / blur /
    // Lanczos models with the round-9 variant table), not from this code.
    const char* metal_rough = "9d7acbc10c7ca2057e1356aabd6442a0bb70914073a90c243001804b1579215b";
    const G golden[] = {
        {1, "f1f4e9376113c208010e95a97804e116885b056d3d32690dffb44088f7259813", nullptr, nullptr},
        {9, "b5d642ab64bcfffcf6746f652e59fb3cac67f7fee3d568c90a13ae0e76200dfa", nullptr, nullptr},
        {13, "046c28461891b5cc85fa9a0a07d8ed4d79b614252e8b160296ea1786d4365700", nullptr, nullptr},
        {15, "365328f2cd3c4f13e9f389f3fa56c0f5c051f0ab3acc129f4b2335421a6d8bfb", "43e53de7c2b01c0e0bad32ba3c396276871ed264fa04b3e8877e08d3bc2d784a", metal_rough},
        {16, "5d3a9cae059af04db4d4475ae57fa5b94ec1b244740a880976082ce43b756189", "589abc8ca6db690cace82dc14bb1defde1375adfe6e7704910b94d7a00cc4c1b", metal_rough},
    };
    CHECK(HashOf(Rgb(F)) == pal_refl && HashOf(Grey(R)) == pal_rough);   // the palette leaves these as they are
    for (const G& g : golden) {
        df::Output o;
        CHECK(df::Make(g.c, 0, a, C, F, R, o, &why));
        CHECK(HashOf(Rgb(o.colour)) == g.colour);
        if (g.refl) {
            CHECK(o.maps == (df::kColour | df::kReflectance | df::kRoughness));
            CHECK(HashOf(Rgb(o.reflectance)) == g.refl && HashOf(Grey(o.roughness)) == g.rough);
        } else {
            CHECK(o.maps == df::kColour && o.reflectance.w == 0 && o.roughness.w == 0);
        }
        // determinism: twice the same bytes
        df::Output o2;
        CHECK(df::Make(g.c, 0, a, C, F, R, o2, &why) && o2.colour.rgba == o.colour.rgba && o2.reflectance.rgba == o.reflectance.rgba);
    }
    // the analysis is reused by every colour; a fresh one is identical
    df::Analysis b;
    CHECK(df::Analyse(C, b, &why) && b.mask == a.mask && b.mask_s == a.mask_s && b.rel_s == a.rel_s);
    // non-fabric pixels far from the fabric keep their bytes (mask 0); every pixel's alpha is 255
    df::Output o;
    CHECK(df::Make(13, 0, a, C, F, R, o, &why));
    bool kept = true, opaque = true;
    for (std::size_t i = 0; i < a.mask.size(); ++i) {
        if (a.mask[i] == 0) kept = kept && std::memcmp(&o.colour.rgba[i * 4], &C.rgba[i * 4], 3) == 0;
        opaque = opaque && o.colour.rgba[i * 4 + 3] == 255;
    }
    CHECK(kept && opaque);
    // every metal variant, from the same model: 1 "Sam's hue", 2 "Sam measured", 3 the round-7 prototype (generator 1's
    // product: its hashes are exactly round 8's variant-0 golden hashes, so the old look stays reachable byte for byte)
    {
        struct V {
            int v, c;
            const char *colour, *refl, *rough;
        };
        const char* r1 = "4980744a580e6a1fa8a70a4e10120c9783d2d70aa6a67525a60cefa79cfc4ddd";
        const char* r75 = "de4c40053fbf8e6aa0a45947df2017e6137ab34138b1aaba85094e1b74c09174";
        const V vg[] = {
            {1, 15, "d596424f09492f9255353fc593271678d2bf5a4415f908b8051fa134b82c0133", "fe4d18ce423f127a2bdf2c30ae33a40c825f088a00eea42a34b38d71a5a72bcf", r1},
            {1, 16, "791b068bfa9e0e1cda184e876c0baf301130919f63d55e59eada02be4ad5c071", "872ecb58b3c195f2870d4674acbb5d8c4e02b780c9cd6f7ee9dee8e0208ac788", r1},
            {2, 15, "6ae28a1bdeeeef9836814d4d7b06aefb11f2c642ec4f14bbbc461c4a5c26aadc", "ae80b725fc8018a2bb4cd3827f8f7237e99e087841252cb3f577c193f9183fe5", r75},
            {2, 16, "114cd7c2c4d102f2243336b42f2b8cd1e923bea66deb869b40eaadcd2260e77c", "f287f8820008c1efa382468ceeef1682d48a43d7c23b3b93f43279607a56e1f0", r75},
            {3, 15, "98d1f52429684babfe1054c631b9c96d2d87bece8a449b0dbf769c2fa1cbbe0f", "50ac6a23c322d323efc16d8f61d96092a6426ec8299998042c5900ca2345f87d", r75},
            {3, 16, "bd3cd7c7c1e2a0844e35ffcfb5a2c3514344f35110107cbdaa904f23f72d6593", "a301db3424414f0ba5806c157f1ee5c0ca69f01982eef37cddf04eed676ed30f", r75},
        };
        for (const V& g : vg) {
            df::Output x;
            CHECK(df::Make(g.c, g.v, a, C, F, R, x, &why));
            CHECK(HashOf(Rgb(x.colour)) == g.colour && HashOf(Rgb(x.reflectance)) == g.refl && HashOf(Grey(x.roughness)) == g.rough);
        }
        CHECK(df::MetalOf(16, 3).rough_permille == 750 && df::MetalOf(16, 3).base[0] == 58 && df::MetalOf(15, 3).shine[0] == 235);
    }
    // the tuning variants change only the metals
    df::Output v1;
    CHECK(df::Make(16, 1, a, C, F, R, v1, &why) && v1.colour.rgba != o.colour.rgba);
    CHECK(df::Make(16, 2, a, C, F, R, v1, &why) && !df::Make(16, df::kMetalVariants, a, C, F, R, v1, &why));
    // variant 0 (the product since generator 2): rough_permille above 1000 (a glossier Roughness on her suit): x1.15 where
    // the mask is full, clamped to 255, never wrapped around
    df::Output v0;
    CHECK(df::Make(16, 0, a, C, F, R, v0, &why) && df::MetalOf(16, 0).rough_permille == 1150 && df::MetalOf(15, 0).rough_permille == 1150);
    bool scaled = true, nowrap = true, clamped = false;
    for (std::size_t i = 0; i < a.mask.size(); ++i) {
        const int r = R.rgba[i * 4], v = v0.roughness.rgba[i * 4];
        if (a.mask[i] == 255) scaled = scaled && v == std::min(255, r * 115 / 100);
        if (a.mask[i] == 255 && r * 115 / 100 > 255) clamped = clamped || v == 255;
        nowrap = nowrap && v >= r;
    }
    CHECK(scaled && nowrap && clamped);
    CHECK(df::MetalOf(16, 0).base[0] == 40 && df::MetalOf(16, 0).base[1] == 36 && df::MetalOf(16, 0).base[2] == 31);
    CHECK(df::MetalOf(16, 0).shine[0] == 232 && df::MetalOf(16, 0).shine[1] == 185 && df::MetalOf(16, 0).shine[2] == 118);
    CHECK(df::MetalOf(15, 0).base[2] == 41 && df::MetalOf(15, 0).shine[1] == 231);
    static_assert(df::kGeneratorVersion == 2, "new golden hashes for a new generator");
}

void Uniforms() {
    // a uniform navy fabric plane: the palette colour becomes exactly the swatch (within 1); the metals their base
    const Image C = Uniform(32, 32, 60, 70, 100), F = Uniform(16, 16, 40, 40, 40), R = Uniform(32, 32, 200, 200, 200);
    df::Analysis a;
    std::string why;
    CHECK(df::Analyse(C, a, &why) && a.n == 32 * 32);
    for (std::uint8_t m : a.mask) CHECK(m == 255);
    for (int c = 1; c <= 14; ++c) {
        df::Output o;
        CHECK(df::Make(c, 0, a, C, F, R, o, &why));
        const st::suitcol::Rgb t = st::suitcol::kLut[c];
        const std::uint8_t* p = o.colour.At(10, 10);
        CHECK(std::abs(p[0] - t.r) <= 1 && std::abs(p[1] - t.g) <= 1 && std::abs(p[2] - t.b) <= 1);
    }
    for (int c = 15; c <= 16; ++c) {
        df::Output o;
        CHECK(df::Make(c, 0, a, C, F, R, o, &why));
        const df::Metal m = df::MetalOf(c, 0);
        const std::uint8_t* p = o.colour.At(10, 10);   // rel = 1
        CHECK(std::abs(p[0] - m.base[0]) <= 1 && std::abs(p[1] - m.base[1]) <= 1 && std::abs(p[2] - m.base[2]) <= 1);
        const std::uint8_t* s = o.reflectance.At(8, 8);   // v = 0.85 + 0.15 * 1.0 = 1.0
        CHECK(std::abs(s[0] - m.shine[0]) <= 1 && std::abs(s[1] - m.shine[1]) <= 1 && std::abs(s[2] - m.shine[2]) <= 1);
        CHECK(o.roughness.At(5, 5)[0] == 230);   // floor(200 * 1.15) (variant 0 since generator 2)
        df::Output proto;
        CHECK(df::Make(c, 3, a, C, F, R, proto, &why) && proto.roughness.At(5, 5)[0] == 150);   // the prototype: floor(200 * 0.75)
    }
    // rel clamps at 0.45 / 1.6: a bright and a dark pixel inside the fabric
    Image C2 = C;
    std::uint8_t* hi = C2.At(16, 16);
    hi[0] = 120, hi[1] = 160, hi[2] = 170;   // lum about 2.3x her fabric's (not fabric: b - r 50 but L over 1.75e6? 255120+1144320+122740 = 1522180: fabric)
    std::uint8_t* lo = C2.At(4, 4);
    lo[0] = 10, lo[1] = 10, lo[2] = 30;   // not fabric (L too low)
    CHECK(df::Analyse(C2, a, &why));
    df::Output o;
    CHECK(df::Make(16, 0, a, C2, F, R, o, &why));
    const df::Metal gold = df::MetalOf(16, 0);
    CHECK(a.mask[4 * 32 + 4] == 255 && a.mask[16 * 32 + 16] == 255);
    CHECK(o.colour.At(4, 4)[0] == gold.base[0] * 29491u / 65536u && o.colour.At(16, 16)[0] == gold.base[0] * 104857u / 65536u);
}

void Refusals() {
    df::Analysis a;
    std::string why;
    CHECK(!df::Analyse(Uniform(15, 16, 60, 70, 100), a, &why) && !why.empty());   // odd
    CHECK(!df::Analyse(Uniform(12, 12, 60, 70, 100), a, &why) && !why.empty());   // too small
    CHECK(!df::Analyse(Uniform(32, 32, 200, 30, 20), a, &why) && why.find("navy fabric was not found") != std::string::npos);
    const Image C = Uniform(32, 32, 60, 70, 100), F = Uniform(16, 16, 40, 40, 40), R = Uniform(32, 32, 200, 200, 200);
    CHECK(df::Analyse(C, a, &why));
    df::Output o;
    CHECK(!df::Make(0, 0, a, C, F, R, o, &why) && !df::Make(17, 0, a, C, F, R, o, &why));
    CHECK(!df::Make(16, 0, a, C, Uniform(32, 32, 1, 1, 1), R, o, &why) && why.find("Reflectance") != std::string::npos);
    CHECK(!df::Make(15, 0, a, C, F, Uniform(16, 16, 1, 1, 1), o, &why) && why.find("Roughness") != std::string::npos);
    CHECK(df::Make(5, 0, a, C, Image{}, Image{}, o, &why));   // the palette needs only the Colour picture
    CHECK(!df::Make(5, 0, a, Uniform(30, 30, 60, 70, 100), F, R, o, &why));
    CHECK(df::ChangedMaps(0) == 0 && df::ChangedMaps(7) == df::kColour && df::ChangedMaps(16) == 7u && !df::HasDefault(0) && df::HasDefault(16));
}

void Identity() {
    Image C, F, R;
    Synth(64, 64, C, F, R);
    const Image ao = Uniform(32, 32, 9, 9, 9);
    const Sha256 h1 = df::OriginalsId(C, F, R, ao), h2 = df::OriginalsId(C, F, R, ao);
    CHECK(h1 == h2);
    Image C2 = C;
    C2.rgba[100] ^= 1;
    CHECK(df::OriginalsId(C2, F, R, ao) != h1);
    // the pixels count, not the PNG bytes: written as rgba32 or rgb24, decoded again: the same H
    std::vector<std::uint8_t> p1, p2;
    CHECK(EncodePng(C, PngLayout::rgb24, p1) && EncodePng(C, PngLayout::rgba32, p2) && p1 != p2);
    Image d1, d2;
    CHECK(DecodePng(p1.data(), p1.size(), d1) && DecodePng(p2.data(), p2.size(), d2));
    CHECK(df::OriginalsId(d1, F, R, ao) == h1 && df::OriginalsId(d2, F, R, ao) == h1);
}

// The mod's folder rule (ready_made_rules.hpp), as a pure function.
void Rules() {
    namespace rm = st::tex::readymade;
    using St = rm::PictureState;
    const St M = St::missing, U = St::unedited, E = St::edited;
    // Colour, Reflectance, Roughness, Ambient occlusion, Normal, Mask
    const St none[rm::kPictures] = {M, M, M, M, M, M};
    const St copies[rm::kPictures] = {U, U, U, M, M, M};
    const St painted[rm::kPictures] = {E, U, U, M, M, M};
    const St ao[rm::kPictures] = {U, U, U, E, M, M};            // only her Ambient occlusion painted
    const St normal[rm::kPictures] = {M, M, M, M, E, M};        // only a Normal picture copied in and painted
    const St refl[rm::kPictures] = {U, E, U, M, M, M};          // only the Reflectance painted
    CHECK(rm::Classify(copies) == rm::FolderState::ready_made && rm::Classify(none) == rm::FolderState::ready_made);
    CHECK(rm::Classify(painted) == rm::FolderState::yours && rm::Classify(ao) == rm::FolderState::yours && rm::Classify(normal) == rm::FolderState::yours);
    CHECK(rm::SlotPictures(rm::kSlotS1) == ((1u << rm::kReflectancePic) | (1u << rm::kAoPic)) && rm::SlotMaps(rm::kSlotS2) == 0);
    // a v0.24 folder (Original copies, unedited): ready-made
    CHECK(rm::SourceFor(16, rm::kSlotS0, rm::Classify(copies), true, copies) == rm::SlotSource::ready_made);
    CHECK(rm::SourceFor(5, rm::kSlotS1, rm::Classify(copies), true, copies) == rm::SlotSource::own);   // palette: S1 untouched
    CHECK(rm::SourceFor(16, rm::kSlotS2, rm::Classify(copies), true, copies) == rm::SlotSource::own);
    // a yours folder: every slot from its files (Original for a missing one); the unedited ones may be the ready-made slot
    // byte for byte (compared by the mod, never assumed from the manifest's version)
    for (int k = rm::kSlotS0; k <= rm::kSlotS3; ++k) CHECK(rm::SourceFor(16, k, rm::FolderState::yours, true, painted) == rm::SlotSource::files);
    CHECK(!rm::MayBeReadyMade(16, rm::kSlotS0, rm::FolderState::yours, true, painted));
    CHECK(rm::MayBeReadyMade(16, rm::kSlotS1, rm::FolderState::yours, true, painted));
    CHECK(!rm::MayBeReadyMade(5, rm::kSlotS1, rm::FolderState::yours, true, painted));   // a palette colour never changes S1
    CHECK(!rm::MayBeReadyMade(16, rm::kSlotS1, rm::FolderState::ready_made, true, copies) && !rm::MayBeReadyMade(16, rm::kSlotS1, rm::FolderState::yours, false, painted));
    // only her Ambient occlusion painted (05): S1 from the files (the painted AO), S0 may be the ready-made grey
    CHECK(rm::SourceFor(5, rm::kSlotS1, rm::Classify(ao), true, ao) == rm::SlotSource::files);
    CHECK(rm::MayBeReadyMade(5, rm::kSlotS0, rm::Classify(ao), true, ao) && !rm::MayBeReadyMade(5, rm::kSlotS1, rm::Classify(ao), true, ao));
    // only a Normal picture painted: S2 from the files
    CHECK(rm::SourceFor(9, rm::kSlotS2, rm::Classify(normal), true, normal) == rm::SlotSource::files);
    // only the Reflectance painted in 05: S0 (her navy copy or the ready-made grey, whatever the file is) from the files
    CHECK(rm::SourceFor(5, rm::kSlotS0, rm::Classify(refl), true, refl) == rm::SlotSource::files && rm::MayBeReadyMade(5, rm::kSlotS0, rm::Classify(refl), true, refl));
    // defaults off, or colour 0: the v0.24 rule (files when a picture of the slot is edited, else her own)
    CHECK(rm::SourceFor(16, rm::kSlotS0, rm::Classify(copies), false, copies) == rm::SlotSource::own);
    CHECK(rm::SourceFor(0, rm::kSlotS0, rm::Classify(copies), true, copies) == rm::SlotSource::own);
    CHECK(rm::SourceFor(0, rm::kSlotS0, rm::Classify(painted), true, painted) == rm::SlotSource::files);
    CHECK(rm::SourceFor(0, rm::kSlotS1, rm::Classify(ao), true, ao) == rm::SlotSource::files && rm::SourceFor(0, rm::kSlotS0, rm::Classify(ao), true, ao) == rm::SlotSource::own);
}

void Bench() {
    // a synthetic 2048 / 1024 set (prints only)
    Image C, F, R;
    C.Resize(2048, 2048);
    R.Resize(2048, 2048);
    F.Resize(1024, 1024);
    std::uint32_t s = 7;
    for (std::size_t i = 0; i < C.rgba.size(); i += 4) {
        s = s * 1664525u + 1013904223u;
        const bool fab = (s >> 28) < 9;
        C.rgba[i] = static_cast<std::uint8_t>(fab ? 60 + (s >> 8 & 15) : s >> 8), C.rgba[i + 1] = static_cast<std::uint8_t>(fab ? 70 + (s >> 12 & 15) : s >> 16);
        C.rgba[i + 2] = static_cast<std::uint8_t>(fab ? 100 + (s >> 16 & 15) : s >> 20), C.rgba[i + 3] = 255;
        R.rgba[i] = R.rgba[i + 1] = R.rgba[i + 2] = static_cast<std::uint8_t>(s >> 4), R.rgba[i + 3] = 255;
    }
    for (std::size_t i = 0; i < F.rgba.size(); i += 4) F.rgba[i] = F.rgba[i + 1] = F.rgba[i + 2] = 50, F.rgba[i + 3] = 255;
    const auto t0 = std::chrono::steady_clock::now();
    df::Analysis a;
    CHECK(df::Analyse(C, a));
    const auto t1 = std::chrono::steady_clock::now();
    for (int c = 1; c <= 16; ++c) {
        df::Output o;
        CHECK(df::Make(c, 0, a, C, F, R, o));
    }
    const auto t2 = std::chrono::steady_clock::now();
    std::printf("bench: analysis %lld ms, 16 colours %lld ms (%.1f ms a colour)\n",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()),
                std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / 16000.0);
}

}  // namespace

int main() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Fabric();
    Median();
    Blur();
    Lanczos();
    Golden();
    Uniforms();
    Refusals();
    Identity();
    Rules();
    Bench();
    if (SUCCEEDED(co)) CoUninitialize();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
