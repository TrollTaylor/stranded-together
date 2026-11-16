// Steam "owner": acquires the steamclient interfaces we need through the game's
// own (already initialized) steam_api64.dll. DS ships an old SDK whose
// steam_api has no ISteamNetworkingMessages exports, so — exactly like Diamond
// Dogs Together on MGSV — we ask steamclient for "SteamNetworkingMessages002"
// by version string and call it through the self-declared vtable ABI in
// stcoop/steam_net_abi.hpp.
//
// Pump() is called from the game's main thread (our SteamAPI_RunCallbacks
// wrapper hook), after the real RunCallbacks. Everything else is lock-free reads.
#pragma once
#include <cstdint>

#include "stcoop/steam_transport.hpp"

namespace st::steam {

enum class Phase { waiting_for_steam, ready, failed };

// Host: also register the 1251 SessionRequest CCallback once Steam is ready
// (backup to the function-pointer dispatch). Call before Pump starts.
void WantSessionRequests(bool want) noexcept;
// Called (game thread) just before the game's SteamAPI_Shutdown runs, so the
// transport can say goodbye and park before steamclient is torn down.
void SetShutdownHandler(void (*fn)()) noexcept;
void Pump() noexcept;  // game thread
Phase GetPhase() noexcept;
const char* FailReason() noexcept;
std::uint64_t SelfId() noexcept;
const char* PersonaName() noexcept;  // empty until ready

// SteamTransportConfig::resolve target: 1 ready, -1 failed, 0 pending.
int Resolve(void* ctx, stcoop::lab::SteamNetApi& out, const char*& reason) noexcept;

// Rich presence (game thread only; no-op until ready).
void SetRichPresence(const char* key, const char* value) noexcept;

}  // namespace st::steam
