// Rasterizes the game font's outlines (game_font.hpp) into 8-bit coverage bitmaps, with a private
// copy of stb_truetype's rasterizer (third_party/imgui/imstb_truetype.h, compiled static in
// game_font_raster.cpp). Bitmaps stay in memory (the overlay's font atlas); nothing is written out.
#pragma once
#include "game_font.hpp"

namespace st::gamefont {

struct PixelBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // pixels, y down, relative to the pen on the baseline
};

// Like stbtt_GetGlyphBitmapBoxSubpixel: font units scaled by (sx, sy), y flipped, then shifted.
PixelBox GlyphBox(const Glyph& g, float sx, float sy, float shift_x, float shift_y) noexcept;

// Like stbtt_MakeGlyphBitmapSubpixelPrefilter. out (w x h, stride) must be zeroed; w and h include the
// oversampling slack (box width + oversample_h - 1). sub_x / sub_y get the prefilter's offsets.
bool RasterGlyph(const Font& f, const Glyph& g, float sx, float sy, float shift_x, float shift_y, int oversample_h, int oversample_v,
                 unsigned char* out, int w, int h, int stride, float* sub_x, float* sub_y) noexcept;

}  // namespace st::gamefont
