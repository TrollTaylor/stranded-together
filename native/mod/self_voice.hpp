// Self voice (docs/audio-research.md section 9, Phase 1): mutes Sam's voice on THIS player's own game
// when they play another character (session.ini self_voice=auto|sam|mute, self_voice_setting.hpp).
//
//   Where: DSTalkComponent's MsgDSStartTalk handler 0x1422D4F70 (reached only through the handler-table
//   thunk 0x1422D6AA0). Every line Sam says goes through it, the spoken lines and the non-verbal ones
//   (grunts, efforts, breathing, humming: the animation voice events end in the same voice queue; Phase 0,
//   section 12.1). A talk is muted when (2026-11-18: with a PostEvent detour in, the talk RUNS and only its voice post
//   is silenced in Wwise, PrePost below; without one, the message is dropped and the original is not run)
//     - the setting resolves to mute,
//     - the talk component's owner [comp+0x48] (fault-free read) is exactly the local player entity
//       (not BB, the cuff device or any other child: they have DSTalkComponents of their own), or the cargo
//       menu's figure (DSPlayerAKukanEntity) while the self view shows the chosen character in its place,
//     - and msg+0x40 == 0 (a start; the other branch is passed through).
//   Everything else (NPCs, BB, the partner's voice entity, radio calls, footsteps, foley, weapons) runs the
//   game's code unchanged. The message stays the sender's: the handler only copies from it (refcounted), so
//   skipping it leaks nothing.
//   Threads: the handler runs on the game's job threads. The detour reads two atomics the game thread
//   publishes (local player, menu figure), does at most two ReadProcessMemory reads, bumps a counter and
//   returns: no allocation, no lock, no logging. The tick logs the counts.
//
// With audio_probe=1 the probe owns the hook on that handler and asks FilterTalk first (one hook per
// target); otherwise this module installs its own. Nothing is hooked when the setting resolves to sam and
// the partner voice (voice_sync, docs/audio-research.md section 15) is off. With voice_sync=1 the same hook
// also copies the local player's own talks for STS1 (FilterTalk), muted or not.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "game_api.hpp"
#include "self_voice_setting.hpp"

namespace st::selfvoice {

// Bootstrap thread, after MH_Initialize (and after audioprobe::Install). is_host: role=host;
// my_character: session.ini (bodies::ChooseOwn decides the character). capture: the partner voice
// (voice_sync=1, partner_voice.hpp) wants the local player's talks copied. Installs the hook only when the
// setting resolves to mute or capture is asked. Logged either way.
bool Install(Mode mode, bool is_host, const std::string& my_character, bool capture) noexcept;
bool Armed() noexcept;      // a talk hook (ours or the probe's) asks FilterTalk and the setting resolved to mute
bool Capturing() noexcept;  // a talk hook asks FilterTalk and copies the local player's talks
const char* Character() noexcept;  // this player's character key as Install resolved it ("sam" before Install)

// Any thread (the talk handler's detour, before the original). True = drop this talk (counted). With the
// capture on, a talk addressed to the local player itself (msg+0x40 == 0) is also copied into a lock-free
// ring (its sentences' pointers and GUIDs, fault-free reads), muted or not.
bool FilterTalk(void* comp, void* msg) noexcept;

// Silencing instead of dropping (2026-11-18, docs/audio-research.md section 17): with a PostEvent hook in (partner_sound's,
// also installed for this alone, or the audio probe's), a muted talk of the local player runs, and the external-source
// post that plays its voice is silenced first (SetGameObjectOutputBusVolume(object, listener 0, 0)), so what the game
// chains to the line (BB's answer to a dialogue line) still happens. The cargo menu figure's talks are still dropped.
// Any thread, inside the PostEvent detour, before the original. Cheap unless the post has external sources.
void PrePost(std::uint32_t ev, std::uint64_t obj, std::uint32_t n_ext) noexcept;
void NoteUnregister(std::uint64_t obj) noexcept;  // any thread: the UnregisterGameObj detour (before the original)
// Bootstrap thread: Install resolved mute and the volume export checked (partner_sound then installs a PostEvent
// hook for this even with sound_sync=0); PostHookReady(true) once a hook calls PrePost, else the talk is dropped.
bool WantsPostHook() noexcept;
void PostHookReady(bool ready) noexcept;
// Game thread: the partner's talking entity (its voice posts are never taken for the local player's).
void NoteEcho(game::Entity* echo) noexcept;

// One captured talk of the local player (the game thread drains them with PopTalk, oldest first).
inline constexpr unsigned kCaptureMax = 4;
struct TalkCapture {
    std::uint8_t n = 0;          // sentences (1..kCaptureMax)
    bool muted = false;          // self_voice dropped it on this game
    bool truncated = false;      // the message had more entries than kCaptureMax
    std::uint64_t qpc = 0;       // when the handler saw it
    float delay = 0;             // the first entry's delay (entry +0x10)
    std::uintptr_t sentence[kCaptureMax] = {};  // SentenceResource* (refcounted by the message; for logs only)
    std::uint8_t guid[kCaptureMax][16] = {};    // RTTIRefObject + 0x08
};
bool PopTalk(TalkCapture* out) noexcept;
// Talks whose message could not be read, and captures overwritten before they were drained.
void CaptureCounters(std::uint32_t* unreadable, std::uint32_t* dropped) noexcept;

// Game thread, every tick: publishes the local player and the menu figure, logs the counts (once a second
// when they changed).
void Tick(game::Entity* local, double now_ms) noexcept;

// Debug command (game thread; `action self_voice <op> [a]`): 0 log the counts, 1 <0/1> mute off / on
// (only when armed), 2 <n> the cargo menu's figure says its line list n (the game's PlayAkukanVoice; the menu
// must be open), 3 <n> test situation n through the local player's voice queue (audioprobe::TestSituation).
bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::selfvoice
