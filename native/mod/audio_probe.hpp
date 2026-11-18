// Audio probe (docs/audio-research.md section 9, Phase 0): logging only, session.ini audio_probe=1.
//
//   Hooks (installed at start-up, before the frame pump; each byte-checked, a mismatch skips that hook):
//     - the exported Wwise API, found by export name and checked against the known RVA and prologue:
//       PostEvent (by id), RegisterGameObj, UnregisterGameObj, SetSwitch, SetRTPCValue, SetPosition,
//       StopPlayingID;
//     - Sam's voice path: the voice-queue enqueue 0x1426A6570, DSTalkComponent's MsgDSStartTalk
//       handler 0x1422D4F70, DSTalkComponent::PlaySentence 0x1422D3FE0 and
//       DSCustomSoundComponent::OnAnimationSoundEvent 0x142D83630 (the local player's only);
//     - who owns a Wwise game object (a sound instance): the instance create 0x141DB61A0 and attach
//       0x141DCBC30, with a thread-local "current component" set by the SoundComponent play paths
//       (0x14235A460, 0x14235AAC0, 0x1423493E0), the talk / animation handlers, a component instance init
//       (0x142D84940), the material effect emitter start (0x141AB3CA0) and the player update 0x142730B30;
//       ExecuteActionOnEvent for stops.
//   The hooks call the game's function and copy a few words into a lock-free ring (the voice-path
//   hooks also a few stack words that are return addresses into ds.exe); no allocation, no lock,
//   no logging. Fault-free reads only (ReadProcessMemory) for anything that is not our own stack.
//   The game thread (Tick) drains the rings, counts per game object and per event, classifies each
//   game object's owner (the local player, one of his components or child entities, or something
//   else) and writes the log. Nothing the game does is changed, except the test action that asks
//   the voice queue for a situation (`audio_probe 2 <n>`, the game's own PlayPlayerVoice path).
#pragma once
#include <cstddef>
#include <cstdint>

#include "game_api.hpp"

namespace st::audioprobe {

// Bootstrap thread, after MH_Initialize and before the frame pump hook. Logs every hook's result.
bool Install() noexcept;
bool Installed() noexcept;
// The MsgDSStartTalk handler is hooked by the probe; its detour then asks selfvoice::FilterTalk first.
bool TalkHooked() noexcept;
// DSTalkComponent::PlaySentence is hooked by the probe (its first bytes are the hook's jump; the probe checked them).
bool SentenceHooked() noexcept;
// True when the probe hooked the function at this RVA (ds.exe, base 0x140000000). The partner sounds (partner_sound.cpp,
// sound_sync=1) use the probe's Wwise and play-path hooks when it is installed: its detours forward to psound::Hot*.
bool Hooked(std::uintptr_t rva) noexcept;

// The partner voice's talking entity (0: none), so the voice post lines say how far each voice instance is from it.
void NoteVoiceEmitter(std::uintptr_t entity) noexcept;
// Game thread, also without the probe installed: asks the local player's voice queue for test situation n (the list of
// the probe's op 2) through the game's PlayPlayerVoice path (byte-checked, SEH-guarded). Used by selfvoice's op 3.
bool TestSituation(game::Entity* local, int n, char* msg, std::size_t cap) noexcept;

// Game thread, every tick (player_sync Tick). local = the local player entity (or null).
void Tick(game::Entity* local, double now_ms) noexcept;

// Debug command (game thread; `action audio_probe <op> [a] [b]`):
//   0 summary to the log, 1 full dump (events, objects, animation indices, talk owners),
//   2 <n> ask the local player's voice queue for test situation n (the game's PlayPlayerVoice path),
//   3 the local player's components to the log, 4 reset the counters.
bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

// Pure (tested): the situation hash the game uses (0x14190D040: CRC32-C, initial 0, no final xor,
// masked to 31 bits).
std::uint32_t SituationHash(const char* name) noexcept;

}  // namespace st::audioprobe
