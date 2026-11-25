// Stranded Together textures: the Fragile Express logo recipe (logo_recipe.hpp).
#include "logo_recipe.hpp"

#include <cstring>

namespace st::tex::logo {
namespace {

bool Fail(std::string* why, const char* w) {
    if (why) *why = w;
    return false;
}

// Weights of an exact-area resample of n source pixels to m: per destination pixel, the first source pixel it covers and
// the length covered of each (in 1/m of a source pixel); every destination's weights add up to n.
struct Span {
    std::uint32_t first = 0;
    std::vector<std::uint32_t> w;
};
std::vector<Span> Spans(std::uint32_t n, std::uint32_t m) {
    std::vector<Span> out(m);
    for (std::uint32_t i = 0; i < m; ++i) {
        const std::uint64_t a0 = static_cast<std::uint64_t>(i) * n, a1 = static_cast<std::uint64_t>(i + 1) * n;
        std::uint64_t s1 = (a1 + m - 1) / m;
        if (s1 > n) s1 = n;
        out[i].first = static_cast<std::uint32_t>(a0 / m);
        for (std::uint64_t s = a0 / m; s < s1; ++s) {
            const std::uint64_t lo = a0 > s * m ? a0 : s * m;
            const std::uint64_t hi = a1 < (s + 1) * m ? a1 : (s + 1) * m;
            out[i].w.push_back(hi > lo ? static_cast<std::uint32_t>(hi - lo) : 0u);
        }
    }
    return out;
}

std::uint8_t Mix(std::uint32_t base, std::uint32_t target, std::uint32_t a) {
    return static_cast<std::uint8_t>((base * (255u - a) + target * a + 127u) / 255u);
}

// Copies the rectangle r from dy rows away (all four channels; the constants keep it inside the picture). The rows are
// copied in the order that reads every source row before it is written (the source may overlap the rectangle).
void CopyRect(Image* img, const Rect& r, int dy) {
    if (!img) return;
    const std::uint32_t n = r.y1 - r.y0;
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::uint32_t y = dy > 0 ? r.y0 + k : r.y1 - 1 - k;
        std::memmove(img->At(r.x0, y), img->At(r.x0, static_cast<std::uint32_t>(static_cast<int>(y) + dy)), (r.x1 - r.x0) * 4u);
    }
}

// The mean of each channel of `img` over the pixels of `box` where `letters` R < 40 (the old print's core). False: none.
bool MeanUnder(const Image& img, const Image& letters, const Rect& box, std::uint32_t mean[4]) {
    std::uint64_t sum[4] = {}, n = 0;
    for (std::uint32_t y = box.y0; y < box.y1; ++y)
        for (std::uint32_t x = box.x0; x < box.x1; ++x)
            if (letters.At(x, y)[0] < 40) {
                const std::uint8_t* p = img.At(x, y);
                for (int c = 0; c < 4; ++c) sum[c] += p[c];
                ++n;
            }
    if (!n) return false;
    for (int c = 0; c < 4; ++c) mean[c] = static_cast<std::uint32_t>((sum[c] + n / 2) / n);
    return true;
}

// Columns [c0, c1) of `cov` laid with their first column at (x, y): channel c of `img` towards target[c] where use[c].
void Lay(Image* img, const Coverage& cov, std::uint32_t c0, std::uint32_t c1, std::uint32_t x, std::uint32_t y, const std::uint32_t target[4],
         const bool use[4]) {
    for (std::uint32_t j = 0; j < cov.h; ++j)
        for (std::uint32_t i = c0; i < c1; ++i) {
            const std::uint32_t a = cov.At(i, j);
            if (!a) continue;
            std::uint8_t* p = img->At(x + (i - c0), y + j);
            for (int c = 0; c < 4; ++c)
                if (use[c]) p[c] = Mix(p[c], target[c], a);
        }
}

bool Square(const Image* img, std::uint32_t side) {
    return !img || (img->w == side && img->h == side && img->rgba.size() == static_cast<std::size_t>(side) * side * 4u);
}

}  // namespace

Coverage AreaResize(const Coverage& src, std::uint32_t w, std::uint32_t h) {
    Coverage out;
    if (!src.w || !src.h || !w || !h || src.a.size() != static_cast<std::size_t>(src.w) * src.h) return out;
    const std::vector<Span> sx = Spans(src.w, w), sy = Spans(src.h, h);
    std::vector<std::uint64_t> row(static_cast<std::size_t>(src.h) * w);  // the horizontal pass (weights add up to src.w)
    for (std::uint32_t y = 0; y < src.h; ++y)
        for (std::uint32_t i = 0; i < w; ++i) {
            std::uint64_t s = 0;
            for (std::size_t k = 0; k < sx[i].w.size(); ++k) s += static_cast<std::uint64_t>(sx[i].w[k]) * src.At(sx[i].first + static_cast<std::uint32_t>(k), y);
            row[static_cast<std::size_t>(y) * w + i] = s;
        }
    out.w = w;
    out.h = h;
    out.a.resize(static_cast<std::size_t>(w) * h);
    const std::uint64_t total = static_cast<std::uint64_t>(src.w) * src.h;
    for (std::uint32_t j = 0; j < h; ++j)
        for (std::uint32_t i = 0; i < w; ++i) {
            std::uint64_t s = 0;
            for (std::size_t k = 0; k < sy[j].w.size(); ++k) s += sy[j].w[k] * row[(static_cast<std::size_t>(sy[j].first) + k) * w + i];
            const std::uint64_t v = (s + total / 2) / total;
            out.a[static_cast<std::size_t>(j) * w + i] = static_cast<std::uint8_t>(v > 255 ? 255 : v);
        }
    return out;
}

std::uint32_t WordHeight(std::uint32_t w) {
    const std::uint32_t ww = kWord.x1 - kWord.x0, wh = kWord.y1 - kWord.y0;
    return (w * wh + ww / 2) / ww;
}

std::uint32_t CapSplit() {
    const std::uint32_t ww = kWord.x1 - kWord.x0;
    return ((kWordSplitX - kWord.x0) * kCapWordW + ww / 2) / ww;
}

bool Wordmark(const Image& decal, std::uint32_t w, std::uint32_t h, Coverage* out, std::string* why) {
    if (!Square(&decal, kDecalSide)) return Fail(why, "the decal picture is not 1024 x 1024");
    Coverage src;
    src.w = kWord.x1 - kWord.x0;
    src.h = kWord.y1 - kWord.y0;
    src.a.resize(static_cast<std::size_t>(src.w) * src.h);
    for (std::uint32_t y = 0; y < src.h; ++y)
        for (std::uint32_t x = 0; x < src.w; ++x) src.a[static_cast<std::size_t>(y) * src.w + x] = decal.At(kWord.x0 + x, kWord.y0 + y)[0];
    *out = AreaResize(src, w, h);
    std::uint32_t peak = 0;
    for (std::uint8_t v : out->a) peak = v > peak ? v : peak;
    if (peak < 128) return Fail(why, "the decal picture has no wordmark where Fragile's decal has it");
    return true;
}

bool StampHood(Image* colour, Image* mask, Image* normal, const Image& palette_mask, const Image& decal, std::string* why) {
    if (!colour || !mask || !Square(colour, kHoodSide) || !Square(mask, kHoodSide) || !Square(normal, kHoodSide) ||
        !Square(&palette_mask, kHoodSide))
        return Fail(why, "a hood picture is not 2048 x 2048");
    std::uint32_t ink[4] = {}, ink_mask[4] = {};
    if (!MeanUnder(*colour, palette_mask, kHoodLetters, ink) || !MeanUnder(*mask, palette_mask, kHoodLetters, ink_mask))
        return Fail(why, "no BRIDGES print where the hood has it (its mask alpha is not 0 there)");
    const std::uint32_t h = WordHeight(kHoodWordW);
    Coverage cov;
    if (!Wordmark(decal, kHoodWordW, h, &cov, why)) return false;
    CopyRect(colour, kHoodErase, kHoodFromDy);
    CopyRect(mask, kHoodErase, kHoodFromDy);
    CopyRect(normal, kHoodErase, kHoodFromDy);
    const std::uint32_t x = kHoodWordCx - kHoodWordW / 2, y = kHoodWordCy - h / 2;
    const bool rgb[4] = {true, true, true, false};
    Lay(colour, cov, 0, cov.w, x, y, ink, rgb);
    const std::uint32_t m[4] = {0, ink_mask[1], 0, 0};   // mask alpha 0 (untinted, like the old print), the old print's roughness
    const bool rg[4] = {true, true, false, false};
    Lay(mask, cov, 0, cov.w, x, y, m, rg);
    return true;
}

bool StampHoodReflectance(Image* refl, const Image& palette_mask, const Image& decal, std::string* why) {
    if (!refl || !Square(refl, kReflSide) || !Square(&palette_mask, kHoodSide)) return Fail(why, "a hood reflectance picture is not 1024 x 1024 (its mask 2048)");
    Image letters;   // the palette mask halved (R: 2 x 2 means) over the letters' box; 255 elsewhere
    letters.Resize(kReflSide, kReflSide);
    std::memset(letters.rgba.data(), 255, letters.rgba.size());
    for (std::uint32_t y = kReflLetters.y0; y < kReflLetters.y1; ++y)
        for (std::uint32_t x = kReflLetters.x0; x < kReflLetters.x1; ++x) {
            const std::uint32_t s = palette_mask.At(2 * x, 2 * y)[0] + palette_mask.At(2 * x + 1, 2 * y)[0] + palette_mask.At(2 * x, 2 * y + 1)[0] +
                                    palette_mask.At(2 * x + 1, 2 * y + 1)[0];
            letters.At(x, y)[0] = static_cast<std::uint8_t>((s + 2) / 4);
        }
    std::uint32_t ink[4] = {};
    if (!MeanUnder(*refl, letters, kReflLetters, ink)) return Fail(why, "no BRIDGES print where the hood has it (its mask alpha is not 0 there)");
    const std::uint32_t h = WordHeight(kReflWordW);
    Coverage cov;
    if (!Wordmark(decal, kReflWordW, h, &cov, why)) return false;
    CopyRect(refl, kReflErase, kReflFromDy);
    const bool rgb[4] = {true, true, true, false};
    Lay(refl, cov, 0, cov.w, kReflWordCx - kReflWordW / 2, kReflWordCy - h / 2, ink, rgb);
    return true;
}

bool StampCap(Image* colour, bool alpha, Image* normal, const Image& letters, const Image& decal, std::string* why) {
    if (!colour || !Square(colour, kCapSide) || !Square(normal, kCapSide) || !Square(&letters, kCapSide))
        return Fail(why, "a cap picture is not 1024 x 1024");
    std::uint32_t ink[4] = {};
    if (!MeanUnder(*colour, letters, kCapLetters, ink)) return Fail(why, "no BRIDGES print where the cap has it (its mask alpha is not 0 there)");
    const std::uint32_t h = WordHeight(kCapWordW);
    Coverage cov;
    if (!Wordmark(decal, kCapWordW, h, &cov, why)) return false;
    CopyRect(colour, kCapErase, kCapFromDy);
    CopyRect(normal, kCapErase, kCapFromDy);
    const std::uint32_t split = CapSplit();
    const std::uint32_t y = kCapWordCy - h / 2;
    const bool use[4] = {true, true, true, alpha};
    Lay(colour, cov, 0, split, kCapPanelAEnd - split, y, ink, use);
    Lay(colour, cov, split, cov.w, kCapPanelB, y, ink, use);
    return true;
}

}  // namespace st::tex::logo
