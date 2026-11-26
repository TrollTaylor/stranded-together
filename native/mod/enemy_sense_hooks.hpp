// Round 27 (docs/enemy-sync.md 16): the one owner of the sense detours that both the M1 probe (enemy_l1_probe.cpp, its read-only
// counts) and the M3 target pick (enemy_target.cpp) need. MinHook cannot hook one target twice, so each of the three is installed
// once, by whichever user asks first, and every detour calls both users:
//   the classifier 0x142E5FA40 (sense, stim, results, int* type, i64* entity, u32 own id): the pick's H5 / H2 filter before the
//     original (it may refuse her stimulus for this perceiver), the probe's count after it;
//   the result append 0x142E59860 (array, record): the probe's count;
//   ProcessSensorResults 0x142E8DAF0 (K, ...): the probe's record[0] scan, then the pick's reorder / suppress, then the original.
// Worker threads: the users' callbacks use atomics and SEH reads only (no allocation, no logging, no game calls).
#pragma once

#include <cstdint>

namespace st::sense {

enum : unsigned { kCls = 1, kApp = 2, kPsr = 4 };
// Game thread. Installs the hooks in `want` that are not in yet; `ok`: those whose byte checks the caller passed. text[0..2]
// (classifier, append, ProcessSensorResults): "on", "off (byte checks)", "off (MinHook)" or MinHook's error, for each in `want`.
// Returns the hooks in now.
unsigned Ensure(unsigned want, unsigned ok, const char* (&text)[3]) noexcept;
unsigned Installed() noexcept;

}  // namespace st::sense
