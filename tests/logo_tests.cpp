// The Fragile Express logo (native/textures/logo_recipe.hpp, logo_slots.hpp, docs/textures.md): the recipe's resampler,
// the wordmark's orientation, where the hood's and the cap's words go and what they erase, on synthetic pictures.
// No game data is read or written.
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "logo_recipe.hpp"
#include "logo_slots.hpp"

using namespace st::tex;
namespace lg = st::tex::logo;

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

void Fill(Image* img, std::uint32_t side, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    img->Resize(side, side);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            std::uint8_t* p = img->At(x, y);
            p[0] = r, p[1] = g, p[2] = b, p[3] = a;
        }
}
void Box(Image* img, const lg::Rect& r, std::uint8_t v0, std::uint8_t v1, std::uint8_t v2, std::uint8_t v3) {
    for (std::uint32_t y = r.y0; y < r.y1; ++y)
        for (std::uint32_t x = r.x0; x < r.x1; ++x) {
            std::uint8_t* p = img->At(x, y);
            p[0] = v0, p[1] = v1, p[2] = v2, p[3] = v3;
        }
}
// A decal whose wordmark is one white block at its left end (the first 72 of its 609 columns, full height).
Image Decal() {
    Image d;
    Fill(&d, lg::kDecalSide, 0, 0, 0, 255);
    Box(&d, lg::Rect{lg::kWord.x0, lg::kWord.y0, lg::kWord.x0 + 72, lg::kWord.y1}, 255, 255, 255, 255);
    return d;
}
// A decal whose wordmark is one white block in its top left corner (72 columns, the top 35 of its 70 rows).
Image CornerDecal() {
    Image d;
    Fill(&d, lg::kDecalSide, 0, 0, 0, 255);
    Box(&d, lg::Rect{lg::kWord.x0, lg::kWord.y0, lg::kWord.x0 + 72, lg::kWord.y0 + 35}, 255, 255, 255, 255);
    return d;
}

void Resampler() {
    lg::Coverage src;
    src.w = 6, src.h = 4;
    for (std::uint32_t i = 0; i < 24; ++i) src.a.push_back(static_cast<std::uint8_t>(i * 10));
    const lg::Coverage same = lg::AreaResize(src, 6, 4);
    CHECK(same.w == 6 && same.h == 4 && same.a == src.a);   // the same size: the same pixels
    lg::Coverage flat;
    flat.w = 609, flat.h = 70;
    flat.a.assign(609u * 70u, 200);
    const lg::Coverage small = lg::AreaResize(flat, 170, 20);
    bool all = small.w == 170 && small.h == 20 && small.a.size() == 3400;
    for (std::uint8_t v : small.a) all = all && v == 200;
    CHECK(all);   // a flat picture stays flat at any size
    lg::Coverage stripes;
    stripes.w = 4, stripes.h = 1;
    stripes.a = {0, 255, 0, 255};
    const lg::Coverage half = lg::AreaResize(stripes, 2, 1);
    CHECK(half.a.size() == 2 && half.a[0] == 128 && half.a[1] == 128);   // every source pixel weighs the area it covers
    const lg::Coverage odd = lg::AreaResize(stripes, 3, 1);   // 4 -> 3: each destination covers 4/3 source pixels
    CHECK(odd.a.size() == 3 && odd.a[0] == 64 && odd.a[1] == 128 && odd.a[2] == 191);
    CHECK(lg::AreaResize(lg::Coverage{}, 3, 3).a.empty());
    CHECK(lg::WordHeight(170) == 20 && lg::WordHeight(284) == 33 && lg::WordHeight(609) == 70);
    CHECK(lg::CapSplit() == 130);
}

void Wordmark() {
    const Image d = Decal();
    lg::Coverage up;
    std::string why;
    CHECK(lg::Wordmark(d, 170, 20, &up, &why));
    CHECK(up.w == 170 && up.h == 20);
    CHECK(up.At(0, 0) == 255 && up.At(169, 19) == 0);   // the block stays at the word's left end
    // upright: the corner block (top left) stays top left, not bottom right (turned), top right (mirrored) or bottom left
    // (flipped); 72 of 609 columns -> about 20 of 170, 35 of 70 rows -> 10 of 20
    const Image cd = CornerDecal();
    lg::Coverage cu;
    CHECK(lg::Wordmark(cd, 170, 20, &cu, &why));
    CHECK(cu.At(0, 0) == 255 && cu.At(18, 8) == 255 && cu.At(0, 19) == 0 && cu.At(169, 0) == 0 && cu.At(169, 19) == 0 && cu.At(30, 0) == 0);
    Image blank;
    Fill(&blank, lg::kDecalSide, 0, 0, 0, 255);
    CHECK(!lg::Wordmark(blank, 170, 20, &up, &why) && why.find("no wordmark") != std::string::npos);
    Image small;
    Fill(&small, 512, 255, 255, 255, 255);
    CHECK(!lg::Wordmark(small, 170, 20, &up, &why) && why.find("1024") != std::string::npos);
}

void Hood() {
    const lg::Rect old_word = {1537, 347, 1699, 376};   // BRIDGES
    Image colour, mask, normal, pal;
    Fill(&colour, lg::kHoodSide, 50, 60, 70, 255);
    Box(&colour, old_word, 200, 190, 180, 255);
    Fill(&mask, lg::kHoodSide, 255, 100, 0, 255);
    Box(&mask, old_word, 0, 30, 0, 255);
    Fill(&normal, lg::kHoodSide, 128, 128, 255, 255);
    Box(&normal, old_word, 90, 160, 255, 255);
    Fill(&pal, lg::kHoodSide, 255, 255, 255, 255);
    Box(&pal, old_word, 0, 0, 0, 255);
    const Image decal = Decal();
    std::string why;
    CHECK(lg::StampHood(&colour, &mask, &normal, pal, decal, &why));
    // the word's box: 170 x 20 at (1533, 351), upright, so the decal's block lands at its left end
    const std::uint32_t x0 = lg::kHoodWordCx - lg::kHoodWordW / 2, y0 = lg::kHoodWordCy - 20 / 2;
    static_assert(lg::kHoodWordCx - lg::kHoodWordW / 2 == 1533 && lg::kHoodWordCy - 20 / 2 == 351, "the hood word's box");
    const std::uint8_t* inked = colour.At(x0 + 5, y0 + 10);
    CHECK(inked[0] == 200 && inked[1] == 190 && inked[2] == 180);   // the old letters' own colour
    CHECK(mask.At(x0 + 5, y0 + 10)[0] == 0 && mask.At(x0 + 5, y0 + 10)[1] == 30);   // untinted, the old print's roughness
    // the rest of the old word is the band's own cloth again, in every map
    const std::uint8_t* erased = colour.At(x0 + 165, y0 + 10);
    CHECK(erased[0] == 50 && erased[1] == 60 && erased[2] == 70);
    CHECK(mask.At(x0 + 165, y0 + 10)[0] == 255 && mask.At(x0 + 165, y0 + 10)[1] == 100);
    CHECK(normal.At(x0 + 165, y0 + 10)[0] == 128 && normal.At(x0 + 165, y0 + 10)[1] == 128);
    bool none_left = true;
    for (std::uint32_t y = old_word.y0; y < old_word.y1; ++y)
        for (std::uint32_t x = x0 + 25; x < old_word.x1; ++x) none_left = none_left && colour.At(x, y)[0] == 50 && normal.At(x, y)[0] == 128;
    CHECK(none_left);
    CHECK(colour.At(1500, 361)[0] == 50 && colour.At(1720, 361)[0] == 50);   // nothing outside the band's rectangle
    // no BRIDGES (mask alpha nowhere 0 there), a wrong size: refused, nothing changed
    Image plain;
    Fill(&plain, lg::kHoodSide, 255, 255, 255, 255);
    Image c2 = colour;
    CHECK(!lg::StampHood(&c2, &mask, nullptr, plain, decal, &why) && why.find("BRIDGES") != std::string::npos && c2.rgba == colour.rgba);
    Image tiny;
    Fill(&tiny, 1024, 0, 0, 0, 255);
    CHECK(!lg::StampHood(&tiny, &mask, nullptr, pal, decal, &why) && why.find("2048") != std::string::npos);
}

void Cap() {
    const lg::Rect old_word = {631, 292, 950, 343};
    for (int alpha = 0; alpha < 2; ++alpha) {
        Image colour, normal, letters;
        Fill(&colour, lg::kCapSide, 80, 90, 100, 255);
        Box(&colour, old_word, 220, 210, 200, 0);
        Fill(&normal, lg::kCapSide, 128, 128, 255, 255);
        Box(&normal, old_word, 60, 200, 255, 255);
        Fill(&letters, lg::kCapSide, 255, 255, 255, 255);
        Box(&letters, old_word, 0, 0, 0, 255);
        std::string why;
        CHECK(lg::StampCap(&colour, alpha != 0, &normal, letters, Decal(), &why));
        // the word is not turned: the block (its left end, "FRAGI") ends at panel A's last column 777
        const std::uint32_t x0 = lg::kCapPanelAEnd - lg::CapSplit(), y0 = lg::kCapWordCy - 33 / 2;
        CHECK(x0 == 648 && y0 == 301);
        const std::uint8_t* inked = colour.At(x0 + 10, y0 + 16);
        CHECK(inked[0] == 220 && inked[1] == 210 && inked[2] == 200 && inked[3] == (alpha ? 0 : 255));
        // the gutter between the panels and the rest of the old word: the cap's own twill (from 60 px above)
        CHECK(colour.At(790, 317)[0] == 80 && normal.At(790, 317)[0] == 128);
        CHECK(colour.At(900, 317)[0] == 80 && colour.At(635, 295)[0] == 80 && normal.At(635, 295)[1] == 128);
    }
    Image c, l;
    Fill(&c, 512, 0, 0, 0, 255);
    Fill(&l, lg::kCapSide, 0, 0, 0, 255);
    std::string why;
    CHECK(!lg::StampCap(&c, false, nullptr, l, Decal(), &why) && why.find("1024") != std::string::npos);
}

void Slots() {
    static_assert(kAnySlotCount == static_cast<int>(kSlotCount) + kLogoSlotCount && kLogoSlotCount == 11, "the slot numbering");
    CHECK(AnySlot(-1) == nullptr && AnySlot(kAnySlotCount) == nullptr);
    CHECK(CapLogoFits(0) && CapLogoFits(9) && CapLogoFits(22) && !CapLogoFits(23) && !CapLogoFits(24) && !CapLogoFits(0xFF));
    CHECK(AnySlot(0) == &kSlots[0] && AnySlot(kLogoBase) == &kLogoSlots[0]);
    std::set<std::string> contents, ids;
    for (int i = 0; i < kAnySlotCount; ++i) {
        const Slot* s = AnySlot(i);
        CHECK(s != nullptr);
        if (!s) continue;
        CHECK((i >= kLogoBase) == (s->group == SlotGroup::logo));
        contents.insert(s->content_guid);
        ids.insert(s->id);
        CHECK(std::strlen(s->content_guid) == 36 && std::strlen(s->object_guid) == 36);
    }
    CHECK(static_cast<int>(contents.size()) == kAnySlotCount);   // every row its own texture content (the ftex keep rule)
    CHECK(static_cast<int>(ids.size()) == kAnySlotCount);
    for (int i = kHoodColour; i <= kHoodNormal; ++i) CHECK(kLogoSlots[i].w == lg::kHoodSide);
    for (int i = kCapColour; i <= kCapNormal; ++i) CHECK(kLogoSlots[i].w == lg::kCapSide);
    for (int i = kSilverRefl; i <= kGoldRefl; ++i) CHECK(kLogoSlots[i].w == lg::kReflSide && kLogoSlots[i].clone_fmt == kBC3);
}

void Reflectance() {
    // the metal hood's reflectance (1024): gold cloth (200, 160, 100), BRIDGES not shining (60, 50, 40), AO 220 everywhere
    const lg::Rect old_word = {1537, 347, 1699, 376};
    const lg::Rect half = {old_word.x0 / 2, old_word.y0 / 2, (old_word.x1 + 1) / 2, (old_word.y1 + 1) / 2};
    Image refl, pal;
    Fill(&refl, lg::kReflSide, 200, 160, 100, 220);
    Box(&refl, half, 60, 50, 40, 220);
    Fill(&pal, lg::kHoodSide, 255, 255, 255, 255);
    Box(&pal, old_word, 0, 0, 0, 255);
    std::string why;
    CHECK(lg::StampHoodReflectance(&refl, pal, Decal(), &why));
    // the word's box: 85 x 10 at (767, 175), upright: the decal's block (about 10 px) at its left end
    static_assert(lg::kReflWordCx - lg::kReflWordW / 2 == 767 && lg::kReflWordCy == 180, "the reflectance word's box");
    CHECK(lg::WordHeight(lg::kReflWordW) == 10);
    const std::uint8_t* inked = refl.At(769, 180);
    CHECK(inked[0] == 60 && inked[1] == 50 && inked[2] == 40 && inked[3] == 220);   // the old print's reflectance, the AO kept
    const std::uint8_t* erased = refl.At(845, 180);
    CHECK(erased[0] == 200 && erased[1] == 160 && erased[2] == 100 && erased[3] == 220);
    bool none_left = true;
    for (std::uint32_t y = half.y0; y < half.y1; ++y)
        for (std::uint32_t x = 767 + 13; x < half.x1; ++x) none_left = none_left && refl.At(x, y)[0] == 200;
    CHECK(none_left);
    CHECK(refl.At(760, 180)[0] == 200 && refl.At(856, 180)[0] == 200);   // nothing outside the rectangle (it was cloth)
    Image small;
    Fill(&small, 512, 0, 0, 0, 255);
    CHECK(!lg::StampHoodReflectance(&small, pal, Decal(), &why) && why.find("1024") != std::string::npos);
    Image plain;
    Fill(&plain, lg::kHoodSide, 255, 255, 255, 255);
    Image r2;
    Fill(&r2, lg::kReflSide, 200, 160, 100, 220);
    CHECK(!lg::StampHoodReflectance(&r2, plain, Decal(), &why) && why.find("BRIDGES") != std::string::npos);
}
// The binding's pure parts (review r20 visual 13): the hood_any merge, the mesh mask, the override estimate and the fit.
void Binding() {
    // hood_any: the palette look {L0 L1 L4} and the metal look {L2 L3 L4 L9 L10}: L4 once (the palette's row), sorted by slot
    const int L0 = kLogoBase + kHoodColour, L1 = kLogoBase + kHoodMask, L4 = kLogoBase + kHoodNormal, L2 = kLogoBase + kMetalColour,
              L3 = kLogoBase + kMetalMask, L9 = kLogoBase + kSilverRefl, L10 = kLogoBase + kGoldRefl;
    const std::vector<int> pal = {L0, L1, L4}, metal_built = {L2, L3, L4, L9, L10}, metal_cached = {L2, L3, L4, L9, L10};
    const auto rows = MergeLookRows(pal, metal_built);
    CHECK(rows.size() == 7);
    for (std::size_t i = 1; i < rows.size(); ++i) CHECK(rows[i - 1].first < rows[i].first);
    int l4 = 0;
    for (const auto& r : rows) {
        if (r.first == L4) {
            ++l4;
            CHECK(r.second == 2);   // from the palette look
        }
        if (r.first == L9) CHECK(r.second == pal.size() + 3);
    }
    CHECK(l4 == 1);
    // the same slots in another order (made in another order): the same merged order
    const std::vector<int> metal_shuffled = {L10, L4, L2, L9, L3};
    const auto rows2 = MergeLookRows(pal, metal_shuffled);
    CHECK(rows2.size() == rows.size());
    for (std::size_t i = 0; i < rows.size() && i < rows2.size(); ++i) CHECK(rows[i].first == rows2[i].first);
    CHECK(MergeLookRows(pal, metal_cached).size() == 7);
    CHECK(MergeLookRows(pal, {}).size() == 3 && MergeLookRows({}, metal_built).size() == 5);
    // the mesh mask (the exo carrier: 16 of 63 meshes hold a logo texture in the runs)
    std::uint64_t m[4] = {};
    CHECK(MaskEmpty(m) && !MaskHas(m, 0));
    CHECK(MaskAdd(m, 0) && MaskAdd(m, 63) && MaskAdd(m, 64) && MaskAdd(m, 255));
    CHECK(!MaskAdd(m, 63));                 // a mesh met twice (the same map twice on it): counted once
    CHECK(!MaskAdd(m, -1) && !MaskAdd(m, 256));
    CHECK(MaskHas(m, 63) && MaskHas(m, 64) && !MaskHas(m, 62) && !MaskHas(m, 256) && !MaskEmpty(m) && MaskCount(m) == 4);
    // the estimate: one entry per map per bound mesh (the hood look: 11 maps on 16 meshes = 176), capped
    const unsigned maps[7] = {3, 2, 1, 1, 2, 1, 1};   // 11 in all
    CHECK(OverrideEstimate(maps, 7, 16, 256) == 176);
    CHECK(OverrideEstimate(maps, 7, 30, 256) == 256);   // over the cap: the cap (the registry room checked against it)
    CHECK(OverrideEstimate(maps, 0, 16, 256) == 0);
    // the fit by the core's actual count: a mesh holding a map twice makes one more entry than the estimate
    CHECK(SlotFits(0, 176, 256) && SlotFits(176, 80, 256) && !SlotFits(176, 81, 256));
    CHECK(SlotFits(240, 16, 256) && !SlotFits(240, 17, 256));   // 16 meshes, one of them holding the map twice: 17, refused whole
    CHECK(!SlotFits(257, 0, 256) && SlotFits(256, 0, 256));
}
}  // namespace

int main() {
    Resampler();
    Wordmark();
    Hood();
    Cap();
    Slots();
    Reflectance();
    Binding();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
