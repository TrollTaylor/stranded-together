// Enemy census (enemy sync P0): see enemy_census.hpp and docs/enemy-sync.md section 3.
#include "enemy_census.hpp"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "game_api.hpp"
#include "log.hpp"
#include "partner_source.hpp"

namespace st::enemy {
namespace {

namespace mem = partner::mem;
using mem::Q;

constexpr std::uintptr_t kImage = 0x140000000ull;  // ds.exe VAs at its preferred base
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe Director's Cut; docs/enemy-sync.md 3.2)
constexpr std::uintptr_t kRegGlobal = 0x147BBB8C0;   // the game actor registry
constexpr std::uintptr_t kNpcGlobal = 0x147BF29B8;   // the NPC manager (GUID -> NPC record)
constexpr std::uintptr_t kMuleGlobal = 0x147BF09D8;  // the MULE manager
constexpr std::uintptr_t kActorXf = 0x141A87230;     // GameActor vfunc +0x18: lea rax, [rcx + 0x28]; ret
// the registry: +0x440 group count, +0x448 groups, +0x4B0 the type limit; a group: +0x30 first slot, +0x40 count, +0x48 actors
constexpr std::uintptr_t kRegGroupCount = 0x440, kRegGroups = 0x448, kRegTypeLimit = 0x4B0;
constexpr std::uintptr_t kGrpFirst = 0x30, kGrpCount = 0x40, kGrpActors = 0x48;
// an actor: +0x08 id, +0x20 in use, +0x24 state (2 = its entity is in the world), +0x28 transform, +0x68 entity ref (entity + 0x20)
constexpr std::uintptr_t kActId = 0x08, kActInUse = 0x20, kActState = 0x24, kActXf = 0x28, kActEntRef = 0x68, kActLowLod = 0x8D;
// DSMuleGameActor: +0x110 the alert it keeps while not in the world (3 / 5), +0x120 / +0x124 flags (+0x124 bit 12 sequence-only),
// +0xEA4 (bits 13 / 14: a sequence requires / occupies it)
constexpr std::uintptr_t kMuleAlert = 0x110, kMuleFlags120 = 0x120, kMuleFlags124 = 0x124, kMuleSeq = 0xEA4;
// the NPC manager: +0x78 the main GUID map (data, +0x80 count, +0x84 capacity), per-kind managers at +8 + kind * 8 (their map at
// +0x18, capacity +0x24), kind 6's map at +0x48 (capacity +0x54); a record: +0x00 kind, +0x20 actor id
constexpr std::uintptr_t kNpcMain = 0x78, kNpcMainCount = 0x80, kNpcMainCap = 0x84, kNpcSub = 0x08, kSubMap = 0x18, kNpcKind6 = 0x48;
constexpr std::uintptr_t kRecKind = 0x00, kRecActor = 0x20;
// the MULE manager: +0x30 member count, +0x38 members (member +0x20 = actor id); killed records: +0x1A0 data, +0x1A8 count,
// +0x1AC capacity, 0x28 each (u32 used at +0)
constexpr std::uintptr_t kMuleMemberCount = 0x30, kMuleMembers = 0x38, kMemberActor = 0x20;
constexpr std::uintptr_t kKilledData = 0x1A0, kKilledCount = 0x1A8, kKilledCap = 0x1AC;
constexpr std::size_t kKilledSlot = 0x28;
// an entity: +0x88 flags (bit 7 dead: Entity_ExportedIsDead 0x142362C80), +0xC8 position
constexpr std::uintptr_t kEntFlags = 0x88, kEntPos = 0xC8;

enum Group : unsigned {
    kCore = 1u,    // the registry and the NPC manager's maps: without them no census
    kMule = 2u,    // DSMuleGameActor fields (alert, flags)
    kDead = 4u,    // the entity's dead flag
    kXf = 8u,      // the actor's own transform
    kMgr = 16u,    // the MULE manager's members and killed records
};
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    // GameActorRegistry lookup (reg, id): the type limit +0x4B0, groups +0x440 / +0x448, group vt+0x38 type, +0x30 first, +0x40 count,
    // +0x48 actors, in use +0x20 (read from Ghidra; split, CodeMatches compares at most 96 bytes)
    {0x1422FA980, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 20 3B 15 B8 67 7F 02 8B DA 0F 84 86 00 00 00 8B C2 C1 E8 0D 44 0F B6 F0 "
                  "44 3B B1 B0 04 00 00 73 74 48 8B B9 48 04 00 00 48 63 81 40 04 00 00 48 8D 2C C7 48 3B FD 74 5D 0F 1F 80 00 00 00 00 48 8B "
                  "37 48 8B CE 48 8B 06 FF", "actor registry lookup (groups)", kCore},
    {0x1422FA9DA, "50 38 41 3B C6 75 1B 44 8B 46 30 8B D3 81 E2 FF 1F 00 00 41 3B D0 72 0A 8B 4E 40 41 03 C8 3B D1 72 0B 48 83 C7 08 48 3B FD "
                  "75 CB EB 1F 48 8B 46 48 81 E3 FF 1F 00 00 41 2B D8 48 63 CB 48 8B 04 C8 48 85 C0 74 06 80 78 20 00 75 02 33 C0 48 8B 5C 24 "
                  "48 48 8B 6C 24 50", "actor registry lookup (slots, in use)", kCore},
    // a caller: the registry global, then actor +0x68 (the entity ref)
    {0x1422D6268, "48 8B 0D 51 56 8E 05 E8 0C 47 02 00 48 85 C0 74 1D 48 8B 48 68", "registry global, actor +0x68", kCore},
    // ForceLowLodNpc: the NPC manager global, record +0x20 = actor id, the registry, actor +0x8D
    {0x142BF7850, "40 53 48 83 EC 20 0F B6 DA 48 8B D1 48 8B 0D 55 B1 FF 04 E8 18 80 F8 FF 48 85 C0 74 25 8B 0D DD 98 EF 01 39 48 20 74 1A 8B 50 "
                  "20 48 8B 0D 3E 40 FC 04 E8 F9 30 70 FF 48 85 C0 74 06 88 98 8D 00 00 00", "ForceLowLodNpc (record +0x20, actor +0x8D)", kCore},
    // the NPC manager's GUID lookup 0x142B7F880: the main map +0x78 (0x18 slots, kind +0x14, capacity +0x84), kind 6 -> +0x48,
    // else [mgr + 8 + kind * 8] +0x18; 0x20 slots, the record at +0x18
    {0x142B7F8B4, "48 8B D6 48 8D 4D 78 E8 20 3F F8 FE 83 F8 FF 75 04 33 DB EB 7F 48 8B D6 48 8D 4D 78 E8 0B 3F F8 FE 83 F8 FF 75 06 8B 85 84 00 "
                  "00 00 48 98 33 DB 48 8D 0C 40 48 8B 45 78 8B 54 C8 14 83 FA 06 74 32 4C 89 74 24 20 4C 8B 74 D5", "NPC GUID lookup (main map)", kCore},
    {0x142B7F8FE, "08 48 8B D6 49 8D 4E 18 E8 E5 3F F8 FE 83 F8 FF 74 10 48 63 C8 49 8B 46 18 48 C1 E1 05 48 8B 5C 01 18 4C 8B 74 24 20 EB 21 48 "
                  "8B D6 48 8D 4D 48 E8 BD 3F F8 FE 83 F8 FF 74 10 48 63 D0 48 8B 45 48 48 C1 E2 05 48 8B 5C 02 18", "NPC GUID lookup (per-kind maps)", kCore},
    // the two map finds: data +0, capacity +0xC; the main map's GUID at slot +4 / +0xC, the per-kind maps' at +8 / +0x10
    {0x141B03880, "44 8B 1C 1A 41 8B CB 41 23 CA 41 3B C9 7F 33 44 3B DF 75 0E 48 39 6C 1A 04 75 07 48 39 74 1A 0C 74 25", "main map slot (GUID +4)", kCore},
    {0x141B03904, "4C 8B 01 4D 85 C0 0F 84 CC 00 00 00 44 8B 51 0C 33 C0 48 8B 2A 41 FF CA", "per-kind map (data, capacity +0xC)", kCore},
    {0x141B039A0, "44 8B 1C 19 48 8D 14 19 41 8B CB 41 23 CA 41 3B C9 7F 29 44 3B DF 75 0C 48 39 6A 08 75 06 48 39 72 10 74 1D", "per-kind map slot (GUID +8)", kCore},
    // SetSequenceOnlyMule: record +0x20, the type-1 id test, actor +0x124 / +0xEA8 / +0x120
    {0x142B2FA58, "8B 48 20 3B 0D EF 16 FC 01 74 71 81 E1 00 E0 1F 00 81 F9 00 20 00 00 75 63 8B 50 20 48 8B 0D 45 BE 08 05 48 89 5C 24 20 E8 FB AE "
                  "7C FF 48 8B D8 48 85 C0 74 42 81 88 24 01 00 00 00 10 00 00 81 88 A8 0E 00 00 00 10 00 00 8B 88 20 01 00 00 C1 E9 0E F6 C1 01 75 20",
     "SetSequenceOnlyMule (type 1, +0x124)", kMule},
    // MsgDSSetKnowledge on a MULE actor not in the world: +0x24 == 2 skips, +0x110 = 3 / 5
    {0x142AE72E0, "83 79 24 02 74 5C 8B 52 3C 83 EA 01 74 2D 83 FA 01 75 4F 83 B9 10 01 00 00 03", "MULE knowledge (+0x110)", kMule},
    // Entity_ExportedIsDead: flags +0x88 bit 7
    {0x142362C80, "48 85 C9 74 0D 48 8B 81 88 00 00 00 C0 E8 07 24 01 C3", "Entity IsDead (+0x88 bit 7)", kDead},
    {kActorXf, "48 8D 41 28 C3", "GameActor transform getter (+0x28)", kXf},
    // RemoveAllMulesFromWorld body: members +0x38 / +0x30, member +0x20 = actor id
    {0x142B140DE, "48 8B 79 38", "MULE manager members +0x38", kMgr},
    {0x142B140FA, "48 63 41 30", "MULE manager member count +0x30", kMgr},
    {0x142B14140, "48 8B 07 4C 8D 44 24 20 48 8B 0D 71 77 0A 05 41 B1 01 8B 50 20 E8", "MULE member +0x20 (actor id)", kMgr},
    // the resurrect body: killed records +0x1A0 / +0x1A8 / +0x1AC, 0x28 each
    {0x142B16932, "44 39 B7 A8 01 00 00 74 49 48 63 87 AC 01 00 00 48 89 5C 24 50 48 8B 9F A0 01 00 00 48 89 74 24 58 48 8D 0C 80 48 8D 34 CB 48 3B "
                  "DE 73 1A 44 39 33 74 0C 48 8D 4B 10 E8 42 EC FD FF 44 89 33 48 83 C3 28 48 3B DE 72 E6", "MULE killed records (+0x1A0, 0x28 each)", kMgr},
    {0x142B64420, "48 8B 0D B1 C5 08 05", "MULE manager global (thunk)", kMgr},
};
// clang-format on

bool g_checked = false;
unsigned g_groups = 0;
char g_off[160] = "";

void Put(char* msg, std::size_t cap, const char* fmt, ...) {
    if (!msg || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
}

unsigned Check() {
    if (g_checked) return g_groups;
    g_checked = true;
    unsigned groups = kCore | kMule | kDead | kXf | kMgr;
    int ok = 0, total = 0;
    if (!mem::Base()) {
        std::snprintf(g_off, sizeof(g_off), "ds.exe base unknown");
        return g_groups = 0;
    }
    for (const Site& s : kSites) {
        ++total;
        if (mem::CodeMatches(A(s.va), s.sig)) {
            ++ok;
            continue;
        }
        Log("[enemy] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
        if ((s.group & kCore) && !g_off[0]) std::snprintf(g_off, sizeof(g_off), "%s differs", s.name);
        groups &= ~s.group;
    }
    struct Rip {
        std::uintptr_t insn, want;
        unsigned group;
        const char* what;
    };
    const Rip rips[] = {
        {0x1422D6268, kRegGlobal, kCore, "the actor registry global"},
        {0x142BF785C, kNpcGlobal, kCore, "the NPC manager global"},
        {0x142BF787B, kRegGlobal, kCore, "the actor registry global (ForceLowLodNpc)"},
        {0x142B64420, kMuleGlobal, kMgr, "the MULE manager global"},
    };
    for (const Rip& r : rips) {
        ++total;
        if (mem::RipTarget(A(r.insn), 3, 7) == A(r.want)) {
            ++ok;
            continue;
        }
        Log("[enemy] byte check FAILED: %s", r.what);
        if ((r.group & kCore) && !g_off[0]) std::snprintf(g_off, sizeof(g_off), "%s is not where expected", r.what);
        groups &= ~r.group;
    }
    if (!(groups & kCore)) groups = 0;
    g_groups = groups;
    Log("[enemy] census byte checks: %d/%d ok; %s%s%s%s%s", ok, total, (groups & kCore) ? "registry and NPC maps ok" : "OFF (the census refuses)",
        (groups & kMule) ? ", MULE fields" : "", (groups & kDead) ? ", dead flag" : "", (groups & kXf) ? ", actor transform" : "",
        (groups & kMgr) ? ", MULE manager" : "");
    return g_groups;
}

bool R32(std::uintptr_t a, std::uint32_t& v) { return mem::Read(a, &v, 4); }
bool RI32(std::uintptr_t a, std::int32_t& v) { return mem::Read(a, &v, 4); }

// Reads `cap` slots of `size` bytes in chunks; calls f(slot bytes) for each. False when a chunk is unreadable.
template <class F>
bool ForSlots(std::uintptr_t data, std::uint32_t cap, std::size_t size, F f) {
    if (!data || !cap || cap > kMaxMapCapacity) return false;
    std::vector<std::uint8_t> buf;
    constexpr std::uint32_t kChunk = 2048;
    for (std::uint32_t at = 0; at < cap; at += kChunk) {
        const std::uint32_t n = cap - at < kChunk ? cap - at : kChunk;
        buf.resize(static_cast<std::size_t>(n) * size);
        if (!mem::Read(data + static_cast<std::uintptr_t>(at) * size, buf.data(), buf.size())) return false;
        for (std::uint32_t i = 0; i < n; ++i) f(buf.data() + static_cast<std::size_t>(i) * size);
    }
    return true;
}

struct NpcInfo {
    Guid16 guid;
    std::uint32_t kind = 0xFFFFFFFF;  // the main map's kind
    std::int32_t rec_kind = -1;       // the record's own +0
    std::uintptr_t rec = 0;
};

struct Actor {
    std::uintptr_t obj = 0;
    std::uint32_t id = kActorNone;
    std::int32_t state = -1;
    std::uintptr_t entity = 0;
    const char* type = "?";
    bool has_xf = false;
    double pos[3] = {};
    float heading = 0;
    bool has_ent_pos = false;
    double ent_pos[3] = {};
    bool dead = false;
    bool is_mule = false;
    std::int32_t alert = -1;
    std::uint32_t f120 = 0, f124 = 0, seq = 0;
    std::uint8_t low_lod = 0;
    const NpcInfo* npc = nullptr;
    double dist = -1;
};

bool ReadPos(std::uintptr_t at, double (&p)[3]) {
    if (!mem::Read(at, p, sizeof(p))) return false;
    for (double v : p)
        if (!std::isfinite(v) || std::fabs(v) > 1e7) return false;
    return true;
}

const char* TypeOf(std::uintptr_t obj) {
    const char* n = obj ? partner::TypeName(reinterpret_cast<const void*>(obj)) : nullptr;
    return n ? n : "?";
}

void Describe(Actor& a, unsigned groups) {
    a.type = TypeOf(a.obj);
    a.is_mule = (groups & kMule) && mem::DerivesFrom(mem::RttiOf(a.obj), "DSMuleGameActor");
    // its own transform, when its vfunc +0x18 is the base getter (lea rax, [rcx + 0x28])
    const std::uintptr_t vt = Q(a.obj);
    if ((groups & kXf) && vt && Q(vt + 0x18) == A(kActorXf) && ReadPos(a.obj + kActXf, a.pos)) {
        a.has_xf = true;
        float rot[9] = {};
        if (mem::Read(a.obj + kActXf + 0x18, rot, sizeof(rot)) && std::isfinite(rot[3]) && std::isfinite(rot[4]))
            a.heading = static_cast<float>(std::atan2(rot[3], rot[4]) * 57.29577951308232);
    }
    if (a.entity) {
        a.has_ent_pos = ReadPos(a.entity + kEntPos, a.ent_pos);
        std::uint64_t fl = 0;
        if ((groups & kDead) && mem::Read(a.entity + kEntFlags, &fl, 8)) a.dead = (fl & 0x80) != 0;
    }
    mem::Rd(a.obj + kActLowLod, a.low_lod);
    if (a.is_mule) {
        RI32(a.obj + kMuleAlert, a.alert);
        R32(a.obj + kMuleFlags120, a.f120);
        R32(a.obj + kMuleFlags124, a.f124);
        R32(a.obj + kMuleSeq, a.seq);
    }
}

const double* PosOf(const Actor& a) { return a.has_xf ? a.pos : a.has_ent_pos ? a.ent_pos : nullptr; }

}  // namespace

static bool CensusImpl(int tag, double radius_m, int flags, char* msg, std::size_t cap) {
    const unsigned groups = Check();
    if (!(groups & kCore)) return Put(msg, cap, "enemy census off: %s", g_off[0] ? g_off : "byte checks failed"), false;
    const double t0 = static_cast<double>(GetTickCount64());
    const std::uintptr_t reg = Q(A(kRegGlobal));
    const std::uintptr_t npc = Q(A(kNpcGlobal));
    if (!reg || !npc) return Put(msg, cap, "no actor registry / NPC manager (no world?)"), false;

    // ---- our Sam
    double sam[3] = {};
    bool have_sam = false;
    {
        game::Entity* e = nullptr;
        game::WorldTransform wt{};
        if (game::TryGetLocalPlayer(&e) && e && game::TryGetTransform(e, &wt)) {
            sam[0] = wt.pos.x, sam[1] = wt.pos.y, sam[2] = wt.pos.z;
            have_sam = true;
        }
    }

    // ---- the NPC manager: GUID -> kind (main map), GUID -> record (per-kind maps), record -> actor id
    std::map<Guid16, NpcInfo> npcs;
    std::uint32_t main_cap = 0, main_count = 0;
    R32(npc + kNpcMainCap, main_cap);
    R32(npc + kNpcMainCount, main_count);
    int kind_count[kKinds + 1] = {};
    const bool main_ok = MapCapacityOk(main_cap) && ForSlots(Q(npc + kNpcMain), main_cap, kKindSlot, [&](const std::uint8_t* s) {
        Guid16 g;
        std::uint32_t k = 0;
        if (!DecodeKindSlot(s, &g, &k)) return;
        NpcInfo& n = npcs[g];
        n.guid = g;
        n.kind = k;
        ++kind_count[k < kKinds ? k : kKinds];
    });
    int recs = 0, recs_orphan = 0;
    for (std::uint32_t k = 0; k < kKinds; ++k) {
        if (!kind_count[k]) continue;
        std::uintptr_t map = 0;
        if (k == 6) map = npc + kNpcKind6;
        else {
            const std::uintptr_t sub = Q(npc + kNpcSub + 8u * k);
            if (!sub) continue;
            map = sub + kSubMap;
        }
        std::uint32_t c = 0;
        R32(map + 0xC, c);
        if (!MapCapacityOk(c)) continue;
        ForSlots(Q(map), c, kValueSlot, [&](const std::uint8_t* s) {
            Guid16 g;
            std::uint64_t v = 0;
            if (!DecodeValueSlot(s, &g, &v)) return;
            auto it = npcs.find(g);
            if (it == npcs.end()) {
                ++recs_orphan;
                return;
            }
            it->second.rec = static_cast<std::uintptr_t>(v);
            ++recs;
        });
    }
    std::map<std::uint32_t, const NpcInfo*> by_actor;
    int recs_with_actor = 0;
    for (auto& kv : npcs) {
        NpcInfo& n = kv.second;
        if (!n.rec) continue;
        RI32(n.rec + kRecKind, n.rec_kind);
        std::uint32_t id = kActorNone;
        if (R32(n.rec + kRecActor, id) && id != kActorNone) {
            by_actor[id] = &n;
            ++recs_with_actor;
        }
    }

    // ---- the actor registry
    std::int32_t ngroups = 0;
    std::uint32_t type_limit = 0;
    RI32(reg + kRegGroupCount, ngroups);
    R32(reg + kRegTypeLimit, type_limit);
    if (ngroups < 0 || ngroups > 512) return Put(msg, cap, "the actor registry reads %d groups: refused", ngroups), false;
    const std::uintptr_t garr = Q(reg + kRegGroups);
    std::vector<Actor> enemies;
    std::vector<Actor> others;
    std::map<std::string, int> per_type;  // RTTI name -> in-use count
    std::map<std::uint32_t, int> per_id_type;
    int in_use = 0, slot_mismatch = 0;
    for (std::int32_t gi = 0; gi < ngroups; ++gi) {
        const std::uintptr_t g = Q(garr + 8u * static_cast<std::uintptr_t>(gi));
        if (!g) continue;
        std::uint32_t first = 0;
        std::int32_t count = 0;
        R32(g + kGrpFirst, first);
        RI32(g + kGrpCount, count);
        if (count <= 0 || count > 8192) continue;
        const std::uintptr_t arr = Q(g + kGrpActors);
        if (!arr) continue;
        std::vector<std::uintptr_t> objs(static_cast<std::size_t>(count));
        if (!mem::Read(arr, objs.data(), objs.size() * 8)) continue;
        for (std::int32_t s = 0; s < count; ++s) {
            const std::uintptr_t o = objs[static_cast<std::size_t>(s)];
            if (!o) continue;
            std::uint8_t head[0x70] = {};
            if (!mem::Read(o, head, sizeof(head)) || !head[kActInUse]) continue;
            Actor a;
            a.obj = o;
            std::memcpy(&a.id, head + kActId, 4);
            std::memcpy(&a.state, head + kActState, 4);
            std::uintptr_t ref = 0;
            std::memcpy(&ref, head + kActEntRef, 8);
            a.entity = ref ? ref - 0x20 : 0;
            ++in_use;
            if (ActorSlot(a.id) != first + static_cast<std::uint32_t>(s)) ++slot_mismatch;
            ++per_id_type[ActorType(a.id)];
            ++per_type[TypeOf(o)];
            const auto it = by_actor.find(a.id);
            if (it != by_actor.end()) a.npc = it->second;
            if (IsHumanEnemyId(a.id)) {
                Describe(a, groups);
                enemies.push_back(a);
            } else if (flags & 1) {
                Describe(a, groups);
                others.push_back(a);
            }
        }
    }

    // ---- the MULE manager (cross-check: its members' actor ids, the killed records)
    std::int32_t members = -1, members_found = 0, killed = -1, killed_cap = 0;
    std::uintptr_t mule = 0, killed_data = 0;
    if (groups & kMgr) {
        mule = Q(A(kMuleGlobal));
        if (mule && RI32(mule + kMuleMemberCount, members) && members >= 0 && members <= 4096) {
            std::vector<std::uintptr_t> m(static_cast<std::size_t>(members));
            if (members && mem::Read(Q(mule + kMuleMembers), m.data(), m.size() * 8)) {
                for (std::uintptr_t p : m) {
                    std::uint32_t id = kActorNone;
                    if (!p || !R32(p + kMemberActor, id)) continue;
                    for (const Actor& a : enemies)
                        if (a.id == id) {
                            ++members_found;
                            break;
                        }
                }
            }
        } else members = -1;
        if (mule) {
            RI32(mule + kKilledCount, killed);
            RI32(mule + kKilledCap, killed_cap);
            killed_data = Q(mule + kKilledData);
        }
    }

    // ---- summary and fingerprints (the same numbers on both games for the same set)
    for (Actor& a : enemies)
        if (const double* p = PosOf(a); p && have_sam)
            a.dist = std::sqrt((p[0] - sam[0]) * (p[0] - sam[0]) + (p[1] - sam[1]) * (p[1] - sam[1]) + (p[2] - sam[2]) * (p[2] - sam[2]));
    std::vector<Guid16> guids;
    std::uint32_t idprint = kFnvSeed;
    std::vector<std::uint32_t> ids;
    int realized = 0, with_guid = 0, mules = 0, dead = 0, with_pos = 0;
    for (const Actor& a : enemies) {
        ids.push_back(a.id);
        if (a.npc) guids.push_back(a.npc->guid), ++with_guid;
        if (a.state == 2 && a.entity) ++realized;
        if (a.is_mule) ++mules;
        if (a.dead) ++dead;
        if (PosOf(a)) ++with_pos;
    }
    std::sort(ids.begin(), ids.end());
    for (std::uint32_t id : ids) idprint = Fnv1a(idprint, &id, 4);
    const std::uint32_t guidprint = GuidSetPrint(guids);
    std::string types;
    for (const auto& kv : per_id_type) {
        char b[24];
        std::snprintf(b, sizeof(b), "%s%u:%d", types.empty() ? "" : " ", kv.first, kv.second);
        types += b;
    }
    std::string names;
    int shown = 0;
    for (const auto& kv : per_type) {
        if (++shown > 24) {
            names += " ...";
            break;
        }
        char b[96];
        std::snprintf(b, sizeof(b), "%s%s %d", names.empty() ? "" : ", ", kv.first.c_str(), kv.second);
        names += b;
    }
    std::string kinds;
    for (std::uint32_t k = 0; k <= kKinds; ++k) {
        if (!kind_count[k]) continue;
        char b[24];
        std::snprintf(b, sizeof(b), "%s%s%u:%d", kinds.empty() ? "" : " ", k == kKinds ? ">" : "", k == kKinds ? 6u : k, kind_count[k]);
        kinds += b;
    }
    Log("[enemy] census tag=%d: registry %d groups (type limit %u), %d actors in use (by id type %s)%s", tag, ngroups, type_limit, in_use,
        types.c_str(), slot_mismatch ? " SLOT MISMATCH" : "");
    Log("[enemy] census tag=%d: actor classes: %s", tag, names.c_str());
    Log("[enemy] census tag=%d: NPC map %u/%u GUIDs %s (by kind %s), %d records (%d with an actor, %d orphan slots)", tag,
        static_cast<unsigned>(npcs.size()), main_count, main_ok ? "read" : "UNREADABLE", kinds.c_str(), recs, recs_with_actor, recs_orphan);
    Log("[enemy] census tag=%d: enemies (type 1) %d: %d DSMuleGameActor, %d with an entity in the world, %d with a GUID, %d placed, %d dead; "
        "MULE manager %d members (%d of them listed), killed records %d; prints ids %08X guids %08X; Sam %s(%.2f, %.2f, %.2f)",
        tag, static_cast<int>(enemies.size()), mules, realized, with_guid, with_pos, dead, members, members_found, killed, idprint, guidprint,
        have_sam ? "" : "? ", sam[0], sam[1], sam[2]);

    // ---- one line per enemy (nearest first), within the radius
    std::vector<const Actor*> order;
    for (const Actor& a : enemies) order.push_back(&a);
    std::sort(order.begin(), order.end(), [](const Actor* x, const Actor* y) {
        const double dx = x->dist < 0 ? 1e12 : x->dist, dy = y->dist < 0 ? 1e12 : y->dist;
        return dx != dy ? dx < dy : x->id < y->id;
    });
    int listed = 0;
    for (const Actor* ap : order) {
        const Actor& a = *ap;
        if (radius_m > 0 && !(a.dist >= 0 && a.dist <= radius_m)) continue;
        char gt[33] = "-";
        if (a.npc) GuidText(a.npc->guid, gt);
        const double* p = PosOf(a);
        char where[96] = "no place";
        if (p) std::snprintf(where, sizeof(where), "(%.2f, %.2f, %.2f) %s hdg %.0f, %.1f m", p[0], p[1], p[2], a.has_xf ? "xf" : "ent", a.heading, a.dist);
        char ent[80] = "no entity";
        if (a.entity) {
            const double d = a.has_ent_pos && a.has_xf ? std::sqrt((a.ent_pos[0] - a.pos[0]) * (a.ent_pos[0] - a.pos[0]) +
                                                                   (a.ent_pos[1] - a.pos[1]) * (a.ent_pos[1] - a.pos[1]) +
                                                                   (a.ent_pos[2] - a.pos[2]) * (a.ent_pos[2] - a.pos[2]))
                                                         : -1;
            std::snprintf(ent, sizeof(ent), "entity %s%s, %.2f m off", a.has_ent_pos ? "" : "(no place) ", a.dead ? " DEAD" : "", d);
        }
        char mf[96] = "";
        if (a.is_mule)
            std::snprintf(mf, sizeof(mf), "; alert %d, flags120 %08X flags124 %08X seq %08X", a.alert, a.f120, a.f124, a.seq);
        Log("[enemy] tag=%d e%03d id 0x%06X %s state %d guid %s npc %d/%d %s; %s; lowlod %u%s", tag, listed, a.id, a.type, a.state, gt,
            a.npc ? static_cast<int>(a.npc->kind) : -1, a.npc ? a.npc->rec_kind : -1, where, ent, a.low_lod, mf);
        ++listed;
    }
    for (const Actor& a : others) {
        const double* p = PosOf(a);
        char gt[33] = "-";
        if (a.npc) GuidText(a.npc->guid, gt);
        Log("[enemy] tag=%d other id 0x%06X type %u %s state %d guid %s %s(%.2f, %.2f, %.2f)%s", tag, a.id, ActorType(a.id), a.type, a.state, gt,
            p ? "" : "no place ", p ? p[0] : 0.0, p ? p[1] : 0.0, p ? p[2] : 0.0, a.entity ? " entity" : "");
    }
    if ((flags & 2) && killed_data && killed > 0 && killed_cap > 0) {
        int n = 0;
        ForSlots(killed_data, static_cast<std::uint32_t>(killed_cap), kKilledSlot, [&](const std::uint8_t* s) {
            if (!Le32(s) || n >= 32) return;
            char hex[2 * kKilledSlot + 1] = {};
            for (std::size_t i = 0; i < kKilledSlot; ++i) std::snprintf(hex + 2 * i, 3, "%02X", s[i]);
            Log("[enemy] tag=%d killed k%02d %s", tag, n++, hex);
        });
    }
    const double ms = static_cast<double>(GetTickCount64()) - t0;
    Log("[enemy] census done tag=%d (%d listed, %d other, %.0f ms)", tag, listed, static_cast<int>(others.size()), ms);
    Put(msg, cap, "enemy census tag %d: %d enemies (%d in the world, %d with a GUID), %d listed; ids %08X guids %08X", tag,
        static_cast<int>(enemies.size()), realized, with_guid, listed, idprint, guidprint);
    return true;
}

static bool FingerprintImpl(std::uint32_t* ids_fp, std::uint32_t* guids_fp, std::uint16_t* count) {
    const unsigned groups = Check();
    if (!(groups & kCore)) return false;
    const std::uintptr_t reg = Q(A(kRegGlobal));
    const std::uintptr_t npc = Q(A(kNpcGlobal));
    if (!reg || !npc) return false;
    // the NPC maps: GUID -> record -> actor id (as the census)
    std::map<Guid16, NpcInfo> npcs;
    std::uint32_t main_cap = 0;
    R32(npc + kNpcMainCap, main_cap);
    bool kinds[kKinds] = {};
    if (!MapCapacityOk(main_cap) || !ForSlots(Q(npc + kNpcMain), main_cap, kKindSlot, [&](const std::uint8_t* s) {
            Guid16 g;
            std::uint32_t k = 0;
            if (!DecodeKindSlot(s, &g, &k)) return;
            NpcInfo& n = npcs[g];
            n.guid = g;
            n.kind = k;
            if (k < kKinds) kinds[k] = true;
        }))
        return false;
    for (std::uint32_t k = 0; k < kKinds; ++k) {
        if (!kinds[k]) continue;
        std::uintptr_t map = 0;
        if (k == 6) map = npc + kNpcKind6;
        else {
            const std::uintptr_t sub = Q(npc + kNpcSub + 8u * k);
            if (!sub) continue;
            map = sub + kSubMap;
        }
        std::uint32_t c = 0;
        R32(map + 0xC, c);
        if (!MapCapacityOk(c)) continue;
        ForSlots(Q(map), c, kValueSlot, [&](const std::uint8_t* s) {
            Guid16 g;
            std::uint64_t v = 0;
            if (!DecodeValueSlot(s, &g, &v)) return;
            auto it = npcs.find(g);
            if (it != npcs.end()) it->second.rec = static_cast<std::uintptr_t>(v);
        });
    }
    std::map<std::uint32_t, Guid16> by_actor;
    for (auto& kv : npcs) {
        std::uint32_t id = kActorNone;
        if (kv.second.rec && R32(kv.second.rec + kRecActor, id) && id != kActorNone) by_actor[id] = kv.second.guid;
    }
    // the registry: every enemy actor in use
    std::int32_t ngroups = 0;
    RI32(reg + kRegGroupCount, ngroups);
    if (ngroups <= 0 || ngroups > 512) return false;
    const std::uintptr_t garr = Q(reg + kRegGroups);
    std::vector<std::uint32_t> ids;
    std::vector<Guid16> guids;
    for (std::int32_t gi = 0; gi < ngroups; ++gi) {
        const std::uintptr_t g = Q(garr + 8u * static_cast<std::uintptr_t>(gi));
        std::int32_t cnt = 0;
        if (!g || !RI32(g + kGrpCount, cnt) || cnt <= 0 || cnt > 8192) continue;
        const std::uintptr_t arr = Q(g + kGrpActors);
        if (!arr) continue;
        std::vector<std::uintptr_t> objs(static_cast<std::size_t>(cnt));
        if (!mem::Read(arr, objs.data(), objs.size() * 8)) continue;
        for (const std::uintptr_t o : objs) {
            if (!o) continue;
            std::uint8_t head[0x28] = {};
            if (!mem::Read(o, head, sizeof(head)) || !head[kActInUse]) continue;
            std::uint32_t id = kActorNone;
            std::memcpy(&id, head + kActId, 4);
            if (!IsHumanEnemyId(id)) continue;
            ids.push_back(id);
            const auto it = by_actor.find(id);
            if (it != by_actor.end()) guids.push_back(it->second);
        }
    }
    if (ids.empty() || ids.size() > 0xFFFF) return false;
    std::sort(ids.begin(), ids.end());
    std::uint32_t idprint = kFnvSeed;
    for (std::uint32_t id : ids) idprint = Fnv1a(idprint, &id, 4);
    *ids_fp = idprint;
    *guids_fp = GuidSetPrint(guids);
    *count = static_cast<std::uint16_t>(ids.size());
    return true;
}

bool Fingerprint(std::uint32_t* ids_fp, std::uint32_t* guids_fp, std::uint16_t* count) noexcept {
    try {
        return FingerprintImpl(ids_fp, guids_fp, count);
    } catch (...) {
        return false;
    }
}

bool Census(int tag, double radius_m, int flags, char* msg, std::size_t cap) noexcept {
    try {
        return CensusImpl(tag, radius_m, flags, msg, cap);
    } catch (...) {
        Put(msg, cap, "enemy census failed (out of memory?)");
        return false;
    }
}

}  // namespace st::enemy
