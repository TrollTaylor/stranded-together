// Fragile's boots (docs/real-partner-body.md section 30, docs/textures.md), the pure part: which boots style a screen
// shows, which folder of the user's textures her boots take their pictures from, and what the boots' 32 ArtParts bits
// say (worn, the load window). No game memory, no Windows: unit-tested in tests/boots_look_tests.cpp.
//
// Two styles (session.ini boots_look):
//   own  (default) her own model's porter boots on both screens, painted with the user's pictures for the boot type the
//        guest's Sam wears ("Fragile boots\NN <type>\", Gold\ / Silver\ / Worn\ inside). No boots prop on the partner,
//        her feet never scaled, Sam's boots entity hidden under the self view. The shape is always her porter boot.
//   prop the v0.22 look: Sam's real boots (their mesh per type and wear) on her feet, her own feet scaled away.
//        Barefoot, sole grass and (boots_own_porter, default on) intact mk1 boots of her own design keep her own boots.
//
// The boots' ArtParts (ds/artparts/characters/sam_sam/part_sam_foot_def, in the decoded order = the bit on the wire):
//    0 mk1inl  1 mk1inr  2 solegrassl 3 solegrassr 4 mk1inbreakl 5 muleinbreakr 6 mk2soler 7 terroinl
//    8 muleinbreakl 9 mk1solel 10 mulesoler 11 mk1inbreakr 12 mk3solel 13 mk2inbreakr 14 muleinr 15 terrosolel
//   16 mk3r 17 terrosoler 18 mulesolel 19 terroinbreakl 20 mk2inr 21 muleinl 22 terroinbreakr 23 terroinr
//   24 mk3breakl 25 mk3breakr 26 mk1soler 27 mk3soler 28 mk2solel 29 mk2inl 30 mk2inbreakl 31 mk3l
// Intact boots show two bits (left and right); a worn pair shows a broken shaft and / or a loose sole (run
// 20261122-080513: Chiral at 100 % 80010000, at 5 % 01011000, at 0 % 0B000000). For 5-7 s after a world load every bit
// reads shown (FFFFFFFF): "not ready".
// The boot type -> shape map (proven, hidden run 20261122-120624-fragile-boots-types, SetShoesForcibly per type):
//   0 BareFoot 00000000 (the entity stays, nothing shown)   2 SoleGrass 0000000C (solegrass)
//   3 NormalBoots 00204000 (the "mule" mesh)                4 StableBootsLv1 00000003 (mk1)
//   5 StableBootsLv2 20100000 (mk2)                          6 StableBootsLv3 80010000 (mk3)
//   7 MuleBoots 00204000 (as NormalBoots)                    8 TerroristBoots 00800080 (terro)
//   9 ChiralBoots 80010000 (mk3)                             10 ReflectBoots 80010000 (mk3)
// Her own model's boots are the mk1 design: they sample sam_body_boot_v00_set, the mk1 part's texture set.
#pragma once
#include <cstdint>
#include <cstring>

namespace st::bootslook {

enum class Style : std::uint8_t { own = 0, prop = 1 };

// session.ini boots_look: "own" / "prop" (also 1 / 0). False for anything else.
inline bool ParseStyle(const char* s, Style* out) {
    if (!s || !out) return false;
    if (std::strcmp(s, "own") == 0 || std::strcmp(s, "1") == 0) return *out = Style::own, true;
    if (std::strcmp(s, "prop") == 0 || std::strcmp(s, "0") == 0) return *out = Style::prop, true;
    return false;
}
inline const char* StyleName(Style s) { return s == Style::own ? "own" : "prop"; }

// A broken shaft or a loose sole: bits 4, 5, 6, 8-13, 15, 17-19, 22, 24-28, 30.
inline constexpr std::uint32_t kBrokenMask = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) |
                                             (1u << 13) | (1u << 15) | (1u << 17) | (1u << 18) | (1u << 19) | (1u << 22) | (1u << 24) | (1u << 25) |
                                             (1u << 26) | (1u << 27) | (1u << 28) | (1u << 30);
inline constexpr std::uint32_t kSoleGrassMask = (1u << 2) | (1u << 3);
inline constexpr std::uint32_t kPorterMask = (1u << 0) | (1u << 1);
inline constexpr int kReadyMaxBits = 6;

inline int Bits(std::uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}
// The parts settled (not the load window where every part reads shown).
inline bool Ready(std::uint32_t visible) { return Bits(visible) <= kReadyMaxBits; }
// Worn: a broken or loose-sole part shows (only when ready).
inline bool Worn(std::uint32_t visible) { return Ready(visible) && (visible & kBrokenMask) != 0; }

// `prop` style: the bits that keep her own boots (no prop, her feet not scaled): nothing shown (barefoot), sole grass only,
// or (with boots_own_porter) intact mk1 boots only (All-terrain Lv1: the design and texture set of her own boots). Also
// the load window (never all 32 shapes at once).
inline bool KeepsOwnBoots(std::uint32_t visible, bool own_porter) {
    if (!Ready(visible)) return true;
    if (visible == 0) return true;
    if ((visible & ~kSoleGrassMask) == 0) return true;
    return own_porter && (visible & ~kPorterMask) == 0;
}

// The boot type (EDSPlayerShoesType, Sam's status +0x320 +0x08) -> the "Fragile boots" folder number: 0 BareFoot,
// 2 SoleGrass, 3 NormalBoots, 4-6 StableBootsLv1-3, 7 MuleBoots, 8 TerroristBoots, 9 ChiralBoots, 10 ReflectBoots;
// 1 (ShoesA_Unused) uses 03 Porter boots. -1: not a known type.
inline int FolderForType(int type) {
    if (type == 1) return 3;
    if (type < 0 || type > 10) return -1;
    return type;
}

// Which subfolder a boots look tries first. Metal: 0 none, 1 gold (Sam's suit colour 16), 2 silver (15).
inline int MetalOfColour(int colour) { return colour == 16 ? 1 : colour == 15 ? 2 : 0; }
inline constexpr int kMaxCandidates = 3;
// The subfolders in the order they are tried ("" = the type folder itself); the first one with an edited picture wins.
// Returns the count.
inline int Candidates(bool worn, int metal, const char* (&out)[kMaxCandidates]) {
    int n = 0;
    if (worn) out[n++] = "Worn";
    if (metal == 1) out[n++] = "Gold";
    else if (metal == 2) out[n++] = "Silver";
    out[n++] = "";
    return n;
}

// The key of one boots look (the worker's cache): folder 0..10, worn, metal 0..2. -1 for an unknown folder.
inline constexpr int kKeyCount = 11 * 2 * 3;
inline int Key(int folder, bool worn, int metal) {
    if (folder < 0 || folder > 10 || metal < 0 || metal > 2) return -1;
    return (folder * 2 + (worn ? 1 : 0)) * 3 + metal;
}
inline void Unkey(int key, int* folder, bool* worn, int* metal) {
    *metal = key % 3;
    *worn = ((key / 3) % 2) != 0;
    *folder = key / 6;
}

// What a screen does with the partner Fragile's / its own self view's boots, from the style and the boots' bits.
struct Decision {
    bool prop = false;        // the partner: the boots prop shown on her feet (her feet scaled away)
    bool hide_sam = false;    // the self view: Sam's boots entity hidden (her own boots show)
    bool scale_feet = false;  // her feet scaled away under Sam's boots / the prop
};
// `boots_shown`: Sam's boots entity is shown (self view) / the partner sent a boots item (partner).
inline Decision Decide(Style style, bool boots_shown, std::uint32_t visible, bool own_porter) {
    Decision d;
    if (style == Style::own) {
        d.hide_sam = boots_shown;
        return d;
    }
    if (!boots_shown) return d;
    if (KeepsOwnBoots(visible, own_porter)) {
        d.hide_sam = true;
        return d;
    }
    d.prop = true;
    d.scale_feet = true;
    return d;
}

}  // namespace st::bootslook
