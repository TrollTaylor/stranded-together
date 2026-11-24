// Enemy sync, P1-0 (docs/enemy-sync.md 4.7, plan R1b): the freeze probe. Hidden runs only: it freezes one realized MULE with
// one lever, drives its place for a few seconds, then gives it back and watches it, and logs whether it held its place, stayed
// silent, kept its animation (and took a written pose) and came back to life. Its result decides P1a's freeze lever (or the
// puppet path). No mirror, no wire: the next round builds P1a on what it finds.
//
//   enemy_probe 0 [radius_m=120]   byte checks (group freeze), then pick the nearest realized, live MULE / Demens not held by a
//                                  sequence: "[enemy] probe pick id=..."
//   enemy_probe 3 <secs> <path>    the next tests' length (default 8 s) and path (0 hold its place, 1 a 1 m/s line along its heading)
//   enemy_probe 1 <lever> <mode>   freeze with the lever, drive it with the mode for <secs>, release, watch 15 s, one summary line
//                                    lever: 0 none (control), 1 Occupied{1}, 2 Occupied{1} + Required{1}, 3 Sleep (vt+0x50), 4 Occupied{1} + Sleep
//                                    mode: 0 no placement, 1 per tick the actor's vt+0x10 + Entity_ExportedSetWorldTransform,
//                                          2 per tick the actor teleport 0x1422F8D90, 3 a teleport every 500 ms with mode 1 between
//   enemy_probe 2 <secs>           during a running test: our Sam's live pose written onto its SkinnedModel (the animate hook,
//                                  pose_hook=1), the animate rate and the largest joint error logged
//   enemy_probe 4 [m]              our Sam m (default 10) from the picked MULE, facing it (the topmost ground there)
//   enemy_probe 9                  release now (Occupied{0}, Required{0}, wake / show what we slept / hid)
#pragma once

#include <cstddef>
#include <cstdint>

#include "game_api.hpp"

namespace st::esync {

bool Probe(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;  // game thread (a debug action)
void Tick(game::Entity* local, bool local_ok, double now) noexcept;              // game thread

}  // namespace st::esync
