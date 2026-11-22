// Held items on the other screen (docs/real-partner-body.md section 31): the "held clone".
//
// Every item in a player's hand is a DSWeaponEntity built from a DSWeaponEntityResource R. A copy of that entity crashes (its
// update reads a weapon instance, 0x142933FA6), and no cutscene prop exists for the ladder, the PCC, the climbing anchor,
// the bola gun, the spray, the launchers... The receiver takes R as its own game has it loaded (the same item in its own
// inventory: a DSWeaponEntity under its Sam or backpack), and for the one EntityResource::CreateEntity call makes R look like
// a plain EntityResource: the EntityResource vtables (+0x00, and the StreamingRefTarget base at +0x28), a filtered component
// list (heldrules::KeepComponent: no sound, no build-check effect) and message-handler tables whose DSWeaponEntity /
// DSWeaponEntityRep handlers are the plain Entity / EntityRep handlers of the same message. Then R is exactly as it was. The
// result is a plain Entity with the weapon's own model, parts and colours and no weapon logic (docs/real-partner-body.md
// section 31 says why a private copy of R cannot work: the entity's resource reference resolves through R's streaming proxy).
//
// Game thread only. Every address is byte-checked once (Available); a mismatch turns held clones off with one log line.
#pragma once
#include <cstddef>
#include <cstdint>

#include "game_api.hpp"

namespace st::held {

// The byte checks passed (checked once; `why` gets the first mismatch).
bool Available(char* why, std::size_t cap) noexcept;

// Creates a held clone of the weapon resource (path, guid) as our own game has it: found among `local`'s weapon entities
// (under Sam and his backpack). `approx`: the same path with another GUID (another level of the item). Returns false with
// the reason (not owned, not loaded, refused, a fault). The entity is created at `at`, invisible placement is the caller's.
bool Create(game::Entity* local, const char* path, const std::uint8_t (&guid)[16], const game::WorldTransform& at, game::Entity** out,
            bool* approx, char* why, std::size_t cap) noexcept;

// Counters for `gear 26` / log_status.
void Info(char* msg, std::size_t cap) noexcept;

}  // namespace st::held
