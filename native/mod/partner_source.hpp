// Where the partner's body comes from, for the synchronously created bodies:
//   echo   - the game's "echo" resource (DSPlayerEchoManagerConfig), a voice-only
//            entity (DSEchoComponent + VoiceComponent + DSTalkComponent, no model):
//            invisible, kept as a fallback and as the partner's voice source;
//   marker - a trace-point prop (DSPlayerTracePointConfig.EntityResources), the
//            static world marks other porters leave: visible, not a person.
// The welcome/collectors hologram (the default, humanoid body) is spawned
// asynchronously and lives in partner_holo.cpp; see docs/partner-hologram.md.
//
// Everything here is fault-free by construction: game memory is read with
// ReadProcessMemory (fails instead of raising), Decima type names are decoded
// from the one-instruction GetRTTI getters instead of calling them, and every
// pointer is checked to lie in ds.exe before use. DS's crash reporter fires on
// any access violation, even one the mod would catch.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "game_api.hpp"

namespace st::partner {

// ---- fault-free memory helpers (shared with partner_holo.cpp)
namespace mem {
std::uintptr_t Base();  // ds.exe image base
bool InImage(std::uintptr_t a);
bool InText(std::uintptr_t a);
// Never raises: ReadProcessMemory on our own process fails cleanly on a bad page.
bool Read(std::uintptr_t addr, void* out, std::size_t n);
template <class T>
bool Rd(std::uintptr_t addr, T& out) {
    return Read(addr, &out, sizeof(T));
}
std::uintptr_t Q(std::uintptr_t addr);  // 0 when unreadable
// Decima RTTI of an RTTIRefObject (decoded from its vtable[0] GetRTTI getter), or 0.
std::uintptr_t RttiOf(std::uintptr_t obj);
const char* RttiName(std::uintptr_t rtti);  // name string in ds.exe .rdata, or nullptr
bool DerivesFrom(std::uintptr_t rtti, const char* name);
// Code bytes at `va` match "48 8B ?? 05 ..." ('??' = any byte).
bool CodeMatches(std::uintptr_t va, const char* sig);
// Target of a RIP-relative operand: insn + insn_len + rel32 read at insn + rel_off.
std::uintptr_t RipTarget(std::uintptr_t insn, unsigned rel_off, unsigned insn_len);
}  // namespace mem

enum class Kind { echo, marker };

struct Source {
    Kind kind = Kind::echo;
    game::EntityResource* res = nullptr;  // AddRef'd by Acquire*, released by Release
    std::uintptr_t cfg = 0;               // config object the resource came from
    const char* label = "";               // "echo", "knot-space echo", "marker"
    int index = -1;                       // marker: index into EntityResources
};

// Find and validate the echo resource. Logs the full discovery (once per
// distinct outcome) and refuses anything that builds a player/Humanoid entity
// or carries DSPlayer*/AI components. Returns false and logs why otherwise.
bool Acquire(Source* out);
// Same for a trace-point prop: `want` < 0 picks the first acceptable entry,
// otherwise that index (falling back to the first acceptable one).
bool AcquireMarker(Source* out, int want);
void Release(Source& s);
// False once the source's config was unloaded or replaced: remove the entity.
bool StillLoaded(const Source& s);
// True while the game is tearing its own echoes down (the conditions under
// which its echo manager removes them, 0x142745CB0): remove the partner too.
bool GameRemovingEchoes();

enum class CreateResult { ok, busy, unloaded, faulted, null_entity };
// CreateEntity from the source after re-checking its config is still the one
// the resource came from (echo: while holding the echo manager's lock, as the
// game does for its own echoes).
CreateResult Create(const Source& s, const game::WorldTransform& at, const char* name, game::Entity** out);

// Read-only diagnostics for an entity: type, flags, model, component names.
void DescribeEntity(game::Entity* e, const char* what);
// First component of `e` whose Decima type is or derives from `type_name`; 0 if none.
std::uintptr_t FindComponentByName(game::Entity* e, const char* type_name);
// Entity_ExportedSetVisible (byte-checked); returns false if unavailable.
bool SetVisible(game::Entity* e, bool visible);
// Base Entity::Wake (vt+0xA8) if the entity is asleep and uses the base
// implementation (never a player override). Returns true if it is awake after.
bool WakeIfAsleep(game::Entity* e);
// Fault-free reads for liveness checks on entities the game may have freed.
bool ReadVtable(game::Entity* e, std::uintptr_t* out);
bool ReadGuid(game::Entity* e, game::Guid* out);
// Fault-free flag read.
bool ReadFlags(game::Entity* e, std::uint64_t* out);

// Plain removal for a non-player entity (Entity::Remove(e, immediate)).
bool Remove(game::Entity* e);

// Decima type name of an RTTIRefObject (via its GetRTTI getter), or nullptr.
const char* TypeName(const void* obj);

}  // namespace st::partner
