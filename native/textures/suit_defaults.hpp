// Stranded Together textures: Fragile's ready-made suit colours (docs/textures.md "Ready-made colours"). Pure code: no
// Windows, no files, no game. Turns HER OWN Original Colour / Reflectance / Roughness (extracted on the player's PC from
// their own game) into one set per Sam suit colour:
//   01..14  her navy fabric in that palette colour (suit_colour_math.hpp kLut), prints / piping / straps unchanged;
//           only the Colour map changes.
//   15, 16  a silver / gold metal finish on the fabric: a dark base that keeps her folds, a tinted Reflectance and her
//           Roughness scaled on the fabric (variant 0, the product since generator 2: Sam's measured metal colours and
//           x1.15, polished; on her suit a lighter Roughness picture looks shinier. Variant 3: the round-7 prototype,
//           x0.75, a satin finish, generator 1's product, kept for comparison).
//   00      nothing (her own suit).
// Integers only (no float, no transcendental function): two PCs and two compilers give the same bytes, so the host can
// make the guest's ready-made colour itself and check it by SHA-256 (STT1 RECIPE, texture_wire.hpp). The spec (and its
// executable Python model, which matches Pillow's MedianFilter(5), GaussianBlur(1.2) and LANCZOS bit for bit) is in
// docs/textures.md "For developers"; tests/suit_defaults_tests.cpp holds golden hashes from that model.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "tex_codec.hpp"
#include "tex_hash.hpp"

namespace st::tex::defaults {

// Bump when any output pixel can change (cache keys, RECIPE, the extractor's kExtractVersion with it). 1: round 8 (metal
// variant 0 = the round-7 prototype). 2: round 9, the polished metal is variant 0 (15 / 16 change; 01..14 do not).
inline constexpr int kGeneratorVersion = 2;
// 0 = the product; 1, 2 = hidden tuning, 3 = generator 1's prototype (tex 11); texture_wire.hpp has the same
inline constexpr int kMetalVariants = 4;
enum Map : unsigned { kColour = 1, kReflectance = 2, kRoughness = 4 };
unsigned ChangedMaps(int colour);             // 1..14: kColour; 15, 16: all three; else 0
bool HasDefault(int colour);                  // 1..16

// Made once per set of Original pictures, shared by all 16 colours.
struct Analysis {
    std::uint32_t w = 0, h = 0;                       // the Colour map (Reflectance is w/2 x h/2, Roughness w x h)
    std::uint64_t n = 0, sum[3] = {}, sum_lum = 0;    // fabric pixel count, channel sums, sum of lum x 10^4
    std::vector<std::uint8_t> mask;                   // w*h: median 5x5 of the fabric, then the GaussianBlur(1.2) model
    std::vector<std::uint8_t> mask_s, rel_s;          // (w/2)*(h/2): Lanczos 2:1 of mask and of q = min(255, rel16 >> 9)
};
// False + a sentence: w or h odd or < 14, fabric under 1% of the pixels (the game's pictures changed).
bool Analyse(const Image& colour, Analysis& out, std::string* why = nullptr);

struct Output {
    Image colour, reflectance, roughness;   // RGBA; roughness grey (R = G = B); only ChangedMaps(colour) are filled
    unsigned maps = 0;
};
// colour 1..16, variant 0..kMetalVariants-1 (only metals use it). refl_in / rough_in are read only for 15 / 16 (sizes
// checked: Reflectance w/2 x h/2, Roughness w x h, its R channel). False + a sentence.
bool Make(int colour, int variant, const Analysis& a, const Image& colour_in, const Image& refl_in, const Image& rough_in, Output& out,
          std::string* why = nullptr);

// The originals' identity H (the link's precheck before the host makes the guest's colour itself): SHA-256 of
// "stx-originals|1|" then, for Colour, Reflectance, Roughness and Ambient occlusion in that order: u32 w, u32 h (LE) and
// the SHA-256 of the pixels (RGB rows for Colour / Reflectance, the R channel for the grey maps). The pixels, not the PNG
// bytes: two PCs with the same game decode the same pixels even when their Windows' PNG encoders write other bytes.
Sha256 OriginalsId(const Image& colour, const Image& refl, const Image& rough, const Image& ao);

// The metal finish of a variant (15 silver, 16 gold).
struct Metal {
    std::uint8_t base[3];
    std::uint8_t shine[3];
    int rough_permille;   // Roughness x this / 1000 on the fabric (above 1000 allowed: clamped to 255)
};
Metal MetalOf(int colour, int variant);

// ---- Building blocks (unit tests) ----
bool IsFabric(std::uint8_t r, std::uint8_t g, std::uint8_t b);
// 255 where at least 13 of the 25 neighbours (edges replicated) are 255, else 0 (Pillow MedianFilter(5) on a 0/255 mask).
void Median5Binary(const std::uint8_t* in, std::uint8_t* out, std::uint32_t w, std::uint32_t h);
// Pillow 12's GaussianBlur(1.2) on 8-bit (an extended box blur: 3 horizontal then 3 vertical passes), in place.
inline constexpr std::uint32_t kBlurCentre = 8724152, kBlurSide = 4026532;   // 24-bit fixed point
void PilGaussianBlur12(std::uint8_t* plane, std::uint32_t w, std::uint32_t h);
// Pillow resize(LANCZOS) by exactly 2:1 on 8-bit (horizontal into a u8 temp, then vertical). w, h even, >= 14.
void Lanczos3Half(const std::uint8_t* in, std::uint32_t w, std::uint32_t h, std::uint8_t* out);
// The 22-bit fixed-point tables (formula numbers): interior (12 taps from 2x-5), x = 0 (7), x = 1 (9), x = 2 (11 taps from 0).
extern const std::int32_t kLanczosInterior[12];
extern const std::int32_t kLanczosEdge0[7];
extern const std::int32_t kLanczosEdge1[9];
extern const std::int32_t kLanczosEdge2[11];

}  // namespace st::tex::defaults
