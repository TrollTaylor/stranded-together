// The partner as a real (solid, non-hologram) character that plays the other
// player's real animation (docs/real-partner-body.md).
//
//   Body: one of the game's own cutscene-actor entities (body_table.hpp, e.g.
//         ds/entities/cinematics/demo_sam_clean), loaded by path + GUID through the
//         streaming manager's own Register function (a StreamingRef we build from a
//         temporary proxy) and created with the same asynchronous spawner the
//         hologram body uses (partner_holo.hpp SpawnBeginField).
//   Animation: pose sync. A MinHook post-hook on the SkinnedModel animate function
//         (ds.exe 0x1423AA540, `void(SkinnedModel*, float dt)`) copies Sam's finished
//         local joint pose after the game computed it, and overwrites the partner
//         body's finished pose with the received one (then reruns the game's own
//         model-matrix and bounds steps, exactly the tail of that function), before
//         cloth, post-update messages and rendering read it.
//
// Threading: everything here runs on the game thread (st::sync::Tick) except the
// detour, which runs on whatever thread updates a SkinnedModel. The detour only
// compares two atomics for every other model; for Sam's and the partner's model it
// reads game memory with ReadProcessMemory, writes the partner's joints with
// WriteProcessMemory (both fail instead of faulting), and calls the two pure-math
// game functions the original just called on the same pose on the same thread.
// No logging, allocation or locks in the detour; counters only.
#pragma once
#include <cstddef>
#include <cstdint>

#include "body_table.hpp"
#include "game_api.hpp"
#include "partner_holo.hpp"
#include "pose_wire.hpp"

namespace st::realbody {

// ---- startup (Bootstrap, after MH_Initialize, before the frame pump hook):
// byte-checks every address and installs the animate hook (disabled detour work
// until a body or a capture is published). Logged; false when unavailable.
bool InstallHook() noexcept;
bool HookInstalled() noexcept;
// All spawn functions verified (lazy, logged once). `why` gets the reason otherwise.
bool Available() noexcept;
const char* UnavailableReason() noexcept;

// ---- per tick (game thread), before the partner logic: publishes Sam's
// SkinnedModel to the detour, drives a debug-owned spawn, mirror mode and the
// per-second [pose] counters.
void Tick(game::Entity* local, bool local_active, double now_ms) noexcept;

// ---- the body (the partner slot, owned by the partner sync or by a debug action; the self
// view has a second slot of its own, below)
enum class Owner : std::uint8_t { none, partner, debug, self };
enum class Phase : std::uint8_t { idle, loading, live };
enum class BeginResult : std::uint8_t { started, busy, unavailable, failed };
BeginResult Begin(const bodies::Body& body, const game::WorldTransform& at, Owner owner, double now_ms, char* why,
                  std::size_t cap) noexcept;
enum class PollResult : std::uint8_t { idle, loading, became_live, live, failed };
PollResult Poll(double now_ms, char* why, std::size_t cap) noexcept;
bool Move(const game::WorldTransform& at) noexcept;
bool Remove(const char* why) noexcept;  // drains the detour first; true if there was something
Phase GetPhase() noexcept;
Owner GetOwner() noexcept;
const char* PhaseName() noexcept;
game::Entity* LiveEntity() noexcept;
const bodies::Body* CurrentBody() noexcept;
// A body that failed to load or validate is not tried again this session.
bool Refused(std::uint8_t body_id) noexcept;
void ResetRefused() noexcept;

// ---- pose stream (game thread)
// Sam's newest captured pose as a frame (77 rotations + root/pelvis translation).
// False when nothing fresh (< 250 ms) was captured.
// `capture_ms` (optional): when the game computed that pose, on link::NowMsPrecise's clock (QPC).
bool CaptureFrame(wire::JointFrame* out, double* capture_ms = nullptr) noexcept;
// Received / echoed frames, keyed by the time they should play at (our clock). False when the
// frame was dropped: not Sam's 77-joint layout (the body could not play it), or not newer than
// the ring's newest key (reordered). Only accepted frames count as "the peer sends joints".
bool PushFrame(const wire::JointFrame& f, double key_ms) noexcept;
void ResetFrames() noexcept;
// True when the body has been live for `limit_ms` without any frame pushed in that time (a
// same-build peer that sends no STJ1: partner_body=holo, or its capture is unavailable, paused or
// behind a notice; other builds are rejected at the link handshake).
bool StarvedOfFrames(double limit_ms) noexcept;
// ---- outfit (docs/real-partner-body.md section 9; game thread)
// This game's own Sam outfit (sent in STJ1). False when unavailable (byte checks, no player).
bool ReadOutfit(game::Entity* local, wire::Outfit* out) noexcept;
// The outfit the partner sent (nullptr: none / a new session). The partner's Sam body
// (demo_sam_clean) is dressed with it: its own darker suit variation removed, the exos
// variation added, the suit colour set.
void SetPartnerOutfit(const wire::Outfit* o) noexcept;

// Interpolates the ring at play_ms and publishes the result to the detour.
// False when the ring is empty (the detour keeps the last pose).
bool SamplePublish(double play_ms) noexcept;

// ---- self view (docs/real-partner-body.md section 8): on THIS player's own screen the local
// Sam is replaced by the chosen character. A second body (its own slot) stands at Sam's world
// transform, copied in the detour every frame Sam's model is updated, and plays Sam's live pose
// (the newest capture, no wire, no delay). Sam's own model is hidden through his mesh-visibility
// controller (player +0xA60, Root hide-reason mask +0xD98, reason 63 ours, set and cleared with
// locked operations); the game's own hide reasons are never touched. Sam is hidden only while
// the body is shown and posed: Sam asleep (loads, cutscenes, knot space), any hide reason of the
// game's own (photo mode, scripted scenes), a pose that stops, a removed body or a new player
// show Sam again first. Everything runs from Tick (game thread).
// `body` = the character to show in Sam's place, nullptr = off (the host, Sam chosen, or
// session.ini self_character=0). Needs the pose hook.
void ConfigureSelf(const bodies::Body* body) noexcept;
// Self view fit (session.ini self_fit, default on): while the self body is shown, Sam's hidden
// skeleton is reshaped to the body's bone lengths (docs/real-partner-body.md section 16).
void ConfigureSelfFit(bool on) noexcept;
// The fit's head lift (section 16.1): Sam's head joint sits this share (percent) of the way from the
// character's head back to where his own head would be (his cap and glasses on HLP_Head were sinking
// over her eyes). `self_view 7 <percent>`.
inline constexpr int kDefaultHeadLiftPct = 100;
// The lift now applied to `sam`'s head joint (joint 31; its subtree 31-34 moves with it), in the neck
// joint's (30) frame of Sam's skeleton: the gear sender takes it off the head joints' frames, so items
// on the head are measured against the character's real head and the other screen gets the same lift.
// False (local zeroed) when no lift is applied to that entity now.
bool HeadLift(game::Entity* sam, float (&local)[3]) noexcept;
// Self view headgear (docs/real-partner-body.md section 20; session.ini self_headgear, default on): with the fit on and a
// self body whose head rig has a row, each of Sam's head accessories gets a constant offset of its own in his head
// helper's frame (HLP_Head: x = -lateral, y = forward, z = up; metres), written into the accessory's own mover by
// partner_gear.cpp, and the head lift above is 0. False (off zeroed): no offset now (the mover's identity applies).
inline constexpr int kHeadItemCap = 0, kHeadItemGlasses = 1, kHeadItemClasses = 2;
bool SelfHeadItemOffset(int cls, float (&off)[3]) noexcept;
// The offsets are wanted now (the key, a row for the self body, the fit on): partner_gear decides whether it can write them.
bool HeadgearWanted() noexcept;
// partner_gear.cpp, every tick (game thread): true while it writes the offsets into every shown head item of ours (its
// site check passed, the mover writes on, no head item refused or unknown). The lift is 0 only while this was said true
// within the last 500 ms; otherwise the v0.11 lift holds the items up.
void SetHeadgearReady(bool ready) noexcept;
void ConfigureSelfHeadgear(bool on) noexcept;
// Self view feet (section 21; session.ini self_feet_hide, default on): her foot joints take a near-zero scale on the
// self body while the game draws Sam's equipped boots (the gear tick says so with SetSamBootsShown).
void ConfigureSelfFeet(bool on) noexcept;
void SetSamBootsShown(bool shown) noexcept;
// The partner body's feet (boots_sync): scaled to pct percent while `on` (the boots prop on it is live, shown and posed).
void SetPartnerFeetHidden(bool on, int pct) noexcept;
void FeetCounters(std::uint32_t* self_writes, std::uint32_t* partner_writes) noexcept;
// Joint `sam_joint` (as JointWorld) as a world frame: position and rotation rows (right, forward, up; normalised).
bool JointFrameWorld(game::Entity* e, unsigned sam_joint, double (&p)[3], float (&r)[3][3]) noexcept;
// Joint j of an entity whose skeleton has n joints (any layout: the boots' 10), position in world space (row 3 only).
bool JointWorldN(game::Entity* e, unsigned j, unsigned n, double (&pos)[3]) noexcept;
bool SelfConfigured() noexcept;
bool SelfShown() noexcept;           // the body is shown and Sam hidden right now
game::Entity* SelfEntity() noexcept;  // the self body (loading or live), nullptr when none
// The cargo menu's Sam figure (DSPlayerAKukanEntity) while the self body stands in for it (Manage Cargo open, the
// self view on; from the opening frames, before the body is shown), else 0. Game thread (self_voice.cpp mutes its voice).
std::uintptr_t MenuSelfFigure() noexcept;

// ---- gear props (docs/real-partner-body.md section 12; partner_gear.cpp owns them)
// Prop entities (backpack, cargo, Odradek, BB pod) placed on a body every frame: in the detour,
// right after the body's pose was written (the partner) or right after the self body got Sam's
// transform (the self view), from the body's own world transform and the model matrix of the
// anchor joint, so they move in the same frame as the body. Each prop is checked (vtable + GUID)
// before the game's Entity_ExportedSetWorldTransform is called on it.
struct GearProp {
    std::uintptr_t e = 0;       // the prop entity
    std::uintptr_t e_vt = 0;    // its vtable when validated
    game::Guid guid;            // its per-creation GUID
    std::uint8_t joint = 0;     // anchor: a joint of Sam's skeleton (mapped onto the body's own)
    std::int8_t parent = -1;    // or an earlier prop of the list (cargo on the backpack)
    float p[3] = {};            // position in the anchor's frame (m)
    float r[3][3] = {};         // rotation rows in the anchor's frame
};
inline constexpr unsigned kMaxGearProps = 56;  // wire::kMaxGearItems
enum class GearSlot : std::uint8_t { partner = 0, self = 1 };
// Game thread. n > 0 publishes the list (it replaces the previous one); n = 0 unpublishes it and
// waits until no detour uses it (call before removing a published prop).
void PublishGear(GearSlot slot, const GearProp* props, unsigned n) noexcept;
// Game thread: places the published props from the body's current world transform and pose now
// (the tick's pass, after the partner body was moved). Returns the props placed.
unsigned PlaceGearNow(GearSlot slot) noexcept;
// Detour placement counters (per slot): placements, ABA refusals, bad matrices.
void GearCounters(GearSlot slot, std::uint32_t* placed, std::uint32_t* aba, std::uint32_t* bad) noexcept;
// The body entity of a slot when live (partner slot: also a debug body), nullptr otherwise.
game::Entity* SlotEntity(GearSlot slot) noexcept;
// Starts loading any DSControlledEntity by path + GUID (the bodies' route, BeginSlot), for props.
// Game thread; the caller owns `out` and must end it with holo::SpawnTeardown.
bool BeginByPath(const char* path, const char* guid_text, const game::WorldTransform& at, std::uint32_t tag, holo::Spawn* out,
                 char* why, std::size_t cap) noexcept;
// The same with the raw GUID bytes (a gear entity resource named by the partner's STR1 table).
bool BeginByPathGuid(const char* path, const std::uint8_t (&guid)[16], const game::WorldTransform& at, std::uint32_t tag, holo::Spawn* out,
                     char* why, std::size_t cap) noexcept;
// Gear skinned poses (docs/real-partner-body.md section 13): target k (< kMaxPoseTargets) is a gear prop
// whose finished pose gets `pose`'s joints (rotations and translations; the prop's own scale kept) in
// its own model updates, then the game's model-matrix and bounds steps, as a body's. The prop is
// checked first (its SkinnedModel, a skeleton of pose->n joints with layout hash pose->skeleton); a
// prop the game stops updating is driven with the forced model update (only while its spawn still
// checks out). The same prop again only replaces the joints. sp = nullptr / pose = nullptr clears the
// target (waits until no detour uses it: call before the prop is removed). A pose write that faults
// turns gear poses off for the session (false from then on). Game thread.
inline constexpr unsigned kMaxPoseTargets = 9;
// Targets 0..7 are the gear's (partner_gear.cpp); target 8 is the partner's vehicle copy (vehicle_sync.cpp,
// docs/real-partner-body.md section 22).
inline constexpr unsigned kGearPoseTargets = 8;
inline constexpr unsigned kVehiclePoseTarget = 8;
bool SetGearPose(unsigned k, const holo::Spawn* sp, const wire::GearPose* pose) noexcept;
// The partner's boots prop (docs/real-partner-body.md section 21; boots_sync): a cutscene prop on the boots' own 10-joint
// skeleton (layout 0x9B68A8AD, parents checked), posed in its own model updates from the partner body's legs (the body's
// pose composed at that moment, identity root). The prop is placed as a gear item on the body's joint 0. sp = nullptr
// clears it (waits until no detour uses it: call before the prop is removed). False (why says): not taken. Game thread.
bool SetBootsProp(const holo::Spawn* sp, char* why, std::size_t cap) noexcept;
void ClearBootsProp() noexcept;
bool BootsPosed(double within_ms) noexcept;  // a pose was written onto it within that time
void BootsCounters(char* out, std::size_t cap) noexcept;
void ClearGearPose(unsigned k) noexcept;
void GearPoseCounters(char* out, std::size_t cap) noexcept;
// How many props of the slot the detour places now (0: unpublished, or a placement fault switched
// the list off until it is published again).
unsigned GearPublished(GearSlot slot) noexcept;
// An entity's resource identity: its StreamingRef (+0x58) path (no ".core") and raw GUID (the bytes
// BeginByPath takes), and the resource's type name when loaded. Fault-free reads; false when unreadable.
bool ResourceIdentity(game::Entity* e, char* path, std::size_t cap, std::uint8_t (&guid)[16], const char** type) noexcept;
// The model-space matrix (4 rows: right, forward, up axes, then the translation) of joint `j` of an
// entity's current pose (its SkinnedModel at +0xB8, checked; 77-joint layout). Fault-free reads.
bool ReadJointModel(game::Entity* e, unsigned j, float (&m)[4][4]) noexcept;
// The head joint (joint 31 of the 77-joint layout, through the body's joint map for a live partner
// or self body; as is for the local Sam) in world space. False when unreadable or implausible
// (not 0.3-2.4 m over the root, or more than 1 m beside it). Name tags anchor on it.
bool HeadWorld(game::Entity* e, double (&pos)[3]) noexcept;
// Joint `sam_joint` of the 77-joint layout (through the body's joint map for a live partner or self body; as is for the
// local Sam) in world space. False when unreadable, not finite or more than 3 m from the entity's root. The partner
// sounds (partner_sound.cpp) place their emitters on it.
bool JointWorld(game::Entity* e, unsigned sam_joint, double (&pos)[3]) noexcept;


// The per-frame sync faulted (dllmain GuardedTick) and Tick will not run again this session: the
// detour stops all work (both slots unpublished, no capture), our hide reason is cleared on Sam and
// a shown self body hidden, so a dead tick never leaves the player invisible. Logged.
void EmergencyStop() noexcept;

// ---- smooth animation above 60 fps (docs/real-partner-body.md section 3.5; session.ini smooth_pose)
// kSmoothBoost: our bodies and gear props are moved to the game's 240 Hz update group (Entity
//   SetUpdateFrequency, byte-checked; the game's own cutscene bodies and props ask for 59.94 Hz), so
//   their models are updated and posed every frame up to 240 fps instead of every other frame.
// kSmoothDetourInterp: the partner body's pose is interpolated in its own model update at that
//   moment's play time (the tick publishes the bracketing frames and a QPC reference).
// kSmoothCaptureStamp: our STJ1 frames carry the time Sam's pose was captured, not the send tick's.
inline constexpr unsigned kSmoothBoost = 1, kSmoothDetourInterp = 2, kSmoothCaptureStamp = 4, kSmoothAll = 7;
// `echo`: the partner replays our own Sam (role echo), so the [smooth] meter can compare it with
// Sam's captured pose at the same play time.
void ConfigureSmooth(unsigned mask, bool echo, double echo_delay_ms) noexcept;
unsigned SmoothMask() noexcept;

// ---- debug actions (game_actions.cpp): op 1 probe, 2 spawn <body id> <dist m> <mirror 0/1>,
// 3 remove, 4 joint names, 5 stats, 6 mirror <0/1>, 7 control item <n>,
// 8 self view <0/1> <body id, 0 = configured> <follow: 1 Sam's update, 2 the body's, 3 tick, 0 unchanged>
//   (8 with 2 <reason 0..63> <0/1>: hidden tests on scratch saves only, sets / clears a hide reason
//   on the local Sam's mask, e.g. 26 = photo mode's, 63 = a stray bit of ours),
// 9 outfit <0 log / 1 like our Sam / 2 exos colour / 3 override off / 4 own suit colour (hidden tests
//   on scratch saves only)>, 10 smooth <0 log / 1 mask / 2 frame cap fps / 3 record n frames>.
bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::realbody
