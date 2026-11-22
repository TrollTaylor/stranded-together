// Stranded Together textures (docs/textures.md, docs/fragile-textures design): the pure pixel code shared by the
// launcher's extractor, the mod's loader (T2) and the tests. No Windows, no game, no files.
//  - the game's pixel formats (EPixelFormat numbers as Texture headers store them) and their tight mip sizes;
//  - BC1 / BC3 / BC4 / BC5 / BC7 decoders (RGBA8 out) and BC1 / BC3 / BC4 / BC5 encoders (deterministic);
//  - the mip chain (box filter: sRGB-aware for colour, renormalised for normals);
//  - TextureSet PackingInfo: which map type and source channel each output channel holds.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace st::tex {

// EPixelFormat (Texture header +9) values seen in Death Stranding's character textures.
enum Fmt : std::uint8_t {
    kRGBA8 = 12,
    kBC1 = 66,
    kBC2 = 67,
    kBC3 = 68,
    kBC4U = 69,
    kBC4S = 70,
    kBC5U = 71,
    kBC5S = 72,
    kBC6U = 73,
    kBC6S = 74,
    kBC7 = 75,
};
const char* FormatName(std::uint8_t fmt);        // "BC7", "RGBA8", ... or "fmt<n>"
int BlockBytes(std::uint8_t fmt);                // bytes per 4x4 block, 0 for RGBA8 and unknown formats
bool CanDecode(std::uint8_t fmt);                // RGBA8, BC1, BC3, BC4U, BC5U, BC7
bool CanEncode(std::uint8_t fmt);                // BC1, BC3, BC4U, BC5U
// Bytes of one mip of a w x h image: block formats at least one block per axis; 0 for an unknown format.
std::uint64_t MipBytes(std::uint8_t fmt, std::uint32_t w, std::uint32_t h);
// Sum of mips 0..mips-1 (each axis halves, at least 1).
std::uint64_t ChainBytes(std::uint8_t fmt, std::uint32_t w, std::uint32_t h, std::uint32_t mips);
// The full chain's length: 12 for 2048, 11 for 1024, 1 for 1x1.
std::uint32_t FullMipCount(std::uint32_t w, std::uint32_t h);

struct Image {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> rgba;   // w * h * 4, row-major, top row first
    void Resize(std::uint32_t width, std::uint32_t height) {
        w = width;
        h = height;
        rgba.assign(static_cast<std::size_t>(width) * height * 4u, 0);
    }
    std::uint8_t* At(std::uint32_t x, std::uint32_t y) { return rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4u; }
    const std::uint8_t* At(std::uint32_t x, std::uint32_t y) const { return rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4u; }
};

// One mip level -> RGBA8. BC4: R=G=B=value, A=255. BC5: R, G, B=0, A=255 (RebuildNormalZ fills B).
// BC1 decodes its 3-colour mode's transparent black as (0,0,0,0). False + a reason for a short buffer or an
// unsupported format (BC2, BC6, signed formats).
bool Decode(std::uint8_t fmt, const std::uint8_t* data, std::size_t size, std::uint32_t w, std::uint32_t h, Image& out,
            std::string* error = nullptr);
// Tangent-space normal in R/G (0..255 = -1..1): B = sqrt(1 - x^2 - y^2) re-encoded, A = 255.
void RebuildNormalZ(Image& img);

// Encoders (whole image, dimensions any; partial edge blocks repeat their last pixel). Output is MipBytes long.
// BC1: 4-colour mode only, principal axis + 2 least-squares refinements. BC4U: channel `channel` of the image.
// BC3: BC4 of A + BC1 of RGB. BC5U: BC4 of R then BC4 of G. Deterministic: the same input gives the same bytes.
bool Encode(std::uint8_t fmt, const Image& img, std::vector<std::uint8_t>& out, std::string* error = nullptr);
void EncodeBC1(const Image& img, std::vector<std::uint8_t>& out);
void EncodeBC4(const Image& img, int channel, std::vector<std::uint8_t>& out);
void EncodeBC3(const Image& img, std::vector<std::uint8_t>& out);
void EncodeBC5(const Image& img, std::vector<std::uint8_t>& out);

// How a mip level is averaged from the one above.
enum class MipKind {
    linear,      // every channel averaged as stored
    srgb_rgb,    // R, G, B decoded from sRGB, averaged, encoded again; A linear
    normal_rg,   // R, G as a tangent-space normal: averaged then renormalised (B rebuilt), A linear
};
// The next mip (each axis halves, at least 1; a 2x2 box, or 2x1 / 1x2 at an edge of size 1).
Image Downsample(const Image& src, MipKind kind);
// Resize to exactly w x h (box filter down, bilinear up; a 2-step for mixed axes). sRGB-aware like Downsample.
Image Resize(const Image& src, std::uint32_t w, std::uint32_t h, MipKind kind);
std::uint8_t SrgbToLinear8(std::uint8_t v);   // through float, rounded
float SrgbToLinear(std::uint8_t v);           // 0..1
std::uint8_t LinearToSrgb8(float v);          // 0..1 in, rounded

// ---- TextureSet PackingInfo ----------------------------------------------------------------------------
// One byte per output channel R, G, B, A: low nibble = ETextureSetType (0 = unused), bits 4-5 = the source channel of
// that map, bit 7 = taken from a single-channel source.
enum SetType : std::uint8_t {
    kTypeInvalid = 0,
    kTypeColor = 1,
    kTypeAlpha = 2,
    kTypeNormal = 3,
    kTypeReflectance = 4,
    kTypeAO = 5,
    kTypeRoughness = 6,
    kTypeHeight = 7,
    kTypeMask = 8,
    kTypeMaskAlpha = 9,
    kTypeIncandescence = 10,
    kTypeMisc01 = 13,
};
struct PackChannel {
    std::uint8_t type = 0;     // SetType, 0 = unused
    std::uint8_t source = 0;   // 0..3 = R, G, B, A of that map
    bool single = false;       // bit 7
};
PackChannel Unpack(std::uint32_t packing, int channel);
const char* SetTypeName(std::uint8_t type);       // "Color", "Normal", ... (the game's names)
const char* MapFileName(std::uint8_t type);       // "Colour", "Normal", "Reflectance", "Ambient occlusion", ...

// A map of one texture: its type and the output channels that hold it, in source-channel order.
struct PackedMap {
    std::uint8_t type = 0;
    std::vector<int> channels;   // output channel indices 0..3 (R..A), sorted by their source channel
    std::string Component() const;   // "RGB", "A", "RG", "G"...
};
std::vector<PackedMap> MapsOf(std::uint32_t packing);   // in the order of their first channel

// Copies `channels` of `src` into a new image: one channel -> grey (R=G=B, A=255); several -> R, G, B in order
// (missing ones 0), A=255.
Image ExtractChannels(const Image& src, const std::vector<int>& channels);
// The reverse: writes the R (and G, B) of `map` into `channels` of `dst` (sizes must match).
bool InsertChannels(Image& dst, const Image& map, const std::vector<int>& channels);

// ---- Quality measures (tests, the extractor's checks) ---------------------------------------------------
double Psnr(const Image& a, const Image& b, int channel_mask = 0x7);   // over the channels set in the mask (bit 0 = R)
void MeanRgb(const Image& img, double out[3], bool (*keep)(const std::uint8_t* px) = nullptr);

}  // namespace st::tex
