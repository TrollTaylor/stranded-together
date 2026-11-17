#include "game_font_raster.hpp"

#include <cmath>
#include <vector>

// A private, static copy of stb_truetype (the repo's imgui copy): only its rasterizer is used.
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4245 4267 4365 4456 4457 4505 4701 4702 4706 6011 6262 6385 6386)
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#pragma warning(pop)

namespace st::gamefont {
namespace {

// Outline points are kept at 1/4 font unit in stbtt_vertex's 16-bit fields (font units are ~1000 per
// line, so +-8191 units fit).
constexpr float kSub = 4.0f;

short Q(float v) {
    const float s = std::round(v * kSub);
    return static_cast<short>(s > 32767.0f ? 32767.0f : (s < -32767.0f ? -32767.0f : s));
}

void Push(std::vector<stbtt_vertex>& v, unsigned char type, float x, float y, float cx = 0, float cy = 0) {
    stbtt_vertex e{};
    e.type = type;
    e.x = Q(x), e.y = Q(y), e.cx = Q(cx), e.cy = Q(cy);
    v.push_back(e);
}

bool BuildVertices(const Font& f, const Glyph& g, std::vector<stbtt_vertex>& v) {
    v.clear();
    for (std::uint32_t c = 0; c < g.n_contours; ++c) {
        const std::size_t ci = static_cast<std::size_t>(g.first_contour) + c;
        if (ci >= f.contours.size()) return false;
        const Contour& ct = f.contours[ci];
        if (static_cast<std::size_t>(ct.first_pt + ct.n_pt) * 2 > f.pts.size() || ct.first_cmd + ct.n_cmd > f.cmds.size()) return false;
        const float* p = &f.pts[static_cast<std::size_t>(ct.first_pt) * 2];
        auto X = [&](std::uint32_t i) { return p[i * 2]; };
        auto Y = [&](std::uint32_t i) { return p[i * 2 + 1]; };
        Push(v, STBTT_vmove, X(0), Y(0));
        std::uint32_t i = 0;
        for (std::uint32_t q = 0; q < ct.n_cmd; ++q) {
            const std::uint8_t b = f.cmds[ct.first_cmd + q];
            const std::uint32_t type = b >> 6, k = b & 63u;
            if (i + k >= ct.n_pt) return false;
            if (type == 0 || k == 1) {
                for (std::uint32_t j = 1; j <= k; ++j) Push(v, STBTT_vline, X(i + j), Y(i + j));
            } else {
                // k - 1 off-curve controls with implied midpoints, then the on-curve end point.
                for (std::uint32_t j = 1; j < k; ++j) {
                    const std::uint32_t cpi = i + j;
                    float nx, ny;
                    if (j == k - 1) nx = X(i + k), ny = Y(i + k);
                    else nx = 0.5f * (X(cpi) + X(cpi + 1)), ny = 0.5f * (Y(cpi) + Y(cpi + 1));
                    Push(v, STBTT_vcurve, nx, ny, X(cpi), Y(cpi));
                }
            }
            i += k;
        }
    }
    return true;
}

}  // namespace

PixelBox GlyphBox(const Glyph& g, float sx, float sy, float shift_x, float shift_y) noexcept {
    PixelBox b;
    if (!g.n_contours) return b;
    b.x0 = static_cast<int>(std::floor(g.min_x * sx + shift_x));
    b.y0 = static_cast<int>(std::floor(-g.max_y * sy + shift_y));
    b.x1 = static_cast<int>(std::ceil(g.max_x * sx + shift_x));
    b.y1 = static_cast<int>(std::ceil(-g.min_y * sy + shift_y));
    return b;
}

bool RasterGlyph(const Font& f, const Glyph& g, float sx, float sy, float shift_x, float shift_y, int oversample_h, int oversample_v,
                 unsigned char* out, int w, int h, int stride, float* sub_x, float* sub_y) noexcept {
    try {
        if (oversample_h < 1 || oversample_h > STBTT_MAX_OVERSAMPLE || oversample_v < 1 || oversample_v > STBTT_MAX_OVERSAMPLE) return false;
        std::vector<stbtt_vertex> v;
        if (!BuildVertices(f, g, v)) return false;
        const PixelBox b = GlyphBox(g, sx, sy, shift_x, shift_y);
        stbtt__bitmap bm;
        bm.w = w - (oversample_h - 1);
        bm.h = h - (oversample_v - 1);
        bm.stride = stride;
        bm.pixels = out;
        if (bm.w > 0 && bm.h > 0 && !v.empty())
            stbtt_Rasterize(&bm, 0.35f, v.data(), static_cast<int>(v.size()), sx / kSub, sy / kSub, shift_x, shift_y, b.x0, b.y0, 1, nullptr);
        if (oversample_h > 1) stbtt__h_prefilter(out, w, h, stride, static_cast<unsigned>(oversample_h));
        if (oversample_v > 1) stbtt__v_prefilter(out, w, h, stride, static_cast<unsigned>(oversample_v));
        if (sub_x) *sub_x = stbtt__oversample_shift(oversample_h);
        if (sub_y) *sub_y = stbtt__oversample_shift(oversample_v);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace st::gamefont
