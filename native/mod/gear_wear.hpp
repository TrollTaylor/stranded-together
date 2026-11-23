// Gear that wears out (docs/real-partner-body.md section 28), the pure part: the steps the rain cover's durability is
// logged in, and which gear resource is the backpack's rain cover. No game memory, no Windows: unit-tested in
// tests/gear_wear_tests.cpp. The game side is partner_gear.cpp (ReadRainCover, the layout logs).
//
// The rain cover over the top of the backpack has a durability (the player's data block, out of DSBackpackConfig's
// RainCoverDurability for its level). Timefall wears it; at 0 the game removes it from Sam, and our gear frames lose its
// entity, so it goes from Fragile on both screens too. That is the game's state, not a co-op fault: the logs say so.
#pragma once
#include <cstdint>
#include <cstring>

namespace st::gearwear {

// A durability in percent of its maximum (0..100; not finite, negative or no maximum: 0).
inline double PercentOf(float v, float max) {
    if (!(v > 0.0f) || !(max > 0.0f)) return 0.0;
    const double pc = static_cast<double>(v) * 100.0 / static_cast<double>(max);
    return pc > 100.0 ? 100.0 : pc;
}

// The step the log reports a percentage in: 1 % steps below 10 % (0..9), 10 % steps from 10 % up (10..19; 100 % is 19).
// A change of step is logged, so a cover wearing out in timefall takes 19 lines from full to nothing.
inline int Bucket(double pc) {
    if (!(pc > 0.0)) return 0;
    if (pc < 10.0) return static_cast<int>(pc);
    const int b = 9 + static_cast<int>(pc / 10.0);
    return b > 19 ? 19 : b;
}

// The rain cover's entity: 'ds/entities/item/backpack/backpack_temp', the EntityResource whose GUID starts 64 CE FE 0D
// (logged as 0dfece64; it carries ArtParts part_bak0_cove_def and the helper HLP_RainCover). guid: the 16 GUID bytes.
constexpr char kRainCoverPath[] = "ds/entities/item/backpack/backpack_temp";
constexpr std::uint8_t kRainCoverGuid4[4] = {0x64, 0xCE, 0xFE, 0x0D};
inline bool IsRainCover(const char* path, const std::uint8_t* guid) {
    return path && guid && std::strcmp(path, kRainCoverPath) == 0 && std::memcmp(guid, kRainCoverGuid4, 4) == 0;
}

// Why the cover's entity went from a gear layout (round 11). A load's first layout can still hold the cover's entity
// when the save's backpack has none (the user's round 10 logs: "no rain cover on our backpack" from the first read, then
// the entity went 6 s later); blaming timefall there was wrong. last_worn: our wear read's last worn byte (-1: none yet,
// or the partner's gear: not ours to read); ever_worn: it read worn at any read since the last load (a cover worn out or
// taken off before a load says nothing about the next load's leftover entity); worn_pc: the durability in percent at the
// last read since that load that had it worn (the wear read runs every second, so the entity may go before or after the
// read that sees it off: either way that last worn reading says whether timefall wore it out).
enum class CoverGone : std::uint8_t { unknown = 0, none_in_save, worn_out, taken_off };
inline CoverGone WhyCoverGone(int last_worn, bool ever_worn, double worn_pc) {
    if (last_worn < 0) return CoverGone::unknown;
    if (!ever_worn) return CoverGone::none_in_save;
    return worn_pc <= 1.0 ? CoverGone::worn_out : CoverGone::taken_off;
}
inline const char* CoverGoneText(CoverGone g) {
    switch (g) {
        case CoverGone::none_in_save:
            return "went: Sam's backpack has no rain cover (none worn since the last load), so this was only the load's leftover "
                   "cover entity (not timefall; a cover is equipped in the game; Fragile mirrors Sam on both screens)";
        case CoverGone::worn_out:
            return "is gone: worn out in timefall; Sam loses it, and Fragile with him on both screens";
        case CoverGone::taken_off:
            return "is gone: taken off (it was not worn out); Sam loses it, and Fragile with him on both screens";
        case CoverGone::unknown: break;
    }
    return "is gone (the game removed it: worn out in timefall, taken off, or the save has none; Sam loses it, and Fragile with him on both "
           "screens)";
}

}  // namespace st::gearwear
