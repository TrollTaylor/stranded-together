// Fragile's folded hood hidden while her raised hood is drawn (round 12; docs/real-partner-body.md section 37.6).
//
// Her porter suit (fgl_body_suiti, body key fragile_suit) carries the rolled-down hood on her collar as part of its one body
// mesh: LodMeshResource 96d8545e, LOD0 a MultiMeshResource of 4 meshes, mesh D (RegularSkinnedMeshResource 7f533fe6) the
// roll around her collar (skinned only on joints 5, 6, 29, 30, 31, 35, 106, 107, 108, 139) plus two cloth knee flaps
// (other joints). While the exo carrier draws Sam's raised hood on her (section 37), the roll showed under it: two hoods.
// Sam's own suit hides its HoodOff part then; hers has no part for it, so the mod hides the roll in the mesh resource:
//   the inverse binds:  mesh D's inverse bind matrices of the roll's 10 joint bindings are zeroed (the roll's vertices
//            collapse to a point: zero-area triangles, nothing drawn); the knee flaps keep their bindings;
//   the LOD pin:        her LOD1 distance raised so LOD0 (where the roll is a mesh of its own) is drawn at every distance
//            (in LOD1 the roll is merged into the suit).
// The resource is shared by every fragile_suit body of this game (the self view and the partner body; a cutscene Fragile in
// her porter suit too, so nothing is changed unless one of our bodies wants it and is shown). Everything written is kept
// and written back when no body wants it any more, when the bodies go, and at the emergency stop. Every structure is
// checked before a write (the types by RTTI, mesh D by its GUID, its 22 joint bindings by their joint list, the first
// matrix by its values, the LOD distances); a mismatch turns the hiding off for the session with one log line, after
// everything of ours is written back (a hide that stopped part way included: every zeroed roll matrix and pinned distance).
// A body's resource missing for one tick (a drawable rebuilt) is kept; mesh D not found is tried again (3 times, 1 s apart).
// Fault-free reads, WriteProcessMemory writes. Game thread (partner_gear's tick).
#pragma once
#include <cstddef>
#include <cstdint>

namespace st::hoodfold {

void Configure(bool on) noexcept;  // session.ini hood_fold_hide (default 0: no visible effect, section 37.7)
bool On() noexcept;
// Every tick: the slots' bodies (entities, 0 none) and whether each wants her folded hood hidden now (a fragile_suit body,
// shown, whose exo carrier is shown with a hood part). Hides while any wants it, shows it again otherwise.
void Tick(std::uintptr_t partner_body, bool partner_hide, std::uintptr_t self_body, bool self_hide, double now_ms) noexcept;
// Writes everything back now (the emergency stop, the session's end). Logged when anything was hidden.
void RestoreAll(const char* why) noexcept;
// `gear 34 <method> [nopin]` (hidden tests): method 0 never hide, 1 the inverse binds (the product); nopin 1 the LOD pin off
// (0 or left out: on, the product). -1 the product again (1, pinned).
void SetTest(int method, bool nopin) noexcept;
const char* Describe(char* buf, std::size_t cap) noexcept;

}  // namespace st::hoodfold
