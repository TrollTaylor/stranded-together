#include "config.hpp"

#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "log.hpp"

namespace st {
namespace {

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool ParseBool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

// "F1".."F12" or a decimal/0x virtual-key code; 0 on anything else.
std::uint32_t ParseKey(const std::string& v) {
    if ((v.size() == 2 || v.size() == 3) && (v[0] == 'F' || v[0] == 'f')) {
        const int n = std::atoi(v.c_str() + 1);
        if (n >= 1 && n <= 12) return 0x70 + static_cast<std::uint32_t>(n - 1);
        return 0;
    }
    const unsigned long k = std::strtoul(v.c_str(), nullptr, 0);
    return k > 0 && k < 0xFF ? static_cast<std::uint32_t>(k) : 0;
}

}  // namespace

const char* RoleName(Role r) {
    switch (r) {
        case Role::host: return "host";
        case Role::guest: return "guest";
        case Role::echo: return "echo";
        case Role::solo: return "solo";
        default: return "none";
    }
}

const char* PartnerBodyName(PartnerBody b) {
    switch (b) {
        case PartnerBody::marker: return "marker";
        case PartnerBody::echo: return "echo";
        default: return "holo";
    }
}

Config LoadConfig(const std::wstring& dir) {
    Config c;
    std::ifstream in(dir + L"\\session.ini");
    if (!in) {
        Log("[config] no session.ini in mod folder: staying idle");
        return c;
    }
    c.present = true;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (lineno == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = Trim(line.substr(0, eq));
        const std::string v = Trim(line.substr(eq + 1));
        if (k == "role") {
            c.role = v == "host"    ? Role::host
                     : v == "guest" ? Role::guest
                     : v == "echo"  ? Role::echo
                     : v == "solo"  ? Role::solo
                                    : Role::none;
        } else if (k == "transport") {
            c.transport = v == "udp" ? Transport::udp : Transport::steam;
        } else if (k == "peer_steamid") {
            c.peer_steam_id = std::strtoull(v.c_str(), nullptr, 10);
        } else if (k == "secret") {
            c.secret = v;
        } else if (k == "udp_peer") {
            c.udp_peer = v;
        } else if (k == "udp_port_local") {
            c.udp_port_local = static_cast<std::uint16_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (k == "udp_port_peer") {
            c.udp_port_peer = static_cast<std::uint16_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (k == "send_hz") {
            c.send_hz = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (k == "interp_delay_ms") {
            c.interp_delay_ms = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (k == "echo_delay_s") {
            c.echo_delay_s = std::strtof(v.c_str(), nullptr);
        } else if (k == "echo_offset_m") {
            c.echo_offset_m = std::strtof(v.c_str(), nullptr);
        } else if (k == "partner_activate") {
            c.partner_activate = ParseBool(v);
        } else if (k == "partner_body") {
            if (v == "holo" || v == "hologram") c.partner_body = PartnerBody::holo;
            else if (v == "marker") c.partner_body = PartnerBody::marker;
            else if (v == "echo") c.partner_body = PartnerBody::echo;
            else Log("[config] session.ini:%d partner_body '%s' not understood; keeping holo", lineno, v.c_str());
        } else if (k == "partner_holo") {
            // Exactly 8 hex digits = hex, as the mod's log prints HashIds ("0089ABCD"); otherwise
            // C syntax ("0x89ABCD" or decimal).
            const bool hex8 = v.size() == 8 && v.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
            c.partner_holo = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, hex8 ? 16 : 0));
        } else if (k == "partner_marker") {
            c.partner_marker = std::atoi(v.c_str());
            if (c.partner_marker < -1 || c.partner_marker > 255) c.partner_marker = -1;
        } else if (k == "partner_voice") {
            c.partner_voice = ParseBool(v);
        } else if (k == "verbose") {
            c.verbose = ParseBool(v);
        } else if (k == "debug_menu") {
            c.debug_menu = ParseBool(v);
        } else if (k == "debug_menu_key") {
            const std::uint32_t vk = ParseKey(v);
            if (vk) c.debug_menu_vk = vk;
            else Log("[config] session.ini:%d debug_menu_key '%s' not understood; keeping F9", lineno, v.c_str());
        } else if (k == "automation" || k == "automation_mute" || k == "automation_save_root" || k == "automation_foreign_saves") {
            // read by automation.cpp before the one-shot rename
        } else if (k == "one_shot") {
            c.one_shot = ParseBool(v);
        } else if (k == "name") {
            c.player_name = v;
        } else {
            Log("[config] session.ini:%d unknown key '%s' ignored", lineno, k.c_str());
        }
    }
    in.close();
    if (c.one_shot) {
        const std::wstring from = dir + L"\\session.ini", to = dir + L"\\session.last.ini";
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
            Log("[config] one-shot session: consumed (renamed to session.last.ini)");
        else
            Log("[config] one-shot session: could not rename session.ini (error %lu)", GetLastError());
    }
    if (c.send_hz < 1 || c.send_hz > 120) c.send_hz = 30;
    if (c.interp_delay_ms > 1000) c.interp_delay_ms = 100;
    if (!(c.echo_delay_s >= 0.0f && c.echo_delay_s <= 10.0f)) c.echo_delay_s = 2.0f;
    if (!(c.echo_offset_m >= -50.0f && c.echo_offset_m <= 50.0f)) c.echo_offset_m = 0.0f;
    Log("[config] role=%s transport=%s peer=%llu udp=%s:%u->%u send_hz=%u interp=%ums debug_menu=%d key=0x%X",
        RoleName(c.role), c.transport == Transport::udp ? "udp" : "steam",
        static_cast<unsigned long long>(c.peer_steam_id), c.udp_peer.c_str(), c.udp_port_local, c.udp_port_peer,
        c.send_hz, c.interp_delay_ms, c.debug_menu ? 1 : 0, c.debug_menu_vk);
    Log("[config] partner_body=%s partner_holo=%08X partner_marker=%d partner_voice=%d", PartnerBodyName(c.partner_body),
        c.partner_holo, c.partner_marker, c.partner_voice ? 1 : 0);
    return c;
}

}  // namespace st
