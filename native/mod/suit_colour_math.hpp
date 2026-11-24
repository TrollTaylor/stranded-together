// Fragile's porter suit in Sam's suit colours: the pure part (no game, tested by tests/suit_colour_tests.cpp).
// docs/real-partner-body.md section 27.
//
// Sam's suit colour (inventory +0x4BE1): 0..14 a row of the game's 8x16 colour LUT (cm_cvar_b002, column 0 = the full
// colour) multiplied into his light grey suit texture; 15 silver and 16 gold whole texture-set swaps (a dark metal
// albedo and a silver / gold reflectance). Fragile's suit has neither (no LUT in her shader, no other texture set), so
// the mod binds three small constant textures to her suit's Main_TextureSet channels instead: what they hold is decided
// here.
//   0 (and anything above 16): her own textures, nothing bound (her navy is LUT row 0's colour already).
//   1..14: her albedo becomes LUT[c] scaled per channel so that row 0 gives exactly her own navy (67, 71, 81); her
//          reflectance and roughness stay hers (cloth).
//   15 / 16: a metal: albedo, reflectance and a low roughness. Sam's silver / gold set measured where it is metal (the
//          decoded v02 / v03 textures' bright islands: albedo (79, 71, 72), reflectance (218, 219, 219) / (220, 176,
//          112)) looked a dull olive on her suit in the game (run 20261121-141717: hard to tell from palette 9), so the
//          product uses brighter, warmer values picked from the screenshots of run 20261121-141927 (variant 2 there), the
//          gold toned down a little after the sunlit two-game run 20261121-142205 (a flat yellow next to Sam's gold).
// All values are 8-bit texture bytes as the game stores them (albedo sRGB, reflectance and roughness linear).
#pragma once
#include <cstddef>
#include <cstdint>

namespace st::suitcol {

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
};
constexpr bool operator==(const Rgb& a, const Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

constexpr int kColours = 17;          // 0..14 palette, 15 silver, 16 gold
constexpr std::uint8_t kSilver = 15, kGold = 16;
constexpr Rgb kHerBase{67, 71, 81};   // fgl_body_suiti_v00_clr, average of the decoded low mip
// cm_cvar_b002 column 0, rows 0..14 (decoded from the game data; the colours the uniform menu shows on Sam).
constexpr Rgb kLut[15] = {{0x50, 0x59, 0x69}, {0xC2, 0x7D, 0x5B}, {0x9C, 0x39, 0x2E}, {0x47, 0x47, 0x47}, {0xAF, 0xA4, 0x90},
                          {0x92, 0x90, 0x90}, {0x6D, 0x5E, 0x50}, {0x72, 0x6C, 0x7B}, {0x60, 0x4C, 0x4C}, {0x9D, 0x84, 0x55},
                          {0x82, 0x4C, 0x47}, {0x5D, 0x5D, 0x5D}, {0x59, 0x5B, 0x41}, {0x62, 0x85, 0xA9}, {0xCA, 0x90, 0x87}};
constexpr Rgb kSilverAlbedo{96, 96, 100};
constexpr Rgb kGoldAlbedo{100, 80, 44};
constexpr Rgb kSilverF0{235, 235, 235};
constexpr Rgb kGoldF0{240, 190, 100};
constexpr std::uint8_t kMetalRoughness = 60;  // ~0.24
// Sam's own sets as measured (the tuning's reference).
constexpr Rgb kSamMetalAlbedo{79, 71, 72};
constexpr Rgb kSamSilverF0{218, 219, 219};
constexpr Rgb kSamGoldF0{220, 176, 112};

// Channels bound (a bit set): the TextureSet channel types the game's ETextureSetType names.
enum Channel : unsigned { kChColor = 1u, kChReflectance = 2u, kChRoughness = 4u };
constexpr std::uint32_t kTypeColor = 1, kTypeReflectance = 4, kTypeRoughness = 6;

struct Look {
    bool own = true;           // nothing bound: her own textures
    unsigned channels = 0;     // Channel bits bound
    Rgb color{}, reflectance{};
    std::uint8_t roughness = 0;
};

constexpr std::uint8_t ScaleChannel(std::uint8_t lut, std::uint8_t lut0, std::uint8_t base) {
    const unsigned v = (static_cast<unsigned>(base) * lut + lut0 / 2u) / (lut0 ? lut0 : 1u);
    return static_cast<std::uint8_t>(v > 255u ? 255u : v);
}

// Metal variants (hidden-test tuning, real_outfit 8): 0 the product's; 1 Sam's measured values as they are; 2 the same as
// 0 (the name the tuning run used).
struct Metal {
    Rgb silver_albedo, gold_albedo, silver_f0, gold_f0;
    std::uint8_t roughness;
};
constexpr int kMetalVariants = 3;
constexpr Metal kMetals[kMetalVariants] = {
    {kSilverAlbedo, kGoldAlbedo, kSilverF0, kGoldF0, kMetalRoughness},
    {kSamMetalAlbedo, kSamMetalAlbedo, kSamSilverF0, kSamGoldF0, 90},
    {kSilverAlbedo, kGoldAlbedo, kSilverF0, kGoldF0, kMetalRoughness},
};

constexpr Look LookFor(std::uint8_t colour, int metal = 0) {
    Look l;
    if (colour == 0 || colour > kGold) return l;
    l.own = false;
    if (colour == kSilver || colour == kGold) {
        const Metal& m = kMetals[metal >= 0 && metal < kMetalVariants ? metal : 0];
        l.channels = kChColor | kChReflectance | kChRoughness;
        l.color = colour == kGold ? m.gold_albedo : m.silver_albedo;
        l.reflectance = colour == kGold ? m.gold_f0 : m.silver_f0;
        l.roughness = m.roughness;
        return l;
    }
    l.channels = kChColor;
    const Rgb& c = kLut[colour];
    l.color = Rgb{ScaleChannel(c.r, kLut[0].r, kHerBase.r), ScaleChannel(c.g, kLut[0].g, kHerBase.g), ScaleChannel(c.b, kLut[0].b, kHerBase.b)};
    return l;
}

// The colour a body should wear: the debug override when set (0..16), else the wanted colour; 0xFF = leave as it is.
constexpr std::uint8_t Wanted(int override_colour, std::uint8_t colour) {
    if (override_colour >= 0 && override_colour <= kGold) return static_cast<std::uint8_t>(override_colour);
    return colour <= kGold ? colour : std::uint8_t{0};
}

// Round 12 (docs/real-partner-body.md section 37.5): her raised hood is Sam's hood on the exo carrier, tinted by the palette
// row (0..14) or the silver / gold set (15, 16). The row nearest a painted suit's average fabric colour (sRGB bytes): her own
// navy counts as row 0 (kHerBase, the colour row 0 stands for on her), every other row by its LUT colour. Squared distance,
// the lower row on a tie.
constexpr int Dist2(const Rgb& a, const Rgb& b) {
    return (a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b);
}
constexpr int NearestRow(const Rgb& avg) {
    int best = 0, bd = Dist2(avg, kLut[0]) < Dist2(avg, kHerBase) ? Dist2(avg, kLut[0]) : Dist2(avg, kHerBase);
    for (int i = 1; i < 15; ++i)
        if (Dist2(avg, kLut[i]) < bd) bd = Dist2(avg, kLut[i]), best = i;
    return best;
}
// The row her hood is drawn in from what her suit wears now: `look` her edited / ready-made textures are bound (a non-empty
// look, the boots' slots included) with `look_row` the suit part's row (-1 unknown: a test pattern); else `tint` the v0.23
// tint bound (suit_colour Dress::applied: 1..16, 0 none = her own navy, row 0). -1: unknown (the carrier keeps Sam's look).
constexpr int HoodRow(bool look, int look_row, std::uint8_t tint) {
    if (look) return look_row >= 0 && look_row <= kGold ? look_row : -1;
    return tint <= kGold ? tint : 0;
}
// A built look's row (texture_looks): the folder's S0 (her Suit Colour picture) measured when it is built from the folder's
// own files (painted), except a silver / gold folder (15, 16) whose Colour picture is not painted, which keeps its metal.
constexpr bool MeasureHoodRow(int colour, bool s0_from_files, bool colour_painted) { return s0_from_files && (colour < 15 || colour_painted); }
// With ready-made colours on: the measured row (`measured` -1: not measured, or the measurement failed), else the folder's
// colour when S0 is in the look (the ready-made S0), else her own fabric (no S0) row 0.
constexpr int ReadyMadeLookRow(int colour, bool s0_in, int measured) { return measured >= 0 && measured <= kGold ? measured : s0_in ? colour : 0; }
// Ready-made colours off: no S0 her own navy (row 0), else the measured row (-1 unknown).
constexpr int FolderLookRow(bool s0_in, int measured) { return !s0_in ? 0 : measured >= 0 && measured <= kGold ? measured : -1; }
// The average sRGB of a picture's pixels (RGBA bytes, `px` pixels) where her fabric mask is 128 or more; false when fewer
// than 1% of the pixels are fabric (then not measured). *n_out the fabric pixels counted.
inline bool FabricAverage(const std::uint8_t* rgba, const std::uint8_t* mask, std::size_t px, Rgb* avg, std::uint64_t* n_out = nullptr) {
    std::uint64_t n = 0, sum[3] = {};
    for (std::size_t i = 0; rgba && mask && i < px; ++i) {
        if (mask[i] < 128) continue;
        ++n;
        for (std::size_t k = 0; k < 3; ++k) sum[k] += rgba[i * 4 + k];
    }
    if (n_out) *n_out = n;
    if (n == 0 || n < px / 100) return false;
    *avg = Rgb{static_cast<std::uint8_t>(sum[0] / n), static_cast<std::uint8_t>(sum[1] / n), static_cast<std::uint8_t>(sum[2] / n)};
    return true;
}

// Round 5: the guest's own pick, remembered across the host's loads. Her world is the host's save loaded into a sandbox
// that is never saved, so a colour she picks for Sam's porter suit in the private room is gone at the next load: the mod
// keeps her last pick (her own folder) and Fragile wears it until she picks again. See(world, her Sam's colour) every frame
// the colour is read: a new world counter takes the loaded colour as the base; a different colour after that is her pick.
//
// Round 15 (section 40): `menu_rule` (the wardrobe's sites verified). The user's "stuck" colour: her Sam's byte is the
// host's after every load, the menu opened on it, and picking that very colour wrote the same byte back (the menu's close
// writes its staging copy unconditionally), so no change was seen and she kept her old pick. With the menu rule a pick is
// the close of the wardrobe (Pick, whatever the byte holds, the same value included) or the test action; a byte change on
// its own (a load, a script's ApplyInventorySetting) is never a pick, and Align says what her Sam's byte should hold so the
// menu opens on what she wears.
struct GuestPick {
    std::uint8_t saved = 0xFF;      // her last pick (0..16), 0xFF none
    std::uint8_t last = 0xFF;       // her Sam's colour last seen in this world
    bool picked = false;            // she picked in this world
    bool menu_rule = false;         // round 15: picks come from the wardrobe's close (Pick), not from byte changes
    std::uint32_t world = 0xFFFFFFFFu;
    // true when `saved` changed (to be written)
    bool See(std::uint32_t world_counter, std::uint8_t cur) {
        if (world_counter != world) {
            world = world_counter;
            last = cur;
            picked = false;
            return false;
        }
        if (cur == last) return false;
        last = cur;
        if (menu_rule) return false;  // a load or a script put a byte there: not her pick
        picked = true;
        if (cur > kGold || cur == saved) return false;
        saved = cur;
        return true;
    }
    // A pick (the wardrobe closed with `cur` in her Sam's byte, or the test action). Counts also when it is the colour she
    // wears already. True when `saved` changed.
    bool Pick(std::uint8_t cur) {
        if (cur > kGold) return false;
        picked = true;
        last = cur;
        const bool changed = saved != cur;
        saved = cur;
        return changed;
    }
    std::uint8_t Effective(std::uint8_t cur) const {
        if (menu_rule) return saved <= kGold ? saved : cur;
        return picked || saved > kGold ? cur : saved;
    }
    // The value her Sam's byte should be given (her pick), 0xFF: leave it.
    std::uint8_t Align(std::uint8_t cur) const { return menu_rule && saved <= kGold && cur <= kGold && cur != saved ? saved : std::uint8_t{0xFF}; }
};

// Round 15: the wardrobe's close, from the open flag polled every tick. `kind6` the subspace kind read this tick (1 kind 6,
// 0 another kind, -1 not readable: then any close counts; a kind-7 close only writes back what is there). True once per close
// of a menu that was kind 6 while open. `staging` (the colour the close will write, -1 not a colour) and `category` (the
// previewed category, 3 the suit colour) are what the menu showed while open: ClosePicks says whether that close is her pick.
struct WardrobeWatch {
    bool open = false, kind6 = false, kind_known = false;
    int staging_open = -1;         // the staging colour first seen in this open (-1 none)
    bool staging_changed = false;  // a colour confirmed inside the menu (the staging copy moved)
    bool previewed3 = false;       // a suit colour was previewed (category 3) in this open
    bool marked = false;           // the test action's stand-in for a suit colour preview (a save without suit colours)
    bool Step(int flag, int kind6_now, int staging = -1, int category = -1) {
        if (flag < 0) return false;
        if (flag) {
            if (!open) open = true, kind6 = false, kind_known = false, staging_open = -1, staging_changed = false, previewed3 = false, marked = false;
            if (kind6_now >= 0) kind_known = true;
            if (kind6_now > 0) kind6 = true;
            if (staging >= 0 && staging <= kGold) {
                if (staging_open < 0) staging_open = staging;
                else if (staging != staging_open) staging_changed = true;
            }
            if (category == 3) previewed3 = true;
            return false;
        }
        const bool closed = open && (kind6 || !kind_known);
        open = false;
        return closed;
    }
    void MarkPreview() {
        if (open) marked = true;
    }
    // The close just reported is her pick (round 15 review): the menu opened on her pick `saved` (what she wears, so the close
    // keeps it), or a suit colour was previewed, or one was confirmed. A close after only a glasses or cap change on a menu that
    // opened on another colour (the host's, before the alignment) never picks: it would overwrite her pick with his.
    bool ClosePicks(std::uint8_t saved) const { return previewed3 || staging_changed || marked || (staging_open >= 0 && staging_open == saved); }
};

// Round 15: at most `kMax` rewrites of her Sam's byte in `kWindowMs`; beyond that the mod stops for the world (it never
// fights the game).
struct RewriteCap {
    static constexpr int kMax = 5;
    static constexpr double kWindowMs = 10000;
    double from_ms = -1e18;
    int n = 0;
    bool tripped = false;
    bool Allow(double now_ms) {
        if (tripped) return false;
        if (now_ms - from_ms > kWindowMs) from_ms = now_ms, n = 0;
        if (n >= kMax) {
            tripped = true;
            return false;
        }
        ++n;
        return true;
    }
    void Reset() { *this = RewriteCap{}; }
};

// Round 15: what the self body shows in the wardrobe: the previewed colour (category 3), else the colour the close would
// write (the staging copy, after a confirm inside the menu), else what she wears. Only while the wardrobe's figure is hers.
constexpr std::uint8_t WardrobeShown(bool hers, int preview, int staging, std::uint8_t worn) {
    if (!hers) return worn;
    if (preview >= 0 && preview <= kGold) return static_cast<std::uint8_t>(preview);
    if (staging >= 0 && staging <= kGold) return static_cast<std::uint8_t>(staging);
    return worn;
}

}  // namespace st::suitcol
