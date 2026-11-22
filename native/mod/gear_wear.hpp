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

}  // namespace st::gearwear
