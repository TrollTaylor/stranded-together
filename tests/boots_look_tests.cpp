// Fragile's boots, the pure part (native/mod/boots_look.hpp): the style setting, the boots' ArtParts bits (settled, worn,
// the bits that keep her own boots), the boot type -> folder, the Worn / Gold / Silver order and the look keys, and what a
// screen does with her boots. No game.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "boots_look.hpp"

using namespace st::bootslook;

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

constexpr std::uint32_t B(int i) { return 1u << i; }

void TestStyle() {
    Style s = Style::prop;
    CHECK(ParseStyle("own", &s) && s == Style::own);
    CHECK(ParseStyle("prop", &s) && s == Style::prop);
    CHECK(ParseStyle("1", &s) && s == Style::own);
    CHECK(ParseStyle("0", &s) && s == Style::prop);
    s = Style::own;
    CHECK(!ParseStyle("Own", &s) && !ParseStyle("", &s) && !ParseStyle(nullptr, &s) && !ParseStyle("boots", &s) && s == Style::own);
    CHECK(std::strcmp(StyleName(Style::own), "own") == 0 && std::strcmp(StyleName(Style::prop), "prop") == 0);
}

void TestBits() {
    // run 20261122-080513 (Chiral boots): 100 % 80010000 (mk3r + mk3l), 5 % 01011000, 0 % 0B000000, the load window FFFFFFFF
    CHECK(Ready(0x80010000u) && !Worn(0x80010000u));
    CHECK(Ready(0x01011000u) && Worn(0x01011000u));
    CHECK(Ready(0x0B000000u) && Worn(0x0B000000u));
    CHECK(!Ready(0xFFFFFFFFu) && !Worn(0xFFFFFFFFu));
    CHECK(Ready(0) && !Worn(0));
    CHECK(Bits(0x3Fu) == 6 && Ready(0x3Fu) && !Ready(0x7Fu));
    // every broken / loose-sole part is worn; the intact shapes are not
    const int broken[] = {4, 5, 6, 8, 9, 10, 11, 12, 13, 15, 17, 18, 19, 22, 24, 25, 26, 27, 28, 30};
    const int intact[] = {0, 1, 2, 3, 7, 14, 16, 20, 21, 23, 29, 31};
    for (int b : broken) CHECK(Worn(B(b)));
    for (int b : intact) CHECK(!Worn(B(b)));
    static_assert((kBrokenMask & (kSoleGrassMask | kPorterMask)) == 0, "the intact porter / sole grass parts are not worn");
    std::uint32_t all = kBrokenMask;
    for (int b : intact) all |= B(b);
    CHECK(all == 0xFFFFFFFFu);
}

void TestKeepsOwn() {
    CHECK(KeepsOwnBoots(0, false));                          // nothing shown (barefoot)
    CHECK(KeepsOwnBoots(B(2) | B(3), false));                // sole grass
    CHECK(KeepsOwnBoots(B(2), false));
    CHECK(KeepsOwnBoots(B(0) | B(1), true));                 // intact mk1 boots (Lv1, her own design) with boots_own_porter
    CHECK(!KeepsOwnBoots(0x00204000u, true));                // NormalBoots / MuleBoots (the "mule" mesh): Sam's shape
    CHECK(!KeepsOwnBoots(0x20100000u, true));                // StableBootsLv2 (mk2)
    CHECK(!Worn(0x00204000u) && !Worn(0x20100000u) && !Worn(0x00800080u) && !Worn(0x00000003u));  // run 120624: intact types
    CHECK(Worn(0x8A000000u));                                // Chiral at 5 % in run 120624
    CHECK(!KeepsOwnBoots(B(0) | B(1), false));               // ... without: Sam's porter boots
    CHECK(!KeepsOwnBoots(B(0) | B(4), true));                // a broken porter boot: the prop shows the shape
    CHECK(!KeepsOwnBoots(0x80010000u, true));                // Chiral (mk3)
    CHECK(!KeepsOwnBoots(B(7) | B(23), true));               // terrorist
    CHECK(KeepsOwnBoots(0xFFFFFFFFu, false));                // the load window: never all 32 shapes at once
}

void TestFolders() {
    CHECK(FolderForType(0) == 0 && FolderForType(1) == 3 && FolderForType(2) == 2 && FolderForType(3) == 3 && FolderForType(9) == 9 &&
          FolderForType(10) == 10);
    CHECK(FolderForType(-1) == -1 && FolderForType(11) == -1 && FolderForType(255) == -1);
    CHECK(MetalOfColour(16) == 1 && MetalOfColour(15) == 2 && MetalOfColour(0) == 0 && MetalOfColour(14) == 0 && MetalOfColour(-1) == 0);
    const char* c[kMaxCandidates] = {};
    CHECK(Candidates(false, 0, c) == 1 && std::strcmp(c[0], "") == 0);
    CHECK(Candidates(true, 0, c) == 2 && std::strcmp(c[0], "Worn") == 0 && std::strcmp(c[1], "") == 0);
    CHECK(Candidates(false, 1, c) == 2 && std::strcmp(c[0], "Gold") == 0 && std::strcmp(c[1], "") == 0);
    CHECK(Candidates(true, 2, c) == 3 && std::strcmp(c[0], "Worn") == 0 && std::strcmp(c[1], "Silver") == 0 && std::strcmp(c[2], "") == 0);
}

void TestKeys() {
    bool seen[kKeyCount] = {};
    for (int f = 0; f <= 10; ++f)
        for (int w = 0; w < 2; ++w)
            for (int m = 0; m < 3; ++m) {
                const int k = Key(f, w != 0, m);
                CHECK(k >= 0 && k < kKeyCount);
                if (k < 0 || k >= kKeyCount) continue;
                CHECK(!seen[k]);
                seen[k] = true;
                int f2 = -1, m2 = -1;
                bool w2 = false;
                Unkey(k, &f2, &w2, &m2);
                CHECK(f2 == f && w2 == (w != 0) && m2 == m);
            }
    CHECK(Key(-1, false, 0) == -1 && Key(11, false, 0) == -1 && Key(3, false, 3) == -1 && Key(3, false, -1) == -1);
}

void TestDecide() {
    // own: never a prop, her feet never scaled, Sam's boots hidden while they are shown
    Decision d = Decide(Style::own, true, 0x80010000u, true);
    CHECK(!d.prop && d.hide_sam && !d.scale_feet);
    d = Decide(Style::own, false, 0, true);
    CHECK(!d.prop && !d.hide_sam && !d.scale_feet);
    // prop: Sam's boots (the prop / his entity) on her feet, her feet scaled
    d = Decide(Style::prop, true, 0x80010000u, true);
    CHECK(d.prop && !d.hide_sam && d.scale_feet);
    d = Decide(Style::prop, true, 0x01011000u, true);  // worn: the broken shape
    CHECK(d.prop && d.scale_feet);
    // prop, but bits that keep her own boots
    d = Decide(Style::prop, true, B(2) | B(3), false);
    CHECK(!d.prop && d.hide_sam && !d.scale_feet);
    d = Decide(Style::prop, true, B(0) | B(1), true);
    CHECK(!d.prop && d.hide_sam && !d.scale_feet);
    d = Decide(Style::prop, true, B(0) | B(1), false);
    CHECK(d.prop && d.scale_feet);
    d = Decide(Style::prop, true, 0xFFFFFFFFu, false);  // the load window
    CHECK(!d.prop && !d.scale_feet);
    d = Decide(Style::prop, false, 0x80010000u, true);  // no boots item / entity: nothing
    CHECK(!d.prop && !d.hide_sam && !d.scale_feet);
}

}  // namespace

int main() {
    TestStyle();
    TestBits();
    TestKeepsOwn();
    TestFolders();
    TestKeys();
    TestDecide();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
