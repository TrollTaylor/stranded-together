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

// Vehicles (docs/real-partner-body.md section 22.7; vsync). Sender: the look of a drivable vehicle: its applied
// variations (gold / silver / broken / ride type) and its paint: the shader variable colour index 0x3A3714ED per
// ArtParts index (the customised primary / secondary colours); no parts (the game switches them itself).
bool ReadVehicle(std::uintptr_t e, wire::GearLook* out) noexcept;
// Receiver: Apply on a vehicle copy, with the variation GUIDs resolved only from the copy's own component
// resources and from `own` (our own registered vehicles of the sender's drivable resource: their resources' gold /
// silver / broken / ride-type references and their variation components). Nothing else is a source.
ApplyResult ApplyVehicle(std::uintptr_t copy, const wire::GearLook& want, const std::uintptr_t* own, unsigned n_own, char* why,
                         std::size_t cap) noexcept;
// Satisfied with the vehicle copies' shader-variable cap (160 entries instead of 48).
bool SatisfiedVehicle(std::uintptr_t copy, const wire::GearLook& want) noexcept;

// Vehicles, the state the game changes as it drives (docs/real-partner-body.md section 22.12; vsync, STM1 version 3).
// Sender: the wear (timefall corrosion 0x32D5AE44: one value, the largest of its entries, on the parts it is set on) and
// the lights (every 0x2B11751D intensity and 0x0978E0A1 battery-gauge colour entry, by ArtParts index) of a vehicle,
// fault-free reads of its ArtPartsShaderVariableComponent. The 0x42A726AA entries (1.0 on every part: the camera's
// dither / fade, the watcher's own) are never read.
struct VehicleState {
    std::uint16_t wear = 0;            // 0..65535 = 0..1
    std::uint64_t wear_parts[2] = {};  // the parts carrying 0x32D5AE44 (index < 128)
    unsigned wear_n = 0;               // entries read
    float wear_spread = 0;             // the largest value less the smallest (one value is expected)
    std::uint8_t nl = 0;
    wire::VehicleLight light[wire::kMaxVehicleLights];
    unsigned left_out = 0;             // entries over the caps (part >= 128, more than 24 lights)
    unsigned entries = 0;              // all shader-variable entries of the vehicle
};
bool ReadVehicleState(std::uintptr_t e, VehicleState* out) noexcept;
// Receiver: shader variables added (up to the vehicle cap) and set on a vehicle copy, nothing else (no variation change).
ApplyResult SetVehicleVars(std::uintptr_t copy, const wire::GearShaderVar* vars, unsigned n, char* why, std::size_t cap) noexcept;
// Hidden tests: the live entries of one hash on an entity (fault-free), for the read-back checks.
unsigned ReadVars(std::uintptr_t e, std::uint32_t hash, wire::GearShaderVar* out, unsigned cap) noexcept;

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
// Debug: the variation resources the entity's own resource refers to (its first 0x400 bytes: direct references and
// arrays; a vehicle's gold / silver / broken / ride-type variations), with their offsets, GUIDs and names.
void DumpResourceVariations(const char* tag, std::uintptr_t e) noexcept;

}  // namespace st::look
