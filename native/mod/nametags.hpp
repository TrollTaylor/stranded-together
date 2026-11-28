// Name tags, game side (docs/real-partner-body.md section 10): which name, where and whether.
//
// Game thread (st::sync::Tick): sends our own name over the link (STN1, link.hpp), composes the
// partner's label, reads the world anchor above the partner body and the gates (game state,
// loads, Sam asleep, no partner), finds the game's camera view object and publishes all of it to
// the overlay (overlay_tags.hpp). The overlay reads the camera at Present through ReadView here
// (ReadProcessMemory only, vtable + byte checked) and draws.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"

namespace st::nametags {

void Init(const Config& config) noexcept;  // after link::Start
// Every tick, after the partner body was moved. partner = the live partner body entity (any
// kind) or nullptr; partner_fresh = its position is current (remote samples arriving).
void Tick(game::Entity* local, bool local_active, game::Entity* partner, bool partner_fresh, double now_ms) noexcept;
// The game camera's forward direction, horizontal and normalised (what the pad's forward walks
// towards). False when the camera view is unavailable. Collision tests (action collision 3).
bool CameraForward(game::Entity* local, double (&fwd)[2]) noexcept;
// The game camera's position and forward axis (unit) as the view object holds them this frame. False when unavailable.
// The ride camera's meter (ride_camera.cpp).
bool CameraView(game::Entity* local, double (&pos)[3], double (&fwd)[3]) noexcept;
// Round 30 BT marks (bt_markers.cpp): the game's camera view object the overlay reads (0 when unavailable: byte checks, no view).
std::uintptr_t CameraViewObject(game::Entity* local) noexcept;

}  // namespace st::nametags
