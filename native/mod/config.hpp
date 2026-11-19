// Session configuration: <game>\stranded_together\session.ini, written by the
// launcher (or by hand for testing). Missing file = mod idle (game runs normally).
// With one_shot=1 (what the launcher writes) the mod renames the file to
// session.last.ini as soon as it has read it, so the next plain Steam launch of
// the game runs unmodded.
#pragma once
#include <cstdint>
#include <string>

#include "self_voice_setting.hpp"

namespace st {

// solo: debug tools only (in-game menu, logging), no partner and no network.
enum class Role { none, host, guest, echo, solo };
enum class Transport { steam, udp };
// What the partner looks like (docs/partner-hologram.md, docs/real-partner-body.md).
// Each body falls back to the next one when it cannot be found or spawned:
// real -> holo -> marker -> echo. `real` is a solid character body that plays the
// other player's real animation (experimental; not the default yet).
enum class PartnerBody { holo, marker, echo, real };

struct Config {
    bool present = false;          // session.ini existed and parsed
    Role role = Role::none;
    Transport transport = Transport::steam;

    // Steam transport
    std::uint64_t peer_steam_id = 0;   // guest: host's SteamID64 (0 = wait for invite)
    std::string secret;                // shared join secret (from the join code)

    // UDP transport (LAN / same-PC testing without Steam networking)
    std::string udp_peer = "127.0.0.1";
    std::uint16_t udp_port_local = 27555;
    std::uint16_t udp_port_peer = 27556;

    // Sync tuning
    std::uint32_t send_hz = 30;
    std::uint32_t interp_delay_ms = 100;
    float echo_delay_s = 2.0f;         // echo role: replay own movement this many seconds behind
    float echo_offset_m = 0.0f;        // echo role: extra sideways offset

    // Partner
    bool partner_activate = true;      // wake the spawned clone (gated: never touches the real Sam); 0 = leave it asleep
    PartnerBody partner_body = PartnerBody::holo;  // partner_body=holo|marker|echo
    std::uint32_t partner_holo = 0;    // collectors item HashId to show (hex or decimal); 0 = automatic
    int partner_marker = -1;           // trace-point prop index for the marker body; -1 = first usable
    bool partner_voice = false;        // also spawn the (invisible) echo next to a holo/marker body as its voice source
    // partner_body=real, debug override: show this body_table key (sam, porter, fragile, ...) for the
    // partner instead of the one the peer sends (STJ1 body_id). "" = follow the peer (the normal case).
    std::string partner_character;
    // The character THIS player appears as on the other player's screen (body_table key), sent as
    // STJ1 body_id. "" / unknown = sam; the host always sends sam (docs/real-partner-body.md).
    std::string my_character;
    // Self view: on THIS player's own screen the local Sam is replaced by my_character too (a
    // guest whose character is not sam; the host is always Sam). Written by the launcher
    // (self_character=1, Debug option, default on); off when absent. Needs the pose hook
    // (partner_body=real or pose_hook=1). docs/real-partner-body.md section 8.
    bool self_character = false;
    // Self view fit: while the self body is shown, Sam's hidden skeleton takes the character's bone
    // lengths, so the gear, cargo and boots the game hangs on it sit on the character
    // (docs/real-partner-body.md section 16). Default on; self_fit=0 keeps Sam's proportions.
    bool self_fit = true;
    // Self view headgear (docs/real-partner-body.md section 20): with the fit on, Sam's cap and glasses get an offset of
    // their own in his head helper's frame (the accessory's own mover), made for the character's head, instead of the
    // head lift. Default on; self_headgear=0 keeps the v0.11 head lift.
    bool self_headgear = true;
    // Self view feet (section 21): her own feet are scaled to nothing under Sam's equipped boots. Default on;
    // self_feet_hide=0 keeps her own boots under his.
    bool self_feet_hide = true;
    // Partner boots (section 21): the sender's equipped boots as a gear item (STC1 kind 7) and, on the other screen, a
    // cutscene boots prop posed from the partner body's legs with the partner's own feet scaled away. Default on;
    // boots_sync=0: nothing read, sent or spawned (the partner shows its own model's boots).
    bool boots_sync = true;
    // Vehicles (docs/real-partner-body.md section 22): the vehicle the player rides is sent (STM1) and shown under the
    // partner's body on the other screen as a visual copy (a cutscene trike / truck, the roadster's hologram). 1 = on,
    // all three copies (default; each proven against real drives), 3 = the same (the v0.12 value for all three), 2 =
    // solid copies only (trike, truck; no hologram roadster), 4 = the roadster's copy only (the v0.12 default), 0 =
    // nothing read, sent or spawned. Every value but 0 sends what we ride.
    int vehicle_sync = 1;
    // The partner's vehicle copy hidden while the watcher's own vehicle of the same kind stands in the same place (two of
    // the same vehicle in one spot; docs/real-partner-body.md section 22). Default on; 0: the copy is always shown.
    bool vehicle_overlap_hide = true;
    // The partner's vehicle copy wears the partner vehicle's paint and variations (gold / silver / broken / ride type;
    // the solid trike and truck copies only) and shows its battery / defense packs (docs/real-partner-body.md section
    // 22.7). Default on; 0: neither is read, sent or applied (the copies' own look, no packs).
    bool vehicle_look = true;
    bool vehicle_packs = true;
    bool pose_hook = false;           // install the pose hook even without partner_body=real (debug spawns)

    // Debug
    bool verbose = false;
    bool debug_menu = true;            // in-game ImGui debug menu (D3D12 overlay)
    std::uint32_t debug_menu_vk = 0x78;  // virtual-key that toggles it (default VK_F9)
    bool one_shot = false;             // consumed on load (renamed to session.last.ini)
    // name=<text>: the name shown above this player's head on the partner's screen (name tags);
    // "" = the Steam persona name. Sent over the link (STN1), never logged in full.
    std::string player_name;
    // name_tags=0|1 (default 1): show the partner's name above their head (overlay) and send our
    // own (STN1). 0: neither (the partner then shows "Host" / "Guest" for us).
    bool name_tags = true;
    // partner_collision=0|1 (default 1): the local player cannot walk through the partner's real body
    // (the game's own character capsule on that body; docs/real-partner-body.md section 11).
    // partner_collision_layer=10|9|25 (default 10 "Humanoid blocker"; 9 = the game's "AI or Remote
    // Player" layer, which bullets and cargo hit too; 25 "vs Humanoids" did not block reliably). Test setting.
    // partner_collision_height=<m> (default 4.0; 0 = Sam's own 1.65 m): the capsule's standing height. Taller
    // than Sam can climb or reach with a jump-grab (2.6 m was still grabbed), so the partner cannot be climbed
    // (section 11.5).
    bool partner_collision = true;
    int partner_collision_layer = 10;
    float partner_collision_height = 4.0f;
    // name_tag_font=game|default (default game): name tags use the game's own menu font ("EX PS Medium
    // Neon", the "Map/Information" header), read at run time from the installed game's archives into
    // memory only (never written or shipped); default = the overlay's Segoe UI.
    bool name_tag_game_font = true;
    // partner_gear=0|1 (default 1): the partner's real body (and the self view's body) carries the
    // player's backpack, Odradek, BB pod and cargo (docs/real-partner-body.md section 12), and our
    // own gear layout is sent (STC1). 0: nothing is read, sent or spawned.
    bool partner_gear = true;
    // smooth_pose=0|1 (default 1; docs/real-partner-body.md section 3.5): the partner's and the self
    // view's bodies (and their gear props) get a fresh pose every presented frame above 60 fps: 240 Hz
    // model updates, interpolation inside the body's own update, capture-time stamps on STJ1. 0 = the
    // v0.7 behaviour (bodies updated at the game's 59.94 Hz for cutscene actors).
    bool smooth_pose = true;
    // audio_probe=0|1 (default 0; docs/audio-research.md section 9, Phase 0): logging-only probes on Sam's voice path
    // (voice queue, talk handler, animation sound events) and on the exported Wwise API (PostEvent, RegisterGameObj,
    // SetSwitch, SetRTPCValue, SetPosition, StopPlayingID per game object). Changes nothing the game does.
    bool audio_probe = false;
    // self_voice=auto|sam|mute (docs/audio-research.md section 9, Phase 1; self_voice.cpp): Sam's own voice (spoken
    // lines, grunts, efforts, breathing) on THIS player's game. auto = muted when this player's character
    // (my_character; the host is always sam) is not Sam, and the cargo menu's figure while it shows that character;
    // sam = as the game plays it; mute = always muted. Absent (or not understood) = sam: nothing is hooked. The
    // launcher writes auto (Debug option, default on) or sam.
    selfvoice::Mode self_voice = selfvoice::Mode::sam;
    // voice_sync=0|1 (default 0; docs/audio-research.md section 15, Phase 2; partner_voice.cpp): each player hears
    // the other's voice. The local player's talks (Sam's lines, grunts, efforts, breathing) go out as STS1 on the
    // link's reliable channel (a player who is not Sam sends only "muted" frames: never Sam's lines), and the
    // partner's are played with the game's DSTalkComponent::PlaySentence on an invisible talking entity (the echo
    // resource, as partner_voice=1 spawns it) kept at the partner body's head. 0: nothing is hooked, sent or played
    // for it (self_voice still mutes).
    bool voice_sync = false;
    // voice_subtitles=0|1 (default 0): with voice_sync, the game may show the partner's lines as subtitles; 0 drops
    // the subtitle of a line the partner voice just played (the subtitle hook, byte-checked), 1 leaves them.
    bool voice_subtitles = false;
    // sound_sync=0|1 (default 0; docs/audio-research.md section 16, Phase 3; partner_sound.cpp): each player hears the
    // other's footsteps, foley, cargo and weapon sounds. The Wwise events posted on the local player's own game objects
    // (the SoundComponents of the player and his child entities: animation foley, weapons, gear, cargo) go out as STA1 in
    // the pose datagram, with their RTPC values; the partner's are posted on game objects of our own kept at the partner
    // body's joints, at the pose play time. 0: nothing is hooked, sent or posted for it.
    bool sound_sync = false;
    // sound_steps=0|1 (default 1; with sound_sync): footsteps and the movement layer, which the game plays from material
    // effect emitters with no owner, are taken as the local player's by position (within 1 m of his root or 0.5 m of a
    // foot; experimental: another walker that close is mistaken for him). 0: only the sounds tied to the player exactly.
    bool sound_steps = true;
    // partner_volume=0..100 (default 100; with sound_sync; Phase 4): the partner's footsteps, foley and weapon sounds at this
    // percent of their normal volume (SetGameObjectOutputBusVolume on the receiver's own Wwise objects); 0 posts nothing.
    // The partner's voice is not affected. Out of range: clamped.
    int partner_volume = 100;
    // vehicle_sound=0|1 (default 1; with sound_sync and vehicle_sync; docs/real-partner-body.md section 22.9): the engine,
    // road noise and other sounds of the vehicle the partner rides (its own SoundComponent's Wwise events, with their
    // RTPCs: rpm, speed, throttle, boost...) are posted at the partner's vehicle copy, at partner_volume. 0: only the
    // player's own sounds are sent and posted (the v0.12 behaviour).
    bool vehicle_sound = true;


    // Local two-instance test (docs/sandbox-and-local-test.md): local_input=host|host_pad|guest.
    // Read here for the overlay's input split; the sandbox keys (sandbox_saves, sandbox_foreign_saves,
    // appdir_settings, second_instance) are read by automation.cpp in DllMain.
    std::string local_input;           // "" = a normal game
    std::string sandbox_saves;         // the test-save sandbox this game runs on ("" = the real saves)
    unsigned frame_cap = 0;            // frame_cap=<15..240>: at most this many frames per second (0 = off)
    std::string local_window;          // local_window=left|right: the game window takes that half of the monitor
};

// Loads <dir>\session.ini. Never throws; unknown keys are logged and ignored.
Config LoadConfig(const std::wstring& dir);
// dllmain.cpp: the frame cap at run time (session.ini frame_cap sets it at start; the `smooth` test
// action changes it). 0 = off, else clamped to 15..240.
void SetRuntimeFrameCap(unsigned fps) noexcept;
unsigned RuntimeFrameCap() noexcept;
const char* RoleName(Role r);
const char* PartnerBodyName(PartnerBody b);

}  // namespace st
