// The exact model of a carried baggage piece on the other screen (docs/real-partner-body.md section 39): the "baggage model".
//
// The game draws a piece Sam carries (the weapon case and the folding ladder at his hips, ...) with one of the 31
// SkinnedModelResources of its baggage config (DSBaggage2Config.ModelResources, EDSBaggage2Model order); the piece has no
// entity of its own, and no cutscene prop exists for the weapon cases or the ladder. The receiver makes a plain Entity that
// shows exactly that model: the baggage config's own plain EntityResource (CargoStrandEntityResource, an EntityResource with
// 3 components of its own at run time, none of them the model) is given, for the one EntityResource::CreateEntity call, a
// component list holding the model resource (and the extras), and is restored at once (the held clone's method,
// held_items.cpp, without any re-typing: the resource is already a plain EntityResource). Entity::Init (0x14234F3E0) builds
// the SkinnedModel from that list (and the ArtPartsData the model names, 0x1423760C0); a component the resource's own
// message tables do not know is registered through the game's runtime path (0x142348220 -> 0x1423714B0), as for any
// component added later. Once restored, the entity's resource is the strand again (its own 3 components): anything the
// game looks up later through the resource does not see the model (an open lead of section 39.5). A private resource is
// not an option: one with its own identity resolves to nothing through the streaming proxy (section 31.1, the fault of run
// 20261122-125554).
//
// Game thread only. The code addresses are byte-checked once (Available). The config in full (its type, the array's size
// and element types, the model array cross-checked against the config's own baggage entity resource, the host's vtables and
// lists) once per config pointer; before every call: the config still names the host and the model, the host's vtables and
// component count, the model's type; after every call: the host restored. A mismatch is refused (the full check runs again
// next time); a fault or a host not restored turns baggage models off with one log line (the new entity removed). The swap
// window: a strand entity the game built during our one call would get our list (the same window as held clones).
#pragma once
#include <cstddef>

#include "game_api.hpp"

namespace st::bagmodel {

// The byte checks passed (checked once; `why` gets the first mismatch) and baggage models were not turned off.
bool Available(char* why, std::size_t cap) noexcept;

// Creates a plain Entity showing EDSBaggage2Model `model` (0..30) at `at` (invisible placement is the caller's). `extra` (up to
// 3, 0 entries skipped): component resources added after the model, each of an allowed type (DSSimpleAnimationComponentResource:
// the item's own animation on the model's skeleton; ExternalAnimationComponentResource: the weapons' generic one; or
// StencilBufferComponentResource), all from our own game. False with the reason (off, the config does not check out,
// refused, a fault).
bool Create(unsigned model, const std::uintptr_t* extra, unsigned n_extra, const game::WorldTransform& at, game::Entity** out, char* why,
            std::size_t cap) noexcept;

// Round 15 (docs/real-partner-body.md section 39.6): after CreateEntity, each of our components' representation is given its
// class's message handlers with the game's own runtime registration (Entity::Init binds them only from the strand
// resource's own rep table, which does not know the model). `gear 36 5 <0/1>` (default on): off = round 14's behaviour.
void SetBind(bool on) noexcept;
bool BindOn() noexcept;
// The model's rep of a baggage model entity: handlers bound to it on the entity rep's dispatcher and its mesh entry count
// (ModelRep +0x40; > 0 once the rep built its meshes on MsgEntityInit). False (-1s) when the layout does not check out.
bool RepState(game::Entity* e, int* bound, int* entries) noexcept;
// A made entity found with its handlers bound but no mesh entry (partner_gear's check 1.5 s after it was made): counted as
// not made (Info's "undrawn").
void NoteUndrawn() noexcept;

// Counters for `gear 36` / the status line.
void Info(char* msg, std::size_t cap) noexcept;

}  // namespace st::bagmodel
