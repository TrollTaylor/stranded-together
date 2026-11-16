// Death Stranding Director's Cut (ds.exe, Decima) engine bindings.
//
// Every address here was found statically in ds.exe (PE timestamp 1706443442,
// image base 0x140000000) — see docs/reverse-engineering.md for the evidence
// behind each one. Functions are located at runtime by byte signature first,
// then by the known RVA if the signature misses but the bytes there still
// match, so a game patch fails closed (logged, feature disabled) instead of
// calling into the wrong code.
#pragma once
#include <cstddef>
#include <cstdint>

namespace st::game {

// Decima WorldTransform: double-precision position + 3x3 float rotation.
// Layout confirmed from Entity::GetWorldTransform (copies 0x3C bytes) and the
// internal setter at RVA 0x22DFF80 (pos -> Entity+0xC8, rot -> Entity+0xE0).
struct WorldPosition {
    double x, y, z;
};
struct RotMatrix {
    float m[3][3];  // three rows (right, forward, up in Decima's convention — see docs)
};
struct WorldTransform {
    WorldPosition pos;  // +0x00
    RotMatrix rot;      // +0x18
};
static_assert(offsetof(WorldTransform, rot) == 0x18 && sizeof(RotMatrix) == 0x24, "WorldTransform layout");
// The engine copies exactly 0x3C bytes; our struct is padded to 0x40, which is fine for out-params.

// Entity field offsets (ds.exe DC). Only read through SafeRead helpers.
namespace entity_off {
constexpr std::uintptr_t kFlags = 0x88;          // bit0 transform dirty, bit4 propagate
constexpr std::uintptr_t kPosition = 0xC8;       // WorldPosition (doubles)
constexpr std::uintptr_t kRotation = 0xE0;       // RotMatrix (floats)
constexpr std::uintptr_t kResourceRef = 0x58;    // StreamingRef<EntityResource>
constexpr std::uintptr_t kTransformLock = 0x288; // CRITICAL_SECTION
}  // namespace entity_off

using Entity = void;
using EntityResource = void;

// 16-byte by-value argument of EntityResource::CreateEntity. Byte 0 selects
// what to do with the new entity afterwards (RVA 0x23C8130); 0 = nothing.
struct CreateEntityPostAction {
    std::uint8_t mode = 0;
    std::uint8_t pad[15] = {};
};

struct Api {
    // DSPlayerEntity_sExportedGetLocalDSPlayerEntity (RVA 0x2757A30)
    Entity* (*GetLocalPlayerEntity)() = nullptr;
    // Entity_ExportedGetWorldTransform (RVA 0x2362C30): returns out
    WorldTransform* (*EntityGetWorldTransform)(WorldTransform* out, Entity* e) = nullptr;
    // Entity_ExportedSetWorldTransform (RVA 0x2361EA0)
    void (*EntitySetWorldTransform)(Entity* e, const WorldTransform* wt) = nullptr;
    // Resolves Entity+0x58 to its EntityResource (RVA 0x1DDE990); no net refcount change.
    EntityResource* (*ResolveEntityResource)(void* resourceRef) = nullptr;
    // EntityResource_ExportedCreateEntity (RVA 0x23806A0).
    //   rot is three float4 rows (only xyz used); name is hashed into Entity+0x50;
    //   placeOnGround snaps to terrain; p6/p7 are optional (pass null).
    Entity* (*EntityResourceCreateEntity)(EntityResource* res, const WorldPosition* pos, const float (*rot)[4],
                                          const char* name, bool placeOnGround, void* p6, void* p7,
                                          const CreateEntityPostAction* post) = nullptr;
    // Entity::Remove (RVA 0x234CAA0); immediate=false queues removal (safe from game code).
    void (*EntityRemove)(Entity* e, bool immediate) = nullptr;
    // Game wrapper around SteamAPI_RunCallbacks (RVA 0x1A1F690), called directly
    // by the per-frame application tick (0x14263A5D0 at +0x385) on the main thread.
    void (*SteamRunCallbacksWrapper)() = nullptr;

    // DSPlayerEntity overrides Entity::Sleep/Wake (vtable +0xA0/+0xA8): unless
    // DSPlayerSystem+0x206D1 bit 4 ("gate") is set, they add/remove a sleep
    // reason (0x1C26F835) on the player system, which then sleeps/wakes THE
    // local Sam instead of the entity itself. The player system's own update
    // (0x1426E0410) sets the gate around its Sleep/Wake calls on Sam; the mod
    // does the same around every call that may sleep/wake the partner clone.
    // Decoded from the Sleep override (RVA 0x2743810):
    //   mov rcx,[DSPlayerSystem]; test byte [rcx+0x206D1],4; ... jmp Humanoid::Sleep
    std::uint8_t** DSPlayerSystem = nullptr;  // address of the singleton pointer (RVA 0x7BE7980)
    void* HumanoidSleep = nullptr;            // RVA 0x37406F0 (logged only)

    bool ready = false;  // every required function resolved
};

constexpr std::uintptr_t kDSPlayerSystemGateOffset = 0x206D1;
constexpr std::uint8_t kDSPlayerSystemGateBit = 0x04;
// Entity vtable: +0xA0 Sleep(Entity*, bool), +0xA8 Wake(Entity*). Base versions
// 0x14235F8E0 / 0x1423614F0 set / clear flag 0x100 and recurse into children;
// Entity_ExportedSetSleeping (0x142361E20) picks between them on that bit.
constexpr std::uintptr_t kEntityVtSleep = 0xA0;
constexpr std::uintptr_t kEntityVtWake = 0xA8;
constexpr std::uintptr_t kEntityGuid = 0x08;  // 16-byte GUID, fresh per created entity (CoCreateGuid)
// Entity+0x88 flag bits (see docs/reverse-engineering.md). 0x100 = asleep:
// confirmed in game, the awake local Sam reads with it clear.
constexpr std::uint64_t kEntityFlagSleeping = 0x100;

// Resolve everything. Safe to call more than once; logs each result.
bool Resolve();
const Api& Get();

// Guarded helpers: return false instead of crashing on a bad pointer.
bool TryGetLocalPlayer(Entity** out);
bool TryGetTransform(Entity* e, WorldTransform* out);
bool TrySetTransform(Entity* e, const WorldTransform& wt);
bool TryGetResource(Entity* e, EntityResource** out);
// Returns false only on a fault; *out may still be null.
bool TryCreateEntity(EntityResource* res, const WorldTransform& wt, const char* name, Entity** out);
// Partner lifecycle for an entity built from the player's DSPlayerEntityResource.
// Creation, waking and removal all run with the DSPlayerSystem gate set, so any
// Sleep/Wake on the clone acts on the clone itself and never adds or removes a
// sleep reason for the real Sam.
bool TryCreatePartner(EntityResource* res, const WorldTransform& wt, const char* name, Entity** out);
bool TryWakePartner(Entity* e);
bool TryRemovePartner(Entity* e);
bool TryReadFlags(Entity* e, std::uint64_t* out);
struct Guid {
    std::uint64_t lo = 0, hi = 0;
    bool operator==(const Guid& o) const { return lo == o.lo && hi == o.hi; }
    bool operator!=(const Guid& o) const { return !(*this == o); }
};
bool TryReadGuid(Entity* e, Guid* out);

std::uintptr_t ModuleBase();

}  // namespace st::game
