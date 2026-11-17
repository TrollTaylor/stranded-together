// Animated-body census and Sam's pose memory (debug tooling for the "real player
// models" research, docs/character-catalog.md). Reads only: every game read goes
// through ReadProcessMemory (partner::mem), no game function is called except the
// existing byte-checked teleport (game::TeleportLocalPlayer).
//
//   * StartAnimCensus: logs every ControlledEntity in the world list that has an
//     AnimationManager (or is a Humanoid / AIEntityType Humanoid / a skinned body on a
//     77-joint skeleton): type, resource, components, skeleton, animation manager and
//     its resource, position, flags. Also the subspace (akukan) Sam resource and whether
//     a live entity was made from it. Optionally a worker thread then scans committed
//     memory for AnimationManager components and Humanoid entities outside the world
//     list, and for DSDeliveryPointInfoResource / DSMuleCPGameActorLocator objects
//     (named places with world positions).
//   * PoseDump: dumps Sam's AnimationManager, its resource, his Model and the model
//     resource plus the buffers they point to (and every array of ~77 entries) to
//     <automation out>\pose-<tag>-<n>.bin, on `frames` consecutive ticks after a delay.
//   * StartPoseScan: worker thread; finds float/double triples close to Sam's world
//     position (world-space joint matrices) and references to Sam's Skeleton object.
//   * LookAt: teleports Sam so the n-th body of the last census stands `dist` m in
//     front of him (for a screenshot).
#pragma once
#include <cstddef>

namespace st::census {

// scan: 0 = world list only; bit 1 = also scan memory for AnimationManagers and Humanoids
// outside the world list; bit 2 = also scan for delivery points and MULE camp locators.
// radius_m <= 0: no distance limit. "[census] done tag=<tag>" ends it (after the scan).
bool StartAnimCensus(double radius_m, int tag, int scan, char* msg, std::size_t cap) noexcept;
bool PoseDump(int tag, int frames, double delay_ms, char* msg, std::size_t cap) noexcept;
bool StartPoseScan(int tag, char* msg, std::size_t cap) noexcept;
bool LookAt(int index, double dist_m, char* msg, std::size_t cap) noexcept;
// Game thread, every tick (delayed pose dumps).
void Tick(double now_ms) noexcept;

}  // namespace st::census
