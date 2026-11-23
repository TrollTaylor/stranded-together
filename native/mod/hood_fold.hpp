// Fragile's folded hood hidden while her raised hood is drawn (rounds 12-13; docs/real-partner-body.md sections 37.6-37.8).
//
// Her porter suit (fgl_body_suiti, body key fragile_suit) carries the rolled-down hood on her collar as part of its one body
// mesh: LodMeshResource 96d8545e, LOD0 a MultiMeshResource of 4 meshes, mesh D (RegularSkinnedMeshResource 7f533fe6) the
// roll around her collar (skinned only on joints 5, 6, 29, 30, 31, 35, 106, 107, 108, 139) plus two cloth knee flaps
// (other joints). While the exo carrier draws Sam's raised hood on her (section 37), the roll showed under it: two hoods.
// Sam's own suit hides its HoodOff part then; hers has no part for it.
//
// Round 12 zeroed the roll's inverse binds in the shared mesh resource: nothing changed on screen, because the game reads
// neither after a body's drawable is built (37.8: the skinning uses bindings merged once per LodMeshResource, and the LOD
// thresholds are copied into each body's own mesh instance tree). Round 13 works per body, in that body's own instance
// tree (each body's mesh entry builds one; a cutscene Fragile's is never touched):
//   the mask (method 1):  mesh D's primitive slot gets view mask 0, the byte the game's own ArtPart hide writes into every
//            slot of a part (Sam's HoodOff); the culler skips the slot in every view, shadows too. The whole of mesh D
//            goes: the roll and the knee flaps (one primitive);
//   the private bindings (method 2, for tests): mesh D's node points at a copy of the merged bindings with the roll
//            joints' inverse binds zeroed (the roll collapses to a point, the knee flaps stay);
//   the LOD pin:  the tree's LOD1 threshold (2 m) raised to its MaxDistance, so LOD0 (where mesh D is a mesh of its own)
//            is drawn while the roll is hidden (in LOD1 the roll is merged into the suit). Or (pin 2, for tests) the
//            drawable's LodDistanceScale, the engine's ModelLodBiasEvent field.
// Each body is found from its entity every tick (entity -> drawable -> ModelRep -> her entry -> its tree holder -> the
// tree's LodMesh root, her LOD0, mesh D's node by its resource's GUID), every node bounds-checked in the tree's buffer;
// the 26 code sites that make these layouts (and her entry's part visibility, which writes the slot masks) are
// byte-checked before the first write (a mismatch: off for the session). Every write is a compare-and-swap of the value
// just read (SEH-guarded); what is ours is written back when the body no longer wants it (the raised hood gone), when the
// body goes (realbody's teardown calls BodyGoing while it is still alive; a body the game destroyed is never walked),
// when the hiding goes off (a race in that write-back tried again every second) and at the emergency stop. The mask goes
// back as the entry's current view mask, and not at all while the game hides the body. A tree the game rebuilt (a colour
// change) has the game's values again: nothing to write back, the hide is applied again (a one-frame flash). Game thread
// (partner_gear's and realbody's ticks).
#pragma once
#include <cstddef>
#include <cstdint>

namespace st::hoodfold {

void Configure(bool on) noexcept;  // session.ini hood_fold_hide (default 1 since round 13)
bool On() noexcept;
// Every tick: the slots' bodies (entities, 0 none) and whether each wants her folded hood hidden now (a fragile_suit body,
// shown, whose exo carrier is shown with a hood part). Each body follows its own.
void Tick(std::uintptr_t partner_body, bool partner_hide, std::uintptr_t self_body, bool self_hide, double now_ms) noexcept;
// Writes everything back now (the emergency stop, the session's end; `gear 34 -2` in hidden tests). Logged when anything
// was hidden.
void RestoreAll(const char* why) noexcept;
// realbody's teardown, while the body (entity) is still alive: everything of ours on it written back, then forgotten.
void BodyGoing(std::uintptr_t body) noexcept;
// `gear 34 <method> [pin]` (hidden tests): method 0 never hide, 1 the mask, 2 the private bindings, 3 the LOD pin only, 4 /
// 5 the mask / the private bindings on her bodies with no raised hood needed (her bare collar in the stills);
// pin 0 (or left out) the tree's threshold, 1 off, 2 the LOD scale. -1 the product again. (Methods 2, 4, 5: hidden tests
// only, checked by partner_gear.)
void SetTest(int method, int pin) noexcept;
const char* Describe(char* buf, std::size_t cap) noexcept;

}  // namespace st::hoodfold
