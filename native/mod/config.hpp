// Session configuration: <game>\stranded_together\session.ini, written by the
// launcher (or by hand for testing). Missing file = mod idle (game runs normally).
// With one_shot=1 (what the launcher writes) the mod renames the file to
// session.last.ini as soon as it has read it, so the next plain Steam launch of
// the game runs unmodded.
#pragma once
#include <cstdint>
#include <string>

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
    bool pose_hook = false;            // install the pose hook even without partner_body=real (debug spawns)

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
