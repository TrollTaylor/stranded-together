// Hologram catalog (debug tooling, docs/character-catalog.md): spawns the
// collectors-DB holograms (Type 4 Reward_Holo and Type 20 Reward_Special_Holo)
// one at a time in front of Sam, logs what each one is (item fields, decoded
// name, entity resource, components, DSSimpleAnimation clip list, skeletons,
// shader variables), takes a screenshot (automation runs) and removes it again.
// Also logs a one-time census of the subspace config resources and the
// ControlledEntities near Sam.
//
// Game thread only (st::game::ActionsTick drives it). Reads go through
// ReadProcessMemory; every game call is one of partner_holo's byte-checked calls.
#pragma once
#include <cstddef>

namespace st::catalog {

// Starts a batch: catalog indices [start, start + count) of the sorted Type 4 / 20
// list (index order is logged once). distance_m: how far in front of Sam the
// item stands (0 = default 5 m; it also stands 1.5 m to Sam's right). False (msg says why) when a batch is running
// or nothing can be spawned.
bool Start(int start, int count, double distance_m, char* msg, std::size_t cap) noexcept;
// One step per game tick.
void Tick(double now_ms) noexcept;
bool Running() noexcept;

// Animation clip survey (game thread to start): logs the named borrowable clip
// sources (Sam's PlayerPoseAnimations, DSExternalAnimationResource), then a
// worker thread scans committed memory (ReadProcessMemory only) for every loaded
// SkeletonAnimationResource and logs the ones with root motion or a walk/run-like
// path, marking those on Sam's skeleton layout. "[survey] done" ends it.
bool StartClipSurvey(char* msg, std::size_t cap) noexcept;

}  // namespace st::catalog
