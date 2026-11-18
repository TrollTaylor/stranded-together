// Gear looks: how each mirrored gear item is coloured (docs/real-partner-body.md section 13).
//
// The game colours a gear entity at run time on the entity itself, through two channels:
//   - ArtPartsVariation components (texture-set swaps: the gold / silver Bridges cap, the backpack's
//     paint, the rain cover's level, a shield's level, the canteen, a case's colour), and
//   - its ArtPartsShaderVariableComponent (ArtPartsData +0xF0): sorted entries of
//     {key (hash | part bits), hash, f32[4], count, removed} (colour indices, lens colours ...).
// A copy spawned from the entity resource has neither, so it shows the material's default (the black
// cap of the user's report). The sender reads both from each entity it mirrors (fault-free), the
// receiver resolves each variation GUID among resources its own game already holds (no loads) and
// applies variations, then shader variables, to its copy with the game's own functions (byte-checked).
//
// Game thread only (except nothing: no detour use).
#pragma once
#include <cstddef>
#include <cstdint>

#include "game_api.hpp"
#include "pose_wire.hpp"

namespace st::look {

// The read side (fault-free) and the apply side (game calls, byte-checked once; a fault turns the
// apply side off for the session). Logged once.
bool ReadAvailable() noexcept;
bool ApplyAvailable() noexcept;

// Sender: the look of one of our gear entities (applied variations by GUID in component order, live
// shader variables). False when the entity's ArtParts do not check out.
bool ReadEntity(std::uintptr_t e, wire::GearLook* out) noexcept;
// Sender: the case variation the game gives one carried DSBaggage2 (the baggage manager's own lookup
// 0x142C541D0, called on the game thread); an empty look (nvar 0) when the game gives none.
bool ReadBaggage(std::uintptr_t bag, wire::GearLook* out) noexcept;
// Sender (hidden tests' synthetic cases): the baggage config's variation `index` as a look.
bool ReadConfigVariation(unsigned index, wire::GearLook* out) noexcept;

// Receiver. The resources a look may name are found among those this game holds (the accessory
// table, the item infos, our backpack's resource, the baggage config, our own gear entities, the
// prop's own component resources, the backpack config): `ents` = our own gear entities (and backpack)
// this read, `player` = our player (its components' resources).
void SetOwnGear(const std::uintptr_t* ents, unsigned n, std::uintptr_t player) noexcept;
enum class ApplyResult : std::uint8_t { done, partial, failed, off };
// Applies `want` to the prop entity: variations made equal to the wanted list (unwanted ones
// removed, missing ones added), then each shader variable added / set. `why` says what was not done.
ApplyResult Apply(std::uintptr_t prop, const wire::GearLook& want, char* why, std::size_t cap) noexcept;
// True when the prop shows `want` (its variations are the wanted ones that can be resolved, every
// wanted shader variable present with the wanted values).
bool Satisfied(std::uintptr_t prop, const wire::GearLook& want) noexcept;
// Counters for the log: applies, variations added / removed, variables set, unresolved GUIDs.
void Counters(char* out, std::size_t cap) noexcept;

// Skinned pose of an entity (its SkinnedModel's current local joints; read twice, equal reads only).
bool ReadPose(std::uintptr_t e, wire::GearPose* out) noexcept;
// The skeleton (pointer, layout hash, joint count) of an entity's current pose.
bool PoseSkeleton(std::uintptr_t e, std::uintptr_t* skel, std::uint32_t* layout, std::int32_t* count) noexcept;

// Debug: the look of an entity in full (variations with names, every shader variable, its pose's
// skeleton and how its local joints compare with `ref`'s), and where each of our gear's variation
// resources can be found (the receiver's sources).
void Dump(const char* tag, std::uintptr_t e, std::uintptr_t ref) noexcept;
void DumpSources(const std::uintptr_t* ents, unsigned n, std::uintptr_t player) noexcept;
void DumpJoints(const char* tag, std::uintptr_t e) noexcept;

}  // namespace st::look
