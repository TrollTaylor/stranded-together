#include "partner_collision.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "game_actions.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "partner_tackle.hpp"
#include "player_sync.hpp"

namespace st::collision {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// docs/real-partner-body.md section 11. Everything is byte-checked before the first call.
constexpr std::uintptr_t kNewGuid = 0x1920CF0;          // void* NewGuid(GGUUID* out): CoCreateGuid, byte order shuffled
constexpr std::uintptr_t kCreateComponent = 0x22B4FC0;  // Component* (RTTI*, name, resource, GGUUID*)
constexpr std::uintptr_t kAddComponentThunk = 0x2348210;  // jmp AddComponent
constexpr std::uintptr_t kAddComponent = 0x2348220;     // void (Entity*, Component*): list insert, init messages
constexpr std::uintptr_t kConfigure = 0x2446340;        // void Capsule::Configure(Capsule*, const Cfg*): (re)creates the mover
constexpr std::uintptr_t kSetTransform = 0x2446980;     // void Capsule::SetTransform(Capsule*, const WorldTransform*)
constexpr std::uintptr_t kSetVelocity = 0x2446910;      // void Capsule::SetVelocity(Capsule*, const float[4])
constexpr std::uintptr_t kDisable = 0x243A740;          // ++disable counter, out of the world
constexpr std::uintptr_t kEnable = 0x243A9E0;           // --disable counter, into the world when <= 0 and entity flag 0x4000
constexpr std::uintptr_t kAddRemoveWorld = 0x243AA30;   // what Disable / Enable / removal call
constexpr std::uintptr_t kHumanoidFilter = 0x3744220;   // the contact filter Humanoid::Init stores at capsule +0x70
constexpr std::uintptr_t kHumanoidInit = 0x373A7D1;     // Humanoid::Init: call NewGuid .. call AddComponent (the reference sequence)
constexpr std::uintptr_t kHumanoidConfigureCall = 0x373A8E8;  // Humanoid::Init: call Capsule::Configure
constexpr std::uintptr_t kCapsuleRtti = 0x4B39F70;      // CapsuleComponent
constexpr std::uintptr_t kNoneNameGlobal = 0x4B386D0;   // what Humanoid::Init passes as the name (-> 0x144B02740)
constexpr std::uintptr_t kCapsuleVtable = 0x3D1BD08;
constexpr std::uintptr_t kCapsuleListenerVt = 0x3D1BD70;  // capsule +0x50; slot 1 jumps to [capsule +0x70] unconditionally
constexpr std::uintptr_t kListenerFilterThunk = 0x24417B0;
constexpr std::uintptr_t kMoverRigidVt = 0x3BDBD00;     // PhysicsCharacterMoverRigidBody (type 0)
constexpr std::uintptr_t kMoverProxyVt = 0x3BDC020;     // PhysicsCharacterMoverProxy (type 1)
constexpr std::uintptr_t kPhysicsGlobal = 0x578C140;    // [it] +0x30 -> the collision layer matrix (bits at +0x30)

// CapsuleComponent (0xB0)
constexpr std::uintptr_t kCapEntity = 0x48, kCapDisable = 0x58, kCapInWorld = 0x5C, kCapMover = 0x60, kCapMoverRes = 0x68,
                         kCapFilter = 0x70, kCapType = 0x78, kCapLayer = 0x7C, kCapGroup = 0x80;
constexpr std::uintptr_t kMoverResRadius = 0x3C;  // PhysicsCharacterMoverResource radius (capsule radius getter 0x14243BF70)
// Entity
constexpr std::uintptr_t kEntParent = 0x70, kEntFlags = 0x88, kEntComponents = 0x90, kEntPos = 0xC8;
constexpr std::uintptr_t kSamCapsuleRef = 0x5A0;  // Humanoid: weak reference to its capsule (+0x20)
constexpr std::uint64_t kFlagVisible = 0x2, kFlagAsleep = 0x100, kFlagInWorld = 0x4000, kFlagInitialized = 0x10000000;

constexpr int kGroup = 3;              // DSControlledEntityResource vt+0x98 (0x1434F6CC0: base 1 | 2), what the game would use
constexpr float kMaxSlope = 1.0472f;   // 60 degrees, Humanoid::Init's default
constexpr double kSeparatedM = 0.90;   // into the world only this far apart (2 x 0.40 m + margin)
constexpr double kOverlapM = 0.55;     // out of the world when closer than this (a snap, lag): no depenetration push
constexpr double kOverlapDzM = 1.8;    // ... and vertically closer than this
// Sam's feet this far above the partner's origin: off (review: the tall capsule, 4 m from the partner's
// feet, reaches Sam on a ledge, rock or vehicle 1.65-4 m above the partner, where the v0.7 1.65 m capsule
// never could: an invisible pillar or a push). Below this the 1.65 m capsule overlapped Sam's feet the
// same way; the climb it defeats starts from the partner's own level.
constexpr double kSamAboveM = 1.6;
constexpr double kFastMps = 8.0;       // the partner rides something
constexpr double kJumpM = 2.0;         // one tick: a snap or teleport
constexpr double kStaleMs = 500.0;     // no remote sample for this long
// Only near the local Sam (review: the capsule is placed where the partner is in THEIR world, so
// away from Sam it could only meet our own parked vehicles or AI, never Sam). In the world within
// kNearM, out again beyond kFarM.
constexpr double kNearM = 4.0;
constexpr double kFarM = 4.5;

struct Cfg {
    std::uint64_t mover_res;
    std::int32_t type;
    std::int32_t layer;
    std::int32_t group;
    std::int32_t mask;  // 0x1F, as the game passes
    float max_slope;
    std::uint32_t pad;
};
static_assert(sizeof(Cfg) == 0x20, "Capsule::Configure argument");

// ---------------------------------------------------------------- state (game thread)
Config g_cfg;
bool g_on = false;          // partner_collision=1 (runtime: action collision 1)
int g_layer = 10, g_type = 0;
int g_checked = 0;          // 0 not yet, 1 ok, -1 a check failed
char g_why_off[160] = {};
std::uintptr_t g_base = 0;
unsigned g_attach_failures = 0;

struct Body {
    std::uintptr_t e = 0;
    game::Guid guid;
    std::uintptr_t comp = 0;
    bool failed = false;     // attach failed for this entity: not tried again
    bool disabled = false;   // our Disable is outstanding (balanced by Enable)
    bool close_by = false;   // within kNearM of Sam (hysteresis to kFarM)
    bool have_prev = false;
    double prev[3] = {};
    double prev_ms = 0;
    double speed = 0;
    double hold_until_ms = 0;  // after a jump
};
Body g_b;
const char* g_state = "none";  // logged on change
bool g_logged_self = false;

// statistics
double g_trace_until_ms = 0;
double g_trace_request_ms = 0;  // set by the debug action, started by the next tick
double g_next_stats_ms = 0;
double g_min_d = 1e9;
unsigned g_ticks_on = 0, g_ticks = 0, g_enables = 0, g_disables = 0;
double g_sam_prev[2] = {};
bool g_sam_prev_ok = false;

bool Check(const char* name, std::uintptr_t rva, const char* sig) {
    if (mem::CodeMatches(g_base + rva, sig)) return true;
    std::snprintf(g_why_off, sizeof(g_why_off), "%s at ds.exe+0x%llX does not match", name, static_cast<unsigned long long>(rva));
    return false;
}

bool Resolve() {
    if (g_checked) return g_checked > 0;
    g_base = mem::Base();
    const std::uintptr_t b = g_base;
    bool ok = Check("NewGuid", kNewGuid, "40 53 48 83 EC 40 48 8B 05") &&
              Check("CreateComponent", kCreateComponent, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56") &&
              Check("AddComponent thunk", kAddComponentThunk, "E9 0B 00 00 00") &&
              Check("AddComponent", kAddComponent, "48 89 5C 24 18 55 56 57 41 56 41 57 48 83 EC 60") &&
              Check("Capsule::Configure", kConfigure, "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B 32 48 8B D9 48 8B 49") &&
              Check("Capsule::SetTransform", kSetTransform, "48 89 5C 24 08 57 48 83 EC 60 48 8B D9 48 8B FA 48 8B 49 60 48 85 C9 74") &&
              Check("Capsule::SetVelocity", kSetVelocity, "48 8B 49 60 48 85 C9 74 0A 48 8B 01 48 FF A0 28 01 00 00 C3") &&
              Check("Capsule::Disable", kDisable, "40 53 48 83 EC 20 48 8B D9 48 83 C1 58 E8") &&
              Check("Capsule::Enable", kEnable, "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 83 C1 58 E8") &&
              Check("Capsule add/remove world", kAddRemoveWorld, "40 57 48 83 EC 60 48 8B 05") &&
              Check("Humanoid contact filter", kHumanoidFilter, "48 89 5C 24 18 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 D2 74 5A 48 8B 02") &&
              // Humanoid::Init: call NewGuid; mov rdx,[name]; lea r9,[rsp+40]; xor r8d,r8d; lea rcx,[RTTI];
              // call CreateComponent; mov rdx,rax; mov rcx,rdi; mov rsi,rax; call AddComponent thunk
              Check("Humanoid::Init capsule sequence", kHumanoidInit,
                    "E8 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 4C 8D 4C 24 40 45 33 C0 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D0 48 8B CF 48 8B F0 "
                    "E8") &&
              Check("Humanoid::Init Configure call", kHumanoidConfigureCall, "E8") &&
              Check("capsule listener filter thunk", kListenerFilterThunk, "40 53 48 83 EC 20 48 8B D9 48 8B 4A 08 E8 ?? ?? ?? ?? 4C 8B 43 20");
    if (ok && (mem::RipTarget(b + kHumanoidInit, 1, 5) != b + kNewGuid ||
               mem::RipTarget(b + kHumanoidInit + 5, 3, 7) != b + kNoneNameGlobal ||
               mem::RipTarget(b + kHumanoidInit + 0x14, 3, 7) != b + kCapsuleRtti ||
               mem::RipTarget(b + kHumanoidInit + 0x1B, 1, 5) != b + kCreateComponent ||
               mem::RipTarget(b + kHumanoidInit + 0x29, 1, 5) != b + kAddComponentThunk ||
               mem::RipTarget(b + kAddComponentThunk, 1, 5) != b + kAddComponent ||
               mem::RipTarget(b + kHumanoidConfigureCall, 1, 5) != b + kConfigure)) {
        std::snprintf(g_why_off, sizeof(g_why_off), "Humanoid::Init's capsule calls do not lead where expected");
        ok = false;
    }
    const char* rn = ok ? mem::RttiName(b + kCapsuleRtti) : nullptr;
    if (ok && (!rn || std::strcmp(rn, "CapsuleComponent") != 0)) {
        std::snprintf(g_why_off, sizeof(g_why_off), "the CapsuleComponent RTTI at ds.exe+0x%llX has another name",
                      static_cast<unsigned long long>(kCapsuleRtti));
        ok = false;
    }
    if (ok && Q(b + kCapsuleListenerVt + 8) != b + kListenerFilterThunk) {
        std::snprintf(g_why_off, sizeof(g_why_off), "the capsule listener vtable slot 1 is not the filter thunk");
        ok = false;
    }
    if (ok && !Q(b + kNoneNameGlobal)) {
        std::snprintf(g_why_off, sizeof(g_why_off), "the component name global is not set");
        ok = false;
    }
    g_checked = ok ? 1 : -1;
    if (ok) Log("[coll] capsule functions verified (Humanoid::Init sequence, CreateComponent, AddComponent, Configure, SetTransform, "
                "SetVelocity, Disable/Enable, contact filter)");
    else Log("[coll] player collision unavailable: %s", g_why_off);
    return ok;
}

// ---------------------------------------------------------------- guarded game calls
bool CallNewGuid(void* g) {
    __try {
        reinterpret_cast<void* (*)(void*)>(g_base + kNewGuid)(g);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallCreate(const void* name, const void* guid, std::uintptr_t* out) {
    __try {
        *out = reinterpret_cast<std::uintptr_t>(reinterpret_cast<void* (*)(const void*, const void*, void*, const void*)>(
            g_base + kCreateComponent)(reinterpret_cast<const void*>(g_base + kCapsuleRtti), name, nullptr, guid));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallAdd(std::uintptr_t e, std::uintptr_t comp) {
    __try {
        reinterpret_cast<void (*)(void*, void*)>(g_base + kAddComponent)(reinterpret_cast<void*>(e), reinterpret_cast<void*>(comp));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallConfigure(std::uintptr_t comp, const Cfg* cfg) {
    __try {
        reinterpret_cast<void (*)(void*, const void*)>(g_base + kConfigure)(reinterpret_cast<void*>(comp), cfg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallPlace(std::uintptr_t comp, const void* wt) {
    alignas(16) static const float kZero[4] = {0, 0, 0, 0};
    __try {
        reinterpret_cast<void (*)(void*, const void*)>(g_base + kSetTransform)(reinterpret_cast<void*>(comp), wt);
        reinterpret_cast<void (*)(void*, const void*)>(g_base + kSetVelocity)(reinterpret_cast<void*>(comp), kZero);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallComp(std::uintptr_t rva, std::uintptr_t comp) {
    __try {
        reinterpret_cast<void (*)(void*)>(g_base + rva)(reinterpret_cast<void*>(comp));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- reads (fault-free)
bool ReadWt(std::uintptr_t e, game::WorldTransform* wt) {
    std::uint8_t raw[0x3C];
    if (!mem::Read(e + kEntPos, raw, sizeof(raw))) return false;
    std::memcpy(&wt->pos, raw, 0x18);
    std::memcpy(&wt->rot, raw + 0x18, 0x24);
    return std::isfinite(wt->pos.x) && std::isfinite(wt->pos.y) && std::isfinite(wt->pos.z);
}

// The capsule is one of the entity's components, is a CapsuleComponent and belongs to it.
bool CompValid(std::uintptr_t e, std::uintptr_t comp) {
    if (!e || !comp || Q(comp) != g_base + kCapsuleVtable || Q(comp + kCapEntity) != e) return false;
    std::int32_t n = 0;
    if (!Rd(e + kEntComponents, n) || n <= 0 || n > 64) return false;
    const std::uintptr_t data = Q(e + kEntComponents + 8);
    std::uintptr_t list[64];
    if (!data || !mem::Read(data, list, sizeof(list[0]) * static_cast<std::size_t>(n))) return false;
    for (std::int32_t i = 0; i < n; ++i)
        if (list[i] == comp) return true;
    return false;
}

// The local Sam's own capsule (Humanoid +0x5A0 -> capsule +0x20), or 0.
std::uintptr_t SamCapsule(std::uintptr_t sam) {
    const std::uintptr_t ref = sam ? Q(sam + kSamCapsuleRef) : 0;
    if (!ref) return 0;
    const std::uintptr_t c = ref - 0x20;
    return Q(c) == g_base + kCapsuleVtable && Q(c + kCapEntity) == sam ? c : 0;
}

bool SamCapsuleInWorld(std::uintptr_t cap) {
    std::uint8_t in = 0;
    std::int32_t dis = 0;
    return cap && Rd(cap + kCapInWorld, in) && in == 1 && Rd(cap + kCapDisable, dis) && dis <= 0;
}

int MatrixBit(int a, int b2) {
    const std::uintptr_t phys = Q(g_base + kPhysicsGlobal);
    const std::uintptr_t m = phys ? Q(phys + 0x30) : 0;
    if (!m) return -1;
    const int lo = a < b2 ? a : b2, hi = a < b2 ? b2 : a;
    const int idx = lo * 127 - (lo - 1) * lo / 2 + hi;
    std::uint8_t byte = 0;
    if (!Rd(m + 0x30 + static_cast<std::uintptr_t>(idx >> 3), byte)) return -1;
    return (byte >> (idx & 7)) & 1;
}

void LogSelfChecks(std::uintptr_t sam) {
    const std::uintptr_t c = SamCapsule(sam);
    if (!c) {
        Log("[coll] self check: the local Sam has no readable capsule at +0x5A0");
        return;
    }
    std::int32_t type = -1, layer = -1, group = -1, dis = 0;
    std::uint8_t in = 0;
    float radius = 0;
    Rd(c + kCapType, type), Rd(c + kCapLayer, layer), Rd(c + kCapGroup, group), Rd(c + kCapDisable, dis), Rd(c + kCapInWorld, in);
    const std::uintptr_t res = Q(c + kCapMoverRes);
    if (res) Rd(res + kMoverResRadius, radius);
    const std::uintptr_t rtti = res ? mem::RttiOf(res) : 0;
    const char* rn = rtti ? mem::RttiName(rtti) : nullptr;
    const std::uintptr_t mover = Q(c + kCapMover);
    const std::uintptr_t mvt = mover ? Q(mover) : 0;
    Log("[coll] self check: Sam's capsule %p type %d layer %d group %d in world %u disable %d mover %s; resource %s radius %.2f m; "
        "layer matrix (8,9)=%d (8,10)=%d (9,10)=%d (8,8)=%d (8,25)=%d (8,14)=%d (8,34)=%d",
        reinterpret_cast<void*>(c), type, layer, group, in, dis,
        mvt == g_base + kMoverProxyVt ? "proxy" : mvt == g_base + kMoverRigidVt ? "rigid body" : "?", rn ? rn : "?", radius,
        MatrixBit(8, 9), MatrixBit(8, 10), MatrixBit(9, 10), MatrixBit(8, 8), MatrixBit(8, 25), MatrixBit(8, 14), MatrixBit(8, 34));
}

// The capsule's layer (docs/real-partner-body.md 11.5): 10 "Humanoid blocker" by default. Its row in the
// live layer matrix is logged once. Layers 9 and 25 are test settings: in hidden runs 25 did not block Sam
// reliably (he walked through it on 2 of 4 approaches) and the game offered Climb on 9, 10 and 25 alike, so
// the layer does not decide climbing; the capsule's height does (TallResource below).
int g_layer_picked = 0;  // 0 not yet

int PickLayer() {
    if (g_layer_picked) return g_layer_picked;
    g_layer_picked = g_layer;
    Log("[coll] capsule layer %d; matrix: (%d,8)=%d (%d,9)=%d (%d,14)=%d (%d,16)=%d (%d,34)=%d (%d,17)=%d (%d,31)=%d", g_layer, g_layer,
        MatrixBit(g_layer, 8), g_layer, MatrixBit(g_layer, 9), g_layer, MatrixBit(g_layer, 14), g_layer, MatrixBit(g_layer, 16), g_layer,
        MatrixBit(g_layer, 34), g_layer, MatrixBit(g_layer, 17), g_layer, MatrixBit(g_layer, 31));
    return g_layer_picked;
}

// ---------------------------------------------------------------- a capsule too tall to climb
// Sam climbs any obstacle up to about his own height: on the partner's 1.65 m capsule (Sam's own mover
// resource) the game offered Climb and Sam climbed onto it, on every layer. The partner's capsule gets a
// private copy of Sam's PhysicsCharacterMoverResource with a larger StandingHeight instead, which
// Capsule::Configure turns into the mover's height (0x141C31930: mover vt+0x268(..., StandingHeight - 0.05)).
// The copy is plain data (RTTI: vtable, ObjectUUID, refcount at +0x18, floats, the material Ref at +0x60):
// its refcount is set far above anything the game adds or releases, so the release at 0x1419F5E50 never
// frees it; the material it points to gets one reference of ours. Copies live for the session (at most
// kMaxTall, one page).
constexpr std::size_t kMoverResSize = 0x68;  // RTTI size of PhysicsCharacterMoverResource
constexpr std::uintptr_t kMoverResRefcount = 0x18, kMoverResStanding = 0x30, kMoverResMaterial = 0x60;
constexpr std::uint32_t kRefcountMask = 0x3FFFFF;  // the low 22 bits count (0x1419F5E50)
constexpr std::uint32_t kOurRefcount = 0x200000;
constexpr int kMaxTall = 16;
struct Tall {
    std::uintptr_t src = 0;
    float height = 0;
    std::uintptr_t copy = 0;
};
Tall g_tall[kMaxTall];
int g_tall_n = 0;
std::uint8_t* g_tall_page = nullptr;
float g_height = 4.0f;  // partner_collision_height (0: Sam's own resource)

// The resource to configure the partner capsule with: Sam's own, or a copy of it with StandingHeight =
// height. 0 when Sam's resource does not check out.
std::uintptr_t TallResource(std::uintptr_t sam_res, float height) {
    if (!sam_res) return 0;
    const std::uintptr_t rtti = mem::RttiOf(sam_res);
    const char* rn = rtti ? mem::RttiName(rtti) : nullptr;
    if (!rn || std::strcmp(rn, "PhysicsCharacterMoverResource") != 0) return 0;
    float standing = 0;
    if (!Rd(sam_res + kMoverResStanding, standing) || !std::isfinite(standing)) return 0;
    if (!(height > standing + 0.05f)) return sam_res;  // off, or not taller than Sam's own
    alignas(16) std::uint8_t raw[kMoverResSize];
    if (!mem::Read(sam_res, raw, sizeof(raw))) return 0;
    // A copy is reused only while the source still holds the same bytes (review: after a reload a new
    // resource can sit at the old address): everything but the height and the refcount compared.
    for (int i = 0; i < g_tall_n; ++i) {
        if (g_tall[i].src != sam_res || std::fabs(g_tall[i].height - height) >= 0.001f) continue;
        const auto* c = reinterpret_cast<const std::uint8_t*>(g_tall[i].copy);
        bool same = true;
        for (std::size_t k = 0; k < sizeof(raw) && same; ++k) {
            const bool skip = (k >= kMoverResStanding && k < kMoverResStanding + 4) || (k >= kMoverResRefcount && k < kMoverResRefcount + 4);
            same = skip || c[k] == raw[k];
        }
        if (same) return g_tall[i].copy;
    }
    if (g_tall_n >= kMaxTall) {
        Log("[coll] tall capsule: %d resource copies made already; Sam's own height is used", kMaxTall);
        return sam_res;
    }
    std::uintptr_t mat = 0;
    std::memcpy(&mat, raw + kMoverResMaterial, sizeof(mat));
    std::uint32_t mat_count = 0;
    if (mat && (!Rd(mat + kMoverResRefcount, mat_count) || (mat_count & kRefcountMask) == 0 || (mat_count & kRefcountMask) > 0x100000)) {
        Log("[coll] tall capsule: the mover resource's material %p does not read as a live resource; Sam's own height is used",
            reinterpret_cast<void*>(mat));
        return sam_res;
    }
    if (!g_tall_page) {
        g_tall_page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!g_tall_page) return sam_res;
    }
    std::uint8_t* dst = g_tall_page + static_cast<std::size_t>(g_tall_n) * 0x80;
    std::memcpy(dst, raw, sizeof(raw));
    std::memcpy(dst + kMoverResStanding, &height, sizeof(height));
    std::uint32_t rc = 0;
    std::memcpy(&rc, raw + kMoverResRefcount, sizeof(rc));
    rc = (rc & ~kRefcountMask) | kOurRefcount;
    std::memcpy(dst + kMoverResRefcount, &rc, sizeof(rc));
    if (mat) InterlockedIncrement(reinterpret_cast<volatile LONG*>(mat + kMoverResRefcount));  // the copy's reference
    g_tall[g_tall_n++] = Tall{sam_res, height, reinterpret_cast<std::uintptr_t>(dst)};
    Log("[coll] tall capsule: a copy of Sam's mover resource with standing height %.2f m (his own %.2f m)", static_cast<double>(height),
        static_cast<double>(standing));
    return reinterpret_cast<std::uintptr_t>(dst);
}
void SetState(const char* s, double d) {
    if (std::strcmp(s, g_state) == 0) return;
    g_state = s;
    if (d < 1e8) Log("[coll] %s (partner %.2f m away)", s, d);
    else Log("[coll] %s", s);
}

// The body is gone or another one: the capsule went with the old body (BodyGoing took it out of the
// world first while the entity was still live, so a body the teardown could not remove leaves no
// invisible wall). Nothing is called here: the old entity may already be freed.
void Forget(const char* why) {
    if (g_b.e && (g_b.comp || g_b.failed))
        Log("[coll] body %p gone (%s): its capsule went with it%s", reinterpret_cast<void*>(g_b.e), why,
            g_b.comp && !g_b.disabled ? " (it was still in the world)" : "");
    g_b = Body{};
}

// The game's own sequence (Humanoid::Init), for the partner body.
bool Attach(std::uintptr_t e, std::uintptr_t sam, const game::WorldTransform& wt) {
    const std::uintptr_t sam_cap = SamCapsule(sam);
    const std::uintptr_t mover_res = sam_cap ? Q(sam_cap + kCapMoverRes) : 0;
    const std::uintptr_t rtti = mover_res ? mem::RttiOf(mover_res) : 0;
    const char* rn = rtti ? mem::RttiName(rtti) : nullptr;
    if (!rn || std::strcmp(rn, "PhysicsCharacterMoverResource") != 0) {
        Log("[coll] attach: Sam's capsule has no PhysicsCharacterMoverResource (%s)", rn ? rn : "unreadable");
        return false;
    }
    std::uint64_t f = 0;
    if (!Rd(e + kEntFlags, f) || !(f & kFlagInWorld) || !(f & kFlagInitialized)) {
        Log("[coll] attach: body flags 0x%llX lack 0x4000 (in world) or 0x10000000 (initialized)", static_cast<unsigned long long>(f));
        return false;
    }
    if (!g_logged_self) {
        g_logged_self = true;
        LogSelfChecks(sam);
    }
    alignas(16) std::uint8_t guid[16] = {};
    std::uintptr_t comp = 0;
    const void* name = reinterpret_cast<const void*>(Q(g_base + kNoneNameGlobal));
    if (!CallNewGuid(guid) || !CallCreate(name, guid, &comp) || !comp) {
        Log("[coll] attach: CreateComponent failed");
        return false;
    }
    if (Q(comp) != g_base + kCapsuleVtable || Q(comp + 0x50) != g_base + kCapsuleListenerVt) {
        Log("[coll] attach: the new component %p is not a CapsuleComponent; not added (leaked)", reinterpret_cast<void*>(comp));
        return false;
    }
    // The contact filter the capsule's listener jumps to unconditionally (Humanoid::Init stores its own).
    const std::uintptr_t filter = g_base + kHumanoidFilter;
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(comp + kCapFilter), &filter, sizeof(filter), &wrote) ||
        wrote != sizeof(filter)) {
        Log("[coll] attach: could not set the contact filter; not added (leaked)");
        return false;
    }
    if (!CallAdd(e, comp)) {
        Log("[coll] attach: AddComponent faulted");
        return false;
    }
    if (!CompValid(e, comp)) {
        Log("[coll] attach: the capsule is not in the body's component list after AddComponent");
        return false;
    }
    const std::uintptr_t res = TallResource(mover_res, g_height);
    Cfg cfg{};
    cfg.mover_res = res ? res : mover_res;
    cfg.type = g_type;
    cfg.layer = PickLayer();
    cfg.group = kGroup;
    cfg.mask = 0x1F;
    cfg.max_slope = kMaxSlope;
    if (!CallConfigure(comp, &cfg)) {
        Log("[coll] attach: Configure faulted");
        if (Q(comp + kCapMover)) g_b.comp = comp;  // a mover exists: the caller takes it out of the world
        return false;
    }
    g_b.comp = comp;  // from here on it is ours (it dies with the body)
    const std::uintptr_t mover = Q(comp + kCapMover);
    std::uint8_t in = 0;
    Rd(comp + kCapInWorld, in);
    const std::uintptr_t want_vt = g_base + (g_type == 1 ? kMoverProxyVt : kMoverRigidVt);
    if (!mover || Q(mover) != want_vt) {
        Log("[coll] attach: no %s mover after Configure (mover %p)", g_type == 1 ? "proxy" : "rigid body", reinterpret_cast<void*>(mover));
        return false;
    }
    if (!CallPlace(comp, &wt)) {
        Log("[coll] attach: SetTransform faulted");
        return false;
    }
    float radius = 0, height = 0;
    Rd(cfg.mover_res + kMoverResRadius, radius);
    Rd(cfg.mover_res + kMoverResStanding, height);
    std::int32_t live_layer = -1;
    Rd(comp + kCapLayer, live_layer);
    Log("[coll] capsule attached to the partner body %p: component %p, %s mover %p on layer %d, group %d, radius %.2f m, height %.2f m, in world %u",
        reinterpret_cast<void*>(e), reinterpret_cast<void*>(comp), g_type == 1 ? "proxy" : "rigid-body", reinterpret_cast<void*>(mover),
        live_layer, kGroup, radius, static_cast<double>(height), in);
    return in == 1;
}

bool SetInWorld(bool on, const game::WorldTransform& wt) {
    if (on == !g_b.disabled) return true;
    if (!on) {
        CallPlace(g_b.comp, &wt);  // the write-back on removal then changes nothing
        if (!CallComp(kDisable, g_b.comp)) return false;
        g_b.disabled = true;
        ++g_disables;
    } else {
        if (!CallComp(kEnable, g_b.comp)) return false;
        g_b.disabled = false;
        ++g_enables;
        CallPlace(g_b.comp, &wt);
    }
    return true;
}

}  // namespace

void Init(const Config& config) noexcept {
    g_cfg = config;
    g_on = config.partner_collision;
    g_layer = config.partner_collision_layer;
    g_height = config.partner_collision_height;
    if (!config.partner_collision) Log("[coll] player collision off (partner_collision=0)");
    else
        Log("[coll] player collision on: the partner's real body gets the game's character capsule (layer %d); the hologram and "
            "the self view never do; capsule height %.2f m",
            g_layer, static_cast<double>(g_height));
}

namespace {
// The body's place as the vehicle rules take it.
const double (&BodyPoint(const game::WorldTransform& w))[3] {
    static double p[3];
    p[0] = w.pos.x, p[1] = w.pos.y, p[2] = w.pos.z;
    return p;
}
}  // namespace

void Tick(game::Entity* local, bool local_active, game::Entity* body_e, bool partner_in_vehicle, double remote_age_ms, double now) noexcept {
    const std::uintptr_t body = reinterpret_cast<std::uintptr_t>(body_e);
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(local);
    // The body changed (removed, respawned, another character): the old capsule went with it.
    if (g_b.e && body != g_b.e) Forget(body ? "another body" : "no body");
    game::Guid guid;
    if (body && !partner::ReadGuid(body_e, &guid)) {
        // Not forgotten: a later good read of the same body must not attach a second capsule.
        if (g_b.comp && !g_b.disabled && g_b.e == body && g_checked > 0 && CompValid(g_b.e, g_b.comp) && CallComp(kDisable, g_b.comp)) {
            g_b.disabled = true;
            ++g_disables;
        }
        return SetState("off: the partner body is unreadable", 1e9);
    }
    if (body && g_b.e == body && guid != g_b.guid) Forget("a new entity at the same address");
    if (!body) return SetState(g_on ? "off: no real partner body" : "off", 1e9);
    if (!g_b.e) {
        g_b.e = body;
        g_b.guid = guid;
    }
    if (g_b.comp && !CompValid(g_b.e, g_b.comp)) {
        Log("[coll] the capsule %p is no longer one of the body's components: forgotten", reinterpret_cast<void*>(g_b.comp));
        g_b.comp = 0;
        g_b.failed = true;
    }

    // Positions (fault-free).
    game::WorldTransform bw{}, sw{};
    const bool bw_ok = ReadWt(body, &bw);
    const bool sw_ok = sam && ReadWt(sam, &sw);
    double d = 1e9, dz = 0;
    if (bw_ok && sw_ok) {
        const double dx = bw.pos.x - sw.pos.x, dy = bw.pos.y - sw.pos.y;
        d = std::sqrt(dx * dx + dy * dy);
        dz = bw.pos.z - sw.pos.z;
    }
    if (d < kNearM && std::fabs(dz) < 3.0) g_b.close_by = true;
    else if (d > kFarM || std::fabs(dz) >= 3.0) g_b.close_by = false;
    if (bw_ok) {
        if (g_b.have_prev && now > g_b.prev_ms) {
            const double mx = bw.pos.x - g_b.prev[0], my = bw.pos.y - g_b.prev[1], mz = bw.pos.z - g_b.prev[2];
            const double step = std::sqrt(mx * mx + my * my + mz * mz);
            if (step > kJumpM) g_b.hold_until_ms = now + 500;
            const double inst = step * 1000.0 / (now - g_b.prev_ms);
            g_b.speed = g_b.speed * 0.8 + inst * 0.2;
        }
        g_b.prev[0] = bw.pos.x, g_b.prev[1] = bw.pos.y, g_b.prev[2] = bw.pos.z;
        g_b.prev_ms = now;
        g_b.have_prev = true;
    }

    // Gates.
    const char* why = nullptr;
    static double next_seq_ms = 0;
    static const char* seq_block = nullptr;
    std::uint64_t bf = 0;
    if (!g_on) why = "off";
    else if (!Resolve()) why = "off: byte checks failed";
    else if (!sam || !local_active) why = "off: no local player awake";
    else if (!bw_ok || !sw_ok) why = "off: positions unreadable";
    else if (!Rd(body + kEntFlags, bf) || !(bf & kFlagVisible) || (bf & kFlagAsleep)) why = "off: the partner body is hidden or asleep";
    else if (remote_age_ms > kStaleMs) why = "off: no fresh partner position";
    else if (partner_in_vehicle) why = "off: the partner is in a vehicle";
    else if (g_b.speed > kFastMps) why = "off: the partner moves fast (vehicle?)";
    else if (now < g_b.hold_until_ms) why = "off: the partner jumped (snap or teleport)";
    // M4.4 (partner_tackle.hpp): a vehicle moving over 1 m/s within 5 m of the body (an NPC truck, a rolling vehicle, the
    // partner's just after they got off) meets no invisible pillar
    else if (const char* vb = vhit::CapsuleVehicleBlock(BodyPoint(bw), false)) why = vb;
    else if (-dz > kSamAboveM) why = "off: Sam is above the partner (over 1.6 m)";
    else if (!SamCapsuleInWorld(SamCapsule(sam))) why = "off: Sam's own capsule is not in the world (ladder, zipline, ...)";
    else {
        const bool seq = now >= next_seq_ms;
        if (seq) next_seq_ms = now + 250;
        const char* b2 = game::LocalCollisionBlock(seq);
        if (seq || b2) seq_block = b2;  // the sequence answer holds for 250 ms
        if (seq_block) why = seq_block;
    }
    if (!why && g_b.failed) why = "off: the capsule failed on this body";
    if (!why && !g_b.close_by) why = "off: the partner is not near Sam (over 4 m)";
    const bool overlap = d < kOverlapM && std::fabs(dz) < kOverlapDzM;
    const bool separated = d > kSeparatedM || std::fabs(dz) > 2.0;
    bool want = why == nullptr;
    if (want && overlap) {
        want = false;
        why = "off: overlapping Sam (waits until 0.9 m apart)";
    }

    // Attach once (only when it can go straight into the world, apart from Sam and every vehicle).
    if (want && !g_b.comp && !g_b.failed) {
        if (const char* vb = vhit::CapsuleVehicleBlock(BodyPoint(bw), true)) {
            want = false;
            why = vb;
        } else if (!separated) {
            want = false;
            why = "waiting: Sam too close to attach";
        } else if (g_attach_failures >= 3) {
            want = false;
            why = "off: attaching failed 3 times this session";
        } else if (!Attach(body, sam, bw)) {
            g_b.failed = true;
            ++g_attach_failures;
            why = "off: attaching the capsule failed";
            want = false;
            if (g_b.comp && !g_b.disabled) {  // configured but not usable: out of the world
                CallComp(kDisable, g_b.comp);
                g_b.disabled = true;
            }
        }
    }
    if (g_b.comp) {
        if (!want) {
            if (!g_b.disabled && !SetInWorld(false, bw)) Log("[coll] Capsule::Disable faulted");
        } else if (g_b.disabled) {
            const char* vb = vhit::CapsuleVehicleBlock(BodyPoint(bw), true);  // M4.4: never back into a vehicle
            if (vb) {
                why = vb;
                want = false;
            } else if (separated) {
                if (!SetInWorld(true, bw)) {
                    Log("[coll] Capsule::Enable faulted: collision off for this body");
                    g_b.failed = true;
                }
            } else {
                why = "waiting: Sam too close";
                want = false;
            }
        }
        if (want && !g_b.disabled) {
            if (!CallPlace(g_b.comp, &bw)) {
                Log("[coll] Capsule SetTransform faulted: collision off for this body");
                CallComp(kDisable, g_b.comp);
                g_b.disabled = true;
                g_b.failed = true;
                want = false;
                why = "off: SetTransform faulted";
            }
        }
    }
    const bool on = g_b.comp && !g_b.disabled && want;
    SetState(on ? "on: the partner blocks Sam" : (why ? why : "off"), d);

    // Measurements.
    ++g_ticks;
    if (on) ++g_ticks_on;
    if (d < g_min_d) g_min_d = d;
    double sam_step = 0;
    if (sw_ok) {
        if (g_sam_prev_ok) sam_step = std::hypot(sw.pos.x - g_sam_prev[0], sw.pos.y - g_sam_prev[1]);
        g_sam_prev[0] = sw.pos.x, g_sam_prev[1] = sw.pos.y;
        g_sam_prev_ok = true;
    }
    if (g_trace_request_ms != 0) {
        g_trace_until_ms = g_trace_request_ms > 0 ? now + g_trace_request_ms : 0;
        g_trace_request_ms = 0;
    }
    if (now < g_trace_until_ms)
        Log("[coll] trace d=%.3f dz=%.2f sam=(%.3f, %.3f, z %.3f) step=%.4f body=(%.3f, %.3f, z %.3f) speed=%.2f %s", d, dz, sw.pos.x,
            sw.pos.y, sw.pos.z, sam_step, bw.pos.x, bw.pos.y, bw.pos.z, g_b.speed, on ? "on" : "off");
    if (now >= g_next_stats_ms) {
        if (g_next_stats_ms > 0 && g_min_d < 5.0)
            Log("[coll] 1 s: min distance %.3f m, capsule on %u of %u ticks, enables %u, disables %u, partner speed %.2f m/s", g_min_d,
                g_ticks_on, g_ticks, g_enables, g_disables, g_b.speed);
        g_next_stats_ms = now + 1000;
        g_min_d = 1e9;
        g_ticks = g_ticks_on = 0;
    }
}

void BodyGoing(game::Entity* body) noexcept {
    const std::uintptr_t e = reinterpret_cast<std::uintptr_t>(body);
    if (!e || e != g_b.e || !g_b.comp || g_b.disabled || g_checked <= 0) return;
    game::Guid guid;
    if (!partner::ReadGuid(body, &guid) || guid != g_b.guid || !CompValid(e, g_b.comp)) return;
    if (CallComp(kDisable, g_b.comp)) {
        g_b.disabled = true;
        ++g_disables;
        Log("[coll] the partner body is being removed: its capsule is out of the world first");
    }
}

void EmergencyStop() noexcept {
    if (g_b.comp && !g_b.disabled && g_checked > 0 && CompValid(g_b.e, g_b.comp)) {
        CallComp(kDisable, g_b.comp);
        g_b.disabled = true;
        Log("[coll] emergency stop: the partner's capsule is out of the world");
    }
    g_on = false;
}

}  // namespace st::collision

namespace st::game {

// action collision <op> [a] [b]: 0 log, 1 <0/1> off/on, 2 <ms> per-tick distance trace,
// 3 <m> pin the partner m ahead of Sam (0 = follow again), 4 <layer> <type> reconfigure, 5 <height m> the capsule's height,
// 6 <m> the pin of 3 with the partner's back to Sam.
bool CollisionCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    using namespace st::collision;
    char text[160] = {};
    switch (op) {
        case 0: {
            Entity* local = nullptr;
            TryGetLocalPlayer(&local);
            // collision::Resolve, not game::Resolve (which the using-directive would not hide).
            if (collision::Resolve() && local) LogSelfChecks(reinterpret_cast<std::uintptr_t>(local));
            std::int32_t dis = 0;
            std::uint8_t in = 0;
            if (g_b.comp) Rd(g_b.comp + kCapDisable, dis), Rd(g_b.comp + kCapInWorld, in);
            Log("[coll] state: %s; body %p capsule %p (in world %u, disable counter %d, ours %s), layer %d type %d", g_state,
                reinterpret_cast<void*>(g_b.e), reinterpret_cast<void*>(g_b.comp), in, dis, g_b.disabled ? "disabled" : "enabled", g_layer,
                g_type);
            std::snprintf(text, sizeof(text), "collision: %s", g_state);
            break;
        }
        case 1:
            g_on = v[1] != 0;
            Log("[coll] %s by debug action", g_on ? "on" : "off");
            std::snprintf(text, sizeof(text), "collision %s", g_on ? "on" : "off");
            break;
        case 2:
            // Started by the next tick, on the tick's own clock.
            g_trace_request_ms = v[1] > 0 ? v[1] : -1;
            std::snprintf(text, sizeof(text), "collision trace %.0f ms", v[1]);
            break;
        case 3:
            return sync::DebugPinPartner(v[1], v[2], msg, cap);
        case 6:
            // the same pin, its back to Sam (hidden tests: the partner's back cargo close to the camera)
            return sync::DebugPinPartner(v[1], v[2], msg, cap, true);
        case 4: {
            const int layer = static_cast<int>(v[1]), type = static_cast<int>(v[2]);
            if ((layer != 9 && layer != 10 && layer != 25) || (type != 0 && type != 1)) {
                if (msg && cap) std::snprintf(msg, cap, "collision 4 <layer 25|10|9> <type 0|1>");
                return false;
            }
            g_layer = layer, g_type = type;
            g_layer_picked = layer;
            if (g_b.comp && g_checked > 0 && CompValid(g_b.e, g_b.comp)) {
                Cfg cfg{};
                cfg.mover_res = Q(g_b.comp + kCapMoverRes);
                cfg.type = type, cfg.layer = layer, cfg.group = kGroup, cfg.mask = 0x1F, cfg.max_slope = kMaxSlope;
                const bool ok = CallConfigure(g_b.comp, &cfg);
                Log("[coll] reconfigured: layer %d type %d (%s)", layer, type, ok ? "ok" : "faulted");
            }
            std::snprintf(text, sizeof(text), "collision layer %d type %d", layer, type);
            break;
        }
        case 5: {
            // Test op: the capsule's standing height in m (0 = Sam's own resource), applied now.
            const float h = static_cast<float>(v[1]);
            if (!(h == 0.0f || (h >= 1.0f && h <= 6.0f))) {
                if (msg && cap) std::snprintf(msg, cap, "collision 5 <height m 1..6, 0 = Sam's own>");
                return false;
            }
            g_height = h;
            Entity* local = nullptr;
            TryGetLocalPlayer(&local);
            const std::uintptr_t sam_cap = local ? SamCapsule(reinterpret_cast<std::uintptr_t>(local)) : 0;
            const std::uintptr_t sam_res = sam_cap ? Q(sam_cap + kCapMoverRes) : 0;
            const std::uintptr_t res = TallResource(sam_res, h);
            if (g_b.comp && res && g_checked > 0 && CompValid(g_b.e, g_b.comp)) {
                Cfg cfg{};
                cfg.mover_res = res;
                cfg.type = g_type, cfg.layer = PickLayer(), cfg.group = kGroup, cfg.mask = 0x1F, cfg.max_slope = kMaxSlope;
                const bool ok = CallConfigure(g_b.comp, &cfg);
                float sh = 0;
                Rd(res + kMoverResStanding, sh);
                Log("[coll] reconfigured: standing height %.2f m (%s)", static_cast<double>(sh), ok ? "ok" : "faulted");
            }
            std::snprintf(text, sizeof(text), "collision height %.2f m", static_cast<double>(h));
            break;
        }
        default:
            if (msg && cap) std::snprintf(msg, cap, "collision: 0 log, 1 off/on, 2 trace ms, 3 pin m, 4 layer type, 5 height m, 6 pin m (its back to Sam)");
            return false;
    }
    if (msg && cap) std::snprintf(msg, cap, "%s", text);
    return true;
}

}  // namespace st::game
