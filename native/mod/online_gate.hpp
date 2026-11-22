// Never uploading what the mod creates (world sync, docs/world-sync.md "The online gate"). Detours on the game's online
// structure service (DSOnlineObject service [0x147C18670], vtable 0x143EDECC8):
//   RegisterNew  0x143485040 (vt+0x28: queue a new own structure for createObject)
//   drain        0x143482F30 (vt+0x38: queue a removed structure's server object for deleteObject)
//   telemetry    0x143484AC0 (vt+0xB8: TelemetryDSOnlineObject*: RemoveManually, AppendMaterials, ...)
//   upload       0x142CB5BC0 (setConstructionMaterials for a structure with a server id)
//   likes        0x142E14820 (the likes manager's sendLike sender: dropped in hidden runs and on a world-sync guest)
// and a logger on the OTGC request constructor 0x141B91D30 (every request the game builds; a write request logs
// "[gate] OTGC WRITE request: <name>", which the hidden-test supervisor counts as game_server_writes and fails on).
//
// Installed when world_sync is on (both games) and ALWAYS in a hidden run (automation=1): a hidden run's host plays signed
// in to the real game server with the player's account, so everything is dropped there, on both games. In real use the
// host's own normal builds stay vanilla (the game uploads them, not the mod); the mod's structures carry the game's offline
// bit (cp+0xAB bit 7) and are dropped here too. The guest (world_sync on) drops everything: its world is a copy of the
// host's save and its copies of the host's uploaded structures carry the host's server ids.
//
// Byte-checked at Install; fails closed: if the gate cannot go in, world sync stays off (Ok() false).
#pragma once
#include <cstdint>

namespace st {
struct Config;
}

namespace st::gate {

// Bootstrap thread, after MH_Initialize, before world_share::Install. `sync_wanted`: world_sync asked for in this game.
void Install(const Config& config, bool sync_wanted) noexcept;
// The gate is in (its byte checks passed and every detour went in).
bool Ok() noexcept;
const char* OffReason() noexcept;
// world_share tells the gate the sync is on (after its own checks), so the host's mod structures and the guest's calls drop.
void SetSyncOn(bool on) noexcept;

// RAII: the mod's own game calls (MirrorCreate, the removals the mod applies): every online call made inside drops.
struct ScopeMirroring {
    ScopeMirroring() noexcept;
    ~ScopeMirroring();
    ScopeMirroring(const ScopeMirroring&) = delete;
    ScopeMirroring& operator=(const ScopeMirroring&) = delete;
};
bool Mirroring() noexcept;  // this thread is inside a ScopeMirroring

// A construction point (pool address) the mod made or removed on the host: its online calls drop.
void NoteModCp(std::uintptr_t cp) noexcept;

struct Counts {
    std::uint32_t reg = 0, drain = 0, telemetry = 0, upload = 0;           // dropped
    std::uint32_t reg_pass = 0, drain_pass = 0, telemetry_pass = 0, upload_pass = 0;
    std::uint32_t otgc = 0, otgc_write = 0;                                // requests the game built
    std::uint32_t likes = 0, likes_pass = 0;                               // the likes manager's sendLike sender (W2)
};
Counts GetCounts() noexcept;
std::uint32_t DroppedTotal() noexcept;
// "[gate] dropped register n, drain n, telemetry n, upload n; passed ...; OTGC requests n (write n)"
void LogCounts(const char* when) noexcept;

}  // namespace st::gate
