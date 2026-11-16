#pragma once
// Stranded Together: the join-code check behind the Join tab's live
// validation. Pure (no I/O, no Steam): built on the mod's own DST1 parser
// (st_net, stcoop/steam_join_code.hpp), so the launcher accepts exactly what
// the game accepts: DST1-SSSSS-SSSSS-SSSSS-SSSSS-CC@<SteamID64>.
// Adapted from Diamond Dogs Together's launcher_join_code.
#include <cstdint>
#include <string>

#include "launcher_model.hpp"

namespace st {

// Checks a pasted or typed code. Tolerant like the mod: blanks, case, dashes,
// O->0 and I/L->1; a pasted Steam invite string ("+stranded_join <code>") is
// accepted too. own_steam_id is the player's own SteamID64 (0 = unknown); the
// player's own code is refused. An empty field gives {false, ""}.
CodeCheck CheckJoinCode(const std::string& text, std::uint64_t own_steam_id);

// The canonical code for a secret and host, "" when either is invalid.
std::string FormatJoinCode(const std::string& secret20, std::uint64_t host_steam_id);

// The same tolerant parse as CheckJoinCode: true with the canonical
// 20-character secret and the host's SteamID64. Never logs anything.
bool ParseJoinCode(const std::string& text, std::string& secret_out, std::uint64_t& host_out);

}  // namespace st
