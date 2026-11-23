// Fragile's porter suit in Sam's suit colours, the pure part (native/mod/suit_colour_math.hpp): which constant textures a
// suit colour binds on her suit (none at 0, her albedo for the palette, albedo + reflectance + roughness for the metals),
// the palette scaled to her own navy, the debug override. No game.
#include <cstdio>

#include "suit_colour_math.hpp"

using namespace st::suitcol;

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

void OwnLook() {
    // 0 and anything not a suit colour: nothing bound (her own textures; LUT row 0 is her navy already).
    const int odd[] = {0, 17, 0x7F, 0xFE, 0xFF};
    for (int c : odd) {
        const Look l = LookFor(static_cast<std::uint8_t>(c));
        CHECK(l.own);
        CHECK(l.channels == 0u);
    }
}

void Palette() {
    // 1..14: only her albedo, scaled per channel so that row 0 would give exactly her navy.
    for (int c = 1; c <= 14; ++c) {
        const Look l = LookFor(static_cast<std::uint8_t>(c));
        CHECK(!l.own);
        CHECK(l.channels == static_cast<unsigned>(kChColor));
    }
    CHECK(ScaleChannel(kLut[0].r, kLut[0].r, kHerBase.r) == kHerBase.r);
    CHECK(ScaleChannel(kLut[0].g, kLut[0].g, kHerBase.g) == kHerBase.g);
    CHECK(ScaleChannel(kLut[0].b, kLut[0].b, kHerBase.b) == kHerBase.b);
    // row 9 (Sam's olive 9d8455): 67*157/80, 71*132/89, 81*85/105, rounded
    CHECK((LookFor(9).color == Rgb{131, 105, 66}));
    // row 3 (a grey 474747) stays a grey-ish darker than her navy in blue
    const Look grey = LookFor(3);
    CHECK(grey.color.b < kHerBase.b);
    // row 14 (ca9087) is the brightest red: clamped at 255 at most, never wraps
    const Look pink = LookFor(14);
    CHECK(pink.color.r > pink.color.g && pink.color.r > pink.color.b);
    CHECK(ScaleChannel(255, 1, 255) == 255);
    CHECK(ScaleChannel(10, 0, 50) == 255);  // a zero divisor never divides
    // every palette colour differs from every other (no two rows collapse)
    for (int a = 1; a <= 14; ++a)
        for (int b = a + 1; b <= 14; ++b) CHECK(!(LookFor(static_cast<std::uint8_t>(a)).color == LookFor(static_cast<std::uint8_t>(b)).color));
}

void Metals() {
    const Look s = LookFor(kSilver), g = LookFor(kGold);
    CHECK(!s.own && !g.own);
    CHECK(s.channels == (kChColor | kChReflectance | kChRoughness));
    CHECK(g.channels == s.channels);
    CHECK(s.color == kSilverAlbedo && g.color == kGoldAlbedo);
    CHECK(g.color.r > g.color.g && g.color.g > g.color.b);  // a warm gold albedo
    // the gold is told apart from palette 9 (olive): brighter and warmer in its reflectance
    CHECK(g.reflectance.r > LookFor(9).color.r);
    // variant 1: Sam's measured sets as they are; variant 2 = the product's; an unknown variant is the product's
    const Look s1 = LookFor(kSilver, 1), g1 = LookFor(kGold, 1);
    CHECK(s1.color == kSamMetalAlbedo && g1.color == kSamMetalAlbedo && s1.reflectance == kSamSilverF0 && g1.reflectance == kSamGoldF0);
    CHECK(LookFor(kGold, 2).reflectance == g.reflectance && LookFor(kGold, 7).reflectance == g.reflectance);
    CHECK(LookFor(9, 1).color == LookFor(9).color);  // the variant only changes the metals
    CHECK(s.reflectance == kSilverF0);
    CHECK(g.reflectance == kGoldF0);
    CHECK(g.reflectance.r > g.reflectance.g && g.reflectance.g > g.reflectance.b);  // gold: warm
    CHECK(s.reflectance.r == s.reflectance.b || s.reflectance.b > s.reflectance.r);  // silver: neutral
    CHECK(s.roughness == kMetalRoughness && g.roughness == kMetalRoughness);
}

void Override() {
    CHECK(Wanted(-1, 9) == 9);
    CHECK(Wanted(-1, 16) == 16);
    CHECK(Wanted(-1, 0xFF) == 0);  // an unreadable / unknown byte: her own look
    CHECK(Wanted(-1, 17) == 0);
    CHECK(Wanted(16, 3) == 16);
    CHECK(Wanted(0, 16) == 0);
    CHECK(Wanted(17, 5) == 5);  // an out-of-range override is no override
}

void GuestPickRemembered() {
    GuestPick g;
    CHECK(!g.See(1, 4) && g.Effective(4) == 4);  // world 1 loaded with the host's colour 4, nothing picked yet
    CHECK(g.See(1, 16) && g.saved == 16);        // she picks gold
    CHECK(g.Effective(16) == 16);
    CHECK(!g.See(2, 4) && g.Effective(4) == 16);  // the host's next load brings 4 back: Fragile stays gold
    CHECK(g.See(2, 4 + 1) && g.Effective(5) == 5);  // she picks again
    CHECK(g.See(2, 4) && g.Effective(4) == 4 && g.saved == 4);  // ... even back to the loaded colour
    GuestPick none;
    CHECK(!none.See(7, 3) && none.Effective(3) == 3);
    CHECK(!none.See(7, 0xFF) && none.saved == 0xFF);  // an unreadable byte is never saved
}

void ChannelTypes() {
    // ETextureSetType values her own set's packing uses (fgl_body_suiti_v00_set: Color 1, Reflectance 4, Roughness 6 in the
    // Color texture's alpha).
    static_assert(kTypeColor == 1u && kTypeReflectance == 4u && kTypeRoughness == 6u, "ETextureSetType");
    CHECK(LookFor(kGold).channels & kChRoughness);
}
}  // namespace

// Round 12 (docs/real-partner-body.md section 37.5): her raised hood's colour row
void TestHoodRow() {
    // a painted suit's fabric average -> the nearest row; her own navy (and Sam's row 0 swatch) is row 0, not charcoal (3)
    CHECK(NearestRow(kHerBase) == 0 && NearestRow(kLut[0]) == 0 && NearestRow(Rgb{66, 71, 81}) == 0);
    for (int i = 1; i < 15; ++i) CHECK(NearestRow(kLut[i]) == i);
    CHECK(NearestRow(Rgb{71, 71, 71}) == 3 && NearestRow(Rgb{200, 145, 130}) == 14 && NearestRow(Rgb{90, 130, 175}) == 13);
    CHECK(NearestRow(Rgb{0, 0, 0}) == 3 && NearestRow(Rgb{255, 255, 255}) == 4);
    // what her suit wears: a look's row (-1 unknown: a test pattern), else the tint bound (0 none: her navy, row 0)
    CHECK(HoodRow(true, 14, 0) == 14 && HoodRow(true, 0, 5) == 0 && HoodRow(true, -1, 5) == -1 && HoodRow(true, 17, 0) == -1);
    CHECK(HoodRow(false, 14, 0) == 0 && HoodRow(false, -1, 7) == 7 && HoodRow(false, 3, 16) == 16 && HoodRow(false, 3, 200) == 0);
}

// Round 12 review: a built look's row (texture_looks PaintedRow / BuildColourReadyMade / BuildColour, the pure part)
void TestLookRow() {
    // measured only for an S0 from the folder's files; a silver / gold folder only when its Colour picture is painted
    CHECK(MeasureHoodRow(14, true, false) && MeasureHoodRow(1, true, true) && !MeasureHoodRow(14, false, true));
    CHECK(!MeasureHoodRow(15, true, false) && !MeasureHoodRow(16, true, false) && MeasureHoodRow(16, true, true) && MeasureHoodRow(15, true, true));
    // ready-made colours on: measured row, else the folder's colour with S0, else her fabric (0)
    CHECK(ReadyMadeLookRow(14, true, -1) == 14 && ReadyMadeLookRow(14, true, 3) == 3 && ReadyMadeLookRow(14, false, -1) == 0);
    CHECK(ReadyMadeLookRow(16, true, -1) == 16 && ReadyMadeLookRow(9, true, 0) == 0 && ReadyMadeLookRow(9, true, 17) == 9);
    // ready-made colours off: no S0 her navy (0), S0 measured, a failed measurement unknown (-1)
    CHECK(FolderLookRow(false, 5) == 0 && FolderLookRow(true, 5) == 5 && FolderLookRow(true, -1) == -1 && FolderLookRow(true, 40) == -1);
    // the fabric average: only mask >= 128, alpha ignored, at least 1% of the pixels
    constexpr std::size_t kPx = 400;
    std::uint8_t rgba[kPx * 4] = {}, mask[kPx] = {};
    for (std::size_t i = 0; i < kPx; ++i) {
        const bool fabric = i % 4 == 0;  // 100 fabric pixels, pink; the rest bright green (must not count)
        rgba[i * 4 + 0] = fabric ? kLut[14].r : 0, rgba[i * 4 + 1] = fabric ? kLut[14].g : 255, rgba[i * 4 + 2] = fabric ? kLut[14].b : 0;
        rgba[i * 4 + 3] = static_cast<std::uint8_t>(i);
        mask[i] = fabric ? 200 : 127;
    }
    Rgb avg{};
    std::uint64_t n = 0;
    CHECK(FabricAverage(rgba, mask, kPx, &avg, &n) && n == 100 && avg.r == kLut[14].r && avg.g == kLut[14].g && avg.b == kLut[14].b);
    CHECK(NearestRow(avg) == 14);
    // half navy, half pink fabric: the mean of the two, its nearest row
    for (std::size_t i = 0; i < kPx; i += 8) rgba[i * 4 + 0] = kHerBase.r, rgba[i * 4 + 1] = kHerBase.g, rgba[i * 4 + 2] = kHerBase.b;
    CHECK(FabricAverage(rgba, mask, kPx, &avg, &n) && n == 100 && avg.r == (kLut[14].r + kHerBase.r) / 2);
    // too little fabric (3 of 400 < 1%), none, no picture
    for (std::size_t i = 0; i < kPx; ++i) mask[i] = i < 3 ? 255 : 0;
    Rgb keep{1, 2, 3};
    CHECK(!FabricAverage(rgba, mask, kPx, &keep, &n) && n == 3 && keep.r == 1 && keep.b == 3);
    for (std::size_t i = 0; i < kPx; ++i) mask[i] = 0;
    CHECK(!FabricAverage(rgba, mask, kPx, &keep, &n) && n == 0 && !FabricAverage(nullptr, mask, kPx, &keep) && !FabricAverage(rgba, nullptr, kPx, &keep));
    // her own navy picture measures row 0 (not charcoal), a painted sky blue 13
    for (std::size_t i = 0; i < kPx; ++i) mask[i] = 255, rgba[i * 4 + 0] = kHerBase.r, rgba[i * 4 + 1] = kHerBase.g, rgba[i * 4 + 2] = kHerBase.b;
    CHECK(FabricAverage(rgba, mask, kPx, &avg) && NearestRow(avg) == 0);
    for (std::size_t i = 0; i < kPx; ++i) rgba[i * 4 + 0] = kLut[13].r, rgba[i * 4 + 1] = kLut[13].g, rgba[i * 4 + 2] = kLut[13].b;
    CHECK(FabricAverage(rgba, mask, kPx, &avg) && NearestRow(avg) == 13);
}

int main() {
    TestHoodRow();
    TestLookRow();
    OwnLook();
    Palette();
    Metals();
    Override();
    ChannelTypes();
    GuestPickRemembered();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
