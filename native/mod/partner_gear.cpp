#include "partner_gear.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "automation.hpp"
#include "body_choice.hpp"
#include "boots_look.hpp"
#include "exo_parts.hpp"
#include "game_actions.hpp"
#include "gear_keep.hpp"
#include "gear_look.hpp"
#include "gear_wear.hpp"
#include "held_items.hpp"
#include "held_rules.hpp"
#include "log.hpp"
#include "partner_holo.hpp"
#include "partner_outfit.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"
#include "ride_together.hpp"
#include "texture_share.hpp"
#include "vehicle_sync.hpp"

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
constexpr std::uintptr_t kInvOdra = 0x4CA0, kInvPack = 0x4CB8, kInvBbPod = 0x4D48;  // +0x4D48: the pod (a reference); +0x4D60 is its DSBBPodComponent
constexpr std::uintptr_t kCarrierObj = 0x50, kCarSlotN = 0x10, kCarSlots = 0x18;
constexpr std::uintptr_t kSlotType = 0x01, kSlotStackN = 0x18, kSlotStacks = 0x20;
constexpr std::uintptr_t kStackN = 0x10, kStackItems = 0x18;
// DSBaggage2 (0x240): +0x20 u8 EDSBaggage2Model, +0x28 DSGameBaggageListItem*, +0x38 world position
// (3 doubles), +0x50 rotation rows (9 floats). DSGameBaggageListItem: +0x30 ID, +0x3A attribute,
// +0x3B case type, +0x3E volume, +0x50 weight (dsdc_types offsets).
constexpr std::uintptr_t kBagModel = 0x20, kBagListItem = 0x28, kBagWorld = 0x38;
constexpr std::uintptr_t kListId = 0x30, kListAttribute = 0x3A, kListWeight = 0x50;
constexpr std::uintptr_t kSlotTotal = 0x0C;
// v0.23: the game's own cargo hides (CargoHideSites): the carrier object's hide-reason mask (0x142CB27A0: reasons 0..4, the
// inventory update's 0 = HideBackpack / the Root mask, 1 = a cutscene's HideBaggage), and the back slot's own hidden bit
// (0x142CB2620: slot +0x75 bit 6, a cutscene's HideOnlyBackpackBaggages).
constexpr std::uintptr_t kCarHideMask = 0x8C, kSlotHideByte = 0x75;
constexpr std::uint8_t kSlotHiddenBit = 0x40;
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
    // The equipped boots (docs/real-partner-body.md section 21): the cutscene copy of DSPlayerBootsComponent's entity, the
    // same ArtParts (part_sam_foot_def) on the same 10-joint skeleton, posed from the partner body's legs (real_body.cpp).
    {"boots", "ds/entities/cinematics/demo_apv_sam_foot_def_v01", "7e71552e-bf48-464d-8628-fd9f0afc70cd"},
    // Weapons and hand tools (docs/real-partner-body.md section 17): cutscene props with the player
    // weapon's own art parts (decoded offline from the cores; a copy of the game's DSWeaponEntity crashes).
    {"w_assault_rifle", "ds/entities/cinematics/demo_apv_asr2_main_def_v04_def", "d6215dea-4e9d-7e4c-8ef6-bd9f9751ab39"},
    {"w_blood_grenade", "ds/entities/cinematics/demo_grbl_main_def", "76840453-91d4-6746-a01c-fada1c6c8bbb"},
    {"w_electric_grenade", "ds/entities/cinematics/demo_grdc_main_def", "0ffe5ad6-2e4e-d34d-8ee2-86c28c78874b"},
    {"w_grenade", "ds/entities/cinematics/demo_grfl_main_def", "7b42becd-2980-9c46-8418-c27e17fe5255"},
    {"w_smoke_grenade", "ds/entities/cinematics/demo_grsm_main_def", "9f924eca-835d-434e-bd7e-5a06ed29c976"},
    {"w_multi_rod", "ds/entities/cinematics/demo_mrod_main_def", "e00d083a-7090-284b-aa9f-fca80353a2d4"},
    {"w_hand_gun", "ds/entities/cinematics/demo_hag2_main_def", "af940392-03bd-ea4f-a161-dcb1fe17a44e"},
    {"w_shot_gun", "ds/entities/cinematics/demo_shg2_main_def", "c739ae4a-1ed9-2d47-8c91-e237dab76b69"},
};
enum PropId : int {
    kPropNone = -1, kPropBackpack = 0, kPropOdradek, kPropBbPod, kPropS, kPropM, kPropL, kPropXL, kPropCube, kPropBoots,
    kPropWAsr, kPropWGrbl, kPropWGrdc, kPropWGrfl, kPropWGrsm, kPropWMrod, kPropWHag, kPropWShg, kPropCount
};
static_assert(sizeof(kProps) / sizeof(kProps[0]) == kPropCount, "prop table");
bool IsWeaponProp(int key) { return key >= kPropWAsr && key < kPropCount; }
// Round 6 (section 31): a weapon without a cutscene prop is drawn as a "held clone" of its own resource (held_items.cpp): the
// prop keys kKeyHeld + resource id, after the entity keys (kPropCount + resource id). session.ini held_items / `gear 26`.
constexpr int kKeyHeld = kPropCount + static_cast<int>(wire::kMaxGearRes);
bool IsHeldKey(int key) { return key >= kKeyHeld && key < kKeyHeld + static_cast<int>(wire::kMaxGearRes); }
bool IsHandProp(int key) { return IsWeaponProp(key) || IsHeldKey(key); }
heldrules::Mode g_held_mode = heldrules::Mode::all;

// The sender's weapon resource (its core, the path STR1 names) -> the prop drawn for it. Exact: the
// prop's model is the player weapon's own art parts (part_asr2_main_def, part_grbl_main_def ...; only
// the colour variation may differ). Approximate: a cutscene model of the same kind (the player's
// handguns are hag1 / hag3, the prop hag2; the shotgun shg0, the prop shg2). Not listed (no prop
// exists): the bola gun, grenade launchers, sticky gun, maser gun, EX grenades, PCC, ladder, climbing
// anchor, spray, strand ... (nothing is drawn for them).
struct WeaponMap {
    const char* core;
    std::int8_t prop;
    bool exact;
};
constexpr WeaponMap kWeaponMap[] = {
    {"ds/entities/weapons/assault_rifle", kPropWAsr, true},       {"ds/entities/weapons/blood_grenade", kPropWGrbl, true},
    {"ds/entities/weapons/electrical_grenade", kPropWGrdc, true}, {"ds/entities/weapons/grenade", kPropWGrfl, true},
    {"ds/entities/weapons/smoke_grenade", kPropWGrsm, true},      {"ds/entities/weapons/multi_rod", kPropWMrod, true},
    {"ds/entities/weapons/hand_gun", kPropWHag, false},           {"ds/entities/weapons/shot_gun", kPropWShg, false},
};
const WeaponMap* WeaponFor(const char* core) {
    if (!core) return nullptr;
    for (const WeaponMap& m : kWeaponMap)
        if (std::strcmp(core, m.core) == 0) return &m;
    return nullptr;
}

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

struct ResEntry;
// The prop key an item needs (-1: none). `res`: the resource table its entity / weapon ids name (a
// weapon's prop follows its resource's core; without a table a weapon has none).
int PropFor(const wire::GearItem& it, const ResEntry* res);
bool g_boots_sync = true;  // session.ini boots_sync / `gear 21 <0/1>` (section 21)
int PropForKind(const wire::GearItem& it) {
    switch (it.kind) {
        case wire::GearKind::backpack: return kPropBackpack;
        case wire::GearKind::odradek: return kPropOdradek;
        case wire::GearKind::bb_pod: return kPropBbPod;
        case wire::GearKind::boots: return g_boots_sync ? kPropBoots : kPropNone;
        case wire::GearKind::cargo: return it.model < 31 ? kModelMap[it.model].prop : (it.model == wire::kGearNone ? kPropM : kPropNone);
        case wire::GearKind::entity: return it.res < wire::kMaxGearRes ? kPropCount + it.res : kPropNone;
        case wire::GearKind::weapon: return kPropNone;
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

// A joint of the entity's pose in world space (model matrix axes normalised). The head joints (31-34)
// of a Sam whose head the self view fit lifts (realbody::HeadLift, section 16.1) are read without the
// lift: items on the head are then measured against the character's real head, so the other screen,
// placing them on her body's head, shows them where they sit on this one.
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
    float lift[3];
    float neck[4][4];
    if (j >= 31 && j <= 34 && realbody::HeadLift(e, lift) && realbody::ReadJointModel(e, 30, neck))
        for (int k = 0; k < 3; ++k) p[k] -= static_cast<double>(lift[0]) * neck[0][k] + static_cast<double>(lift[1]) * neck[1][k] + static_cast<double>(lift[2]) * neck[2][k];
    *out = Compose(world, p, r);
    return true;
}

// ---------------------------------------------------------------- state
struct Checks {
    bool done = false, ok = false;
    char why[160] = "not checked";
} g_chk;

bool g_enabled = true;
// The self view hides Sam through the global part mask, so the game itself keeps drawing his gear on his
// skeleton (real_body.cpp kPlayerGlobalMask); the self view's props are only the BB pod (Tick).
bool g_partner_on = true, g_self_on = true;
int g_fault_spawns = 0;  // hidden tests (`gear 5 <n>`): the next n prop starts fail
// Round 11 (section 35): the BB pod under the self view (gearkeep::DecidePod): the game's own rule for Sam's pod with our
// bit 63 ignored, cross-checked against the pod's own flag while the self view is off; the fallback is the last plain-play
// reading, taken as worn after a load. `gear 30 <0|1|2>`: 0 the rule (default), 1 the fallback only, 2 the status.
gearkeep::PodState g_pod;
bool g_pod_rule_on = true;
struct PodLog {
    bool have = false, vis = false;
    gearkeep::PodSource src = gearkeep::PodSource::flag;
    unsigned changes = 0, mismatches = 0;
    double next_summary_ms = 0;
    std::uint32_t summary_compared = 0;
} g_podlog;
const char* TypeOf(std::uintptr_t obj);

// Which of the game's gear entities are mirrored (each side spawns copies of them by resource): the
// copies get no player, so only entities whose class and components draw a model and do nothing
// else. Runs 20261117-131822 and -133622 crashed a worker thread (0x14296F3AE, 0x142933FA6) right
// after copies of the Odradek (DSOdradek*Entity), the BB pod (DSBBPodComponent) and the backpack's
// cylinder / belt were created: classes other than Entity are refused. Level 0: model components only;
// 1 (default): also the small DS components proven in run 20261117-132631 (shields, the stowed weapons'
// paint, hanging boots, simple animation, sound); 2 (tests only, not proven): also the chiral cylinder and
// the backpack belt. `gear 7 <level>`. Never a DSWeaponEntity, at any level: its update reads its weapon
// instance (+0x338) without a null check (0x142933FA6), and a copy has none: run 20261117-133622 crashed
// there right after a copy of the PCC (`ds/entities/weapons/builder`) was created at level 2. Weapons are
// sent as their own kind and drawn as cutscene props (section 17).
int g_mirror_level = 1;
// The BB pod: the cinematic pod prop (0, demo_bbp1_main_def, the default) or a copy of the game's own
// pod entity (1, `gear 8 1`, tests only: its DSBBPodComponent binds to the player, and both runs that
// created such a copy crashed a worker thread, 20261117-131822 and -133622).
bool g_pod_entity = false;
bool ComponentTier(const char* n, int* tier) {
    static const char* const kTier0[] = {"SkinnedModel", "StaticModel", "ArtPartsData", "ArtPartsVariation", "ArtPartsShaderVariableComponent",
                                         "ShaderVariableComponent", "StencilBufferComponent", "MeshPaintTextureComponent",
                                         "BoostMeshStreamingForArtPartsDataComponent", "ExternalAnimationComponent", "LodDistanceOverrideComponent",
                                         "AttachToHelperMover", "CustomizedAttachToHelperMover", "DSOutlineEffectComponent"};
    static const char* const kTier1[] = {"DSArmorPlateComponent", "BackpackCustomizePaintManagementComponent", "DSSimpleAnimationComponent",
                                         "SoundComponent", "DSPlayerHangedBootsComponent"};
    static const char* const kTier2[] = {"DSChiralCylinderComponent", "DSPlayerBackpackBeltComponent"};
    for (const char* k : kTier0)
        if (std::strcmp(n, k) == 0) return (*tier = 0), true;
    for (const char* k : kTier1)
        if (std::strcmp(n, k) == 0) return (*tier = 1), true;
    for (const char* k : kTier2)
        if (std::strcmp(n, k) == 0) return (*tier = 2), true;
    if (g_pod_entity && std::strcmp(n, "DSBBPodComponent") == 0) return (*tier = 1), true;
    return false;
}
// The entity's class and components allow a copy at level (why: the first reason not).
bool MirrorableAt(std::uintptr_t e, int level, const char** why) {
    const char* tn = TypeOf(e);
    if (std::strcmp(tn, "DSWeaponEntity") == 0) return (*why = "a weapon (sent as a weapon item, never copied)"), false;
    if (std::strcmp(tn, "Entity") != 0) return (*why = "not a plain Entity class"), false;
    std::int32_t n = 0;
    if (!Rd(e + 0x90, n) || n <= 0 || n > 64) return (*why = "component list unreadable"), false;
    const std::uintptr_t data = Q(e + 0x98);
    static char buf[96];
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t c = Q(data + 8ull * static_cast<unsigned>(i));
        const char* cn = c ? mem::RttiName(mem::RttiOf(c)) : nullptr;
        int tier = 9;
        if (!cn || !ComponentTier(cn, &tier) || tier > level) {
            std::snprintf(buf, sizeof(buf), "carries %s", cn ? cn : "?");
            return (*why = buf), false;
        }
    }
    return true;
}
bool SendableEntity(std::uintptr_t e, const char** why) { return MirrorableAt(e, g_mirror_level, why); }
int g_test_cases = 0;       // hidden tests: this many synthetic delivery cases on our backpack (0 none)
bool g_debug_body = false;  // our own gear on the debug body (real_spawn)

// The local snapshot (sender side; also the self view's props).
struct LocalItem {
    wire::GearKind kind;
    std::uint8_t model, variation, slot_type;
    std::uintptr_t e;
    bool visible;
    Xf world;
    std::uint8_t res = wire::kGearNone;  // entity, weapon: our resource id
    std::uintptr_t parent_e = 0;         // entity, weapon: its parent entity (anchors on that item when it is one)
    std::uint8_t hint_joint = 0xFF;      // weapon: the Sam joint its mover's helper names (in a hand), else 0xFF
};

// ---------------------------------------------------------------- gear resource tables (STR1)
// Ours: an id per distinct entity resource seen this session (stable, never reused). The partner's:
// what its STR1 frames said (cleared with its gear on a new session).
struct ResEntry {
    bool used = false;
    bool weapon_only = false;  // the partner's weapon resource: shown as a prop by its core, never spawned itself
    std::uint8_t guid[16] = {};
    char path[wire::kMaxGearResPath + 1] = {};
};
int PropFor(const wire::GearItem& it, const ResEntry* res) {
    if (it.kind != wire::GearKind::weapon) return PropForKind(it);
    if (!res || it.res >= wire::kMaxGearRes || !res[it.res].used) return kPropNone;
    const WeaponMap* m = WeaponFor(res[it.res].path);
    return heldrules::WeaponKey(g_held_mode, res[it.res].path, m ? m->prop : kPropNone, it.res, wire::kMaxGearRes, kKeyHeld);
}
// A key's name for the log: the cutscene prop's, or the resource's path (entity items, held clones).
const char* KeyLabel(int key, const ResEntry* res) {
    if (key >= 0 && key < kPropCount) return kProps[key].key;
    const int r = IsHeldKey(key) ? key - kKeyHeld : key - kPropCount;
    if (key >= kPropCount && r >= 0 && r < static_cast<int>(wire::kMaxGearRes)) return res && res[r].used ? res[r].path : "an unnamed resource";
    return "none";
}
ResEntry g_res_local[wire::kMaxGearRes];
unsigned g_res_local_n = 0;
std::uint64_t g_res_unsent = 0;  // ids not sent yet (sent first)
unsigned g_res_rr = 0;           // round-robin resend position
double g_res_next_ms = 0;
std::uint32_t g_res_full = 0;    // resources not given an id (table full)
ResEntry g_res_partner[wire::kMaxGearRes];
std::uint32_t g_res_rx = 0;

int LocalResId(const char* path, const std::uint8_t (&guid)[16]) {
    for (unsigned i = 0; i < g_res_local_n; ++i)
        if (std::memcmp(g_res_local[i].guid, guid, 16) == 0 && std::strcmp(g_res_local[i].path, path) == 0) return static_cast<int>(i);
    if (g_res_local_n >= wire::kMaxGearRes || !wire::GearResPathOk(path)) {
        static bool s_full_logged = false;
        if (g_res_local_n >= wire::kMaxGearRes && !s_full_logged) {
            s_full_logged = true;
            Log("[gear] the resource table is full (%u ids this session): '%s' and any later new gear resource are not sent", wire::kMaxGearRes, path);
        }
        ++g_res_full;
        return -1;
    }
    ResEntry& r = g_res_local[g_res_local_n];
    r.used = true;
    std::memcpy(r.guid, guid, 16);
    std::snprintf(r.path, sizeof(r.path), "%s", path);
    g_res_unsent |= 1ull << g_res_local_n;
    Log("[gear] resource %u: '%s' %02x%02x%02x%02x...", g_res_local_n, path, guid[3], guid[2], guid[1], guid[0]);
    return static_cast<int>(g_res_local_n++);
}

// Sam's own ArtParts that are gear (EDSPlayerMesh: BBPod 30, BBPodPipe 31, ExoMain 32, Balloon 35,
// ExoSub 38, Speed 39, Pile 40, BatteryEmission 41, Heavy 42, EmissionHeavy 43, LegIn/Out(+Lamp)
// 44-47, SubPouch 51, DreamCatcher 52, GlovePower 54, BackpackBelt 63, Strand 69, BeachLeg lamps
// 70-71, Hip 72, HipGun 73, GravityGlove 78, Support 86, EmissionSupport 87): the mesh controller
// maps each to an ArtParts index (controller +0x38 + 8k, -1: none; census run 20261117-130343).
constexpr std::uint8_t kGearMeshParts[] = {30, 31, 32, 35, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 51, 52, 54, 63, 69, 70, 71, 72, 73, 78, 86, 87};
std::uint64_t g_parts_apply[2] = {}, g_parts_visible[2] = {};
bool g_have_parts = false;
std::uint64_t g_pack_apply = 0, g_pack_visible = 0;
bool g_have_pack_parts = false;

// ArtPartsData: +0x38 initialised, +0x100 part count, +0x108 parts; part +0x18 parent, +0x20 its
// resource, +0x2C flags (bit 0 asked visible, bit 1 shown).
struct Apd {
    std::uintptr_t apd = 0, arr = 0;
    std::int32_t n = 0;
};
bool ReadApd(std::uintptr_t e, Apd* out) {
    const std::uintptr_t apd = partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), "ArtPartsData");
    std::int32_t n = 0;
    if (!apd || !Rd(apd + 0x100, n) || n <= 0 || n > 512) return false;
    const std::uintptr_t arr = Q(apd + 0x108);
    if (!arr) return false;
    out->apd = apd, out->arr = arr, out->n = n;
    return true;
}
bool PartFlags(const Apd& a, unsigned i, std::uint8_t* fl) {
    const std::uintptr_t p = Q(a.arr + 8ull * i);
    return p && Rd(p + 0x2C, *fl);
}

// Sam's head (docs/real-partner-body.md section 14): hair, hood, face lines, cap hair. The inventory
// update chooses these parts every frame from what he wears (cap, glasses, the cyphead accessory, the
// hood's state and type); demo_sam_clean has no mesh controller and keeps the defaults. Every ArtParts
// part below the part the controller maps to Head (EDSPlayerMesh 2; index 52 in part_sam_body_suiti) is
// mirrored by its asked-visible bit (flags bit 0), only while Sam wears the porter suit (107 parts, the
// body's own resource), no Root or global hide is set (+0x338 / +0x348: photo mode, scenes) and no hood
// switch animation runs. The animated switch meshes themselves are never sent.
bool Bit(const std::uint64_t* words, unsigned i) { return ((words[i >> 6] >> (i & 63)) & 1) != 0; }
constexpr unsigned kHeadMesh = 2;
constexpr std::int32_t kSuitiParts = 107;
constexpr std::uint8_t kHeadSwitchParts[] = {68, 83, 84};  // hoodswitch, hoodotterswitch, hoodoxiswitch (suiti)
constexpr std::uint8_t kHeadSkipParts[] = {68, 83, 84, 85, 86};  // + oxi switch children
bool g_head_on = true;  // `gear 16 <0/1>`
struct HeadState {
    std::uintptr_t arr = 0;  // the ArtParts array the subtree was computed for
    std::uint64_t subtree[2] = {};
    std::uint64_t apply[2] = {}, vis[2] = {};
    bool have = false;
    unsigned held = 0, reads = 0;
    const char* why = "not read yet";
} g_head;

bool HeadSubtree(std::uintptr_t ctrl, const Apd& a, std::uint64_t* out) {
    std::int32_t head = -1;
    if (!Rd(ctrl + 0x38 + 8ull * kHeadMesh, head) || head < 0 || head >= a.n) return false;
    const std::uintptr_t hp = Q(a.arr + 8ull * static_cast<unsigned>(head));
    if (!hp) return false;
    out[0] = out[1] = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(a.n) && i < wire::kMaxGearParts; ++i) {
        std::uintptr_t p = Q(a.arr + 8ull * i);
        if (!p || p == hp) continue;
        for (int hop = 0; hop < 6 && p; ++hop) {
            p = Q(p + 0x18);
            if (p == hp) {
                out[i >> 6] |= 1ull << (i & 63);
                break;
            }
        }
    }
    for (std::uint8_t k : kHeadSkipParts) out[k >> 6] &= ~(1ull << (k & 63));
    return out[0] || out[1];
}

// Reads the head parts into g_head, or keeps the last reading (g_head.why says why).
void ReadHead(std::uintptr_t sam, std::uintptr_t ctrl, const Apd& a) {
    ++g_head.reads;
    auto hold = [](const char* why) {
        g_head.why = why;
        ++g_head.held;
    };
    if (a.n != kSuitiParts || !outfit::LocalWearsPorter(sam)) return hold("not the porter suit");
    // (reason 63 of the global mask is the self view's own: real_body.cpp kOurReason; the head parts'
    // asked bits are still the game's then)
    if (Q(ctrl + 0x338) != 0 || (Q(ctrl + 0x348) & ~(1ull << 63)) != 0) return hold("Sam hidden by a Root / global part mask");
    if (g_head.arr != a.arr) {
        std::uint64_t st[2] = {};
        if (!HeadSubtree(ctrl, a, st)) return hold("no head part in the controller's map");
        g_head.arr = a.arr;
        g_head.subtree[0] = st[0], g_head.subtree[1] = st[1];
        unsigned n = 0;
        for (unsigned i = 0; i < 128; ++i) n += Bit(st, i) ? 1u : 0u;
        Log("[gear] Sam's head: %u ArtParts below the Head part are mirrored (hair, hood, face lines)", n);
    }
    for (std::uint8_t k : kHeadSwitchParts) {
        std::uint8_t fl = 0;
        if (k < a.n && PartFlags(a, k, &fl) && (fl & 1)) return hold("the hood switch animation runs");
    }
    std::uint64_t vis[2] = {};
    for (unsigned i = 0; i < static_cast<unsigned>(a.n) && i < wire::kMaxGearParts; ++i) {
        if (!Bit(g_head.subtree, i)) continue;
        std::uint8_t fl = 0;
        if (!PartFlags(a, i, &fl)) return hold("a part is unreadable");
        if (fl & 1) vis[i >> 6] |= 1ull << (i & 63);
    }
    if (g_head.have && (vis[0] != g_head.vis[0] || vis[1] != g_head.vis[1]))
        Log("[gear] Sam's head parts changed: shown %016llX %016llX -> %016llX %016llX", static_cast<unsigned long long>(g_head.vis[1]),
            static_cast<unsigned long long>(g_head.vis[0]), static_cast<unsigned long long>(vis[1]), static_cast<unsigned long long>(vis[0]));
    g_head.apply[0] = g_head.subtree[0], g_head.apply[1] = g_head.subtree[1];
    g_head.vis[0] = vis[0], g_head.vis[1] = vis[1];
    g_head.have = true;
    g_head.why = "read";
}

// Round 7 (section 32): what the mesh controller asks of each gear part, by the game's own formula (ApplyMeshControl
// 0x1426D9120, exo_parts.hpp MeshAsked) with our self view's bit 63 of the global mask ignored: his skeleton (and every
// other gear part) is known while the self view hides him (before: the parts' flags, which read hidden then, and the last
// reading stood). Byte-checked once: the per-part step of ApplyMeshControl's loop (root +0x338, global mask +0x348, forced
// show +0x8D8, forced hide +0xE68, compare mode +0x19A0, compare value +0x1998, the mask +0x348 + 8k) and the map read.
constexpr std::uintptr_t kMeshCtlStep = 0x26D91E2, kMeshCtlMap = 0x26D924E;
struct MeshCtlCheck {
    bool done = false, ok = false;
} g_mcc;
bool MeshCtlOk() {
    if (g_mcc.done) return g_mcc.ok;
    g_mcc.done = true;
    g_mcc.ok = mem::CodeMatches(mem::Base() + kMeshCtlStep,
                                "48 83 BB 38 03 00 00 00 40 B6 01 75 46 48 83 BB 48 03 00 00 00 75 3C 48 83 BC FB D8 08 00 00 00 74 02 EB 32 "
                                "48 83 BC FB 68 0E 00 00 00 75 24 80 BC 1F A0 19 00 00 00 48 8B 84 FB 48 03 00 00 74 0D 48 3B 83 98 19 00 00 "
                                "40 0F 95 C6 EB 08 48 85 C0 74 03 40 32 F6") &&
               mem::CodeMatches(mem::Base() + kMeshCtlMap, "44 8B 44 FB 38 41 83 F8 FF");
    Log("[gear] the mesh controller's part formula (ApplyMeshControl) %s", g_mcc.ok ? "verified: Sam's gear parts are read through the self view"
                                                                                    : "does not match: the parts' flags are read (the last reading stands under the self view)");
    return g_mcc.ok;
}
bool ReadMeshCtl(std::uintptr_t ctrl, unsigned k, exo::MeshCtl* m) {
    std::uint8_t mode = 0;
    if (!Rd(ctrl + 0x338, m->root) || !Rd(ctrl + 0x348, m->mask0) || !Rd(ctrl + 0x8D8 + 8ull * k, m->b) || !Rd(ctrl + 0xE68 + 8ull * k, m->c) ||
        !Rd(ctrl + 0x348 + 8ull * k, m->mask) || !Rd(ctrl + 0x1998, m->cmpval) || !Rd(ctrl + 0x19A0 + k, mode))
        return false;
    m->cmpmode = mode != 0;
    return true;
}

// Round 11 (section 35): the game's rule for Sam's BB pod (gear_keep.hpp PodAsked: the mesh controller's attachment slot 0,
// 0x1426C7760) read with our self view's bit 63 ignored. Byte-checked once: the rule itself (0x1426C7B0B through the call
// that sets the pod's flag, 0x14235F640), the slot's base (+0x2100, 0x320 a slot), the inventory gate (0x1427AD860:
// +0x4C7E bit 4) and the controller's part formula (MeshCtlOk). On a mismatch the fallback (the last plain-play reading,
// taken as worn after a load) stands.
constexpr std::uintptr_t kPodRule = 0x26C7B0B, kPodSlotBase = 0x26C78D0, kPodGate = 0x27AD860;
constexpr std::uintptr_t kPodSlotK = 0x2108, kPodSlotRef = 0x2118, kPodSlotFlags = 0x2410;  // slot 0 (+0x2100): +8 k, +0x18 ref, +0x310 flags
// The rule's two fixed reads (meaning unknown: by ReadMeshCtl's layout index 119 of the forced-hide and mask arrays, past
// the 0x59 parts, so not mesh 30's +0xF58 / +0x438); read at the same offsets as the game.
constexpr std::uintptr_t kPodForcedHide = 0x1220, kPodMaskK = 0x700;
constexpr std::uintptr_t kInvPodReady = 0x4C7E, kInvPodOn = 0x4C52;
constexpr unsigned kMeshKCount = 0x59;  // ApplyMeshControl's parts (k 0..0x58)
int g_pod_sites = -1;  // -1 not checked, 0 mismatch (the fallback), 1 ok
bool PodRuleOk() {
    if (g_pod_sites < 0) {
        const std::uintptr_t b = mem::Base();
        // (CodeMatches compares at most 96 bytes per signature: the 174 bytes in two parts)
        const bool rule = mem::CodeMatches(b + kPodRule,
                                           "41 0F B6 9E 10 03 00 00 80 E3 01 75 13 48 8B 06 48 8B 88 88 00 00 00 D0 E9 F6 C1 01 75 3E B3 01 85 FF "
                                           "0F 85 F2 03 00 00 4D 85 ED 0F 84 81 04 00 00 48 8B 06 48 8B 88 C8 54 00 00 E8 15 5D 0E 00 84 C0 74 5D "
                                           "48 8B 06 48 8B 88 C8 54 00 00 80 B9 52 4C 00 00 00 74 4A 80") &&
                          mem::CodeMatches(b + kPodRule + 88,
                                           "F3 01 EB 47 41 0F B6 46 08 8B 54 C6 38 83 FA FF 74 13 48 8B 4C 24 60 E8 21 BA BE FF 84 C0 0F 94 C3 84 "
                                           "C0 74 A3 85 FF 0F 85 95 03 00 00 48 83 BE 20 12 00 00 00 74 04 B3 01 EB 95 48 83 BE 00 07 00 00 00 74 "
                                           "8B B3 01 EB 87 B3 01 0F B6 D3 49 8B CD E8 87 7A C9 FF");
        const bool slot = mem::CodeMatches(b + kPodSlotBase, "49 69 C7 20 03 00 00 4C 8D B6 00 21 00 00 4C 03 F0 49 8B 46 18");
        const bool gate = mem::CodeMatches(b + kPodGate, "0F B6 81 7E 4C 00 00 C0 E8 04 24 01 C3");
        g_pod_sites = rule && slot && gate && MeshCtlOk() ? 1 : 0;
        Log("[gear] BB pod: the game's rule for Sam's pod (0x1426C7B0B) %s", g_pod_sites == 1
                ? "verified: under the self view her pod follows what the game asks for his (cross-checked against its own flag while the self view is off)"
                : "does not match this game build: under the self view her pod keeps its last plain-play state (taken as worn after a load)");
    }
    return g_pod_sites == 1;
}
// The rule's inputs for the pod entity `pod` (false: not readable now, or the slot is not this pod).
bool ReadPodRule(std::uintptr_t p, std::uintptr_t inv, std::uintptr_t pod, gearkeep::PodRead* r) {
    const std::uintptr_t ctrl = p + 0xA60;
    if (!PodRuleOk() || Q(ctrl) != p) return false;
    std::uint8_t k = 0, slot_fl = 0, ready = 0, on = 0;
    std::uint64_t sam_fl = 0, hide = 0, mask = 0, ref = 0;
    std::int32_t map = -1;
    // The slot's own part index k (its byte +8) picks the ArtParts part, as the game's rule does (run 20261123-094135: k 1,
    // not EDSPlayerMesh 30; the forced hide and mask the rule reads are fixed, above).
    if (!Rd(ctrl + kPodSlotK, k) || k >= kMeshKCount || !Rd(ctrl + kPodSlotRef, ref) || !Rd(ctrl + kPodSlotFlags, slot_fl) ||
        !Rd(p + kEntFlags, sam_fl) || !Rd(ctrl + 0x38 + 8ull * k, map) || !Rd(ctrl + kPodForcedHide, hide) || !Rd(ctrl + kPodMaskK, mask) ||
        !Rd(inv + kInvPodReady, ready) || !Rd(inv + kInvPodOn, on))
        return false;
    static int s_k_logged = -1;
    static unsigned s_k_logs = 0;
    const bool same = ref && ref - 0x20 == pod;
    if (s_k_logged != k * 2 + same && s_k_logs < 20) {
        ++s_k_logs;
        s_k_logged = k * 2 + same;
        Log("[gear] BB pod: the controller's slot 0: part %u (map %d), its entity %s our pod", k, map, same ? "is" : "is NOT (the rule is not read)");
    }
    if (!same) return false;  // the slot holds another pod (or none yet)
    r->gate_ready = (ready & 0x10) != 0;
    r->gate_on = on != 0;
    r->slot_hidden = (slot_fl & 1) != 0;
    r->sam_visible = (sam_fl & 0x2) != 0;
    r->part_mapped = map != -1;
    r->part_asked = true;
    if (r->part_mapped) {
        exo::MeshCtl m;
        if (!ReadMeshCtl(ctrl, k, &m)) return false;
        r->part_asked = exo::MeshAsked(m);
    }
    r->forced_hide = hide != 0;
    r->mask_k = mask != 0;
    return true;
}
// Plain play for the gear's own rules: no load or post-load setup, and none of a menu, the private room, a cutscene, a
// terminal, subspace (DSGameState flags, current or requested). *in_room: the PrivateRoom flags.
constexpr std::uint64_t kGsTerminal = 0x3ull, kGsSubspace = 1ull << 8, kGsGameOver = 1ull << 13, kGsFaint = 1ull << 14, kGsMovie = 1ull << 18,
                        kGsInGameCutscene = 1ull << 19, kGsDirectRoom = 1ull << 30;
constexpr std::uint64_t kGsRoom = game::kGameStatePrivateRoom | kGsDirectRoom;
constexpr std::uint64_t kGsNotPlain = kGsRoom | kGsTerminal | game::kGameStatePauseMenu | game::kGameStateCutscene | game::kGameStateSystemPause |
                                      kGsSubspace | kGsGameOver | kGsFaint | kGsMovie | kGsInGameCutscene;
bool PlainPlay(bool* in_room) {
    std::uint64_t gs = 0;
    const char* load = game::LoadOrSetup(&gs, nullptr);
    if (in_room) *in_room = (gs & kGsRoom) != 0;
    return !load && !(gs & kGsNotPlain);
}
int g_exo_force = -1;     // `gear 28 <type>` (hidden tests): the skeleton pieces read replaced by a type's (-1 off)
int g_exo_logged = -2;    // the skeleton type last logged
std::uint64_t g_exo_logged_raw = 0;  // the game's skeleton bits last logged (ReadParts)
unsigned g_exo_raw_logs = 0;         // logs of a changed reading of no known type
bool g_exo_porter = false;  // our Sam's map is the porter suit's for every skeleton piece (the exo bits are sent)
std::uint64_t g_exo_settled[2] = {};  // the skeleton pieces of the last known type read (none before one)
std::uint32_t g_exo_unsettled = 0;    // reads whose pieces were no known type (held)

// Our Sam's gear parts (which exist, which are shown) and his head parts. False when unreadable.
bool ReadParts(std::uintptr_t sam) {
    const std::uintptr_t ctrl = sam + 0xA60;
    Apd a;
    if (Q(ctrl) != sam || !ReadApd(sam, &a)) return false;
    const bool self = realbody::SelfShown();
    const bool formula = MeshCtlOk();
    if (self && !formula) return true;  // the flags read hidden under the self view: the last reading stands
    std::uint64_t apply[2] = {}, vis[2] = {};
    // the head parts are read by their flags (not by the controller): under the self view the last reading stands
    if (!self) ReadHead(sam, ctrl, a);
    if (g_head_on && g_head.have)
        for (unsigned w = 0; w < 2; ++w) apply[w] = g_head.apply[w], vis[w] = g_head.vis[w];
    // the skeleton pieces only with the porter suit's map (the receiver's carrier takes these indices as those parts)
    std::int32_t exo_idx[exo::kExoMeshCount] = {};
    for (unsigned i = 0; i < exo::kExoMeshCount; ++i)
        if (!Rd(ctrl + 0x38 + 8ull * exo::kExoMesh[i], exo_idx[i])) exo_idx[i] = -1;
    g_exo_porter = a.n == exo::kSuitParts && exo::MapIsPorter(exo_idx);
    // The gear parts (their effective bit) win where both name a part (the balloon, 57).
    for (std::uint8_t k : kGearMeshParts) {
        std::int32_t idx = -1;
        if (!Rd(ctrl + 0x38 + 8ull * k, idx) || idx < 0 || idx >= a.n || idx >= static_cast<std::int32_t>(wire::kMaxGearParts)) continue;
        bool is_exo = false;
        for (std::uint8_t ek : exo::kExoMesh) is_exo |= ek == k;
        if (is_exo && !g_exo_porter) continue;
        bool on = false;
        if (formula) {
            exo::MeshCtl m;
            if (!ReadMeshCtl(ctrl, k, &m)) continue;
            on = exo::MeshAsked(m);
        } else {
            std::uint8_t fl = 0;
            if (!PartFlags(a, static_cast<unsigned>(idx), &fl)) continue;
            on = (fl & 0x2) != 0;
        }
        apply[idx >> 6] |= 1ull << (idx & 63);
        vis[idx >> 6] &= ~(1ull << (idx & 63));
        if (on) vis[idx >> 6] |= 1ull << (idx & 63);
    }
    // The skeleton pieces only as one of the game's types (exo_parts.hpp): for a moment after a world load every piece reads
    // shown (the masks not set yet: all four skeletons at once); then the last known type stands (none before one was read).
    int type = -3;
    std::uint64_t raw = 0;  // the skeleton bits the game asked for (before the hold below), for the log
    if (g_exo_porter) {
        exo::OverrideParts(g_exo_force, apply, vis);
        raw = vis[0] & 0x000C00BF00000000ull;
        type = exo::TypeOfParts(vis);
        if (type >= 0) {
            g_exo_settled[0] = vis[0], g_exo_settled[1] = vis[1];
        } else {
            for (unsigned i = 0; i < exo::kExoMeshCount; ++i) exo::SetBit(vis, exo::kExoPart[i], exo::Bit(g_exo_settled, exo::kExoPart[i]));
            ++g_exo_unsettled;
        }
    }
    // Logged on a type change, and again when the game's raw pieces change while they are of no known type (at most 40 of
    // those: a long partial reading, a cutscene's forced show, is visible in the log). The bits are the game's own reading,
    // not the held type's; not the porter suit's map: none (other suits' mesh slots are not skeleton pieces).
    if (type != g_exo_logged || (type == -1 && raw != g_exo_logged_raw && g_exo_raw_logs < 40)) {
        if (type == g_exo_logged) ++g_exo_raw_logs;
        g_exo_logged = type;
        g_exo_logged_raw = raw;
        const int sent = g_exo_porter ? exo::TypeOfParts(vis) : -3;
        char bits[40] = "none read";
        if (g_exo_porter) std::snprintf(bits, sizeof(bits), "pieces shown %016llX", static_cast<unsigned long long>(raw));
        Log("[exo] our Sam's skeleton: %s%s (%s)%s%s", type >= 0 ? exo::TypeName(static_cast<exo::Type>(type)) : type == -1 ? "pieces of no known type" : "not read (not the porter suit's parts)",
            g_exo_force >= 0 ? " (forced: gear 28)" : self ? " (read through the self view)" : "", bits,
            type == -1 ? ": the last known type stands: " : "", type == -1 ? exo::TypeName(static_cast<exo::Type>(sent < 0 ? 0 : sent)) : "");
    }
    g_parts_apply[0] = apply[0], g_parts_apply[1] = apply[1];
    g_parts_visible[0] = vis[0], g_parts_visible[1] = vis[1];
    g_have_parts = true;
    return true;
}
constexpr unsigned kMaxLocal = 128;
LocalItem g_items[kMaxLocal];
unsigned g_item_count = 0;
wire::GearFrame g_local;           // the frame we show and send (rel poses refreshed when still)
bool g_have_local = false;
std::uintptr_t g_local_e[wire::kMaxGearItems] = {};  // g_local's items: their entity (or DSBaggage2); 0 synthetic

// Gear looks (docs/real-partner-body.md section 13): ours per item of g_local (sender), the partner's
// per item of its layout (receiver). `gear 12 <0/1>` switches them off / on (both sides).
bool g_looks_on = true;
struct LocalLook {
    bool have = false;
    wire::GearLook l;
    std::uint32_t hash = 0, sent_hash = 0;
    std::uint16_t sent_layout = 0;
    std::uintptr_t e = 0;
};
LocalLook g_look[wire::kMaxGearItems];
double g_next_look_ms = 0, g_look_rr_ms = 0;
unsigned g_look_rr = 0;
std::uint32_t g_looks_sent = 0;
struct PartnerLook {
    bool have = false;
    wire::GearLook l;
    std::uint32_t hash = 0;
};
PartnerLook g_plook[wire::kMaxGearItems];
std::uint32_t g_looks_rx = 0;
// The backpack's skinned pose (STK1): ours (sender) and the partner's (receiver). `gear 13 <0/1>`.
bool g_poses_on = true;
wire::GearPose g_pose_local, g_pose_sent;
bool g_have_pose_local = false, g_have_pose_sent = false;
double g_pose_sent_ms = 0;
std::uint32_t g_poses_sent = 0;
wire::GearPose g_pose_partner;
bool g_have_pose_partner = false;
std::uint32_t g_pose_partner_seq = 0, g_poses_rx = 0;
// Round 6 (section 31): the held item's own skinned pose (the ladder folded in the hand, unfolded over the shoulder while LT is
// held; the PCC's arms): ours (sender, a second STK1 stream on the weapon item's index) and the partner's (receiver, written onto
// the held clone through pose target kHeldPoseTarget). `gear 26` counts them.
constexpr unsigned kHeldPoseTarget = realbody::kGearPoseTargets - 1;
wire::GearPose g_hpose_local, g_hpose_sent, g_hpose_partner;
bool g_have_hpose_local = false, g_have_hpose_sent = false, g_have_hpose_partner = false;
double g_hpose_sent_ms = 0;
std::uint32_t g_hposes_sent = 0, g_hposes_rx = 0, g_hpose_partner_seq = 0, g_hpose_writes = 0;
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

// A prop's key: what it shows. kProps index (0..kPropCount-1), or kPropCount + resource id (an
// entity item). Props are matched to items by key on a layout change (wire::GearAssignProps).
constexpr int kKeyEntity = kPropCount;
constexpr int kKeyCount = kKeyHeld + static_cast<int>(wire::kMaxGearRes);  // entity keys, then the held clones' keys

struct Prop {
    int def = kPropNone;   // key (see above); kPropNone = no prop for this item
    holo::Spawn sp;
    bool loading = false, live = false, failed = false, shown = false;
    std::uintptr_t vt = 0;
    game::Guid guid;
    unsigned fails = 0;     // failed starts / loads in a row (retried with a backoff)
    double retry_ms = 0;    // not before this (failed)
    double placed_ms = 0;   // the last tick it was placed
    unsigned reshows = 0;   // times it was found changed by the game in the current 3 s window
    double reshow_since = 0;
    bool game_owned = false;  // the prop's own components keep hiding it: its visibility is left to them
    double live_ms = 0;       // when it became live (looks wait 0.5 s)
    std::uint32_t look_hash = 0;  // the look applied (wire::GearLookHash; 0 none)
    double look_check_ms = 0;     // next check that it still shows it
    double coll_next_ms = 0;      // its collision (off since it was realized) checked again every second
    unsigned coll_again = 0;      // times it was found on again
};
struct PropSet {
    const char* tag;
    realbody::GearSlot slot;
    game::Entity* body = nullptr;  // the body the props belong to
    Prop props[wire::kMaxGearItems];
    wire::GearFrame want;          // the layout + poses the props follow
    bool have_want = false;
    const ResEntry* res = nullptr; // the resource table the entity items name
    std::uint16_t published_hash = 0;
    unsigned published_n = 0;
    bool published = false;
    double asleep_since = 0;
    bool pillion_hide = false;     // v0.23: our Sam on the partner's trike pillion: its backpack props hidden (DriveSet)
    unsigned spawned = 0, failed = 0, removed = 0, retried = 0, reshown = 0, kept = 0;
    bool refused[kKeyCount] = {};  // keys whose entity was refused by validation (deterministic: this session)
    double next_stats_ms = 0;
    double last_placed_ms = 0;
    std::uint32_t placement_off = 0;  // ticks the detour's list was found unpublished (a placement fault)
    unsigned looks = 0, looks_partial = 0, looks_reapplied = 0, looks_failed = 0;
    std::uintptr_t pose_e[realbody::kGearPoseTargets] = {};  // the props given the partner's backpack pose
    std::uint32_t pose_seq_pub[realbody::kGearPoseTargets] = {};
    unsigned pose_targets = 0;
    unsigned pose_target_logs = 0;  // target changes logged (the first 40)
    // Weapon props no item wants any more (holstered into a pouch, thrown): kept hidden for 60 s and taken
    // again by the next item of the same key (the next draw / grenade shows at once, without a load).
    struct Spare {
        Prop p;
        double since = 0;
    };
    Spare spare[6];
    unsigned spare_reused = 0, spare_kept = 0;
    double layout_ms = 0;          // when the current layout was taken (the weapon props' show latency)
    unsigned weapon_shows = 0;     // weapon props shown (logged, the first 60)
    // Round 7 (section 32): the exo carrier of this slot (ExoDrive), outside the item list: published as the list's last entry.
    struct Exo {
        Prop p;
        bool want = false, refused = false, target = false, parts_ok = false;
        bool tree_checked = false, tree_ok = false, tree_logged = false, body_sam = false;
        std::uint64_t vis[2] = {}, asked[2] = {}, shown_bits[2] = {~0ull, ~0ull};
        std::int16_t parent[exo::kMaxParts] = {};
        int n = 0;
        double unwanted_ms = 0, next_parts_ms = 0, target_retry_ms = 0, next_meter_ms = 0;
        char target_why[160] = "";
        std::uintptr_t body_checked = 0;
        unsigned parts_set = 0, game_changes = 0, spawned = 0, removed = 0, show_logs = 0;
        outfit::Dress dress;
        // the first show waits for the tint (the sender's exos variation on the carrier), at most kExoTintWaitMs
        std::uint8_t tint_want = 0xFE;  // wire::PorterExos of the outfit it copies (0xFE: no outfit read)
        bool tint_ok = false;
        // her rain hood on it (section 37): what it shows (HoodAnimTick), its hood parts per type {on, off, switch} looked up
        // on this carrier, its animation slots (bit per type, -1 not checked), the slot being unfolded
        exo::HoodAnim hood;
        int hood_part = -1;
        std::uintptr_t hood_e = 0;
        std::int16_t hood_idx[exo::kHoodTypes][3] = {{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}};
        int anim_ok = -1, anim_slot = -1, anim_slot_next = -1, anim_type = -1;
        bool anim_alt = false;
        unsigned anim_checks = 0, hood_calls = 0, hood_faults = 0, hood_logs = 0;
        double anim_check_ms = 0;
        // her hair under the hood (hood_hair): the body, its hair parts, those we hid
        std::uintptr_t hair_body = 0;
        std::int16_t hair_parts[4] = {-1, -1, -1, -1};
        unsigned hair_n = 0, hair_mask = 0, hair_logs = 0;
        bool hair_hidden = false, hair_checked = false;
        double hair_next_ms = 0;
    } exo;
};
PropSet g_sets[2] = {{"gear", realbody::GearSlot::partner}, {"gear-self", realbody::GearSlot::self}};
std::uint8_t g_self_src[wire::kMaxGearItems] = {};  // the self set's item i shows item g_self_src[i] of our own frame (g_local)

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
// Delicate, SmallDelicate, ToxicGas) anywhere. Equipment, weapons (also the weapon case at the waist: the
// game draws a dark netted weapon crate, sam_weapon_case_*, a physics entity we do not copy; v0.8 drew a
// white delivery case there, which the player never wears: docs/real-partner-body.md section 13), tools
// (the folded ladder at the waist, carts, builders), BB pod, bodies: not drawn (in game they sit in the
// backpack's pouches or are models we have no prop for).
bool Drawn(std::uint8_t kase, std::uint8_t slot_type) {
    (void)slot_type;
    switch (kase) {
        case 0: case 1: case 9: case 13: case 17: case 18: return true;
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

// The entity walk's verdicts, cached per entity (review: every check is a ReadProcessMemory call, and the
// class / component / resource checks of ~60 nodes ran every 100 ms on the game thread). An entry holds
// while the entity keeps its address, vtable, resource reference and component list; visibility, the
// model and the transform are still read every time. `gear 10 0` switches the cache off (a comparison).
struct NodeMemo {
    std::uintptr_t e = 0, vt = 0, res = 0, comps = 0;
    std::int32_t ncomp = -1;
    std::int8_t cls = 0;        // 1 an entity the walk looks at (descends into), 2 not (camera, player, no Entity), 3 a weapon
    std::int16_t verdict = -9;  // -9 not known yet, -1 no usable resource, -2 not mirrored, >= 0 our resource id
};
NodeMemo g_memo[512];
bool g_memo_on = true;
std::uint64_t g_memo_hits = 0, g_memo_misses = 0;
// hdr: the node's first 0xC0 bytes (one read: vtable, resource reference +0x58, children +0x78, flags
// +0x88, components +0x90 / +0x98, model +0xB8).
constexpr std::size_t kNodeHdr = 0xC0;
NodeMemo* Memo(std::uintptr_t e, const std::uint8_t (&hdr)[kNodeHdr]) {
    if (!g_memo_on) return nullptr;
    NodeMemo& m = g_memo[((e >> 4) * 0x9E3779B97F4A7C15ull) >> 55];  // 9 bits
    std::uintptr_t vt = 0, res = 0, comps = 0;
    std::int32_t n = -1;
    std::memcpy(&vt, hdr, 8), std::memcpy(&res, hdr + 0x58, 8), std::memcpy(&n, hdr + 0x90, 4), std::memcpy(&comps, hdr + 0x98, 8);
    if (m.e == e && m.vt == vt && m.res == res && m.comps == comps && m.ncomp == n) {
        ++g_memo_hits;
        return &m;
    }
    ++g_memo_misses;
    m = NodeMemo{};
    m.e = e, m.vt = vt, m.res = res, m.comps = comps, m.ncomp = n;
    return &m;
}
// ---------------------------------------------------------------- weapons (docs/real-partner-body.md section 17)
// A DSWeaponEntity the game shows (in a hand: parent Sam, its AttachToHelperMover on MTP_R_Hand_a /
// MTP_L_Hand_a; on the backpack hanger: parent the backpack). Read fault-free, every field checked:
//   entity +0x70 parent, +0xB0 mover, +0x338 the weapon instance (owned by the inventory)
//   instance +0x12 u8 EDSWeaponId, +0x18 DSWeaponParameter (+0x20 Id, +0x113 Lv), +0x300 owner (Sam),
//            +0x308 back reference (the entity + 0x20), +0x450 flags
//   mover (AttachToHelperMover) +0x48 its entity, +0xA0 u8 "local is identity", +0xB0 helper name
// (static analysis: 0x142911300 AttachTo, 0x1427ABCE0 EquipToHand, 0x14239B930 the mover).
bool g_weapons_on = true;  // gear 17 <0/1>
struct WeaponInfo {
    std::uint8_t id = wire::kGearNone, level = wire::kGearNone, joint = 0xFF, param_id = 0xFF, identity = 0xFF;
    bool inst_ok = false, mover_ok = false;
    std::uint32_t flags = 0;
    std::uintptr_t parent = 0;
    char helper[40] = "";
};
// A Decima String (character pointer; its length at ptr - 8) into buf, printable ASCII only.
void ReadString(std::uintptr_t p, char* buf, std::size_t cap) {
    buf[0] = 0;
    std::int32_t len = -1;
    if (!p || !Rd(p - 8, len) || len <= 0 || len > 256) return;
    const std::size_t n = static_cast<std::size_t>(len) < cap - 1 ? static_cast<std::size_t>(len) : cap - 1;
    if (!mem::Read(p, buf, n)) {
        buf[0] = 0;
        return;
    }
    buf[n] = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (static_cast<unsigned char>(buf[i]) < 0x20 || static_cast<unsigned char>(buf[i]) > 0x7E) buf[i] = '?';
}
// The 77-joint layout's attach joints (the names logged by real_names).
std::uint8_t HelperJoint(const char* n) {
    static const struct {
        const char* name;
        std::uint8_t j;
    } kHelpers[] = {{"MTP_R_Hand_a", 26}, {"MTP_R_Hand_b", 27}, {"MTP_R_Hand_c", 28}, {"MTP_L_Hand_a", 55}, {"MTP_L_Hand_b", 56},
                    {"MTP_L_Hand_c", 57}, {"MTP_Spine_a", 58},  {"MTP_Spine_b", 59},  {"MTP_Spine_c", 60}};
    for (const auto& h : kHelpers)
        if (std::strcmp(n, h.name) == 0) return h.j;
    return 0xFF;
}
void ReadWeaponInfo(std::uintptr_t e, std::uintptr_t sam, WeaponInfo* w) {
    *w = WeaponInfo{};
    w->parent = Q(e + 0x70);
    const std::uintptr_t inst = Q(e + 0x338);
    if (inst && Q(inst + 0x308) == e + 0x20 && Q(inst + 0x300) == sam) {
        std::uint8_t id = 0xFF;
        if (Rd(inst + 0x12, id) && id < 0x5D) {
            w->id = id;
            w->inst_ok = true;
            const std::uintptr_t prm = Q(inst + 0x18);
            std::uint8_t lv = 0;
            if (prm && Derives(prm, "DSWeaponParameter")) {
                Rd(prm + 0x20, w->param_id);
                if (Rd(prm + 0x113, lv) && lv >= 1 && lv <= 9) w->level = lv;
            }
            Rd(inst + 0x450, w->flags);
        }
    }
    const std::uintptr_t mover = Q(e + 0xB0);
    if (mover && Derives(mover, "AttachToHelperMover") && Q(mover + 0x48) == e) {
        w->mover_ok = true;
        Rd(mover + 0xA0, w->identity);
        ReadString(Q(mover + 0xB0), w->helper, sizeof(w->helper));
        if (w->parent == sam) w->joint = HelperJoint(w->helper);
    }
}
// What the last reads saw of each weapon (logged when it changes: drawn, holstered, the other hand).
struct WeaponSeen {
    std::uintptr_t e = 0, parent = 0;
    std::uint8_t joint = 0xFF;
    bool seen = false;
};
WeaponSeen g_wseen[16];
std::uint32_t g_weapon_logs = 0;
// The fast watch (every tick): the inventory's current weapon (0x1427A4390: the instance at inventory
// +0xD0 + 0x70 * u8 [+0x1C78]) and its entity's visible bit and parent. A change makes the next read
// happen at once and skips the layout debounce, so a draw / holster goes out in about one tick.
struct WeaponWatch {
    bool checked = false, ok = false;
    std::uintptr_t e = 0, parent = 0;
    bool vis = false;
    // the weapon that was current before (put away: the inventory's current slot changes when the wheel's
    // item is picked, the old weapon's entity is hidden later, when the hands are empty)
    std::uintptr_t prev_e = 0, prev_parent = 0;
    bool prev_vis = false;
    std::uint32_t changes = 0;
    double changed_ms = 0;  // when the watch last saw a change (latency: until the frame is taken)
    double lat_sum = 0, lat_max = 0;
    std::uint32_t lat_n = 0;
    double forced_ms = -1e9;     // when the watch last asked for a read (at most one per kWatchGapMs)
    std::uint32_t deferred = 0;  // changes whose read waited for that gap
} g_ww;
constexpr double kWatchGapMs = 40;  // a state that toggles every tick must not turn the 100 ms walk into a per-tick one

// ReadItems' cost on the game thread (QPC), logged after 50 reads and then every 600 (about a minute).
double g_read_us_sum = 0, g_read_us_max = 0;
std::uint32_t g_read_timed = 0, g_read_window = 0;

void NoteHeadItem(std::uintptr_t e, int res_id, double now);  // section 20 (below)
void ReadHoodHat(std::uintptr_t p, std::uintptr_t inv, const Xf& sam, double now);  // section 26.8 (below)
void ReadHood(std::uintptr_t inv);                                                  // section 37 (below)
// Sam's rain hood as read (section 37, ReadHood): the state 0..12, the type (-1 none), the porter's second clip set.
struct HoodRead {
    bool valid = false;
    int state = 0, type = -1;
    bool alt = false;
    int logged_state = -2, logged_type = -2;
    unsigned logs = 0;
} g_hoodr;
bool HeadConst(std::uintptr_t e, float (&p)[3], float (&r)[3][3], const char** name);  // section 26.8 (below)
void ReadBootsShown(std::uintptr_t inv);                     // section 21 (below)
bool CargoHideSites();                                       // v0.23 (below)
void ReadRainCover(std::uintptr_t inv, double now);          // section 28 (below)
// Boots (section 21; session.ini boots_sync): ours sent as STC1 kind 7, the partner's drawn as the boots prop.
struct BootsRead {
    bool checked = false, ok = false;
    std::uintptr_t e = 0;
    bool shown = false;
} g_boots;
std::uint32_t g_boots_apply = 0, g_boots_visible = 0;  // our boots' ArtParts (the last read)
std::uintptr_t g_boots_prop_e = 0;  // the partner's boots prop given to realbody::SetBootsProp (0 none)
bool g_boots_prop_shown = false;     // ... and shown on the partner body now (BootsTick; the boots meter)
int g_pfeet_pct = 1;  // `gear 20 <percent>`: the partner body's feet under the boots prop (100: not scaled)
// Round 6 T3 (section 30): how her boots look (session.ini boots_look / `gear 25 <0 prop / 1 own>`), and in the prop style
// whether intact porter boots keep her own (boots_own_porter). The visible bits sent: the last settled ones (bootslook::Ready;
// for 5-7 s after a load every part reads shown).
bootslook::Style g_boots_style = bootslook::Style::own;
bool g_boots_own_porter = true;
std::uint32_t g_boots_sent_visible = 0;
bool g_boots_have_ready = false;
void SelfBootsTick();  // section 30 (below)

// Section 35: whether the BB pod is shown, its flag or (under the self view) the game's rule / the fallback; logged when that
// changes (100 lines), on a rule-vs-flag mismatch (40 lines) and in a summary of the cross-check (every 60 s while it runs).
bool PodTick(std::uintptr_t p, std::uintptr_t inv, std::uintptr_t pod) {
    const double now = static_cast<double>(GetTickCount64());
    gearkeep::PodTick t;
    t.self_shown = realbody::SelfShown();
    t.flag = Visible(pod);
    t.plain = PlainPlay(&t.in_room);
    t.now_ms = now;
    gearkeep::PodRead r;
    if (g_pod_rule_on && ReadPodRule(p, inv, pod, &r)) {
        t.have_ask = true;
        t.ask = gearkeep::PodAsked(r);
    }
    const gearkeep::PodOut o = gearkeep::DecidePod(g_pod, t);
    auto inputs = [&](char* d, std::size_t cap) {
        std::snprintf(d, cap, "gate %d/%d, slot hidden %d, Sam visible %d, part %s, forced hide %d, mask %d; %s", r.gate_ready, r.gate_on, r.slot_hidden,
                      r.sam_visible, !r.part_mapped ? "not mapped" : r.part_asked ? "asked" : "not asked", r.forced_hide, r.mask_k,
                      t.plain ? "plain play" : "not plain play");
    };
    if (o.ev == gearkeep::PodEvent::mismatch_started && ++g_podlog.mismatches <= 40) {
        char d[200];
        inputs(d, sizeof(d));
        Log("[gear] BB pod: controller ask vs its flag: mismatch (ask %s, flag %s; %s)", t.ask ? "shown" : "hidden", t.flag ? "shown" : "hidden", d);
    } else if (o.ev == gearkeep::PodEvent::mismatch_ended && g_podlog.mismatches <= 40) {
        Log("[gear] BB pod: controller ask vs its flag: agree again (%s)", t.flag ? "shown" : "hidden");
    } else if (o.ev == gearkeep::PodEvent::disproved) {
        char d[200];
        inputs(d, sizeof(d));
        Log("[gear] BB pod: controller ask vs its flag disagree for %.1f s in plain play (ask %s, flag %s; %s): the game's rule is off for this "
            "session, under the self view her pod keeps its last plain-play state",
            gearkeep::kPodDisproveMs / 1000, t.ask ? "shown" : "hidden", t.flag ? "shown" : "hidden", d);
    }
    if (g_pod.compared != g_podlog.summary_compared && (now >= g_podlog.next_summary_ms || g_pod.compared == 50)) {
        g_podlog.summary_compared = g_pod.compared;
        g_podlog.next_summary_ms = now + 60000;
        Log("[gear] BB pod: controller ask vs its flag: %u match, %u mismatch over %u reads with the self view off%s", g_pod.matched, g_pod.mismatched,
            g_pod.compared, g_pod.disproved ? " (the rule is off for this session)" : "");
    }
    if ((!g_podlog.have || o.vis != g_podlog.vis || o.src != g_podlog.src) && ++g_podlog.changes <= 100)
        Log("[gear] BB pod: %s (%s)", o.vis ? "shown" : "hidden", gearkeep::PodSourceName(o.src));
    g_podlog.have = true, g_podlog.vis = o.vis, g_podlog.src = o.src;
    return o.vis;
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
        it.res = wire::kGearNone;
        it.parent_e = 0;
        it.hint_joint = 0xFF;
    };
    auto add_entity = [&](wire::GearKind k, std::uintptr_t e) {
        Xf w;
        if (e && ReadWorld(e, &w) && Dist(w, sam) < 5.0) add(k, wire::kGearNone, 0, e, Visible(e), w);
    };
    ReadBootsShown(inv);
    if (g_boots.shown && g_boots.e) {
        // the boots item (STC1 kind 7): which of the boots' 32 ArtParts are reported and shown (read for the self view's
        // boots style too, section 30; sent only with boots_sync)
        Apd a;
        std::uint32_t apply = 0, visible = 0;
        if (ReadApd(g_boots.e, &a))
            for (unsigned i = 0; i < static_cast<unsigned>(a.n) && i < wire::kMaxGearBootsParts; ++i) {
                std::uint8_t fl = 0;
                if (!PartFlags(a, i, &fl)) continue;
                apply |= 1u << i;
                if (fl & 0x2) visible |= 1u << i;
            }
        if (apply && (apply != g_boots_apply || visible != g_boots_visible))
            Log("[boots] our boots' ArtParts: %d, shown %08X of %08X%s", a.n, visible, apply,
                bootslook::Ready(visible & apply) ? "" : " (not settled: the last settled parts are sent)");
        g_boots_apply = apply, g_boots_visible = visible;
        // the load window (every part shown for a few seconds after a load) is never sent: the last settled parts are
        if (apply && bootslook::Ready(visible & apply)) {
            g_boots_sent_visible = visible & apply;
            g_boots_have_ready = true;
        }
        if (apply && g_boots_sync && g_boots_have_ready) add_entity(wire::GearKind::boots, g_boots.e);
    } else {
        g_boots_visible = 0;
    }
    SelfBootsTick();
    const std::uintptr_t pack = RefEntity(Q(inv + kInvPack));
    if (pack && Derives(pack, "DSBackpackEntity")) add_entity(wire::GearKind::backpack, pack);
    // The BB pod (inventory +0x4D48, a reference: entity = value - 0x20; the entity with the
    // DSBBPodComponent, which binds to the player) is drawn as the cinematic pod prop. The game shows it
    // only while Sam's own BBPod part is shown (0x1426C7760 slot 0, k 30), so under our self view (all his
    // parts hidden) its own flag reads hidden: PodTick takes the game's rule instead (section 35).
    std::uintptr_t pod = RefEntity(Q(inv + kInvBbPod));
    if (pod && !partner::FindComponentByName(reinterpret_cast<game::Entity*>(pod), "DSBBPodComponent")) pod = 0;
    if (pod) {
        Xf w;
        if (ReadWorld(pod, &w) && Dist(w, sam) < 5.0) {
            const bool vis = PodTick(p, inv, pod);
            char ppath[wire::kMaxGearResPath + 8] = {};
            std::uint8_t pguid[16] = {};
            const char* prt = nullptr;
            const char* no = nullptr;
            const int pid = g_pod_entity && MirrorableAt(pod, 1, &no) &&
                                    realbody::ResourceIdentity(reinterpret_cast<game::Entity*>(pod), ppath, sizeof(ppath), pguid, &prt)
                                ? LocalResId(ppath, pguid)
                                : -1;
            add(pid >= 0 ? wire::GearKind::entity : wire::GearKind::bb_pod, wire::kGearNone, 0, pod, vis, w);
            if (pid >= 0 && g_item_count && g_items[g_item_count - 1].e == pod) {
                g_items[g_item_count - 1].res = static_cast<std::uint8_t>(pid);
                g_items[g_item_count - 1].parent_e = p;
            }
        }
    }
    const std::uintptr_t odra = RefEntity(Q(inv + kInvOdra));
    if (verbose)
        Log("[gear] inventory %p: backpack %p (%s), Odradek %p (%s)", reinterpret_cast<void*>(inv), reinterpret_cast<void*>(pack),
            pack ? TypeOf(pack) : "-", reinterpret_cast<void*>(odra), odra ? TypeOf(odra) : "-");
    // Every other gear entity the game shows on Sam (docs/real-partner-body.md section 12): his child
    // entities and theirs (depth 3, at most 320 visited), each visible one that has a model and an
    // entity resource under ds/entities/ (weapons in hand and stowed, the BB pod and what hangs on it,
    // shields, the cuff device, hanging boots, everything on the backpack). Not sent: his boots (the
    // body's own feet), the baby in the pod (talk / voice components), the camera, effects.
    {
        struct Node {
            std::uintptr_t e, parent;
            int depth;
        };
        static Node stack[256];  // Sam has ~45 children of his own, the backpack ~15
        int sp = 0;
        unsigned visited = 0, skipped = 0;
        for (std::uintptr_t c = Q(p + 0x78), guard = 0; c && sp < 256 && guard < 128; c = Q(c + 0x80), ++guard) stack[sp++] = {c, p, 1};
        while (sp > 0 && visited < 320) {
            const Node nd = stack[--sp];
            ++visited;
            std::uint8_t hdr[kNodeHdr];
            const bool have_hdr = mem::Read(nd.e, hdr, sizeof(hdr));
            auto HQ = [&](std::size_t off) {
                std::uintptr_t v = 0;
                if (have_hdr) std::memcpy(&v, hdr + off, sizeof(v));
                else v = Q(nd.e + off);
                return v;
            };
            NodeMemo* memo = verbose || !have_hdr ? nullptr : Memo(nd.e, hdr);
            if (memo && memo->cls == 0)
                memo->cls = (!Derives(nd.e, "Entity") || Derives(nd.e, "CameraEntity") || Derives(nd.e, "DSPlayerEntity")) ? 2
                            : std::strcmp(TypeOf(nd.e), "DSWeaponEntity") == 0                                           ? 3
                                                                                                                         : 1;
            if (memo ? memo->cls == 2 : (!Derives(nd.e, "Entity") || Derives(nd.e, "CameraEntity") || Derives(nd.e, "DSPlayerEntity"))) continue;
            const bool weapon = memo ? memo->cls == 3 : std::strcmp(TypeOf(nd.e), "DSWeaponEntity") == 0;
            const bool vis = (HQ(kEntFlags) & 0x2) != 0;
            // children first pushed (visited after this one: a parent is always added before them)
            // (a hidden backpack hides what hangs on it: the game draws a hidden parent's children hidden, whatever their own flag
            // says; v0.23, the trike driver's HideBackpack: none of it is sent then)
            if (nd.depth < 3 && !(nd.e == pack && !vis))
                for (std::uintptr_t c = HQ(0x78), guard = 0; c && sp < 256 && guard < 64; c = Q(c + 0x80), ++guard) stack[sp++] = {c, nd.e, nd.depth + 1};
            if (!vis || nd.e == pack || !HQ(0xB8)) continue;
            if (nd.e == pod) continue;  // sent as the cinematic BB pod (above)
            if (weapon) {
                // A weapon (section 17): its own kind, never the entity rule (never copied). The resource is
                // cached like an entity's (it does not change while the node keeps its resource reference).
                if (!g_weapons_on || (memo && memo->verdict == -1)) continue;
                int wid = memo && memo->verdict >= 0 ? memo->verdict : -1;
                if (wid < 0) {
                    char wpath[wire::kMaxGearResPath + 8] = {};
                    std::uint8_t guid[16] = {};
                    const char* rt = nullptr;
                    if (!realbody::ResourceIdentity(reinterpret_cast<game::Entity*>(nd.e), wpath, sizeof(wpath), guid, &rt) || !wire::GearResPathOk(wpath) ||
                        std::strncmp(wpath, "ds/entities/weapons/", 20) != 0) {
                        if (memo) memo->verdict = -1;
                        continue;
                    }
                    if (!WeaponFor(wpath) && !heldrules::Wanted(g_held_mode, wpath, false)) {
                        // no prop on the other screen (section 17.7; held_items off or not wanted, section 31): not sent, and no
                        // id of the 64 used up
                        static unsigned s_noprop_logs = 0;
                        if (memo) memo->verdict = -1;
                        if (++s_noprop_logs <= 20) Log("[gear] our weapon '%s': no prop for it on the other screen (not sent)", wpath);
                        continue;
                    }
                    wid = LocalResId(wpath, guid);
                    if (wid < 0) continue;
                    if (memo) memo->verdict = static_cast<std::int16_t>(wid);
                }
                Xf w;
                if (!ReadWorld(nd.e, &w) || Dist(w, sam) > 3.0) continue;
                WeaponInfo wi;
                ReadWeaponInfo(nd.e, p, &wi);
                add(wire::GearKind::weapon, wi.id, 0, nd.e, true, w);
                if (g_item_count && g_items[g_item_count - 1].e == nd.e) {
                    LocalItem& li = g_items[g_item_count - 1];
                    li.res = static_cast<std::uint8_t>(wid);
                    li.parent_e = nd.parent;
                    li.variation = wi.level;
                    li.hint_joint = wi.joint;
                }
                // logged when it changes (drawn, holstered, the other hand) or in a verbose read
                WeaponSeen* ws = nullptr;
                for (WeaponSeen& k : g_wseen)
                    if (k.e == nd.e) ws = &k;
                const bool changed = !ws || ws->parent != nd.parent || ws->joint != wi.joint;
                if (!ws)
                    for (WeaponSeen& k : g_wseen)
                        if (!k.e) {
                            ws = &k;
                            break;
                        }
                if (ws) ws->e = nd.e, ws->parent = nd.parent, ws->joint = wi.joint, ws->seen = true;
                if (verbose || (changed && ++g_weapon_logs <= 200))
                    Log("[gear] our weapon %p res %d '%s': weapon id %d (parameter id %d) level %d, flags 0x%X, %s; parent %p (%s), mover %s helper '%s' "
                        "(joint %d, local %s), %.2f m from Sam",
                        reinterpret_cast<void*>(nd.e), wid, static_cast<unsigned>(wid) < g_res_local_n ? g_res_local[wid].path : "?",
                        wi.id == wire::kGearNone ? -1 : wi.id, wi.param_id == 0xFF ? -1 : wi.param_id, wi.level == wire::kGearNone ? -1 : wi.level,
                        wi.flags, wi.inst_ok ? "instance ok" : "INSTANCE NOT CONFIRMED", reinterpret_cast<void*>(nd.parent),
                        nd.parent == p ? "Sam" : nd.parent == pack ? "the backpack" : TypeOf(nd.parent), wi.mover_ok ? "ok" : "NOT CONFIRMED", wi.helper,
                        wi.joint == 0xFF ? -1 : wi.joint, wi.identity == 1 ? "identity" : wi.identity == 0 ? "a matrix" : "?", Dist(w, sam));
                continue;
            }
            if (memo && memo->verdict == -1) {
                ++skipped;
                continue;
            }
            if (memo && memo->verdict == -2) continue;
            int id = memo && memo->verdict >= 0 ? memo->verdict : -1;
            char path[wire::kMaxGearResPath + 8] = {};
            const char* rt = nullptr;
            if (id < 0) {
                std::uint8_t guid[16] = {};
                if (!realbody::ResourceIdentity(reinterpret_cast<game::Entity*>(nd.e), path, sizeof(path), guid, &rt) || !wire::GearResPathOk(path) ||
                    std::strncmp(path, "ds/entities/effects/", 20) == 0 || std::strncmp(path, "ds/entities/player/camera", 25) == 0) {
                    if (memo) memo->verdict = -1;
                    ++skipped;
                    continue;
                }
                const char* not_sent = nullptr;
                if (!SendableEntity(nd.e, &not_sent)) {
                    if (memo) memo->verdict = -2;
                    if (verbose) Log("[gear]   entity %p %s '%s' not mirrored: %s", reinterpret_cast<void*>(nd.e), TypeOf(nd.e), path, not_sent);
                    continue;
                }
                id = LocalResId(path, guid);
                if (id < 0) continue;  // table full: not cached (counted in g_res_full)
                if (memo) memo->verdict = static_cast<std::int16_t>(id);
            }
            Xf w;
            if (!ReadWorld(nd.e, &w) || Dist(w, sam) > 3.0) continue;
            NoteHeadItem(nd.e, id, static_cast<double>(GetTickCount64()));
            add(wire::GearKind::entity, wire::kGearNone, 0, nd.e, true, w);
            if (g_item_count && g_items[g_item_count - 1].e == nd.e) {
                g_items[g_item_count - 1].res = static_cast<std::uint8_t>(id);
                g_items[g_item_count - 1].parent_e = nd.parent;
            }
            if (verbose)
                Log("[gear]   entity %p %s res %d '%s' (%s), parent %p", reinterpret_cast<void*>(nd.e), TypeOf(nd.e), id, path, rt ? rt : "-",
                    reinterpret_cast<void*>(nd.parent));
        }
        if (verbose) Log("[gear] entity walk: %u visited, %u not under ds/entities", visited, skipped);
        // weapons no longer shown (holstered into a pouch, thrown, put away)
        for (WeaponSeen& k : g_wseen) {
            if (k.e && !k.seen) {
                if (++g_weapon_logs <= 200) Log("[gear] our weapon %p is no longer shown", reinterpret_cast<void*>(k.e));
                k = WeaponSeen{};
            }
            k.seen = false;
        }
    }
    // Sam's rain hood (section 37), then her hat in the rain (section 26.8): the worn hat the game hides while Sam's hood is
    // up, as a shown head item, unless her own hood is drawn up over it.
    ReadHood(inv);
    ReadHoodHat(p, inv, sam, static_cast<double>(GetTickCount64()));
    // What wears out (section 28): the rain cover's durability, logged in steps (read only, every second).
    ReadRainCover(inv, static_cast<double>(GetTickCount64()));
    // Read through the self view too (section 32: the controller's formula without our bit; the head's flags keep the last
    // reading then).
    ReadParts(p);
    // The backpack's own ArtParts (its bag, rain cover, pouches: 24 parts in the 100% save).
    g_have_pack_parts = false;
    if (pack) {
        Apd a;
        if (ReadApd(pack, &a)) {
            g_pack_apply = g_pack_visible = 0;
            for (unsigned i = 0; i < static_cast<unsigned>(a.n) && i < 64; ++i) {
                std::uint8_t fl = 0;
                if (!PartFlags(a, i, &fl)) continue;
                g_pack_apply |= 1ull << i;
                if (fl & 0x2) g_pack_visible |= 1ull << i;
            }
            g_have_pack_parts = g_pack_apply != 0;
        }
    }

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
    unsigned pieces = 0, refused = 0, walked = 0, hidden = 0;
    // v0.23: the game's own cargo hides mirrored (the trike driver's HideBackpack while the partner rides behind him, a
    // cutscene's HideBaggage, the Root mask): a hidden piece is not sent. Without the checks every piece counts as shown (v0.22).
    const bool hide_sites = CargoHideSites();
    std::uint32_t car_hide = 0;
    if (hide_sites && !Rd(car + kCarHideMask, car_hide)) car_hide = 0;
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
        std::uint8_t sflags = 0;
        const bool slot_shown = car_hide == 0 && !(hide_sites && type == 1 && Rd(s + kSlotHideByte, sflags) && (sflags & kSlotHiddenBit));
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
                    std::int32_t eidx = -2;
                    Rd(bag + 0xD8, eidx);
                    std::uint8_t lx[0x10] = {};
                    if (li_ok) mem::Read(li + 0x30, lx, sizeof(lx));
                    Log("[gear]     slot type %u stack %d piece %d: %p entity index %d model byte %u (+0x21 %u), list item %08X attribute %u case %u volume %u "
                        "weight %.1f [+0x30: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X] -> model %u (%s); at %.3f %.3f %.3f (%.2f m from Sam)%s",
                        type, ti, bi, reinterpret_cast<void*>(bag), eidx, mb[0], mb[1], id, f[0], f[1], f[4], weight, lx[0], lx[1], lx[2], lx[3], lx[4], lx[5],
                        lx[6], lx[7], lx[8], lx[9], lx[10], lx[11], lx[12], lx[13], lx[14], lx[15], from_li,
                        model != wire::kGearNone ? "drawn" : "not drawn: equipment / weapon / tool", w.p[0], w.p[1], w.p[2], Dist(w, sam),
                        placed ? "" : " NOT PLACED");
                }
                ++pieces;
                if (!slot_shown) ++hidden;
                if (placed && model != wire::kGearNone) add(wire::GearKind::cargo, model, type, bag, slot_shown, w);
            }
        }
    }
    if (cut) {
        static bool logged = false;
        if (!logged || verbose) Log("[gear] carrier %p: more than %u stacks + pieces: walk cut short", reinterpret_cast<void*>(car), kMaxWalk);
        logged = true;
    }
    if (verbose)
        Log("[gear] carrier %p: %d slots, %u pieces read (%u refused, %u hidden by the game: hide mask 0x%X)", reinterpret_cast<void*>(car), slots, pieces,
            refused, hidden, car_hide);
    {
        // the hidden count's changes (the trike pillion's hide and its end: n -> 0 -> n shown)
        static unsigned s_hidden = 0, s_logs = 0;
        if (hidden != s_hidden && ++s_logs <= 40)
            Log("[gear] our carried cargo: %u of %u pieces hidden by the game now (carrier hide mask 0x%X)", hidden, pieces, car_hide);
        s_hidden = hidden;
    }
    return true;
}

// Entity items keep their anchor joint while it stays near (Sam's joint chosen per entity).
struct JointMemo {
    std::uintptr_t e = 0;
    std::uint8_t j = 0;
};
JointMemo g_joint_memo[64];
unsigned g_joint_memo_next = 0;
void RememberJoint(std::uintptr_t e, std::uint8_t j) {
    for (JointMemo& m : g_joint_memo)
        if (m.e == e) {
            m.j = j;
            return;
        }
    g_joint_memo[g_joint_memo_next++ % 64] = {e, j};
}

// Builds the frame from g_items: anchors and relative poses. `prev`: keep its poses for items whose
// layout matches (Sam moving: the tick's reads of Sam and the gear may be a frame apart).
bool BuildFrame(game::Entity* local, const Xf& sam, bool self_hidden, bool keep_poses, wire::GearFrame* out, std::uintptr_t* out_e) {
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
    bool boots_added = false;
    unsigned cargo = 0, dropped = 0;
    bool head_c[wire::kMaxGearItems] = {};
    std::uintptr_t frame_e[wire::kMaxGearItems] = {};
    Xf frame_w[wire::kMaxGearItems];
    (void)self_hidden;  // the self view hides Sam through the global part mask: his gear stays shown (real flags)
    // The cargo comes last but must not be what the cap cuts (the pieces stacked on the backpack are
    // what the user sees): the other items leave it room.
    unsigned want_cargo = static_cast<unsigned>(g_test_cases);
    for (unsigned i = 0; i < g_item_count; ++i) want_cargo += g_items[i].kind == wire::GearKind::cargo;
    const unsigned other_cap = want_cargo >= wire::kMaxGearItems - 8 ? 8 : wire::kMaxGearItems - want_cargo;
    for (unsigned i = 0; i < g_item_count; ++i) {
        const LocalItem& it = g_items[i];
        // Everything as the game shows it (v0.23: cargo too: the carrier's hides, ReadItems; v0.22 sent every piece).
        const bool vis = it.visible;
        if (!vis) continue;
        if (it.kind == wire::GearKind::cargo) ++cargo;
        if (f.count >= wire::kMaxGearItems || (it.kind != wire::GearKind::cargo && it.kind != wire::GearKind::backpack && f.count >= other_cap)) {
            ++dropped;
            continue;
        }
        wire::GearItem gi;
        gi.kind = it.kind;
        gi.model = it.model;
        gi.variation = it.variation;
        gi.res = it.res;
        const Xf* anchor = nullptr;
        int parent_item = -1;
        float hc_p[3], hc_r[3][3];
        const char* hc_name = "";
        bool head_const = false;
        if ((it.kind == wire::GearKind::entity || it.kind == wire::GearKind::weapon) && it.parent_e)
            for (unsigned k = 0; k < f.count; ++k)
                if (frame_e[k] == it.parent_e) parent_item = static_cast<int>(k);
        if (parent_item >= 0) {
            gi.parent = static_cast<std::uint8_t>(parent_item);
            anchor = &frame_w[parent_item];
        } else if (it.kind == wire::GearKind::entity && HeadConst(it.e, hc_p, hc_r, &hc_name) && joint(31)) {
            // a head item we offset (section 26.8): its pose on joint 31 is a constant (the helper's axes, the offset), so
            // the other screen gets exactly that, never a reading that mixes Sam's frame with the per-frame correction
            gi.joint = 31;
            anchor = joint(31);
            head_const = true;
        } else if (it.kind == wire::GearKind::weapon && it.hint_joint < 77 && joint(it.hint_joint)) {
            // a weapon in a hand: the very joint its mover's helper names (MTP_R_Hand_a ...), so its pose
            // relative to the anchor is the mover's constant local and the hand carries it on the body
            gi.joint = it.hint_joint;
            anchor = joint(gi.joint);
        } else if (it.kind == wire::GearKind::entity || it.kind == wire::GearKind::bb_pod || it.kind == wire::GearKind::weapon) {
            // the nearest joint of Sam's skeleton, kept while it is at most 15 cm farther than the nearest
            // (a layout that flips between two joints would respawn the prop)
            // Torso joints win over a limb joint that is at most 10 cm nearer (an arm swinging past the
            // chest must not carry the BB pod away).
            // (root, hips, spine 2-5, neck, head: the shoulders' joint frames differ between the
            // characters' skeletons, a pod anchored there turns on Fragile)
            static const bool kTorso[77] = {true, true, true, true, true, true, false, false, false, false, false, false, false, false, false,
                                            false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                                            true, true};
            int best = -1, best_torso = -1;
            double bd = 1e9, bt = 1e9;
            for (unsigned j = 0; j < 77; ++j) {
                const Xf* jw = joint(j);
                if (!jw) continue;
                const double d = Dist(*jw, it.world);
                if (d < bd) bd = d, best = static_cast<int>(j);
                if (kTorso[j] && d < bt) bt = d, best_torso = static_cast<int>(j);
            }
            if (best < 0) continue;
            if (best_torso >= 0 && bt <= bd + 0.10) best = best_torso, bd = bt;
            int keep = -1;
            for (const JointMemo& m : g_joint_memo)
                if (m.e == it.e) keep = m.j;
            if (keep >= 0 && joint(static_cast<unsigned>(keep)) && Dist(*joint(static_cast<unsigned>(keep)), it.world) <= bd + 0.15) best = keep;
            RememberJoint(it.e, static_cast<std::uint8_t>(best));
            gi.joint = static_cast<std::uint8_t>(best);
            anchor = joint(gi.joint);
        } else if (it.kind == wire::GearKind::cargo && it.slot_type == 1 && pack_index >= 0) {
            gi.parent = static_cast<std::uint8_t>(pack_index);
            anchor = &pack_world;
        } else {
            std::uint8_t j = kJSpine4;
            if (it.kind == wire::GearKind::boots) j = 0;
            else if (it.kind == wire::GearKind::odradek) j = kJLShoulder;
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
        if (head_const) {
            // the reading against the constant, a few times per session (the helper's axes and the offset proven on Sam)
            static unsigned s_cmp_logs = 0;
            float rq[3][3];
            for (int a = 0; a < 3; ++a)
                for (int c = 0; c < 3; ++c) rq[a][c] = rr[a][c];
            double dp = 0, dot = 0;
            for (int k = 0; k < 3; ++k) dp += (rp[k] - hc_p[k]) * (rp[k] - hc_p[k]);
            for (int a = 0; a < 3; ++a)
                for (int c = 0; c < 3; ++c) dot += static_cast<double>(rq[a][c]) * hc_r[a][c];
            const double cosang = (dot - 1.0) / 2.0, ang = std::acos(cosang < -1 ? -1 : cosang > 1 ? 1 : cosang) * 57.29578;
            if (s_cmp_logs < 12 && Visible(it.e) && std::isfinite(dp)) {
                ++s_cmp_logs;
                Log("[headgear] sender: %s on joint 31 sent as (%.2f, %.2f, %.2f) cm; read off Sam now (%.2f, %.2f, %.2f) cm, %.2f cm / %.1f deg apart", hc_name,
                    hc_p[0] * 100.0, hc_p[1] * 100.0, hc_p[2] * 100.0, rp[0] * 100.0, rp[1] * 100.0, rp[2] * 100.0, std::sqrt(dp) * 100.0, ang);
            }
            for (int k = 0; k < 3; ++k) rp[k] = hc_p[k];
            for (int a = 0; a < 3; ++a)
                for (int c = 0; c < 3; ++c) rr[a][c] = hc_r[a][c];
        }
        if (std::fabs(rp[0]) > 30 || std::fabs(rp[1]) > 30 || std::fabs(rp[2]) > 30) continue;
        gi.p[0] = static_cast<float>(rp[0]), gi.p[1] = static_cast<float>(rp[1]), gi.p[2] = static_cast<float>(rp[2]);
        gi.q = wire::Normalize(wire::QuatFromMatrix(rr));
        if (it.kind == wire::GearKind::boots) {
            // the boots follow the legs, not a frame: the receiver's prop takes the body's own leg joints on its root
            gi.p[0] = gi.p[1] = gi.p[2] = 0;
            gi.q = wire::Quat{};
            if (boots_added) continue;
            boots_added = true;
        }
        if (it.kind == wire::GearKind::backpack && pack_index < 0) {
            pack_index = static_cast<int>(f.count);
            pack_world = it.world;
        }
        frame_e[f.count] = it.e;
        frame_w[f.count] = it.world;
        head_c[f.count] = head_const;
        f.items[f.count++] = gi;
    }
    if (boots_added) {
        f.flags |= wire::kGearBoots;
        f.boots_apply = g_boots_apply;
        f.boots_visible = g_boots_sent_visible & g_boots_apply;
    }
    if (g_have_pack_parts && pack_index >= 0) {
        f.flags |= wire::kGearPackParts;
        f.pack_apply = g_pack_apply;
        f.pack_visible = g_pack_visible;
    }
    if (g_have_parts) {
        f.flags |= wire::kGearParts;
        f.parts_apply[0] = g_parts_apply[0], f.parts_apply[1] = g_parts_apply[1];
        f.parts_visible[0] = g_parts_visible[0], f.parts_visible[1] = g_parts_visible[1];
    }
    // STC1 v5: Sam's rain hood (section 37)
    if (g_hoodr.valid) {
        f.hood = wire::PackGearHood(g_hoodr.state);
        f.hood_info = wire::PackGearHoodInfo(g_hoodr.type, g_hoodr.alt);
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
    // Sam moving: a pose relative to one of his joints may mix two frames (the joint and the item read
    // a frame apart), so those keep the last still reading. An item on another item (the cargo and
    // everything on the backpack) is read in the same pass as its parent: its pose stays live (the
    // stack's sway).
    if (keep_poses && g_have_local && wire::GearLayoutHash(f) == wire::GearLayoutHash(g_local) && f.count == g_local.count) {
        for (unsigned i = 0; i < f.count; ++i) {
            if (f.items[i].parent != wire::kGearNoParent || head_c[i]) continue;
            std::memcpy(f.items[i].p, g_local.items[i].p, sizeof(f.items[i].p));
            f.items[i].q = g_local.items[i].q;
        }
    }
    if (out_e)
        for (unsigned i = 0; i < wire::kMaxGearItems; ++i) out_e[i] = i < f.count ? frame_e[i] : 0;
    *out = f;
    return true;
}

void DescribeFrame(const wire::GearFrame& f, const char* what, const ResEntry* res) {
    unsigned per[kPropCount] = {}, none = 0, approx = 0, pack = 0, odra = 0, pod = 0, cargo = 0, ents = 0, weapons = 0, weapons_drawn = 0;
    for (unsigned i = 0; i < f.count; ++i) {
        const wire::GearItem& it = f.items[i];
        const int p = PropFor(it, res);
        if (it.kind == wire::GearKind::weapon) {
            ++weapons;
            weapons_drawn += p >= 0;
            const char* path = res && it.res < wire::kMaxGearRes && res[it.res].used ? res[it.res].path : "?";
            static unsigned logged = 0;
            if (++logged <= 300)
                Log("[gear] %s: weapon item %u '%s' (weapon id %d, level %d) on %s %u: %s", what, i, path, it.model == wire::kGearNone ? -1 : it.model,
                it.variation == wire::kGearNone ? -1 : it.variation, it.parent == wire::kGearNoParent ? "joint" : "item",
                it.parent == wire::kGearNoParent ? it.joint : it.parent,
                p < 0 ? "no prop for it (not drawn)" : IsHeldKey(p) ? "a held clone of its own resource" : kProps[p].key);
        }
        if (p >= 0 && p < kPropCount) ++per[p];
        else if (p < 0) ++none;
        ents += it.kind == wire::GearKind::entity;
        if (it.kind == wire::GearKind::cargo) {
            ++cargo;
            if (it.model >= 31 || !kModelMap[it.model].exact) ++approx;
        }
        pack += it.kind == wire::GearKind::backpack;
        odra += it.kind == wire::GearKind::odradek;
        pod += it.kind == wire::GearKind::bb_pod;
    }
    unsigned parts = 0, parts_vis = 0;
    if (f.flags & wire::kGearParts)
        for (unsigned k = 0; k < 2; ++k)
            for (unsigned b = 0; b < 64; ++b) {
                parts += (f.parts_apply[k] >> b) & 1;
                parts_vis += (f.parts_visible[k] >> b) & 1;
            }
    Log("[gear] %s: %u items (backpack %u, Odradek %u, BB pod %u, cargo %u: %u approximate, %u not drawn; game entities %u; dropped over the cap %u), "
        "layout %04X; props S %u M %u L %u XL %u cube %u; weapons %u (%u drawn); Sam's gear parts %u of %u shown",
        what, f.count, pack, odra, pod, cargo, approx, none, ents, f.dropped, wire::GearLayoutHash(f), per[kPropS], per[kPropM], per[kPropL], per[kPropXL],
        per[kPropCube], weapons, weapons_drawn, parts_vis, parts);
}

// Section 28: a layout change names the game entities it lost and gained (the layout lines only count them), and says the
// rain cover's removal in one line (the game wore it out or took it off; it goes from Fragile on both screens with it).
// delta[r]: how many more items of resource r the old layout had than the new one. `ours`: our own gear (the cover's wear
// read says why it went: round 11, a save without a cover is not timefall); the partner's: the general wording.
gearwear::CoverGone OurCoverGone(double* pc);  // section 28 (below)
void NameResChanges(const int (&delta)[wire::kMaxGearRes], const ResEntry* res, const char* who, bool ours) {
    static unsigned logs = 0;
    for (unsigned r = 0; r < wire::kMaxGearRes; ++r) {
        if (!delta[r] || ++logs > 200) continue;
        const ResEntry* e = res && res[r].used ? &res[r] : nullptr;
        Log("[gear] %s %s %d item(s) of resource %u '%s' %02x%02x%02x%02x...", who, delta[r] > 0 ? "lost" : "gained", delta[r] > 0 ? delta[r] : -delta[r], r,
            e ? e->path : "?", e ? e->guid[3] : 0, e ? e->guid[2] : 0, e ? e->guid[1] : 0, e ? e->guid[0] : 0);
        if (e && gearwear::IsRainCover(e->path, e->guid)) {
            if (delta[r] < 0) {
                Log("[gear] %s: the backpack's rain cover is back (a new one worn)", who);
                continue;
            }
            double pc = 0;
            const gearwear::CoverGone why = ours ? OurCoverGone(&pc) : gearwear::CoverGone::unknown;
            if (why == gearwear::CoverGone::worn_out || why == gearwear::CoverGone::taken_off)
                Log("[gear] %s: the backpack's rain cover %s (durability %.2f %% at the last read that had it worn)", who, gearwear::CoverGoneText(why), pc);
            else
                Log("[gear] %s: the backpack's rain cover %s", who, gearwear::CoverGoneText(why));
        }
    }
}
void NameLayoutChange(const wire::GearFrame& was, const wire::GearFrame& now, const ResEntry* res, const char* who) {  // our gear
    int delta[wire::kMaxGearRes] = {};
    for (unsigned i = 0; i < was.count; ++i)
        if (was.items[i].kind == wire::GearKind::entity && was.items[i].res < wire::kMaxGearRes) ++delta[was.items[i].res];
    for (unsigned i = 0; i < now.count; ++i)
        if (now.items[i].kind == wire::GearKind::entity && now.items[i].res < wire::kMaxGearRes) --delta[now.items[i].res];
    NameResChanges(delta, res, who, true);
}

// Our gear's looks (every 500 ms; a carried case's variation once per piece): each item of g_local that
// is one of the game's entities (its variations and shader variables), the backpack, the delivery cases
// (the baggage manager's own variation lookup) and the synthetic test cases (the config's v01 of their
// size). The BB pod and Odradek props are cinematic stand-ins: not coloured.
std::uintptr_t g_local_player = 0;  // the player of the last read (look sources)

double g_look_us_sum = 0, g_look_us_max = 0;  // ReadLooks' cost (QPC), logged with the read cost
std::uint32_t g_look_window = 0;

// Our weapon item that the other screen draws as a held clone (section 31).
bool HeldItem(const wire::GearItem& it) { return it.kind == wire::GearKind::weapon && IsHeldKey(PropFor(it, g_res_local)); }

void ReadLooks(double now) {
    if (!look::ReadAvailable()) return;
    // Our own gear entities are a source of the partner's variations (on and off): the list every read
    // (100 ms), so entries found through gear we no longer hold are dropped before any use.
    static std::uintptr_t own[wire::kMaxGearItems];
    unsigned nown = 0;
    for (unsigned i = 0; i < g_local.count; ++i)
        if ((g_local.items[i].kind == wire::GearKind::entity || g_local.items[i].kind == wire::GearKind::backpack ||
             g_local.items[i].kind == wire::GearKind::boots || HeldItem(g_local.items[i])) &&
            g_local_e[i])
            own[nown++] = g_local_e[i];
    look::SetOwnGear(own, nown, g_local_player);
    if (now < g_next_look_ms || !g_looks_on) return;
    g_next_look_ms = now + 500;
    LARGE_INTEGER t0, t1, qf;
    QueryPerformanceCounter(&t0);
    const std::uint16_t layout = wire::GearLayoutHash(g_local);
    for (unsigned i = 0; i < wire::kMaxGearItems; ++i) {
        LocalLook& L = g_look[i];
        if (i >= g_local.count) {
            L.have = false;
            continue;
        }
        const wire::GearItem& it = g_local.items[i];
        const std::uintptr_t e = g_local_e[i];
        wire::GearLook l;
        bool ok = false;
        if (it.kind == wire::GearKind::entity || it.kind == wire::GearKind::backpack || it.kind == wire::GearKind::boots || HeldItem(it)) {
            // (a held item, section 31: its ArtParts too: the clone has no weapon logic to show its parts, and its variation)
            ok = e && look::ReadEntity(e, &l);
        } else if (it.kind == wire::GearKind::cargo) {
            if (e && L.have && L.e == e) {
                l = L.l;  // a carried piece keeps its variation
                ok = true;
            } else if (e) {
                ok = look::ReadBaggage(e, &l);
            } else {
                const int p = PropFor(it, nullptr);
                ok = (p == kPropS || p == kPropM || p == kPropL) && look::ReadConfigVariation(static_cast<unsigned>(p - kPropS + 1), &l);
            }
        }
        if (!ok) {
            L.have = false;
            continue;
        }
        l.item = static_cast<std::uint8_t>(i);
        l.layout = layout;
        l.kind = it.kind;
        l.res = it.kind == wire::GearKind::entity ? it.res : wire::kGearNone;
        L.l = l;
        L.e = e;
        L.have = true;
        L.hash = wire::GearLookHash(l);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&qf);
    const double us = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(qf.QuadPart);
    g_look_us_sum += us;
    if (us > g_look_us_max) g_look_us_max = us;
    ++g_look_window;
}

// Our backpack's skinned pose (every read: its frame extends with the stack, its cover joints shape
// the rain cover; the receiver writes it onto its copies).
void ReadBackpackPose() {
    g_have_pose_local = false;
    if (!g_poses_on) return;
    for (unsigned i = 0; i < g_local.count; ++i) {
        if (g_local.items[i].kind != wire::GearKind::backpack || !g_local_e[i]) continue;
        if (look::ReadPose(g_local_e[i], &g_pose_local)) {
            g_pose_local.item = static_cast<std::uint8_t>(i);
            g_pose_local.layout = wire::GearLayoutHash(g_local);
            g_pose_local.flags = 0;  // the backpack's stream
            g_have_pose_local = true;
        }
        break;
    }
}
// The pose of the item in our hand that the other screen draws as a held clone (section 31): its own skeleton's local joints.
void ReadHeldPose() {
    g_have_hpose_local = false;
    if (!g_poses_on || g_held_mode == heldrules::Mode::off) return;
    for (unsigned i = 0; i < g_local.count; ++i) {
        const wire::GearItem& it = g_local.items[i];
        if (it.kind != wire::GearKind::weapon || it.parent != wire::kGearNoParent || !g_local_e[i] || !IsHeldKey(PropFor(it, g_res_local))) continue;
        if (look::ReadPose(g_local_e[i], &g_hpose_local) && g_hpose_local.n > 1) {
            g_hpose_local.item = static_cast<std::uint8_t>(i);
            g_hpose_local.layout = wire::GearLayoutHash(g_local);
            g_hpose_local.flags = wire::kGearPoseHeld;  // the held item's stream: never taken for the backpack's
            g_have_hpose_local = true;
        } else {
            static std::uintptr_t s_logged = 0;
            static unsigned s_logs = 0;
            if (s_logged != g_local_e[i] && ++s_logs <= 20) {
                s_logged = g_local_e[i];
                std::uintptr_t sk = 0;
                std::uint32_t lay = 0;
                std::int32_t c = 0;
                const bool have = look::PoseSkeleton(g_local_e[i], &sk, &lay, &c);
                Log("[gear] our held item %u: its own pose is not read (skeleton %s, %08X, %d joints; over %u or one joint: not sent)", i, have ? "ok" : "unreadable",
                    lay, c, wire::kMaxGearPoseJoints);
            }
        }
        return;
    }
}

// The layout without its weapon items (16-bit FNV-1a like GearLayoutHash): two frames that differ only in
// their weapons skip the debounce (a draw / holster goes out at once; a weapon prop is cheap to keep).
std::uint16_t NonWeaponHash(const wire::GearFrame& f) {
    std::uint32_t h = 2166136261u;
    auto mix = [&h](std::uint8_t b) {
        h ^= b;
        h *= 16777619u;
    };
    for (unsigned i = 0; i < f.count && i < wire::kMaxGearItems; ++i) {
        const wire::GearItem& it = f.items[i];
        if (it.kind == wire::GearKind::weapon) continue;
        mix(static_cast<std::uint8_t>(it.kind));
        mix(it.model);
        mix(it.variation);
        mix(it.parent == wire::kGearNoParent ? it.joint : std::uint8_t{0});
        mix(it.parent);
        mix(it.res);
    }
    return static_cast<std::uint16_t>((h >> 16) ^ (h & 0xFFFFu));
}
unsigned WeaponCount(const wire::GearFrame& f) {
    unsigned n = 0;
    for (unsigned i = 0; i < f.count; ++i) n += f.items[i].kind == wire::GearKind::weapon;
    return n;
}

// The fast weapon watch (every tick, a few fault-free reads; see WeaponWatch).
void WatchWeapon(game::Entity* local, double now) {
    if (!g_weapons_on || !local) return;
    const std::uintptr_t b = mem::Base();
    if (!g_ww.checked) {
        g_ww.checked = true;
        g_ww.ok = mem::CodeMatches(b + 0x27A4390, "0F B6 81 78 1C 00 00 48 6B D0 70 48 8B 84 0A D0 00 00 00 33 C9 80 78 12");
        Log("[gear] weapon watch: the current-weapon getter at ds.exe+0x27A4390 %s", g_ww.ok ? "verified (watched every tick)" : "does not match: reads every 100 ms only");
    }
    if (!g_ww.ok) return;
    const auto p = reinterpret_cast<std::uintptr_t>(local);
    if (Q(p) != b + kPlayerVtable) return;
    const std::uintptr_t inv = Q(p + kPInv);
    if (!inv || Q(inv + kCompOwner) != p) return;
    std::uint8_t slot = 0xFF;
    std::uintptr_t e = 0;
    if (Rd(inv + 0x1C78, slot) && slot < 64) {
        const std::uintptr_t inst = Q(inv + 0xD0 + 0x70ull * slot);
        std::uint8_t id = 0x5D;
        if (inst && Rd(inst + 0x12, id) && id < 0x5D) {
            const std::uintptr_t back = Q(inst + 0x308);
            if (back > 0x20 && Q(back - 0x20 + 0x338) == inst) e = back - 0x20;
        }
    }
    const bool vis = e && Visible(e);
    const std::uintptr_t parent = e ? Q(e + 0x70) : 0;
    // the previous weapon (while it is still a weapon entity of ours: the instance's back reference)
    if (g_ww.prev_e && Q(Q(g_ww.prev_e + 0x338) + 0x308) != g_ww.prev_e + 0x20) g_ww.prev_e = 0;
    const bool prev_vis = g_ww.prev_e && Visible(g_ww.prev_e);
    const std::uintptr_t prev_parent = g_ww.prev_e ? Q(g_ww.prev_e + 0x70) : 0;
    const bool prev_changed = g_ww.prev_e && (prev_vis != g_ww.prev_vis || prev_parent != g_ww.prev_parent);
    g_ww.prev_vis = prev_vis, g_ww.prev_parent = prev_parent;
    if (e == g_ww.e && vis == g_ww.vis && parent == g_ww.parent && !prev_changed) return;
    if (e != g_ww.e && g_ww.e) {
        g_ww.prev_e = g_ww.e;
        g_ww.prev_vis = g_ww.vis, g_ww.prev_parent = g_ww.parent;
    }
    g_ww.e = e, g_ww.vis = vis, g_ww.parent = parent;
    ++g_ww.changes;
    g_ww.changed_ms = now;
    // read now, or as soon as the gap since the last forced read allows (the read itself takes a
    // weapon-only layout change without the debounce: NonWeaponHash in ReadLocalTick)
    const double earliest = g_ww.forced_ms + kWatchGapMs;
    if (now >= earliest) {
        g_next_read_ms = 0;
        g_ww.forced_ms = now;
    } else {
        if (g_next_read_ms > earliest) g_next_read_ms = earliest;
        g_ww.forced_ms = earliest;
        ++g_ww.deferred;
    }
}

// Section 35: what the private room took (gearkeep::RegainTick over our sent frame, the cap case of round 10): a line on
// entering it, and one once play is plain again after leaving it, naming what Sam wore before it and does not now.
constexpr unsigned kKeepKeys = wire::kMaxGearRes + 3;  // the gear resources, then the BB pod, the backpack, the Odradek
constexpr unsigned kKeyPod = wire::kMaxGearRes, kKeyPack = kKeyPod + 1, kKeyOdra = kKeyPod + 2;
gearkeep::RoomRegain<kKeepKeys> g_regain;
void RoomRegainTick() {
    int counts[kKeepKeys] = {}, missing[kKeepKeys] = {}, n = 0;
    for (unsigned i = 0; i < g_local.count; ++i) {
        const wire::GearItem& it = g_local.items[i];
        if (it.res < wire::kMaxGearRes) ++counts[it.res];  // the items that name a gear resource (the game's entities, the arms)
        else if (it.kind == wire::GearKind::bb_pod) ++counts[kKeyPod];
        else if (it.kind == wire::GearKind::backpack) ++counts[kKeyPack];
        else if (it.kind == wire::GearKind::odradek) ++counts[kKeyOdra];
    }
    bool in_room = false;
    const bool plain = PlainPlay(&in_room);
    const double now = static_cast<double>(GetTickCount64());
    const gearkeep::RegainEvent ev = gearkeep::RegainTick(g_regain, in_room, plain, now, counts, missing, &n);
    int before = 0;
    for (unsigned k = 0; k < kKeepKeys; ++k) before += g_regain.before[k];
    static unsigned logs = 0;
    if (ev == gearkeep::RegainEvent::none || ++logs > 40) return;
    if (ev == gearkeep::RegainEvent::entered) {
        if (g_regain.have_before)
            Log("[gear] entering the private room: %d gear item(s) Sam wore in plain play before it noted (BB pod %d); checked again %.0f s after "
                "leaving it",
                before, g_regain.before[kKeyPod], gearkeep::kRegainWaitMs / 1000);
        else
            Log("[gear] entering the private room: no plain-play reading of Sam's gear before it (what it takes is not checked this time)");
        return;
    }
    if (!g_regain.have_before) return;
    if (n == 0) {
        Log("[gear] left the private room: every gear item Sam wore before it is back (%d before, BB pod %d)", before, counts[kKeyPod]);
        return;
    }
    char list[640] = {};
    std::size_t len = 0;
    for (unsigned k = 0; k < kKeepKeys && len + 1 < sizeof(list); ++k) {
        if (!missing[k]) continue;
        int w = 0;
        if (k == kKeyPod || k == kKeyPack || k == kKeyOdra) {
            w = std::snprintf(list + len, sizeof(list) - len, "%s%d x %s", len ? "; " : "", missing[k],
                              k == kKeyPod ? "BB pod" : k == kKeyPack ? "backpack" : "Odradek");
        } else {
            const ResEntry& e = g_res_local[k];
            const char* name = e.used ? std::strrchr(e.path, '/') : nullptr;
            w = std::snprintf(list + len, sizeof(list) - len, "%s%d x resource %u '%s' %02x%02x%02x%02x", len ? "; " : "", missing[k], k,
                              name ? name + 1 : e.used ? e.path : "?", e.guid[3], e.guid[2], e.guid[1], e.guid[0]);
        }
        if (w < 0) break;
        len += static_cast<std::size_t>(w) < sizeof(list) - len ? static_cast<std::size_t>(w) : sizeof(list) - len - 1;
    }
    Log("[gear] left the private room: %d gear item(s) Sam wore before it are not back: %s. Sam does not wear them now (taken off in the room: "
        "by the game with the room's clothes, or in the wardrobe), so Fragile does not either, on both screens",
        n, list);
}

void ReadLocalTick(game::Entity* local, bool local_active, double now) {
    if (now < g_next_read_ms) return;
    g_next_read_ms = now + kReadEveryMs;
    if (!local || !local_active || !Checked()) return;
    char why[160] = {};
    Xf sam;
    LARGE_INTEGER t0, t1, qf;
    QueryPerformanceCounter(&t0);
    const bool read_ok = ReadWorld(reinterpret_cast<std::uintptr_t>(local), &sam) && ReadItems(local, false, why, sizeof(why));
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&qf);
    const double us = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(qf.QuadPart);
    g_read_us_sum += us;
    if (us > g_read_us_max) g_read_us_max = us;
    ++g_read_window;
    if (++g_read_timed == 50 || g_read_window >= 600) {
        Log("[gear] read cost (game thread): mean %.0f us, max %.0f us over %u reads; walk cache %s (%llu hits, %llu misses); looks read "
            "(every 500 ms) mean %.0f us, max %.0f us over %u",
            g_read_us_sum / g_read_window, g_read_us_max, g_read_window, g_memo_on ? "on" : "off", static_cast<unsigned long long>(g_memo_hits),
            static_cast<unsigned long long>(g_memo_misses), g_look_window ? g_look_us_sum / g_look_window : 0.0, g_look_us_max, g_look_window);
        g_read_us_sum = 0, g_read_us_max = 0, g_read_window = 0;
        g_look_us_sum = 0, g_look_us_max = 0, g_look_window = 0;
    }
    if (!read_ok) {
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
    static std::uintptr_t fe[wire::kMaxGearItems];
    if (!BuildFrame(local, sam, realbody::SelfShown(), moving, &f, fe)) return;
    const std::uint16_t nh = wire::GearLayoutHash(f);
    const bool new_layout = g_have_local && (nh != wire::GearLayoutHash(g_local) || f.count != g_local.count);
    const bool weapons_only = new_layout && NonWeaponHash(f) == NonWeaponHash(g_local);
    if (new_layout && !weapons_only) {
        if (!(g_pending && g_pending_hash == nh && g_pending_count == f.count)) {
            g_pending = true, g_pending_hash = nh, g_pending_count = f.count;
            if (++g_layout_held <= 30) {
                // what differs (the debounce's churn, logged for the first changes)
                unsigned d = 0;
                while (d < f.count && d < g_local.count && f.items[d].kind == g_local.items[d].kind && f.items[d].res == g_local.items[d].res &&
                       f.items[d].joint == g_local.items[d].joint && f.items[d].parent == g_local.items[d].parent && f.items[d].model == g_local.items[d].model)
                    ++d;
                const wire::GearItem* a = d < g_local.count ? &g_local.items[d] : nullptr;
                const wire::GearItem* b = d < f.count ? &f.items[d] : nullptr;
                Log("[gear] a new layout %04X (%u items, was %04X with %u) held a read: first difference at item %u (was / now): kind %d/%d res %d/%d joint %d/%d parent %d/%d",
                    nh, f.count, wire::GearLayoutHash(g_local), g_local.count, d, a ? static_cast<int>(a->kind) : -1, b ? static_cast<int>(b->kind) : -1,
                    a ? a->res : -1, b ? b->res : -1, a ? a->joint : -1, b ? b->joint : -1, a ? a->parent : -1, b ? b->parent : -1);
            }
            return;
        }
    }
    g_pending = false;
    if (weapons_only || (new_layout && WeaponCount(f) != WeaponCount(g_local))) {
        // a weapon drawn / holstered / switched: the time from the watch's sighting to this frame
        if (g_ww.changed_ms > 0) {
            const double lat = now - g_ww.changed_ms;
            g_ww.lat_sum += lat, ++g_ww.lat_n;
            if (lat > g_ww.lat_max) g_ww.lat_max = lat;
            if (g_ww.lat_n <= 40)
                Log("[gear] weapon layout %04X taken %.0f ms after the watch saw the change (%u weapon item(s); mean %.0f ms, max %.0f ms over %u)", nh, lat,
                    WeaponCount(f), g_ww.lat_sum / g_ww.lat_n, g_ww.lat_max, g_ww.lat_n);
            g_ww.changed_ms = 0;
        }
    }
    if (moving && g_have_local && wire::GearLayoutHash(f) == wire::GearLayoutHash(g_local)) ++g_moving_skips;
    if (new_layout && !weapons_only) NameLayoutChange(g_local, f, g_res_local, "our gear");
    g_local = f;
    g_have_local = true;
    std::memcpy(g_local_e, fe, sizeof(g_local_e));
    g_local_player = reinterpret_cast<std::uintptr_t>(local);
    ReadLooks(now);
    ReadBackpackPose();
    ReadHeldPose();
    const std::uint16_t h = wire::GearLayoutHash(f);
    if (h != g_logged_hash) {
        g_logged_hash = h;
        DescribeFrame(f, "our gear", g_res_local);
    }
    RoomRegainTick();
}

// ---------------------------------------------------------------- self view headgear (docs/real-partner-body.md section 20)
// Our Sam's head accessories (ds/entities/player/ds_player_inventory: eyewear0, glasses00-02, cap00-03; each an
// Entity whose AttachToHelperMover hangs it on HLP_Head) get the offset realbody::SelfHeadItemOffset asks for, as the
// translation of the mover's own local matrix (+0x60, a Mat44: rows x, y, z, then the translation at +0x90; identity
// from its constructor 0x14239B930). The mover's update (0x1423A7450) combines it with the helper's world transform
// every frame when +0xA0 is 0 (0x1423A7773: cmp byte [rsi+0xA0],bl; jnz; lea r8,[rsi+0x60]; ...; call 0x14195DF70,
// which multiplies the local by the helper's rotation: the translation is in the helper's frame). Written from the
// game thread only when it differs from what is there (show, hide, tuning); an item whose matrix is not the identity
// or ours is left alone. The sender then measures the item on Sam's head joint with the offset in it, so the other
// screen places it the same way on her body's head (no wire change).
struct HeadItemRes {
    const char* guid;
    int cls;
    const char* name;
};
constexpr HeadItemRes kHeadItemRes[] = {
    {"3b9eb11e-cdfb-0146-b4e7-bf6ad3926f01", realbody::kHeadItemGlasses, "eyewear0"},
    {"d2306d68-4458-c04c-9566-2463d7b93578", realbody::kHeadItemGlasses, "glasses00"},
    {"036f3017-bb5e-844f-8939-6790c89c4067", realbody::kHeadItemGlasses, "glasses01"},
    {"6482902d-bc58-4d4d-b5b5-279e919364ba", realbody::kHeadItemGlasses, "glasses02"},
    {"c83ac923-bcb3-5e4e-b062-8b9789e6a81f", realbody::kHeadItemCap, "cap00"},
    {"17de6d02-2c8d-ac4b-ac9f-46672eaa5f6e", realbody::kHeadItemCap, "cap01"},
    {"dbd027af-9b73-ca4f-bca1-3d708ee2f8c8", realbody::kHeadItemCap, "cap02"},
    {"6247acf7-74d5-a846-8e29-6459ef4b1f71", realbody::kHeadItemCap, "cap03"},
};
// the row index is the item index realbody's per-item offsets use (section 26.3)
static_assert(sizeof(kHeadItemRes) / sizeof(kHeadItemRes[0]) == realbody::kHeadItems, "one offset per head item");
static_assert(kHeadItemRes[realbody::kHeadItemFirstCap - 1].cls == realbody::kHeadItemGlasses &&
                  kHeadItemRes[realbody::kHeadItemFirstCap].cls == realbody::kHeadItemCap,
              "glasses first, then the caps");
constexpr std::uintptr_t kMoverUseLocal = 0x23A7773;  // in the mover update: the +0xA0 test and the +0x60 local
constexpr std::uintptr_t kEntMover = 0xB0, kMoverEntity = 0x48, kMoverLocal = 0x60, kMoverLocalT = 0x90, kMoverNoLocal = 0xA0, kMoverHelper = 0xB0;
struct HeadItem {
    std::uintptr_t e = 0, e_vt = 0, mover = 0, mover_vt = 0;
    int cls = -1, item = -1;  // class (cap / glasses) and item (kHeadItemRes row)
    const char* name = "";
    double seen_ms = -1e9;  // the last read that saw it shown
    bool ours = false;      // our offset is in its mover (put back to 0 when no longer wanted)
    float written[3] = {};
    bool refused = false;   // its matrix was neither the identity nor ours: left alone
    std::uint8_t orig_flag = 1;  // the game's +0xA0 (skip the local matrix) before ours
};
HeadItem g_hi[8];
struct HeadgearState {
    bool checked = false, ok = false;
    std::uint32_t writes = 0, restores = 0, game_changed = 0, refusals = 0, bad = 0;
    unsigned logs = 0;
} g_hg;
bool g_hg_on = true;  // `gear 18 <0/1>`: the mover writes (realbody still decides the offsets)
// Any other shown entity of ours hung on HLP_Head (an accessory not in kHeadItemRes, or a listed one whose mover is not
// the expected kind) keeps the v0.11 head lift for everything: it gets no offset of its own. Checked once per entity.
struct HeadProbe {
    std::uintptr_t e = 0, vt = 0;
    bool head = false;
};
HeadProbe g_hprobe[64];
unsigned g_hprobe_rr = 0, g_unknown_logs = 0;
double g_unknown_head_ms = -1e9;  // the last read that saw such an item shown

bool OnHeadHelper(std::uintptr_t e, std::uintptr_t* mover_out, char (&helper)[40]) {
    helper[0] = 0;
    const std::uintptr_t mover = Q(e + kEntMover);
    if (mover_out) *mover_out = mover;
    if (mover && Derives(mover, "AttachToHelperMover") && Q(mover + kMoverEntity) == e) ReadString(Q(mover + kMoverHelper), helper, sizeof(helper));
    return mover && helper[0] && std::strcmp(helper, "HLP_Head") == 0;
}

void NoteUnknownHeadItem(std::uintptr_t e, double now) {
    const std::uintptr_t vt = Q(e);
    HeadProbe* p = nullptr;
    for (HeadProbe& x : g_hprobe)
        if (x.e == e && x.vt == vt) p = &x;
    if (!p) {
        p = &g_hprobe[g_hprobe_rr++ % 64];
        char helper[40];
        p->e = e, p->vt = vt, p->head = OnHeadHelper(e, nullptr, helper);
        if (p->head && g_unknown_logs++ < 20)
            Log("[headgear] our entity %p %s hangs on HLP_Head but has no offset of its own: the head lift stays while it is shown", reinterpret_cast<void*>(e),
                TypeOf(e));
    }
    if (p->head) g_unknown_head_ms = now;
}

// The kHeadItemRes row (the item index) of resource `res_id`, -1 when it is none of them.
int HeadItemOf(int res_id, const char** name) {
    if (res_id < 0 || res_id >= static_cast<int>(wire::kMaxGearRes) || !g_res_local[res_id].used) return -1;
    char g[37];
    wire::GearGuidText(g_res_local[res_id].guid, g);
    for (int i = 0; i < realbody::kHeadItems; ++i)
        if (std::strcmp(g, kHeadItemRes[i].guid) == 0) return (*name = kHeadItemRes[i].name), i;
    return -1;
}

// A mover's local matrix is ours: the flag cleared and a rigid matrix (rows orthonormal, column 3 zero, 1 at [3][3])
// whose translation is within 1.6 m of the offset we gave it (Sam's update writes the offset corrected onto her drawn
// head every frame, section 26.8: at most 1.5 m off); the identity: the game's own.
bool RigidLocal(const float (&m)[4][4]) {
    if (std::fabs(m[3][3] - 1.0f) > 1e-4f) return false;
    for (int r = 0; r < 3; ++r) {
        if (std::fabs(m[r][3]) > 1e-4f) return false;
        for (int c = 0; c < 3; ++c) {
            const float d = m[r][0] * m[c][0] + m[r][1] * m[c][1] + m[r][2] * m[c][2];
            if (std::fabs(d - (r == c ? 1.0f : 0.0f)) > 2e-3f) return false;
        }
    }
    return true;
}
bool IdentityRot(const float (&m)[4][4]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::fabs(m[r][c] - (r == c ? 1.0f : 0.0f)) >= 1e-5f) return false;
    return std::fabs(m[3][3] - 1.0f) < 1e-5f;
}
bool NearOurs(const float (&m)[4][4], const float (&written)[3]) {
    const double d = std::sqrt(static_cast<double>(m[3][0] - written[0]) * (m[3][0] - written[0]) + static_cast<double>(m[3][1] - written[1]) * (m[3][1] - written[1]) +
                               static_cast<double>(m[3][2] - written[2]) * (m[3][2] - written[2]));
    return RigidLocal(m) && d <= 1.6;
}
// The identity (rotation rows and zero translation, 1 at [3][3]) into a mover's local matrix.
bool WriteIdentity(std::uintptr_t mover) {
    const float id[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    SIZE_T wrote = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(mover + kMoverLocal), id, sizeof(id), &wrote) && wrote == sizeof(id);
}

// The movers Sam's update corrects (section 26.8): every slot that is ours now. Before a slot's mover is put back, it
// leaves this set (PublishHeadMovers then waits until no Sam update can still write into it).
void PublishOurMovers(const HeadItem* except = nullptr) {
    realbody::HeadMoverPub pub[realbody::kMaxHeadMovers];
    unsigned n = 0;
    for (const HeadItem& h : g_hi)
        if (h.e && h.ours && &h != except && n < realbody::kMaxHeadMovers) {
            realbody::HeadMoverPub& p = pub[n++];
            p.e = h.e, p.e_vt = h.e_vt, p.mover = h.mover, p.mover_vt = h.mover_vt, p.item = h.item;
            std::memcpy(p.off, h.written, sizeof(p.off));
        }
    realbody::PublishHeadMovers(pub, n);
}

// Our offset out of a slot's mover before the slot is used for another item: only when the entity and mover still check
// (fault-free reads) and the mover still holds our write (flag 0, a rigid matrix near our offset); a fresh mover is the
// game's and is left alone. Out of the corrected set first, then the flag back, then the identity (as HeadgearTick's
// restore).
void RestoreHeadMover(HeadItem& h) {
    if (!h.ours) return;
    PublishOurMovers(&h);
    h.ours = false;
    std::uint8_t no_local = 1;
    float m[4][4];
    if (Q(h.e) != h.e_vt || Q(h.e + kEntMover) != h.mover || Q(h.mover) != h.mover_vt || Q(h.mover + kMoverEntity) != h.e ||
        !Rd(h.mover + kMoverNoLocal, no_local) || !realbody::ReadHeadMoverLocal(h.mover + kMoverLocal, m) || no_local != 0 || !NearOurs(m, h.written))
        return;
    SIZE_T wrote_f = 0;
    if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(h.mover + kMoverNoLocal), &h.orig_flag, 1, &wrote_f) && wrote_f == 1 &&
        WriteIdentity(h.mover))
        ++g_hg.restores;
    else
        ++g_hg.bad;
}

// The entity walk saw a shown entity of resource `res_id` (game thread).
void NoteHeadItem(std::uintptr_t e, int res_id, double now) {
    const char* name = "";
    const int item = HeadItemOf(res_id, &name);
    if (item < 0) {
        NoteUnknownHeadItem(e, now);
        return;
    }
    HeadItem* h = nullptr;
    for (HeadItem& x : g_hi)
        if (x.e == e) h = &x;
    if (!h)
        for (HeadItem& x : g_hi)
            if (!x.e) {
                h = &x;
                break;
            }
    if (!h) {
        g_unknown_head_ms = now;  // more head items than slots: the lift stays
        return;
    }
    if (h->e == e && h->item != item) {
        // Another accessory at the freed address of the one before (a swap: same class, same size): the slot is set up
        // again for the new item (its own offset, section 26.3), our offset out of the old mover first.
        if (g_hg.logs++ < 40)
            Log("[headgear] our %p is now %s (was %s, same address): set up again", reinterpret_cast<void*>(e), name, h->name);
        RestoreHeadMover(*h);
        h->e = 0;
    }
    if (h->e != e) {
        *h = HeadItem{};
        std::uintptr_t mover = 0;
        char helper[40];
        const bool ok = OnHeadHelper(e, &mover, helper);
        if (g_hg.logs++ < 40)
            Log("[headgear] our %s %p (res %d): mover %p %s helper '%s'%s", name, reinterpret_cast<void*>(e), res_id, reinterpret_cast<void*>(mover),
                mover ? TypeOf(mover) : "-", helper, ok ? "" : ": not used (not an AttachToHelperMover on HLP_Head: not on the head)");
        if (!ok) return;
        h->e = e, h->e_vt = Q(e), h->mover = mover, h->mover_vt = Q(mover), h->cls = kHeadItemRes[item].cls, h->item = item, h->name = name;
    }
    h->seen_ms = now;
}

bool HeadgearChecked() {
    if (g_hg.checked) return g_hg.ok;
    g_hg.checked = true;
    g_hg.ok = mem::CodeMatches(mem::Base() + kMoverUseLocal, "38 9E A0 00 00 00 75 4A 4C 8D 46 60 48 8D 54 24 20 48 8D 4C 24 60 E8 E2 67 5B FF");
    Log("[headgear] the accessory mover's local-matrix site %s", g_hg.ok ? "verified" : "does not match: no headgear offsets");
    return g_hg.ok;
}

// ---- Her hat in the rain (docs/real-partner-body.md section 26.8). The game hides Sam's worn hat while his hood is up
// (timefall): the inventory update (0x1427EFE76) sets each hat entity invisible (0x14235F640) when the hood state
// (inventory +0x4970) is up: cap00 (type 0x0F, entity +0x4E18) for states 6-11 (5 and 12 by the hood's progress
// +0x4974), the Santa hat (0x24, +0x4E20), the headcrab (0x31, +0x4E28) and the hair ornament (0x32, +0x4E30) for 5 and
// over. Sam's hood covers his head; her character has no hood, so she ended up bare-headed (the user's play tests 3 and
// 4). While our character has no hood (fragile, fragile_suit) a worn hat that is hidden with the hood up is treated as
// shown: its slot keeps its offset, the sender sends it (as the constant pose on joint 31: the other screen draws it on
// her), and the self view draws it as a prop on her body (the self slot, placed in her body's own update). Nothing is
// written into the game's state; the real hat stays hidden. `gear 22 <0/1>` (default on).
struct HoodHat {
    std::uintptr_t e = 0, vt = 0;  // the hidden hat now (0: none)
    int res = -1, item = -1;
    const char* name = "";
    std::int32_t hood = 0;
    std::uintptr_t memo_e = 0, memo_vt = 0;  // the resource of the last hat looked at
    int memo_res = -1;
    unsigned logs = 0;
} g_hood;
bool g_hood_on = true;        // `gear 22 <0/1>`
bool g_own_hoodless = false;  // Init: the character both screens show for us has no hood
int g_hood_sites = -1;        // -1 not checked, 0 mismatch (off), 1 ok
constexpr std::uintptr_t kInvHood = 0x4970, kInvAccList = 0x37F9, kAccStride = 0x40, kAccWorn = 0x1D;
struct HatSlot {
    std::uint8_t type;
    std::uintptr_t ent;  // the inventory's entity pointer for that hat
};
constexpr HatSlot kHats[4] = {{0x0F, 0x4E18}, {0x24, 0x4E20}, {0x31, 0x4E28}, {0x32, 0x4E30}};

// ---- Round 11 (docs/real-partner-body.md section 37): Sam's rain hood, read for her hood on the exo carrier. The inventory's
// hood state machine (0x1428125A0, every frame): the state +0x4970 (0..12; exo_parts.hpp HoodPhaseOf), the type +0x4A88 (0
// porter, 1 oxi, 2 sea otter: 0x142791EE0), the opening clip's index +0x4A8C (4: the porter hood's second clip set).
// Byte-checked once; on a mismatch nothing is read: her hood is not drawn and her hat stays on her in the rain (26.8).
int g_hood_mode = 2;      // session.ini fragile_hood / `gear 31 <0/1/2>`: 0 off, 1 the timed swap, 2 the unfold where the carrier can play it
bool g_hood_hair = true;  // session.ini hood_hair / `gear 33 <0/1>`: her hair hidden while her hood is drawn
int g_hood_force_state = -1, g_hood_force_type = 2;  // `gear 32 <state> [type]` (hidden tests; -1 off): our reading replaced
int g_hood_read_sites = -1;                           // -1 not checked, 0 mismatch (no reading), 1 ok
constexpr std::uintptr_t kInvHoodType = 0x4A88, kInvHoodClipOpen = 0x4A8C;
bool HoodReadSites() {
    if (g_hood_read_sites < 0) {
        const std::uintptr_t b = mem::Base();
        const bool ok = mem::CodeMatches(b + 0x2812C5E, "8B 86 70 49 00 00") &&                         // the state machine reads +0x4970
                        mem::CodeMatches(b + 0x2813002, "C7 86 70 49 00 00 06 00 00 00") &&             // ... up: 6
                        mem::CodeMatches(b + 0x28132BE, "C7 86 70 49 00 00 0C 00 00 00") &&             // ... lowering: 12
                        mem::CodeMatches(b + 0x2791F1A, "38 91 88 4A 00 00") &&                         // the type +0x4A88
                        mem::CodeMatches(b + 0x27924B7, "C6 86 89 4A 00 00 02") &&                      // the porter's slot 2
                        mem::CodeMatches(b + 0x28134EE, "48 63 96 8C 4A 00 00 41 B0 01 48 8B 46 30 48 8B 88 98 03 00");  // the opening clip +0x4A8C
        g_hood_read_sites = ok ? 1 : 0;
        Log("[hood] Sam's hood state sites %s", ok ? "verified: his rain hood is read for her hood" : "do not match this game build: her hood is not drawn");
    }
    return g_hood_read_sites == 1;
}
void ReadHood(std::uintptr_t inv) {
    HoodRead& h = g_hoodr;
    std::int32_t st = 0, clip = 0;
    std::uint8_t ty = 0;
    h.valid = HoodReadSites() && Rd(inv + kInvHood, st) && Rd(inv + kInvHoodType, ty) && Rd(inv + kInvHoodClipOpen, clip) && st >= 0 &&
              st <= wire::kGearHoodStateMax && ty <= wire::kGearHoodTypeMax;
    if (g_hood_force_state >= 0) {
        h.valid = true;
        st = g_hood_force_state;
        ty = static_cast<std::uint8_t>(g_hood_force_type);
        clip = 0;
    }
    h.state = h.valid ? st : 0;
    h.type = h.valid ? ty : -1;
    h.alt = h.valid && ty == 0 && clip == 4;
    if ((h.state != h.logged_state || h.type != h.logged_type) && h.logs < 400) {
        ++h.logs;
        if (h.valid)
            Log("[hood] our Sam's hood: %s, %s (state %d)%s", exo::HoodTypeName(h.type), exo::HoodPhaseName(exo::HoodPhaseOf(h.state)), h.state,
                g_hood_force_state >= 0 ? " (gear 32)" : "");
        else
            Log("[hood] our Sam's hood: not read");
        h.logged_state = h.state, h.logged_type = h.type;
    }
}
bool OwnHoodDrawnUp();  // (below, with the carrier)

bool HoodSites() {
    if (g_hood_sites >= 0) return g_hood_sites == 1;
    const std::uintptr_t b = mem::Base();
    struct Site {
        std::uintptr_t rva;
        const char* sig;
    };
    static const Site kSites[] = {
        {0x27F0FD9, "48 8D BE 16 38 00 00"},                    // the accessory list (type at -0x1D, worn at +0)
        {0x27F14B6, "80 78 E3 0F 75 19 44 38 20"},              // cap00's loop: type 0x0F, worn
        {0x27F14E4, "0F B6 8E 80 4C 00 00 8B 96 70 49 00 00"},  // cap00: the hood state +0x4970
        {0x27F156B, "48 8B 8E 18 4E 00 00 41 0F B6 D6"},        // cap00's entity +0x4E18 -> its visibility
        {0x27F15C0, "80 78 E3 24 75 1D"},                       // the Santa hat (0x24)
        {0x27F15FD, "83 BE 70 49 00 00 05"},                    // hidden with the hood state 5 and over
        {0x27F161F, "48 8B 8E 20 4E 00 00 41 0F B6 D4"},        // its entity +0x4E20
        {0x27F1674, "80 78 E3 31 75 1D"},                       // the headcrab (0x31)
        {0x27F16AD, "44 8B 86 70 49 00 00"},                    // the hood state again
        {0x27F16D2, "80 78 E3 32 75 1D"},                       // the hair ornament (0x32)
        {0x27F17B5, "48 8B 8E 28 4E 00 00 41 0F B6 D7"},        // the headcrab's entity +0x4E28
        {0x27F1819, "48 8B 8E 30 4E 00 00 40 0F B6 D7"},        // the hair ornament's entity +0x4E30
        {0x235F640, "40 53 48 83 EC 40"},                       // the visibility setter (+0x88 bit 1), not called by us
    };
    bool ok = true;
    for (const Site& x : kSites)
        if (!mem::CodeMatches(b + x.rva, x.sig)) {
            Log("[headgear] her hat in the rain off: the inventory's hat site ds.exe+0x%llX does not match this game build", static_cast<unsigned long long>(x.rva));
            ok = false;
            break;
        }
    g_hood_sites = ok ? 1 : 0;
    if (ok) Log("[headgear] her hat in the rain: the inventory's hat and hood sites verified (%s)", g_own_hoodless ? "our character has no hood: on" : "our character has a hood: not used");
    return ok;
}

// v0.23: the carrier's hide reasons (read only; the game writes them): byte-checked once; off = every piece counts as shown.
bool CargoHideSites() {
    static int ok = -1;
    if (ok >= 0) return ok == 1;
    const std::uintptr_t b = mem::Base();
    const bool m = mem::CodeMatches(b + 0x2CB27A0, "8B 81 8C 00 00 00 4C 8B C9 85 C0") &&                    // the hide-reason mask +0x8C
                   mem::CodeMatches(b + 0x2CB2620, "80 61 75 BF 80 F2 01 C0 E2 06 08 51 75 C3") &&           // the back slot's +0x75 bit 6
                   mem::CodeMatches(b + 0x27F2A9A, "0F B6 54 24 31 45 33 C0 4C 8B E0 48 89 45 D0 48 8B 48 50 E8 EE FC 4B 00");  // its reason 0 call
    ok = m ? 1 : 0;
    Log("[gear] the game's cargo hides %s", m ? "verified (carrier +0x8C, back slot +0x75): hidden cargo is not sent"
                                              : "do not match this game build: every carried piece is sent as shown (v0.22)");
    return m;
}

// Section 28 (round 6): the backpack's rain cover wears out. Its state is a data block of the inventory's backpack object
// (inventory +0x4CB8, block at +0x3E0, as the HUD's read 0x1427D9F8E takes it; 0x21D8 bytes, read by 0x1429234B0 and
// committed through a second copy at +0x21D8 by 0x142937010): +0x21C8 byte worn, +0x21CC level (0..2), +0x21D0 f32
// durability, out of DSBackpackConfig's RainCoverDurability / Lv2 / Lv3 (+0x20 / +0x24 / +0x28; the config at settings
// +0x5D6C0). The timefall update (0x142BE8E70) takes the rain's damage off it and at 0 clears the worn byte: the game
// removes the cover from Sam, our gear frames lose its entity and it goes from Fragile on both screens too (the game's
// state, mirrored: not a co-op fault). Read only, every second, on the game thread; logged at first sight, at every step
// (gearwear::Bucket) and when it breaks, so the log says in one line why it went.
constexpr std::uintptr_t kInvPackObj = 0x4CB8, kPData = 0x3E0, kPDataSize = 0x21D8, kCoverWorn = 0x21C8, kCoverLevel = 0x21CC, kCoverDur = 0x21D0;
struct CoverWear {
    int sites = -1;               // -1 not checked, 0 mismatch (off), 1 ok
    std::uintptr_t settings = 0;  // the settings global's address (its value: the settings object)
    std::uintptr_t player = 0;    // the block seen last (another one, a load: seen afresh)
    int worn = -1, level = -1, bucket = -1;
    double pc = 0;
    double next_ms = 0;
    unsigned logs = 0;
    bool insane_logged = false;
    int last_worn = -1;      // round 11: the last worn byte read (-1 none yet, or none since the last load)
    bool ever_worn = false;  // ... worn at any read since the last load (another cover block)
    double worn_pc = 0;      // the durability (percent) at the last read since then that had it worn
} g_cover;

gearwear::CoverGone OurCoverGone(double* pc) {
    if (pc) *pc = g_cover.worn_pc;
    return gearwear::WhyCoverGone(g_cover.last_worn, g_cover.ever_worn, g_cover.worn_pc);
}

bool CoverSites() {
    if (g_cover.sites < 0) {
        const std::uintptr_t b = mem::Base();
        const bool m = mem::CodeMatches(b + 0x29268E0, "0F B6 81 C8 21 00 00 C3") &&                       // worn: byte +0x21C8
                       mem::CodeMatches(b + 0x27D9F8E, "48 8B 88 C8 54 00 00 48 8B 81 B8 4C 00 00 48 85 C0 48 8D 88 E0 03 00 00") &&  // inv +0x4CB8 +0x3E0
                       mem::CodeMatches(b + 0x2BE90CD, "0F B6 85 78 21 00 00 C5 FA 10 85 80 21 00 00") &&  // worn, durability
                       mem::CodeMatches(b + 0x2BE90E4, "C4 C1 7A 5C C0 C5 FA 5F CF C5 F8 2F F9 0F B6 C0 0F 43 C6 88 85 78 21 00 00") &&  // at 0: worn cleared
                       mem::CodeMatches(b + 0x2BE90B5, "C5 FA 10 71 20") &&                                // level 0's maximum: config +0x20
                       mem::CodeMatches(b + 0x29235C1, "48 8B 05 ?? ?? ?? ?? 48 8B 88 C0 D6 05 00") &&     // settings -> DSBackpackConfig
                       mem::CodeMatches(b + 0x2937010, "40 53 48 83 EC 20 48 8B D9 41 B8 D8 21 00 00");    // the commit (second copy)
        g_cover.settings = m ? mem::RipTarget(b + 0x29235C1, 3, 7) : 0;
        g_cover.sites = m && g_cover.settings ? 1 : 0;
        Log("[gear] wear: the rain cover's durability sites %s", g_cover.sites ? "verified (its wear is logged in steps)"
                                                                               : "do not match this game build: the rain cover's wear is not logged");
    }
    return g_cover.sites == 1;
}

// Our Sam's cover data block (inventory +0x4CB8 +0x3E0), or 0.
std::uintptr_t CoverBlock(std::uintptr_t inv) {
    const std::uintptr_t o = inv && CoverSites() ? Q(inv + kInvPackObj) : 0;
    return o ? o + kPData : 0;
}
// The cover's state off our Sam's inventory: worn, level, durability and its maximum. False when unreadable or not sane.
bool ReadCover(std::uintptr_t inv, std::uint8_t* worn, std::int32_t* level, float* dur, float* max) {
    const std::uintptr_t blk = CoverBlock(inv);
    if (!blk) return false;
    const std::uintptr_t cfg = Q(Q(g_cover.settings) + 0x5D6C0);
    if (!cfg || !Rd(blk + kCoverWorn, *worn) || !Rd(blk + kCoverLevel, *level) || !Rd(blk + kCoverDur, *dur)) return false;
    if (*worn > 1 || *level < 0 || *level > 2 || !Rd(cfg + 0x20 + 4u * static_cast<unsigned>(*level), *max)) return false;
    return *max > 0 && *max < 1e9f && *dur == *dur && *dur >= 0 && *dur <= *max * 1.01f;
}

void ReadRainCover(std::uintptr_t inv, double now) {
    if (now < g_cover.next_ms) return;
    g_cover.next_ms = now + 1000;
    std::uint8_t worn = 0;
    std::int32_t level = 0;
    float dur = 0, max = 0;
    if (!ReadCover(inv, &worn, &level, &dur, &max)) {
        if (g_cover.sites == 1 && !g_cover.insane_logged) {
            g_cover.insane_logged = true;
            Log("[gear] wear: the rain cover's state does not read as expected on our Sam (worn %u level %d durability %.1f of %.1f): not logged", worn,
                level, dur, max);
        }
        return;
    }
    if (CoverBlock(inv) != g_cover.player) {  // another block (a load): seen afresh
        g_cover.player = CoverBlock(inv);
        g_cover.worn = -1, g_cover.level = -1, g_cover.bucket = -1;
        // the wording of a cover entity's removal looks at this load only (a cover worn out before it is not this one)
        g_cover.last_worn = -1, g_cover.ever_worn = false, g_cover.worn_pc = 0;
    }
    const double pc = gearwear::PercentOf(dur, max);
    const int bk = gearwear::Bucket(pc);
    if (g_cover.worn == 1 && worn == 0) {
        if (++g_cover.logs <= 200)
            Log("[gear] wear: the backpack's rain cover %s (durability %.2f %% before): the game removes it from Sam, and so from Fragile on both screens",
                g_cover.pc <= 1.0 ? "broke: worn out in timefall" : "was taken off", g_cover.pc);
    } else if (worn && (g_cover.worn != 1 || bk != g_cover.bucket || level != g_cover.level)) {
        if (++g_cover.logs <= 200)
            Log("[gear] wear: the backpack's rain cover (level %d): durability %.2f %% (%.0f of %.0f)%s", level + 1, pc, dur, max,
                bk == 0 ? ": nearly worn out, in timefall the game removes it soon (from Sam, and so from Fragile)" : "");
    } else if (!worn && g_cover.worn < 0) {
        if (++g_cover.logs <= 200) Log("[gear] wear: no rain cover on our backpack");
    }
    g_cover.worn = worn, g_cover.level = level, g_cover.bucket = bk, g_cover.pc = pc;
    g_cover.last_worn = worn;
    if (worn) g_cover.ever_worn = true, g_cover.worn_pc = pc;
}

// Every read (100 ms, game thread): the worn hat hidden by the hood, if any, noted as a shown head item and added to the
// frame. `sam`: Sam's world.
void ReadHoodHat(std::uintptr_t p, std::uintptr_t inv, const Xf& sam, double now) {
    std::uintptr_t found = 0, found_vt = 0;
    int found_res = -1, found_item = -1;
    const char* found_name = "";
    std::int32_t hood = 0;
    const bool sites = g_own_hoodless && HoodSites() && Rd(inv + kInvHood, hood);
    const bool own_hood = sites && OwnHoodDrawnUp();  // her own hood is drawn up over it (section 37): the hat hides under it
    if (g_hood_on && sites && hood >= 5 && hood <= 12 && !own_hood) {
        for (const HatSlot& hs : kHats) {
            bool worn = false;
            for (unsigned i = 0; i < 64 && !worn; ++i) {
                std::uint8_t type = 0, w = 0;
                worn = Rd(inv + kInvAccList + kAccStride * i, type) && type == hs.type && Rd(inv + kInvAccList + kAccStride * i + kAccWorn, w) && w != 0;
            }
            if (!worn) continue;
            const std::uintptr_t e = Q(inv + hs.ent);
            if (!e || !Derives(e, "Entity") || Q(e + 0x70) != p || Visible(e)) continue;
            int id = -1;
            if (g_hood.memo_e == e && g_hood.memo_vt == Q(e)) {
                id = g_hood.memo_res;
            } else {
                char path[wire::kMaxGearResPath + 8] = {};
                std::uint8_t guid[16] = {};
                const char* rt = nullptr;
                if (realbody::ResourceIdentity(reinterpret_cast<game::Entity*>(e), path, sizeof(path), guid, &rt) && wire::GearResPathOk(path))
                    id = LocalResId(path, guid);
                g_hood.memo_e = e, g_hood.memo_vt = Q(e), g_hood.memo_res = id;
            }
            const char* name = "";
            const int item = HeadItemOf(id, &name);
            if (item < realbody::kHeadItemFirstCap) continue;
            found = e, found_vt = Q(e), found_res = id, found_item = item, found_name = name;
            break;
        }
    }
    if (found != g_hood.e && g_hood.logs++ < 40) {
        if (found)
            Log("[headgear] Sam's hood is up (state %d): the game hides his worn %s %p; her character has no hood, so it stays on her (a prop on her "
                "head on both screens)", hood, found_name, reinterpret_cast<void*>(found));
        else if (g_hood.e && own_hood)
            Log("[headgear] her hat in the rain: %s hides under her own hood now (hood state %d)", g_hood.name, hood);
        else if (g_hood.e)
            Log("[headgear] her hat in the rain: %s no longer hidden by the hood (hood state %d)", g_hood.name, hood);
    }
    g_hood.e = found, g_hood.vt = found_vt, g_hood.res = found_res, g_hood.item = found_item, g_hood.name = found_name, g_hood.hood = hood;
    if (!found || g_item_count >= kMaxLocal) return;
    NoteHeadItem(found, found_res, now);  // its slot keeps its offset (no flash at its normal place when the hood goes down)
    LocalItem& it = g_items[g_item_count++];
    it = LocalItem{};
    it.kind = wire::GearKind::entity;
    it.model = wire::kGearNone;
    it.variation = wire::kGearNone;
    it.slot_type = 0;
    it.e = found;
    it.visible = true;
    it.world = sam;  // not read: the sender sends the constant pose on joint 31 (HeadConst)
    it.res = static_cast<std::uint8_t>(found_res);
    it.parent_e = p;
    it.hint_joint = 0xFF;
}

// The constant pose on joint 31 of a head item the sender sends so (section 26.8): one we offset (its offset), or the
// hat hidden by the hood (its offset when we offset it, else none: Sam's own head).
bool HeadConst(std::uintptr_t e, float (&p)[3], float (&r)[3][3], const char** name) {
    if (!e) return false;
    for (const HeadItem& h : g_hi)
        if (h.e == e && h.ours) {
            realbody::HeadItemOnJoint(h.written, p, r);
            *name = h.name;
            return true;
        }
    if (e != g_hood.e) return false;
    const float zero[3] = {0, 0, 0};
    realbody::HeadItemOnJoint(zero, p, r);
    *name = g_hood.name;
    return true;
}

// Every tick (game thread): each accessory's mover = the offset wanted now (identity when none); Sam's own update then
// corrects every mover that is ours onto her drawn head each frame (section 26.8, realbody::PublishHeadMovers). `stop`
// (the emergency stop): every mover of ours goes back to the identity and the game's flag, and the lift is told so.
void HeadgearTick(double now, bool stop = false) {
    // Ready: the offsets can be written into every shown head item of ours; else none is written and the lift holds
    // them all up as in v0.11 (realbody::EffectiveLiftPct).
    bool refused_any = false;
    for (const HeadItem& h : g_hi) refused_any |= h.e && h.refused && now - h.seen_ms < 600;
    const bool ready = !stop && g_hg_on && realbody::HeadgearWanted() && HeadgearChecked() && !refused_any && now - g_unknown_head_ms >= 600;
    realbody::SetHeadgearReady(ready);
    // What each slot wants now; a slot that stops being ours (or changes its offset) leaves the corrected set before its
    // mover is touched below.
    float want_all[8][3] = {};
    bool on_all[8] = {}, leaving = false;
    for (unsigned i = 0; i < 8; ++i) {
        HeadItem& h = g_hi[i];
        if (!h.e) continue;
        const bool shown = now - h.seen_ms < 600;
        on_all[i] = ready && shown && realbody::SelfHeadItemOffset(h.item, want_all[i]);
        leaving = leaving || (h.ours && (!on_all[i] || std::memcmp(want_all[i], h.written, sizeof(h.written)) != 0));
    }
    if (leaving) {
        realbody::HeadMoverPub pub[realbody::kMaxHeadMovers];
        unsigned n = 0;
        for (unsigned i = 0; i < 8; ++i) {
            const HeadItem& h = g_hi[i];
            if (!h.e || !h.ours || !on_all[i] || std::memcmp(want_all[i], h.written, sizeof(h.written)) != 0 || n >= realbody::kMaxHeadMovers) continue;
            realbody::HeadMoverPub& p = pub[n++];
            p.e = h.e, p.e_vt = h.e_vt, p.mover = h.mover, p.mover_vt = h.mover_vt, p.item = h.item;
            std::memcpy(p.off, h.written, sizeof(p.off));
        }
        realbody::PublishHeadMovers(pub, n);
    }
    for (unsigned i = 0; i < 8; ++i) {
        HeadItem& h = g_hi[i];
        if (!h.e) continue;
        const bool shown = now - h.seen_ms < 600;
        float want[3] = {want_all[i][0], want_all[i][1], want_all[i][2]};
        const bool on = on_all[i];
        if (!on && !h.ours) {
            if (!shown) h = HeadItem{};  // gone and nothing of ours in it
            continue;
        }
        if (!on) want[0] = want[1] = want[2] = 0;
        // still the same entity and mover (fault-free reads)
        std::uint8_t no_local = 1;
        if (Q(h.e) != h.e_vt || Q(h.e + kEntMover) != h.mover || Q(h.mover) != h.mover_vt || Q(h.mover + kMoverEntity) != h.e ||
            !Rd(h.mover + kMoverNoLocal, no_local)) {
            ++g_hg.bad;
            h = HeadItem{};
            continue;
        }
        // Read under the lock Sam's update writes an owned mover with (HeadCorrect, every frame): an unlocked read could
        // see half of one frame's corrected matrix and half of the next and refuse our own item for the session.
        float m[4][4];
        if (!realbody::ReadHeadMoverLocal(h.mover + kMoverLocal, m)) {
            if (!h.ours) ++g_hg.bad;  // ours: Sam's update held the lock; judged next tick
            continue;
        }
        // +0xA0 set: the update skips the local matrix (the game's movers of these accessories have it set, their
        // local being the identity); ours goes in with it cleared, and the game's value comes back with the identity.
        const bool is_zero = IdentityRot(m) && m[3][0] == 0 && m[3][1] == 0 && m[3][2] == 0 && (!h.ours || no_local == h.orig_flag);
        const bool is_ours = h.ours && no_local == 0 && NearOurs(m, h.written);
        if ((!is_zero && !is_ours) || no_local > 1) {
            if (!h.refused) {
                h.refused = true;
                ++g_hg.refusals;
                Log("[headgear] our %s %p: its mover's local matrix is not the identity or ours (translation %.3f %.3f %.3f, +0xA0 %u): left alone", h.name,
                    reinterpret_cast<void*>(h.e), static_cast<double>(m[3][0]), static_cast<double>(m[3][1]), static_cast<double>(m[3][2]), no_local);
            }
            if (h.ours) {
                // Out of the corrected set first (returns once no Sam update can still write into it); then, if the mover
                // still holds a write of ours (flag 0, translation within 1.6 m of our offset), the game's flag and the
                // identity go back, so the item is not left on a stale corrected matrix.
                ++g_hg.game_changed;
                PublishOurMovers(&h);
                h.ours = false;
                std::uint8_t f2 = 1;
                float m2[4][4];
                if (Rd(h.mover + kMoverNoLocal, f2) && f2 == 0 && realbody::ReadHeadMoverLocal(h.mover + kMoverLocal, m2)) {
                    const double dx = m2[3][0] - h.written[0], dy = m2[3][1] - h.written[1], dz = m2[3][2] - h.written[2];
                    SIZE_T wrote_f = 0;
                    if (std::sqrt(dx * dx + dy * dy + dz * dz) <= 1.6 &&
                        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(h.mover + kMoverNoLocal), &h.orig_flag, 1, &wrote_f) && wrote_f == 1 &&
                        WriteIdentity(h.mover)) {
                        ++g_hg.restores;
                        Log("[headgear] our %s %p: refused while ours: taken out of the corrected set and put back to the identity", h.name, reinterpret_cast<void*>(h.e));
                    }
                }
            }
            continue;
        }
        h.refused = false;
        if (!h.ours) h.orig_flag = no_local;
        const bool any = want[0] != 0 || want[1] != 0 || want[2] != 0;
        const std::uint8_t want_flag = any ? 0 : h.orig_flag;
        // ours with the same offset: Sam's update owns the matrix (corrected every frame)
        if (h.ours && any && std::memcmp(want, h.written, sizeof(want)) == 0 && no_local == 0) continue;
        if (!h.ours && !any && is_zero && no_local == want_flag) continue;
        SIZE_T wrote = 0, wrote_f = 0;
        bool ok;
        if (any) {  // the matrix first (identity rotation, the offset), then the flag: an update between the two still skips the local
            const float mm[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {want[0], want[1], want[2], 1}};
            ok = WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(h.mover + kMoverLocal), mm, sizeof(mm), &wrote) && wrote == sizeof(mm) &&
                 WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(h.mover + kMoverNoLocal), &want_flag, 1, &wrote_f) && wrote_f == 1;
        } else {  // the flag back first, then the identity
            ok = WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(h.mover + kMoverNoLocal), &want_flag, 1, &wrote_f) && wrote_f == 1 &&
                 WriteIdentity(h.mover);
        }
        if (!ok) {
            ++g_hg.bad;
            continue;
        }
        std::memcpy(h.written, want, sizeof(want));
        h.ours = any;
        ++(any ? g_hg.writes : g_hg.restores);
        if (g_hg.logs++ < 60)
            Log("[headgear] our %s %p: mover offset %s (%.1f, %.1f, %.1f) cm in HLP_Head's frame (x = -lateral, y forward, z up)", h.name, reinterpret_cast<void*>(h.e),
                any ? "set to" : "back to the identity", static_cast<double>(want[0]) * 100, static_cast<double>(want[1]) * 100, static_cast<double>(want[2]) * 100);
    }
    PublishOurMovers();
    // the correction's counters: every second in hidden tests, every 10 s in play, while it has movers
    static double next_ms = 0;
    bool any_ours = false;
    for (const HeadItem& h : g_hi) any_ours = any_ours || (h.e && h.ours);
    if (any_ours && now >= next_ms) {
        next_ms = now + (automation::Enabled() ? 1000 : 10000);
        char info[320];
        realbody::HeadCorrInfo(info, sizeof(info));
        Log("[headgear] %s", info);
    }
}

// The headgear meter: where a head item sits in the drawn body's head frame (joint 31), every tick, cm. The self view:
// our accessory against the self body's head (and against Sam's fitted head); the partner: our prop against the partner
// body's head. Logged each second: mean, standard deviation and the largest tick-to-tick step of that position.
struct HeadSeries {
    double n = 0, s[3] = {}, ss[3] = {}, step_max = 0, prev[3] = {};
    bool have_prev = false;
    void Add(const double (&v)[3]) {
        if (have_prev) {
            const double d = std::sqrt((v[0] - prev[0]) * (v[0] - prev[0]) + (v[1] - prev[1]) * (v[1] - prev[1]) + (v[2] - prev[2]) * (v[2] - prev[2]));
            if (d > step_max) step_max = d;
        }
        std::memcpy(prev, v, sizeof(prev));
        have_prev = true;
        ++n;
        for (int k = 0; k < 3; ++k) s[k] += v[k], ss[k] += v[k] * v[k];
    }
    void Describe(char* out, std::size_t cap) const {
        double m[3], sd[3];
        for (int k = 0; k < 3; ++k) {
            m[k] = s[k] / n;
            const double var = ss[k] / n - m[k] * m[k];
            sd[k] = var > 0 ? std::sqrt(var) : 0;
        }
        std::snprintf(out, cap, "mean (%.2f, %.2f, %.2f) sd (%.2f, %.2f, %.2f) step max %.2f over %.0f", m[0], m[1], m[2], sd[0], sd[1], sd[2], step_max, n);
    }
    void Reset() { *this = HeadSeries{}; }
};
HeadSeries g_hm_self[2], g_hm_sam[2], g_hm_partner[4];
const char* g_hm_item[2] = {"", ""};  // the item last measured per class (cap00..03, eyewear0 / glasses00..02)
double g_bm_self[4] = {}, g_bm_p[4] = {};  // R sum, L sum, R max, L max
unsigned g_bm_self_n = 0, g_bm_p_n = 0;
double g_hm_next_ms = 0;
int g_hm_on = -1;  // `gear 19 <0/1>`; -1: on in hidden tests only (automation), off in play

// v: the point in the frame (p, r rows) in cm
void InFrame(const double (&p)[3], const float (&r)[3][3], const Xf& w, double (&v)[3]) {
    const double d[3] = {w.p[0] - p[0], w.p[1] - p[1], w.p[2] - p[2]};
    for (int i = 0; i < 3; ++i) v[i] = (d[0] * r[i][0] + d[1] * r[i][1] + d[2] * r[i][2]) * 100.0;
}

void HeadMeter(game::Entity* local, double now) {
    if (g_hm_on == 0 || (g_hm_on < 0 && !automation::Enabled())) return;
    // the self view: our shown accessories
    game::Entity* self = realbody::SelfShown() ? realbody::SelfEntity() : nullptr;
    if (self) {
        double hp[3], sp[3];
        float hr[3][3], sr[3][3];
        const bool have_h = realbody::JointFrameWorld(self, 31, hp, hr);
        const bool have_s = local && realbody::JointFrameWorld(local, 31, sp, sr);
        for (const HeadItem& h : g_hi) {
            if (!h.e || now - h.seen_ms > 600 || h.cls < 0 || h.cls > 1) continue;
            Xf w;
            if (!ReadWorld(h.e, &w)) continue;
            double v[3];
            g_hm_item[h.cls] = h.name;
            if (have_h) InFrame(hp, hr, w, v), g_hm_self[h.cls].Add(v);
            if (have_s) InFrame(sp, sr, w, v), g_hm_sam[h.cls].Add(v);
        }
    }
    // the partner: our props anchored on its head joints
    PropSet& ps = g_sets[0];
    game::Entity* pb = ps.body;
    if (pb && ps.have_want) {
        double hp[3];
        float hr[3][3];
        if (realbody::JointFrameWorld(pb, 31, hp, hr)) {
            unsigned k = 0;
            for (unsigned i = 0; i < ps.want.count && k < 4; ++i) {
                const wire::GearItem& it = ps.want.items[i];
                if (it.parent != wire::kGearNoParent || it.joint < 31 || it.joint > 34 || !ps.props[i].live || !ps.props[i].shown) continue;
                Xf w;
                if (!holo::SpawnAlive(ps.props[i].sp) || !ReadWorld(reinterpret_cast<std::uintptr_t>(ps.props[i].sp.e), &w)) continue;
                double v[3];
                InFrame(hp, hr, w, v);
                g_hm_partner[k++].Add(v);
            }
        }
    }
    // the boots meter (section 21): the boots' feet (their joints 4 and 8) against the drawn body's feet (63, 68), cm
    {
        auto foot_d = [](game::Entity* boots, game::Entity* body, unsigned bj, unsigned sj, double* d) {
            double a[3], b[3];
            if (!realbody::JointWorldN(boots, bj, 10, a) || !realbody::JointWorld(body, sj, b)) return false;
            *d = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])) * 100.0;
            return std::isfinite(*d);
        };
        double dr = 0, dl = 0;
        if (self && g_boots.shown && g_boots.e && foot_d(reinterpret_cast<game::Entity*>(g_boots.e), self, 4, 63, &dr) &&
            foot_d(reinterpret_cast<game::Entity*>(g_boots.e), self, 8, 68, &dl)) {
            g_bm_self[0] += dr, g_bm_self[1] += dl, ++g_bm_self_n;
            if (dr > g_bm_self[2]) g_bm_self[2] = dr;
            if (dl > g_bm_self[3]) g_bm_self[3] = dl;
        }
        if (pb && g_boots_prop_shown && g_boots_prop_e && foot_d(reinterpret_cast<game::Entity*>(g_boots_prop_e), pb, 4, 63, &dr) &&
            foot_d(reinterpret_cast<game::Entity*>(g_boots_prop_e), pb, 8, 68, &dl)) {
            g_bm_p[0] += dr, g_bm_p[1] += dl, ++g_bm_p_n;
            if (dr > g_bm_p[2]) g_bm_p[2] = dr;
            if (dl > g_bm_p[3]) g_bm_p[3] = dl;
        }
    }
    if (now < g_hm_next_ms) return;
    g_hm_next_ms = now + (automation::Enabled() ? 1000 : 10000);  // every second in hidden tests, else every 10 s
    if (g_bm_self_n || g_bm_p_n) {
        char sa[96] = "-", pa[96] = "-";
        if (g_bm_self_n) std::snprintf(sa, sizeof(sa), "R %.1f/%.1f L %.1f/%.1f over %u", g_bm_self[0] / g_bm_self_n, g_bm_self[2], g_bm_self[1] / g_bm_self_n, g_bm_self[3], g_bm_self_n);
        if (g_bm_p_n) std::snprintf(pa, sizeof(pa), "R %.1f/%.1f L %.1f/%.1f over %u", g_bm_p[0] / g_bm_p_n, g_bm_p[2], g_bm_p[1] / g_bm_p_n, g_bm_p[3], g_bm_p_n);
        std::uint32_t sw = 0, pw = 0;
        realbody::FeetCounters(&sw, &pw);
        Log("[boots] boots' feet vs the drawn feet cm mean/max: self (Sam's boots, her body) %s; partner (boots prop, partner body) %s; feet-scaled pose writes self %u partner %u",
            sa, pa, sw, pw);
    }
    for (double& v : g_bm_self) v = 0;
    for (double& v : g_bm_p) v = 0;
    g_bm_self_n = g_bm_p_n = 0;
    char a[160];
    static const char* const kCls[2] = {"cap", "glasses"};
    for (int c = 0; c < 2; ++c) {
        if (g_hm_self[c].n > 0) {
            char b[160] = "-";
            g_hm_self[c].Describe(a, sizeof(a));
            if (g_hm_sam[c].n > 0) g_hm_sam[c].Describe(b, sizeof(b));
            Log("[headgear] self %s (%s) in her drawn head joint's frame cm (x, y up, z forward): %s; in Sam's fitted head joint's frame: %s", kCls[c],
                g_hm_item[c], a, b);
        }
        g_hm_self[c].Reset(), g_hm_sam[c].Reset();
    }
    for (int k = 0; k < 4; ++k) {
        if (g_hm_partner[k].n > 0) {
            g_hm_partner[k].Describe(a, sizeof(a));
            Log("[headgear] partner head prop %d in the partner body's head joint's frame cm (x, y up, z forward): %s", k, a);
        }
        g_hm_partner[k].Reset();
    }
    static std::uint32_t pw = 0, pr = 0, pc = 0, pf = 0;
    if (pw != g_hg.writes || pr != g_hg.restores || pc != g_hg.game_changed || pf != g_hg.refusals)
        Log("[headgear] mover writes %u, restores %u, changed by the game %u, refused %u, bad reads %u", g_hg.writes, g_hg.restores, g_hg.game_changed,
            g_hg.refusals, g_hg.bad);
    pw = g_hg.writes, pr = g_hg.restores, pc = g_hg.game_changed, pf = g_hg.refusals;
}

// Our Sam's equipped boots entity (inventory +0x4D80, a plain entity pointer: the inventory's visual update reads its
// flags there, 0x1427F1D2F) and whether the game shows it (+0x88 bit 1). The self view hides her feet only while it is.
constexpr std::uintptr_t kInvBoots = 0x4D80, kInvBootsSite = 0x27F1D2F;
void ReadBootsShown(std::uintptr_t inv) {
    if (!g_boots.checked) {
        g_boots.checked = true;
        g_boots.ok = mem::CodeMatches(mem::Base() + kInvBootsSite, "48 8B 86 80 4D 00 00 48 8B 88 88 00 00 00 D0 E9 F6 C1 01") &&
                     mem::CodeMatches(mem::Base() + kInvBootsSite + 0x1B, "48 8B 86 88 4D 00 00") &&
                     mem::CodeMatches(mem::Base() + kInvBootsSite + 0x25, "48 8B 8E 80 4D 00 00");
        Log("[boots] the inventory's boots entity sites %s", g_boots.ok ? "verified" : "do not match: her feet are not hidden, no boots sent");
    }
    std::uintptr_t e = 0;
    bool shown = false;
    if (g_boots.ok && inv) {
        e = Q(inv + kInvBoots);
        if (e && !Derives(e, "Entity")) e = 0;
        shown = e && Visible(e);
    }
    if (e != g_boots.e || shown != g_boots.shown)
        Log("[boots] our Sam's boots entity %p %s", reinterpret_cast<void*>(e), shown ? "shown" : e ? "not shown" : "(none)");
    if (e != g_boots.e) g_boots_have_ready = false;  // other boots: their own settled parts first
    g_boots.e = e;
    g_boots.shown = shown;
}

// ---------------------------------------------------------------- her boots' style (section 30)
// Sam's boots entity hidden under the self view (boots_look=own, or prop with boots that keep her own): the game's own
// entity hide flag (+0x88 bit 2) through its setter 0x14234D6B0 (a locked bit op, then the visibility propagated as
// 0x14235F640 does). The inventory's visual update only ever writes bit 1 (0x14235F640 from 0x1427F1D69), so the hide
// holds; a bit 2 the game set itself is never taken over or cleared by us. Game thread; byte-checked once; a fault turns it
// off (her feet are then scaled away under Sam's boots, the v0.22 look).
constexpr std::uintptr_t kEntSetHidden = 0x234D6B0;
struct SamBootsHide {
    bool checked = false, ok = false, faulted = false;
    std::uintptr_t e = 0;  // the boots entity whose bit 2 we set (0 none)
    unsigned sets = 0, clears = 0, reasserts = 0;
} g_sbh;
bool SamBootsHideOk() {
    if (!g_sbh.checked) {
        g_sbh.checked = true;
        g_sbh.ok = mem::CodeMatches(mem::Base() + kEntSetHidden,
                                    "48 83 EC 48 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 38 48 8B 81 88 00 00 00 C0 E8 02 24 01 3A D0 74 6E 84 D2 74 0B "
                                    "F0 48 83 89 88 00 00 00 04");
        Log("[boots] the entity hide setter (+0x88 bit 2) %s", g_sbh.ok ? "verified: Sam's boots can be hidden under the self view"
                                                                        : "does not match: Sam's boots stay drawn over her (her feet scaled away instead)");
    }
    return g_sbh.ok && !g_sbh.faulted;
}
bool CallSetHidden(std::uintptr_t e, bool on) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, unsigned char)>(mem::Base() + kEntSetHidden)(e, on ? 1 : 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void HideSamBoots(std::uintptr_t e, bool want) {
    if (!SamBootsHideOk()) return;
    if (g_sbh.e && g_sbh.e != e) g_sbh.e = 0;  // other boots: the old entity is the game's again (it may be gone: not touched)
    std::uint64_t f = 0;
    if (!e || !Rd(e + kEntFlags, f)) return;
    if (want) {
        if (f & 4) return;  // hidden (by us, or by the game: then not ours)
        if (!CallSetHidden(e, true)) {
            g_sbh.faulted = true;
            Log("[boots] the entity hide setter FAULTED: Sam's boots stay drawn under the self view");
            return;
        }
        if (g_sbh.e == e) {
            ++g_sbh.reasserts;
        } else {
            ++g_sbh.sets;
            Log("[boots] Sam's boots entity %p hidden under the self view (her own boots show; %u time(s))", reinterpret_cast<void*>(e), g_sbh.sets);
        }
        g_sbh.e = e;
    } else if (g_sbh.e == e) {
        if ((f & 4) && !CallSetHidden(e, false)) {
            g_sbh.faulted = true;
            Log("[boots] the entity hide setter FAULTED");
        }
        ++g_sbh.clears;
        Log("[boots] Sam's boots entity %p shown again", reinterpret_cast<void*>(e));
        g_sbh.e = 0;
    }
}
bool IsHerSuit(const bodies::Body* b) { return b && std::strcmp(b->key, "fragile_suit") == 0; }
// The self view (her body in Sam's place): Sam's boots drawn over her (her feet scaled away) or hidden (her own boots).
// Other characters keep the v0.22 rule (their feet scaled under Sam's boots).
void SelfBootsTick() {
    const bool her = IsHerSuit(realbody::SelfBody());
    bootslook::Decision d;
    if (her) d = bootslook::Decide(g_boots_style, g_boots.shown, g_boots_visible & g_boots_apply, g_boots_own_porter);
    else d.scale_feet = g_boots.shown;
    const bool can_hide = SamBootsHideOk();
    HideSamBoots(g_boots.e, her && d.hide_sam && realbody::SelfShown());
    realbody::SetSamBootsShown(d.scale_feet || (d.hide_sam && !can_hide));  // no hide setter: the v0.22 feet
    static int last = -1;
    const int code = !her ? 8 : (d.hide_sam ? 1 : 0) + (d.scale_feet ? 2 : 0) + (g_boots.shown ? 4 : 0);
    if (code != last) {
        last = code;
        Log("[boots] self view: %s (boots_look=%s, Sam's boots entity %s, parts %08X)",
            !her ? (realbody::SelfBody() ? "another character: its feet scaled under Sam's boots" : "no self view (Sam's own boots)") : d.hide_sam ? "her own boots, Sam's hidden" : d.scale_feet ? "Sam's boots on her feet" : "her own boots",
            bootslook::StyleName(g_boots_style), g_boots.shown ? "shown" : g_boots.e ? "not shown" : "none", g_boots_visible & g_boots_apply);
    }
}

// ---------------------------------------------------------------- props (game thread)
// Game calls on ArtParts (a partner Sam body's gear parts, the boots on our Sam's feet under the self
// view), byte-checked once: the per-part setter 0x1422B2770 (apd, visible, index), the top-level
// setter 0x1422B25F0 (apd, visible, 0) and the model update bracket ApplyMeshControl wraps them in
// (0x142391620 (model, 1) ... 0x1423966A0 (model): the entity's critical section +0x288).
constexpr std::uintptr_t kApdSetPart = 0x22B2770, kApdSetAll = 0x22B25F0, kModelBegin = 0x2391620, kModelEnd = 0x23966A0;
struct ApdFns {
    bool done = false, ok = false, faulted = false;
    char why[160] = {};
} g_apd;

bool ApdFnsOk() {
    if (g_apd.done) return g_apd.ok && !g_apd.faulted;
    g_apd.done = true;
    struct Site {
        const char* name;
        std::uintptr_t rva;
        const char* sig;
    };
    static const Site kSites[] = {
        {"ArtParts part setter", kApdSetPart, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 0F B6 41 38 0F B6 F2 49 63 D8"},
        {"ArtParts top-level setter", kApdSetAll, "48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 48 8B 41 48"},
        {"model update begin", kModelBegin, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 79 48 48 8B D9 8B F2"},
        {"model update end", kModelEnd, "48 89 5C 24 10 57 48 83 EC 60 48 8B 05"}};
    g_apd.ok = true;
    for (const Site& s : kSites)
        if (!mem::CodeMatches(mem::Base() + s.rva, s.sig)) {
            std::snprintf(g_apd.why, sizeof(g_apd.why), "%s at ds.exe+0x%llX does not match", s.name, static_cast<unsigned long long>(s.rva));
            g_apd.ok = false;
            break;
        }
    Log("[gear] ArtParts functions %s", g_apd.ok ? "verified (part / top-level setters, model update bracket)" : g_apd.why);
    return g_apd.ok;
}

using ModelBeginFn = void (*)(std::uintptr_t, unsigned);
using ModelEndFn = void (*)(std::uintptr_t);
using SetPartFn = void (*)(std::uintptr_t, unsigned long long, int);
using SetAllFn = void (*)(std::uintptr_t, unsigned char, char);
// idx < 0: every top-level part. SEH: a fault switches these calls off for the session. A fault
// after the bracket opened closes it (the entity's critical section +0x288 must not stay held: the
// body's next model update, on a worker thread at 240 Hz, would wait on it for good).
void CloseModelBracket(std::uintptr_t sm) {
    __try {
        reinterpret_cast<ModelEndFn>(mem::Base() + kModelEnd)(sm);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
bool CallSetParts(std::uintptr_t sm, std::uintptr_t apd, int idx, bool vis) {
    const std::uintptr_t b = mem::Base();
    volatile bool open = false;
    __try {
        reinterpret_cast<ModelBeginFn>(b + kModelBegin)(sm, 1);
        open = true;
        if (idx < 0) reinterpret_cast<SetAllFn>(b + kApdSetAll)(apd, vis ? 1 : 0, 0);
        else reinterpret_cast<SetPartFn>(b + kApdSetPart)(apd, vis ? 1ull : 0ull, idx);
        open = false;
        reinterpret_cast<ModelEndFn>(b + kModelEnd)(sm);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_apd.faulted = true;
        if (open) CloseModelBracket(sm);
        return false;
    }
}

// The entity's model (+0xB8) and ArtPartsData, checked: the model points back at the entity.
bool ModelAndApd(std::uintptr_t e, std::uintptr_t* sm, Apd* a) {
    *sm = Q(e + 0xB8);
    return *sm && Q(*sm + 0x48) == e && ReadApd(e, a);
}

// Mirrored ArtParts: a target (the partner's Sam body, the backpack prop) shows the sender's parts when
// its ArtParts are the very parts of our own reference entity (our Sam, our backpack): the same part
// resources at every index the sender names. Only those indices are set, and only when they differ.
struct PartsMirror {
    const char* what = "";
    std::uintptr_t target = 0;
    bool checked = false, compatible = false;
    unsigned applied = 0, failed = 0, checks = 0;
    double next_ms = 0, recheck_ms = 0;
};
PartsMirror g_pm{"partner body"}, g_pack_pm{"backpack prop"};
PartsMirror g_boots_pm{"boots prop"};  // the partner's boots prop (BootsTick)
// The boots prop `e` may be shown, and the partner body's feet scaled away under it: it is the pose target, a pose was
// written onto it within 250 ms and the partner's boots parts are mirrored on it (our own boots entity is the parts'
// reference: without one, barefoot or unreadable, the partner keeps its own boots and no prop shows).
bool BootsPropReady(std::uintptr_t e) {
    return e && e == g_boots_prop_e && realbody::BootsPosed(250) && g_boots_pm.target == e && g_boots_pm.compatible;
}

void MirrorParts(PartsMirror& m, std::uintptr_t target, std::uintptr_t ref, const std::uint64_t* apply, const std::uint64_t* visible, unsigned max,
                 double now) {
    if (m.target != target) {
        const char* w = m.what;
        m = PartsMirror{};
        m.what = w;
        m.target = target;
    }
    if (!target || !ref || now < m.next_ms || !ApdFnsOk()) return;
    m.next_ms = now + 100;
    std::uintptr_t sm = 0;
    Apd b, l;
    if (!ModelAndApd(target, &sm, &b) || !ReadApd(ref, &l)) return;
    // Not compatible yet: checked again every 3 s (our own Sam may have been in another suit or body
    // variant when the body came; the body keeps its parts until then).
    if (m.checked && !m.compatible && now >= m.recheck_ms) m.checked = false;
    if (!m.checked) {
        m.checked = true;
        m.recheck_ms = now + 3000;
        const bool was = m.compatible;
        ++m.checks;
        unsigned same = 0, named = 0;
        for (unsigned i = 0; i < max; ++i) {
            if (!Bit(apply, i)) continue;
            ++named;
            const std::uintptr_t bp = i < static_cast<unsigned>(b.n) ? Q(b.arr + 8ull * i) : 0;
            const std::uintptr_t lp = i < static_cast<unsigned>(l.n) ? Q(l.arr + 8ull * i) : 0;
            if (bp && lp && Q(bp + 0x20) == Q(lp + 0x20)) ++same;  // (a part without a resource of its own: none on both)
        }
        m.compatible = b.n == l.n && named > 0 && same == named;
        if (m.checks == 1 || m.compatible != was || m.checks % 100 == 0)
            Log("[gear] %s %p: %d ArtParts (ours %d), %u of %u named parts are the same resources: %s", m.what, reinterpret_cast<void*>(target), b.n, l.n,
                same, named, m.compatible ? "the partner's parts are mirrored on it" : "another model: its parts are left as they are (checked again every 3 s)");
    }
    if (!m.compatible) return;
    unsigned changed = 0;
    for (unsigned i = 0; i < max && i < static_cast<unsigned>(b.n); ++i) {
        if (!Bit(apply, i)) continue;
        const bool want = Bit(visible, i);
        std::uint8_t fl = 0;
        if (!PartFlags(b, i, &fl) || ((fl & 1) != 0) == want) continue;
        if (!CallSetParts(sm, b.apd, static_cast<int>(i), want)) {
            ++m.failed;
            Log("[gear] the ArtParts part setter faulted on the %s: parts off for this session", m.what);
            return;
        }
        ++changed;
    }
    if (changed) {
        m.applied += changed;
        if (m.applied <= 200) Log("[gear] %s: %u part(s) set (total %u)", m.what, changed, m.applied);
    }
}

// The boots prop's pose target (section 21) goes before the prop does.
void ReleaseBootsTarget(game::Entity* e) {
    if (!e || reinterpret_cast<std::uintptr_t>(e) != g_boots_prop_e) return;
    realbody::SetPartnerFeetHidden(false, 100);
    realbody::ClearBootsProp();
    g_boots_prop_e = 0;
}

void ClearPoses(PropSet& s) {
    for (unsigned k = 0; k < realbody::kGearPoseTargets; ++k)
        if (s.pose_e[k]) {
            realbody::ClearGearPose(k);
            s.pose_e[k] = 0;
            s.pose_seq_pub[k] = 0;
        }
    s.pose_targets = 0;
}

void Unpublish(PropSet& s) {
    ClearPoses(s);
    if (!s.published) return;
    realbody::PublishGear(s.slot, nullptr, 0);
    s.published = false;
}

// A held clone's removal (section 31: our own entity, no spawner), logged (the first 60).
void NoteHeldRemoved(const PropSet& s, const Prop& p, const char* step) {
    static unsigned logs = 0;
    if (IsHeldKey(p.def) && ++logs <= 60) Log("[%s] held clone '%s' removed: %s", s.tag, KeyLabel(p.def, s.res), step);
}

void RemoveProp(PropSet& s, unsigned i, const char* why) {
    Prop& p = s.props[i];
    ReleaseBootsTarget(p.sp.e);
    if (p.loading || p.live || p.sp.sp || p.sp.direct) {
        const Prop was = p;
        char step[160] = {};
        holo::SpawnTeardown(p.sp, why, step, sizeof(step));
        NoteHeldRemoved(s, was, step);
        ++s.removed;
    }
    p = Prop{};
}

void RemoveSpare(PropSet& s, unsigned k, const char* why) {
    PropSet::Spare& sp = s.spare[k];
    ReleaseBootsTarget(sp.p.sp.e);
    if (sp.p.loading || sp.p.live || sp.p.sp.sp || sp.p.sp.direct) {
        const Prop was = sp.p;
        char step[160] = {};
        holo::SpawnTeardown(sp.p.sp, why, step, sizeof(step));
        NoteHeldRemoved(s, was, step);
        ++s.removed;
    }
    sp = PropSet::Spare{};
}

void RemoveExo(PropSet& s, const char* why);  // section 32 (below)

void RemoveAll(PropSet& s, const char* why) {
    Unpublish(s);
    RemoveExo(s, why);
    for (unsigned k = 0; k < sizeof(s.spare) / sizeof(s.spare[0]); ++k) RemoveSpare(s, k, why);
    unsigned n = 0;
    for (unsigned i = 0; i < wire::kMaxGearItems; ++i)
        if (s.props[i].loading || s.props[i].live || s.props[i].sp.sp || s.props[i].sp.direct) {
            RemoveProp(s, i, why);
            ++n;
        } else {
            s.props[i] = Prop{};
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
    // the exo carrier (section 32): on the body's joint 0, identity pose (its own root is identity: its joints are the body's)
    if (s.exo.p.live && s.exo.target && n < realbody::kMaxGearProps) {
        realbody::GearProp& g = list[n++];
        g = realbody::GearProp{};
        g.e = reinterpret_cast<std::uintptr_t>(s.exo.p.sp.e);
        g.e_vt = s.exo.p.vt;
        g.guid = s.exo.p.guid;
        g.joint = 0;
        g.parent = -1;
        g.r[0][0] = g.r[1][1] = g.r[2][2] = 1.0f;
    }
    if (n == 0 && !s.published) return;  // nothing to place, nothing placed: no call (an empty publish waits for the detour)
    // Unpublish first when a prop of the old list may go away (the caller removes only after this).
    realbody::PublishGear(s.slot, list, n);
    s.published = n > 0;
    s.published_n = n;
}

bool ValidateProp(game::Entity* e, bool entity_item, char* why, std::size_t cap) {
    const auto a = reinterpret_cast<std::uintptr_t>(e);
    const std::uintptr_t rtti = mem::RttiOf(a);
    if (!rtti) return std::snprintf(why, cap, "entity type unknown"), false;
    if (mem::DerivesFrom(rtti, "Humanoid") || mem::DerivesFrom(rtti, "DSPlayerEntity") || mem::DerivesFrom(rtti, "DSBackpackEntity"))
        return std::snprintf(why, cap, "a Humanoid / player / backpack entity"), false;
    for (const char* bad : {"AIIndividualComponent", "AIBodyComponent", "DSPlayerControllerComponent", "HumanoidController", "CapsuleComponent",
                            "PhysicsCollisionComponent", "PhysicsRagdollComponent"})
        if (partner::FindComponentByName(e, bad)) return std::snprintf(why, cap, "carries %s", bad), false;
    if (!entity_item) return true;
    // A game gear entity's copy: the same class / component rule the sender applies (MirrorableAt).
    const char* no = nullptr;
    if (!MirrorableAt(a, g_mirror_level, &no)) return std::snprintf(why, cap, "%s", no ? no : "not mirrorable"), false;
    return true;
}

void SetShown(PropSet& s, unsigned i, bool on, double now) {
    Prop& p = s.props[i];
    if (!p.live || !holo::SpawnAlive(p.sp)) return;
    // The real flag, not a cached one: Sleep / Wake, activation or a menu may have changed it.
    std::uint64_t fl = 0;
    const bool is_on = partner::ReadFlags(p.sp.e, &fl) && (fl & 0x2) != 0;
    if (p.shown == on && is_on == on) return;
    if (p.shown == on && is_on != on) {
        // A prop whose own components hide it again and again (more than 5 times in 3 s) is left to
        // them while it should be shown (hiding it with the body still wins); a one-off change is undone.
        if (now - p.reshow_since > 3000) p.reshow_since = now, p.reshows = 0;
        if (on && !p.game_owned && ++p.reshows > 5) {
            p.game_owned = true;
            Log("[%s] prop %u is hidden by its own components again and again: its visibility is left to them", s.tag, i);
        }
        if (on && p.game_owned) return;
        if (++s.reshown <= 20) Log("[%s] prop %u was %s by the game: set %s again", s.tag, i, is_on ? "shown" : "hidden", on ? "visible" : "invisible");
    }
    if (partner::SetVisible(p.sp.e, on)) {
        if (on && !p.shown && IsHandProp(p.def) && ++s.weapon_shows <= 60)
            Log("[%s] %s %u '%s' shown %.0f ms after its layout was taken", s.tag, IsHeldKey(p.def) ? "held clone" : "weapon prop", i, KeyLabel(p.def, s.res),
                s.layout_ms > 0 ? now - s.layout_ms : -1.0);
        p.shown = on;
    }
}

// The path + GUID a prop key loads (false: an entity resource the table does not name yet).
bool KeySource(const PropSet& s, int key, const char** path, const std::uint8_t** guid, const char** label) {
    static std::uint8_t parsed[kPropCount][16];
    static bool parsed_ok[kPropCount] = {};
    if (key >= 0 && key < kPropCount) {
        if (!parsed_ok[key]) parsed_ok[key] = bodies::ParseGuid(kProps[key].guid, parsed[key]);
        if (!parsed_ok[key]) return false;
        *path = kProps[key].path, *guid = parsed[key], *label = kProps[key].key;
        return true;
    }
    const int r = IsHeldKey(key) ? key - kKeyHeld : key - kKeyEntity;
    if (r < 0 || r >= static_cast<int>(wire::kMaxGearRes) || !s.res || !s.res[r].used) return false;
    *path = s.res[r].path, *guid = s.res[r].guid, *label = s.res[r].path;
    return true;
}

// An entity copy's own ArtParts as the look names them (the same entity resource: the same parts): true
// when they match; `set`: the differing ones are set (the game's part setter in the model bracket).
bool LookPartsMatch(std::uintptr_t e, const wire::GearLook& l, bool set, unsigned* changed) {
    if (!l.parts_apply) return true;
    std::uintptr_t sm = 0;
    Apd a;
    if (!ModelAndApd(e, &sm, &a)) return true;  // nothing we can do
    bool match = true;
    for (unsigned i = 0; i < 64 && i < static_cast<unsigned>(a.n); ++i) {
        if (!((l.parts_apply >> i) & 1)) continue;
        const bool want = ((l.parts_visible >> i) & 1) != 0;
        std::uint8_t fl = 0;
        if (!PartFlags(a, i, &fl) || ((fl & 1) != 0) == want) continue;
        match = false;
        if (!set || !ApdFnsOk()) continue;
        if (!CallSetParts(sm, a.apd, static_cast<int>(i), want)) {
            Log("[gear] the ArtParts part setter faulted on a gear copy: parts off for this session");
            return false;
        }
        if (changed) ++*changed;
    }
    return match;
}

// The look the partner set's item i should show: the partner's STV1 for that item of this layout (a
// debug body: our own), or nullptr.
const wire::GearLook* WantLook(const PropSet& s, unsigned i, std::uint32_t* hash) {
    const wire::GearFrame& f = s.want;
    if (&s == &g_sets[1]) {
        // the self view's props: the look of the item of our own frame each one shows (the hat in the rain's colour)
        if (i >= f.count || g_self_src[i] >= g_local.count) return nullptr;
        const unsigned src = g_self_src[i];
        const LocalLook& L = g_look[src];
        if (!L.have || L.l.layout != wire::GearLayoutHash(g_local) || L.l.item != src || L.l.kind != f.items[i].kind ||
            (L.l.kind == wire::GearKind::entity && L.l.res != f.items[i].res))
            return nullptr;
        *hash = L.hash;
        return &L.l;
    }
    const std::uint16_t h = wire::GearLayoutHash(f);
    const bool debug = realbody::GetOwner() == realbody::Owner::debug;
    const wire::GearLook* l = nullptr;
    if (debug && g_look[i].have) l = &g_look[i].l, *hash = g_look[i].hash;
    else if (!debug && g_plook[i].have) l = &g_plook[i].l, *hash = g_plook[i].hash;
    if (!l || l->layout != h || l->item != i || i >= f.count || l->kind != f.items[i].kind) return nullptr;
    if (l->kind == wire::GearKind::entity && l->res != f.items[i].res) return nullptr;
    return l;
}

// Each live prop (0.5 s after it became live) gets its item's look; every 2 s it is checked again and
// re-applied when its own components changed it. At most 4 applies per tick.
void ApplyLooks(PropSet& s, double now) {
    if (!g_looks_on || !look::ApplyAvailable()) return;
    unsigned budget = 4;
    for (unsigned i = 0; i < s.want.count && budget; ++i) {
        Prop& p = s.props[i];
        if (!p.live || !holo::SpawnAlive(p.sp) || now - p.live_ms < 500) continue;
        std::uint32_t hash = 0;
        const wire::GearLook* want = WantLook(s, i, &hash);
        if (!want) continue;
        const auto e = reinterpret_cast<std::uintptr_t>(p.sp.e);
        // The boots prop's 32 parts have one writer (section 30.4): the STC1 boots block (BootsTick, MirrorParts), never
        // the look's part list too (two writers fed by frames that arrive apart flipped the old parts back for 0.3 s).
        const bool parts_too = want->kind != wire::GearKind::boots;
        bool again = false;
        if (p.look_hash == hash) {
            if (now < p.look_check_ms) continue;
            p.look_check_ms = now + 2000;
            if (look::Satisfied(e, *want) && (!parts_too || LookPartsMatch(e, *want, false, nullptr))) continue;
            again = true;
        }
        --budget;
        char why[160] = {};
        const look::ApplyResult r = look::Apply(e, *want, why, sizeof(why));
        unsigned parts_set = 0;
        if (r != look::ApplyResult::off && parts_too) LookPartsMatch(e, *want, true, &parts_set);
        p.look_hash = hash;
        p.look_check_ms = now + 2000;
        if (r == look::ApplyResult::off) return;
        if (r == look::ApplyResult::failed) ++s.looks_failed;
        else if (r == look::ApplyResult::partial) ++s.looks_partial;
        if (again) ++s.looks_reapplied;
        else ++s.looks;
        const char* a = nullptr;
        const std::uint8_t* b = nullptr;
        const char* label = "?";
        KeySource(s, p.def, &a, &b, &label);
        if (s.looks + s.looks_reapplied <= 60)
            Log("[look] prop %u '%s': %s %u variation(s), %u shader variable(s), %u part(s) set: %s%s%s", i, label,
                again ? "look applied again (it had changed):" : "look applied:", want->nvar, want->nsv, parts_set,
                r == look::ApplyResult::done ? "done" : r == look::ApplyResult::partial ? "partly" : "failed", why[0] ? "; " : "", why);
    }
}

// The partner's backpack pose onto the backpack prop and onto every prop on it that has the same
// skeleton (the rain cover, the frame's own sub-entities), in their model updates (real_body).
void ApplyPoses(PropSet& s, double now) {
    const bool debug = realbody::GetOwner() == realbody::Owner::debug;
    const wire::GearPose* pose = debug ? (g_have_pose_local ? &g_pose_local : nullptr) : (g_have_pose_partner ? &g_pose_partner : nullptr);
    const std::uint32_t seq = debug ? g_reads : g_pose_partner_seq;
    const wire::GearFrame& f = s.want;
    // the held clone's own pose (section 31): target kHeldPoseTarget
    {
        const wire::GearPose* hp = debug ? (g_have_hpose_local ? &g_hpose_local : nullptr) : (g_have_hpose_partner ? &g_hpose_partner : nullptr);
        const std::uint32_t hseq = debug ? g_reads : g_hpose_partner_seq;
        const Prop* tp = nullptr;
        if (g_poses_on && hp && hp->layout == wire::GearLayoutHash(f) && hp->item < f.count && f.items[hp->item].kind == wire::GearKind::weapon) {
            const Prop& p = s.props[hp->item];
            std::uintptr_t sk = 0;
            std::uint32_t lay = 0;
            std::int32_t c = 0;
            if (p.live && IsHeldKey(p.def) && holo::SpawnAlive(p.sp) && look::PoseSkeleton(reinterpret_cast<std::uintptr_t>(p.sp.e), &sk, &lay, &c) &&
                lay == hp->skeleton && c == hp->n)
                tp = &p;
        }
        const std::uintptr_t te = tp ? reinterpret_cast<std::uintptr_t>(tp->sp.e) : 0;
        if (te != s.pose_e[kHeldPoseTarget] || (te && s.pose_seq_pub[kHeldPoseTarget] != hseq)) {
            bool ok = true;
            if (!te) realbody::ClearGearPose(kHeldPoseTarget);
            else ok = realbody::SetGearPose(kHeldPoseTarget, &tp->sp, hp);
            if (te != s.pose_e[kHeldPoseTarget] && ++s.pose_target_logs <= 40)
                Log("[gear] held item pose target: %p%s", reinterpret_cast<void*>(te), te ? (ok ? " (the held clone, its own joints)" : " NOT TAKEN") : "");
            s.pose_e[kHeldPoseTarget] = ok ? te : 0;
            s.pose_seq_pub[kHeldPoseTarget] = hseq;
            if (ok && te) ++g_hpose_writes;
        }
    }
    if (!g_poses_on || !pose || pose->layout != wire::GearLayoutHash(f) || pose->item >= f.count || f.items[pose->item].kind != wire::GearKind::backpack) {
        for (unsigned k = 0; k < kHeldPoseTarget; ++k)
            if (s.pose_e[k]) {
                realbody::ClearGearPose(k);
                s.pose_e[k] = 0;
                s.pose_seq_pub[k] = 0;
            }
        s.pose_targets = s.pose_e[kHeldPoseTarget] ? 1 : 0;
        return;
    }
    const unsigned bi = pose->item;
    std::uintptr_t want_e[realbody::kGearPoseTargets] = {};
    const holo::Spawn* want_sp[realbody::kGearPoseTargets] = {};
    unsigned n = 0;
    const Prop& bp = s.props[bi];
    std::uintptr_t skel = 0;
    std::uint32_t layout = 0;
    std::int32_t count = 0;
    if (bp.live && holo::SpawnAlive(bp.sp) && now - bp.live_ms >= 500) {
        const auto be = reinterpret_cast<std::uintptr_t>(bp.sp.e);
        if (look::PoseSkeleton(be, &skel, &layout, &count) && layout == pose->skeleton && count == pose->n) want_sp[n] = &bp.sp, want_e[n++] = be;
    }
    for (unsigned i = 0; i < f.count && n && n < kHeldPoseTarget; ++i) {
        const Prop& p = s.props[i];
        if (i == bi || f.items[i].parent != bi || !p.live || !holo::SpawnAlive(p.sp) || now - p.live_ms < 500) continue;
        const auto e = reinterpret_cast<std::uintptr_t>(p.sp.e);
        std::uintptr_t sk = 0;
        std::uint32_t lay = 0;
        std::int32_t c = 0;
        if (look::PoseSkeleton(e, &sk, &lay, &c) && sk == skel && c == pose->n) want_sp[n] = &p.sp, want_e[n++] = e;
    }
    for (unsigned k = 0; k < kHeldPoseTarget; ++k) {
        if (want_e[k] != s.pose_e[k] || (want_e[k] && s.pose_seq_pub[k] != seq)) {
            if (!want_e[k]) {
                realbody::ClearGearPose(k);
            } else if (!realbody::SetGearPose(k, want_sp[k], pose)) {
                want_e[k] = 0;
            }
            if (want_e[k] != s.pose_e[k] && ++s.pose_target_logs <= 40)
                Log("[gear] backpack pose target %u: %p%s", k, reinterpret_cast<void*>(want_e[k]), k == 0 ? " (the backpack prop)" : " (a prop on it, same skeleton)");
            s.pose_e[k] = want_e[k];
            s.pose_seq_pub[k] = seq;
        }
    }
    unsigned live = 0;
    for (std::uintptr_t e : s.pose_e) live += e != 0;
    if (live != s.pose_targets) s.pose_targets = live;
}

// A held clone (section 31): a plain Entity of the weapon resource our own game has loaded for the same item (held_items.cpp),
// created at once; `sp` describes it as a realized, spawner-less entity (holo::SpawnTeardown removes it).
std::uint32_t g_held_made = 0, g_held_failed = 0;
bool HeldBegin(const char* path, const std::uint8_t (&guid)[16], const game::WorldTransform& at, holo::Spawn* sp, char* why, std::size_t cap) {
    game::Entity* local = nullptr;
    if (!game::TryGetLocalPlayer(&local) || !local) return std::snprintf(why, cap, "no local player"), false;
    game::Entity* e = nullptr;
    bool approx = false;
    if (!held::Create(local, path, guid, at, &e, &approx, why, cap)) {
        ++g_held_failed;
        return false;
    }
    *sp = holo::Spawn{};
    sp->direct = true;
    sp->realized = true;
    sp->e = e;
    partner::ReadVtable(e, &sp->vt);
    partner::ReadGuid(e, &sp->guid);
    ++g_held_made;
    if (g_held_made <= 60) Log("[gear] %s", why);
    return true;
}

// ---------------------------------------------------------------- the exo carrier (docs/real-partner-body.md section 32)
// Sam's skeleton on a body that is not Sam (Fragile): a demo_sam_clean body (the same ArtParts as the porter suit, no mesh
// controller, so parts we set stay set) showing only the skeleton pieces the sender's Sam shows, posed every frame in its own
// model update from the slot's body (real_body.cpp ExoTarget), placed on the body's joint 0 (the last entry of the slot's
// published list), tinted with the exos variation (outfit::Apply). Shown only once its parts are set and a pose was written
// within 250 ms (never a full Sam for a frame). Set 0: the partner body (the sender's parts block); set 1: the self body (our
// own reading through the self view). A Sam body keeps MirrorParts (its own skeleton pieces).
constexpr const char* kExoPath = "ds/entities/cinematics/demo_sam_clean";
constexpr const char* kExoGuid = "514b5705-aaf0-a447-a82e-0bb83b855201";
bool g_exo_on = true;    // session.ini exo_carrier / `gear 27 <0/1>`
bool g_exo_leg = false;  // `gear 29 <0/1>`: the sender's LegOut suit piece on her too (comparison)

const char* ExoTag(const PropSet& s) { return s.slot == realbody::GearSlot::self ? "exo-self" : "exo"; }

// The body is a Sam body (its own ArtParts are the porter suit's 107: MirrorParts shows his skeleton on it).
bool BodyIsSam(PropSet::Exo& x, game::Entity* body) {
    const auto e = reinterpret_cast<std::uintptr_t>(body);
    if (x.body_checked != e) {
        Apd a;
        x.body_checked = e;
        x.body_sam = ReadApd(e, &a) && a.n == exo::kSuitParts;
    }
    return x.body_sam;
}

// What the slot's sender shows of the skeleton (false: no carrier wanted).
bool HoodWanted(const PropSet& s);  // section 37 (below)
// (round 11: or the sender's rain hood, section 37: then `vis` may name no skeleton piece)
bool ExoWant(PropSet& s, game::Entity* body, std::uint64_t (&vis)[2]) {
    vis[0] = vis[1] = 0;
    if (!g_exo_on || !body || BodyIsSam(s.exo, body)) return false;
    if (s.slot == realbody::GearSlot::self) {
        if (!g_have_parts || !g_exo_porter || !exo::AnyExoShown(g_parts_apply, g_parts_visible) || exo::TypeOfParts(g_parts_visible) < 1)
            return HoodWanted(s);
        vis[0] = g_parts_visible[0], vis[1] = g_parts_visible[1];
        return true;
    }
    if (!s.have_want || !(s.want.flags & wire::kGearParts) || !exo::SenderNamesExo(s.want.parts_apply) ||
        !exo::AnyExoShown(s.want.parts_apply, s.want.parts_visible) || exo::TypeOfParts(s.want.parts_visible) < 1)
        return HoodWanted(s);  // (only one of the game's types: never every piece at once)
    vis[0] = s.want.parts_visible[0] & s.want.parts_apply[0], vis[1] = s.want.parts_visible[1] & s.want.parts_apply[1];
    return true;
}

// Takes the carrier out of the slot's published list (waits for the detour), its pose target, its entity.
void RemoveExo(PropSet& s, const char* why) {
    PropSet::Exo& x = s.exo;
    if (!x.p.loading && !x.p.live && !x.p.sp.sp && !x.p.sp.direct) {
        x.p = Prop{};
        return;
    }
    if (s.published) {
        realbody::PublishGear(s.slot, nullptr, 0);  // the list without it is published again by DriveSet
        s.published = false;
    }
    realbody::ClearExoCarrier(s.slot);
    outfit::Forget(x.dress);
    char step[160] = {};
    holo::SpawnTeardown(x.p.sp, why, step, sizeof(step));
    Log("[%s] carrier removed (%s): %s", ExoTag(s), why, step);
    x.p = Prop{};
    x.target = false;
    x.parts_ok = false;
    x.tint_ok = false;
    x.tree_checked = false;
    x.shown_bits[0] = x.shown_bits[1] = ~0ull;
    x.hood = exo::HoodAnim{};
    x.hood_part = -1;
    x.hood_e = 0;
    x.anim_ok = -1;
    x.anim_checks = 0;
    x.anim_slot = -1;
    ++x.removed;
}

// Its ArtParts tree (parent index per part), checked against the porter suit's.
bool ExoTree(PropSet::Exo& x, const Apd& a) {
    if (x.tree_checked) return x.tree_ok;
    x.tree_checked = true;
    x.n = a.n;
    std::uintptr_t ptr[exo::kMaxParts] = {};
    bool read = a.n > 0 && a.n <= static_cast<std::int32_t>(exo::kMaxParts);
    for (int i = 0; read && i < a.n; ++i) read = (ptr[i] = Q(a.arr + 8ull * static_cast<unsigned>(i))) != 0;
    for (int i = 0; read && i < a.n; ++i) {
        const std::uintptr_t par = Q(ptr[i] + 0x18);
        x.parent[i] = -1;
        for (int j = 0; j < a.n && par; ++j)
            if (ptr[j] == par) x.parent[i] = static_cast<std::int16_t>(j);
    }
    x.tree_ok = read && exo::CarrierTreeOk(a.n, x.parent);
    return x.tree_ok;
}

// The carrier's parts as the plan says (only those that differ; every 100 ms). True: all as wanted.
bool ExoParts(PropSet& s, const std::uint64_t (&vis)[2], double now) {
    PropSet::Exo& x = s.exo;
    if (now < x.next_parts_ms) return x.parts_ok;
    x.next_parts_ms = now + 100;
    std::uintptr_t sm = 0;
    Apd a;
    if (!ApdFnsOk() || !ModelAndApd(reinterpret_cast<std::uintptr_t>(x.p.sp.e), &sm, &a)) return x.parts_ok = false;
    if (!ExoTree(x, a)) {
        if (!x.tree_logged) {
            x.tree_logged = true;
            Log("[%s] carrier %p: %d ArtParts, not the porter suit's tree: no skeleton shown on it this session", ExoTag(s), static_cast<void*>(x.p.sp.e), a.n);
        }
        return x.parts_ok = false;
    }
    std::uint64_t touch[2], asked[2];
    exo::CarrierPlan(a.n, x.parent, vis, g_exo_leg, x.hood_part, touch, asked);
    const bool plan_changed = asked[0] != x.asked[0] || asked[1] != x.asked[1];
    unsigned set = 0;
    for (int i = 0; i < a.n; ++i) {
        const unsigned u = static_cast<unsigned>(i);
        if (!exo::Bit(touch, u)) continue;
        std::uint8_t fl = 0;
        if (!PartFlags(a, u, &fl)) return x.parts_ok = false;
        const bool want = exo::Bit(asked, u);
        if (((fl & 1) != 0) == want) continue;
        if (!CallSetParts(sm, a.apd, i, want)) {
            Log("[%s] the ArtParts part setter faulted on the carrier: parts off for this session", ExoTag(s));
            return x.parts_ok = false;
        }
        ++set;
    }
    if (set && !plan_changed && x.parts_ok) ++x.game_changes;
    x.asked[0] = asked[0], x.asked[1] = asked[1];
    x.parts_set += set;
    // what it shows now (the parts' shown bits), logged when it changes
    std::uint64_t shown[2] = {};
    for (int i = 0; i < a.n; ++i) {
        std::uint8_t fl = 0;
        if (PartFlags(a, static_cast<unsigned>(i), &fl) && (fl & 2)) exo::SetBit(shown, static_cast<unsigned>(i), true);
    }
    if (shown[0] != x.shown_bits[0] || shown[1] != x.shown_bits[1]) {
        x.shown_bits[0] = shown[0], x.shown_bits[1] = shown[1];
        char list[200] = "";
        std::size_t at = 0;
        for (unsigned i = 0; i < static_cast<unsigned>(a.n) && at + 6 < sizeof(list); ++i)
            if (exo::Bit(shown, i)) at += static_cast<std::size_t>(std::snprintf(list + at, sizeof(list) - at, "%s%u", at ? " " : "", i));
        const int type = exo::TypeOfParts(vis);
        Log("[%s] carrier %p parts: %u set (%u in all, %u changed by the game); shown now: %s (the sender's skeleton: %s%s; hood part %d)", ExoTag(s),
            static_cast<void*>(x.p.sp.e), set, x.parts_set, x.game_changes, at ? list : "none", type >= 0 ? exo::TypeName(static_cast<exo::Type>(type)) : "pieces of no known type",
            g_exo_leg ? ", LegOut on" : "", x.hood_part);
    }
    return x.parts_ok = true;
}

// ---- Her rain hood on the exo carrier (round 11, docs/real-partner-body.md section 37). The carrier has every hood mesh of
// the porter suit: it shows the sender's hood type when the sender's Sam has his hood up (exo_parts.hpp CarrierPlan's
// `hood_part`), on both screens (the self slot from our own reading, the partner slot from the STC1 v5 hood bytes). Its
// hood parts are looked up on the carrier's own ArtParts resource the way the game's controller does (0x14268C100: the
// type's three name hashes through the resource's part map, 0x1422B69B0 / 0x1419756A0), read only. The unfold
// (fragile_hood=2): the game's own Apd calls mirrored on the carrier (exo_parts.hpp HoodAnimTick: the slot on
// 0x1422B46D0, the opening / open loop / closing / close loop clips 0x1422B4830 from our own Sam's clip table, the slot's
// "done" read as 0x1422C8110 reads it), only when the carrier's Apd has an animation object with the type's slot; else
// (and with fragile_hood=1) the timed swap: the raised hood shown while the sender's is up. Every site byte-checked once;
// a mismatch turns the hood (or only the unfold) off with one log line; a fault in a call turns the unfold off.
struct HoodLook {
    int ok = -1;                                // -1 not checked, 0 mismatch (no hood on the carrier), 1 ok
    std::uintptr_t hash[exo::kHoodTypes][3] = {};  // the controller's name hashes: {on, off, switch} per type
    int anim = -1;                              // the unfold's sites: -1 not checked, 0 mismatch (the timed swap), 1 ok
    bool faulted = false;                       // a call faulted: the timed swap for the session
    std::uintptr_t clip_tbl = 0;                // our Sam's clip table last checked
    bool clips_ok = false;
    unsigned clip_logs = 0;
} g_hlook;

bool HoodLookupOk() {
    if (g_hlook.ok >= 0) return g_hlook.ok == 1;
    const std::uintptr_t b = mem::Base();
    // The type dispatch (1 the oxi's branch, 2 the sea otter's, else the porter's) and each branch's three name-hash addresses
    // (on, off, switch), then the three lookups into the controller's k4 / k5 / k6 map entries (+0x58 / +0x60 / +0x68).
    static const std::uintptr_t kLea[exo::kHoodTypes][3] = {{0x268C15B, 0x268C169, 0x268C177}, {0x268C33E, 0x268C34E, 0x268C35E}, {0x268C266, 0x268C274, 0x268C282}};
    bool ok = mem::CodeMatches(b + 0x268C12B, "33 F6 44 0F B6 EA 45 8B C5 4C 8B E1 8B FE 41 83 E8 01 0F 84 F5 01 00 00 33 D2 33 C9 41 83 F8 01 44 8D 46 0C 0F 84 0B 01 00 00") &&
              mem::CodeMatches(b + 0x268C44F, "49 8B 04 24 49 8D 54 24 54 48 8B A8 80 01 00 00 48 8B 05 ?? ?? ?? ?? 8B 08 89 0A 48 8B 4D 30 E8 ?? ?? ?? ?? "
                                              "41 89 44 24 58") &&
              mem::CodeMatches(b + 0x268C478, "49 8D 54 24 5C 48 8B 05 ?? ?? ?? ?? 8B 08 89 0A 48 8B 4D 30 E8 ?? ?? ?? ?? 41 89 44 24 60 "
                                              "49 8D 54 24 64 48 8B 05 ?? ?? ?? ?? 8B 08 89 0A 48 8B 4D 30 E8 ?? ?? ?? ?? 41 89 44 24 68") &&
              mem::CodeMatches(b + 0x22B69B0, "40 53 48 83 EC 20 8B 02 48 8D 99 E0 00 00 00 48 8B CB 89 44 24 30 48 8D 54 24 30 E8 ?? ?? ?? ?? 83 F8 FF 74 13 48 98 48 8D 0C 40 48 8B 03 8B 44 88 08") &&
              mem::CodeMatches(b + 0x19756AA, "4C 8B 01 4D 85 C0 0F 84 B6 00 00 00 44 8B 51 0C 33 DB 8B 3A 41 FF CA F2 0F 38 F1 DF 81 CB 00 00 00 80");
    // (each LEA is followed by its store to the pointer the lookups read; the oxi's second by an xor first)
    static const char* const kLeaSig[exo::kHoodTypes][3] = {{"48 8D 05 ?? ?? ?? ?? 48 89 05", "48 8D 05 ?? ?? ?? ?? 48 89 05", "48 8D 05 ?? ?? ?? ?? 48 89 05"},
                                                            {"48 8D 05 ?? ?? ?? ?? 48 89 05", "48 8D 05 ?? ?? ?? ?? 33 C9 48 89 05", "48 8D 05 ?? ?? ?? ?? 48 89 05"},
                                                            {"48 8D 05 ?? ?? ?? ?? 48 89 05", "48 8D 05 ?? ?? ?? ?? 48 89 05", "48 8D 05 ?? ?? ?? ?? 48 89 05"}};
    for (int t = 0; ok && t < exo::kHoodTypes; ++t)
        for (int w = 0; ok && w < 3; ++w) {
            ok = mem::CodeMatches(b + kLea[t][w], kLeaSig[t][w]);
            g_hlook.hash[t][w] = ok ? mem::RipTarget(b + kLea[t][w], 3, 7) : 0;
            std::uint32_t h = 0;
            ok = ok && g_hlook.hash[t][w] && Rd(g_hlook.hash[t][w], h) && h != 0;
        }
    g_hlook.ok = ok ? 1 : 0;
    Log("[hood] the controller's hood part lookup (0x14268C100) %s", ok ? "verified: her hood is drawn on the exo carrier"
                                                                        : "does not match this game build: her hood is not drawn");
    return ok;
}

bool HoodAnimSitesOk() {
    if (g_hlook.anim < 0) {
        const std::uintptr_t b = mem::Base();
        const bool ok =
            mem::CodeMatches(b + 0x22B46D0, "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 41 0F B6 F8 48 8B 49 68 48 85 C9 74 38 E8 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 74 2B "
                                            "8B C8 B8 01 00 00 00 D3 E0 48 8B 4B 68 40 84 FF 74 13 F7 D0 21 81 B4 00 00 00") &&
            mem::CodeMatches(b + 0x22B4830, "48 89 5C 24 08 57 48 83 EC 40 48 8B D9 C5 F8 29 74 24 30 48 8B 49 68 49 8B F8 C5 F8 28 F3 48 85 C9 74 3E "
                                            "E8 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 74 31 C5 FA 10 44 24 70 48 8B 4B 68 4C 8B C7 C5 F8 28 DE 8B D0 C5 FA 11 44 24 20") &&
            mem::CodeMatches(b + 0x22C80B0, "80 FA 09 74 0B 0F B6 C2 8B 84 81 90 00 00 00 C3 B8 FF FF FF FF C3") &&
            mem::CodeMatches(b + 0x22C8110, "83 FA FF 74 21 4C 8B 81 88 00 00 00 48 63 C2 48 69 C8 10 01 00 00 42 F7 84 01 88 00 00 00 FD FF FF FF 0F 94 C0 C3") &&
            mem::CodeMatches(b + 0x22C85C0, "83 FA FF 0F 84 58 01 00 00 4C 8B DC 55 57 48 81 EC B8 00 00 00") &&
            mem::CodeMatches(b + 0x2812E3C, "48 63 8E 90 4A 00 00 48 8B 46 30 0F B6 96 89 4A 00 00 C5 F8 28 DE C5 FA 11 74 24 20 4C 8B 80 98 03 00 00");
        g_hlook.anim = ok ? 1 : 0;
        Log("[hood] the hood's unfold calls (0x1422B46D0 / 0x1422B4830, the clip table) %s", ok ? "verified: the unfold can be mirrored on the carrier"
                                                                                               : "do not match this game build: the timed swap");
    }
    return g_hlook.anim == 1 && !g_hlook.faulted;
}

// Our own Sam's hood clip table (inventory +0x30 -> +0x398, 16 clips: porter 0-7, oxi 8-11, sea otter 12-15): all present and of
// one type, checked once per table. The receiver's own game data: the same clips as the sender's.
std::uintptr_t HoodClipTable() {
    game::Entity* local = nullptr;
    if (!game::TryGetLocalPlayer(&local) || !local) return 0;
    const auto lp = reinterpret_cast<std::uintptr_t>(local);
    if (Q(lp) != mem::Base() + kPlayerVtable) return 0;
    const std::uintptr_t inv = Q(lp + kPInv);
    if (!inv || Q(inv + kCompOwner) != lp) return 0;
    const std::uintptr_t tbl = Q(Q(inv + 0x30) + 0x398);
    if (!tbl) return 0;
    if (tbl != g_hlook.clip_tbl) {
        g_hlook.clip_tbl = tbl;
        const std::uintptr_t c0 = Q(tbl), vt0 = c0 ? Q(c0) : 0;
        bool ok = vt0 != 0;
        for (unsigned i = 1; ok && i < 16; ++i) {
            const std::uintptr_t c = Q(tbl + 8ull * i);
            ok = c && Q(c) == vt0;
        }
        const char* tn = ok ? mem::RttiName(mem::RttiOf(c0)) : nullptr;
        g_hlook.clips_ok = ok && tn;
        if (g_hlook.clip_logs++ < 10)
            Log("[hood] our Sam's hood clips %p: %s%s", reinterpret_cast<void*>(tbl), g_hlook.clips_ok ? "16 clips of type " : "not 16 clips of one type: the timed swap",
                g_hlook.clips_ok ? tn : "");
    }
    return g_hlook.clips_ok ? tbl : 0;
}

using HoodSlotFn = void (*)(std::uintptr_t, unsigned char, char);
using HoodPlayFn = int (*)(std::uintptr_t, unsigned char, std::uintptr_t, float, float);
bool CallHoodSlot(std::uintptr_t apd, int slot, bool on) {
    __try {
        reinterpret_cast<HoodSlotFn>(mem::Base() + 0x22B46D0)(apd, static_cast<unsigned char>(slot), on ? 1 : 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_hlook.faulted = true;
        return false;
    }
}
bool CallHoodPlay(std::uintptr_t apd, int slot, std::uintptr_t clip) {
    __try {
        reinterpret_cast<HoodPlayFn>(mem::Base() + 0x22B4830)(apd, static_cast<unsigned char>(slot), clip, 0.0f, 0.0f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_hlook.faulted = true;
        return false;
    }
}
// The slot's layer (as 0x1422C80B0: [anim +0x90 + 4 slot], -1 none) and whether its clip is done (as 0x1422C8110).
int HoodLayer(std::uintptr_t apd, int slot) {
    const std::uintptr_t anim = Q(apd + 0x68);
    std::int32_t layer = -1;
    if (!anim || slot < 0 || slot >= 9 || !Q(anim + 0x88) || !Rd(anim + 0x90 + 4ull * static_cast<unsigned>(slot), layer)) return -1;
    return layer >= 0 && layer < 64 ? layer : -1;
}
bool HoodSlotDone(std::uintptr_t apd, int slot) {
    const int layer = HoodLayer(apd, slot);
    std::uint32_t fl = 0;
    return layer >= 0 && Rd(Q(Q(apd + 0x68) + 0x88) + 0x110ull * static_cast<unsigned>(layer) + 0x88, fl) && (fl & 0xFFFFFFFDu) == 0;
}

// The slot's hood reading: the state (0 none) and type (-1 none), our own for the self slot, the sender's bytes for the
// partner slot. False: no reading (or the hood is off).
bool SlotHood(const PropSet& s, int* state, int* type, bool* alt) {
    *state = 0, *type = -1, *alt = false;
    if (g_hood_mode == 0) return false;
    if (s.slot == realbody::GearSlot::self) {
        if (!g_hoodr.valid) return false;
        *state = g_hoodr.state, *type = g_hoodr.type, *alt = g_hoodr.alt;
        return true;
    }
    if (!s.have_want || !(s.want.hood & wire::kGearHoodRead)) return false;
    *state = s.want.hood & 0x7F;
    *type = s.want.hood_info & 0x03;
    *alt = (s.want.hood_info & wire::kGearHoodAltClips) != 0;
    return *type < exo::kHoodTypes;
}
// The carrier is wanted for the hood: the hood is anything but down (state 1 and 2 count: the carrier loads while the game
// readies the hood, before it unfolds), or it still shows / unfolds.
bool HoodWanted(const PropSet& s) {
    int st = 0, ty = -1;
    bool alt = false;
    return HoodLookupOk() && SlotHood(s, &st, &ty, &alt) && (st != 0 || s.exo.hood.show != exo::HoodShow::none || s.exo.hood.mode != 0);
}

// The carrier's hood parts (its own resource's part map, the controller's hashes) and its animation slots, once per carrier.
void HoodLookupOn(PropSet& s, const Apd& a, double now) {
    PropSet::Exo& x = s.exo;
    const auto e = reinterpret_cast<std::uintptr_t>(x.p.sp.e);
    if (x.hood_e != e) {
        x.hood_e = e;
        x.anim_ok = -1;
        for (auto& t : x.hood_idx)
            for (auto& w : t) w = -1;
        const std::uintptr_t res = Q(a.apd + 0x30), map = res ? Q(res + 0xE0) : 0;
        std::uint32_t cap = 0;
        if (map && Rd(res + 0xEC, cap) && cap > 0 && cap <= 4096 && (cap & (cap - 1)) == 0) {
            for (std::uint32_t i = 0; i < cap; ++i) {
                std::uint32_t ent[3] = {};
                if (!mem::Read(map + 12ull * i, ent, sizeof(ent)) || ent[0] == 0) continue;
                for (int t = 0; t < exo::kHoodTypes; ++t)
                    for (int w = 0; w < 3; ++w) {
                        std::uint32_t h = 0;
                        if (Rd(g_hlook.hash[t][w], h) && ent[1] == h && static_cast<std::int32_t>(ent[2]) >= 0 && static_cast<std::int32_t>(ent[2]) < a.n)
                            x.hood_idx[t][w] = static_cast<std::int16_t>(ent[2]);
                    }
            }
        }
        // a part the plan cannot show (not under the head) is no part
        for (auto& t : x.hood_idx)
            for (auto& w : t)
                if (w >= 0 && !(x.tree_ok && exo::HoodPartOk(a.n, x.parent, w))) w = -1;
        Log("[%s] carrier %p hood parts (on/off/switch): porter %d/%d/%d, oxi %d/%d/%d, sea otter %d/%d/%d (part_sam_body_suiti: 54/53/68, 71/76/84, "
            "69/70/83)", ExoTag(s), reinterpret_cast<void*>(e), x.hood_idx[0][0], x.hood_idx[0][1], x.hood_idx[0][2], x.hood_idx[1][0], x.hood_idx[1][1],
            x.hood_idx[1][2], x.hood_idx[2][0], x.hood_idx[2][1], x.hood_idx[2][2]);
    }
    if (x.anim_ok <= 0 && now >= x.anim_check_ms && x.anim_checks < 6) {
        x.anim_check_ms = now + 5000;
        ++x.anim_checks;
        int ok = 0;
        for (int t = 0; t < exo::kHoodTypes; ++t)
            if (HoodLayer(a.apd, exo::HoodAnimSlot(t)) >= 0) ok |= 1 << t;
        if (ok != x.anim_ok)
            Log("[%s] carrier %p: hood animation slots (porter 2, oxi 5, sea otter 4): %s%s", ExoTag(s), reinterpret_cast<void*>(e),
                ok == 7 ? "ok" : ok ? "some" : "missing", ok == 7 ? "" : ok ? " (the others: the timed swap)" : ": the timed swap");
        x.anim_ok = ok;
    }
}

// Every tick before the parts: what the carrier shows of the hood (HoodAnimTick). Returns the unfold's calls to make after
// the parts are set (HoodCalls).
unsigned HoodDrive(PropSet& s, double now) {
    PropSet::Exo& x = s.exo;
    int st = 0, type = -1;
    bool alt = false;
    const bool have = x.p.live && HoodLookupOk() && SlotHood(s, &st, &type, &alt);
    std::uintptr_t sm = 0;
    Apd a;
    const bool apd = have && x.tree_ok && ModelAndApd(reinterpret_cast<std::uintptr_t>(x.p.sp.e), &sm, &a);
    if (apd) HoodLookupOn(s, a, now);
    if (!have || !apd || type < 0 || x.hood_idx[type][0] < 0) type = -1;
    const exo::HoodPhase ph = exo::HoodPhaseOf(st);
    const bool anim = g_hood_mode == 2 && type >= 0 && x.target && x.hood_idx[type][2] >= 0 && x.anim_ok > 0 && ((x.anim_ok >> type) & 1) &&
                      HoodAnimSitesOk() && HoodClipTable() != 0;
    const bool done = anim && x.hood.mode != 0 && HoodSlotDone(a.apd, x.anim_slot);
    const exo::HoodShow before = x.hood.show;
    const unsigned calls = exo::HoodAnimTick(x.hood, type, ph, anim, done, now);
    if (calls & (exo::kHoodEnable)) x.anim_slot_next = exo::HoodAnimSlot(type);
    x.anim_type = type, x.anim_alt = alt;
    const int part = type < 0 ? -1 : x.hood.show == exo::HoodShow::on ? x.hood_idx[type][0] : x.hood.show == exo::HoodShow::sw ? x.hood_idx[type][2] : -1;
    const int part_before = x.hood_part;
    if (part != x.hood_part) {
        x.hood_part = part;
        x.next_parts_ms = 0;  // the parts at once
    }
    if ((x.hood.show != before || part != part_before) && x.hood_logs < 200) {
        ++x.hood_logs;
        Log("[%s] carrier %p hood: %s (part %d; the sender's %s hood %s, state %d; %s)", ExoTag(s), static_cast<void*>(x.p.sp.e), exo::HoodShowName(x.hood.show),
            part, exo::HoodTypeName(type), exo::HoodPhaseName(ph), st, anim ? "the unfold" : g_hood_mode == 2 ? "the timed swap: no unfold on this carrier" : "the timed swap");
    }
    return calls;
}

// The unfold's calls after the parts (the slot off first, then in HoodCall order).
// No call is made with a slot other than the three hood slots, and none at all after a fault (not even the disable the
// state machine asks for when the unfold turns off); an enable that could not be made starts the unfold again.
void HoodCalls(PropSet& s, unsigned calls, double now) {
    PropSet::Exo& x = s.exo;
    if (!calls || !x.p.live) return;
    if (g_hlook.faulted) {
        x.anim_slot = -1;
        return;
    }
    std::uintptr_t sm = 0;
    Apd a;
    if (!ModelAndApd(reinterpret_cast<std::uintptr_t>(x.p.sp.e), &sm, &a)) {
        if (calls & exo::kHoodEnable) x.hood = exo::HoodAnim{};  // the slot was not turned on: nothing may be played on it
        return;
    }
    bool ok = true;
    if ((calls & exo::kHoodDisable) && exo::HoodSlotValid(x.anim_slot)) ok = CallHoodSlot(a.apd, x.anim_slot, false);
    if (calls & exo::kHoodDisable) x.anim_slot = -1;
    if (ok && (calls & exo::kHoodEnable)) {
        if (!exo::HoodSlotValid(x.anim_slot_next)) {
            Log("[%s] carrier %p hood: slot %d is not a hood slot: no unfold call", ExoTag(s), static_cast<void*>(x.p.sp.e), x.anim_slot_next);
            x.hood = exo::HoodAnim{};
            return;
        }
        x.anim_slot = x.anim_slot_next;
        ok = CallHoodSlot(a.apd, x.anim_slot, true);
    }
    const bool slot_on = exo::HoodSlotValid(x.anim_slot);  // no slot turned on: nothing is played
    static const struct {
        unsigned bit;
        int clip;
    } kPlays[] = {{exo::kHoodPlayOpen, exo::kHoodClipOpen}, {exo::kHoodPlayOpenLoop, exo::kHoodClipOpenLoop}, {exo::kHoodPlayClose, exo::kHoodClipClose},
                  {exo::kHoodPlayCloseLoop, exo::kHoodClipCloseLoop}};
    for (const auto& pl : kPlays) {
        if (!ok || !slot_on || !(calls & pl.bit)) continue;
        const std::uintptr_t tbl = HoodClipTable();
        const int idx = exo::HoodClipIndex(x.anim_type, x.anim_alt, pl.clip);
        const std::uintptr_t clip = tbl && idx >= 0 ? Q(tbl + 8ull * static_cast<unsigned>(idx)) : 0;
        if (!clip) continue;  // (the slot's 3 s limit ends it)
        ok = CallHoodPlay(a.apd, x.anim_slot, clip);
        if (ok && x.hood_logs < 200) {
            ++x.hood_logs;
            Log("[%s] carrier %p hood: clip %d played on slot %d (%.0f ms into the %s)", ExoTag(s), static_cast<void*>(x.p.sp.e), idx, x.anim_slot,
                now - x.hood.since_ms, x.hood.mode == 2 ? "lowering" : "raising");
        }
    }
    ++x.hood_calls;
    if (!ok) {
        ++x.hood_faults;
        Log("[%s] the hood's unfold call FAULTED on carrier %p: the timed swap for this session", ExoTag(s), static_cast<void*>(x.p.sp.e));
    }
}

// Her hair under her hood (hood_hair, default on): her body's hair parts (named "...hair...": fgl_hair_def) hidden while the
// carrier draws her hood, shown again after. Only the parts we hid are shown again; a new body starts afresh.
std::string ObjName(std::uintptr_t o);
// A name containing `want` (lower case) on a part's sub-model resource (+0x20) or SkinnedModel (+0x60), or on the objects
// they point to (+0x10..+0x100: its mesh resources): the sub-model resources carry no name (census of run
// 20261122-170859). *name: the match; *first: the first name seen (for the log).
bool PartNamed(std::uintptr_t part, const char* want, std::string* name, std::string* first = nullptr) {
    auto has = [want](const std::string& s) {
        std::string low = s;
        for (char& c : low) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        return !s.empty() && low.find(want) != std::string::npos;
    };
    for (unsigned off : {0x20u, 0x60u}) {  // its sub-model resource, its SkinnedModel (census: +0x20, +0x60)
        const std::uintptr_t q = part ? Q(part + off) : 0;
        if (!q || !mem::RttiOf(q)) continue;
        std::string nm = ObjName(q);
        if (first && first->empty() && !nm.empty()) *first = nm;
        if (has(nm)) return *name = nm, true;
        for (unsigned o2 = 0x10; o2 < 0x100; o2 += 8) {
            const std::uintptr_t r = Q(q + o2);
            if (!r || r == part || !mem::RttiOf(r)) continue;
            nm = ObjName(r);
            if (first && first->empty() && !nm.empty()) *first = nm;
            if (has(nm)) return *name = nm, true;
        }
    }
    return false;
}
void HairTick(PropSet& s, game::Entity* body, double now) {
    PropSet::Exo& x = s.exo;
    const auto e = reinterpret_cast<std::uintptr_t>(body);
    if (e != x.hair_body) {
        x.hair_body = e;
        x.hair_n = 0;
        x.hair_mask = 0;
        x.hair_hidden = false;
        x.hair_checked = false;
    }
    if (!e || !ApdFnsOk()) return;
    std::uintptr_t sm = 0;
    Apd a;
    if (!x.hair_checked) {
        if (!ModelAndApd(e, &sm, &a)) return;
        x.hair_checked = true;
        char names[320] = "";
        std::size_t at = 0;
        for (int i = 0; i < a.n && x.hair_n < 4; ++i) {
            const std::uintptr_t p = Q(a.arr + 8ull * static_cast<unsigned>(i));
            std::string nm, first;
            if (!PartNamed(p, "hair", &nm, &first)) {
                if (at + 40 < sizeof(names))
                    at += static_cast<std::size_t>(std::snprintf(names + at, sizeof(names) - at, "%s%d '%.24s'", at ? ", " : "", i, first.c_str()));
                continue;
            }
            x.hair_parts[x.hair_n++] = static_cast<std::int16_t>(i);
            if (at + 40 < sizeof(names)) at += static_cast<std::size_t>(std::snprintf(names + at, sizeof(names) - at, "%s%d '%s'", at ? ", " : "", i, nm.c_str()));
        }
        Log("[%s] her body %p: %s%s", ExoTag(s), reinterpret_cast<void*>(e), x.hair_n ? "hair parts under her hood (with the other parts): " : "no hair part found (her hair stays under the hood); parts: ",
            names);
    }
    const bool want = g_hood_hair && x.p.live && x.p.shown && x.hood_part >= 0 && x.parts_ok;
    if (want == x.hair_hidden || x.hair_n == 0 || now < x.hair_next_ms) return;
    x.hair_next_ms = now + 100;
    if (!ModelAndApd(e, &sm, &a)) return;
    unsigned changed = 0;
    for (unsigned k = 0; k < x.hair_n; ++k) {
        std::uint8_t fl = 0;
        if (!PartFlags(a, static_cast<unsigned>(x.hair_parts[k]), &fl)) return;
        if (want ? !(fl & 1) : !((x.hair_mask >> k) & 1u)) continue;  // hidden already, or not hidden by us: left as it is
        if (!CallSetParts(sm, a.apd, x.hair_parts[k], !want)) {
            Log("[%s] the ArtParts part setter faulted on her body: her hair stays as it is", ExoTag(s));
            x.hair_n = 0;
            return;
        }
        x.hair_mask = want ? x.hair_mask | (1u << k) : x.hair_mask & ~(1u << k);
        ++changed;
    }
    x.hair_hidden = want;
    if (x.hair_logs < 100) {
        ++x.hair_logs;
        Log("[%s] her hair %s (%u part(s))", ExoTag(s), want ? "hidden under her hood" : "shown again", changed);
    }
}

// Her hood is drawn up on her (her hat hides under it, section 26.8): our reading up, the hood and the carrier on, our
// own carrier not refused.
bool OwnHoodDrawnUp() {
    return g_hoodr.valid && g_hood_mode != 0 && g_exo_on && !g_sets[1].exo.refused && HoodLookupOk() && exo::HoodHidesHat(exo::HoodPhaseOf(g_hoodr.state));
}

// Spawns, polls, poses, sets and tints the carrier (game thread, from DriveSet before it publishes). True: the published list
// must change (the carrier became the pose target, or went).
bool ExoDrive(PropSet& s, game::Entity* body, double now) {
    PropSet::Exo& x = s.exo;
    std::uint64_t vis[2];
    const bool want = ExoWant(s, body, vis);
    x.want = want;
    if (want) {
        x.vis[0] = vis[0], x.vis[1] = vis[1];
        x.unwanted_ms = 0;
    } else if (x.p.loading || x.p.live) {
        if (x.unwanted_ms == 0) x.unwanted_ms = now;
        if (!g_exo_on || now - x.unwanted_ms >= 10000) {
            RemoveExo(s, g_exo_on ? "no skeleton or hood for 10 s" : "exo_carrier off");
            return true;
        }
    }
    bool changed = false;
    Prop& p = x.p;
    if (want && !p.loading && !p.live && !x.refused && !(p.failed && now < p.retry_ms)) {
        game::WorldTransform at{};
        char why[200] = {};
        if (game::TryGetTransform(body, &at)) {
            at.pos.z -= 20.0;  // out of sight until the first placement
            if (realbody::BeginByPath(kExoPath, kExoGuid, at, 0x6E400000u + (s.slot == realbody::GearSlot::self ? 1u : 0u), &p.sp, why, sizeof(why))) {
                p.loading = true;
                p.failed = false;
                Log("[%s] carrier: loading %s (the sender's skeleton: %s)", ExoTag(s), kExoPath,
                    exo::TypeOfParts(vis) >= 0 ? exo::TypeName(static_cast<exo::Type>(exo::TypeOfParts(vis))) : "pieces of no known type");
            } else {
                p.failed = true;
                p.retry_ms = now + wire::GearRetryDelayMs(++p.fails);
                Log("[%s] carrier not started (try %u): %s", ExoTag(s), p.fails, why);
            }
        }
    }
    if (p.loading) {
        char why[200] = {};
        switch (holo::SpawnPoll(p.sp, why, sizeof(why))) {
            case holo::SpawnState::loading: break;
            case holo::SpawnState::cancelled:
            case holo::SpawnState::failed: {
                const unsigned fails = p.fails + 1;
                RemoveExo(s, "load failed");
                p.failed = true;
                p.fails = fails;
                p.retry_ms = now + wire::GearRetryDelayMs(fails);
                Log("[%s] carrier did not load (try %u): %s", ExoTag(s), fails, why);
                break;
            }
            case holo::SpawnState::realized: {
                p.loading = false;
                char bad[160] = "no entity";
                if (!p.sp.e || !ValidateProp(p.sp.e, false, bad, sizeof(bad)) || !vsync::CopyCollisionOff(p.sp.e, "exo carrier")) {
                    Log("[%s] carrier refused for this session: %s", ExoTag(s), p.sp.e ? bad : "no entity");
                    if (p.sp.e) partner::DescribeEntity(p.sp.e, "refused exo carrier");
                    RemoveExo(s, "refused");
                    x.refused = true;
                    break;
                }
                partner::SetVisible(p.sp.e, false);
                partner::WakeIfAsleep(p.sp.e);
                partner::ReadVtable(p.sp.e, &p.vt);
                partner::ReadGuid(p.sp.e, &p.guid);
                p.live = true;
                p.shown = false;
                p.fails = 0;
                p.live_ms = now;
                p.coll_next_ms = now + 1000;
                ++x.spawned;
                static bool described = false;
                if (!described) {
                    described = true;
                    partner::DescribeEntity(p.sp.e, "exo carrier");
                }
                Log("[%s] carrier %p live", ExoTag(s), static_cast<void*>(p.sp.e));
                break;
            }
        }
    }
    if (p.live && !holo::SpawnAlive(p.sp)) {
        RemoveExo(s, "destroyed by the game");
        x.p.failed = true;
        x.p.retry_ms = now + 5000;
        return true;
    }
    if (!p.live) return changed;
    if (now >= p.coll_next_ms) {
        p.coll_next_ms = now + 1000;
        if (vsync::CopyCollisionKeepOff(p.sp.e, "exo carrier") < 0) {
            RemoveExo(s, "collision not off");
            x.refused = true;
            return true;
        }
    }
    // the pose target (retried every 500 ms while the body is not ready)
    if (!x.target && now >= x.target_retry_ms) {
        char why[200] = {};
        if (realbody::SetExoCarrier(s.slot, &p.sp, why, sizeof(why))) {
            x.target = true;
            changed = true;
            Log("[%s] carrier %p follows the %s body's pose", ExoTag(s), static_cast<void*>(p.sp.e), s.slot == realbody::GearSlot::self ? "self" : "partner");
        } else {
            x.target_retry_ms = now + 500;
            if (std::strcmp(why, x.target_why) != 0) {
                std::snprintf(x.target_why, sizeof(x.target_why), "%s", why);
                Log("[%s] carrier %p is not posed yet: %s", ExoTag(s), static_cast<void*>(p.sp.e), why);
            }
        }
    } else if (x.target && realbody::ExoCarrierEntity(s.slot) != reinterpret_cast<std::uintptr_t>(p.sp.e)) {
        x.target = false;  // cleared by real_body (a fault, the session's stop): out of the list
        changed = true;
    }
    const unsigned hood_calls = HoodDrive(s, now);  // section 37: the hood part first (the parts at once when it changes)
    if (want || x.parts_ok) ExoParts(s, want ? vis : x.vis, now);
    HoodCalls(s, hood_calls, now);
    // the exos variation (silver / gold, tints) like the sender's Sam
    game::Entity* local = nullptr;
    wire::Outfit o;
    bool have_o = false;
    if (game::TryGetLocalPlayer(&local) && local) {
        if (s.slot == realbody::GearSlot::self || realbody::GetOwner() == realbody::Owner::debug) have_o = outfit::ReadLocal(reinterpret_cast<std::uintptr_t>(local), &o);
        else have_o = realbody::PartnerOutfit(&o);
        if (have_o && now - p.live_ms >= 500) outfit::Apply(x.dress, reinterpret_cast<std::uintptr_t>(p.sp.e), o, reinterpret_cast<std::uintptr_t>(local), now);
    }
    x.tint_want = have_o ? wire::PorterExos(o) : std::uint8_t{0xFE};
    return changed;
}

// Shown with the body only when everything is ready; the meter and the counters.
constexpr double kExoTintWaitMs = 2000;
void ExoShow(PropSet& s, game::Entity* body, bool body_on, double now) {
    PropSet::Exo& x = s.exo;
    Prop& p = x.p;
    if (!p.live || !holo::SpawnAlive(p.sp)) {
        realbody::SetExoPoseWanted(s.slot, false);
        return;
    }
    // Posed by forced model updates (the game does not update it by itself) only while it is meant to be seen: never while
    // hidden for a load, the self view's unshow or the 10 s grace after unequipping (round 7 review: 293 forced updates in
    // the 1.3 s reload of run 20261122-193826 while it was hidden).
    const bool ready = body_on && x.want && x.target && x.parts_ok;
    realbody::SetExoPoseWanted(s.slot, ready);
    // The first show waits for the tint (the exos variation like the sender's: silver / gold); else the plain demo_sam_clean
    // skeleton showed for 0.5-1.2 s first (runs 20261122-193625, -191730). A receiver whose own Sam is not in the porter suit
    // has no variation to copy: shown as it is after kExoTintWaitMs.
    if (!x.tint_ok) {
        const bool tinted = x.dress.e == reinterpret_cast<std::uintptr_t>(p.sp.e) && x.dress.v01_done && x.tint_want != 0xFE &&
                            x.dress.exos == x.tint_want;
        if (tinted || now - p.live_ms >= kExoTintWaitMs) {
            x.tint_ok = true;
            if (!tinted)
                Log("[%s] carrier %p: no tint after %.0f ms (exos wanted %u, on it %u): shown as it is", ExoTag(s), static_cast<void*>(p.sp.e),
                    now - p.live_ms, x.tint_want, x.dress.exos);
        }
    }
    const bool on = ready && x.tint_ok && realbody::ExoPosed(s.slot, 250);
    std::uint64_t fl = 0;
    const bool is_on = partner::ReadFlags(p.sp.e, &fl) && (fl & 0x2) != 0;
    if (on != is_on || on != p.shown) {
        if (partner::SetVisible(p.sp.e, on)) {
            if (on != p.shown && x.show_logs < 40) {
                ++x.show_logs;
                Log("[%s] carrier %p: %s (%.0f ms after it became live)", ExoTag(s), static_cast<void*>(p.sp.e), on ? "shown" : "hidden", now - p.live_ms);
            }
            p.shown = on;
        }
    }
    // The meter (cm): the pose, the carrier's joints in its model space against the body's in the body's joint-0 frame (both
    // from their current model matrices: what the carrier draws relative to where it is placed), and the placement, both in
    // world space at this tick (the tick's placement is extrapolated one frame ahead: moving, about one frame's travel).
    if (p.shown && body && now >= x.next_meter_ms) {
        x.next_meter_ms = now + (automation::Enabled() ? 1000 : 10000);
        static const unsigned kMeterJ[] = {1, 62, 63, 67, 68, 9, 38, 31};
        double sum = 0, mx = 0, wsum = 0, wmx = 0, head = -1;  // head: joint 31 (the hood sits on it, section 37)
        unsigned n = 0, wn = 0;
        float r0[4][4];
        const bool have_r0 = realbody::ReadJointModel(body, 0, r0);
        for (unsigned j : kMeterJ) {
            float cm[4][4], bm[4][4];
            if (have_r0 && realbody::ReadJointModel(p.sp.e, j, cm) && realbody::ReadJointModel(body, j, bm)) {
                // the body's joint j in its joint-0 frame (rows: axes, translation)
                double d2 = 0;
                for (int k = 0; k < 3; ++k) {
                    double v = 0;
                    for (int c = 0; c < 3; ++c) v += static_cast<double>(bm[3][c] - r0[3][c]) * r0[k][c];
                    const double e = v - cm[3][k];
                    d2 += e * e;
                }
                const double d = std::sqrt(d2) * 100.0;
                sum += d, mx = d > mx ? d : mx, ++n;
            }
            double a[3], b[3];
            if (!realbody::JointWorldN(p.sp.e, j, 77, a) || !realbody::JointWorld(body, j, b)) continue;
            const double d = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])) * 100.0;
            wsum += d, wmx = d > wmx ? d : wmx, ++wn;
            if (j == 31) head = d;
        }
        char c[200];
        realbody::ExoCounters(s.slot, c, sizeof(c));
        if (n || wn)
            Log("[%s] carrier vs body cm mean/max: pose %.2f / %.2f (%u joints), placed %.2f / %.2f (%u, the tick's), head %.2f; %s; parts set %u, changed by the game %u; hood %s",
                ExoTag(s), n ? sum / n : -1.0, mx, n, wn ? wsum / wn : -1.0, wmx, wn, head, c, x.parts_set, x.game_changes, exo::HoodShowName(x.hood.show));
    }
}

void DriveSet(PropSet& s, game::Entity* body, bool body_visible, bool local_active, double now) {
    if (!local_active) {
        if (s.asleep_since == 0) s.asleep_since = now > 0 ? now : 1;
        for (unsigned i = 0; i < wire::kMaxGearItems; ++i) SetShown(s, i, false, now);
        ExoShow(s, nullptr, false, now);
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
    bool changed = false;
    // A new layout: the props are matched to the new items by what they show (one piece removed must
    // not respawn every later one); props nobody wants any more go (after the detour stopped using the
    // old list). A failed prop keeps its backoff when its item still wants the same key.
    if (h != s.published_hash) {
        int old_keys[wire::kMaxGearItems], want[wire::kMaxGearItems], map[wire::kMaxGearItems];
        for (unsigned i = 0; i < wire::kMaxGearItems; ++i) {
            const Prop& p = s.props[i];
            old_keys[i] = (p.loading || p.live || p.failed) ? p.def : -1;
            want[i] = i < f.count ? PropFor(f.items[i], s.res) : -1;
        }
        wire::GearAssignProps(old_keys, wire::kMaxGearItems, want, f.count, map);
        Unpublish(s);
        static Prop next[wire::kMaxGearItems];
        bool used[wire::kMaxGearItems] = {};
        unsigned kept = 0, moved = 0, gone = 0;
        for (unsigned j = 0; j < wire::kMaxGearItems; ++j) {
            next[j] = Prop{};
            if (j < f.count && map[j] >= 0) {
                next[j] = s.props[map[j]];
                used[map[j]] = true;
                ++kept;
                moved += map[j] != static_cast<int>(j);
            }
        }
        constexpr unsigned kSpares = sizeof(s.spare) / sizeof(s.spare[0]);
        for (unsigned i = 0; i < wire::kMaxGearItems; ++i)
            if (!used[i] && (s.props[i].loading || s.props[i].live || s.props[i].sp.sp)) {
                Prop& op = s.props[i];
                if (IsHandProp(op.def) && op.live && holo::SpawnAlive(op.sp)) {
                    // a weapon prop: kept hidden for the next draw (the oldest spare goes when all are taken)
                    unsigned k = 0;
                    for (unsigned m = 0; m < kSpares; ++m) {
                        if (!s.spare[m].p.live) {
                            k = m;
                            break;
                        }
                        if (s.spare[m].since < s.spare[k].since) k = m;
                    }
                    if (s.spare[k].p.live || s.spare[k].p.sp.sp) RemoveSpare(s, k, "spare replaced");
                    partner::SetVisible(op.sp.e, false);
                    op.shown = false;
                    s.spare[k].p = op;
                    s.spare[k].since = now;
                    ++s.spare_kept;
                    op = Prop{};
                    continue;
                }
                RemoveProp(s, i, "layout changed");
                ++gone;
            }
        // new weapon items take a kept prop of their key
        for (unsigned j = 0; j < f.count; ++j) {
            if (map[j] >= 0 || !IsHandProp(want[j])) continue;
            for (unsigned k = 0; k < kSpares; ++k) {
                PropSet::Spare& sp = s.spare[k];
                if (!sp.p.live || sp.p.def != want[j]) continue;
                if (!holo::SpawnAlive(sp.p.sp)) {
                    RemoveSpare(s, k, "spare destroyed by the game");
                    continue;
                }
                next[j] = sp.p;
                sp = PropSet::Spare{};
                ++s.spare_reused;
                ++kept;
                break;
            }
        }
        s.layout_ms = now;
        for (unsigned j = 0; j < wire::kMaxGearItems; ++j) s.props[j] = next[j];
        s.kept += moved;
        if (s.published_hash != 0xFFFF || gone)
            Log("[%s] layout %04X -> %04X: %u prop(s) kept (%u moved to another item), %u removed", s.tag, s.published_hash, h, kept, moved, gone);
        if (s.published_hash != 0xFFFF && s.slot == realbody::GearSlot::partner) {
            int delta[wire::kMaxGearRes] = {};
            for (unsigned i = 0; i < wire::kMaxGearItems; ++i) {
                if (old_keys[i] >= kKeyEntity && old_keys[i] < kKeyHeld) ++delta[old_keys[i] - kKeyEntity];
                if (i < f.count && want[i] >= kKeyEntity && want[i] < kKeyHeld) --delta[want[i] - kKeyEntity];
            }
            NameResChanges(delta, s.res, "the partner's gear", false);
        }
        s.published_hash = h;
        DescribeFrame(f, s.tag, s.res);
        changed = true;
    }
    for (unsigned k = 0; k < sizeof(s.spare) / sizeof(s.spare[0]); ++k)
        if ((s.spare[k].p.live || s.spare[k].p.sp.sp) && (now - s.spare[k].since > 60000 || !holo::SpawnAlive(s.spare[k].p.sp)))
            RemoveSpare(s, k, now - s.spare[k].since > 60000 ? "spare unused for 60 s" : "spare destroyed by the game");
    // Spawn the missing props (a few per tick), poll the loading ones; failures are retried.
    unsigned begun = 0, loading = 0;
    game::WorldTransform at{};
    const bool have_at = game::TryGetTransform(body, &at);
    at.pos.z -= 20.0;  // out of sight until the first placement
    for (unsigned i = 0; i < f.count; ++i) {
        Prop& p = s.props[i];
        const int want = PropFor(f.items[i], s.res);
        if (want < 0 || want >= kKeyCount || s.refused[want]) continue;
        const char* path = nullptr;
        const std::uint8_t* gid = nullptr;
        const char* label = "?";
        const bool known = KeySource(s, want, &path, &gid, &label);
        if (!p.loading && !p.live && have_at && begun < 4 && known && !(p.failed && now < p.retry_ms)) {
            char why[200] = {};
            std::uint8_t g16[16];
            std::memcpy(g16, gid, 16);
            p.def = want;
            const bool injected = g_fault_spawns > 0;  // hidden tests: `gear 5 <n>` makes the next n starts fail
            if (injected) {
                --g_fault_spawns;
                std::snprintf(why, sizeof(why), "TEST: start failure injected (gear 5)");
            }
            bool started = false;
            if (!injected && IsHeldKey(want)) {
                // a held clone (section 31): made at once from our own game's copy of the item's resource (no spawner)
                started = HeldBegin(path, g16, at, &p.sp, why, sizeof(why));
            } else if (!injected) {
                started = realbody::BeginByPathGuid(path, g16, at, 0x6E000000u + i, &p.sp, why, sizeof(why));
            }
            if (started) {
                p.loading = true;
                p.failed = false;
                ++begun;
                if (p.fails) ++s.retried;
            } else {
                p.failed = true;
                p.retry_ms = now + wire::GearRetryDelayMs(++p.fails);
                ++s.failed;
                if (s.failed <= 30) Log("[%s] prop %u '%s' not started (try %u, again in %.0f s): %s", s.tag, i, label, p.fails, (p.retry_ms - now) / 1000, why);
            }
        }
        if (p.loading) {
            ++loading;
            char why[200] = {};
            switch (p.sp.direct ? holo::SpawnState::realized : holo::SpawnPoll(p.sp, why, sizeof(why))) {
                case holo::SpawnState::loading: break;
                case holo::SpawnState::cancelled:
                case holo::SpawnState::failed: {
                    const unsigned fails = p.fails + 1;
                    RemoveProp(s, i, "load failed");
                    s.props[i].def = want;
                    s.props[i].failed = true;
                    s.props[i].fails = fails;
                    s.props[i].retry_ms = now + wire::GearRetryDelayMs(fails);
                    ++s.failed;
                    if (s.failed <= 30) Log("[%s] prop %u '%s' did not load (try %u, again in %.0f s): %s", s.tag, i, label, fails, wire::GearRetryDelayMs(fails) / 1000, why);
                    break;
                }
                case holo::SpawnState::realized: {
                    p.loading = false;
                    char bad[160] = "no entity";
                    if (!p.sp.e || !ValidateProp(p.sp.e, want >= kKeyEntity, bad, sizeof(bad))) {
                        Log("[%s] prop %u '%s' refused for this session: %s", s.tag, i, label, bad);
                        if (p.sp.e) partner::DescribeEntity(p.sp.e, "refused gear prop");
                        RemoveProp(s, i, "refused");
                        s.props[i].def = want;
                        s.props[i].failed = true;
                        s.refused[want] = true;
                        ++s.failed;
                        break;
                    }
                    partner::SetVisible(p.sp.e, false);
                    partner::WakeIfAsleep(p.sp.e);
                    // Its collision off on this one prop (docs/real-partner-body.md section 22.13): the cutscene case props
                    // carry a physics instance on a model part (case_s, case_m: `physics instances 1 -> 0`), and a prop set
                    // on a moving body every tick must never push or block the local player. Not off: refused.
                    if (!vsync::CopyCollisionOff(p.sp.e, label)) {
                        Log("[%s] prop %u '%s' refused for this session: its collision could not be turned off", s.tag, i, label);
                        RemoveProp(s, i, "collision not off");
                        s.props[i].def = want;
                        s.props[i].failed = true;
                        s.refused[want] = true;
                        ++s.failed;
                        break;
                    }
                    p.coll_next_ms = now + 1000;
                    partner::ReadVtable(p.sp.e, &p.vt);
                    partner::ReadGuid(p.sp.e, &p.guid);
                    p.live = true;
                    p.shown = false;
                    p.fails = 0;
                    p.live_ms = now;
                    p.look_hash = 0;
                    ++s.spawned;
                    changed = true;
                    static bool described[kKeyCount] = {};
                    if (!described[want]) {
                        described[want] = true;
                        partner::DescribeEntity(p.sp.e, label);
                    }
                    break;
                }
            }
        }
        if (p.live && !holo::SpawnAlive(p.sp)) {
            // SpawnTeardown handles a dead entity (the spawner reference only); then the failure backoff,
            // so a resource whose entity removes itself is not respawned every tick.
            const unsigned fails = p.fails + 1;
            Unpublish(s);
            RemoveProp(s, i, "destroyed by the game");
            s.props[i].def = want;
            s.props[i].failed = true;
            s.props[i].fails = fails;
            s.props[i].retry_ms = now + wire::GearRetryDelayMs(fails);
            ++s.failed;
            if (s.failed <= 30) Log("[%s] prop %u '%s' was destroyed by the game: again in %.0f s", s.tag, i, label, wire::GearRetryDelayMs(fails) / 1000);
            changed = true;
        }
        // its collision stays off (every second; a look apply rebuilds its materials): found on again more than 3 times,
        // or not off: refused for the session (section 22.13)
        if (p.live && now >= p.coll_next_ms) {
            p.coll_next_ms = now + 1000;
            const int k = vsync::CopyCollisionKeepOff(p.sp.e, label);
            if (k == 0) ++p.coll_again;
            if (k < 0 || p.coll_again > 3) {
                Log("[%s] prop %u '%s' refused for this session: its collision %s", s.tag, i, label, k < 0 ? "could not be kept off" : "kept coming back");
                Unpublish(s);
                RemoveProp(s, i, "collision not off");
                s.props[i].def = want;
                s.props[i].failed = true;
                s.refused[want] = true;
                ++s.failed;
                changed = true;
            }
        }
    }
    if (&s == &g_sets[0]) {
        ApplyLooks(s, now);
        ApplyPoses(s, now);
    } else if (g_hood.e) {
        ApplyLooks(s, now);  // the self view's hat in the rain: our own cap's colour (WantLook's self branch; section 26.8)
    }
    if (ExoDrive(s, body, now)) changed = true;  // section 32
    // Publish when anything changed (new props live, new poses), and again when a placement fault
    // switched the detour's list off (at most 3 times a session: a fault fires the crash reporter).
    static wire::GearFrame last_pub[2];
    const int si = s.slot == realbody::GearSlot::self ? 1 : 0;
    float dm = 0, dd = 0;
    const bool poses_moved = !wire::GearPoseDelta(last_pub[si], f, &dm, &dd) || dm > 0.0005f || dd > 0.05f;
    bool fault_off = s.published && realbody::GearPublished(s.slot) == 0;
    if (fault_off) {
        ++s.placement_off;
        Log("[%s] the detour's prop list was switched off (a placement fault): %s", s.tag, s.placement_off <= 3 ? "published again" : "left off");
        if (s.placement_off > 3) fault_off = false;
    }
    if (changed || poses_moved || !s.published || fault_off) {
        PublishNow(s);
        last_pub[si] = f;
    }
    // Place now from the tick too (the detour places them every frame the body is updated).
    const unsigned placed = realbody::PlaceGearNow(s.slot);
    if (placed > 0) s.last_placed_ms = now;
    // Visibility: with the body; a tick without a placement (a torn read) does not hide them, 300 ms do.
    const bool place_ok = placed > 0 || (s.last_placed_ms > 0 && now - s.last_placed_ms < 300);
    // v0.23: our Sam on the pillion of the partner's trike: the partner's backpack, what hangs on it and its back cargo stay
    // hidden (the driver's game hides them itself and stops sending them; this covers the frames before that arrives).
    const bool pillion = &s == &g_sets[0] && ride::PillionNow() == ride::Pillion::we_ride;
    if (pillion != s.pillion_hide) {
        s.pillion_hide = pillion;
        Log("[%s] the partner's backpack props %s (our Sam %s the partner's trike pillion)", s.tag, pillion ? "hidden" : "follow its frame again",
            pillion ? "rides on" : "is off");
    }
    for (unsigned i = 0; i < f.count; ++i) {
        bool on = body_visible && place_ok;
        if (on && pillion) {
            for (unsigned k = i, n = 0; k < f.count && n < 8; k = f.items[k].parent, ++n) {
                if (f.items[k].kind == wire::GearKind::backpack) {
                    on = false;
                    break;
                }
                if (f.items[k].parent == wire::kGearNoParent) break;
            }
        }
        // The boots prop (section 21) only once BootsTick made it the pose target, it is posed and the partner's boots parts
        // are mirrored on it: never in the cutscene's own pose or parts (the spawn pop of 12-14 cm the review measured).
        if (on && f.items[i].kind == wire::GearKind::boots) on = BootsPropReady(reinterpret_cast<std::uintptr_t>(s.props[i].sp.e));
        SetShown(s, i, on, now);
    }
    ExoShow(s, body, body_visible && place_ok, now);  // section 32
    HairTick(s, body, now);                           // section 37
    if (now >= s.next_stats_ms && s.published_n) {
        s.next_stats_ms = now + 5000;
        unsigned live = 0, shown = 0, waiting = 0, failed_now = 0, refused = 0;
        for (unsigned i = 0; i < f.count; ++i) {
            const Prop& p = s.props[i];
            live += p.live, shown += p.shown, failed_now += p.failed;
            const int k = PropFor(f.items[i], s.res);
            const char* a = nullptr;
            const std::uint8_t* b = nullptr;
            const char* c = nullptr;
            if (k >= 0 && k < kKeyCount && s.refused[k]) ++refused;
            else if (k >= 0 && !KeySource(s, k, &a, &b, &c)) ++waiting;
        }
        std::uint32_t pl = 0, aba = 0, bad = 0;
        realbody::GearCounters(s.slot, &pl, &aba, &bad);
        char fixes[160];
        realbody::GearPlacementInfo(s.slot, fixes, sizeof(fixes));
        if (&s == &g_sets[0] && (s.looks || s.pose_targets)) {
            char lc[400], pc[200];
            look::Counters(lc, sizeof(lc));
            realbody::GearPoseCounters(pc, sizeof(pc));
            Log("[look] props: %u looks applied (%u partly, %u again, %u failed); %s; backpack pose: %s", s.looks, s.looks_partial, s.looks_reapplied,
                s.looks_failed, lc, pc);
        }
        unsigned spares = 0;
        for (const PropSet::Spare& sp : s.spare) spares += sp.p.live;
        Log("[%s] %u items, %u props live (%u loading, %u shown, %u failing now, %u refused, %u waiting for their resource; %u failures, %u retries, %u "
            "reshown, %u kept across layouts, %u removed); placements %u (aba %u, bad matrix %u; %s); weapon spares %u (%u kept, %u reused)",
            s.tag, f.count, live, loading, shown, failed_now, refused, waiting, s.failed, s.retried, s.reshown, s.kept, s.removed, pl, aba, bad, fixes,
            spares, s.spare_kept, s.spare_reused);
    }
}


// ---------------------------------------------------------------- census (debug, read-only, fault-free)
// What the game draws on and around an entity: its child tree, the inventory's gear entities, the
// mesh controller's part masks and the ArtParts it drives. Every read is fault-free; nothing is called.
std::string CStr(std::uintptr_t p, std::size_t max = 96) {
    if (!p) return "";
    std::int32_t len = -1;
    char buf[128] = {};
    if (max > sizeof(buf) - 1) max = sizeof(buf) - 1;
    if (!Rd(p - 8, len) || len < 0 || len > 4096) return "";
    const std::size_t n = static_cast<std::size_t>(len) < max ? static_cast<std::size_t>(len) : max;
    if (n && !mem::Read(p, buf, n)) return "";
    for (std::size_t i = 0; i < n; ++i)
        if (static_cast<unsigned char>(buf[i]) < 0x20 || static_cast<unsigned char>(buf[i]) > 0x7E) buf[i] = '?';
    return std::string(buf, n);
}

// The name of a Decima object (ResourceWithName +0x20 String), or "".
std::string ObjName(std::uintptr_t o) {
    if (!o || !mem::DerivesFrom(mem::RttiOf(o), "ResourceWithName")) return "";
    return CStr(Q(o + 0x20));
}

std::string GuidStr(const std::uint8_t* g) {
    char b[40];
    std::snprintf(b, sizeof(b), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
                  g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return b;
}

void CensusEntity(const char* tag, int depth, std::uintptr_t e, std::uintptr_t sam, const Xf* sam_w) {
    std::uint64_t f = 0;
    Rd(e + kEntFlags, f);
    char path[200] = {};
    std::uint8_t g[16] = {};
    const char* rt = nullptr;
    const bool have = realbody::ResourceIdentity(reinterpret_cast<game::Entity*>(e), path, sizeof(path), g, &rt);
    Xf w;
    char where[200] = "";
    if (sam_w && ReadWorld(e, &w)) {
        double rp[3];
        float rr[3][3];
        Relative(*sam_w, w, rp, rr);
        int best = -1;
        double bd = 1e9;
        for (unsigned j = 0; j < 77; ++j) {
            Xf jw;
            if (!JointWorld(reinterpret_cast<game::Entity*>(sam), *sam_w, j, &jw)) continue;
            const double d = Dist(jw, w);
            if (d < bd) bd = d, best = static_cast<int>(j);
        }
        std::snprintf(where, sizeof(where), " at Sam-local (%.3f %.3f %.3f) nearest joint %d (%.3f m)", rp[0], rp[1], rp[2], best, bd);
    }
    Log("[census] %s %*s%p %s flags 0x%llX vis %d parent %p res '%s' %s (%s)%s", tag, depth * 2, "", reinterpret_cast<void*>(e), TypeOf(e),
        static_cast<unsigned long long>(f), (f & 2) ? 1 : 0, reinterpret_cast<void*>(Q(e + 0x70)), have ? path : "?", have ? GuidStr(g).c_str() : "-",
        rt ? rt : "-", where);
    partner::DescribeEntity(reinterpret_cast<game::Entity*>(e), "census");
}

void CensusTree(const char* tag, std::uintptr_t root, std::uintptr_t sam, const Xf* sam_w, unsigned* budget, int depth = 0) {
    if (!root || depth > 4) return;
    std::uintptr_t c = Q(root + 0x78);
    for (int guard = 0; c && guard < 64 && *budget; ++guard, c = Q(c + 0x80)) {
        if (!Derives(c, "Entity")) {
            Log("[census] %s %*s%p is not an Entity (%s): stop", tag, depth * 2 + 2, "", reinterpret_cast<void*>(c), TypeOf(c));
            break;
        }
        --*budget;
        CensusEntity(tag, depth + 1, c, sam, sam_w);
        CensusTree(tag, c, sam, sam_w, budget, depth + 1);
    }
}

// ArtPartsData: +0x100 count, +0x108 part pointers; part +0x08 child count, +0x18 parent part,
// +0x2C flags (bit 0 asked visible, bit 1 shown). The part's name: the first named resource it points to.
void CensusArtParts(const char* tag, std::uintptr_t e) {
    const std::uintptr_t apd = partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), "ArtPartsData");
    std::int32_t n = -1;
    const std::uintptr_t arr = apd ? Q(apd + 0x108) : 0;
    if (!apd || !Rd(apd + 0x100, n) || n < 0 || n > 512 || !arr) {
        Log("[census] %s ArtPartsData %p: count %d (not walked); entity +0x180 = %p", tag, reinterpret_cast<void*>(apd), n,
            reinterpret_cast<void*>(Q(e + 0x180)));
        return;
    }
    Log("[census] %s ArtPartsData %p (%s): %d parts; entity +0x180 = %p", tag, reinterpret_cast<void*>(apd), TypeOf(apd), n,
        reinterpret_cast<void*>(Q(e + 0x180)));
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t p = Q(arr + 8ull * static_cast<unsigned>(i));
        if (!p) continue;
        std::uint8_t fl = 0;
        std::int32_t kids = 0;
        Rd(p + 0x2C, fl);
        Rd(p + 0x08, kids);
        const std::uintptr_t parent = Q(p + 0x18);
        int pi = -1;
        for (std::int32_t k = 0; k < n && parent; ++k)
            if (Q(arr + 8ull * static_cast<unsigned>(k)) == parent) pi = k;
        std::string name;
        std::string refs;
        for (unsigned off = 0x20; off < 0x80 && name.empty(); off += 8) {
            const std::uintptr_t q = Q(p + off);
            const char* tn = q ? mem::RttiName(mem::RttiOf(q)) : nullptr;
            if (!tn) continue;
            name = ObjName(q);
            char b[96];
            std::snprintf(b, sizeof(b), " +%X %s", off, tn);
            refs += b;
        }
        Log("[census] %s   part %d %s flags 0x%02X parent %d kids %d name '%s'%s", tag, i, TypeOf(p), fl, pi, kids, name.c_str(), refs.c_str());
    }
}

void CensusMeshControl(std::uintptr_t sam) {
    const std::uintptr_t ctrl = sam + 0xA60;
    if (Q(ctrl) != sam) {
        Log("[census] mesh controller: +0xA60 does not point back at Sam");
        return;
    }
    std::uint64_t root = 0, cmp = 0;
    Rd(ctrl + 0x338, root);
    Rd(ctrl + 0x1998, cmp);
    Log("[census] mesh controller %p: Root mask 0x%llX, compare value 0x%llX", reinterpret_cast<void*>(ctrl), static_cast<unsigned long long>(root),
        static_cast<unsigned long long>(cmp));
    for (unsigned k = 0; k < 89; ++k) {
        std::uint64_t m = 0, m2 = 0, m3 = 0;
        std::int32_t idx = -1;
        std::uint8_t mode = 0;
        Rd(ctrl + 0x348 + 8ull * k, m);
        Rd(ctrl + 0x8D8 + 8ull * k, m2);
        Rd(ctrl + 0xE68 + 8ull * k, m3);
        Rd(ctrl + 0x38 + 8ull * k, idx);
        Rd(ctrl + 0x19A0 + k, mode);
        if (idx == -1 && !m && !m2 && !m3 && !mode && k) continue;
        Log("[census]   part %2u: ArtParts index %d, mask 0x%llX, b 0x%llX, c 0x%llX, compare mode %u", k, idx, static_cast<unsigned long long>(m),
            static_cast<unsigned long long>(m2), static_cast<unsigned long long>(m3), mode);
    }
}

void Census(game::Entity* who, bool sam_like) {
    const auto e = reinterpret_cast<std::uintptr_t>(who);
    if (!e) {
        Log("[census] no entity");
        return;
    }
    Xf w;
    const bool have_w = ReadWorld(e, &w);
    CensusEntity("root", 0, e, e, have_w ? &w : nullptr);
    unsigned budget = 200;
    CensusTree("tree", e, e, have_w ? &w : nullptr, &budget);
    CensusArtParts("root", e);
    if (!sam_like) return;
    CensusMeshControl(e);
    const std::uintptr_t inv = Q(e + kPInv);
    if (!inv || Q(inv + kCompOwner) != e) {
        Log("[census] no inventory component");
        return;
    }
    // Every reference / entity pointer in the inventory's gear block (+0x4C80 .. +0x4F00).
    for (std::uintptr_t off = 0x4C80; off < 0x4F00; off += 8) {
        const std::uintptr_t v = Q(inv + off);
        const std::uintptr_t ent = RefEntity(v);
        if (!v) continue;
        if (!ent) {
            const char* tn = mem::RttiName(mem::RttiOf(v));
            if (tn) Log("[census] inv +0x%llX = %p (%s)", static_cast<unsigned long long>(off), reinterpret_cast<void*>(v), tn);
            continue;
        }
        char tag[32];
        std::snprintf(tag, sizeof(tag), "inv+%llX", static_cast<unsigned long long>(off));
        CensusEntity(tag, 0, ent, e, have_w ? &w : nullptr);
        unsigned b2 = 40;
        CensusTree(tag, ent, e, have_w ? &w : nullptr, &b2);
        CensusArtParts(tag, ent);
    }
}

}  // namespace

void Init(const Config& config) noexcept {
    g_enabled = config.partner_gear;
    {
        const bodies::OwnChoice own = bodies::ChooseOwn(config.role == Role::host, config.my_character.c_str());
        g_own_hoodless = own.body && std::strncmp(own.body->key, "fragile", 7) == 0;  // fragile, fragile_suit: no hood
    }
    g_boots_sync = config.boots_sync;
    g_boots_style = config.boots_look_own ? bootslook::Style::own : bootslook::Style::prop;
    g_boots_own_porter = config.boots_own_porter;
    g_exo_on = config.exo_carrier;
    g_hood_mode = config.fragile_hood < 0 ? 0 : config.fragile_hood > 2 ? 2 : config.fragile_hood;
    g_hood_hair = config.hood_hair;
    g_held_mode = static_cast<heldrules::Mode>(config.held_items < 0 ? 0 : config.held_items > 2 ? 2 : config.held_items);
    Log("[boots] her boots: boots_look=%s (%s), boots_own_porter=%d", bootslook::StyleName(g_boots_style),
        g_boots_style == bootslook::Style::own ? "her own boots, painted with the guest's boot pictures; no boots prop, feet not scaled"
                                               : "Sam's boots on her feet; sole grass / intact porter boots keep her own",
        g_boots_own_porter ? 1 : 0);
    Log("[gear] gear on bodies %s (partner_gear=%d); boots %s (boots_sync=%d)", g_enabled ? "on" : "off", g_enabled ? 1 : 0,
        g_boots_sync ? "sent and drawn on the partner" : "not sent or drawn", g_boots_sync ? 1 : 0);
}

// The partner's boots prop (section 21): its pose target (real_body.cpp), its ArtParts as the sender shows them, and
// the partner body's own feet scaled away while it is shown and posed. Game thread, after DriveSet (which shows the prop only once
// BootsPropReady: posed and its parts mirrored).
void BootsTick(PropSet& s, game::Entity* body, double now) {
    const Prop* bp = nullptr;
    unsigned bi = 0;
    // Round 6 T3 (section 30): her boots on the partner Fragile follow the guest's boots style (its LOOK says; our own
    // boots_look until one came): own = her model's boots (textured), no prop, her feet never scaled; prop = Sam's boots on
    // her feet, except the bits that keep her own (sole grass, intact porter boots, the load window). Sam bodies: the prop.
    const bool her = IsHerSuit(realbody::CurrentBody());
    const int sent_style = texshare::PartnerBootsStyle();
    const bootslook::Style style = !her ? bootslook::Style::prop
                                        : sent_style >= 0 ? (sent_style == 1 ? bootslook::Style::own : bootslook::Style::prop) : g_boots_style;
    const bool sent = g_boots_sync && body && s.have_want && (s.want.flags & wire::kGearBoots);
    const bootslook::Decision d = bootslook::Decide(style, sent, sent ? s.want.boots_visible : 0u, g_boots_own_porter);
    static int last_d = -1;
    const int dc = (sent ? 1 : 0) + (d.prop ? 2 : 0) + (her ? 4 : 0) + (style == bootslook::Style::own ? 8 : 0) + (sent_style >= 0 ? 16 : 0);
    if (dc != last_d && body) {
        last_d = dc;
        Log("[boots] partner: %s (boots_look=%s%s, parts %08X)", !sent ? "no boots item" : d.prop ? "Sam's boots on the body (the prop)" : "her own boots (no prop)",
            bootslook::StyleName(style), !her ? ", a Sam body" : sent_style >= 0 ? " from the partner's look" : " (ours: no look from the partner yet)",
            sent ? s.want.boots_visible : 0u);
    }
    if (sent && d.prop)
        for (unsigned i = 0; i < s.want.count; ++i)
            if (s.want.items[i].kind == wire::GearKind::boots && s.props[i].live && holo::SpawnAlive(s.props[i].sp) &&
                now - s.props[i].live_ms >= 300) {
                bp = &s.props[i], bi = i;
                break;
            }
    std::uintptr_t want = bp ? reinterpret_cast<std::uintptr_t>(bp->sp.e) : 0;
    static std::uintptr_t refused_e = 0;
    if (want && want == refused_e) want = 0;
    if (want != g_boots_prop_e) {
        realbody::SetPartnerFeetHidden(false, 100);
        char why[160] = {};
        if (!want) {
            realbody::ClearBootsProp();
            Log("[boots] the partner's boots prop: no pose target now");
        } else if (!realbody::SetBootsProp(&bp->sp, why, sizeof(why))) {
            Log("[boots] the partner's boots prop %p (item %u) is not posed: %s", reinterpret_cast<void*>(want), bi, why);
            refused_e = want;
            want = 0;
        } else {
            Log("[boots] the partner's boots prop %p (item %u) follows the partner body's legs", reinterpret_cast<void*>(want), bi);
        }
        g_boots_prop_e = want;
    }
    if (bp && g_boots_prop_e) {
        const std::uint64_t pa[1] = {s.want.boots_apply}, pv[1] = {s.want.boots_visible};
        // the parts' reference: our own boots entity (the same part_sam_foot_def), or, barefoot / unreadable, the prop's own
        // parts (the prop is the known boots cutscene prop): a receiver without boots still shows the partner's
        MirrorParts(g_boots_pm, g_boots_prop_e, g_boots.e ? g_boots.e : g_boots_prop_e, pa, pv, wire::kMaxGearBootsParts, now);
    }
    g_boots_prop_shown = bp && bp->shown && BootsPropReady(g_boots_prop_e);
    const bool hide = g_boots_prop_shown && g_pfeet_pct < 100;
    static bool hid = false;
    realbody::SetPartnerFeetHidden(hide, g_pfeet_pct);
    if (hide != hid) {
        std::uint32_t sw = 0, pw = 0;
        realbody::FeetCounters(&sw, &pw);
        Log("[boots] the partner body's feet %s (scale %d%%; %u pose writes with them scaled so far)", hide ? "scaled away under the boots prop" : "shown", g_pfeet_pct, pw);
        hid = hide;
    }
    static double next_ms = 0;
    if (g_boots_prop_e && now >= next_ms) {
        next_ms = now + 5000;
        char c[200];
        realbody::BootsCounters(c, sizeof(c));
        Log("[boots] %s; parts %s", c, g_boots_pm.compatible ? "mirrored" : "not mirrored (another model?)");
    }
}

bool Enabled() noexcept { return g_enabled; }

bool OwnBoots(int* type, std::uint32_t* visible, bool* own_style) noexcept {
    *visible = g_boots.shown ? (g_boots_visible & g_boots_apply) : 0u;
    *own_style = g_boots_style == bootslook::Style::own;
    *type = -1;
    static int sites = -1;  // the status tick's type compare and SetShoesForcibly's status read, checked once
    if (sites < 0) {
        sites = mem::CodeMatches(mem::Base() + 0x273251B, "0F B6 BB B8 03 00 00 40 80 FF 0B 74 68 40 38 BB 28 03 00 00") &&
                        mem::CodeMatches(mem::Base() + 0x26E7430, "40 53 48 83 EC 20 0F B6 D9 E8 ?? ?? ?? ?? 48 85 C0 74 0D 48 8B 80 E0 54 00 00 88 98 B8 03 00 00")
                    ? 1
                    : 0;
        Log("[boots] Sam's boots type (status +0x328) sites %s", sites ? "verified" : "do not match: her boots pictures follow no boot type (03 Porter boots)");
    }
    if (!sites) return false;
    game::Entity* local = nullptr;
    if (!game::TryGetLocalPlayer(&local) || !local) return false;
    // Sam's boots type: the status block (player +0x54E0, its owner at +0x88) +0x320 (the shoes) +0x08 (EDSPlayerShoesType;
    // the status tick 0x142732528 compares it with SetShoesForcibly's request +0x3B8)
    const auto p = reinterpret_cast<std::uintptr_t>(local);
    const std::uintptr_t st = Q(p + 0x54E0);
    std::uint8_t t = 0xFF;
    if (!st || Q(st + 0x88) != p || !Rd(st + 0x328, t) || t > 10) return false;
    *type = t;
    return true;
}

void Tick(game::Entity* local, bool local_active, double now_ms) noexcept {
    if (!g_enabled || !realbody::HookInstalled()) return;
    if (local_active && g_chk.ok) WatchWeapon(local, now_ms);
    ReadLocalTick(local, local_active, now_ms);
    HeadgearTick(static_cast<double>(GetTickCount64()), false);
    realbody::GearMeterTick(now_ms);  // hidden tests: before this tick's placements (section 26.7)
    // The partner slot: the partner's real body (or, debug, a mirroring debug body with our gear).
    PropSet& ps = g_sets[0];
    game::Entity* pbody = g_partner_on ? realbody::SlotEntity(realbody::GearSlot::partner) : nullptr;
    const bool debug = realbody::GetOwner() == realbody::Owner::debug;
    if (debug && !g_debug_body) pbody = nullptr;
    if (debug) {
        ps.want = g_local;
        ps.have_want = g_have_local;
        ps.res = g_res_local;
    } else {
        ps.want = g_partner;
        ps.have_want = g_have_partner;
        ps.res = g_res_partner;
    }
    std::uint64_t flags = 0;
    const bool pvis = pbody && partner::ReadFlags(pbody, &flags) && (flags & 0x2) != 0;
    DriveSet(ps, pbody, pvis, local_active, now_ms);
    BootsTick(ps, pbody, now_ms);
    // The partner's own gear parts (exoskeleton, BB pod pipe, ...) on a Sam body, and its backpack's
    // parts (the bag, rain cover, pouches) on the backpack prop: each only when the target's ArtParts
    // are the same resources as our own Sam's / backpack's.
    if (pbody && local_active && ps.have_want) {
        const auto lp = reinterpret_cast<std::uintptr_t>(local);
        if (ps.want.flags & wire::kGearParts)
            MirrorParts(g_pm, reinterpret_cast<std::uintptr_t>(pbody), lp, ps.want.parts_apply, ps.want.parts_visible, wire::kMaxGearParts, now_ms);
        std::uintptr_t prop = 0, ours = 0;
        for (unsigned i = 0; i < ps.want.count; ++i)
            if (ps.want.items[i].kind == wire::GearKind::backpack && ps.props[i].live && holo::SpawnAlive(ps.props[i].sp)) {
                prop = reinterpret_cast<std::uintptr_t>(ps.props[i].sp.e);
                break;
            }
        if (lp && Q(lp) == mem::Base() + kPlayerVtable) {
            const std::uintptr_t inv = Q(lp + kPInv);
            if (inv && Q(inv + kCompOwner) == lp) ours = RefEntity(Q(inv + kInvPack));
        }
        if (prop && (ps.want.flags & wire::kGearPackParts)) {
            const std::uint64_t pa[1] = {ps.want.pack_apply}, pv[1] = {ps.want.pack_visible};
            MirrorParts(g_pack_pm, prop, ours, pa, pv, 64, now_ms);
        }
    }
    // The self view's props: only what the game itself stops drawing while Sam's parts are hidden (the
    // BB pod, which follows his BBPod part); everything else is his real gear on his skeleton.
    PropSet& ss = g_sets[1];
    game::Entity* sbody = g_self_on ? realbody::SlotEntity(realbody::GearSlot::self) : nullptr;
    static wire::GearFrame self_frame;
    self_frame = wire::GearFrame{};
    self_frame.flags = g_local.flags & wire::kGearValid;
    for (unsigned i = 0; i < g_local.count; ++i)
        if ((g_local.items[i].kind == wire::GearKind::bb_pod && g_local.items[i].parent == wire::kGearNoParent) ||
            (g_local.items[i].kind == wire::GearKind::entity && g_hood.e && g_local_e[i] == g_hood.e)) {  // her hat in the rain (26.8)
            g_self_src[self_frame.count] = static_cast<std::uint8_t>(i);
            self_frame.items[self_frame.count++] = g_local.items[i];
        }
    ss.want = self_frame;
    // (an empty frame still drives the set while the exo carrier is wanted on the self body: section 32)
    std::uint64_t exo_vis[2];
    ss.have_want = g_have_local && (self_frame.count > 0 || ExoWant(ss, sbody, exo_vis) || ss.exo.p.live || ss.exo.p.loading);
    ss.res = g_res_local;
    DriveSet(ss, sbody, sbody && realbody::SelfShown(), local_active, now_ms);
    // the meters last: the props were just placed from the bodies' current transforms and poses (PlaceGearNow)
    HeadMeter(local, static_cast<double>(GetTickCount64()));
}

bool TakeSendRes(double now_ms, wire::GearRes* out) noexcept {
    if (!g_enabled || !out || g_res_local_n == 0) return false;
    unsigned id = wire::kMaxGearRes;
    if (g_res_unsent) {
        for (unsigned i = 0; i < g_res_local_n; ++i)
            if ((g_res_unsent >> i) & 1) {
                id = i;
                break;
            }
    } else {
        if (now_ms < g_res_next_ms) return false;
        id = g_res_rr++ % g_res_local_n;
    }
    if (id >= g_res_local_n) return false;
    g_res_unsent &= ~(1ull << id);
    g_res_next_ms = now_ms + 250;
    out->id = static_cast<std::uint8_t>(id);
    std::memcpy(out->guid, g_res_local[id].guid, 16);
    std::snprintf(out->path, sizeof(out->path), "%s", g_res_local[id].path);
    return true;
}

// The receiver's own check of a partner resource, before anything is loaded from it (review: the
// class / component policy can only run after the copy is realized, and realizing is the step that
// crashed a worker thread in runs 20261117-131822 / -133622). Default: only the cores whose entities
// were mirrored in the hidden runs (the backpack's items, the stowed weapons, the hanging boots, the
// inventory's shields / tags / pouches), and never the resources the sender's level-1 policy refuses
// that were seen on Sam (census 20261117-130343): the BB pod entity (DSBBPodComponent), the cuff device,
// the baby, the boots, the chiral cylinder, the backpack belt. A game whose own tests raised the mirror
// level (gear 7 2) or chose the pod entity (gear 8 1) accepts what that level sends.
bool ResAllowed(const char* path, const std::uint8_t (&guid)[16], const char** why) {
    // A weapon's own resource is never loaded, at any level (its entity is a DSWeaponEntity: a copy
    // crashes the game, section 17.1); SetPartnerRes keeps it for the weapon items' props.
    if (wire::GearResWeaponOnly(path)) {
        if (why) *why = "a weapon's own resource (DSWeaponEntity): never loaded";
        return false;
    }
    if (g_mirror_level >= 2) return true;  // tests only (gear 7 2): any ds/entities/ path the wire allows
    return wire::GearResProven(path, guid, g_pod_entity, why);
}
std::uint32_t g_res_refused = 0, g_res_changed = 0;

void MarkResUnsent(int id) noexcept {
    if (id < 0) g_res_unsent = g_res_local_n >= 64 ? ~0ull : ((1ull << g_res_local_n) - 1);
    else if (static_cast<unsigned>(id) < g_res_local_n) g_res_unsent |= 1ull << id;
}

void SetPartnerRes(const wire::GearRes& r) noexcept {
    if (r.id >= wire::kMaxGearRes) return;
    ResEntry& e = g_res_partner[r.id];
    if (e.used && std::memcmp(e.guid, r.guid, 16) == 0 && std::strcmp(e.path, r.path) == 0) return;
    PropSet& s = g_sets[0];
    const int key = kKeyEntity + r.id;
    const char* no = nullptr;
    if (!ResAllowed(r.path, r.guid, &no)) {
        s.refused[key] = true;  // never spawned as an entity
        if (wire::GearResWeaponOnly(r.path)) {
            // a weapon resource: kept for the partner's weapon items, which show a prop by its core (section 17)
            const bool fresh = !e.used || std::strcmp(e.path, r.path) != 0 || std::memcmp(e.guid, r.guid, 16) != 0;
            e.used = true;
            e.weapon_only = true;
            std::memcpy(e.guid, r.guid, 16);
            std::snprintf(e.path, sizeof(e.path), "%s", r.path);
            ++g_res_rx;
            const WeaponMap* m = WeaponFor(r.path);
            if (fresh && ++g_res_refused <= 40)
                Log("[gear] the partner's weapon resource %u '%s' %s: never loaded itself; its weapon items show %s", r.id, r.path, GuidStr(r.guid).c_str(),
                    m ? (m->exact ? kProps[m->prop].key : "an approximate prop") : "nothing (no prop for this weapon)");
            return;
        }
        if (++g_res_refused <= 20) Log("[gear] the partner's resource %u '%s' %s not loaded: %s", r.id, r.path, GuidStr(r.guid).c_str(), no);
        return;
    }
    if (e.used) {
        // An honest peer never changes an id; a changed one must not keep showing the old resource.
        if (++g_res_changed <= 10) Log("[gear] the partner's resource %u changed: '%s' -> '%s'", r.id, e.path, r.path);
        bool unpublished = false;
        for (unsigned i = 0; i < wire::kMaxGearItems; ++i) {
            Prop& p = s.props[i];
            if (p.def != key || !(p.loading || p.live || p.sp.sp || p.failed)) continue;
            if (!unpublished) Unpublish(s), unpublished = true;
            RemoveProp(s, i, "its resource changed");
        }
    }
    e.used = true;
    e.weapon_only = false;
    std::memcpy(e.guid, r.guid, 16);
    std::snprintf(e.path, sizeof(e.path), "%s", r.path);
    ++g_res_rx;
    // A prop of this id refused or failed before (another resource) is tried again.
    g_sets[0].refused[kKeyEntity + r.id] = false;
}

bool TakeSend(double now_ms, wire::GearFrame* out) noexcept {
    if (!g_enabled || !g_have_local || !out) return false;
    const bool parts_changed = (g_sent.flags & wire::kGearParts) != (g_local.flags & wire::kGearParts) ||
                               std::memcmp(g_sent.parts_visible, g_local.parts_visible, sizeof(g_sent.parts_visible)) != 0 ||
                               std::memcmp(g_sent.parts_apply, g_local.parts_apply, sizeof(g_sent.parts_apply)) != 0 ||
                               g_sent.hood != g_local.hood || g_sent.hood_info != g_local.hood_info;  // (section 37)
    const bool layout = !g_have_sent || parts_changed || wire::GearLayoutHash(g_sent) != wire::GearLayoutHash(g_local) || g_sent.count != g_local.count;
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

bool TakeSendLook(double now_ms, wire::GearLook* out) noexcept {
    if (!g_enabled || !g_looks_on || !g_have_local || !out) return false;
    const std::uint16_t layout = wire::GearLayoutHash(g_local);
    for (unsigned i = 0; i < g_local.count; ++i) {
        LocalLook& L = g_look[i];
        if (!L.have || L.l.layout != layout || (L.hash == L.sent_hash && L.sent_layout == layout)) continue;
        L.sent_hash = L.hash, L.sent_layout = layout;
        *out = L.l;
        ++g_looks_sent;
        return true;
    }
    if (now_ms < g_look_rr_ms || g_local.count == 0) return false;
    g_look_rr_ms = now_ms + 250;
    for (unsigned k = 0; k < g_local.count; ++k) {
        const unsigned i = g_look_rr++ % g_local.count;
        if (!g_look[i].have || g_look[i].l.layout != layout) continue;
        *out = g_look[i].l;
        ++g_looks_sent;
        return true;
    }
    return false;
}

void MarkLooksUnsent() noexcept {
    for (LocalLook& L : g_look) L.sent_hash = 0, L.sent_layout = 0;
    g_have_pose_sent = false;
}

void SetPartnerLook(const wire::GearLook& l) noexcept {
    if (l.item >= wire::kMaxGearItems) return;
    PartnerLook& p = g_plook[l.item];
    const std::uint32_t h = wire::GearLookHash(l);
    if (++g_looks_rx <= 3 || (p.have && (p.hash != h || p.l.layout != l.layout) && g_looks_rx <= 80))
        Log("[look] the partner's item %u (layout %04X, kind %u res %u): %u variation(s), %u shader variable(s)", l.item, l.layout, static_cast<unsigned>(l.kind),
            l.res, l.nvar, l.nsv);
    p.l = l;
    p.hash = h;
    p.have = true;
}

bool TakeSendPose(double now_ms, wire::GearPose* out) noexcept {
    if (!g_enabled || !g_poses_on || !out) return false;
    // two streams on STK1: the backpack's pose, and the held item's (section 31; its weapon item's index tells them apart)
    auto due = [now_ms](const wire::GearPose& local, const wire::GearPose& sent, bool have_sent, double sent_ms, double min_ms) {
        const double since = now_ms - sent_ms;
        float dm = 0, dd = 0;
        const bool same = have_sent && sent.item == local.item && sent.layout == local.layout && wire::GearPoseDiff(sent, local, &dm, &dd);
        const bool changed = !same || dm > 0.001f || dd > 0.5f;
        return since >= 1000 || (changed && since >= min_ms);
    };
    if (g_have_pose_local && due(g_pose_local, g_pose_sent, g_have_pose_sent, g_pose_sent_ms, 100)) {
        g_pose_sent = g_pose_local;
        g_have_pose_sent = true;
        g_pose_sent_ms = now_ms;
        ++g_poses_sent;
        *out = g_pose_local;
        return true;
    }
    if (g_have_hpose_local && due(g_hpose_local, g_hpose_sent, g_have_hpose_sent, g_hpose_sent_ms, 100)) {
        g_hpose_sent = g_hpose_local;
        g_have_hpose_sent = true;
        g_hpose_sent_ms = now_ms;
        ++g_hposes_sent;
        *out = g_hpose_local;
        return true;
    }
    return false;
}

void SetPartnerPose(const wire::GearPose& p) noexcept {
    // the held item's stream (section 31): flagged by the sender (round 6), so a held pose that arrives before its layout's
    // gear frame (STC1 is one unreliable send per change) never lands in the backpack's slot and takes the frame's pose off.
    // An older sender (no flag): the pose of a weapon item of the partner's current layout.
    const bool held = (p.flags & wire::kGearPoseHeld) != 0 ||
                      (g_have_partner && p.layout == wire::GearLayoutHash(g_partner) && p.item < g_partner.count &&
                       g_partner.items[p.item].kind == wire::GearKind::weapon);
    if (held) {
        if (++g_hposes_rx <= 3) Log("[gear] the partner's held item pose: item %u, %u joints (skeleton %08X)", p.item, p.n, p.skeleton);
        g_hpose_partner = p;
        g_have_hpose_partner = true;
        ++g_hpose_partner_seq;
        return;
    }
    if (++g_poses_rx <= 2) Log("[gear] the partner's backpack pose: item %u, %u joints (skeleton %08X)", p.item, p.n, p.skeleton);
    g_pose_partner = p;
    g_have_pose_partner = true;
    ++g_pose_partner_seq;
}

void SetPartnerGear(const wire::GearFrame* f) noexcept {
    if (!f) {
        if (g_have_partner) Log("[gear] the partner's gear cleared");
        g_have_partner = false;
        for (ResEntry& e : g_res_partner) e = ResEntry{};
        for (PartnerLook& l : g_plook) l = PartnerLook{};
        g_have_pose_partner = false;
        g_have_hpose_partner = false;
        return;
    }
    if (!g_have_partner || wire::GearLayoutHash(*f) != wire::GearLayoutHash(g_partner)) DescribeFrame(*f, "the partner's gear", g_res_partner);
    g_partner = *f;
    g_have_partner = true;
}

void EmergencyStop() noexcept {
    realbody::PublishGear(realbody::GearSlot::partner, nullptr, 0);
    realbody::PublishGear(realbody::GearSlot::self, nullptr, 0);
    // Our head accessories' movers back to the identity and the game's own flag (fault-free checks first: only a mover
    // that still holds exactly what we wrote), so the real Sam's cap and glasses sit where the game puts them.
    const std::uint32_t before = g_hg.restores;
    HeadgearTick(static_cast<double>(GetTickCount64()), true);
    if (g_hg.restores != before) Log("[headgear] emergency stop: %u accessory mover(s) put back", g_hg.restores - before);
}

std::uint8_t CargoModelOf(std::uintptr_t bag) noexcept {
    if (!bag || std::strcmp(TypeOf(bag), "DSBaggage2") != 0) return wire::kGearNone;
    const std::uintptr_t li = Q(bag + kBagListItem);
    if (!li || std::strcmp(TypeOf(li), "DSGameBaggageListItem") != 0) return wire::kGearNone;
    std::uint8_t lf[5] = {};
    if (!mem::Read(li + kListAttribute, lf, sizeof(lf))) return wire::kGearNone;
    return Drawn(lf[1], 0) ? ModelFromListItem(li) : wire::kGearNone;
}

bool CargoProp(std::uint8_t model, const char** key, const char** path, const char** guid) noexcept {
    if (model >= 31 || kModelMap[model].prop < 0 || !key || !path || !guid) return false;
    const PropDef& d = kProps[kModelMap[model].prop];
    *key = d.key, *path = d.path, *guid = d.guid;
    return true;
}

// A few joints of an entity's skeleton against its own origin (gear 26: where a held item's mesh is drawn).
void HeldJoints(std::uintptr_t e, std::int32_t n, char* out, std::size_t cap) {
    out[0] = 0;
    Xf w;
    if (n <= 0 || !ReadWorld(e, &w)) {
        std::snprintf(out, cap, "joints -");
        return;
    }
    std::size_t len = 0;
    const unsigned js[4] = {0u, 1u, static_cast<unsigned>(n) / 2, static_cast<unsigned>(n) - 1};
    for (unsigned j : js) {
        double pos[3];
        if (j >= static_cast<unsigned>(n) || len + 40 >= cap) continue;
        if (realbody::JointWorldN(reinterpret_cast<game::Entity*>(e), j, static_cast<unsigned>(n), pos))
            len += static_cast<std::size_t>(std::snprintf(out + len, cap - len, "j%u %.2f m ", j,
                                                          std::sqrt((pos[0] - w.p[0]) * (pos[0] - w.p[0]) + (pos[1] - w.p[1]) * (pos[1] - w.p[1]) +
                                                                    (pos[2] - w.p[2]) * (pos[2] - w.p[2]))));
        else
            len += static_cast<std::size_t>(std::snprintf(out + len, cap - len, "j%u ? ", j));
    }
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
                          "%s; ours %u items (layout %04X, reads %u, failed %u, kept while moving %u, new layouts held a read %u; resources %u, %u "
                          "without an id); partner's %s (%u items, %u resource entries received); props partner %u spawned %u failed %u retried %u reshown, "
                          "self %u spawned %u failed; placements %u/%u (aba %u/%u, bad %u/%u); partner gear parts set %u",
                          ok ? "read ok" : why, g_local.count, wire::GearLayoutHash(g_local), g_reads, g_read_fail, g_moving_skips, g_layout_held,
                          g_res_local_n, g_res_full, g_have_partner ? "received" : "none", g_have_partner ? g_partner.count : 0u, g_res_rx,
                          g_sets[0].spawned, g_sets[0].failed, g_sets[0].retried, g_sets[0].reshown, g_sets[1].spawned, g_sets[1].failed, pl[0], pl[1],
                          aba[0], aba[1], bad[0], bad[1], g_pm.applied + g_pack_pm.applied);
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
        case 5:
        case 6: {
            if (!automation::Enabled() || !automation::SavesRedirected()) {
                std::snprintf(msg, cap, "refused: only in a hidden test run on scratch saves");
                return false;
            }
            if (op == 5) {
                g_fault_spawns = v[1] <= 0 ? 0 : v[1] > 100 ? 100 : static_cast<int>(v[1]);
                std::snprintf(msg, cap, "TEST: the next %d prop start(s) fail", g_fault_spawns);
            } else {
                // what the game might do to a prop (Sleep / Wake, activation): hide every partner prop
                // behind our back; the tick must show them again
                unsigned n = 0;
                for (Prop& p : g_sets[0].props)
                    if (p.live && holo::SpawnAlive(p.sp) && partner::SetVisible(p.sp.e, false)) ++n;
                std::snprintf(msg, cap, "TEST: %u partner prop(s) hidden behind the mod's back", n);
            }
            Log("[gear] %s", msg);
            return true;
        }
        case 8:
            if (v[1] != 0 && (!automation::Enabled() || !automation::SavesRedirected())) {
                std::snprintf(msg, cap, "refused: the pod entity copy only in a hidden test run on scratch saves (it crashed a worker thread)");
                return false;
            }
            g_pod_entity = v[1] != 0;
            g_next_read_ms = 0;
            std::snprintf(msg, cap, "BB pod drawn as %s", g_pod_entity ? "a copy of the game's pod entity" : "the cinematic pod prop");
            Log("[gear] %s", msg);
            return true;
        case 7:
            if (v[1] > 1 && (!automation::Enabled() || !automation::SavesRedirected())) {
                std::snprintf(msg, cap, "refused: mirror level 2 only in a hidden test run on scratch saves (not proven safe)");
                return false;
            }
            g_mirror_level = v[1] < 0 ? 0 : v[1] > 2 ? 2 : static_cast<int>(v[1]);
            g_next_read_ms = 0;
            std::snprintf(msg, cap, "game gear entities mirrored at level %d (0 model components only, 1 + the proven DS components, 2 + cylinder, belt, weapons)",
                          g_mirror_level);
            Log("[gear] %s", msg);
            return true;
        case 10:
            g_memo_on = v[1] != 0;
            for (NodeMemo& m : g_memo) m = NodeMemo{};
            g_read_us_sum = 0, g_read_us_max = 0, g_read_timed = 0, g_read_window = 0;
            std::snprintf(msg, cap, "gear walk cache %s (read cost logged after 50 reads)", g_memo_on ? "on" : "off");
            Log("[gear] %s", msg);
            return true;
        case 11: {
            // looks: 0 our gear (every entity, the sources of their variations, the backpack's joints), 1 the
            // partner's props
            if (v[1] == 0) {
                game::Entity* local = nullptr;
                game::TryGetLocalPlayer(&local);
                char why[160] = {};
                if (!Checked() || !ReadItems(local, false, why, sizeof(why))) {
                    std::snprintf(msg, cap, "our gear not read: %s", why);
                    return false;
                }
                static std::uintptr_t ents[kMaxLocal];
                unsigned n = 0;
                std::uintptr_t pack = 0;
                for (unsigned i = 0; i < g_item_count; ++i) {
                    const LocalItem& it = g_items[i];
                    if (it.kind != wire::GearKind::entity && it.kind != wire::GearKind::backpack) continue;
                    ents[n++] = it.e;
                    if (it.kind == wire::GearKind::backpack) pack = it.e;
                }
                look::DumpSources(ents, n, reinterpret_cast<std::uintptr_t>(local));
                for (unsigned i = 0; i < n; ++i) {
                    char tag[32];
                    std::snprintf(tag, sizeof(tag), "ours %u", i);
                    look::Dump(tag, ents[i], pack);
                }
                if (pack) look::DumpJoints("our backpack", pack);
                for (unsigned i = 0; i < g_local.count; ++i)
                    if (g_look[i].have)
                        Log("[look] our item %u kind %u: %u variation(s), %u shader variable(s), hash %08X (sent %08X)", i, static_cast<unsigned>(g_local.items[i].kind),
                            g_look[i].l.nvar, g_look[i].l.nsv, g_look[i].hash, g_look[i].sent_hash);
                std::snprintf(msg, cap, "our gear's looks logged (%u entities); %u looks sent, %u backpack poses sent", n, g_looks_sent, g_poses_sent);
                return true;
            }
            PropSet& s = g_sets[0];
            std::uintptr_t pack = 0;
            for (unsigned i = 0; i < s.want.count; ++i)
                if (s.want.items[i].kind == wire::GearKind::backpack && s.props[i].live) pack = reinterpret_cast<std::uintptr_t>(s.props[i].sp.e);
            unsigned n = 0;
            for (unsigned i = 0; i < s.want.count; ++i) {
                const Prop& p = s.props[i];
                if (!p.live || !holo::SpawnAlive(p.sp)) continue;
                char tag[48];
                std::uint32_t hash = 0;
                const wire::GearLook* w = WantLook(s, i, &hash);
                std::snprintf(tag, sizeof(tag), "prop %u (want %u var %u sv)", i, w ? w->nvar : 0u, w ? w->nsv : 0u);
                look::Dump(tag, reinterpret_cast<std::uintptr_t>(p.sp.e), pack);
                ++n;
            }
            if (pack) look::DumpJoints("the backpack prop", pack);
            char lc[400], pc[200];
            look::Counters(lc, sizeof(lc));
            realbody::GearPoseCounters(pc, sizeof(pc));
            std::snprintf(msg, cap, "%u partner props' looks logged; %u looks received, %u poses received; %s; pose: %s", n, g_looks_rx, g_poses_rx, lc, pc);
            Log("[look] %s", msg);
            return true;
        }
        case 14:
        case 15:
        case 23:
        case 24: {
            // hidden tests: what the player wears on the head (the inventory's accessory list, inv +0x37F9 +
            // 0x40 i: type at +0 (0x0D / 0x0E glasses, 0x0F cap), worn flag at +0x1D, the item at +0x1F; read
            // by the inventory update 0x1427F0FD9 / 0x1427F14B6) and the cap colour (inv +0x4BE0)
            const std::uintptr_t b = mem::Base();
            if (!mem::CodeMatches(b + 0x27F0FD9, "48 8D BE 16 38 00 00") || !mem::CodeMatches(b + 0x27F14B6, "80 78 E3 0F 75 19 44 38 20") ||
                !mem::CodeMatches(b + 0x27F18EB, "0F B6 96 E0 4B 00 00 48 8B 8E 18 4E 00 00")) {
                std::snprintf(msg, cap, "the inventory accessory list / cap colour sites do not match");
                return false;
            }
            game::Entity* local = nullptr;
            game::TryGetLocalPlayer(&local);
            const auto p = reinterpret_cast<std::uintptr_t>(local);
            const std::uintptr_t inv = p && Q(p) == b + kPlayerVtable ? Q(p + kPInv) : 0;
            if (!inv || Q(inv + kCompOwner) != p) {
                std::snprintf(msg, cap, "no local player inventory");
                return false;
            }
            const bool write = op == 15 || op == 23 || op == 24 || v[1] != 0 || v[2] != 0;
            if (write && (!automation::Enabled() || !automation::SavesRedirected())) {
                std::snprintf(msg, cap, "refused: only in a hidden test run on scratch saves");
                return false;
            }
            auto put = [](std::uintptr_t at, std::uint8_t val) {
                SIZE_T w = 0;
                return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at), &val, 1, &w) && w == 1;
            };
            if (op == 23) {
                // hidden tests: gear 23 <which> <value>: the glasses' colours (inventory +0x4BDC: 0 A lens, 1 B lens, 2 A frame, 3 B frame;
                // partner_outfit.cpp reads them), to wear the user's outfit in a run
                const int which = static_cast<int>(v[1]);
                if (which < 0 || which > 3 || v[2] < 0 || v[2] > 63) {
                    std::snprintf(msg, cap, "gear 23 <0 A lens / 1 B lens / 2 A frame / 3 B frame> <0..63>");
                    return false;
                }
                std::uint8_t was = 0;
                Rd(inv + 0x4BDC + static_cast<unsigned>(which), was);
                const bool ok = put(inv + 0x4BDC + static_cast<unsigned>(which), static_cast<std::uint8_t>(v[2]));
                static const char* const kWhat[4] = {"A lens", "B lens", "A frame", "B frame"};
                std::snprintf(msg, cap, "TEST: our glasses %s colour %u -> %d%s", kWhat[which], was, static_cast<int>(v[2]), ok ? "" : " (write failed)");
                Log("[look] %s", msg);
                return ok;
            }
            if (op == 24) {
                // hidden tests: gear 24 <percent>: our rain cover's durability (section 28) in both copies of the player's data
                // (the game reads the first and commits through the second), to wear it out in a run as a user's long
                // timefall did; the game's own timefall update then breaks it
                std::uint8_t worn = 0;
                std::int32_t level = 0;
                float dur = 0, max = 0;
                if (!ReadCover(inv, &worn, &level, &dur, &max)) {
                    std::snprintf(msg, cap, "the rain cover's state cannot be read (sites %s)", g_cover.sites == 1 ? "ok" : "off");
                    return false;
                }
                if (!worn) {
                    std::snprintf(msg, cap, "no rain cover on our backpack");
                    return false;
                }
                const double pc = v[1] < 0 ? 0 : v[1] > 100 ? 100 : v[1];
                const float to = static_cast<float>(max * pc / 100.0);
                SIZE_T w1 = 0, w2 = 0;
                const std::uintptr_t blk = CoverBlock(inv);
                const bool ok = WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(blk + kCoverDur), &to, sizeof(to), &w1) &&
                                w1 == sizeof(to) &&
                                WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(blk + kPDataSize + kCoverDur), &to, sizeof(to), &w2) &&
                                w2 == sizeof(to);
                std::snprintf(msg, cap, "TEST: our rain cover (level %d) durability %.1f -> %.1f of %.1f (%.2f %%)%s", level + 1, dur, to, max, pc,
                              ok ? "" : " (write failed)");
                Log("[gear] %s", msg);
                g_cover.next_ms = 0;
                return ok;
            }
            if (op == 15) {
                const std::uint8_t c = static_cast<std::uint8_t>(v[1] < 0 ? 0 : v[1] > 24 ? 24 : v[1]);
                std::uint8_t was = 0;
                Rd(inv + 0x4BE0, was);
                const bool ok = put(inv + 0x4BE0, c);
                std::snprintf(msg, cap, "TEST: our cap colour %u -> %u%s", was, c, ok ? "" : " (write failed)");
                Log("[look] %s", msg);
                return ok;
            }
            const std::uint8_t want_type = static_cast<std::uint8_t>(v[1]);
            unsigned changed = 0, listed = 0;
            for (unsigned i = 0; i < 64; ++i) {
                const std::uintptr_t ent = inv + 0x37F9 + 0x40ull * i;
                std::uint8_t type = 0, worn = 0;
                if (!Rd(ent, type) || !Rd(ent + 0x1D, worn) || type == 0) continue;
                const std::uintptr_t item = Q(inv + 0x3818 + 0x40ull * i);
                float f34 = -1;
                if (item) Rd(item + 0x34, f34);
                if (++listed <= 64) Log("[look] accessory entry %u: type 0x%02X worn %u item %p (+0x34 %.2f)", i, type, worn, reinterpret_cast<void*>(item), f34);
                if (write && type == want_type && changed == 0) {
                    const std::uint8_t to = static_cast<std::uint8_t>(v[2] != 0 ? 1 : 0);
                    if (put(ent + 0x1D, to)) ++changed;
                }
            }
            std::snprintf(msg, cap, "%u accessory entries listed%s", listed, write ? (changed ? "; TEST: one entry's worn flag set" : "; no entry of that type") : "");
            Log("[look] %s", msg);
            return true;
        }
        case 12:
            g_looks_on = v[1] != 0;
            if (!g_looks_on)
                for (PropSet& s : g_sets)
                    for (Prop& p : s.props) p.look_hash = 0;
            std::snprintf(msg, cap, "gear looks (colours, variations) %s", g_looks_on ? "on" : "off");
            Log("[look] %s", msg);
            return true;
        case 22:
            // her hat in the rain (section 26.8): the worn hat the game hides under Sam's hood drawn on her, on / off
            g_hood_on = v[1] != 0;
            std::snprintf(msg, cap, "her hat in the rain %s (our character %s; sites %s; now %s, hood state %d)", g_hood_on ? "on" : "off",
                          g_own_hoodless ? "has no hood" : "has a hood: not used", g_hood_sites < 0 ? "not checked yet" : g_hood_sites ? "ok" : "MISMATCH",
                          g_hood.e ? g_hood.name : "no hat hidden", g_hood.hood);
            Log("[headgear] %s", msg);
            return true;
        case 16:
            // Sam's head parts (hair, hood, face lines) named in our frames: off / on (section 14). Off only
            // stops naming them: a partner body keeps what it was last given (set it before the body comes).
            g_head_on = v[1] != 0;
            std::snprintf(msg, cap, "head parts %s (%s; %u reads, %u held; mirrored on the partner: %u part(s) set)", g_head_on ? "sent" : "not sent",
                          g_head.why, g_head.reads, g_head.held, g_pm.applied);
            Log("[gear] %s", msg);
            return true;
        case 17:
            // weapons (section 17): read and sent / not (the partner's props of them follow)
            g_weapons_on = v[1] != 0;
            g_next_read_ms = 0;
            std::snprintf(msg, cap, "weapons %s (watch %s, %u changes seen, %u read after the 40 ms gap; draw / holster to frame: mean %.0f ms, max %.0f ms over %u)",
                          g_weapons_on ? "sent" : "not sent", g_ww.ok ? "on" : "off", g_ww.changes, g_ww.deferred, g_ww.lat_n ? g_ww.lat_sum / g_ww.lat_n : 0.0,
                          g_ww.lat_max, g_ww.lat_n);
            Log("[gear] %s", msg);
            return true;
        case 18:
            // the self view headgear's mover writes (section 20): off puts every accessory's mover back to the identity
            g_hg_on = v[1] != 0;
            std::snprintf(msg, cap, "headgear mover offsets %s (writes %u, restores %u, changed by the game %u, refused %u)", g_hg_on ? "on" : "off",
                          g_hg.writes, g_hg.restores, g_hg.game_changed, g_hg.refusals);
            Log("[gear] %s", msg);
            return true;
        case 21:
            // boots sync (section 21) at run time: our boots item sent / not, the partner's boots prop drawn / not
            g_boots_sync = v[1] != 0;
            g_next_read_ms = 0;
            std::snprintf(msg, cap, "boots %s", g_boots_sync ? "sent and drawn on the partner" : "not sent or drawn");
            Log("[gear] %s", msg);
            return true;
        case 25: {
            // her boots' style (section 30) at run time: 0 prop (Sam's boots on her feet), 1 own (her own boots), 2 log
            if (v[1] == 0 || v[1] == 1) {
                g_boots_style = v[1] != 0 ? bootslook::Style::own : bootslook::Style::prop;
                g_next_read_ms = 0;
            }
            std::snprintf(msg, cap, "boots_look %s (partner: %s; self: Sam's boots %s, hidden by us %s, sets %u clears %u reasserts %u; parts %08X sent %08X)",
                          bootslook::StyleName(g_boots_style),
                          texshare::PartnerBootsStyle() < 0 ? "ours" : texshare::PartnerBootsStyle() ? "own (the partner's look)" : "prop (the partner's look)",
                          g_boots.shown ? "shown" : "not shown", g_sbh.e ? "yes" : "no", g_sbh.sets, g_sbh.clears, g_sbh.reasserts, g_boots_visible & g_boots_apply,
                          g_boots_sent_visible);
            Log("[gear] %s", msg);
            return true;
        }
        case 26: {
            // held items (section 31): 0 off (cutscene-prop weapons only), 1 the ladder / PCC / climbing anchor, 2 every item in a
            // hand; any other value: status only. Both sides (sender and receiver) follow it.
            if (v[1] == 0 || v[1] == 1 || v[1] == 2) {
                g_held_mode = static_cast<heldrules::Mode>(static_cast<int>(v[1]));
                for (NodeMemo& m : g_memo) m = NodeMemo{};  // the walk's "not sent" verdicts depend on the mode
                g_next_read_ms = 0;
            }
            char info[300];
            held::Info(info, sizeof(info));
            unsigned live = 0, shown = 0;
            for (const PropSet& s : g_sets)
                for (const Prop& p : s.props)
                    if (IsHeldKey(p.def)) live += p.live, shown += p.shown;
            std::snprintf(msg, cap, "held_items %d; %s; held props %u live, %u shown; %u made here, %u not made; the item's own pose: %u sent, %u received, %u written",
                          static_cast<int>(g_held_mode), info, live, shown, g_held_made, g_held_failed, g_hposes_sent, g_hposes_rx, g_hpose_writes);
            Log("[gear] %s", msg);
            // where each held clone is against the joint its item names, and the skeletons (ours in hand, the clone's)
            for (unsigned i = 0; i < g_local.count; ++i) {
                const wire::GearItem& it = g_local.items[i];
                if (it.kind != wire::GearKind::weapon || !g_local_e[i]) continue;
                std::uintptr_t sk = 0;
                std::uint32_t lay = 0;
                std::int32_t c = 0;
                const bool have = look::PoseSkeleton(g_local_e[i], &sk, &lay, &c);
                char js[200];
                HeldJoints(g_local_e[i], have ? c : 0, js, sizeof(js));
                Log("[gear]   ours: weapon item %u '%s' on %s %u rel (%.3f, %.3f, %.3f) m: skeleton %s %08X, %d joints; %s", i,
                    it.res < g_res_local_n ? g_res_local[it.res].path : "?", it.parent == wire::kGearNoParent ? "joint" : "item",
                    it.parent == wire::kGearNoParent ? it.joint : it.parent, it.p[0], it.p[1], it.p[2], have ? "ok" : "unreadable", lay, c, js);
            }
            for (const PropSet& s : g_sets)
                for (unsigned i = 0; i < s.want.count; ++i) {
                    const Prop& p = s.props[i];
                    if (!IsHeldKey(p.def) || !p.live || !holo::SpawnAlive(p.sp)) continue;
                    const auto e = reinterpret_cast<std::uintptr_t>(p.sp.e);
                    Xf w, bw, jw;
                    const bool hw = ReadWorld(e, &w);
                    const wire::GearItem& it = s.want.items[i];
                    const bool hj = s.body && it.parent == wire::kGearNoParent && ReadWorld(reinterpret_cast<std::uintptr_t>(s.body), &bw) &&
                                    JointWorld(s.body, bw, it.joint, &jw);
                    std::uintptr_t sk = 0;
                    std::uint32_t lay = 0;
                    std::int32_t c = 0;
                    const bool have = look::PoseSkeleton(e, &sk, &lay, &c);
                    std::uint64_t fl = 0;
                    partner::ReadFlags(p.sp.e, &fl);
                    char js[200];
                    HeldJoints(e, have ? c : 0, js, sizeof(js));
                    Log("[gear]   %s held clone %u %p '%s': at %.2f %.2f %.2f%s, %.3f m from the body's joint %u; flags 0x%llX; skeleton %s %08X, %d joints; "
                        "pose target %s; %s",
                        s.tag, i, reinterpret_cast<void*>(e), KeyLabel(p.def, s.res), hw ? w.p[0] : 0.0, hw ? w.p[1] : 0.0, hw ? w.p[2] : 0.0, hw ? "" : " (unreadable)",
                        hw && hj ? Dist(w, jw) : -1.0, it.joint, static_cast<unsigned long long>(fl), have ? "ok" : "unreadable", lay, c,
                        s.pose_e[kHeldPoseTarget] == e ? "this clone" : "none", js);
                }
            return true;
        }
        case 27:
        case 28:
        case 29: {
            // Sam's skeleton on her (section 32): 27 the carrier 0 off / 1 on / 2 status; 28 the skeleton pieces we read replaced
            // by a type's (hidden tests: -1 off, 0 none, 1 speed, 2 all-terrain, 3 power, 4 support); 29 LegOut on her 0 / 1
            const int a = static_cast<int>(v[1]);
            if (op == 27 && (a == 0 || a == 1)) g_exo_on = a != 0;
            if (op == 27 && (a == 3 || a == 4)) realbody::SetExoSelfSource(a == 3 ? 1 : 0);
            if (op == 28 && a >= -1 && a < exo::kTypeCount) {
                g_exo_force = a;
                g_next_read_ms = 0;
            }
            if (op == 29 && (a == 0 || a == 1)) {
                g_exo_leg = a != 0;
                for (PropSet& s : g_sets) s.exo.next_parts_ms = 0;
            }
            char c0[200], c1[200];
            realbody::ExoCounters(realbody::GearSlot::partner, c0, sizeof(c0));
            realbody::ExoCounters(realbody::GearSlot::self, c1, sizeof(c1));
            const int ours = g_exo_porter ? exo::TypeOfParts(g_parts_visible) : -3;
            std::snprintf(msg, cap,
                          "exo_carrier %s, forced type %d, LegOut %s, self source %s; our skeleton %s; partner carrier %s%s%s (%s); self carrier %s%s%s (%s)",
                          g_exo_on ? "on" : "off", g_exo_force, g_exo_leg ? "on" : "off", realbody::ExoSelfSource() == 1 ? "Sam's newest" : "the body's applied",
                          ours >= 0 ? exo::TypeName(static_cast<exo::Type>(ours)) : ours == -1 ? "pieces of no known type" : "not read",
                          g_sets[0].exo.p.live ? "live" : g_sets[0].exo.p.loading ? "loading" : "none", g_sets[0].exo.p.shown ? " shown" : "",
                          g_sets[0].exo.refused ? " refused" : "", c0, g_sets[1].exo.p.live ? "live" : g_sets[1].exo.p.loading ? "loading" : "none",
                          g_sets[1].exo.p.shown ? " shown" : "", g_sets[1].exo.refused ? " refused" : "", c1);
            Log("[exo] %s", msg);
            return true;
        }
        case 30: {
            // the BB pod under the self view (section 35): 3 the game's rule (the default), 1 the fallback only, any other value (0,
            // 2, no argument): the status only
            const int a = static_cast<int>(v[1]);
            if (a == 1 || a == 3) {
                g_pod_rule_on = a == 3;
                g_next_read_ms = 0;
            }
            // the pod in our sent frame, in the partner's received frame, and the self view's pod props (wanted / live)
            auto pods = [](const wire::GearFrame& f, unsigned n) {
                unsigned c = 0;
                for (unsigned i = 0; i < n && i < f.count; ++i) c += f.items[i].kind == wire::GearKind::bb_pod;
                return c;
            };
            unsigned self_live = 0;
            for (unsigned i = 0; i < g_sets[1].want.count; ++i)
                self_live += g_sets[1].want.items[i].kind == wire::GearKind::bb_pod && g_sets[1].props[i].live;
            char where[160];
            std::snprintf(where, sizeof(where), "; our gear: BB pod %u; the partner's gear: BB pod %u; self view: BB pod %u wanted, %u live",
                          g_have_local ? pods(g_local, g_local.count) : 0u, g_sets[0].have_want ? pods(g_sets[0].want, g_sets[0].want.count) : 0u,
                          g_sets[1].have_want ? pods(g_sets[1].want, g_sets[1].want.count) : 0u, self_live);
            char line[640];  // the action's own message is 160 bytes: the whole status goes to the log
            std::snprintf(line, sizeof(line),
                          "BB pod under the self view: %s (rule sites %s%s); now %s (%s); cross-check %u match, %u mismatch over %u reads with the self "
                          "view off; fallback %s (%s)%s",
                          g_pod_rule_on ? "the game's rule" : "the fallback only (gear 30 1; gear 30 3: the rule)", g_pod_sites == 1 ? "ok" : g_pod_sites == 0 ? "off" : "not checked",
                          g_pod.disproved ? ", disproved this session" : "", g_podlog.have ? (g_podlog.vis ? "shown" : "hidden") : "not read",
                          gearkeep::PodSourceName(g_podlog.src), g_pod.matched, g_pod.mismatched, g_pod.compared, g_pod.memo ? "shown" : "hidden",
                          g_pod.memo_plain ? "read in plain play" : "as worn", where);
            Log("[gear] %s", line);
            // the source the last decision actually used (the rule may be on but not read: the slot holds no pod of ours)
            const char* src = !g_podlog.have                                  ? "not read"
                              : g_podlog.src == gearkeep::PodSource::ask      ? "the rule"
                              : g_podlog.src == gearkeep::PodSource::flag     ? "its flag"
                              : g_podlog.src == gearkeep::PodSource::room     ? "the private room"
                              : !g_pod_rule_on                                ? "the fallback: gear 30 1"
                              : g_pod_sites != 1 || g_pod.disproved           ? "the fallback: rule off"
                                                                              : "the fallback: the rule not read";
            std::snprintf(msg, cap, "BB pod %s (%s)%s", g_podlog.have ? (g_podlog.vis ? "shown" : "hidden") : "not read", src, where);
            return true;
        }
        case 31:
        case 32:
        case 33: {
            // her rain hood (section 37): 31 0 off / 1 the timed swap / 2 the unfold where the carrier can play it, any other value
            // the status; 32 <state 0..12 | -1> [type 0 porter, 1 oxi, 2 sea otter] our hood reading replaced (hidden tests; both screens follow); 33 her
            // hair under the hood 0 shown / 1 hidden
            const int a = static_cast<int>(v[1]);
            if (op == 31 && a >= 0 && a <= 2) g_hood_mode = a;
            if (op == 32 && a >= -1 && a <= wire::kGearHoodStateMax) {
                g_hood_force_state = a;
                const int t = static_cast<int>(v[2]);
                if (t >= 0 && t < exo::kHoodTypes) g_hood_force_type = t;
                g_next_read_ms = 0;
            }
            if (op == 33 && (a == 0 || a == 1)) g_hood_hair = a != 0;
            auto slot = [](const PropSet& s, char* out, std::size_t n) {
                const PropSet::Exo& x = s.exo;
                std::snprintf(out, n, "carrier %s, hood %s (part %d, %s, slots %d, %u calls, %u faults), hair %s", x.p.live ? (x.p.shown ? "shown" : "live") : "none",
                              exo::HoodShowName(x.hood.show), x.hood_part, x.hood.mode == 1 ? "unfolding" : x.hood.mode == 2 ? "folding" : "idle", x.anim_ok,
                              x.hood_calls, x.hood_faults, x.hair_hidden ? "hidden" : x.hair_n ? "shown" : "no part");
            };
            char c0[200], c1[200];
            slot(g_sets[0], c0, sizeof(c0));
            slot(g_sets[1], c1, sizeof(c1));
            const PropSet& ps = g_sets[0];
            char line[700];
            std::snprintf(line, sizeof(line),
                          "her hood %s, hair %s; sites %s / lookup %s / unfold %s%s; ours: %s %s (state %d)%s; the partner's: %s; partner %s; self %s",
                          g_hood_mode == 0 ? "off" : g_hood_mode == 1 ? "on (the timed swap)" : "on (the unfold)", g_hood_hair ? "hidden under it where a hair part is found" : "shown",
                          g_hood_read_sites == 1 ? "ok" : g_hood_read_sites == 0 ? "MISMATCH" : "not checked", g_hlook.ok == 1 ? "ok" : g_hlook.ok == 0 ? "MISMATCH" : "not checked",
                          g_hlook.anim == 1 ? "ok" : g_hlook.anim == 0 ? "MISMATCH" : "not checked", g_hlook.faulted ? " (FAULTED)" : "",
                          exo::HoodTypeName(g_hoodr.type), exo::HoodPhaseName(exo::HoodPhaseOf(g_hoodr.state)), g_hoodr.state,
                          g_hood_force_state >= 0 ? " (gear 32)" : "",
                          ps.have_want && (ps.want.hood & wire::kGearHoodRead) ? exo::HoodPhaseName(exo::HoodPhaseOf(ps.want.hood & 0x7F)) : "no reading", c0, c1);
            Log("[hood] %s", line);
            std::snprintf(msg, cap, "her hood %s; ours %s (state %d); partner hood %s; self hood %s", g_hood_mode ? "on" : "off",
                          exo::HoodPhaseName(exo::HoodPhaseOf(g_hoodr.state)), g_hoodr.state, exo::HoodShowName(g_sets[0].exo.hood.show),
                          exo::HoodShowName(g_sets[1].exo.hood.show));
            return true;
        }
        case 20: {
            // the partner body's feet under the boots prop (section 21): scale percent, 100 = not scaled
            g_pfeet_pct = v[1] < 1 ? 1 : v[1] > 100 ? 100 : static_cast<int>(v[1]);
            char c[200];
            realbody::BootsCounters(c, sizeof(c));
            std::snprintf(msg, cap, "partner feet under the boots prop: scale %d%% (%s)", g_pfeet_pct, c);
            Log("[gear] %s", msg);
            return true;
        }
        case 19:
            // the headgear meter (section 20) on / off
            g_hm_on = v[1] != 0 ? 1 : 0;
            std::snprintf(msg, cap, "headgear meter %s", g_hm_on ? "on" : "off");
            Log("[gear] %s", msg);
            return true;
        case 13:
            g_poses_on = v[1] != 0;
            if (!g_poses_on) ClearPoses(g_sets[0]);
            std::snprintf(msg, cap, "backpack pose sync %s", g_poses_on ? "on" : "off");
            Log("[gear] %s", msg);
            return true;
        case 9: {
            // census: 0 the local player, 1 the partner / debug body, 2 the self body
            game::Entity* who = nullptr;
            const int what = static_cast<int>(v[1]);
            if (what == 0) game::TryGetLocalPlayer(&who);
            else who = realbody::SlotEntity(what == 2 ? realbody::GearSlot::self : realbody::GearSlot::partner);
            Census(who, what == 0);
            std::snprintf(msg, cap, "census of %s %p logged", what == 0 ? "the local player" : what == 2 ? "the self body" : "the partner body",
                          static_cast<void*>(who));
            return who != nullptr;
        }
        default:
            std::snprintf(msg, cap, "unknown op %d (0 log, 1 partner 0/1, 2 self 0/1, 3 test cases n, 4 debug body 0/1, 5 n / 6 test faults, 7 level, 8 pod entity, 9 census 0/1/2, 10 walk cache 0/1, 11 looks 0/1, 12 looks off/on, 13 backpack pose off/on, 14 accessory list, 15 cap colour, 16 head parts off/on, 17 weapons off/on, 18 headgear offsets off/on, 19 headgear meter off/on, 20 partner feet scale percent, 21 boots sync off/on, 22 hat in the rain, 23 glasses colour, 24 rain cover durability percent, 25 boots look 0 prop / 1 own / 2 log, 26 held items, 27 exo carrier 0/1/2, 28 skeleton type -1..4, 29 LegOut 0/1, 30 BB pod rule 0/1/2, 31 her hood 0/1/2/3, 32 hood state -1..12 [type], 33 hair under the hood 0/1)", op);
            return false;
    }
}

}  // namespace st::gear

namespace st::game {
bool GearCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept { return gear::DebugCommand(op, v, msg, cap); }
}  // namespace st::game
