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
    std::string player_name;           // shown in logs; from Steam persona when empty

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
const char* RoleName(Role r);
const char* PartnerBodyName(PartnerBody b);

}  // namespace st
