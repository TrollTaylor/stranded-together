// Partner sounds (docs/audio-research.md section 16, Phase 3; session.ini sound_sync=1): each player hears the other's
// footsteps, foley, cargo and weapon sounds, played by the game's own Wwise from the player's own install (the same
// banks on both games: only event ids, RTPC values and joint offsets cross the link; no audio data).
//
//   Sender (hot paths: any thread; the rest on the game thread). The exported Wwise API is hooked (PostEvent,
//   RegisterGameObj, UnregisterGameObj, SetRTPCValue, StopPlayingID, ExecuteActionOnEvent; byte-checked, by export
//   name and RVA), plus the three SoundComponent play paths and the material emitter start as "who registers this
//   game object" contexts (section 12.5). With audio_probe=1 the probe owns those hooks and forwards to the Hot*
//   functions below (one MinHook hook per target). The hot paths only copy a few words (and, for a post, the game
//   object's position, read with ReadProcessMemory) into a lock-free ring: no allocation, lock or logging.
//   The game thread drains the ring in order and decides per registration whose game object it is: a registration
//   made inside a component's play path belongs to that component's owner entity ([[rep+0x20]+0x48]); only an
//   allow-list is "ours" (2026-11-18): the player's own DSCustomSoundComponent (animation foley) and the sound
//   components of a weapon or cargo entity under him; everything else (talk / voice / echo components, the camera, the
//   Odradek, gear, the BB pod, the cuff device, other player components) stays local, and any post with external
//   sources is voice and never mirrored. Footsteps and
//   the movement layer come from material emitters with no owner: with sound_steps=1 such a game object is ours when
//   its post lies within kOwnRootM of the player's root or kOwnFootM of a foot joint (experimental).
//   Each post on our game objects becomes an STA1 item (event id, the playing id the game got, the RTPCs set on the
//   object, the nearest of seven joints and the offset from it, the time); stops of those playing ids and RTPC changes
//   on them follow. The items ride in the next pose datagram (wire::SoundFrame, unreliable).
//   Our own posts (the receiver's) are on game objects of this module and are never sent (counted as "own").
//   Nothing is sent while the local game is paused, in a menu, a cutscene or loading.
//
//   Receiver (game thread). Items are scheduled at the pose play time of the sender's moment (the ring's PlayTime,
//   the same timeline the partner body's pose plays at) and then posted on a private Wwise game object (a pool of
//   kPool objects whose ids are addresses inside this DLL) bound to the sender's game object (tag), placed every
//   frame at the partner body's matching joint plus the offset (Decima Z-up -> Wwise Y-up), after replaying its
//   RTPCs. PostEvent asks for AK_EndOfEvent; the callback (Wwise's thread) only pushes a record into a ring.
//   Only event ids on the receiver's allow-list are posted (the ids the Phase 3 runs mirrored, plus every id this game
//   posted on its own player's allowed objects this session; never the voice presets, the camera's or the global ones).
//   Stops and actions are never dropped for timing; nothing we post plays longer than 15 minutes; partner_volume sets
//   our objects' output bus volume.
//   Not posted (counted, logged): while the local game is paused, in a menu, a cutscene or loading (the partner
//   voice's gate; our playing sounds are stopped when that starts), with no partner body, when the partner is
//   beyond kMaxDistanceM, when an item is more than 1.5 s late, or over the per-slot rate limit.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"
#include "pose_wire.hpp"

namespace st::psound {

inline constexpr double kMaxDistanceM = 100.0;  // farther: the partner's sounds are not posted
inline constexpr double kOwnRootM = 1.0, kOwnFootM = 0.5;  // the footstep position rule (sound_steps=1)

// Bootstrap thread, after MH_Initialize and audioprobe::Install (before the frame pump). With sound_sync=1: resolves
// and byte-checks the Wwise exports it calls, then installs its hooks (or, with the audio probe installed, checks that
// the probe hooked the same targets). Logged either way.
void Install(const Config& cfg) noexcept;
bool Enabled() noexcept;    // sound_sync=1 and ready
bool Capturing() noexcept;  // any thread: the hot paths record (cheap)
// A PostEvent detour (ours, or the audio probe's) calls selfvoice::PrePost. With sound_sync=0 and self_voice silencing
// wanted (selfvoice::WantsPostHook), Install hooks only PostEvent for it.
bool PostHooked() noexcept;

// ---- hot paths (any thread; called by this module's hooks or the audio probe's)
void HotPost(std::uint32_t ev, std::uint64_t obj, std::uint32_t n_ext, std::uint32_t pid) noexcept;
void HotRegister(std::uint64_t obj, std::uintptr_t ctx, bool emitter) noexcept;
void HotUnregister(std::uint64_t obj) noexcept;
void HotRtpc(std::uint32_t id, float value, std::uint64_t obj, std::int32_t ms) noexcept;
void HotStop(std::uint32_t pid, std::int32_t ms) noexcept;
void HotAction(std::uint32_t ev, std::int32_t action, std::uint64_t obj, std::int32_t ms, std::uint32_t pid) noexcept;

// ---- game thread
struct Frame {
    game::Entity* local = nullptr;   // the local player (or null)
    bool local_ok = false;           // active and valid this tick
    game::Entity* body = nullptr;    // the partner body (real body, hologram, ...) or null
    bool have_root = false;          // the partner's interpolated transform this tick (root)
    game::WorldTransform root{};
    bool have_play = false;          // the pose ring plays: play_ms is the sender-clock time shown now
    double play_ms = 0;
    bool net = false;                // a link (host / guest); false: role echo
};
// Every tick, after the partner body was placed: drains the hot ring (sender), posts the partner's due items, keeps
// our game objects on the body's joints, drains the end-of-event callbacks, logs the counters.
void Tick(const Frame& f, double now_ms) noexcept;
// At the pose send (game thread): our pending items as one STA1 frame. False when there is nothing to send.
bool TakeSend(game::Entity* local, double now_ms, wire::SoundFrame* out) noexcept;
// A received (or, role echo, our own encoded / decoded) STA1 frame. `sender_axis_ms` = its sender_ms on the pose
// ring's unwrapped axis (Ring::Unwrapped), the axis PlayTime is on.
void OnFrame(const wire::SoundFrame& f, double sender_axis_ms, double now_ms) noexcept;
// A new link session: the partner's sounds stop, the bindings and duplicate state start again.
void NewSession() noexcept;

// Debug command (game thread; `action sound_sync <op> [a]`, docs/debug-actions.md "Sound sync"):
//   0 log the counters (both sides, per category, per owner label), 1 <0|1> log every mirrored post on both sides,
//   2 stop every partner sound we play now.
bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::psound
