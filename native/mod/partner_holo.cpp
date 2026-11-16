#include "partner_holo.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "log.hpp"
#include "partner_source.hpp"

namespace st::holo {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// Evidence for each: docs/partner-hologram.md. Everything is byte-checked in Resolve().
constexpr std::uintptr_t kCreateSpawner = 0x2380870;    // (void** out, const WorldTransform*, StreamingRef*, RTTIRefObject*, const GGUUID*)
constexpr std::uintptr_t kSpawnerCtorVtLea = 0x236CFCE;  // lea rax,[vtable] in the spawner ctor 0x14236CFC0
constexpr std::uintptr_t kSpawnerVtable = 0x3D07FA8;
constexpr std::uintptr_t kStartSpawner = 0x237DF80;     // (spawner, Entity* parent = 0)
constexpr std::uintptr_t kRealize = 0x2375BA0;          // Entity* (spawner): world attach on first call
constexpr std::uintptr_t kCancel = 0x2372CB0;           // (spawner): not loaded yet
constexpr std::uintptr_t kRemoveEntity = 0x235B8F0;     // (Entity*): world->vt[0x20](world, e, 0, 0)
constexpr std::uintptr_t kWorldGlobal = 0x7BC4E70;
constexpr std::uintptr_t kStartVisible = 0x2B9E450;     // DSWelcomeHoloComponent_sExportedStartVisibleWelcomeHolo: jmp 0x142B9EC10
constexpr std::uintptr_t kStartVisibleBody = 0x2B9EC10;
constexpr std::uintptr_t kStopVisible = 0x2B9E400;      // the inverse export
constexpr std::uintptr_t kDbGetter = 0x345DE20;         // mov rax,[rip+db]; ret
constexpr std::uintptr_t kDbGlobal = 0x7C0E488;         // collectors-hologram DB (0x14345C1F0 creates, 0x14345CD10 frees)
constexpr std::uintptr_t kCpmLoad = 0x2765D64;          // mov rax,[rip+cpm]; mov rcx,[rax+0x56A7A8]
constexpr std::uintptr_t kCpmGlobal = 0x7BF4960;        // construction-point manager
constexpr std::uintptr_t kCpmSpawnerUse = 0x2C870A4;    // mov rcx,[rsi+0x56A7F0]
constexpr std::uintptr_t kCpmItemStore = 0x2C873EB;     // mov [rsi+0x56A7FC],eax; mov [rsi+0x56A808],rbx
constexpr std::uintptr_t kCpmHoloSpawner = 0x56A7F0;
constexpr std::uintptr_t kCpmHoloItem = 0x56A808;
constexpr std::uintptr_t kGameStateLoad = 0x2B9EC41;    // mov rcx,[rip+gs]; mov rax,[rcx+8]; test rax,0xA6240
constexpr std::uintptr_t kGameStateGlobal = 0x7BF6E88;
constexpr std::uintptr_t kPlayerWakeOverride = 0x2757500;  // DSPlayerEntity vt+0xA8

constexpr std::uintptr_t kItemRtti = 0x4DF3B80;  // DSCollectorsItemResource (size 0xA0)
constexpr std::size_t kItemSize = 0xA0;
constexpr std::size_t kItemDisplayNo = 0x20;
constexpr std::size_t kItemHashId = 0x70;
constexpr std::size_t kItemNameHash = 0x74;
constexpr std::size_t kItemDisplayRef = 0x80;  // StreamingRef<EntityResource>: pointer to {obj, flags}
constexpr std::size_t kItemType = 0x90;        // EDSCollectorsItemType (u8)
constexpr std::size_t kItemHidden = 0x93;
constexpr std::size_t kItemSortIndex = 0x9C;
constexpr std::uint8_t kTypeRewardHolo = 4;

constexpr std::uintptr_t kSpRefCount = 0x18;
constexpr std::uintptr_t kSpLoaded = 0x168;
constexpr std::uintptr_t kSpCancelled = 0x169;

constexpr std::uintptr_t kEntFlags = 0x88;
constexpr std::uintptr_t kEntModel = 0xB8;
constexpr std::uintptr_t kEnt180 = 0x180;

// DSWelcomeHoloComponent (size 0x140)
constexpr std::uintptr_t kWhRes = 0x30;
constexpr std::uintptr_t kWhAi = 0x50;  // AIIndividualComponent* (set in init 0x142B88340); update reads [ai+0x3690]
constexpr std::uintptr_t kAiAgent = 0x3690;
constexpr std::uintptr_t kWhState = 0x58;
constexpr std::uintptr_t kWhRequested = 0x5C;
constexpr std::uintptr_t kWhFlag12A = 0x12A;
constexpr std::uintptr_t kWhShown = 0x12B;
constexpr std::uintptr_t kWhFlag12D = 0x12D;

constexpr double kLoadTimeoutMs = 15000;
constexpr unsigned kMaxTimeoutsPerItem = 2;

// ---------------------------------------------------------------- resolution
struct Fns {
    bool resolved = false;
    bool ok = false;
    char why[160] = "not resolved";
    std::uintptr_t create = 0, start = 0, realize = 0, cancel = 0, remove = 0, start_visible = 0, stop_visible = 0;
    std::uintptr_t db_global = 0, spawner_vt = 0;
    std::uintptr_t cpm_global = 0;  // optional: "the game's current structure hologram"
    std::uintptr_t gs_global = 0;   // optional: logging only
} g_fn;

bool Check(const char* name, std::uintptr_t va, const char* sig) {
    if (mem::CodeMatches(va, sig)) return true;
    std::snprintf(g_fn.why, sizeof(g_fn.why), "%s at ds.exe+0x%llX does not match", name,
                  static_cast<unsigned long long>(va - mem::Base()));
    return false;
}

bool Resolve() {
    if (g_fn.resolved) return g_fn.ok;
    g_fn.resolved = true;
    const std::uintptr_t b = mem::Base();
    bool ok =
        Check("CreateSpawner", b + kCreateSpawner,
              "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 33 C0 48 8B D9 48 89 01 49 8B F1 "
              "B9 F0 01 00 00") &&
        Check("Spawner ctor", b + kSpawnerCtorVtLea, "48 8D 05 ?? ?? ?? ?? 44 89 71 18") &&
        Check("StartSpawner", b + kStartSpawner,
              "4C 8B DC 53 41 56 48 83 EC 58 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 38 80 B9 6A 01 00 00 00") &&
        Check("Realize", b + kRealize, "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 8B 70 01 00 00 33 D2 48 85 C9 48 8D 41 E0") &&
        Check("CancelSpawner", b + kCancel, "40 57 48 83 EC 30 80 B9 69 01 00 00 00 48 8B F9 0F 85") &&
        Check("RemoveEntity", b + kRemoveEntity, "48 8B D1 45 33 C9 48 8B 0D ?? ?? ?? ?? 45 33 C0 48 8B 01 48 FF 60 20") &&
        Check("StartVisibleWelcomeHolo", b + kStartVisible, "E9 ?? ?? ?? ??") &&
        Check("StartVisibleWelcomeHolo body", b + kStartVisibleBody,
              "48 85 C9 0F 84 91 00 00 00 57 48 83 EC 20 48 8B F9 48 89 5C 24 38 48 81 C1 90 00 00 00 48 8D 15 ?? ?? ?? ?? E8") &&
        Check("StopVisibleWelcomeHolo", b + kStopVisible, "48 83 EC 28 48 85 C9 74 2F 48 81 C1 90 00 00 00 48 8D 15 ?? ?? ?? ?? E8") &&
        Check("CollectorsDB getter", b + kDbGetter, "48 8B 05 ?? ?? ?? ?? C3");
    if (ok) {
        struct {
            const char* what;
            std::uintptr_t got, want;
        } targets[] = {
            {"spawner vtable", mem::RipTarget(b + kSpawnerCtorVtLea, 3, 7), b + kSpawnerVtable},
            {"world global", mem::RipTarget(b + kRemoveEntity + 6, 3, 7), b + kWorldGlobal},
            {"StartVisible jump", mem::RipTarget(b + kStartVisible, 1, 5), b + kStartVisibleBody},
            {"collectors DB global", mem::RipTarget(b + kDbGetter, 3, 7), b + kDbGlobal},
        };
        for (const auto& t : targets) {
            if (t.got != t.want) {
                std::snprintf(g_fn.why, sizeof(g_fn.why), "%s decodes to ds.exe+0x%llX, expected +0x%llX", t.what,
                              static_cast<unsigned long long>(t.got - b), static_cast<unsigned long long>(t.want - b));
                ok = false;
                break;
            }
        }
    }
    if (ok && !mem::InText(Q(b + kSpawnerVtable))) {
        std::snprintf(g_fn.why, sizeof(g_fn.why), "spawner vtable slot 0 is not code");
        ok = false;
    }
    if (ok) {
        g_fn.create = b + kCreateSpawner;
        g_fn.start = b + kStartSpawner;
        g_fn.realize = b + kRealize;
        g_fn.cancel = b + kCancel;
        g_fn.remove = b + kRemoveEntity;
        g_fn.start_visible = b + kStartVisible;
        g_fn.stop_visible = b + kStopVisible;
        g_fn.db_global = b + kDbGlobal;
        g_fn.spawner_vt = b + kSpawnerVtable;
        // Optional extras: a mismatch only loses a feature.
        if (mem::CodeMatches(b + kCpmLoad, "48 8B 05 ?? ?? ?? ?? 48 8B 88 A8 A7 56 00") &&
            mem::RipTarget(b + kCpmLoad, 3, 7) == b + kCpmGlobal && mem::CodeMatches(b + kCpmSpawnerUse, "48 8B 8E F0 A7 56 00") &&
            mem::CodeMatches(b + kCpmItemStore, "89 86 FC A7 56 00 48 89 9E 08 A8 56 00"))
            g_fn.cpm_global = b + kCpmGlobal;
        // Required: StartVisible's body reads [[gs]+8] with no null check (0x142B9EC41), so the
        // mod must see the same global to check it is non-null before every call.
        if (mem::CodeMatches(b + kGameStateLoad, "48 8B 0D ?? ?? ?? ?? 48 8B 41 08 48 A9 40 62 0A 00") &&
            mem::RipTarget(b + kGameStateLoad, 3, 7) == b + kGameStateGlobal)
            g_fn.gs_global = b + kGameStateGlobal;
        else
            ok = false;
    }
    if (!ok && g_fn.create && !g_fn.gs_global) {
        std::snprintf(g_fn.why, sizeof(g_fn.why), "game-state load in StartVisible (ds.exe+0x%llX) does not match",
                      static_cast<unsigned long long>(kGameStateLoad));
    }
    if (ok) {
        std::snprintf(g_fn.why, sizeof(g_fn.why), "ok");
        Log("[holo] hologram functions verified (spawner ds.exe+0x%llX, StartVisible +0x%llX, DB +0x%llX)%s",
            static_cast<unsigned long long>(kCreateSpawner), static_cast<unsigned long long>(kStartVisible),
            static_cast<unsigned long long>(kDbGlobal), g_fn.cpm_global ? "" : "; structure-hologram lookup unavailable");
    } else {
        Log("[holo] hologram body unavailable: %s", g_fn.why);
    }
    g_fn.ok = ok;
    return ok;
}

void Disable(const char* what) {
    g_fn.ok = false;
    std::snprintf(g_fn.why, sizeof(g_fn.why), "%s faulted; hologram disabled for this run", what);
    Log("[holo] %s", g_fn.why);
}

// ---------------------------------------------------------------- SEH thunks (no unwinding objects)
using CreateFn = void** (*)(void** out, const void* xf, void* ref, void* opt, const void* guid);
using SpawnerFn = void (*)(void* sp, void* parent);
using RealizeFn = void* (*)(void* sp);
using CancelFn = void (*)(void* sp);
using EntityFn = void (*)(void* e);
using DeleteFn = void (*)(void* obj, int flags);

const std::uint8_t kZeroGuid[16] = {};

bool CallCreate(std::uintptr_t fn, void** out, const game::WorldTransform* xf, std::uintptr_t ref) {
    __try {
        reinterpret_cast<CreateFn>(fn)(out, xf, reinterpret_cast<void*>(ref), nullptr, kZeroGuid);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSpawner(std::uintptr_t fn, void* sp) {
    __try {
        reinterpret_cast<SpawnerFn>(fn)(sp, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallRealize(std::uintptr_t fn, void* sp, void** out) {
    __try {
        *out = reinterpret_cast<RealizeFn>(fn)(sp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallOne(std::uintptr_t fn, void* arg) {
    __try {
        reinterpret_cast<EntityFn>(fn)(arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game's inline release (0x142CA1CB2): lock xadd [sp+0x18],-1; on 1 -> vt[0](sp, 1).
bool CallRelease(void* sp, bool* destroyed) {
    __try {
        auto* rc = reinterpret_cast<volatile LONG*>(static_cast<std::uint8_t*>(sp) + kSpRefCount);
        const LONG prev = InterlockedExchangeAdd(rc, -1);
        *destroyed = prev == 1;
        if (prev == 1) {
            const auto vt = *reinterpret_cast<std::uintptr_t*>(sp);
            (*reinterpret_cast<DeleteFn*>(vt))(sp, 1);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- pre-call checks
// Everything a call dereferences without its own null check must be checked by
// us first: DS's crash reporter fires on any access violation, caught or not.

// RemoveEntity (0x14235B8F0) is `world->vt[0x20](world, e, 0, 0)` with
// world = [0x147BC4E70] and no null check.
bool CanRemove(const char** why) {
    const std::uintptr_t world = Q(mem::Base() + kWorldGlobal);
    const std::uintptr_t vt = world ? Q(world) : 0;
    if (!world) *why = "world global is null";
    else if (!mem::InImage(vt)) *why = "world vtable not in ds.exe";
    else if (!mem::InText(Q(vt + 0x20))) *why = "world vt+0x20 is not code";
    else return true;
    return false;
}

// StartVisible (0x142B9EC10) reads [[gs]+8] (0x142B9EC41), [comp+0x30]+0x41 and calls
// e->vt[0xA8] (Wake), all unchecked. Returns nullptr when every one of them is safe.
const char* StartVisibleBlocker(std::uintptr_t e) {
    const std::uintptr_t gs = g_fn.gs_global ? Q(g_fn.gs_global) : 0;
    std::uint64_t g = 0;
    if (!gs || !Rd(gs + 8, g)) return "game-state object not available";
    const std::uintptr_t vt = Q(e);
    if (!mem::InImage(vt)) return "entity vtable not in ds.exe";
    const std::uintptr_t wake = Q(vt + 0xA8);
    if (!mem::InText(wake) || wake == mem::Base() + kPlayerWakeOverride) return "entity Wake (vt+0xA8) unusable";
    const std::uintptr_t comp = partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), "DSWelcomeHoloComponent");
    if (!comp) return "no DSWelcomeHoloComponent";
    std::uint8_t r41 = 0;
    const std::uintptr_t res = Q(comp + kWhRes);
    if (!res || !Rd(res + 0x41, r41)) return "welcome-holo resource (+0x30) unreadable";
    return nullptr;
}

// ---------------------------------------------------------------- collectors DB
struct Item {
    std::uintptr_t ptr = 0;
    std::uint32_t display_no = 0, hash = 0, name_hash = 0;
    std::int32_t sort = 0;
    std::uint8_t type = 0;
    bool hidden = false;
    bool empty = true, loaded = false;
    std::uintptr_t res = 0;  // loaded EntityResource
};

bool RefState(std::uintptr_t field, bool* empty, bool* loaded, std::uintptr_t* res) {
    // StreamingRef: field holds d -> {obj, flags}. Empty test = 0x141DE1350,
    // loaded test + resource = 0x141DDE990.
    *empty = true;
    *loaded = false;
    *res = 0;
    const std::uintptr_t d = Q(field);
    if (!d) return true;
    std::uintptr_t blk[2] = {};
    if (!mem::Read(d, blk, sizeof(blk))) return false;
    const std::uintptr_t obj = blk[0];
    if (!obj) return true;
    std::uintptr_t id[2] = {};
    if (!mem::Read(obj + 0x10, id, sizeof(id))) return false;
    *empty = id[0] == 0 && id[1] == 0;
    *loaded = static_cast<std::int8_t>(static_cast<std::uint8_t>(blk[1] >> 52)) < 0;
    if (*loaded) *res = Q(obj + 0x20);
    return true;
}

// Re-read every frame it is needed: item pointers are never kept across frames
// (the DB can be freed by 0x14345CD10 at any load).
bool ReadDb(std::uintptr_t* db_out, std::vector<Item>* items) {
    items->clear();
    const std::uintptr_t db = Q(g_fn.db_global);
    *db_out = db;
    if (!db) return false;
    std::int32_t n = 0;
    if (!Rd(db + 0x10, n) || n <= 0 || n > 4096) return false;
    const std::uintptr_t arr = Q(db + 0x18);
    std::vector<std::uintptr_t> ptrs(static_cast<std::size_t>(n));
    if (!arr || !mem::Read(arr, ptrs.data(), ptrs.size() * sizeof(std::uintptr_t))) return false;
    const std::uintptr_t rtti = mem::Base() + kItemRtti;
    for (const std::uintptr_t p : ptrs) {
        if (!p || mem::RttiOf(p) != rtti) continue;
        std::uint8_t raw[kItemSize];
        if (!mem::Read(p, raw, sizeof(raw))) continue;
        Item it;
        it.ptr = p;
        std::memcpy(&it.display_no, raw + kItemDisplayNo, 4);
        std::memcpy(&it.hash, raw + kItemHashId, 4);
        std::memcpy(&it.name_hash, raw + kItemNameHash, 4);
        std::memcpy(&it.sort, raw + kItemSortIndex, 4);
        it.type = raw[kItemType];
        it.hidden = raw[kItemHidden] != 0;
        RefState(p + kItemDisplayRef, &it.empty, &it.loaded, &it.res);
        items->push_back(it);
    }
    return !items->empty();
}

std::uintptr_t g_logged_db = 0;
std::size_t g_logged_count = 0;

void LogTable(std::uintptr_t db, const std::vector<Item>& items) {
    if (db == g_logged_db && items.size() == g_logged_count) return;
    g_logged_db = db;
    g_logged_count = items.size();
    std::size_t holos = 0, refs = 0;
    for (const Item& it : items) {
        if (it.type & kTypeRewardHolo) ++holos;
        if (!it.empty) ++refs;
    }
    Log("[holo] collectors DB %p: %zu items, %zu with Type&4 (Reward_Holo), %zu with a display entity. "
        "Columns: HashId NameHash Type DisplayNo ref(-=none, s=streamed out, L=loaded)",
        reinterpret_cast<void*>(db), items.size(), holos, refs);
    std::string line;
    unsigned col = 0;
    for (const Item& it : items) {
        char cell[96];
        if (it.loaded) {
            const char* rn = partner::TypeName(reinterpret_cast<const void*>(it.res));
            std::snprintf(cell, sizeof(cell), "%08X %08X t%u #%u L:%s%s", it.hash, it.name_hash, it.type, it.display_no,
                          rn ? rn : "?", Q(it.res + 0x58) ? "+model" : "");
        } else {
            std::snprintf(cell, sizeof(cell), "%08X %08X t%u #%u %s%s", it.hash, it.name_hash, it.type, it.display_no,
                          it.empty ? "-" : "s", it.hidden ? " hidden" : "");
        }
        line += line.empty() ? "[holo]   " : " | ";
        line += cell;
        if (++col == 4) {
            Log("%s", line.c_str());
            line.clear();
            col = 0;
        }
    }
    if (!line.empty()) Log("%s", line.c_str());
}

// ---------------------------------------------------------------- state
Phase g_phase = Phase::idle;
void* g_sp = nullptr;          // our spawner reference (refcount we own: 1)
std::uintptr_t g_db = 0;       // DB the item came from
std::uint32_t g_hash = 0;
double g_started_ms = 0;
double g_next_progress_ms = 0;
bool g_realized = false;
game::Entity* g_entity = nullptr;
std::uintptr_t g_entity_vt = 0;
game::Guid g_entity_guid;
double g_next_maintain_ms = 0;
std::int32_t g_last_state = -1;
int g_last_visible = -1;
unsigned g_reasserts = 0;
bool g_user_hidden = false;

std::vector<std::uint32_t> g_rejected;
struct Timeouts {
    std::uint32_t hash;
    unsigned count;
};
std::vector<Timeouts> g_timeouts;

bool Rejected(std::uint32_t h) {
    for (const std::uint32_t r : g_rejected)
        if (r == h) return true;
    return false;
}

void Reject(std::uint32_t h, const char* why) {
    if (!h || Rejected(h)) return;
    g_rejected.push_back(h);
    Log("[holo] item %08X will not be tried again this session (%s)", h, why);
}

std::uint64_t GameState() {
    if (!g_fn.gs_global) return 0;
    const std::uintptr_t gs = Q(g_fn.gs_global);
    return gs ? Q(gs + 8) : 0;
}

void Set(char* why, std::size_t cap, const char* fmt, ...) {
    if (!why || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}

int Choose(const std::vector<Item>& items, std::uint32_t want, const char** how) {
    if (want) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].hash != want) continue;
            if (items[i].empty) {
                Log("[holo] partner_holo=%08X has no display entity; choosing automatically", want);
                break;
            }
            if (Rejected(want)) break;
            *how = "session.ini partner_holo";
            return static_cast<int>(i);
        }
        static std::uint32_t warned = 0;
        if (warned != want) {
            warned = want;
            Log("[holo] partner_holo=%08X not usable in this DB; choosing automatically", want);
        }
    }
    if (g_fn.cpm_global) {
        const std::uintptr_t cpm = Q(g_fn.cpm_global);
        const std::uintptr_t sp = cpm ? Q(cpm + kCpmHoloSpawner) : 0;
        const std::uintptr_t item = sp ? Q(cpm + kCpmHoloItem) : 0;
        for (std::size_t i = 0; item && i < items.size(); ++i) {
            if (items[i].ptr == item && !items[i].empty && !Rejected(items[i].hash)) {
                *how = "the game's current structure hologram";
                return static_cast<int>(i);
            }
        }
    }
    int best = -1;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        if (!(it.type & kTypeRewardHolo) || it.empty || Rejected(it.hash)) continue;
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const Item& b = items[static_cast<std::size_t>(best)];
        const bool it_exact = it.type == kTypeRewardHolo, b_exact = b.type == kTypeRewardHolo;
        if ((it_exact && !b_exact) || (it_exact == b_exact && it.display_no < b.display_no)) best = static_cast<int>(i);
    }
    *how = "first Reward_Holo item (lowest DisplayNo)";
    return best;
}

bool EntityAlive() {
    if (!g_entity) return false;
    std::uintptr_t vt = 0;
    game::Guid g;
    return partner::ReadVtable(g_entity, &vt) && vt == g_entity_vt && partner::ReadGuid(g_entity, &g) &&
           g == g_entity_guid;
}

void ReleaseSpawner(const char* why) {
    if (!g_sp) return;
    const auto sp = reinterpret_cast<std::uintptr_t>(g_sp);
    std::int32_t rc = 0;
    if (Q(sp) != g_fn.spawner_vt || !Rd(sp + kSpRefCount, rc) || rc < 1) {
        // Not the object we created any more: leaking our reference is the only safe choice.
        Log("[holo] spawner %p no longer looks like ours (vtable %p, refcount %d); not releasing it (%s)", g_sp,
            reinterpret_cast<void*>(Q(sp)), rc, why);
    } else {
        bool destroyed = false;
        if (!CallRelease(g_sp, &destroyed)) Disable("spawner release");
        else Log("[holo] released spawner %p (refcount was %d%s)", g_sp, rc, destroyed ? ", destroyed" : "");
    }
    g_sp = nullptr;
}

void ResetState() {
    g_sp = nullptr;
    g_phase = Phase::idle;
    g_db = 0;
    g_hash = 0;
    g_realized = false;
    g_entity = nullptr;
    g_entity_vt = 0;
    g_entity_guid = {};
    g_last_state = -1;
    g_last_visible = -1;
    g_reasserts = 0;
}

// Everything torn down exactly once: entity removed (if we realized it, or the
// spawner finished loading), or the load cancelled; then our reference released.
bool Teardown(const char* why) {
    if (!g_sp) {
        ResetState();
        return false;
    }
    const auto sp = reinterpret_cast<std::uintptr_t>(g_sp);
    const char* step = "nothing to remove";
    const char* no_remove = nullptr;
    bool release = true;
    if (g_realized) {
        if (!EntityAlive()) {
            step = "entity already gone";
        } else if (!CanRemove(&no_remove)) {
            // The entity may still be referenced by the spawner: leak both rather than
            // free the spawner under a live entity.
            step = no_remove;
            release = false;
        } else {
            const bool ok = CallOne(g_fn.remove, g_entity);
            step = ok ? "entity removed" : "entity remove FAULTED";
            if (!ok) Disable("RemoveEntity");
        }
    } else {
        std::uint8_t loaded = 0;
        Rd(sp + kSpLoaded, loaded);
        if (loaded && !CanRemove(&no_remove)) {
            step = no_remove;  // cannot remove what realize would attach: leave it loaded, leak the spawner
            release = false;
        } else if (loaded) {
            void* e = nullptr;
            if (!CallRealize(g_fn.realize, g_sp, &e)) {
                Disable("Realize (teardown)");
                step = "realize FAULTED";
            } else if (e && mem::InImage(Q(reinterpret_cast<std::uintptr_t>(e)))) {
                const bool ok = CallOne(g_fn.remove, e);
                step = ok ? "loaded entity realized and removed" : "remove FAULTED";
                if (!ok) Disable("RemoveEntity");
            } else {
                step = "loaded but no entity";
            }
        } else {
            const bool ok = CallOne(g_fn.cancel, g_sp);
            step = ok ? "load cancelled" : "cancel FAULTED";
            if (!ok) Disable("CancelSpawner");
        }
    }
    Log("[holo] teardown (%s): hash=%08X spawner=%p entity=%p: %s%s", why, g_hash, g_sp, g_entity, step,
        release ? "" : " (not removed; spawner reference leaked)");
    if (release) ReleaseSpawner(why);
    ResetState();
    return true;
}

void LogComponent(std::uintptr_t comp, const char* what) {
    if (!comp) return;
    std::int32_t st = -1, req = -1;
    std::uint8_t f12a = 0, shown = 0, f12d = 0;
    Rd(comp + kWhState, st);
    Rd(comp + kWhRequested, req);
    Rd(comp + kWhFlag12A, f12a);
    Rd(comp + kWhShown, shown);
    Rd(comp + kWhFlag12D, f12d);
    const std::uint64_t g = GameState();
    Log("[holo] %s: DSWelcomeHoloComponent %p state=%d requested=%d +12A=%u shown(+12B)=%u +12D=%u; game state G=0x%llX "
        "(G&0xA6240=%s, bit56=%d)",
        what, reinterpret_cast<void*>(comp), st, req, f12a, shown, f12d, static_cast<unsigned long long>(g),
        (g & 0xA6240) ? "set: StartVisible is refused" : "clear", static_cast<int>((g >> 56) & 1));
}

void LogComponentResource(std::uintptr_t comp) {
    const std::uintptr_t res = Q(comp + kWhRes);
    std::uint8_t r[0x48] = {};
    if (!res || !mem::Read(res, r, sizeof(r))) {
        Log("[holo]   welcome-holo resource unreadable (%p)", reinterpret_cast<void*>(res));
        return;
    }
    float f[8] = {};
    std::memcpy(f, r + 0x20, sizeof(f));
    Log("[holo]   resource %p: WelcomeRange=%.1f ActionRange=%.1f Angle=%.1f Scale=%.2f FIFOSpeed=%.2f "
        "ForConstruction=%u ForRace=%u IsNotHuman=%u IsNotEchoTarget=%u AutoSpeak=%u IdleOnly=%u DontMoveWorldPosition=%u",
        reinterpret_cast<void*>(res), f[0], f[1], f[2], f[5], f[6], r[0x40], r[0x41], r[0x42], r[0x43], r[0x44], r[0x45],
        r[0x46]);
}

// Reads only. Returns the refusal reason, or nullptr when the entity is a safe
// hologram body (and *comp_out is its DSWelcomeHoloComponent).
const char* Validate(std::uintptr_t e, std::uintptr_t* comp_out) {
    *comp_out = 0;
    const std::uintptr_t vt = Q(e);
    if (!mem::InImage(vt)) return "entity vtable not in ds.exe";
    const std::uintptr_t rtti = mem::RttiOf(e);
    if (!rtti) return "entity type unknown";
    if (mem::DerivesFrom(rtti, "Humanoid") || mem::DerivesFrom(rtti, "DSPlayerEntity")) return "a Humanoid/player entity";
    if (Q(vt + 0xA8) == mem::Base() + kPlayerWakeOverride) return "uses the player's Wake override";
    if (!Q(e + kEntModel)) return "no model (entity +0xB8 null)";
    const std::uintptr_t comp = partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), "DSWelcomeHoloComponent");
    if (!comp) return "no DSWelcomeHoloComponent";
    *comp_out = comp;
    // DSWelcomeHoloComponent's update (0x142B8BDF0) dereferences these without checks.
    const std::uintptr_t ai = Q(comp + kWhAi);
    if (!ai) return "welcome-holo AIIndividualComponent link (+0x50) is null; its update would fault";
    if (!Q(ai + kAiAgent)) return "AIIndividualComponent +0x3690 is null; the welcome-holo update would fault";
    if (!Q(e + kEnt180)) return "entity +0x180 is null; the welcome-holo update would fault";
    return nullptr;
}

}  // namespace

// ================================================================ public
bool Available() { return Resolve(); }
const char* UnavailableReason() { return g_fn.why; }

Phase GetPhase() { return g_phase; }
const char* PhaseName() {
    switch (g_phase) {
        case Phase::loading: return "loading";
        case Phase::live: return "live";
        default: return "idle";
    }
}
std::uint32_t CurrentHash() { return g_hash; }

game::Entity* LiveEntity() { return g_phase == Phase::live && EntityAlive() ? g_entity : nullptr; }

bool DbChanged() { return g_phase != Phase::idle && g_fn.db_global && Q(g_fn.db_global) != g_db; }

void ResetRejected() {
    g_rejected.clear();
    g_timeouts.clear();
    g_logged_db = 0;
    g_logged_count = 0;
}

BeginResult Begin(const game::WorldTransform& at, std::uint32_t want_hash, double now_ms, char* why, std::size_t cap) {
    if (!Resolve()) {
        Set(why, cap, "%s", g_fn.why);
        return BeginResult::unavailable;
    }
    if (g_phase != Phase::idle) return BeginResult::started;
    std::uintptr_t db = 0;
    std::vector<Item> items;
    if (!ReadDb(&db, &items)) {
        Set(why, cap, db ? "collectors DB %p has no readable items yet" : "collectors DB not loaded yet",
            reinterpret_cast<void*>(db));
        return BeginResult::waiting_db;
    }
    LogTable(db, items);
    const char* how = "";
    const int idx = Choose(items, want_hash, &how);
    if (idx < 0) {
        Set(why, cap, "no usable hologram item in the collectors DB (%zu items, %zu refused)", items.size(), g_rejected.size());
        return BeginResult::no_item;
    }
    const Item& it = items[static_cast<std::size_t>(idx)];
    const std::uintptr_t ref = it.ptr + kItemDisplayRef;
    bool empty = true, loaded = false;
    std::uintptr_t res = 0;
    if (!RefState(ref, &empty, &loaded, &res) || empty) {
        Set(why, cap, "item %08X display ref changed under us", it.hash);
        return BeginResult::failed;
    }
    void* sp = nullptr;
    if (!CallCreate(g_fn.create, &sp, &at, ref)) {
        Disable("CreateSpawner");
        Set(why, cap, "%s", g_fn.why);
        return BeginResult::unavailable;
    }
    if (!sp) {
        Reject(it.hash, "the spawner could not be created");
        Set(why, cap, "CreateSpawner returned null for item %08X", it.hash);
        return BeginResult::failed;
    }
    const auto spa = reinterpret_cast<std::uintptr_t>(sp);
    std::int32_t rc = 0;
    Rd(spa + kSpRefCount, rc);
    if (Q(spa) != g_fn.spawner_vt) {
        Disable("spawner vtable check");
        Set(why, cap, "spawner %p has vtable %p, not ds.exe+0x%llX (left alone)", sp, reinterpret_cast<void*>(Q(spa)),
            static_cast<unsigned long long>(kSpawnerVtable));
        return BeginResult::unavailable;
    }
    g_sp = sp;
    g_db = db;
    g_hash = it.hash;
    g_started_ms = now_ms;
    g_next_progress_ms = now_ms + 5000;
    g_realized = false;
    g_phase = Phase::loading;
    if (!CallSpawner(g_fn.start, sp)) {
        Disable("StartSpawner");
        g_sp = nullptr;  // half-started: never touch it again (leak)
        ResetState();
        Set(why, cap, "%s", g_fn.why);
        return BeginResult::unavailable;
    }
    Log("[holo] spawning item %08X (NameHash %08X, Type %u, DisplayNo %u; %s; ref %s) at (%.2f, %.2f, %.2f): spawner=%p "
        "refcount=%d",
        it.hash, it.name_hash, it.type, it.display_no, how, loaded ? "already loaded" : "streaming", at.pos.x, at.pos.y,
        at.pos.z, sp, rc);
    return BeginResult::started;
}

PollResult Poll(double now_ms, char* why, std::size_t cap) {
    if (g_phase == Phase::idle) return PollResult::idle;
    if (g_phase == Phase::live) {
        if (EntityAlive()) return PollResult::live;
        Set(why, cap, "the hologram entity %p was destroyed by the game", g_entity);
        Log("[holo] %s", why);
        g_entity = nullptr;  // never remove what is gone; the spawner is still ours to release
        g_realized = true;
        Teardown("entity gone");
        return PollResult::failed;
    }
    const auto sp = reinterpret_cast<std::uintptr_t>(g_sp);
    std::uint8_t flags[2] = {};
    if (!mem::Read(sp + kSpLoaded, flags, sizeof(flags))) {
        Set(why, cap, "spawner %p unreadable", g_sp);
        Log("[holo] %s; dropping it", why);
        g_sp = nullptr;
        ResetState();
        return PollResult::failed;
    }
    if (flags[1]) {
        const std::uint32_t h = g_hash;
        Set(why, cap, "the game cancelled loading item %08X", h);
        Log("[holo] %s", why);
        Teardown("cancelled by the game");
        return PollResult::failed;
    }
    if (!flags[0]) {
        if (now_ms - g_started_ms > kLoadTimeoutMs) {
            const std::uint32_t h = g_hash;
            Set(why, cap, "item %08X did not load within %.0f s", h, kLoadTimeoutMs / 1000.0);
            Log("[holo] %s", why);
            Teardown("load timeout");
            bool found = false;
            for (auto& t : g_timeouts) {
                if (t.hash != h) continue;
                found = true;
                if (++t.count >= kMaxTimeoutsPerItem) Reject(h, "timed out twice");
            }
            if (!found) g_timeouts.push_back({h, 1});
            return PollResult::failed;
        }
        if (now_ms >= g_next_progress_ms) {
            g_next_progress_ms = now_ms + 5000;
            Log("[holo] still loading item %08X (%.1f s)", g_hash, (now_ms - g_started_ms) / 1000.0);
        }
        return PollResult::loading;
    }

    // Loaded: realize once (attaches it to the world), validate, show.
    void* ev = nullptr;
    const double took = now_ms - g_started_ms;
    if (!CallRealize(g_fn.realize, g_sp, &ev)) {
        Disable("Realize");
        g_sp = nullptr;  // state unknown after a fault: leak rather than touch it
        ResetState();
        Set(why, cap, "%s", g_fn.why);
        return PollResult::failed;
    }
    g_realized = true;
    const auto e = reinterpret_cast<std::uintptr_t>(ev);
    if (!e) {
        const std::uint32_t h = g_hash;
        Set(why, cap, "item %08X loaded but realized no entity", h);
        Log("[holo] %s", why);
        Teardown("no entity");
        Reject(h, "no entity");
        return PollResult::failed;
    }
    g_entity = ev;
    partner::ReadVtable(ev, &g_entity_vt);
    partner::ReadGuid(ev, &g_entity_guid);
    Log("[holo] item %08X loaded after %.1f s; realized entity %p", g_hash, took / 1000.0, ev);
    partner::DescribeEntity(ev, "holo realized");
    std::uintptr_t comp = 0;
    const char* bad = Validate(e, &comp);
    if (comp) {
        LogComponent(comp, "on realize");
        LogComponentResource(comp);
    }
    if (bad) {
        const std::uint32_t h = g_hash;
        Set(why, cap, "item %08X refused: %s", h, bad);
        Log("[holo] %s", why);
        Teardown("refused");
        Reject(h, bad);
        return PollResult::failed;
    }
    if (const char* blocked = StartVisibleBlocker(e)) {
        Log("[holo] StartVisible not called yet (%s); the 1 s maintenance retries it", blocked);
    } else if (!CallOne(g_fn.start_visible, ev)) {
        Disable("StartVisibleWelcomeHolo");
        Set(why, cap, "%s", g_fn.why);
        Teardown("StartVisible faulted");
        return PollResult::failed;
    }
    std::uint64_t f = 0;
    partner::ReadFlags(ev, &f);
    LogComponent(comp, "after StartVisible");
    Log("[holo] item %08X is the partner body now: entity %p flags=0x%llX (visible=%d)", g_hash, ev,
        static_cast<unsigned long long>(f), (f & 0x2) ? 1 : 0);
    g_phase = Phase::live;
    g_user_hidden = false;
    g_next_maintain_ms = now_ms + 1000;
    Rd(comp + kWhState, g_last_state);
    g_last_visible = (f & 0x2) ? 1 : 0;
    return PollResult::became_live;
}

bool Move(const game::WorldTransform& at) {
    game::Entity* e = LiveEntity();
    return e && game::TrySetTransform(e, at);
}

void Maintain(double now_ms) {
    if (g_phase != Phase::live || now_ms < g_next_maintain_ms) return;
    g_next_maintain_ms = now_ms + 1000;
    if (!EntityAlive()) return;  // Poll handles it
    const std::uintptr_t comp = partner::FindComponentByName(g_entity, "DSWelcomeHoloComponent");
    if (!comp) return;
    std::int32_t st = -1;
    std::uint8_t shown = 0;
    std::uint64_t f = 0;
    Rd(comp + kWhState, st);
    Rd(comp + kWhShown, shown);
    partner::ReadFlags(g_entity, &f);
    const int vis = (f & 0x2) ? 1 : 0;
    if (st != g_last_state || vis != g_last_visible) {
        Log("[holo] state %d -> %d, visible %d -> %d (flags 0x%llX, shown=%u)", g_last_state, st, g_last_visible, vis,
            static_cast<unsigned long long>(f), shown);
        g_last_state = st;
        g_last_visible = vis;
    }
    if (g_user_hidden || st != 0 || shown || !g_fn.ok) return;
    static const char* last_blocked = nullptr;
    if (const char* blocked = StartVisibleBlocker(reinterpret_cast<std::uintptr_t>(g_entity))) {
        if (blocked != last_blocked) Log("[holo] StartVisible re-assert skipped: %s", blocked);
        last_blocked = blocked;
        return;
    }
    last_blocked = nullptr;
    ++g_reasserts;
    if (g_reasserts <= 5 || g_reasserts % 30 == 0)
        Log("[holo] re-asserting StartVisible (#%u: state 0, shown clear, G=0x%llX)", g_reasserts,
            static_cast<unsigned long long>(GameState()));
    if (!CallOne(g_fn.start_visible, g_entity)) Disable("StartVisibleWelcomeHolo (re-assert)");
}

bool Remove(const char* why) { return Teardown(why); }

bool SetShown(bool shown) {
    game::Entity* e = LiveEntity();
    if (!e || !g_fn.ok) return false;
    if (shown) {
        if (const char* blocked = StartVisibleBlocker(reinterpret_cast<std::uintptr_t>(e))) {
            Log("[holo] debug show refused: %s", blocked);
            return false;
        }
    }
    g_user_hidden = !shown;
    const bool ok = CallOne(shown ? g_fn.start_visible : g_fn.stop_visible, e);
    if (!ok) Disable(shown ? "StartVisibleWelcomeHolo" : "StopVisibleWelcomeHolo");
    LogComponent(partner::FindComponentByName(e, "DSWelcomeHoloComponent"), shown ? "debug show" : "debug hide");
    return ok;
}

bool Describe(const char* what) {
    if (g_phase == Phase::idle) return false;
    if (g_phase == Phase::loading) {
        Log("[holo] %s: still loading item %08X (spawner %p)", what, g_hash, g_sp);
        return true;
    }
    game::Entity* e = LiveEntity();
    if (!e) return false;
    partner::DescribeEntity(e, what);
    const std::uintptr_t comp = partner::FindComponentByName(e, "DSWelcomeHoloComponent");
    LogComponent(comp, what);
    if (comp) LogComponentResource(comp);
    return true;
}

}  // namespace st::holo
