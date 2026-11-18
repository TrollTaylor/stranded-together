// Partner voice (docs/audio-research.md section 15, Phase 2; session.ini voice_sync=1): each player hears the
// other's voice, played by the game itself from the player's own install.
//
//   Sender (game thread, Tick): the talk handler's hook (self_voice.cpp FilterTalk, job threads) copies every
//   talk addressed to the local player (Sam's spoken lines and his grunts, efforts and breathing: one path,
//   section 12.1) into a lock-free ring; the tick drains it and sends one STS1 frame per talk on the link's
//   reliable channel: kind 0 with the sentences' GUIDs while this player's character is Sam, kind 1 (no
//   GUIDs) otherwise, so a player who is not Sam never sends Sam's lines (a Fragile guest's partner hears
//   nothing of Sam from her).
//   Receiver (game thread, OnSpeech): a kind 0 frame's GUIDs are resolved among the SentenceResources of the
//   local player's resident situation group ([player + 0x4500 + 0x28], DSSituationSentenceGroupResource,
//   RTTI-checked; built once per group, fault-free reads) and the first one is played with the game's
//   DSTalkComponent::PlaySentence (0x1422D3FE0, vtable +0x68, byte-checked, SEH-guarded) on the DSTalkComponent
//   of the partner's talking entity: the invisible echo entity player_sync spawns next to the partner body
//   (partner_voice / voice_sync) and keeps at the body's head. Skipped (counted, logged) while the local game
//   is paused, in a menu, a cutscene or loading (DSGameState stopping events, GameModule load mask), while
//   the local player is asleep, beyond kMaxDistanceM, with no talking entity, or when a GUID does not resolve.
//   Subtitles (voice_subtitles=0, the default): the subtitle method that builds MsgShowSubtitle (0x14361A6C0,
//   byte-checked; any thread) drops a subtitle whose text is the Text of a line the partner voice played in
//   the last kSubtitleWindowMs (one-shot per line; lock-free table). With voice_subtitles=1 it only counts.
//
//   Our own PlaySentence calls never come back as sent lines: they send MsgStartTalk (not MsgDSStartTalk) to the
//   echo entity, whose owner is never the local player.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "game_api.hpp"
#include "pose_wire.hpp"

namespace st::pvoice {

inline constexpr double kMaxDistanceM = 100.0;        // farther: the line is not played (doc section 8 "Distance")
inline constexpr double kHeadFallbackM = 1.55;         // emitter height over the root when the body has no readable head
inline constexpr double kSubtitleWindowMs = 2000.0;    // a played partner line's subtitle is dropped when it comes this soon (0-31 ms seen)
inline constexpr double kSelfSubtitleWindowMs = 5000.0; // the local player's own muted line (self_voice silencing): its subtitle

// Bootstrap thread, after MH_Initialize (before the frame pump): remembers the settings; with voice_sync=1
// installs the subtitle hook (byte-checked; logged either way).
void Install(const Config& cfg) noexcept;
bool Enabled() noexcept;  // voice_sync=1

struct Frame {
    game::Entity* local = nullptr;     // the local player (or null)
    bool local_ok = false;             // active and valid this tick
    game::Entity* emitter = nullptr;   // the partner's talking entity (the voice echo), or null
    game::Entity* body = nullptr;      // the partner body the emitter follows (for the head-tracking log), or null
    bool net = false;                  // a link (host / guest); false: role echo (our own lines come back to us)
};
// Game thread, every tick: sends the captured talks, logs the emitter tracking and the counters.
void Tick(const Frame& f, double now_ms) noexcept;
// Game thread: a received STS1 (role echo: our own, through encode / decode).
void OnSpeech(const wire::Speech& s, double now_ms) noexcept;
// Game thread: a new link session (the duplicate filter starts again).
void NewSession() noexcept;
// Where the talking entity goes: the partner body's head joint (realbody::HeadWorld) when it reads, else
// kHeadFallbackM over `root`; the rotation is root's. Returns true when the head joint was used.
bool EmitterTransform(game::Entity* body, const game::WorldTransform& root, game::WorldTransform* out) noexcept;

// Any thread (the subtitle hook): true = drop this subtitle (its text is a partner line's, voice_subtitles=0).
bool FilterSubtitle(const void* text) noexcept;
// Any thread (the talk handler's job threads for self): drop the next subtitle request with this LocalizedTextResource once.
// self = the local player's own muted line (always dropped); else a partner line (dropped unless voice_subtitles=1).
void ArmSubtitleText(std::uintptr_t text, bool self) noexcept;

// Game thread: nullptr while the local game is being played; otherwise why not (no or sleeping local player,
// loading, paused, a menu or a cutscene: the DSGameState stopping events). The partner sounds share it.
const char* LocalGate(game::Entity* local) noexcept;


// Debug command (game thread; `action voice_sync <op> [a]`, docs/debug-actions.md "Voice sync"):
//   0 log the counters, 1 <n> play sentence n of the resident group on the talking entity (as a received line; the
//   action succeeds when the line was tried: a refusal by the receiver's checks is logged and in the result text),
//   2 <0|1> test only: send Sam's lines even when this player is not Sam (1) / as normal (0),
//   3 rebuild the GUID map and log it.
bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::pvoice
