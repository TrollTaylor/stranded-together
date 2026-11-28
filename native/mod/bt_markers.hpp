// Round 30 BT S1 (docs/enemy-sync.md section 24): the partner's BT trouble as small in-world markers, game side. bt_sync.cpp feeds
// what TROUBLE and GZST say (a gazer sensing the partner, the partner hunted, a catcher fight with its health); this keeps them in a
// btrules::MarkerBook (each gone 3 s after its last refresh, at most 8) and publishes them to the overlay (overlay_tags.hpp) every
// tick under the name tags' gates (no menu / cutscene / load, Sam awake). Nothing here changes the game; no text is drawn.
// bt_markers=0: nothing is kept or drawn (the relay still logs).
#pragma once
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"

namespace st::btmarks {

void Init(const Config& config) noexcept;
bool On() noexcept;

// Game thread (bt_sync.cpp's message handlers).
void Gazer(std::uint32_t loc, const double (&pos)[3], bool test, double now_ms) noexcept;
void DropGazer(std::uint32_t loc) noexcept;
// The partner hunted: the ring goes on the partner body when it is shown and fresh, else on the position the message carried.
void Tar(const double (&pos)[3], bool gold, bool test, double now_ms) noexcept;
void Catcher(const double (&pos)[3], float hp_pct, double now_ms) noexcept;
void ClearTrouble() noexcept;  // tar and catcher gone at once (CLEAR)

// Game thread, every tick after the name tags (player_sync.cpp).
void Tick(game::Entity* local, bool local_active, game::Entity* partner, bool partner_fresh, double now_ms) noexcept;

}  // namespace st::btmarks
