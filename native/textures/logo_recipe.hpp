// Stranded Together textures: the Fragile Express logo recipe (round 20, docs/textures.md "The Fragile Express logo").
// Pure integer pixel code (no Windows, no game): the mod's worker runs it on the user's own extracted pictures of the
// game (Sam's porter-suit hood maps, the Bridges cap's maps, Fragile's decal map) and makes the copies the per-drawable
// override binds on her exo carrier and her cap only. Only rectangles and rules live here: no game pixels.
//   - the old word: the BRIDGES print is copied over with the same fabric from just beside it (one rectangle, a fixed offset
//     that keeps the fabric's weave in step), in every map that shows it (colour, mask / reflectance, normal);
//   - the new word: "FRAGILE express" from Fragile's own decal map (white letters on black: a coverage), resampled with an
//     exact-area box filter, in the old letters' own colour, mask alpha (0: untinted, like the old print) and roughness.
// Both words lie in their maps as in the decal (upright). The hood band shows only on the raised hood, seen from the
// front, and there the game's own BRIDGES (turned 180 degrees in the map) reads upside down; round 20's stills: a turned
// FRAGILE read upside down and reversed the same way, one flipped top to bottom read upside down, so the map shows the
// band unturned and an upright word reads right way round. The cap's word crosses the seam between the cap's two front
// panels: it is split in the gap between "FRAGI" and "LE", so a seam that is a pixel or two off changes only that gap.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "tex_codec.hpp"

namespace st::tex::logo {

// Bump when any output pixel can change (the built copies' cache keys).
inline constexpr int kRecipeVersion = 5;   // 5: + the metal hoods' reflectance; 4: the hood's word upright, the cap's v00 only

struct Rect {
    std::uint32_t x0, y0, x1, y1;   // [x0, x1) x [y0, y1)
};

// Fragile's decal map (fgl_body_decal_v00_set Alpha, 1024): the "FRAGILE express" wordmark and the column between "FRAGI"
// and "LE" (the middle of a 9-px gap) where the cap's word is split.
inline constexpr std::uint32_t kDecalSide = 1024;
inline constexpr Rect kWord = {328, 37, 937, 107};
inline constexpr std::uint32_t kWordSplitX = 606;

// The hood band (Sam's porter suit maps, 2048): BRIDGES (x 1537..1698, y 347..375), the rectangle erased and where it is
// copied from (the band below it), where the old letters' colour is measured (the palette mask is 0 under them) and
// where the new word goes (170 px wide, centred on the old word).
inline constexpr std::uint32_t kHoodSide = 2048;
inline constexpr Rect kHoodErase = {1531, 343, 1705, 380};
inline constexpr int kHoodFromDy = 38;
inline constexpr Rect kHoodLetters = {1530, 340, 1706, 382};
inline constexpr std::uint32_t kHoodWordW = 170, kHoodWordCx = 1618, kHoodWordCy = 361;
// The metal hoods' reflectance map (silver, gold: 1024, RGB reflectance + A ambient occlusion, half the colour map's
// scale): BRIDGES is a print that does not shine there (the gold / silver is the reflectance), so a colour copy alone
// left a dark ghost of it. The same rectangles halved; the new word 85 px wide, centred on the old word.
inline constexpr std::uint32_t kReflSide = 1024;
inline constexpr Rect kReflErase = {765, 171, 853, 190};
inline constexpr int kReflFromDy = 19;
inline constexpr Rect kReflLetters = {765, 170, 853, 191};
inline constexpr std::uint32_t kReflWordW = kHoodWordW / 2, kReflWordCx = kHoodWordCx / 2, kReflWordCy = kHoodWordCy / 2;

// The cap (cap00 maps, 1024): BRIDGES (x 631..949, y 292..342) over the two front panels; panel A's last column 777,
// panel B's first 809 (778..808: the gutter between them). The fabric is a 4-px twill: copied from 60 px above.
inline constexpr std::uint32_t kCapSide = 1024;
inline constexpr Rect kCapErase = {624, 287, 956, 348};
inline constexpr int kCapFromDy = -60;
inline constexpr Rect kCapLetters = {620, 282, 960, 352};
inline constexpr std::uint32_t kCapWordW = 284, kCapWordCy = 317, kCapPanelAEnd = 778, kCapPanelB = 809;

// An 8-bit coverage map.
struct Coverage {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> a;
    std::uint8_t At(std::uint32_t x, std::uint32_t y) const { return a[static_cast<std::size_t>(y) * w + x]; }
};

// Exact-area box filter (every source pixel weighs the area it covers in the destination pixel), rounded.
Coverage AreaResize(const Coverage& src, std::uint32_t w, std::uint32_t h);
// The height the wordmark gets at width w (its aspect, rounded).
std::uint32_t WordHeight(std::uint32_t w);
// The wordmark (kWord of the decal's R channel) at w x h, upright (as in the decal). False: the decal is not 1024 or has
// no wordmark there.
bool Wordmark(const Image& decal, std::uint32_t w, std::uint32_t h, Coverage* out, std::string* why = nullptr);
// The column of the cap's word where it is split (the gap between "FRAGI" and "LE").
std::uint32_t CapSplit();

// The hood: `colour` (RGB) and `mask` (R mask alpha, G roughness) of one suit set (palette or metal), `normal` (RG; null:
// left alone); `palette_mask` the palette set's Mask alpha picture (R; 0 under the old letters). Edited in place.
bool StampHood(Image* colour, Image* mask, Image* normal, const Image& palette_mask, const Image& decal, std::string* why = nullptr);
// A metal hood's reflectance: `refl` (RGB; A, the ambient occlusion, left alone) at 1024; `palette_mask` as for StampHood
// (2048: halved here, 2 x 2 means). Edited in place.
bool StampHoodReflectance(Image* refl, const Image& palette_mask, const Image& decal, std::string* why = nullptr);
// The cap: `colour` (RGB, and A when `alpha` is true: the mask alpha or the reflectance its set packs there), `normal` (RG;
// null: left alone); `letters` the palette cap's Mask alpha picture (R; 0 under the old letters). Edited in place.
bool StampCap(Image* colour, bool alpha, Image* normal, const Image& letters, const Image& decal, std::string* why = nullptr);

}  // namespace st::tex::logo
