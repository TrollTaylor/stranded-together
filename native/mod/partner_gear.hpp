// Gear: what the player carries, on the partner's real body (and the BB pod on the self view's body;
// the self view keeps Sam's real gear, real_body.cpp) - docs/real-partner-body.md section 12.
//
//   Sender: every 100 ms (game thread), fault-free: every visible gear entity the game shows on Sam
//           (his child entities and theirs: everything on the backpack, shields, hanging boots, the
//           pod's tags ...) that passes the mirror policy, named by its entity resource (a session
//           table sent as STR1), the backpack (the cinematic frame), the BB pod (the cinematic pod),
//           delivery cargo from the baggage carrier (player +0x54D0) as cases, Sam's own gear parts
//           (exoskeleton, BB pod pipe, hip gear: ArtParts shown bits) and the backpack's parts (its bag,
//           rain cover, pouches). Each item has a pose relative to an anchor: a joint of Sam's skeleton
//           or its parent item. STC1 v2 (pose_wire.hpp) goes out when the layout or the parts change
//           and about once a second.
//   Receiver: one prop per item, keyed by what it shows (a cinematic prop or a resource id), loaded
//           by path + GUID like the bodies (realbody::BeginByPath / BeginByPathGuid) from the user's
//           own installed game, validated, spawned invisible, shown with the body, placed every frame
//           by the pose detour on the body's own joints (realbody::PublishGear), retried after a
//           failed start / load, removed with the body. The parts are set on a Sam body and on the
//           backpack prop when their ArtParts are the same resources as ours.
//
// Game thread only (Tick, SetPartnerGear, LocalFrame, DebugCommand); the detour placement lives in
// real_body.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"
#include "pose_wire.hpp"

namespace st::gear {

void Init(const Config& config) noexcept;
bool Enabled() noexcept;

// Per tick, after realbody::Tick and the partner body's move: reads our gear (every 100 ms),
// keeps the partner's and the self view's props in step with their bodies.
void Tick(game::Entity* local, bool local_active, double now_ms) noexcept;

// Sender schedule: true when a gear frame should go out now (the layout changed, a pose moved by
// more than 2 cm / 4 deg, or 1 s since the last one); `out` is the frame (seq filled by the caller).
bool TakeSend(double now_ms, wire::GearFrame* out) noexcept;

// The partner's gear (a received or echoed frame); nullptr: none (a new session, link loss; its
// resource table is cleared too).
void SetPartnerGear(const wire::GearFrame* f) noexcept;

// Our gear resource table (STR1): true when an entry should go out now (a new id at once, then one
// entry every 250 ms round-robin). The partner's entries (received or echoed).
bool TakeSendRes(double now_ms, wire::GearRes* out) noexcept;
// An entry (id) or the whole table (-1) goes out again first: a send that failed (not connected yet),
// a new link session (the partner cleared its table).
void MarkResUnsent(int id) noexcept;
// The partner's entries. Checked before anything is loaded (partner_gear.cpp ResAllowed): only the
// proven gear cores, never the resources the mirror policy refuses.
void SetPartnerRes(const wire::GearRes& r) noexcept;

// Gear looks (STV1, docs/real-partner-body.md section 13): the look of one item of our current layout
// when it should go out now (a changed one at once, then one every 250 ms round-robin); the caller may
// ask up to 4 times a tick. MarkLooksUnsent: a new link session (every look and the pose again).
// SetPartnerLook: a received / echoed one (kept per item; used while its layout is the partner's).
bool TakeSendLook(double now_ms, wire::GearLook* out) noexcept;
void MarkLooksUnsent() noexcept;
void SetPartnerLook(const wire::GearLook& l) noexcept;
// The backpack's skinned pose (STK1): ours when it should go out (changed by more than 1 mm / 0.5 deg,
// at most every 100 ms; else every 1 s), the partner's (written onto our copies of its backpack).
bool TakeSendPose(double now_ms, wire::GearPose* out) noexcept;
void SetPartnerPose(const wire::GearPose& p) noexcept;

// The per-frame sync faulted: the detour stops placing props (they stay where they are).
void EmergencyStop() noexcept;

// Debug (docs/debug-actions.md "Gear"): 0 log (our gear in full, the props), 1 <0/1> partner props
// off/on, 2 <0/1> self-view props off/on, 3 <n> hidden tests only: n synthetic delivery cases on our
// backpack, 4 <0/1> debug: our own gear on the debug body (real_spawn) on/off, 5 <n> / 6 hidden tests:
// the next n prop starts fail / every partner prop hidden behind our back, 7 <0..2> mirror level,
// 8 <0/1> the BB pod as a copy of the game's pod entity (tests), 9 <0/1/2> census, 10 <0/1> walk cache,
// 11 <0/1> looks of our gear / the partner's props (log), 12 <0/1> looks off / on, 13 <0/1> backpack pose
// sync off / on, 14 / 15 accessories / cap colour (tests), 16 <0/1> head parts, 17 <0/1> weapons off / on
// (section 17: weapons in a hand and on the backpack, drawn as cutscene props).
bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::gear
