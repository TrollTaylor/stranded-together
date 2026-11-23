// Fragile's face (docs/real-partner-body.md section 36): the facial animation Sam's own face plays, on the self view's
// body (phase 1) and, through the STJ1 face block, on the partner's body on the other screen (phase 2).
//
// A body the mod spawns (demo_fgl_body_suiti, demo_fgl_new, demo_sam_clean) carries a DSFacialRigComponent but no
// facial manager, so nothing ever plays on its face. Sam's DSPlayerFacialComponent picks his clip each frame (a state,
// an event clip, a private-room gesture, photo mode); this module reads that choice and gives the same clip to the
// body's rig with the game's own DSFacialRigComponent::PlayFacial (0x1422CC710, byte-checked with the code that keeps
// the manager's tables), once per change, from the game thread. Both heads share one rig skeleton and each has its own
// mapping, so Sam's clips retarget onto her face the game's way. A fault in a call switches it off for the session.
// The pure part (ids, the choice, when to call) is face_rules.hpp.
#pragma once
#include <cstddef>
#include <cstdint>

#include "pose_wire.hpp"

namespace st::facesync {

struct TickIn {
    std::uintptr_t sam = 0;            // the local player while awake (0: asleep, loading, absent)
    std::uintptr_t self_body = 0;      // the self view's body entity while it is shown in Sam's place and its spawn alive, else 0
    std::uint32_t self_epoch = 0;      // its showing and spawn (a new show or body gives the clip again)
    const char* self_key = "";
    std::uintptr_t partner_body = 0;   // the partner's real body entity while live and its spawn alive, else 0
    std::uint32_t partner_epoch = 0;   // its spawn count (a new body at the same address gives the clip again)
    const char* partner_key = "";
};

// Game thread, once per tick (realbody::Tick).
void Tick(const TickIn& in, double now_ms) noexcept;

// The sender (player_sync, game thread): our own Sam's face for the STJ1 face block. False when it is not known (the
// sites did not check out, no facial manager, partner face off): the block is then not sent.
bool OwnFace(std::uintptr_t sam, wire::Face* out) noexcept;

// The receiver (player_sync, game thread): the partner's face from a received or echoed STJ1 frame; nullptr on a new
// link session.
void SetPartnerFace(const wire::Face* f) noexcept;

// `self_view 20 <mask>`: bit 1 her self view's face, bit 2 the partner body's face and the sent block (3 the default, 0
// off); `self_view 21`: the status line.
bool SetMode(int mask, char* msg, std::size_t cap) noexcept;
void Status(char* msg, std::size_t cap) noexcept;

}  // namespace st::facesync
