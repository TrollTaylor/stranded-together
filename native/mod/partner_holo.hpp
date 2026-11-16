// The partner's default body: the game's welcome / collectors hologram, the
// humanoid hologram DS stands at other players' structures. It comes from the
// collectors-item database (DSCollectorsItemResource.DisplayEntityResource, a
// StreamingRef) and is spawned through the game's asynchronous entity spawner,
// exactly like the construction-point manager does (0x142C86C80 / 0x142CA1AA0).
// Design, addresses and what to look for in the log: docs/partner-hologram.md.
//
// Game thread only (st::sync::Tick). Every game function is byte-checked once
// before first use; every game read goes through ReadProcessMemory.
#pragma once
#include <cstddef>
#include <cstdint>

#include "game_api.hpp"

namespace st::holo {

enum class Phase : std::uint8_t { idle, loading, live };

// Resolves and byte-checks every function and global the hologram needs (once,
// logged). False when this ds.exe does not match or a call faulted earlier.
bool Available();
const char* UnavailableReason();

enum class BeginResult : std::uint8_t {
    started,      // spawner created and loading (or already busy)
    waiting_db,   // collectors DB not loaded yet
    no_item,      // DB has no usable hologram item (all empty or rejected)
    unavailable,  // functions missing / disabled after a fault
    failed,       // the spawner could not be created
};
// Pick an item (want_hash: session.ini partner_holo, 0 = automatic) and start
// loading it at `at`. `why` receives a one-line reason for anything but started.
BeginResult Begin(const game::WorldTransform& at, std::uint32_t want_hash, double now_ms, char* why, std::size_t cap);

enum class PollResult : std::uint8_t {
    idle,         // nothing spawned
    loading,      // still streaming in
    became_live,  // realized, validated and made visible this frame
    live,
    failed,       // timed out / cancelled / refused / destroyed; everything torn down, `why` says which
};
PollResult Poll(double now_ms, char* why, std::size_t cap);

// Live only: SetWorldTransform on the hologram. False on a fault (then Remove).
bool Move(const game::WorldTransform& at);
// Live only: re-assert StartVisible about once a second and log state changes.
void Maintain(double now_ms);
// Any phase: remove the entity (if realized) or cancel the load, then release the
// spawner. Returns true if there was something to tear down.
bool Remove(const char* why);

Phase GetPhase();
const char* PhaseName();
game::Entity* LiveEntity();  // the realized hologram while it is still the same object, else nullptr
std::uint32_t CurrentHash();  // HashId of the item being shown/loaded (0 = none)
// The collectors DB was freed or replaced since the spawn started.
bool DbChanged();
// Debug menu: StartVisible / StopVisible (hide also stops the periodic re-assert).
bool SetShown(bool shown);
// Debug menu / diagnostics: entity + component state to the log.
bool Describe(const char* what);
// F7 / respawn: forget items refused earlier and log the DB table again.
void ResetRejected();

}  // namespace st::holo
