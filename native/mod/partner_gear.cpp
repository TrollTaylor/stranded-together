#include "partner_gear.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "automation.hpp"
#include "game_actions.hpp"
#include "log.hpp"
#include "partner_holo.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"

namespace st::gear {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// Evidence: docs/real-partner-body.md section 12. Every offset is byte-checked once (Check).
constexpr std::uintptr_t kPlayerVtable = 0x3D7D0D8;   // DSPlayerEntity
constexpr std::uintptr_t kInitInvStore = 0x271FC22;   // mov [rbx+0x54C8],rax  (inventory component)
constexpr std::uintptr_t kInitCarStore = 0x271FC38;   // mov [rbx+0x54D0],rax  (DSBaggage2CarrierComponent)
constexpr std::uintptr_t kInvOdraStore = 0x27C0BC8;   // mov [rdi+0x4CA0],r13  (Odradek body, a ref: entity = value - 0x20)
constexpr std::uintptr_t kInvPackStore = 0x27C0D11;   // mov [rdi+0x4CB8],r15  (backpack, entity = value - 0x20)
constexpr std::uintptr_t kCarrierBind = 0x2CB1ED0;    // mov rax,[rcx+0x40]; mov [rcx+0x50],rdx; mov [rax+0x30],rdx; ret
constexpr std::uintptr_t kSlotLookup = 0x2C52010;     // carrier +0x18 slots, +0x10 count; slot +1 type
constexpr std::uintptr_t kSlotWalk = 0x2C536ED;       // mov edx,[rcx+0xC] (slot total)
constexpr std::uintptr_t kSlotWalk2 = 0x2C5370A;      // mov r14,[rbx+0x20]; movsxd rax,[rbx+0x18] (stacks)
constexpr std::uintptr_t kSlotWalk3 = 0x2C53736;      // mov rbx,[rax+0x18]; movsxd rax,[rax+0x10] (stack items)
constexpr std::uintptr_t kBagSetWorld = 0x2C73D08;    // mov rax,[rcx+0x330]; cmp byte [rax+0x21],1 (cargo entity -> DSBaggage2)
constexpr std::uintptr_t kPInv = 0x54C8, kPCarrier = 0x54D0, kCompOwner = 0x48;
constexpr std::uintptr_t kInvOdra = 0x4CA0, kInvPack = 0x4CB8, kInvBbPod = 0x4D60;
constexpr std::uintptr_t kCarrierObj = 0x50, kCarSlotN = 0x10, kCarSlots = 0x18;
constexpr std::uintptr_t kSlotType = 0x01, kSlotStackN = 0x18, kSlotStacks = 0x20;
constexpr std::uintptr_t kStackN = 0x10, kStackItems = 0x18;
// DSBaggage2 (0x240): +0x20 u8 EDSBaggage2Model, +0x28 DSGameBaggageListItem*, +0x38 world position
// (3 doubles), +0x50 rotation rows (9 floats). DSGameBaggageListItem: +0x30 ID, +0x3A attribute,
// +0x3B case type, +0x3E volume, +0x50 weight (dsdc_types offsets).
constexpr std::uintptr_t kBagModel = 0x20, kBagListItem = 0x28, kBagWorld = 0x38;
constexpr std::uintptr_t kListId = 0x30, kListAttribute = 0x3A, kListWeight = 0x50;
constexpr std::uintptr_t kSlotTotal = 0x0C;
constexpr std::uintptr_t kEntFlags = 0x88, kEntWorld = 0xC8;

// Sam-skeleton joints (the 77-joint layout, docs/real-partner-body.md section 3).
constexpr std::uint8_t kJHips = 1, kJSpine4 = 4, kJSpine5 = 5, kJRArm = 7, kJRHand = 9, kJLShoulder = 35, kJLArm = 36, kJLHand = 38;

constexpr double kReadEveryMs = 100;

// ---------------------------------------------------------------- props (cutscene entities, by path + GUID)
struct PropDef {
    const char* key;
    const char* path;
    const char* guid;
};
// ds/entities/cinematics/* (DSControlledEntityResource: SkinnedModel, no AI / controller / player).
constexpr PropDef kProps[] = {
    {"backpack", "ds/entities/cinematics/demo_bak0_mule_def", "08ab4fca-839a-3142-86d1-b154336412a0"},
    {"odradek", "ds/entities/cinematics/demo_odra_main_def_new", "b95ab259-dcb8-584d-9b49-ce0a10527541"},
    {"bb_pod", "ds/entities/cinematics/demo_bbp1_main_def", "b8e1a738-ff6c-3f44-beeb-0a013933d141"},
    {"case_s", "ds/entities/cinematics/demo_bsm0_baggagesmall0_new", "82b892c4-605d-3a42-b897-436dfc6ca7d5"},
    {"case_m", "ds/entities/cinematics/demo_bmd0_main_def_v02_new", "dcffeaff-5df3-ce4f-a72a-8dbf78de3bf0"},
    {"case_l", "ds/entities/cinematics/demo_bla0_baggagelarge0_new", "39120f1e-e878-3d46-b272-90153e5ed040"},
    {"case_xl", "ds/entities/cinematics/demo_blx0_main_def", "eb084776-a437-174b-8ea0-fa40efe9da07"},
    {"cube", "ds/entities/cinematics/demo_blx1_main_def", "91243825-ad49-6a48-81e1-c4ec0f65a2b2"},
};
enum PropId : int { kPropNone = -1, kPropBackpack = 0, kPropOdradek, kPropBbPod, kPropS, kPropM, kPropL, kPropXL, kPropCube, kPropCount };
static_assert(sizeof(kProps) / sizeof(kProps[0]) == kPropCount, "prop table");

// EDSBaggage2Model (the baggage config's ModelResources, decoded from ds/entities/item/baggage/
// baggage_config) -> the prop drawn for it. Exact: the same mesh family. Approximate: a case of
// about the same size (weapon cases, material tanks, bsm1, blx2). None: not drawn (builders, carts,
// the ladder, shields, the heater).
struct ModelMap {
    std::int8_t prop;
    bool exact;
};
constexpr ModelMap kModelMap[31] = {
    {kPropNone, false},  // 0
    {kPropS, true},  {kPropM, true},  {kPropL, true},   // 1-3  bsm0 bmd0 bla0
    {kPropS, true},  {kPropM, true},  {kPropL, true},   // 4-6  bsm0 bmd0 bla0
    {kPropNone, false}, {kPropNone, false}, {kPropNone, false},  // 7-9 builders (pzbr)
    {kPropS, true},                                     // 10 bsm0
    {kPropNone, false}, {kPropNone, false},             // 11-12 floc cart
    {kPropXL, true},                                    // 13 blx0
    {kPropNone, false},                                 // 14 flad ladder
    {kPropS, false}, {kPropM, false}, {kPropL, false},  // 15-17 weapon cases wsm0 wmd0 wla0
    {kPropS, false}, {kPropM, false}, {kPropL, false}, {kPropXL, false},  // 18-21 material tanks mts0 mtm0 mtl0 mtx0
    {kPropXL, true},                                    // 22 blx0 (bomb)
    {kPropNone, false}, {kPropNone, false}, {kPropNone, false}, {kPropNone, false},  // 23-26 shields
    {kPropS, false},                                    // 27 bsm1
    {kPropNone, false},                                 // 28 heater
    {kPropCube, true},                                  // 29 blx1
    {kPropXL, false},                                   // 30 blx2
};

int PropFor(const wire::GearItem& it) {
    switch (it.kind) {
        case wire::GearKind::backpack: return kPropBackpack;
        case wire::GearKind::odradek: return kPropOdradek;
        case wire::GearKind::bb_pod: return kPropBbPod;
        case wire::GearKind::cargo: return it.model < 31 ? kModelMap[it.model].prop : (it.model == wire::kGearNone ? kPropM : kPropNone);
    }
    return kPropNone;
}

// ---------------------------------------------------------------- math
struct Xf {
    double p[3] = {};
    float r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // rows: right, forward, up
};

Xf Compose(const Xf& a, const double (&p)[3], const float (&r)[3][3]) {
    Xf o;
    for (int k = 0; k < 3; ++k) {
        o.p[k] = a.p[k] + p[0] * a.r[0][k] + p[1] * a.r[1][k] + p[2] * a.r[2][k];
        for (int c = 0; c < 3; ++c) o.r[k][c] = r[k][0] * a.r[0][c] + r[k][1] * a.r[1][c] + r[k][2] * a.r[2][c];
    }
    return o;
}
// b in a's frame
void Relative(const Xf& a, const Xf& b, double (&p)[3], float (&r)[3][3]) {
    const double d[3] = {b.p[0] - a.p[0], b.p[1] - a.p[1], b.p[2] - a.p[2]};
    for (int i = 0; i < 3; ++i) {
        p[i] = d[0] * a.r[i][0] + d[1] * a.r[i][1] + d[2] * a.r[i][2];
        for (int k = 0; k < 3; ++k) r[k][i] = b.r[k][0] * a.r[i][0] + b.r[k][1] * a.r[i][1] + b.r[k][2] * a.r[i][2];
    }
}
double Dist(const Xf& a, const Xf& b) {
    const double x = a.p[0] - b.p[0], y = a.p[1] - b.p[1], z = a.p[2] - b.p[2];
    return std::sqrt(x * x + y * y + z * z);
}

bool ReadWorldAt(std::uintptr_t at, Xf* out) {
#pragma pack(push, 4)
    struct {
        double p[3];
        float r[3][3];
    } a{}, b{};
#pragma pack(pop)
    static_assert(sizeof(a) == 0x3C, "world transform");
    if (!at || !mem::Read(at, &a, sizeof(a)) || !mem::Read(at, &b, sizeof(b)) || std::memcmp(&a, &b, sizeof(a)) != 0) return false;
    for (double v : a.p)
        if (!std::isfinite(v)) return false;
    std::memcpy(out->p, a.p, sizeof(out->p));
    std::memcpy(out->r, a.r, sizeof(out->r));
    return true;
}

bool ReadWorld(std::uintptr_t e, Xf* out) { return e && ReadWorldAt(e + kEntWorld, out); }

// A joint of the entity's pose in world space (model matrix axes normalised).
bool JointWorld(game::Entity* e, const Xf& world, unsigned j, Xf* out) {
    float m[4][4];
    if (!realbody::ReadJointModel(e, j, m)) return false;
    double p[3];
    float r[3][3];
    for (int k = 0; k < 3; ++k) {
        const float len = std::sqrt(m[k][0] * m[k][0] + m[k][1] * m[k][1] + m[k][2] * m[k][2]);
        if (!std::isfinite(len) || len < 0.2f || len > 5.0f || !std::isfinite(m[3][k])) return false;
        for (int c = 0; c < 3; ++c) r[k][c] = m[k][c] / len;
        p[k] = m[3][k];
    }
    *out = Compose(world, p, r);
    return true;
}

// ---------------------------------------------------------------- state
struct Checks {
    bool done = false, ok = false;
    char why[160] = "not checked";
} g_chk;

bool g_enabled = true;
bool g_partner_on = true, g_self_on = true;
int g_test_cases = 0;       // hidden tests: this many synthetic delivery cases on our backpack (0 none)
bool g_debug_body = false;  // our own gear on the debug body (real_spawn)

// The local snapshot (sender side; also the self view's props).
struct LocalItem {
    wire::GearKind kind;
    std::uint8_t model, variation, slot_type;
    std::uintptr_t e;
    bool visible;
    Xf world;
};
constexpr unsigned kMaxLocal = 128;
LocalItem g_items[kMaxLocal];
unsigned g_item_count = 0;
wire::GearFrame g_local;           // the frame we show and send (rel poses refreshed when still)
bool g_have_local = false;
double g_next_read_ms = 0;
Xf g_prev_sam;
bool g_have_prev_sam = false;
bool g_vis_memo[4] = {true, true, true, true};  // per kind: visible the last time Sam was not hidden by our self view
std::uint32_t g_reads = 0, g_read_fail = 0, g_moving_skips = 0, g_layout_held = 0;
// A new layout is taken only when the next read (100 ms later) agrees: one torn or refused read
// (an item dropped, every later index shifted) must not respawn the partner's props.
bool g_pending = false;
std::uint16_t g_pending_hash = 0;
std::uint8_t g_pending_count = 0;
std::uint16_t g_logged_hash = 0xFFFF;

// Sender schedule.
wire::GearFrame g_sent;
bool g_have_sent = false;
double g_last_send_ms = 0;

// Receiver.
wire::GearFrame g_partner;
bool g_have_partner = false;

struct Prop {
    int def = kPropNone;   // kProps index; kPropNone = no prop for this item
    holo::Spawn sp;
    bool loading = false, live = false, failed = false, shown = false;
    std::uintptr_t vt = 0;
    game::Guid guid;
};
struct PropSet {
    const char* tag;
    realbody::GearSlot slot;
    game::Entity* body = nullptr;  // the body the props belong to
    Prop props[wire::kMaxGearItems];
    wire::GearFrame want;          // the layout + poses the props follow
    bool have_want = false;
    std::uint16_t published_hash = 0;
    unsigned published_n = 0;
    bool published = false;
    double asleep_since = 0;
    unsigned spawned = 0, failed = 0, removed = 0;
    std::uint32_t refused = 0;     // bit per prop def that failed to load / validate (this session)
    double next_stats_ms = 0;
};
PropSet g_sets[2] = {{"gear", realbody::GearSlot::partner}, {"gear-self", realbody::GearSlot::self}};

bool Check(const char* name, std::uintptr_t rva, const char* sig) {
    if (mem::CodeMatches(mem::Base() + rva, sig)) return true;
    std::snprintf(g_chk.why, sizeof(g_chk.why), "%s at ds.exe+0x%llX does not match", name, static_cast<unsigned long long>(rva));
    return false;
}

bool Checked() {
    if (g_chk.done) return g_chk.ok;
    g_chk.done = true;
    const std::uintptr_t b = mem::Base();
    bool ok = Check("inventory store", kInitInvStore, "48 89 83 C8 54 00 00") && Check("carrier store", kInitCarStore, "48 89 83 D0 54 00 00") &&
              Check("Odradek store", kInvOdraStore, "4C 89 AF A0 4C 00 00") && Check("backpack store", kInvPackStore, "4C 89 BF B8 4C 00 00") &&
              Check("carrier bind", kCarrierBind, "48 8B 41 40 48 89 51 50 48 89 50 30 C3") &&
              Check("slot lookup", kSlotLookup, "48 8B 41 18 48 63 49 10 4C 8D 04 C8") && Check("slot walk", kSlotWalk, "8B 51 0C 85 D2") &&
              Check("slot stacks", kSlotWalk2, "4C 8B 73 20 48 63 43 18") && Check("stack items", kSlotWalk3, "48 8B 58 18 48 63 40 10") &&
              Check("baggage type check", kBagSetWorld, "48 8B 81 30 03 00 00 80 78 21 01");
    if (ok && Q(b + kPlayerVtable) == 0) {
        std::snprintf(g_chk.why, sizeof(g_chk.why), "DSPlayerEntity vtable unreadable");
        ok = false;
    }
    g_chk.ok = ok;
    if (ok) std::snprintf(g_chk.why, sizeof(g_chk.why), "ok");
    Log("[gear] %s", ok ? "inventory / carrier / baggage offsets verified" : g_chk.why);
    return ok;
}

const char* TypeOf(std::uintptr_t obj) {
    const char* n = obj ? mem::RttiName(mem::RttiOf(obj)) : nullptr;
    return n ? n : "?";
}
bool Derives(std::uintptr_t obj, const char* name) { return obj && mem::DerivesFrom(mem::RttiOf(obj), name); }

bool Visible(std::uintptr_t e) {
    std::uint64_t f = 0;
    return Rd(e + kEntFlags, f) && (f & 0x2) != 0;
}

// An inventory weak reference (value - 0x20 is the entity) or a direct entity pointer.
std::uintptr_t RefEntity(std::uintptr_t v) {
    if (!v) return 0;
    if (Derives(v - 0x20, "Entity")) return v - 0x20;
    if (Derives(v, "Entity")) return v;
    return 0;
}

// The baggage list item (DSGameBaggageListItem: +0x3A attribute, +0x3B case type, +0x3E volume;
// dsdc_types) -> EDSBaggage2Model, for a piece whose own model byte is not usable.
std::uint8_t ModelFromListItem(std::uintptr_t li) {
    std::uint8_t f[5] = {};
    if (!li || !mem::Read(li + kListAttribute, f, sizeof(f))) return wire::kGearNone;
    const std::uint8_t attr = f[0], kase = f[1], vol = f[4];
    const std::uint8_t size = vol > 2 ? 2 : vol;  // S, M, L (LL: L)
    switch (kase) {
        case 0: return static_cast<std::uint8_t>((attr == 1 ? 4 : 1) + size);  // Normal (Personal: 4..6)
        case 1: return 13;                                                       // LiquidOnly: barrel
        case 2: return static_cast<std::uint8_t>(15 + size);                     // Weapon cases (drawn as S/M/L cases)
        case 9: return static_cast<std::uint8_t>(vol >= 3 ? 21 : 18 + size);     // Material
        case 10: return 11;                                                      // Cart
        case 12: return 14;                                                      // Ladder
        case 17: return 27;                                                      // SmallDelicate
        case 18: return 30;                                                      // ToxicGas
        default: return wire::kGearNone;
    }
}

// Which carried pieces are drawn as a case: deliveries (case type Normal, LiquidOnly, Material,
// Delicate, SmallDelicate, ToxicGas) anywhere, and a weapon on the waist (Sam shows it as a small
// case at his hip). Equipment, weapons on the back and arms, tools (ladder, cart, builders), BB pod,
// bodies: not drawn (in game they sit in the backpack's pouches or are models we have no prop for).
bool Drawn(std::uint8_t kase, std::uint8_t slot_type) {
    switch (kase) {
        case 0: case 1: case 9: case 13: case 17: case 18: return true;
        case 2: return slot_type == 4 || slot_type == 5;
        default: return false;
    }
}

// A DSBaggage2's world transform (+0x38 position, 3 doubles; +0x50 rotation rows, 9 floats: the
// entity world-transform layout), read twice; refused unless finite and a rotation.
bool BaggageWorld(std::uintptr_t bag, Xf* out) {
    if (!ReadWorldAt(bag + kBagWorld, out)) return false;
    for (int k = 0; k < 3; ++k) {
        const float* r = out->r[k];
        const float len = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        if (!(len > 0.9f && len < 1.1f)) return false;
    }
    return true;
}

// Reads every gear item of the local player into g_items. `verbose`: the full walk to the log.
bool ReadItems(game::Entity* local, bool verbose, char* why, std::size_t cap) {
    g_item_count = 0;
    const auto p = reinterpret_cast<std::uintptr_t>(local);
    if (!p || Q(p) != mem::Base() + kPlayerVtable) {
        std::snprintf(why, cap, "no local DSPlayerEntity");
        return false;
    }
    const std::uintptr_t inv = Q(p + kPInv);
    if (!inv || Q(inv + kCompOwner) != p || std::strcmp(TypeOf(inv), "DSPlayerInventoryComponent") != 0) {
        std::snprintf(why, cap, "the inventory component does not check out");
        return false;
    }
    Xf sam;
    if (!ReadWorld(p, &sam)) {
        std::snprintf(why, cap, "the player's transform is unreadable");
        return false;
    }
    auto add = [&](wire::GearKind k, std::uint8_t model, std::uint8_t slot, std::uintptr_t obj, bool visible, const Xf& w) {
        if (g_item_count >= kMaxLocal) return;
        LocalItem& it = g_items[g_item_count++];
        it.kind = k;
        it.model = model;
        it.variation = wire::kGearNone;
        it.slot_type = slot;
        it.e = obj;
        it.visible = visible;
        it.world = w;
    };
    auto add_entity = [&](wire::GearKind k, std::uintptr_t e) {
        Xf w;
        if (e && ReadWorld(e, &w) && Dist(w, sam) < 5.0) add(k, wire::kGearNone, 0, e, Visible(e), w);
    };
    const std::uintptr_t pack = RefEntity(Q(inv + kInvPack));
    if (pack && Derives(pack, "DSBackpackEntity")) add_entity(wire::GearKind::backpack, pack);
    // The Odradek is read for the log only: the cutscene prop stands in its unfolded bind pose (run
    // 20261117-104819: a gold arm hanging off the left shoulder), not folded on the shoulder.
    const std::uintptr_t odra = RefEntity(Q(inv + kInvOdra));
    if (verbose)
        Log("[gear] inventory %p: backpack %p (%s), Odradek %p (%s)", reinterpret_cast<void*>(inv), reinterpret_cast<void*>(pack),
            pack ? TypeOf(pack) : "-", reinterpret_cast<void*>(odra), odra ? TypeOf(odra) : "-");

    // The carrier: slots -> stacks -> DSBaggage2. Carried pieces have no entity of their own (their
    // entity index +0xD8 is -1); the DSBaggage2 keeps its own world transform and model.
    const std::uintptr_t comp = Q(p + kPCarrier);
    if (!comp || Q(comp + kCompOwner) != p || !Derives(comp, "DSBaggage2CarrierComponent")) {
        if (verbose) Log("[gear] the baggage carrier component %p (%s) does not check out", reinterpret_cast<void*>(comp), comp ? TypeOf(comp) : "-");
        return true;
    }
    const std::uintptr_t car = Q(comp + kCarrierObj);
    std::int32_t slots = 0;
    if (!car || !Rd(car + kCarSlotN, slots) || slots < 0 || slots > 64) {
        if (verbose) Log("[gear] carrier %p: %d slots: not walked", reinterpret_cast<void*>(car), slots);
        return true;
    }
    const std::uintptr_t slot_arr = Q(car + kCarSlots);
    unsigned pieces = 0, refused = 0, walked = 0;
    constexpr unsigned kMaxWalk = 256;  // stacks + pieces per read: garbage counts cannot make 64^3 reads
    bool cut = false;
    for (std::int32_t si = 0; si < slots && !cut; ++si) {
        const std::uintptr_t s = Q(slot_arr + 8ull * static_cast<unsigned>(si));
        std::uint8_t type = 0;
        std::int32_t nstack = 0;
        if (!s || !Rd(s + kSlotType, type) || !Rd(s + kSlotStackN, nstack) || nstack < 0 || nstack > 64) continue;
        if (verbose && nstack > 0) {
            std::int32_t total = -1;
            Rd(s + kSlotTotal, total);
            Log("[gear]   slot %d type %u: %d stack(s), %d piece(s)%s", si, type, nstack, total, type >= 1 && type <= 7 ? "" : " (not shown)");
        }
        if (type < 1 || type > 7) continue;  // back, arms, waist, hands
        const std::uintptr_t st_arr = Q(s + kSlotStacks);
        for (std::int32_t ti = 0; ti < nstack && !cut; ++ti) {
            if (++walked > kMaxWalk) {
                cut = true;
                break;
            }
            const std::uintptr_t t = Q(st_arr + 8ull * static_cast<unsigned>(ti));
            std::int32_t n = 0;
            if (!t || !Rd(t + kStackN, n) || n < 0 || n > 64) continue;
            const std::uintptr_t items = Q(t + kStackItems);
            for (std::int32_t bi = 0; bi < n; ++bi) {
                if (++walked > kMaxWalk) {
                    cut = true;
                    break;
                }
                const std::uintptr_t bag = Q(items + 8ull * static_cast<unsigned>(bi));
                if (!bag || std::strcmp(TypeOf(bag), "DSBaggage2") != 0) {
                    ++refused;
                    continue;
                }
                std::uint8_t mb[2] = {};
                mem::Read(bag + kBagModel, mb, sizeof(mb));
                const std::uintptr_t li = Q(bag + kBagListItem);
                const bool li_ok = li && std::strcmp(TypeOf(li), "DSGameBaggageListItem") == 0;
                std::uint8_t lf[5] = {};
                if (li_ok) mem::Read(li + kListAttribute, lf, sizeof(lf));
                const std::uint8_t from_li = li_ok ? ModelFromListItem(li) : wire::kGearNone;
                const std::uint8_t model = Drawn(lf[1], type) ? from_li : wire::kGearNone;
                Xf w;
                const bool placed = BaggageWorld(bag, &w) && Dist(w, sam) < 6.0;
                if (verbose && pieces < 48) {
                    std::uint8_t f[5] = {};
                    float weight = 0;
                    std::uint32_t id = 0;
                    if (li_ok) {
                        mem::Read(li + kListAttribute, f, sizeof(f));
                        Rd(li + kListWeight, weight);
                        Rd(li + kListId, id);
                    }
                    Log("[gear]     slot type %u stack %d piece %d: %p model byte %u (+0x21 %u), list item %08X attribute %u case %u volume %u "
                        "weight %.1f -> model %u (%s); at %.3f %.3f %.3f (%.2f m from Sam)%s",
                        type, ti, bi, reinterpret_cast<void*>(bag), mb[0], mb[1], id, f[0], f[1], f[4], weight, from_li,
                        model != wire::kGearNone ? "drawn" : "not drawn: equipment / weapon / tool", w.p[0], w.p[1], w.p[2], Dist(w, sam),
                        placed ? "" : " NOT PLACED");
                }
                ++pieces;
                if (placed && model != wire::kGearNone) add(wire::GearKind::cargo, model, type, bag, true, w);
            }
        }
    }
    if (cut) {
        static bool logged = false;
        if (!logged || verbose) Log("[gear] carrier %p: more than %u stacks + pieces: walk cut short", reinterpret_cast<void*>(car), kMaxWalk);
        logged = true;
    }
    if (verbose) Log("[gear] carrier %p: %d slots, %u pieces read (%u refused)", reinterpret_cast<void*>(car), slots, pieces, refused);
    return true;
}

// Builds the frame from g_items: anchors and relative poses. `prev`: keep its poses for items whose
// layout matches (Sam moving: the tick's reads of Sam and the gear may be a frame apart).
bool BuildFrame(game::Entity* local, const Xf& sam, bool self_hidden, bool keep_poses, wire::GearFrame* out) {
    wire::GearFrame f;
    f.flags = wire::kGearValid;
    // Joint frames of Sam (model matrices) in world space.
    Xf joints[77];
    bool have_joint[77] = {};
    auto joint = [&](unsigned j) -> const Xf* {
        if (j >= 77) return nullptr;
        if (!have_joint[j]) have_joint[j] = JointWorld(local, sam, j, &joints[j]);
        return have_joint[j] ? &joints[j] : nullptr;
    };
    int pack_index = -1;
    Xf pack_world;
    unsigned cargo = 0, dropped = 0;
    for (unsigned i = 0; i < g_item_count; ++i) {
        const LocalItem& it = g_items[i];
        const unsigned kind = static_cast<unsigned>(it.kind);
        // Visibility: our own self view hides Sam (reason 63), and the game then hides the backpack,
        // Odradek and pod too; their last state from before is used. Cargo in a carrier slot counts as
        // carried.
        bool vis = it.visible;
        if (it.kind != wire::GearKind::cargo && kind < 4) {
            if (self_hidden) vis = g_vis_memo[kind];
            else g_vis_memo[kind] = it.visible;
        } else {
            vis = true;
        }
        if (!vis) continue;
        if (it.kind == wire::GearKind::cargo) ++cargo;
        if (f.count >= wire::kMaxGearItems) {
            ++dropped;
            continue;
        }
        wire::GearItem gi;
        gi.kind = it.kind;
        gi.model = it.model;
        gi.variation = it.variation;
        const Xf* anchor = nullptr;
        if (it.kind == wire::GearKind::cargo && it.slot_type == 1 && pack_index >= 0) {
            gi.parent = static_cast<std::uint8_t>(pack_index);
            anchor = &pack_world;
        } else {
            std::uint8_t j = kJSpine4;
            if (it.kind == wire::GearKind::odradek) j = kJLShoulder;
            else if (it.kind == wire::GearKind::bb_pod) j = kJSpine5;
            else if (it.kind == wire::GearKind::cargo && it.slot_type != 1) {
                // arms (slot types 2, 3) on the upper arm, waist (4, 5) on the hips, hands (6, 7) on the
                // hand: of a left / right pair the nearer joint (the pair is far apart: a stable choice)
                std::uint8_t a = kJHips, b = kJHips;
                if (it.slot_type == 2 || it.slot_type == 3) a = kJRArm, b = kJLArm;
                if (it.slot_type == 6 || it.slot_type == 7) a = kJRHand, b = kJLHand;
                const Xf* ja = joint(a);
                const Xf* jb = joint(b);
                j = !jb || (ja && Dist(*ja, it.world) <= Dist(*jb, it.world)) ? a : b;
            }
            gi.joint = j;
            anchor = joint(j);
        }
        if (!anchor) continue;
        double rp[3];
        float rr[3][3];
        Relative(*anchor, it.world, rp, rr);
        if (std::fabs(rp[0]) > 30 || std::fabs(rp[1]) > 30 || std::fabs(rp[2]) > 30) continue;
        gi.p[0] = static_cast<float>(rp[0]), gi.p[1] = static_cast<float>(rp[1]), gi.p[2] = static_cast<float>(rp[2]);
        gi.q = wire::Normalize(wire::QuatFromMatrix(rr));
        if (it.kind == wire::GearKind::backpack && pack_index < 0) {
            pack_index = static_cast<int>(f.count);
            pack_world = it.world;
        }
        f.items[f.count++] = gi;
    }
    // Hidden tests only: synthetic delivery cases stacked on the backpack (large, medium, small, ...),
    // in the innermost of the layers the game uses on Sam's backpack (run 20261117-105905: layers 0.13 m
    // apart outwards along the backpack's y axis, the bottom piece 0.12 m out and 0.16 m below its origin).
    for (int k = 0; k < g_test_cases && pack_index >= 0; ++k) {
        if (f.count >= wire::kMaxGearItems) {
            ++dropped;
            continue;
        }
        static const std::uint8_t kSizes[3] = {3, 2, 1};
        static const float kHeight[3] = {0.40f, 0.30f, 0.22f};
        wire::GearItem gi;
        gi.kind = wire::GearKind::cargo;
        gi.model = kSizes[k % 3];
        gi.parent = static_cast<std::uint8_t>(pack_index);
        float z = -0.16f;
        for (int m = 0; m < k; ++m) z += kHeight[m % 3];
        gi.p[0] = 0.0f, gi.p[1] = -0.118f, gi.p[2] = z;
        f.items[f.count++] = gi;
        ++cargo;
    }
    f.dropped = static_cast<std::uint8_t>(dropped > 255 ? 255 : dropped);
    if (keep_poses && g_have_local && wire::GearLayoutHash(f) == wire::GearLayoutHash(g_local) && f.count == g_local.count) {
        for (unsigned i = 0; i < f.count; ++i) {
            std::memcpy(f.items[i].p, g_local.items[i].p, sizeof(f.items[i].p));
            f.items[i].q = g_local.items[i].q;
        }
    }
    *out = f;
    return true;
}

void DescribeFrame(const wire::GearFrame& f, const char* what) {
    unsigned per[kPropCount] = {}, none = 0, approx = 0, pack = 0, odra = 0, pod = 0, cargo = 0;
    for (unsigned i = 0; i < f.count; ++i) {
        const wire::GearItem& it = f.items[i];
        const int p = PropFor(it);
        if (p >= 0) ++per[p];
        else ++none;
        if (it.kind == wire::GearKind::cargo) {
            ++cargo;
            if (it.model >= 31 || !kModelMap[it.model].exact) ++approx;
        }
        pack += it.kind == wire::GearKind::backpack;
        odra += it.kind == wire::GearKind::odradek;
        pod += it.kind == wire::GearKind::bb_pod;
    }
    Log("[gear] %s: %u items (backpack %u, Odradek %u, BB pod %u, cargo %u: %u approximate, %u not drawn; dropped over the cap %u), layout %04X; "
        "props S %u M %u L %u XL %u cube %u",
        what, f.count, pack, odra, pod, cargo, approx, none, f.dropped, wire::GearLayoutHash(f), per[kPropS], per[kPropM], per[kPropL], per[kPropXL],
        per[kPropCube]);
}

void ReadLocalTick(game::Entity* local, bool local_active, double now) {
    if (now < g_next_read_ms) return;
    g_next_read_ms = now + kReadEveryMs;
    if (!local || !local_active || !Checked()) return;
    char why[160] = {};
    Xf sam;
    if (!ReadWorld(reinterpret_cast<std::uintptr_t>(local), &sam) || !ReadItems(local, false, why, sizeof(why))) {
        ++g_read_fail;
        return;
    }
    ++g_reads;
    // Moving or turning since the last read: keep the poses (only a new layout is taken at once).
    bool moving = false;
    if (g_have_prev_sam) {
        const double d = Dist(sam, g_prev_sam);
        const float dot = sam.r[1][0] * g_prev_sam.r[1][0] + sam.r[1][1] * g_prev_sam.r[1][1] + sam.r[1][2] * g_prev_sam.r[1][2];
        moving = d > 0.02 || dot < 0.9994f;  // 2 cm or about 2 degrees in 100 ms
    }
    g_prev_sam = sam;
    g_have_prev_sam = true;
    wire::GearFrame f;
    if (!BuildFrame(local, sam, realbody::SelfShown(), moving, &f)) return;
    const std::uint16_t nh = wire::GearLayoutHash(f);
    if (g_have_local && (nh != wire::GearLayoutHash(g_local) || f.count != g_local.count)) {
        if (!(g_pending && g_pending_hash == nh && g_pending_count == f.count)) {
            g_pending = true, g_pending_hash = nh, g_pending_count = f.count;
            ++g_layout_held;
            return;
        }
    }
    g_pending = false;
    if (moving && g_have_local && wire::GearLayoutHash(f) == wire::GearLayoutHash(g_local)) ++g_moving_skips;
    g_local = f;
    g_have_local = true;
    const std::uint16_t h = wire::GearLayoutHash(f);
    if (h != g_logged_hash) {
        g_logged_hash = h;
        DescribeFrame(f, "our gear");
    }
}

// ---------------------------------------------------------------- props (game thread)
void Unpublish(PropSet& s) {
    if (!s.published) return;
    realbody::PublishGear(s.slot, nullptr, 0);
    s.published = false;
}

void RemoveProp(PropSet& s, unsigned i, const char* why) {
    Prop& p = s.props[i];
    if (p.loading || p.live || p.sp.sp) {
        char step[160] = {};
        holo::SpawnTeardown(p.sp, why, step, sizeof(step));
        ++s.removed;
    }
    p = Prop{};
}

void RemoveAll(PropSet& s, const char* why) {
    Unpublish(s);
    unsigned n = 0;
    for (unsigned i = 0; i < wire::kMaxGearItems; ++i)
        if (s.props[i].loading || s.props[i].live || s.props[i].sp.sp) {
            RemoveProp(s, i, why);
            ++n;
        }
    if (n) Log("[%s] %u prop(s) removed (%s)", s.tag, n, why);
    s.body = nullptr;
    s.published_n = 0;
}

void PublishNow(PropSet& s) {
    static realbody::GearProp list[realbody::kMaxGearProps];
    const wire::GearFrame& f = s.want;
    unsigned n = 0;
    for (unsigned i = 0; i < f.count && i < realbody::kMaxGearProps; ++i) {
        const wire::GearItem& it = f.items[i];
        realbody::GearProp& g = list[n++];
        g = realbody::GearProp{};
        const Prop& p = s.props[i];
        if (p.live) {
            g.e = reinterpret_cast<std::uintptr_t>(p.sp.e);
            g.e_vt = p.vt;
            g.guid = p.guid;
        }
        g.joint = it.joint;
        g.parent = it.parent == wire::kGearNoParent ? std::int8_t{-1} : static_cast<std::int8_t>(it.parent);
        g.p[0] = it.p[0], g.p[1] = it.p[1], g.p[2] = it.p[2];
        wire::MatrixFromQuat(it.q, g.r);
    }
    // Unpublish first when a prop of the old list may go away (the caller removes only after this).
    realbody::PublishGear(s.slot, list, n);
    s.published = n > 0;
    s.published_n = n;
}

bool ValidateProp(game::Entity* e, const char** why) {
    const auto a = reinterpret_cast<std::uintptr_t>(e);
    const std::uintptr_t rtti = mem::RttiOf(a);
    if (!rtti) return (*why = "entity type unknown"), false;
    if (mem::DerivesFrom(rtti, "Humanoid") || mem::DerivesFrom(rtti, "DSPlayerEntity")) return (*why = "a Humanoid / player entity"), false;
    for (const char* bad : {"AIIndividualComponent", "AIBodyComponent", "DSPlayerControllerComponent", "HumanoidController", "CapsuleComponent",
                            "PhysicsCollisionComponent", "PhysicsRagdollComponent"})
        if (partner::FindComponentByName(e, bad)) return (*why = "carries AI, controller or physics components"), false;
    return true;
}

void SetShown(PropSet& s, unsigned i, bool on) {
    Prop& p = s.props[i];
    if (!p.live || p.shown == on) return;
    if (!holo::SpawnAlive(p.sp)) return;
    if (partner::SetVisible(p.sp.e, on)) p.shown = on;
}

void DriveSet(PropSet& s, game::Entity* body, bool body_visible, bool local_active, double now) {
    if (!local_active) {
        if (s.asleep_since == 0) s.asleep_since = now > 0 ? now : 1;
        for (unsigned i = 0; i < wire::kMaxGearItems; ++i) SetShown(s, i, false);
        if (now - s.asleep_since >= 1000 && s.body) RemoveAll(s, "Sam asleep");
        return;
    }
    s.asleep_since = 0;
    if (body != s.body) {
        if (s.body) RemoveAll(s, body ? "the body changed" : "the body is gone");
        s.body = body;
        if (body) s.published_hash = 0xFFFF;
    }
    if (!body) return;
    if (!s.have_want) {
        RemoveAll(s, "no gear");
        s.body = body;
        return;
    }
    const wire::GearFrame& f = s.want;
    const std::uint16_t h = wire::GearLayoutHash(f);
    // A new layout: props whose kind changes go (after the detour stopped using the old list).
    if (h != s.published_hash) {
        bool any_change = false;
        for (unsigned i = 0; i < wire::kMaxGearItems; ++i) {
            const int want = i < f.count ? PropFor(f.items[i]) : kPropNone;
            const Prop& p = s.props[i];
            if ((p.loading || p.live || p.failed) && p.def != want) any_change = true;
        }
        if (any_change) {
            Unpublish(s);
            unsigned gone = 0;
            for (unsigned i = 0; i < wire::kMaxGearItems; ++i) {
                const int want = i < f.count ? PropFor(f.items[i]) : kPropNone;
                Prop& p = s.props[i];
                if ((p.loading || p.live || p.failed) && p.def != want) {
                    RemoveProp(s, i, "layout changed");
                    ++gone;
                }
            }
            Log("[%s] layout %04X -> %04X: %u prop(s) removed", s.tag, s.published_hash, h, gone);
        }
        s.published_hash = h;
        DescribeFrame(f, s.tag);
    }
    // Spawn the missing props (a few per tick), poll the loading ones.
    unsigned begun = 0, loading = 0;
    game::WorldTransform at{};
    const bool have_at = game::TryGetTransform(body, &at);
    at.pos.z -= 20.0;  // out of sight until the first placement
    bool changed = false;
    for (unsigned i = 0; i < f.count; ++i) {
        Prop& p = s.props[i];
        const int want = PropFor(f.items[i]);
        if (want < 0 || (s.refused & (1u << want))) continue;
        if (!p.loading && !p.live && !p.failed && have_at && begun < 4) {
            char why[200] = {};
            if (realbody::BeginByPath(kProps[want].path, kProps[want].guid, at, 0x6E000000u + i, &p.sp, why, sizeof(why))) {
                p.def = want;
                p.loading = true;
                ++begun;
            } else {
                p.def = want;
                p.failed = true;
                ++s.failed;
                Log("[%s] prop %u '%s' not started: %s", s.tag, i, kProps[want].key, why);
            }
        }
        if (p.loading) {
            ++loading;
            char why[200] = {};
            switch (holo::SpawnPoll(p.sp, why, sizeof(why))) {
                case holo::SpawnState::loading: break;
                case holo::SpawnState::cancelled:
                case holo::SpawnState::failed:
                    Log("[%s] prop %u '%s' did not load: %s", s.tag, i, kProps[want].key, why);
                    RemoveProp(s, i, "load failed");
                    s.props[i].def = want;
                    s.props[i].failed = true;
                    s.refused |= 1u << want;
                    ++s.failed;
                    break;
                case holo::SpawnState::realized: {
                    p.loading = false;
                    const char* bad = nullptr;
                    if (!p.sp.e || !ValidateProp(p.sp.e, &bad)) {
                        Log("[%s] prop %u '%s' refused: %s", s.tag, i, kProps[want].key, bad ? bad : "no entity");
                        if (p.sp.e) partner::DescribeEntity(p.sp.e, "refused gear prop");
                        RemoveProp(s, i, "refused");
                        s.props[i].def = want;
                        s.props[i].failed = true;
                        s.refused |= 1u << want;
                        ++s.failed;
                        break;
                    }
                    partner::SetVisible(p.sp.e, false);
                    partner::WakeIfAsleep(p.sp.e);
                    partner::ReadVtable(p.sp.e, &p.vt);
                    partner::ReadGuid(p.sp.e, &p.guid);
                    p.live = true;
                    p.shown = false;
                    ++s.spawned;
                    changed = true;
                    static bool described[kPropCount] = {};
                    if (!described[want]) {
                        described[want] = true;
                        partner::DescribeEntity(p.sp.e, kProps[want].key);
                    }
                    break;
                }
            }
        }
        if (p.live && !holo::SpawnAlive(p.sp)) {
            Log("[%s] prop %u '%s' was destroyed by the game", s.tag, i, kProps[want].key);
            Unpublish(s);
            p = Prop{};
            changed = true;
        }
    }
    // Publish when anything changed (new props live, new poses).
    static wire::GearFrame last_pub[2];
    const int si = s.slot == realbody::GearSlot::self ? 1 : 0;
    float dm = 0, dd = 0;
    const bool poses_moved = !wire::GearPoseDelta(last_pub[si], f, &dm, &dd) || dm > 0.0005f || dd > 0.05f;
    if (changed || poses_moved || !s.published) {
        PublishNow(s);
        last_pub[si] = f;
    }
    // Place now from the tick too (the detour places them every frame the body is updated).
    const unsigned placed = realbody::PlaceGearNow(s.slot);
    // Visibility: with the body.
    for (unsigned i = 0; i < f.count; ++i) SetShown(s, i, body_visible && placed > 0);
    if (now >= s.next_stats_ms && s.published_n) {
        s.next_stats_ms = now + 5000;
        unsigned live = 0, shown = 0;
        for (unsigned i = 0; i < f.count; ++i) live += s.props[i].live, shown += s.props[i].shown;
        std::uint32_t pl = 0, aba = 0, bad = 0;
        realbody::GearCounters(s.slot, &pl, &aba, &bad);
        Log("[%s] %u items, %u props live (%u loading, %u shown, %u failed, %u removed); placements %u (aba %u, bad matrix %u)", s.tag, f.count, live,
            loading, shown, s.failed, s.removed, pl, aba, bad);
    }
}

}  // namespace

void Init(const Config& config) noexcept {
    g_enabled = config.partner_gear;
    Log("[gear] gear on bodies %s (partner_gear=%d)", g_enabled ? "on" : "off", g_enabled ? 1 : 0);
}

bool Enabled() noexcept { return g_enabled; }

void Tick(game::Entity* local, bool local_active, double now_ms) noexcept {
    if (!g_enabled || !realbody::HookInstalled()) return;
    ReadLocalTick(local, local_active, now_ms);
    // The partner slot: the partner's real body (or, debug, a mirroring debug body with our gear).
    PropSet& ps = g_sets[0];
    game::Entity* pbody = g_partner_on ? realbody::SlotEntity(realbody::GearSlot::partner) : nullptr;
    const bool debug = realbody::GetOwner() == realbody::Owner::debug;
    if (debug && !g_debug_body) pbody = nullptr;
    if (debug) {
        ps.want = g_local;
        ps.have_want = g_have_local;
    } else {
        ps.want = g_partner;
        ps.have_want = g_have_partner;
    }
    std::uint64_t flags = 0;
    const bool pvis = pbody && partner::ReadFlags(pbody, &flags) && (flags & 0x2) != 0;
    DriveSet(ps, pbody, pvis, local_active, now_ms);
    // The self view: only while the self body is shown in Sam's place.
    PropSet& ss = g_sets[1];
    game::Entity* sbody = g_self_on ? realbody::SlotEntity(realbody::GearSlot::self) : nullptr;
    ss.want = g_local;
    ss.have_want = g_have_local;
    DriveSet(ss, sbody, sbody && realbody::SelfShown(), local_active, now_ms);
}

bool TakeSend(double now_ms, wire::GearFrame* out) noexcept {
    if (!g_enabled || !g_have_local || !out) return false;
    const bool layout = !g_have_sent || wire::GearLayoutHash(g_sent) != wire::GearLayoutHash(g_local) || g_sent.count != g_local.count;
    float dm = 0, dd = 0;
    const bool moved = !layout && wire::GearPoseDelta(g_sent, g_local, &dm, &dd) && (dm > 0.02f || dd > 4.0f);
    const double since = now_ms - g_last_send_ms;
    if (!(layout || (moved && since >= 250) || since >= 1000)) return false;
    g_sent = g_local;
    g_have_sent = true;
    g_last_send_ms = now_ms;
    *out = g_local;
    return true;
}

void SetPartnerGear(const wire::GearFrame* f) noexcept {
    if (!f) {
        if (g_have_partner) Log("[gear] the partner's gear cleared");
        g_have_partner = false;
        return;
    }
    if (!g_have_partner || wire::GearLayoutHash(*f) != wire::GearLayoutHash(g_partner)) DescribeFrame(*f, "the partner's gear");
    g_partner = *f;
    g_have_partner = true;
}

void EmergencyStop() noexcept {
    realbody::PublishGear(realbody::GearSlot::partner, nullptr, 0);
    realbody::PublishGear(realbody::GearSlot::self, nullptr, 0);
}

bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    switch (op) {
        case 0: {
            game::Entity* local = nullptr;
            game::TryGetLocalPlayer(&local);
            char why[160] = {};
            const bool ok = Checked() && ReadItems(local, true, why, sizeof(why));
            Xf sam;
            if (ok && ReadWorld(reinterpret_cast<std::uintptr_t>(local), &sam)) {
                // Where Sam's anchor joints are against the gear (model matrices x world: the frame check).
                for (unsigned j : {0u, 4u, 5u, 9u, 31u, 35u, 38u}) {
                    Xf jw;
                    if (JointWorld(local, sam, j, &jw))
                        Log("[gear]   Sam joint %u at %.3f %.3f %.3f (%.3f m above his root)", j, jw.p[0], jw.p[1], jw.p[2], jw.p[2] - sam.p[2]);
                }
                for (unsigned i = 0; i < g_item_count; ++i) {
                    const LocalItem& it = g_items[i];
                    Log("[gear]   item %u kind %u model %u var %u slot %u visible %d at %.3f %.3f %.3f (%.3f m from Sam's root)", i,
                        static_cast<unsigned>(it.kind), it.model, it.variation, it.slot_type, it.visible ? 1 : 0, it.world.p[0], it.world.p[1],
                        it.world.p[2], Dist(it.world, sam));
                }
            }
            for (unsigned i = 0; i < g_local.count; ++i) {
                const wire::GearItem& it = g_local.items[i];
                Log("[gear]   frame %u kind %u model %u var %u joint %u parent %d rel %.3f %.3f %.3f q %.3f %.3f %.3f %.3f", i,
                    static_cast<unsigned>(it.kind), it.model, it.variation, it.joint, it.parent == wire::kGearNoParent ? -1 : it.parent, it.p[0], it.p[1],
                    it.p[2], it.q.x, it.q.y, it.q.z, it.q.w);
            }
            std::uint32_t pl[2] = {}, aba[2] = {}, bad[2] = {};
            realbody::GearCounters(realbody::GearSlot::partner, &pl[0], &aba[0], &bad[0]);
            realbody::GearCounters(realbody::GearSlot::self, &pl[1], &aba[1], &bad[1]);
            std::snprintf(msg, cap,
                          "%s; ours %u items (layout %04X, reads %u, failed %u, kept while moving %u, new layouts held a read %u); partner's %s (%u items); props partner %u spawned "
                          "%u failed, self %u spawned %u failed; placements %u/%u (aba %u/%u, bad %u/%u)",
                          ok ? "read ok" : why, g_local.count, wire::GearLayoutHash(g_local), g_reads, g_read_fail, g_moving_skips, g_layout_held,
                          g_have_partner ? "received" : "none", g_have_partner ? g_partner.count : 0u, g_sets[0].spawned, g_sets[0].failed,
                          g_sets[1].spawned, g_sets[1].failed, pl[0], pl[1], aba[0], aba[1], bad[0], bad[1]);
            Log("[gear] status: %s", msg);
            return ok;
        }
        case 1:
            g_partner_on = v[1] != 0;
            std::snprintf(msg, cap, "partner props %s", g_partner_on ? "on" : "off");
            return true;
        case 2:
            g_self_on = v[1] != 0;
            std::snprintf(msg, cap, "self-view props %s", g_self_on ? "on" : "off");
            return true;
        case 3:
            if (!automation::Enabled() || !automation::SavesRedirected()) {
                std::snprintf(msg, cap, "refused: only in a hidden test run on scratch saves");
                return false;
            }
            g_test_cases = v[1] <= 0 ? 0 : v[1] > 12 ? 12 : static_cast<int>(v[1]);
            g_next_read_ms = 0;
            std::snprintf(msg, cap, "TEST: %d synthetic delivery case(s) on our backpack (sent and shown like carried cargo)", g_test_cases);
            Log("[gear] %s", msg);
            return true;
        case 4:
            g_debug_body = v[1] != 0;
            std::snprintf(msg, cap, "our own gear on the debug body %s", g_debug_body ? "on" : "off");
            return true;
        default:
            std::snprintf(msg, cap, "unknown op %d (0 log, 1 partner 0/1, 2 self 0/1, 3 test limit n, 4 debug body 0/1)", op);
            return false;
    }
}

}  // namespace st::gear

namespace st::game {
bool GearCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept { return gear::DebugCommand(op, v, msg, cap); }
}  // namespace st::game
