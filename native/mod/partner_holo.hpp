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
#include <vector>

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

// ---------------------------------------------------------------- collectors DB
// One DSCollectorsItemResource, read fresh. `ptr` is valid for the current frame
// only (the DB can be freed at any load).
struct Item {
    std::uintptr_t ptr = 0;
    std::uint32_t display_no = 0, hash = 0, name_hash = 0;
    std::int32_t sort = 0;
    std::uint8_t type = 0;
    bool hidden = false;
    bool empty = true, loaded = false;
    std::uintptr_t res = 0;  // loaded EntityResource (when `loaded`)
};
// Game thread. False when the functions did not verify or the DB is not loaded.
bool ReadItems(std::uintptr_t* db, std::vector<Item>* items);

// ---------------------------------------------------------------- independent spawns
// Debug tooling (the hologram catalog, docs/character-catalog.md): the same
// spawner calls and checks as the partner body, with the caller owning the state.
// Game thread only. Every Spawn that SpawnBegin started must end in SpawnTeardown.
struct Spawn {
    void* sp = nullptr;  // our spawner reference (we own one count)
    std::uint32_t hash = 0;
    bool realized = false;
    game::Entity* e = nullptr;
    std::uintptr_t vt = 0;
    game::Guid guid;
};
// Starts loading `item_ptr`'s DisplayEntityResource at `at` (item read this frame).
bool SpawnBegin(std::uintptr_t item_ptr, std::uint32_t hash, const game::WorldTransform& at, Spawn* s, char* why,
                std::size_t cap);
enum class SpawnState : std::uint8_t {
    loading,    // still streaming
    realized,   // loaded and realized this call: s.e is the entity (may be null: nothing realized)
    cancelled,  // the game cancelled the load (tear down)
    failed,     // spawner unreadable or a call faulted: s.sp was dropped (leaked), nothing to tear down
};
SpawnState SpawnPoll(Spawn& s, char* why, std::size_t cap);
// The realized entity is still the same object (vtable + GUID).
bool SpawnAlive(const Spawn& s);
// The partner body's welcome-hologram checks (reads only): nullptr when `e` is a
// safe welcome hologram (*comp = its DSWelcomeHoloComponent), else the reason.
const char* ValidateWelcomeHolo(game::Entity* e, std::uintptr_t* comp);
// StartVisibleWelcomeHolo after the same pre-call checks the partner body uses.
bool SpawnStartVisible(game::Entity* e, char* why, std::size_t cap);
// Welcome-hologram component state + resource flags to the log.
void LogWelcomeHolo(game::Entity* e, const char* what);
// Removes the entity (or cancels the load) and releases the spawner, exactly
// once. `step` receives what was done. Returns false when something had to be
// leaked (world unavailable, spawner no longer ours); the log says which.
bool SpawnTeardown(Spawn& s, const char* why, char* step, std::size_t cap);

}  // namespace st::holo
