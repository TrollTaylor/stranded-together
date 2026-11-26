// Shots, ship A (shot_sync.hpp; docs/enemy-sync.md P3a).
#include "shot_sync.hpp"

#include <windows.h>
#include <psapi.h>
#include <intrin.h>

#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "automation.hpp"
#include "game_actions.hpp"
#include "link.hpp"
#include "log.hpp"
#include "nametags.hpp"
#include "overlay/overlay_input.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"
#include "enemy_combat.hpp"
#include "enemy_rules.hpp"
#include "enemy_sync.hpp"
#include "npc_damage.hpp"
#include "sam_damage.hpp"
#include "shot_wire.hpp"
#include "vehicle_share.hpp"
#include "world_share.hpp"

namespace st::shots {
namespace {
namespace mem = partner::mem;
using game::WorldTransform;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut)
constexpr std::uintptr_t kEnqueue = 0x142916860;        // DSBulletSystem::Enqueue(system, record 0xF0): one bullet; 16 a frame
constexpr std::uintptr_t kEnqueueSlots = 0x142916886;   // count +0xC < 16, serial +8, slot = system + count * 0xF0
constexpr std::uintptr_t kEnqueueCopy = 0x1429168BC;    // the record copied to slot + 0x10, the serial to slot + 0xEC
constexpr std::uintptr_t kCreateBullet = 0x14294AD40;   // the script export's thunk: the byte arguments, then jmp the body
constexpr std::uintptr_t kCreateBody = 0x14294D340;     // CreateBullet's body
constexpr std::uintptr_t kRegInsn = 0x14294D3FD;        // mov rdi, [rip -> 0x147BEB0F8]: the weapon / magazine registry
constexpr std::uintptr_t kSysInsn = 0x14294DBA9;        // mov rcx, [rip -> 0x147BEB108]; lea rdx, rec; call Enqueue
constexpr std::uintptr_t kRegGlobal = 0x147BEB0F8, kSysGlobal = 0x147BEB108;
constexpr std::uintptr_t kWeaponById = 0x1429252E0;     // registry +0x10 count, +0x18 list, key u8 entry +0x20 (0 when missing)
constexpr std::uintptr_t kMagById = 0x142923E60;        // registry +0x20 count, +0x28 list, key u8 entry +0x20 (list[-1] when missing)
constexpr std::uintptr_t kCurWeapon = 0x1427A4390;      // the inventory's current weapon (slot +0x1C78, instances +0xD0 + 0x70 * slot)
constexpr std::uintptr_t kHumanoidDmg = 0x1437393D0;    // Humanoid MsgDamage handler (MULEs, Demens, warriors)
constexpr std::uintptr_t kEnqueueCallers[] = {0x142918FC4, 0x14294CFA3, 0x14294DBB8, 0x142984727, 0x142DF8A90};
constexpr std::uintptr_t kRegDummy = 0x5D718;           // registry +0x5D718: the system dummy entity's reference (CreateBullet's owner)
constexpr std::uintptr_t kPlayerVtable = 0x3D7D0D8, kPInv = 0x54C8, kCompOwner = 0x48;
constexpr unsigned kHandJoint = 26;                     // MTP_R_Hand_a (the 77-joint layout)
// The drop at the bullet (round 16; docs/enemy-sync.md 7.5): every bullet hands its damage to what it hit through
// BulletDamageA (the hit processor's two calls) or B (the second bullet kind's): (bullet, receiver, hit, f32 amount,
// [rsp+0x28] u64), the DamageInfo built from Q(bullet + 0x38) (the replay set's key), then receiver->vt[+0x10](receiver, DI).
// The receiver of an entity is entity + 0x38 (the cast 0x1423816E0). The BT reaction (InvalidPhysicalAttackOnBT 0x182) is
// one more call from the hit processor (0x142992D80 at 0x14291D64E) that builds its own context: a thread-local set by a
// detour on the hit processor (rcx = the bullet) tells its detour that a replay is being processed.
constexpr std::uintptr_t kBulletDmgA = 0x142912120, kBulletDmgB = 0x142912380;
constexpr std::uintptr_t kEntityCast = 0x1423816E0;     // receiver -> entity: rcx - 0x38
constexpr std::uintptr_t kHitProc = 0x14291CBC0;        // the bullet hit processor (bullet, system, f32)
constexpr std::uintptr_t kBtReact = 0x142992D80;        // the BT reaction (hit, owner, u8, u32, [rsp+0x28] u16 attack)
constexpr std::uintptr_t kBtSite = 0x14291D637, kBtRet = 0x14291D653;
constexpr std::uintptr_t kBulletRef = 0x38, kBulletOwner = 0x20, kBulletHitFlag = 0x16C, kBulletState = 0x164;
constexpr std::uintptr_t kReceiverOfEntity = 0x38, kEntityParent = 0x70, kEntityComponents = 0x90;
// The cargo of the aimed test (hidden runs, `shots 6`): the baggage manager [0x147BF4968] keeps every DSBaggage2 (0x240 B) at
// +0x20 + index * 0x240 (+0x21 non-zero: in use); its script exports AddBaggageToTemporaryList(actor id: index in the low
// 13 bits) and PlaceTemporaryListBaggagesAroundPlayer(u8) take a piece off its holder (+0x90) and put it on the ground.
constexpr std::uintptr_t kBagMgrGlobal = 0x147BF4968, kBagAddTemp = 0x142CD1380, kBagPlaceThunk = 0x142CD16F0, kBagPlaceBody = 0x142CD8F90;
constexpr std::uintptr_t kBagArray = 0x20, kBagStride = 0x240, kBagTempList = 0x46DDA8;
constexpr unsigned kBagMax = 8000;  // the array ends before the temporary list (+0x46DDA8)
constexpr std::uintptr_t kBagInUse = 0x21, kBagLife = 0x34, kBagPos = 0x38, kBagHolder = 0x90, kBagEntityIdx = 0xD8;

enum class Group : std::uint8_t { core, npc, hit, bt, cargo };
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    Group group;  // core: shots off on a mismatch; npc: the Humanoid drop; hit: the drop at the bullet; bt: the BT reaction; cargo: shots 6
};
// clang-format off
constexpr Site kSites[] = {
    {kEnqueue,      "40 53 55 48 83 EC 28 48 89 74 24 50 48 8D 99 18 A0 02 00 48 8B F1 4C 89", "the bullet Enqueue", Group::core},
    {kEnqueueSlots, "4C 63 46 0C 41 83 F8 10 7C 0A BD FF FF 00 00 E9 41 01 00 00 8B 6E 08 49 8B D6 48 89 7C 24 58 49 69 F8 F0 00 00 00", "the Enqueue's slots", Group::core},
    {kEnqueueCopy,  "48 8D 4F 10 E8 7B 80 FF FF 89 AF EC 00 00 00 48 8B 4F 30", "the Enqueue's record copy and serial", Group::core},
    {kCreateBullet, "0F B6 44 24 70 88 44 24 70 0F B6 44 24 68 88 44 24 68 0F B6 44 24 60 88 44 24 60 0F B6 44 24 58 88 44 24 58 "
                    "0F B6 44 24 50 88 44 24 50 0F B6 44 24 48 88 44 24 48 0F B6 44 24 40 88 44 24 40 0F B6 44 24 38 88 44 24 38 "
                    "0F B7 44 24 30 66 89 44 24 30 E9 A9 25 00 00", "CreateBullet (the thunk's argument layout)", Group::core},
    {kCreateBody,   "40 56 57 41 54 41 55 41 57 48 81 EC E0 03 00 00 C5 78 29 94 24 90 03 00", "CreateBullet's body", Group::core},
    {kRegInsn,      "48 8B 3D F4 DC 29 05", "the registry global in CreateBullet", Group::core},
    {kSysInsn,      "48 8B 0D 58 D5 29 05 48 8D 94 24 40 02 00 00 E8 A3 8C FC FF", "the bullet system global and CreateBullet's Enqueue call", Group::core},
    {kWeaponById,   "48 89 5C 24 08 48 63 41 10 45 33 D2 48 8B D9 85 C0 7E 24 4C 8B 49 18 4C 8B D8 45 8B C2 0F 1F 00 49 8B 01 3A 50 20", "WeaponById (+0x10 / +0x18, key +0x20)", Group::core},
    {kMagById,      "48 89 5C 24 08 48 63 41 20 45 33 D2 48 8B D9 85 C0 7E 24 4C 8B 49 28 4C 8B D8 45 8B C2 0F 1F 00 49 8B 01 3A 50 20", "MagazineById (+0x20 / +0x28, key +0x20)", Group::core},
    {kCurWeapon,    "0F B6 81 78 1C 00 00 48 6B D0 70 48 8B 84 0A D0 00 00 00 33 C9 80 78 12", "the current-weapon getter", Group::core},
    {kHumanoidDmg,  "40 55 57 41 54 41 56 48 81 EC 68 01 00 00 C5 F8 29 B4 24 50 01 00 00 48", "Humanoid MsgDamage handler", Group::npc},
    {kBulletDmgA,   "40 53 55 56 57 41 56 48 81 EC 30 01 00 00 C5 F8 29 B4 24 20 01 00 00", "BulletDamageA", Group::hit},
    {kBulletDmgA + 0x48, "48 8B 47 38", "BulletDamageA's reference read (bullet + 0x38)", Group::hit},
    {kBulletDmgB,   "40 53 55 56 57 41 56 48 81 EC 30 01 00 00 C5 F8 29 B4 24 20 01 00 00", "BulletDamageB", Group::hit},
    {kBulletDmgB + 0x48, "48 8B 47 38", "BulletDamageB's reference read (bullet + 0x38)", Group::hit},
    {0x14291D10A,   "C5 FA 10 59 24 48 8B CE 48 89 44 24 20 E8 04 50 FF FF", "the hit processor's first BulletDamageA call", Group::hit},
    {0x14291D68C,   "C5 FA 10 59 24 48 8B CE 48 89 44 24 20 E8 82 4A FF FF", "the hit processor's second BulletDamageA call", Group::hit},
    {0x14294172A,   "C5 FA 10 59 24 48 8B CF 48 89 44 24 20 E8 44 0C FD FF", "the BulletDamageB call", Group::hit},
    {kEntityCast,   "33 D2 48 8D 41 C8 48 85 C9 48 0F 44 C2 C3", "the receiver -> entity cast (- 0x38)", Group::hit},
    {kHitProc,      "4C 8B DC 56 41 57 48 81 EC 58 07 00 00 C4 41 78 29 9B 78 FF FF FF 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 D0 06 00 00 "
                    "80 B9 6C 01 00 00 00 4C 8B FA C5 78 28 DA 48 89 54 24 70 48 8B F1 0F 84 ?? ?? ?? ?? 83 B9 64 01 00 00 02", "the bullet hit processor", Group::bt},
    {kBtReact,      "48 89 5C 24 20 55 56 41 54 41 56 41 57 48 81 EC 20 01 00 00", "the BT reaction", Group::bt},
    {kBtSite,       "44 0F B6 46 58 48 8D 50 E0 B8 82 01 00 00 48 0F 44 D3 66 89 44 24 20 E8 2D 57 07 00", "the BT reaction's call (attack 0x182)", Group::bt},
    {kBagAddTemp,   "40 53 48 83 EC 20 48 8B 1D DB 35 F2 04 81 E1 FF FF 1F 00 8B C1 81 F9 FF FF 1F 00 74 30 25 FF 1F 00 00 48 8D 04 C0 48 C1 E0 06 "
                    "80 7C 18 41 00 74 1C 48 8D 53 20 48 03 D0 74 13 48 8D 8B A8 DD 46 00", "AddBaggageToTemporaryList", Group::cargo},
    {kBagPlaceThunk, "E9 9B 78 00 00", "PlaceTemporaryListBaggagesAroundPlayer (the export)", Group::cargo},
    {kBagPlaceBody, "4C 8B DC 56 48 81 EC 90 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 80 00 00 00 48 8B 05 B4 B9 F1 04 0F B6 F1 F6 80 D9 E0 46 00 20",
                    "PlaceTemporaryListBaggagesAroundPlayer", Group::cargo},
};
// clang-format on

using FnEnqueue = int (*)(std::uintptr_t sys, std::uintptr_t rec);
using FnCreate = void (*)(const double* origin, const double* target, std::uint32_t actor, float speed, std::uint32_t a5, std::uint16_t attack,
                          std::uint8_t a7, std::uint8_t p8, std::uint8_t p9, std::uint8_t p10, std::uint8_t p11, std::uint8_t p12, std::uint8_t weapon,
                          std::uint8_t mag);
using FnDamage = void (*)(std::uintptr_t, std::uintptr_t);

// ---------------------------------------------------------------- config / state
Config g_cfg;
bool g_checks_ok = false, g_npc_checks_ok = false;
unsigned g_checks_n = 0, g_checks_pass = 0;
bool g_hooked = false, g_npc_hooked = false, g_filter_registered = false;
std::atomic<bool> g_on{false};        // capture, send, replay (shots 1 / 2 live)
std::atomic<bool> g_stopped{false};   // a fault: off for the session
char g_off[200] = "shots_sync=0";
FnEnqueue o_enqueue = nullptr;
std::atomic<std::uintptr_t> g_sam{0};    // our Sam (the tick)
std::atomic<std::uintptr_t> g_dummy{0};  // the bullet system's dummy owner entity (CreateBullet's bullets; diagnostics only)
double g_now = 0;
// Enemy shots (enemy sync P1a, docs/enemy-sync.md 8): wanted (enemy_sync and enemy_shots), live (enemy_sync's gate: 1 the host
// captures and sends, 2 the guest replays), the host's shooters (its 10 Hz class: entity -> actor id, published by enemy_sync),
// the frozen mirrors' own records dropped in the Enqueue.
bool g_enemy_want = false;
std::atomic<int> g_enemy_role{0};
erules::KeySet g_shooters;
std::atomic<unsigned> g_mirror_seen{0}, g_mirror_dropped{0};
std::uint8_t g_enemy_parts = 0, g_enemy_freeze = 0;
// P1c (round 21): the attack ids above 0x18C (weapon 92's 0x191). This game's attack registry as a bit set, read once it is
// plausible (50+ entries, 0x57 listed); a replay or a capture takes an id above 0x18C only when it is listed here, and a send
// drops such a shot for a partner whose beacon has no kPartAttackWide (an old build's decoder refuses the whole frame).
bool g_attack_want = true;   // enemy_attack_registry
std::uint8_t g_attack_bits[64] = {};
bool g_attack_ok = false;
double g_attack_next = 0;
unsigned g_attack_tries = 0, g_attack_narrowed = 0, g_attack_refused_logged = 0;
std::uint16_t g_enemy_count = 0;
std::uint32_t g_enemy_ids = 0, g_enemy_guids = 0;
shotw::Beacon g_peer_beacon;
// the enemy shots' counters (game thread) and the kept enemy rounds on our Sam (worker threads)
unsigned g_e_sent = 0, g_e_pulls = 0, g_e_frames = 0, g_e_capped = 0, g_e_no_peer = 0, g_e_rx = 0, g_e_replayed = 0, g_e_muzzle = 0, g_e_mag = 0, g_e_late = 0;
unsigned g_e_nothold = 0;  // round 23 B: replays from a not-holding (hidden) copy's owner place
unsigned g_e_ref_shooter = 0, g_e_ref_origin = 0, g_e_ref_off = 0, g_e_hold_full = 0, g_e_logged = 0;
std::atomic<unsigned> g_kept_enemy{0}, g_drop_enemy{0}, g_kept_enemy_logged{0};
shotw::PullGroups g_e_pulls_by;
shotw::EnemyRateCap g_e_cap;
shotw::Shot g_last_enemy_rx;
bool g_have_enemy_rx = false;
std::uint32_t g_last_enemy_shooter = 0;  // the newest replayed (the guest) or sent (the host) enemy shot's shooter (enemy_look -2)
unsigned g_e_pair_id[16] = {}, g_e_pair_n = 0;  // (weapon << 8 | mag) logged once each
// enemy_shot_test (hidden runs, the guest)
std::atomic<bool> g_et_armed{false}, g_et_nohurt{false};
std::atomic<int> g_et_match{0}, g_et_kept{-1};
std::atomic<float> g_et_amount{0};
double g_et_t0 = 0;
float g_et_h0 = 0;
bool g_et_wait = false;
std::uint32_t g_et_id = 0;

// the Enqueue detour's capture ring (any thread; under g_cap_lock)
constexpr std::size_t kRecSize = 0xF0;
struct Cap {
    std::uint8_t rec[kRecSize];
    double ms;
    std::uint32_t shooter;  // shotw::kShooterSam, or an enemy's actor id (the host's enemy shots)
    std::uintptr_t owner;   // the record's owner entity
};
constexpr unsigned kCapRing = 64;
SRWLOCK g_cap_lock = SRWLOCK_INIT;
Cap g_cap[kCapRing];
unsigned g_cap_head = 0, g_cap_count = 0;
std::atomic<unsigned> g_captured{0}, g_cap_full{0}, g_others{0}, g_det_faults{0}, g_echo_skips{0};
std::atomic<unsigned> g_diag_enq{0}, g_diag_ours{0};
// the probe's owner table
std::atomic<bool> g_own_armed{false};
constexpr unsigned kOwnRing = 16;
struct OwnRound {
    float o[3], v[3];  // the record's origin (+0x60) and velocity (+0x70)
};
struct OwnCount {
    std::uintptr_t e;
    unsigned n;
    OwnRound ring[kOwnRing];  // round n (1-based) at ring[(n - 1) % kOwnRing] (the L1 probe's real round lines)
};
OwnCount g_own[16];

// our replays (game thread): what the detour patches in, and what it found
struct Patch {
    std::uint8_t r05 = 0, r58 = 0, r5A = 0, rD0 = 0, rD1 = 0;
    float r94 = 0;
    bool on = false;
};
thread_local bool t_replaying = false;
thread_local Patch t_patch;
thread_local bool t_enqueued = false;
thread_local std::uint64_t t_refs[shotw::kReplayRefs] = {};
thread_local unsigned t_nrefs = 0;
thread_local std::uint8_t t_slot_rec[kRecSize];
thread_local bool t_have_slot = false;

// the replay set (the damage detours read it on their threads)
SRWLOCK g_rs_lock = SRWLOCK_INIT;
shotw::ReplaySet g_rs;

// damage verdicts (worker threads)
std::atomic<unsigned> g_drop_sam_ref{0}, g_seen_sam_dummy{0}, g_drop_npc{0}, g_kept_ff{0}, g_kept_test{0};
// the drop at the bullet (worker threads; round 16)
using FnBulletDmg = void (*)(std::uintptr_t bullet, std::uintptr_t receiver, std::uintptr_t hit, float amount, std::uint64_t w);
using FnHitProc = void (*)(std::uintptr_t bullet, std::uintptr_t sys, float dt);
using FnBtReact = std::uint64_t (*)(std::uintptr_t hit, std::uintptr_t owner, std::uint8_t b58, std::uint32_t e0, std::uint16_t attack);
FnBulletDmg o_bullet_a = nullptr, o_bullet_b = nullptr;
FnHitProc o_hitproc = nullptr;
FnBtReact o_bt = nullptr;
bool g_hit_checks_ok = false, g_bt_checks_ok = false, g_cargo_checks_ok = false;
bool g_hit_hooked = false, g_bt_hooked = false;
std::atomic<double> g_last_replay_ms{-1e18};  // the newest replay's enqueue (the detours' fast exit when none can be in flight)
std::atomic<bool> g_aimed_keep{false};        // shots 5 with a negative count: the aimed replays' damage let through (control)
std::atomic<unsigned> g_hd_seen{0}, g_hd_sam{0}, g_hd_kept_test{0}, g_hd_faults{0}, g_hd_bt{0}, g_hd_logged{0}, g_hd_part{0};
std::atomic<unsigned> g_hd_drop[shotw::kHitClasses] = {};
// enemy sync's guard at the bullet (round 16 review): any bullet's hit on a frozen mirror (or a part of one) dropped while the
// guard is on; and the replay-set matches on our Sam whose attacker is not the dummy (a reused reference: kept)
std::atomic<unsigned> g_hd_mirror{0}, g_kept_reuse{0};
// ... by whose bullet (round 16b: the guard step wants a drop of her own rifle's rounds, not of the replays'): our Sam's own
// rounds, a replay's; each kind logged up to 8 times
std::atomic<unsigned> g_hd_mirror_ours{0}, g_hd_mirror_rep{0}, g_hd_mirror_logged{0}, g_hd_mirror_logged_ours{0};
// P2 (docs/enemy-sync.md 9): her own rounds on a mirror passed to Entity OnHit (the HIT goes to the host), and those on a part with
// no entity receiver (dropped, no HIT)
std::atomic<unsigned> g_hd_mirror_pass{0}, g_hd_mirror_noent{0}, g_hd_mirror_pass_logged{0};
// the beacon's hits part (enemy_combat: enemy_hits on, OnHit hooked, not stopped) and why it is off
std::atomic<bool> g_hits_part{false};
std::atomic<bool> g_restrain_part{false};  // round 23: HIT kind 5 (enemy_combat's SetRestrainPart)
std::uint8_t g_player_down = 0;            // round 23 C: our player's down code (enemy_sync's SetPlayerDown), game thread
std::uint8_t g_down_sent = 0;              // ... the code the beacon last carried
// round 23 review: the partner's beacon carried kPartRestrain at least once on this link generation. Byte 22 is a decoder
// capability of its build (any round-23 build decodes it), so it is never withdrawn when the partner's restrain part drops later
// (a fault, its hits switch, its CarryReady): a 0 sent for that reason would read as "back" there. Reset on a new link generation.
bool g_peer_down_cap = false;
unsigned g_flushed = 0;                     // round 23: enemy replays dropped because their shooter went down
const char* g_hits_why = "not in a world yet)";
std::atomic<unsigned> g_hd_kept_cls[shotw::kHitClasses] = {};
thread_local bool t_hit_replay = false;  // the hit processor runs one of our replays' bullets now (the BT reaction's detour reads it)
std::atomic<unsigned> g_dmg_logged{0};
// The enemy replays' first hits (round 16b: in run 20261124-104304 31 of 37 replay hits landed on NPCs and no still showed a
// MULE's tracer on the guest's screen): each enemy replay's references with its shooter (the mirror), start, direction and
// time (the game thread notes them), and the drop at the bullet names the first thing each one hits: the shooting mirror
// itself, another frozen mirror, the partner body, our Sam or something else, and how far along the shot it is.
struct EnemyRep {
    std::uint64_t ref[shotw::kReplayRefs] = {};
    std::uintptr_t shooter = 0;
    std::uint32_t id = 0;
    unsigned num = 0;
    double origin[3] = {};
    double dir[3] = {};
    double speed = 0;
    double t = -1e18;
    bool hit = false;
};
constexpr unsigned kEnemyReps = 64;
SRWLOCK g_er_lock = SRWLOCK_INIT;
EnemyRep g_er[kEnemyReps];
unsigned g_er_head = 0;
enum EnemyHitWhat : unsigned { kEhShooter, kEhMirror, kEhBody, kEhSam, kEhOther, kEhN };
const char* const kEhName[kEhN] = {"the shooting mirror itself", "another frozen mirror", "the partner body", "our Sam", "something else"};
std::atomic<unsigned> g_eh[kEhN] = {};
// P1b (docs/enemy-sync.md 10): the REAL rounds of the enemies this game owns (not replays) by what they hit at the bullet: our
// Sam, the partner body, something else (does her owned MULE notice the host's body?)
std::atomic<unsigned> g_oh_sam{0}, g_oh_body{0}, g_oh_other{0};
std::atomic<unsigned> g_eh_logged{0};
std::atomic<std::uintptr_t> g_body_ent{0};  // the partner body (the tick)
double g_enemy_ahead = 0.0;                 // shots 10 (hidden runs): the enemy replays start this far along the shot
// shots_test (game thread sets, the filter reads)
std::atomic<bool> g_test_armed{false}, g_test_nofilter{false};
std::atomic<int> g_test_ref_match{0}, g_test_dummy_match{0}, g_test_how{-1}, g_test_slot{-1};
std::atomic<int> g_test_dropped{0};
double g_test_t0 = 0;
bool g_test_wait = false;

std::uint8_t OurPartsFwd() noexcept;  // the beacon's parts (below)
// counters (game thread)
unsigned g_sent = 0, g_sent_pulls = 0, g_sent_frames = 0, g_cap_dropped = 0, g_no_peer = 0;
unsigned g_replayed = 0, g_rep_muzzle = 0, g_rep_mag = 0, g_rep_late = 0, g_rep_now = 0, g_rep_no_enqueue = 0, g_rep_faults = 0;
unsigned g_ref_weapon = 0, g_ref_mag = 0, g_ref_attack = 0, g_ref_origin = 0, g_ref_target = 0, g_rx_bad = 0, g_rx_dup = 0, g_rx = 0, g_hold_full = 0;
unsigned g_ref_filter = 0, g_ref_category = 0, g_ref_speed = 0, g_ref_set_full = 0, g_ref_dummy = 0;
unsigned g_weapon_hist_id[8] = {}, g_weapon_hist_n[8] = {}, g_mag_hist_id[8] = {}, g_mag_hist_n[8] = {};
unsigned g_logged_sent = 0, g_logged_rep = 0, g_logged_drop = 0, g_logged_hd = 0;
double g_next_log_ms = 0;
bool g_reg_logged = false;

// the shooter side
std::uint32_t g_next_shot_id = 0, g_seq = 0;
shotw::PullGroup g_pull;
shotw::RateCap g_cap_rate;
shotw::Shot g_last_shot;  // the newest captured shot (shots_test's ids)
bool g_have_last = false;
shotw::Shot g_last_rx;    // the partner's newest received shot (the aimed test's ids)
bool g_have_rx = false;
std::uint8_t g_cur_weapon = 0, g_cur_mag = 0;

// the beacon
std::uint8_t g_peer_parts = 0;
bool g_have_peer = false;
double g_peer_ms = -1e18, g_beacon_next = 0, g_beacon_fast_until = 0;
std::uint32_t g_gen = 0xFFFFFFFFu;
std::uint8_t g_last_logged_parts = 0xFF, g_last_logged_peer = 0xFF;

// the receiver side
shotw::Dedupe g_dedupe;
struct Held {
    shotw::Shot s;
    double due = 0;      // the sender's unwrapped ms (the pose play time's clock)
    double rx = 0;       // our clock
    bool echo = false;
    bool test = false;
};
constexpr unsigned kHoldMax = 64;
Held g_hold[kHoldMax];
unsigned g_nhold = 0;
Held g_ehold[kHoldMax];  // the enemy shots (the guest)
unsigned g_nehold = 0;
bool g_echo = false;
double g_echo_off = 2.5;
// the partner body's place (the tick; shots 3 stands our Sam beside it)
bool g_have_body_xf = false;
WorldTransform g_body_xf{};
// the partner body's chest (spine joint 4 of the 77-joint layout; the root + 1.2 m when unreadable): shots 4's steered aim point
double g_body_chest[3] = {};
// shots 4 (hidden runs): the game camera turned toward the partner body (a scenario pulses the right stick until "aimed").
// shots 4 <deg> 1: steered (the round 15 prove, run 20261123-221121: the pulsed yaw left the camera's pitch where the teleport
// put it, and a 120 ms pulse still turned the camera on after "aimed": the guest's 7 shots went past the host's left foot into
// the ground). The mod holds LT (the aim stance) and drives the virtual right stick itself every tick until the camera's ray
// (its position and 3D forward) points at the body's chest in yaw AND pitch within deg, the stick released and the aim still
// within deg 250 ms later. Each axis adapts (run 20261124-081004: in the aim stance the camera stalled at 11 deg yaw / 6 deg
// pitch with the stick at 13000-15000 / 9000): an axis driven for 400 ms that gained less than 0.5 deg gets 4000 more stick, an
// overshoot (the error's sign turned) halves that again, and an error that grew by 3 deg on the same side turns its sign.
struct AimAxis {
    int sign = 1;          // rx > 0 turns the camera right (yaw error up), ry > 0 looks up: flipped if the error grows
    double boost = 0;      // extra stick over the 9000 floor
    double win_e = 1e9;    // the window's signed error at its start
    bool drove = false;
    std::int16_t Stick(double err_deg, double tol) const noexcept {
        const double a = std::fabs(err_deg);
        if (a <= tol * 0.5) return 0;
        const double lo = 9000.0 + boost;
        double m = a * 1200.0;
        m = m < lo ? lo : m;
        m = m > 30000.0 ? 30000.0 : m;
        return static_cast<std::int16_t>(err_deg < 0 ? -m : m);
    }
    // the window's end: returns 1 when the sign was flipped, 2 when the boost grew, 3 when it was halved, else 0
    int Window(double e) noexcept {
        int r = 0;
        if (drove && win_e < 1e8) {
            const bool same = (e < 0) == (win_e < 0);
            if (same && std::fabs(e) > std::fabs(win_e) + 3) sign = -sign, r = 1;
            else if (same && std::fabs(win_e) - std::fabs(e) < 0.5) boost = boost + 4000 > 21000 ? 21000 : boost + 4000, r = 2;
            else if (!same && boost > 0) boost *= 0.5, r = 3;
        }
        win_e = e;
        drove = false;
        return r;
    }
};
struct Aim {
    bool on = false;
    double tol = 12;
    unsigned n = 0;
    double next_log = 0;
    bool steer = false;
    double start = 0, settle_until = 0;
    double win_t = 0;
    AimAxis yaw, pitch;
    bool fire = false;      // shots 4 <deg> 2: once aimed, RT held for 2 s while the aim keeps following its point
    double fire_until = 0;
} g_aim;
double g_aim_off_yaw = 0, g_aim_off_pitch = 0;
double g_aim_override[3] = {}, g_aim_override_until = 0;  // SetAimTarget: the steered aim's point instead of the body's chest  // shots 9: the steered aim's offset (the body off the screen's centre)
std::uintptr_t g_aim_ent = 0;  // SetAimEntity: the override follows this entity's place (+ g_aim_ent_up) every tick
double g_aim_ent_up = 1.2;
void AimPad(std::int16_t rx, std::int16_t ry, bool fire = false) noexcept {
    overlay::input::PadState p;
    p.connected = true;
    p.lt = 255;  // the aim stance: the camera that aims the shot
    p.rt = fire ? 255 : 0;
    p.rx = rx, p.ry = ry;
    overlay::input::SetVirtualPad(p);
}
void AimSteerTick(game::Entity* local) {
    double cp[3] = {}, cf[3] = {};
    if (!nametags::CameraView(local, cp, cf)) return;
    const double d[3] = {g_body_chest[0] - cp[0], g_body_chest[1] - cp[1], g_body_chest[2] - cp[2]};
    const double h = std::sqrt(d[0] * d[0] + d[1] * d[1]);
    double ey = (std::atan2(d[1], d[0]) - std::atan2(cf[1], cf[0])) * 57.29577951308232;
    ey += g_aim_off_yaw;  // shots 9: the camera held that far left of the body
    while (ey > 180) ey -= 360;
    while (ey < -180) ey += 360;
    const double ep = (std::atan2(d[2], h) - std::asin(cf[2] < -1 ? -1 : cf[2] > 1 ? 1 : cf[2])) * 57.29577951308232 - g_aim_off_pitch;
    const bool in = std::fabs(ey) <= g_aim.tol && std::fabs(ep) <= g_aim.tol;
    if (g_aim.fire_until > 0) {
        // firing (shots 4 <deg> 2): the stick keeps the aim on its point (a mirror that walks) while RT is held
        if (g_now >= g_aim.fire_until) {
            g_aim.on = false, g_aim.fire_until = 0;
            AimPad(0, 0);  // RT let go, LT still held
            Log("[shots] camera aim: fired while following (yaw %.1f, pitch %.1f deg off at the end)", ey, ep);
            return;
        }
        AimPad(static_cast<std::int16_t>(-g_aim.yaw.sign * g_aim.yaw.Stick(ey, g_aim.tol)), static_cast<std::int16_t>(g_aim.pitch.sign * g_aim.pitch.Stick(ep, g_aim.tol)), true);
        return;
    }
    if (in) {
        if (g_aim.settle_until == 0) {
            g_aim.settle_until = g_now + 250;
            AimPad(0, 0);
            return;
        }
        if (g_now < g_aim.settle_until) return;
        if (g_aim.fire) {
            g_aim.fire_until = g_now + 2000;
            AimPad(0, 0, true);
        } else {
            g_aim.on = false;
            AimPad(0, 0);  // LT stays held: the scenario's next pad step fires from this aim
        }
        Log("[shots] camera aimed #%u at the partner body (steered: yaw %.1f, pitch %.1f deg off, %.1f m; stick signs %d %d, boosts %.0f %.0f)%s", ++g_aim.n,
            ey, ep, std::sqrt(h * h + d[2] * d[2]), g_aim.yaw.sign, g_aim.pitch.sign, g_aim.yaw.boost, g_aim.pitch.boost,
            g_aim.fire ? "; firing 2 s while it follows" : "");
        return;
    }
    g_aim.settle_until = 0;
    if (g_now - g_aim.start > 30000) {
        g_aim.on = false;
        overlay::input::SetVirtualPad(overlay::input::PadState{true});
        Log("[shots] camera aim: gave up after 30 s (yaw %.1f, pitch %.1f deg off)", ey, ep);
        return;
    }
    if (g_now - g_aim.win_t >= 400) {
        g_aim.win_t = g_now;
        static const char* const kWhat[4] = {"", "stick sign flipped", "more stick", "less stick (overshoot)"};
        const int ry_ = g_aim.yaw.Window(ey), rp_ = g_aim.pitch.Window(ep);
        if (ry_ == 1 || rp_ == 1 || ((ry_ | rp_) != 0 && g_now >= g_aim.next_log))
            Log("[shots] camera aim: yaw %s, pitch %s (boosts %.0f %.0f)", ry_ ? kWhat[ry_] : "as is", rp_ ? kWhat[rp_] : "as is", g_aim.yaw.boost,
                g_aim.pitch.boost);
    }
    const std::int16_t rx = static_cast<std::int16_t>(-g_aim.yaw.sign * g_aim.yaw.Stick(ey, g_aim.tol));
    const std::int16_t ry = static_cast<std::int16_t>(g_aim.pitch.sign * g_aim.pitch.Stick(ep, g_aim.tol));
    g_aim.yaw.drove = g_aim.yaw.drove || rx != 0;
    g_aim.pitch.drove = g_aim.pitch.drove || ry != 0;
    AimPad(rx, ry);
    if (g_now >= g_aim.next_log) g_aim.next_log = g_now + 1000, Log("[shots] camera aim: yaw %.1f, pitch %.1f deg off (steering, stick %d %d)", ey, ep, rx, ry);
}
void AimTick(game::Entity* local) {
    if (!g_aim.on || !local) return;
    if (g_aim.steer && (g_have_body_xf || g_aim_override_until > g_now)) return AimSteerTick(local);
    if (!g_have_body_xf || g_aim.steer) return;
    WorldTransform s{};
    double cf[2] = {};
    if (!game::TryGetTransform(local, &s) || !nametags::CameraForward(local, cf)) return;
    double diff = (std::atan2(g_body_xf.pos.y - s.pos.y, g_body_xf.pos.x - s.pos.x) - std::atan2(cf[1], cf[0])) * 57.29577951308232;
    while (diff > 180) diff -= 360;
    while (diff < -180) diff += 360;
    if (std::fabs(diff) > g_aim.tol) {
        if (g_now >= g_aim.next_log) g_aim.next_log = g_now + 1000, Log("[shots] camera aim: %.1f deg off", diff);
        return;
    }
    g_aim.on = false;
    Log("[shots] camera aimed #%u at the partner body (%.1f deg off)", ++g_aim.n, diff);
}
// the leak test
int g_leak_left = 0, g_leak_done = 0;
SIZE_T g_leak_ws0 = 0;

// ---------------------------------------------------------------- reads
std::uintptr_t Reg() noexcept { return mem::Q(A(kRegGlobal)); }
bool ReadXf(std::uintptr_t e, WorldTransform& out) noexcept {
    std::uint8_t b[0x3C];
    if (!e || !mem::Read(e + 0xC8, b, sizeof(b))) return false;
    std::memcpy(&out.pos, b, 0x18);
    std::memcpy(&out.rot, b + 0x18, 0x24);
    return std::isfinite(out.pos.x) && std::isfinite(out.pos.y) && std::isfinite(out.pos.z) && std::fabs(out.pos.x) < 1e7 && std::fabs(out.pos.y) < 1e7;
}
double Dist3(const double (&a)[3], const double (&b)[3]) noexcept {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// A registry list (count at +cnt, list at +lst) holds an entry keyed `id` (u8 at entry + 0x20): the entry, else 0.
std::uintptr_t RegFind(std::uintptr_t cnt, std::uintptr_t lst, std::uint8_t id) noexcept {
    const std::uintptr_t reg = Reg();
    std::int32_t n = 0;
    if (!id || !reg || !mem::Rd(reg + cnt, n) || n <= 0 || n > 4096) return 0;
    const std::uintptr_t list = mem::Q(reg + lst);
    if (!list) return 0;
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uintptr_t e = mem::Q(list + 8ull * static_cast<unsigned>(i));
        std::uint8_t k = 0;
        if (e && mem::Rd(e + 0x20, k) && k == id) return e;
    }
    return 0;
}
shotw::IdCheck WeaponCheck(std::uint8_t id) noexcept {
    shotw::IdCheck c;
    const std::uintptr_t w = RegFind(0x10, 0x18, id);
    c.listed = w != 0;
    if (w) {
        const std::uintptr_t ch = mem::Q(w + 0x80);
        std::int32_t n = -1;
        c.child_ok = ch && mem::Rd(ch + 0x90, n) && n >= 0 && n <= 64 && (n == 0 || mem::Q(ch + 0x98) != 0);
    }
    return c;
}
shotw::IdCheck MagCheck(std::uint8_t id) noexcept {
    shotw::IdCheck c;
    c.listed = RegFind(0x20, 0x28, id) != 0;
    return c;
}

// Our Sam's current weapon (the inventory's, as the gear watch reads it): the registry keys CreateBullet takes.
void ReadCurrentWeapon(std::uintptr_t p, std::uint8_t* weapon, std::uint8_t* mag) noexcept {
    *weapon = 0, *mag = 0;
    if (!p || mem::Q(p) != mem::Base() + kPlayerVtable) return;
    const std::uintptr_t inv = mem::Q(p + kPInv);
    if (!inv || mem::Q(inv + kCompOwner) != p) return;
    std::uint8_t slot = 0xFF;
    if (!mem::Rd(inv + 0x1C78, slot) || slot >= 64) return;
    const std::uintptr_t inst = mem::Q(inv + 0xD0 + 0x70ull * slot);
    std::uint8_t id = 0x5D;
    if (!inst || !mem::Rd(inst + 0x12, id) || id >= 0x5D) return;
    // the registry key: the weapon parameter's Id (+0x20), the same as the instance's +0x12
    const std::uintptr_t prm = mem::Q(inst + 0x18);
    std::uint8_t key = 0;
    *weapon = prm && mem::Rd(prm + 0x20, key) && key ? key : id;
    std::uint8_t mi = 0xFF;
    if (mem::Rd(inst + 0x2F8, mi) && mi < 8) {
        const std::uintptr_t mp = mem::Q(inst + 0x40ull * mi + 0x38);
        std::uint8_t mk = 0;
        if (mp && mem::Rd(mp + 0x20, mk)) *mag = mk;
    }
}

// ---------------------------------------------------------------- the Enqueue detour (any thread)
void CountOwner(std::uintptr_t owner, std::uintptr_t rec) noexcept {
    OwnRound r{};
    std::uint8_t raw[0x20] = {};
    if (mem::Read(rec + 0x60, raw, sizeof(raw))) std::memcpy(r.o, raw, 12), std::memcpy(r.v, raw + 0x10, 12);
    AcquireSRWLockExclusive(&g_cap_lock);
    int at = -1, free_i = -1;
    for (int i = 0; i < 16; ++i) {
        if (g_own[i].e == owner) {
            at = i;
            break;
        }
        if (!g_own[i].e && free_i < 0) free_i = i;
    }
    if (at < 0 && free_i >= 0) at = free_i, g_own[at].e = owner, g_own[at].n = 0;
    if (at >= 0) g_own[at].ring[g_own[at].n++ % kOwnRing] = r;
    ReleaseSRWLockExclusive(&g_cap_lock);
}

int H_Enqueue(std::uintptr_t sys, std::uintptr_t rec) {
    if (t_replaying) {
        // our replay (CreateBullet built the record on its stack): make it the original field for field but its owner, then
        // keep the attack references the bullet carries (the echo guard: never captured)
        g_echo_skips.fetch_add(1, std::memory_order_relaxed);
        t_nrefs = 0;
        if (t_patch.on) {
            auto* r = reinterpret_cast<volatile std::uint8_t*>(rec);
            r[0x05] = t_patch.r05, r[0x58] = t_patch.r58, r[0x5A] = t_patch.r5A, r[0xD0] = t_patch.rD0, r[0xD1] = t_patch.rD1;
            *reinterpret_cast<volatile float*>(rec + 0x94) = t_patch.r94;
        }
        const std::uint64_t pre = mem::Q(rec + 0x38), pre18 = mem::Q(rec + 0x38 + 0x18);
        const int serial = o_enqueue(sys, rec);
        t_enqueued = serial != 0xFFFF;
        t_have_slot = false;
        if (t_enqueued) {
            for (unsigned i = 0; i < 16; ++i) {
                const std::uintptr_t slot = sys + 0xF0ull * i;
                std::int32_t sn = -1;
                if (mem::Rd(slot + 0xEC, sn) && sn == serial) {
                    const std::uint64_t h = mem::Q(slot + 0x48), h18 = mem::Q(slot + 0x48 + 0x18);
                    if (h) t_refs[t_nrefs++] = h;
                    if (h18 && t_nrefs < shotw::kReplayRefs) t_refs[t_nrefs++] = h18;
                    t_have_slot = mem::Read(slot + 0x10, t_slot_rec, kRecSize);
                    break;
                }
            }
        }
        if (pre && t_nrefs < shotw::kReplayRefs) t_refs[t_nrefs++] = pre;
        if (pre18 && t_nrefs < shotw::kReplayRefs) t_refs[t_nrefs++] = pre18;
        return serial;
    }
    const std::uintptr_t oref = mem::Q(rec + 0x20);
    const std::uintptr_t owner = oref ? oref - 0x20 : 0;
    // a frozen mirror's own fire (the guest, enemy sync P1a): never enqueued. Returning 0xFFFF without the original is what a full
    // queue does (the fire path ignores the result and frees its own stack record); the game cancels a frozen MULE's trigger
    // requests anyway (+0x268), so this is expected to stay 0.
    if (owner && esync::IsFrozenMirror(owner)) {
        g_mirror_seen.fetch_add(1, std::memory_order_relaxed);
        if (esync::GuardOn()) {
            g_mirror_dropped.fetch_add(1, std::memory_order_relaxed);
            return 0xFFFF;
        }
    }
    const std::uintptr_t sam = g_sam.load(std::memory_order_relaxed);
    if (g_own_armed.load(std::memory_order_relaxed) && owner) CountOwner(owner, rec);
    // ours: the record's owner is our Sam (the fire path writes [weapon instance + 0x300] + 0x20), or an object whose +0x300 is
    // our Sam (a weapon instance), or a weapon entity whose instance's (+0x338) +0x300 is
    int how = 0;
    if (owner && sam) {
        if (owner == sam) how = 1;
        else if (mem::Q(owner + 0x300) == sam) how = 2;
        else if (const std::uintptr_t inst = mem::Q(owner + 0x338); inst && mem::Q(inst + 0x300) == sam) how = 3;
    }
    if (g_diag_enq.fetch_add(1, std::memory_order_relaxed) < 3 || (how && g_diag_ours.fetch_add(1, std::memory_order_relaxed) < 2)) {
        const char* rn = owner ? mem::RttiName(mem::RttiOf(owner)) : nullptr;
        std::uint16_t atk = 0;
        mem::Rd(rec + 0x5C, atk);
        LogLater("[shots] enqueue: owner %p (%s) our Sam %p => %s (%d); attack 0x%X; from ds.exe+0x%llX", reinterpret_cast<void*>(owner), rn ? rn : "?",
            reinterpret_cast<void*>(sam), how ? "ours" : "not ours", how, atk,
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - mem::Base()));
    }
    // the host's enemy shots: a record owned by one of the shooters enemy_sync published (its 10 Hz class)
    std::uint32_t shooter = shotw::kShooterSam;
    bool take = how && g_on.load(std::memory_order_relaxed);
    if (!how && owner && shotw::EnemyCaptureOn(g_enemy_role.load(std::memory_order_relaxed)) && g_shooters.Find(owner, &shooter, false)) take = true;
    if (take) {
        Cap c;
        if (mem::Read(rec, c.rec, kRecSize)) {
            c.ms = link::NowMsPrecise();
            c.shooter = shooter;
            c.owner = owner;
            AcquireSRWLockExclusive(&g_cap_lock);
            if (g_cap_count < kCapRing) {
                g_cap[(g_cap_head + g_cap_count) % kCapRing] = c;
                ++g_cap_count;
                g_captured.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_cap_full.fetch_add(1, std::memory_order_relaxed);
            }
            ReleaseSRWLockExclusive(&g_cap_lock);
        } else {
            g_det_faults.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_others.fetch_add(1, std::memory_order_relaxed);
    }
    return o_enqueue(sys, rec);
}

// ---------------------------------------------------------------- the damage filters (worker threads)
// A hit of one of our replays: its attack reference (or context) is in the replay set (exact, per bullet). The bullet system's
// dummy owner (every CreateBullet bullet's, the game's own scripted ones too) is never a reason to drop: while shots_test waits,
// a hit by it is only noted (DummyNote) for the test line.
bool IsReplayHit(const samdmg::View& v, std::uint8_t* kind, int* how, int* slot) noexcept {
    const double now = link::NowMsPrecise();
    AcquireSRWLockShared(&g_rs_lock);
    const bool found = g_rs.Find(v.ref, now, kind, how, slot) || g_rs.Find(v.ctx, now, kind, how, slot);
    ReleaseSRWLockShared(&g_rs_lock);
    return found;
}
void DummyNote(const samdmg::View& v) noexcept {
    const std::uintptr_t dummy = g_dummy.load(std::memory_order_relaxed);
    if (!dummy || v.attacker != dummy || !g_test_armed.load(std::memory_order_relaxed)) return;
    g_test_dummy_match.store(1);
    g_seen_sam_dummy.fetch_add(1, std::memory_order_relaxed);
}

void LogDamage(const char* who, const samdmg::View& v, int how, int slot, const char* verdict) {
    if (g_dmg_logged.fetch_add(1, std::memory_order_relaxed) >= 12) return;
    std::uintptr_t w[16] = {};
    mem::Read(v.di, w, sizeof(w));
    LogLater("[shots] replay hit on %s: %s (ref %p ctx %p attacker %p dummy %p; %s%s how %d slot %d) amount %.1f; DamageInfo +0..+0x78: %llX %llX %llX %llX %llX %llX "
        "%llX %llX %llX %llX %llX %llX %llX %llX %llX %llX",
        who, verdict, reinterpret_cast<void*>(v.ref), reinterpret_cast<void*>(v.ctx), reinterpret_cast<void*>(v.attacker),
        reinterpret_cast<void*>(g_dummy.load()), "replay-set match", "", how, slot,
        static_cast<double>(v.amount), static_cast<unsigned long long>(w[0]), static_cast<unsigned long long>(w[1]), static_cast<unsigned long long>(w[2]),
        static_cast<unsigned long long>(w[3]), static_cast<unsigned long long>(w[4]), static_cast<unsigned long long>(w[5]), static_cast<unsigned long long>(w[6]),
        static_cast<unsigned long long>(w[7]), static_cast<unsigned long long>(w[8]), static_cast<unsigned long long>(w[9]), static_cast<unsigned long long>(w[10]),
        static_cast<unsigned long long>(w[11]), static_cast<unsigned long long>(w[12]), static_cast<unsigned long long>(w[13]), static_cast<unsigned long long>(w[14]),
        static_cast<unsigned long long>(w[15]));
}

// The damage policy's input for a replay of `kind` (erules::ReplayPolicy decides: the Sam filter, and a part of our Sam at the
// bullet).
erules::DamageIn PolicyIn(std::uint8_t kind) noexcept {
    erules::DamageIn d;
    d.replay = true, d.kind = kind;
    d.friendly_fire = g_cfg.shots_friendly_fire;
    d.enemy_hurt = g_cfg.enemy_shots_hurt;
    d.test_nofilter = g_test_nofilter.load(std::memory_order_relaxed);
    d.nohurt_once = g_et_armed.load(std::memory_order_relaxed) && g_et_nohurt.load(std::memory_order_relaxed);
    return d;
}

// Still matched after a fault-stop (g_stopped): replays already in flight keep being dropped until their entries expire. A hit
// is one of our replays only when its reference is in the replay set AND its attacker is the bullet system's dummy
// (shotw::ReplayHit; every replay's is, runs 20261124-093441 / -104304): a game round whose reference reuses a replay's is kept.
samdmg::Verdict SamFilter(const samdmg::View& v) {
    std::uint8_t kind = 0;
    int how = -1, slot = -1;
    if (!IsReplayHit(v, &kind, &how, &slot)) {
        DummyNote(v);
        return samdmg::Verdict::keep;
    }
    const std::uintptr_t dummy = g_dummy.load(std::memory_order_relaxed);
    if (!shotw::ReplayHit(true, v.attacker, dummy)) {
        if (g_kept_reuse.fetch_add(1, std::memory_order_relaxed) < 4)
            LogLater("[shots] a hit on our Sam kept: its reference matches a replay but its attacker %p is not the dummy %p (a reused reference), amount %.1f",
                     reinterpret_cast<void*>(v.attacker), reinterpret_cast<void*>(dummy), static_cast<double>(v.amount));
        return samdmg::Verdict::keep;
    }
    if (g_test_armed.load(std::memory_order_relaxed)) g_test_ref_match.store(1), g_test_how.store(how), g_test_slot.store(slot);
    const bool keep = erules::ReplayPolicy(PolicyIn(kind)) == erules::Dmg::keep;
    if (kind == shotw::kReplayKindEnemy) {
        // an enemy round replayed here (enemy sync P1a): the victim decides (enemy_shots_hurt), a nohurt test drops it once armed
        if (g_et_armed.load(std::memory_order_relaxed)) g_et_match.store(1), g_et_kept.store(keep ? 1 : 0), g_et_amount.store(v.amount);
        if (keep) {
            g_kept_enemy.fetch_add(1, std::memory_order_relaxed);
            if (g_kept_enemy_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                std::uint16_t atk = 0;
                mem::Rd(v.di + 0x30, atk);
                LogLater("[shots] an enemy round on our Sam: kept (enemy_shots_hurt=1), amount %.1f (DamageInfo +0x30 0x%X)", static_cast<double>(v.amount), atk);
            }
            return samdmg::Verdict::keep;
        }
        g_drop_enemy.fetch_add(1, std::memory_order_relaxed);
        LogDamage("our Sam", v, how, slot, "dropped (an enemy round: enemy_shots_hurt=0 or a nohurt test)");
        return samdmg::Verdict::drop;
    }
    if (keep) {
        if (kind == shotw::kReplayKindTest) g_kept_test.fetch_add(1, std::memory_order_relaxed), LogDamage("our Sam", v, how, slot, "kept (test: no filter)");
        else g_kept_ff.fetch_add(1, std::memory_order_relaxed), LogDamage("our Sam", v, how, slot, "kept (shots_friendly_fire=1)");
        return samdmg::Verdict::keep;
    }
    g_drop_sam_ref.fetch_add(1, std::memory_order_relaxed);
    if (kind == shotw::kReplayKindTest) g_test_dropped.store(1);
    LogDamage("our Sam", v, how, slot, "dropped");
    return samdmg::Verdict::drop;
}

// The Humanoid backstop (npc_damage's shared detour, filter "shots"): a replay's hit on an NPC of this world dropped (also after a
// fault-stop: replays in flight stay dropped). The attacker first (the cheap exit: not the dummy, not a replay), then the set.
npcdmg::Verdict NpcFilter(std::uintptr_t /*e*/, std::uintptr_t /*msg*/, const samdmg::View& v) {
    if (!shotw::ReplayHit(true, v.attacker, g_dummy.load(std::memory_order_relaxed))) return npcdmg::Verdict::keep;
    std::uint8_t kind = 0;
    int how = -1, slot = -1;
    if (!IsReplayHit(v, &kind, &how, &slot)) return npcdmg::Verdict::keep;
    if (kind == shotw::kReplayKindTest && g_test_nofilter.load(std::memory_order_relaxed)) return npcdmg::Verdict::keep;
    LogDamage("an NPC", v, how, slot, "dropped");
    g_drop_npc.fetch_add(1, std::memory_order_relaxed);
    return npcdmg::Verdict::drop;
}

// ---------------------------------------------------------------- the drop at the bullet (worker threads)
bool ReplaysLive(double now) noexcept { return now - g_last_replay_ms.load(std::memory_order_relaxed) < shotw::kReplayTtlMaxMs + 1000.0; }
bool ReplayRef(std::uint64_t ref, double now, std::uint8_t* kind) noexcept {
    AcquireSRWLockShared(&g_rs_lock);
    const bool f = g_rs.Find(ref, now, kind);
    ReleaseSRWLockShared(&g_rs_lock);
    return f;
}
// CreateBullet's bullets are owned by the bullet system's dummy entity: a bullet of the game's own whose (freed and reused)
// reference is still in the replay set is never dropped. While the dummy is unknown (0: the first seconds after a load) no bullet
// is ours: nothing is replayed then either (ReplayShot).
bool OwnerDummy(std::uintptr_t owner) noexcept {
    const std::uintptr_t d = g_dummy.load(std::memory_order_relaxed);
    return d && owner == d;
}
// The entity a receiver belongs to (entity + 0x38: a non-zero byte at receiver + 8), else 0.
std::uintptr_t ReceiverEntity(std::uintptr_t receiver) noexcept {
    std::uint8_t ent = 0;
    return mem::Rd(receiver + 8, ent) && ent ? receiver - kReceiverOfEntity : 0;
}
// A part of our Sam (an entity whose parent chain, +0x70, reaches him within 4 levels: his armor plates, run 20261124-093441).
bool PartOfOurSam(std::uintptr_t e, std::uintptr_t sam) noexcept {
    for (int d = 0; e && sam && d < 4; ++d) {
        e = mem::Q(e + kEntityParent);
        if (e == sam) return true;
    }
    return false;
}
// The class of what a receiver belongs to: its entity's; for a part with no class of its own ("Entity": a MULE's gear, Sam's
// armor plate) the first parent (+0x70, up to 4 levels) that says more; for a plain entity with no such parent (a postbox,
// runs 20261124-093019 / -093441) its components' (entity +0x90 count, +0x98 list; a component never makes it "player":
// a postbox carries DSJointAimToPlayerComponent). `name` the entity's own class name, `pname` the deciding one.
shotw::HitClass ReceiverClass(std::uintptr_t receiver, const char** name, const char** pname) noexcept {
    *pname = "";
    std::uintptr_t e = ReceiverEntity(receiver);
    const char* n = e ? mem::RttiName(mem::RttiOf(e)) : nullptr;
    if (!n) e = 0, n = mem::RttiName(mem::RttiOf(receiver));
    *name = n ? n : "?";
    shotw::HitClass c = shotw::ClassifyHit(n);
    for (std::uintptr_t p = e; p && c == shotw::HitClass::other;) {
        int d = 0;
        for (p = mem::Q(p + kEntityParent); p && d < 4; p = mem::Q(p + kEntityParent), ++d) {
            const char* pn = mem::RttiName(mem::RttiOf(p));
            if (!pn) break;
            const shotw::HitClass pc = shotw::ClassifyHit(pn);
            if (pc != shotw::HitClass::other) {
                c = pc, *pname = pn;
                break;
            }
        }
        break;
    }
    std::int32_t nc = 0;
    if (c == shotw::HitClass::other && e && mem::Rd(e + kEntityComponents, nc) && nc > 0 && nc <= 64) {
        const std::uintptr_t list = mem::Q(e + kEntityComponents + 8);
        for (std::int32_t i = 0; list && i < nc && c == shotw::HitClass::other; ++i) {
            const char* cn = mem::RttiName(mem::RttiOf(mem::Q(list + 8ull * static_cast<unsigned>(i))));
            const shotw::HitClass cc = shotw::ClassifyHit(cn);
            if (cc != shotw::HitClass::other && cc != shotw::HitClass::player) c = cc, *pname = cn;
        }
    }
    return c;
}
// A frozen mirror of enemy sync (the guest, P1a), or a part of one (its gear: the +0x70 parent chain, up to 4 levels).
bool MirrorReceiver(std::uintptr_t receiver) noexcept {
    if (!esync::AnyFrozen()) return false;  // the cheap exit: no mirror frozen (the host, or no enemy sync)
    std::uintptr_t e = ReceiverEntity(receiver);
    for (int d = 0; e && d < 5; ++d, e = mem::Q(e + kEntityParent))
        if (esync::IsFrozenMirror(e)) return true;
    return false;
}
// True: drop this hit. (1) enemy sync's guard: any bullet's hit on a frozen mirror while the guard is on (hers, this world's, a
// replay's: the host decides the mirror's fate until P2; run 20261124-093844 showed bullet hits on a MULE never reach the
// Humanoid handler, so the guard lives here). (2) one of our replays' bullets (its owner the dummy, its reference in the set),
// its receiver not our Sam, no hidden-run control.
// A bullet of our Sam's own (the Enqueue's test: the record's owner is our Sam, run 20261124-114315's "ours (1)", or a weapon
// instance / entity of his).
bool OwnedByOurSam(std::uintptr_t owner, std::uintptr_t sam) noexcept {
    if (!owner || !sam) return false;
    if (owner == sam || mem::Q(owner + 0x300) == sam) return true;
    const std::uintptr_t inst = mem::Q(owner + 0x338);
    return inst && mem::Q(inst + 0x300) == sam;
}
// The game thread: one enemy replay enqueued (its references are this thread's t_refs).
void NoteEnemyReplay(std::uintptr_t shooter, const shotw::Shot& s, const double (&origin)[3], unsigned num) noexcept {
    AcquireSRWLockExclusive(&g_er_lock);
    EnemyRep& r = g_er[g_er_head];
    g_er_head = (g_er_head + 1) % kEnemyReps;
    r = EnemyRep{};
    for (unsigned i = 0; i < t_nrefs && i < shotw::kReplayRefs; ++i) r.ref[i] = t_refs[i];
    r.shooter = shooter, r.id = s.shooter, r.num = num, r.speed = s.speed, r.t = link::NowMsPrecise();
    for (int k = 0; k < 3; ++k) r.origin[k] = origin[k], r.dir[k] = s.dir[k];
    ReleaseSRWLockExclusive(&g_er_lock);
}
// The drop at the bullet: an enemy replay's bullet hit `receiver`; its first hit is named and counted (each replay once).
void EnemyFirstHit(std::uint64_t ref, std::uintptr_t receiver, double now) noexcept {
    EnemyRep r;
    bool found = false;
    AcquireSRWLockExclusive(&g_er_lock);
    for (unsigned k = 0; k < kEnemyReps && !found; ++k) {
        EnemyRep& x = g_er[(g_er_head + kEnemyReps - 1 - k) % kEnemyReps];  // the newest first
        if (x.t < 0 || now - x.t > shotw::kReplayTtlMaxMs) continue;
        for (unsigned i = 0; i < shotw::kReplayRefs; ++i) {
            const std::uint64_t v = x.ref[i];
            if (!v || !(ref == v || ref == v + 0x20 || v == ref + 0x20)) continue;
            if (!x.hit) x.hit = true, r = x, found = true;
            k = kEnemyReps;  // matched (a later hit of the same replay is not its first)
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_er_lock);
    if (!found) return;
    const std::uintptr_t ent = ReceiverEntity(receiver), raw = receiver - kReceiverOfEntity;
    std::uint8_t b8 = 0;
    mem::Rd(receiver + 8, b8);
    const std::uintptr_t body = g_body_ent.load(std::memory_order_relaxed), sam = g_sam.load(std::memory_order_relaxed);
    unsigned what = kEhOther;
    std::uintptr_t at = 0;
    // the receiver's entity and its parents; without the entity byte the object it sits in (receiver - 0x38) is tried too
    const std::uintptr_t starts[2] = {ent, ent ? 0 : raw};
    for (std::uintptr_t start : starts) {
        std::uintptr_t p = start;
        for (int d = 0; p && d < 5 && what == kEhOther; ++d, p = what == kEhOther ? mem::Q(p + kEntityParent) : p) {
            if (p == r.shooter) what = kEhShooter;
            else if (esync::IsFrozenMirror(p)) what = kEhMirror;
            else if (body && p == body) what = kEhBody;
            else if (sam && p == sam) what = kEhSam;
        }
        if (what != kEhOther) {
            at = p;
            break;
        }
    }
    g_eh[what].fetch_add(1, std::memory_order_relaxed);
    if (g_eh_logged.fetch_add(1, std::memory_order_relaxed) >= (automation::Enabled() ? 80u : 8u)) return;
    const std::uintptr_t pe = at ? at : ent;
    WorldTransform x{};
    double dist = -1, along = 0;
    if (pe && ReadXf(pe, x)) {
        const double p[3] = {x.pos.x, x.pos.y, x.pos.z + 1.0};  // about the chest
        dist = Dist3(p, r.origin);
        for (int k = 0; k < 3; ++k) along += (p[k] - r.origin[k]) * r.dir[k];
    }
    const char* n = mem::RttiName(mem::RttiOf(ent ? ent : receiver));
    const double ms = now - r.t;
    LogLater("[shots] enemy replay #%u's first hit (shooter %05X): %s (%s; receiver %p, entity byte %u), %.1f m from its start (%.1f m along the shot), %.0f ms "
             "after it (%.1f m of flight at %.0f m/s)",
             r.num, r.id, kEhName[what], n ? n : "?", reinterpret_cast<void*>(receiver), b8, dist, along, ms, ms * r.speed / 1000.0, r.speed);
}
bool JudgeBullet(std::uintptr_t bullet, std::uintptr_t receiver) {
    if (!receiver) return false;
    const double now = link::NowMsPrecise();
    const std::uint64_t oref = *reinterpret_cast<const volatile std::uint64_t*>(bullet + kBulletOwner);
    const std::uintptr_t owner = oref ? static_cast<std::uintptr_t>(oref - 0x20) : 0;
    // one of our replays: its owner the dummy first (the cheap exit: no bullet of the game's own pays the set's scan), then its
    // reference in the set
    std::uint64_t ref = 0;
    std::uint8_t kind = 0;
    bool replay = false;
    if (ReplaysLive(now) && OwnerDummy(owner)) {
        ref = *reinterpret_cast<const volatile std::uint64_t*>(bullet + kBulletRef);
        replay = ref && ReplayRef(ref, now, &kind);
    }
    if (replay && kind == shotw::kReplayKindEnemy) EnemyFirstHit(ref, receiver, now);
    if (!replay && owner && g_enemy_role.load(std::memory_order_relaxed) == 3 && g_shooters.Size() && g_shooters.Find(owner, nullptr, false)) {
        // an owned shooter's own round (P1b): what it hits
        const std::uintptr_t body = g_body_ent.load(std::memory_order_relaxed), sam = g_sam.load(std::memory_order_relaxed);
        std::uintptr_t p = ReceiverEntity(receiver);
        unsigned what = 2;
        for (int d = 0; p && d < 5 && what == 2; ++d, p = mem::Q(p + kEntityParent))
            what = sam && p == sam ? 0u : body && p == body ? 1u : 2u;
        (what == 0 ? g_oh_sam : what == 1 ? g_oh_body : g_oh_other).fetch_add(1, std::memory_order_relaxed);
    }
    if (esync::GuardOn() && MirrorReceiver(receiver)) {
        const bool ours = !replay && OwnedByOurSam(owner, g_sam.load(std::memory_order_relaxed));
        // P2: her own round goes on to Entity OnHit while the hits are captured (the OnHit detour drops it there and sends the HIT;
        // the game keeps her impacts and the bullet-hit message to her Sam); on a part with no entity receiver it is dropped here
        const shotw::MirrorBullet mb = shotw::MirrorBulletRule(replay, ours, ecombat::CaptureOurRounds(), ours && ecombat::IsOnHitReceiver(receiver));
        if (mb == shotw::MirrorBullet::pass_capture) {
            g_hd_mirror_pass.fetch_add(1, std::memory_order_relaxed);
            ecombat::CountBullet(true);
            if (g_hd_mirror_pass_logged.fetch_add(1, std::memory_order_relaxed) < 8)
                LogLater("[shots] our Sam's own round on a frozen mirror passed to Entity OnHit (enemy hits: the HIT goes to the host; bullet owner %p)",
                         reinterpret_cast<void*>(owner));
            return false;
        }
        if (mb == shotw::MirrorBullet::drop_no_entity) g_hd_mirror_noent.fetch_add(1, std::memory_order_relaxed), ecombat::CountBullet(false);
        g_hd_mirror.fetch_add(1, std::memory_order_relaxed);
        if (ours) g_hd_mirror_ours.fetch_add(1, std::memory_order_relaxed);
        else if (replay) g_hd_mirror_rep.fetch_add(1, std::memory_order_relaxed);
        if ((ours ? g_hd_mirror_logged_ours : g_hd_mirror_logged).fetch_add(1, std::memory_order_relaxed) < 8)
            LogLater("[shots] a bullet hit on a frozen mirror dropped (enemy sync: the host decides; %s, bullet owner %p)",
                     ours ? "our Sam's own round" : replay ? "a replay's round" : "another's round", reinterpret_cast<void*>(owner));
        return true;
    }
    if (!replay) return false;
    g_hd_seen.fetch_add(1, std::memory_order_relaxed);
    const std::uintptr_t sam = g_sam.load(std::memory_order_relaxed);
    // our Sam himself: the Sam filter decides; a part of him (an armor plate) takes its own damage, so it goes through only
    // where the Sam filter's policy would keep the hit (erules::ReplayPolicy: friendly fire, enemy_shots_hurt, a nohurt test)
    bool our_sam = sam && receiver == sam + kReceiverOfEntity;
    if (!our_sam && sam && PartOfOurSam(ReceiverEntity(receiver), sam)) {
        g_hd_part.fetch_add(1, std::memory_order_relaxed);
        our_sam = erules::ReplayPolicy(PolicyIn(kind)) == erules::Dmg::keep;
    }
    const bool keep = kind == shotw::kReplayKindAimed && g_aimed_keep.load(std::memory_order_relaxed);
    if (our_sam) g_hd_sam.fetch_add(1, std::memory_order_relaxed);
    const bool drop = shotw::BulletRule(true, true, our_sam, keep) == shotw::BulletVerdict::drop;
    if (!drop && !(keep && !our_sam)) return false;
    const char* name = "?";
    const char* pname = "";
    const shotw::HitClass c = ReceiverClass(receiver, &name, &pname);
    const unsigned ci = static_cast<unsigned>(c) < shotw::kHitClasses ? static_cast<unsigned>(c) : shotw::kHitClasses - 1;
    if (drop) g_hd_drop[ci].fetch_add(1, std::memory_order_relaxed);
    else g_hd_kept_test.fetch_add(1, std::memory_order_relaxed), g_hd_kept_cls[ci].fetch_add(1, std::memory_order_relaxed);
    if (g_hd_logged.fetch_add(1, std::memory_order_relaxed) < 64)
        LogLater("[shots] replay hit %s at the bullet: %s%s%s (%s; kind %u)", drop ? "dropped" : "let through (test control)", name, pname[0] ? " of " : "", pname,
            shotw::HitClassName(c), kind);
    return drop;
}
bool JudgeGuarded(std::uintptr_t bullet, std::uintptr_t receiver) noexcept {
    __try {
        return JudgeBullet(bullet, receiver);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_hd_faults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}
void H_BulletA(std::uintptr_t bullet, std::uintptr_t receiver, std::uintptr_t hit, float amount, std::uint64_t w) {
    if (JudgeGuarded(bullet, receiver)) return;  // nothing built yet: the DamageInfo and its context are made inside
    ecombat::MarkBullet(true);                   // P2: what the OnHit detour captures inside is a bullet's
    o_bullet_a(bullet, receiver, hit, amount, w);
    ecombat::MarkBullet(false);
}
void H_BulletB(std::uintptr_t bullet, std::uintptr_t receiver, std::uintptr_t hit, float amount, std::uint64_t w) {
    if (JudgeGuarded(bullet, receiver)) return;
    ecombat::MarkBullet(true);
    o_bullet_b(bullet, receiver, hit, amount, w);
    ecombat::MarkBullet(false);
}
// The hit processor works on a bullet with a pending hit (+0x16C set, +0x164 == 2); the BT reaction's detour drops its call
// while that bullet is one of our replays'.
bool HitProcReplay(std::uintptr_t bullet) noexcept {
    __try {
        if (!*reinterpret_cast<const volatile std::uint8_t*>(bullet + kBulletHitFlag) ||
            *reinterpret_cast<const volatile std::int32_t*>(bullet + kBulletState) != 2)
            return false;
        const double now = link::NowMsPrecise();
        if (!ReplaysLive(now)) return false;
        const std::uint64_t ref = *reinterpret_cast<const volatile std::uint64_t*>(bullet + kBulletRef);
        const std::uint64_t oref = *reinterpret_cast<const volatile std::uint64_t*>(bullet + kBulletOwner);
        std::uint8_t kind = 0;
        if (!ref || !OwnerDummy(oref ? static_cast<std::uintptr_t>(oref - 0x20) : 0) || !ReplayRef(ref, now, &kind)) return false;
        return !(kind == 4 && g_aimed_keep.load(std::memory_order_relaxed));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void H_HitProc(std::uintptr_t bullet, std::uintptr_t sys, float dt) {
    const bool prev = t_hit_replay;
    t_hit_replay = HitProcReplay(bullet);
    o_hitproc(bullet, sys, dt);
    t_hit_replay = prev;
}
// The skipped call returns 0: its only caller (0x14291D64E) jumps on to 0x14291D5CA and never reads rax before writing it
// (checked in ds.exe, round 16 review); BulletDamageA / B's three callers ignore rax too (void).
std::uint64_t H_BtReact(std::uintptr_t hit, std::uintptr_t owner, std::uint8_t b58, std::uint32_t e0, std::uint16_t attack) {
    if (t_hit_replay && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == A(kBtRet)) {
        if (g_hd_bt.fetch_add(1, std::memory_order_relaxed) < 4) LogLater("[shots] replay hit on a BT: its reaction (attack 0x%X) dropped", attack);
        return 0;
    }
    return o_bt(hit, owner, b58, e0, attack);
}

// ---------------------------------------------------------------- replay (game thread)
struct Replay {
    double origin[3] = {}, target[3] = {};
    std::uint32_t actor = 0xFFFFFFFFu;
    float speed = 0;
    std::uint32_t a5 = 0;
    std::uint16_t attack = 0;
    std::uint8_t a7 = 0, weapon = 0, mag = 0;
    shotw::D0Args d0;
    Patch patch;
    std::uint8_t kind = 1;
};

int CallCreate(const Replay& r) noexcept {
    t_replaying = true;
    t_patch = r.patch;
    t_enqueued = false;
    t_nrefs = 0;
    __try {
        reinterpret_cast<FnCreate>(A(kCreateBullet))(r.origin, r.target, r.actor, r.speed, r.a5, r.attack, r.a7, r.d0.p8, r.d0.p9, r.d0.p10, r.d0.p11, r.d0.p12,
                                                     r.weapon, r.mag);
        t_replaying = false;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_replaying = false;
        return -1;
    }
}

void Stop(const char* why) {
    g_stopped.store(true);
    g_on.store(false);
    g_enemy_role.store(0);
    std::snprintf(g_off, sizeof(g_off), "stopped (%s)", why);
    Log("[shots] FAULT in %s: shots stopped for this session", why);
}

bool CategoryFind(std::uint8_t cat, std::uint16_t* at38, std::uint16_t* at3A) noexcept;  // below (the shooter side)

// Whether this game may replay at all: the Sam damage filter registered, the NPC drop hooked unless shots_npc_drop=0, and the
// drop at the bullet hooked unless shots_hit_drop=0.
bool CanReplay() noexcept {
    return g_filter_registered && (g_npc_hooked || !g_cfg.shots_npc_drop) && (g_hit_hooked || !g_cfg.shots_hit_drop);
}
const char* CannotReplayWhy() noexcept {
    return !g_filter_registered                       ? "the Sam damage filter is off"
           : !(g_npc_hooked || !g_cfg.shots_npc_drop) ? "the NPC drop is not installed (shots_npc_drop=1)"
           : !CanReplay()                             ? "the drop at the bullet is not installed (shots_hit_drop=1)"
                                                      : "";
}

// P1c: this game's attack registry (the listed ids), or nullptr (the 0x18C cap): enemy_attack_registry=0, not read yet, implausible.
const std::uint8_t* AttackListed() noexcept { return g_attack_want && g_attack_ok ? g_attack_bits : nullptr; }
void AttackRegistryTick() noexcept {
    if (!g_attack_want || g_attack_ok || g_now < g_attack_next || g_attack_tries >= 60) return;
    g_attack_next = g_now + 5000, ++g_attack_tries;
    std::uint16_t max_id = 0;
    const int n = ecombat::AttackRegistry(g_attack_bits, &max_id);
    const bool has57 = (g_attack_bits[0x57 >> 3] >> (0x57 & 7)) & 1u, has191 = (g_attack_bits[0x191 >> 3] >> (0x191 & 7)) & 1u;
    if (n < 50 || !has57) {
        if (g_attack_tries == 1 || g_attack_tries == 60) Log("[shots] attack registry n=%d not plausible yet (0x57 %s): the 0x18C cap%s", n, has57 ? "listed" : "missing",
                                                              g_attack_tries == 60 ? " (given up)" : "");
        return;
    }
    g_attack_ok = true;
    Log("[shots] attack registry n=%d, max id=0x%X, 0x191 listed %s: ids above 0x18C replayed when listed", n, max_id, has191 ? "yes" : "no");
}
// The shots a partner without kPartAttackWide can decode (its decoder refuses a frame with an id above 0x18C): the others dropped.
unsigned NarrowFor(shotw::Shot* s, unsigned n) noexcept {
    if (g_peer_parts & shotw::kPartAttackWide) return n;
    unsigned k = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (s[i].attack_id > shotw::kMaxAttackId) {
            if (g_attack_narrowed++ < 8) Log("[shots] attack 0x%X not sent: the partner's build has the 0x18C cap", s[i].attack_id);
            continue;
        }
        s[k++] = s[i];
    }
    return k;
}

// One replay of `s` from `origin`. Returns true when the game enqueued it. Every value the game reads under the bullet system's
// lock is checked first: a fault there cannot be recovered (the SEH below is a last resort; the lock would stay held).
bool ReplayShot(const shotw::Shot& s, const double (&origin)[3], std::uint8_t kind, bool muzzle_wanted, bool enemy = false) {
    if (!CanReplay()) {
        ++g_ref_filter;
        return false;
    }
    if (!g_dummy.load(std::memory_order_relaxed)) {
        // the bullet system's dummy not known yet (the first seconds after a load): the drop at the bullet could not tell a
        // replay's bullet from the game's own
        ++g_ref_dummy;
        return false;
    }
    if (!shotw::AttackAllowed(s.attack_id, true, AttackListed())) {
        ++g_ref_attack;
        if (s.attack_id > shotw::kMaxAttackId && g_attack_refused_logged < 8)
            ++g_attack_refused_logged, Log("[shots] attack 0x%X refused (%s)", s.attack_id, AttackListed() ? "not listed in this game's attack registry" : "the registry not read: the 0x18C cap");
        return false;
    }
    if (!(s.speed >= shotw::kMinReplaySpeed)) {
        ++g_ref_speed;
        return false;
    }
    const std::uint8_t d1 = shotw::MaskD1(s.recD1);
    {
        std::uint16_t a38 = 0, a3A = 0;
        const bool listed = s.rec5A && CategoryFind(s.rec5A, &a38, &a3A);
        if (!shotw::CategoryOk(s.rec5A, d1, listed)) {
            ++g_ref_category;
            return false;
        }
    }
    {
        AcquireSRWLockShared(&g_rs_lock);
        const bool room = g_rs.HasRoom(link::NowMsPrecise());
        ReleaseSRWLockShared(&g_rs_lock);
        if (!room) {
            ++g_ref_set_full;
            return false;
        }
    }
    Replay r;
    std::memcpy(r.origin, origin, sizeof(r.origin));
    for (int k = 0; k < 3; ++k) r.target[k] = origin[k] + static_cast<double>(s.dir[k]) * 150.0;
    r.actor = 0xFFFFFFFFu;  // ship A: never a target actor (an absolute point along the shot)
    r.speed = s.speed;
    r.a5 = s.rec84;
    r.attack = s.attack_id;
    r.a7 = s.rec04;
    r.d0 = shotw::UnpackD0(s.recD0);
    const bool muzzle = muzzle_wanted && (s.flags & shotw::kMuzzle);
    std::uint8_t w = muzzle && g_cfg.shots_muzzle ? s.weapon_id : 0;
    std::uint8_t m = muzzle && (enemy ? g_cfg.enemy_shots_sound : g_cfg.shots_sound != 0) ? s.magazine_id : 0;
    if (w && !shotw::ValidId(w, WeaponCheck(w))) {
        ++g_ref_weapon;
        w = 0;
    }
    if (m && !shotw::ValidId(m, MagCheck(m))) {
        ++g_ref_mag;
        m = 0;
    }
    r.weapon = w, r.mag = m;
    r.patch.on = true;
    r.patch.r05 = s.rec05, r.patch.r58 = s.rec58, r.patch.r5A = s.rec5A, r.patch.rD0 = s.recD0, r.patch.rD1 = d1, r.patch.r94 = s.rec94;
    r.kind = kind;
    const int res = CallCreate(r);
    if (res < 0) {
        ++g_rep_faults;
        Stop("the CreateBullet replay");
        return false;
    }
    if (!t_enqueued) {
        ++g_rep_no_enqueue;
        return false;
    }
    const double added = link::NowMsPrecise();
    AcquireSRWLockExclusive(&g_rs_lock);
    g_rs.Add(t_refs, t_nrefs, added, kind, shotw::ReplayTtlMs(s.speed));
    ReleaseSRWLockExclusive(&g_rs_lock);
    g_last_replay_ms.store(added, std::memory_order_relaxed);
    if (!enemy) ++g_replayed;
    if (enemy) {
        ++g_e_replayed;
        if (w) ++g_e_muzzle;
        if (m) ++g_e_mag;
        return true;
    }
    if (w) ++g_rep_muzzle;
    if (m) ++g_rep_mag;
    return true;
}

// ---------------------------------------------------------------- the shooter side (game thread)
void Hist(unsigned (&ids)[8], unsigned (&ns)[8], unsigned id) {
    for (int i = 0; i < 8; ++i) {
        if (ns[i] && ids[i] == id) {
            ++ns[i];
            return;
        }
        if (!ns[i]) {
            ids[i] = id, ns[i] = 1;
            return;
        }
    }
}

// The attack a bullet category stands for (the Enqueue's own step: registry +0x30 count, +0x38 list, key u8 +0x20; the attack
// u16 at +0x38, or +0x3A when the record's +0xD1 bit 0 is set). Sam's own records carry attack 0 and their category (run
// 20261123-204612): CreateBullet needs the attack id itself.
// The category entry (found: the registry lists `cat`, with its two attack words readable). Also the receiver's check before a
// replay (shotw::CategoryOk): the Enqueue reads list[-1] for a missing category while it holds the bullet system's lock.
bool CategoryFind(std::uint8_t cat, std::uint16_t* at38, std::uint16_t* at3A) noexcept {
    *at38 = *at3A = 0;
    const std::uintptr_t reg = Reg();
    std::int32_t n = 0;
    if (!cat || !reg || !mem::Rd(reg + 0x30, n) || n <= 0 || n > 4096) return false;
    const std::uintptr_t list = mem::Q(reg + 0x38);
    for (std::int32_t i = 0; list && i < n; ++i) {
        const std::uintptr_t e = mem::Q(list + 8ull * static_cast<unsigned>(i));
        std::uint8_t k = 0;
        if (e && mem::Rd(e + 0x20, k) && k == cat) return mem::Rd(e + 0x38, *at38) && mem::Rd(e + 0x3A, *at3A);
    }
    return false;
}

bool BuildShot(const Cap& c, std::uintptr_t sam, shotw::Shot* out) {
    shotw::Shot s;
    float o[3], v[3];
    std::memcpy(o, c.rec + 0x60, sizeof(o));
    std::memcpy(v, c.rec + 0x70, sizeof(v));
    const double sp = std::sqrt(static_cast<double>(v[0]) * v[0] + static_cast<double>(v[1]) * v[1] + static_cast<double>(v[2]) * v[2]);
    if (!(sp > 0.1) || !std::isfinite(sp)) return false;
    for (int k = 0; k < 3; ++k) s.origin[k] = o[k], s.dir[k] = static_cast<float>(v[k] / sp);
    s.speed = static_cast<float>(sp);
    std::uint16_t rec5C = 0, at38 = 0, at3A = 0;
    std::memcpy(&rec5C, c.rec + 0x5C, 2);
    const bool cat_found = !rec5C && c.rec[0x5A] && CategoryFind(c.rec[0x5A], &at38, &at3A);
    s.attack_id = shotw::RecordAttack(rec5C, c.rec[0x5A], cat_found, at38, at3A, c.rec[0xD1]);
    if (rec5C == 0 && c.rec[0x5A]) {
        static unsigned logged = 0;
        if (logged++ < 2) Log("[shots] our record: attack 0 with category %u (D1 0x%02X): the category's attack 0x%X", c.rec[0x5A], c.rec[0xD1], s.attack_id);
    }
    s.rec04 = c.rec[0x04], s.rec05 = c.rec[0x05], s.rec06 = c.rec[0x06], s.rec58 = c.rec[0x58], s.rec5A = c.rec[0x5A];
    s.recD0 = c.rec[0xD0], s.recD1 = shotw::MaskD1(c.rec[0xD1]);
    std::memcpy(&s.rec84, c.rec + 0x84, 4);
    std::memcpy(&s.rec94, c.rec + 0x94, 4);
    if (!std::isfinite(s.rec94)) s.rec94 = 0;
    s.target_actor = shotw::kNoActor;  // ship A: the target actor is never sent (no enemy identity yet)
    s.weapon_id = g_cur_weapon;
    s.magazine_id = g_cur_mag;
    if (g_pull.Next(c.ms, g_cur_weapon)) s.flags |= shotw::kMuzzle;
    const double age = g_now - c.ms;
    s.age_ms = static_cast<std::uint16_t>(age < 0 ? 0 : age > 60000 ? 60000 : age);
    // the hand anchor: the origin from our right hand, in our yaw frame
    WorldTransform xf{};
    double hand[3];
    if (sam && ReadXf(sam, xf) && realbody::JointWorld(reinterpret_cast<game::Entity*>(sam), kHandJoint, hand)) {
        const float fwd[3] = {xf.rot.m[1][0], xf.rot.m[1][1], xf.rot.m[1][2]};
        s.yaw = shotw::YawOf(fwd);
        const double d[3] = {s.origin[0] - hand[0], s.origin[1] - hand[1], s.origin[2] - hand[2]};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 9.0) {
            shotw::ToYawFrame(d, s.yaw, s.hand_rel);
            s.flags |= shotw::kHandRel;
        }
    }
    if (!shotw::AttackAllowed(s.attack_id, true, AttackListed()) || !(s.speed >= shotw::kMinReplaySpeed)) return false;
    s.shot_id = ++g_next_shot_id;
    *out = s;
    return true;
}

// Every shot twice: this tick's new ones, and last tick's again (flag resent). One sender per frame kind (the players' and the
// enemies' never share a frame).
struct Resend {
    shotw::Shot last[shotw::kMaxShots];
    unsigned n = 0;
};
Resend g_resend, g_e_resend;
unsigned SendFrames(Resend& rs, std::uint16_t flags, const shotw::Shot* fresh, unsigned nfresh) {
    shotw::Shot all[shotw::kMaxShots * 3];
    unsigned n = 0, frames = 0;
    for (unsigned i = 0; i < nfresh && n < shotw::kMaxShots * 3; ++i) all[n++] = fresh[i];
    for (unsigned i = 0; i < rs.n && n < shotw::kMaxShots * 3; ++i) {
        all[n] = rs.last[i];
        all[n].flags |= shotw::kResent;
        ++n;
    }
    rs.n = 0;
    for (unsigned i = 0; i < nfresh && i < shotw::kMaxShots; ++i) rs.last[rs.n++] = fresh[i];
    for (unsigned at = 0; at < n; at += shotw::kMaxShots) {
        shotw::Frame f;
        f.flags = flags;
        f.n = static_cast<std::uint8_t>(n - at < shotw::kMaxShots ? n - at : shotw::kMaxShots);
        f.seq = ++g_seq;
        f.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(g_now) & 0xFFFFFFFFull);
        f.world = 0;
        for (unsigned i = 0; i < f.n; ++i) f.shots[i] = all[at + i];
        std::uint8_t buf[shotw::kMaxFrameSize];
        const std::size_t size = shotw::EncodeShots(f, buf, sizeof(buf));
        if (size && link::SendCombatFrame(buf, size)) ++frames;
    }
    return frames;
}

void PushHeld(const Held& h) {
    if (g_nhold >= kHoldMax) {
        // the oldest goes
        std::memmove(&g_hold[0], &g_hold[1], sizeof(Held) * (kHoldMax - 1));
        --g_nhold;
        ++g_hold_full;
    }
    g_hold[g_nhold++] = h;
}
void PushEnemyHeld(const Held& h) {
    if (g_nehold >= kHoldMax) {
        std::memmove(&g_ehold[0], &g_ehold[1], sizeof(Held) * (kHoldMax - 1));
        --g_nehold;
        ++g_e_hold_full;
    }
    g_ehold[g_nehold++] = h;
}

// An enemy's record (the host): the weapon and magazine ids from the record itself (rec +0x58 / +0x59, the fire path's [V]), the
// attack from its category (as Sam's), the pull per shooter, the origin from the MULE's right hand in its yaw frame.
bool BuildEnemyShot(const Cap& c, shotw::Shot* out) {
    shotw::Shot s;
    float o[3], v[3];
    std::memcpy(o, c.rec + 0x60, sizeof(o));
    std::memcpy(v, c.rec + 0x70, sizeof(v));
    const double sp = std::sqrt(static_cast<double>(v[0]) * v[0] + static_cast<double>(v[1]) * v[1] + static_cast<double>(v[2]) * v[2]);
    if (!(sp > 0.1) || !std::isfinite(sp)) return false;
    for (int k = 0; k < 3; ++k) s.origin[k] = o[k], s.dir[k] = static_cast<float>(v[k] / sp);
    s.speed = static_cast<float>(sp);
    std::uint16_t rec5C = 0, at38 = 0, at3A = 0;
    std::memcpy(&rec5C, c.rec + 0x5C, 2);
    const bool cat_found = !rec5C && c.rec[0x5A] && CategoryFind(c.rec[0x5A], &at38, &at3A);
    s.attack_id = shotw::RecordAttack(rec5C, c.rec[0x5A], cat_found, at38, at3A, c.rec[0xD1]);
    s.rec04 = c.rec[0x04], s.rec05 = c.rec[0x05], s.rec06 = c.rec[0x06], s.rec58 = c.rec[0x58], s.rec5A = c.rec[0x5A];
    s.recD0 = c.rec[0xD0], s.recD1 = shotw::MaskD1(c.rec[0xD1]);
    std::memcpy(&s.rec84, c.rec + 0x84, 4);
    std::memcpy(&s.rec94, c.rec + 0x94, 4);
    if (!std::isfinite(s.rec94)) s.rec94 = 0;
    s.target_actor = shotw::kNoActor;
    s.weapon_id = c.rec[0x58];
    s.magazine_id = c.rec[0x59];
    s.shooter = c.shooter;
    {
        const unsigned key = static_cast<unsigned>(s.weapon_id) << 8 | s.magazine_id;
        bool seen = false;
        for (unsigned i = 0; i < g_e_pair_n && !seen; ++i) seen = g_e_pair_id[i] == key;
        if (!seen && g_e_pair_n < 16) {
            g_e_pair_id[g_e_pair_n++] = key;
            const shotw::IdCheck wc = WeaponCheck(s.weapon_id), mc = MagCheck(s.magazine_id);
            Log("[shots] enemy record: weapon %u mag %u category %u attack 0x%X (registry: weapon %s, mag %s) shooter %05X", s.weapon_id, s.magazine_id, s.rec5A,
                s.attack_id, wc.listed ? (wc.child_ok ? "found" : "found, no child list") : "NOT", mc.listed ? "found" : "NOT", c.shooter);
        }
    }
    if (g_e_pulls_by.Next(c.shooter, c.ms, s.weapon_id)) s.flags |= shotw::kMuzzle;
    const double age = g_now - c.ms;
    s.age_ms = static_cast<std::uint16_t>(age < 0 ? 0 : age > 60000 ? 60000 : age);
    WorldTransform xf{};
    double hand[3];
    if (c.owner && ReadXf(c.owner, xf) && realbody::JointWorld(reinterpret_cast<game::Entity*>(c.owner), kHandJoint, hand)) {
        const float fwd[3] = {xf.rot.m[1][0], xf.rot.m[1][1], xf.rot.m[1][2]};
        s.yaw = shotw::YawOf(fwd);
        const double d[3] = {s.origin[0] - hand[0], s.origin[1] - hand[1], s.origin[2] - hand[2]};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 9.0) {
            shotw::ToYawFrame(d, s.yaw, s.hand_rel);
            s.flags |= shotw::kHandRel;
        }
    }
    if (!shotw::AttackAllowed(s.attack_id, true, AttackListed()) || !(s.speed >= shotw::kMinReplaySpeed)) return false;
    s.shot_id = ++g_next_shot_id;
    *out = s;
    return true;
}

void EnemyShooterTick(const TickIn& in, const Cap* caps, unsigned nc) {
    const bool send = in.net && shotw::EnemyCaptureOn(g_enemy_role.load()) && shotw::BothHave(OurPartsFwd(), g_peer_parts, shotw::kPartEnemyFire);
    if (!nc) {
        if (g_e_resend.n && send) g_e_frames += SendFrames(g_e_resend, shotw::kEnemyShooters, nullptr, 0);
        else g_e_resend.n = 0;
        return;
    }
    shotw::Shot fresh[32];
    unsigned nf = 0;
    for (unsigned i = 0; i < nc; ++i) {
        shotw::Shot s;
        if (!BuildEnemyShot(caps[i], &s)) continue;
        if (!g_e_cap.Take(caps[i].ms)) {
            ++g_e_capped;
            continue;
        }
        ++g_e_sent;
        g_last_enemy_shooter = s.shooter;  // the host's enemy_look -2
        if (s.flags & shotw::kMuzzle) ++g_e_pulls;
        fresh[nf++] = s;
    }
    if (send) {
        nf = NarrowFor(fresh, nf);
        g_e_frames += SendFrames(g_e_resend, shotw::kEnemyShooters, fresh, nf);
    } else {
        g_e_no_peer += nf;
        g_e_resend.n = 0;  // never an old pull resent when sending resumes
    }
}

void ShooterTick(const TickIn& in) {
    const std::uintptr_t sam = in.local_ok ? reinterpret_cast<std::uintptr_t>(in.local) : 0;
    ReadCurrentWeapon(sam, &g_cur_weapon, &g_cur_mag);
    Cap all[32];
    unsigned na = 0;
    AcquireSRWLockExclusive(&g_cap_lock);
    while (g_cap_count && na < 32) {
        all[na++] = g_cap[g_cap_head];
        g_cap_head = (g_cap_head + 1) % kCapRing;
        --g_cap_count;
    }
    ReleaseSRWLockExclusive(&g_cap_lock);
    static Cap caps[32], ecaps[32];
    unsigned nc = 0, ne = 0;
    for (unsigned i = 0; i < na; ++i) {
        if (all[i].shooter == shotw::kShooterSam) caps[nc++] = all[i];
        else ecaps[ne++] = all[i];
    }
    AttackRegistryTick();
    EnemyShooterTick(in, ecaps, ne);
    if (!g_on.load()) return;
    if (!nc) {
        if (g_resend.n && in.net && shotw::BothHave(shotw::kPartFire, g_peer_parts, shotw::kPartFire)) g_sent_frames += SendFrames(g_resend, 0, nullptr, 0);  // the resend
        else g_resend.n = 0;
        return;
    }
    shotw::Shot fresh[32];
    unsigned nf = 0;
    WorldTransform sx{};
    const bool have_sx = sam && ReadXf(sam, sx);
    for (unsigned i = 0; i < nc; ++i) {
        shotw::Shot s;
        if (!BuildShot(caps[i], sam, &s)) continue;
        if (!g_cap_rate.Take(caps[i].ms)) {
            ++g_cap_dropped;
            continue;
        }
        g_last_shot = s;
        g_have_last = true;
        ++g_sent;
        if (s.flags & shotw::kMuzzle) ++g_sent_pulls;
        Hist(g_weapon_hist_id, g_weapon_hist_n, s.weapon_id);
        Hist(g_mag_hist_id, g_mag_hist_n, s.magazine_id);
        fresh[nf++] = s;
        if (g_echo && have_sx) {
            Held h;
            h.s = s;
            for (int k = 0; k < 3; ++k) h.s.origin[k] += static_cast<double>(sx.rot.m[0][k]) * g_echo_off;  // to our right
            h.s.flags &= static_cast<std::uint8_t>(~shotw::kHandRel);
            h.echo = true;
            h.rx = g_now;
            PushHeld(h);
        }
    }
    if (in.net) {
        if (shotw::BothHave(shotw::kPartFire, g_peer_parts, shotw::kPartFire)) nf = NarrowFor(fresh, nf), g_sent_frames += SendFrames(g_resend, 0, fresh, nf);
        else g_no_peer += nf;
    }
}

// ---------------------------------------------------------------- the beacon (game thread)
// The fire bit says "send me your shots": only while this game can replay them (CanReplay), so a partner never sends what we drop.
// The enemy fire bit the same for the enemies' shots (enemy_sync and enemy_shots); the mirror bit and the fingerprints are
// enemy_sync's (SetEnemyBeacon).
std::uint8_t OurParts() noexcept {
    std::uint8_t p = g_on.load() && CanReplay() ? shotw::kPartFire : 0;
    p |= g_enemy_parts & (shotw::kPartMirror | shotw::kPartOwn | shotw::kPartCamp);  // P1b / P1c: kPartOwn, kPartCamp are enemy_sync's
    if (AttackListed()) p |= shotw::kPartAttackWide;  // P1c: this build replays the listed attack ids above 0x18C
    if (g_enemy_want && (g_enemy_parts & shotw::kPartMirror) && CanReplay()) p |= shotw::kPartEnemyFire;
    if (g_hits_part.load(std::memory_order_relaxed) && (g_enemy_parts & shotw::kPartMirror)) p |= shotw::kPartHits;  // P2
    // round 23: the tie-up relay (HIT kind 5), only with the hits part
    if ((p & shotw::kPartHits) && g_restrain_part.load(std::memory_order_relaxed)) p |= shotw::kPartRestrain;
    return p;
}
std::uint8_t OurPartsFwd() noexcept { return OurParts(); }

void BeaconTick(const TickIn& in) {
    if (!in.net) return;
    const std::uint32_t gen = link::Generation();
    if (gen != g_gen) {
        g_gen = gen;
        g_have_peer = false;
        g_peer_parts = 0;
        g_peer_beacon = shotw::Beacon{};
        g_peer_down_cap = false;
        g_beacon_fast_until = g_now + 10000;
        g_beacon_next = g_now;
        g_dedupe.Reset();
        g_nhold = 0;
        g_nehold = 0;
    }
    // the partner's
    std::uint8_t buf[64];
    std::size_t n = 0;
    std::uint32_t fgen = 0;
    while (link::PopCombatFrame(link::CombatFrame::beacon, buf, sizeof(buf), n, fgen)) {
        shotw::Beacon b;
        if (fgen != gen || !shotw::DecodeBeacon(buf, n, b)) continue;
        g_have_peer = true;
        g_peer_ms = g_now;
        g_peer_parts = b.parts;
        g_peer_beacon = b;
        if (shotw::PeerTakesDown(g_peer_down_cap, b.parts) != g_peer_down_cap) {
            g_peer_down_cap = true;
            Log("[enemy] the partner's build takes our player's down code (beacon byte 22; kept for this link even if its restrain part drops)");
        }
    }
    if (g_have_peer && g_now - g_peer_ms > 20000) {
        g_have_peer = false;
        g_peer_parts = 0;
        g_peer_beacon = shotw::Beacon{};
    }
    const std::uint8_t ours = OurParts();
    static std::uint32_t sent_print = 0;
    const std::uint32_t print = static_cast<std::uint32_t>(g_enemy_count) ^ g_enemy_ids ^ (g_enemy_guids * 3u) ^ g_enemy_freeze;
    if (print != sent_print) sent_print = print, g_beacon_next = g_now;  // new fingerprints: at once
    if (ours != g_last_logged_parts || g_peer_parts != g_last_logged_peer) {
        const bool changed_ours = ours != g_last_logged_parts;
        g_last_logged_parts = ours, g_last_logged_peer = g_peer_parts;
        const bool fire = shotw::BothHave(ours, g_peer_parts, shotw::kPartFire);
        const bool hits = shotw::BothHave(ours, g_peer_parts, shotw::kPartHits);
        Log("[link] combat parts: ours 0x%02X theirs 0x%02X%s: fire %s%s%s; hits %s%s%s", ours, g_peer_parts, g_have_peer ? "" : " (no beacon yet)", fire ? "on" : "off",
            fire                                   ? ""
            : !(ours & shotw::kPartFire)           ? (g_on.load() ? " (no replays here)" : " (shots off here)")
            : g_have_peer                          ? " (the partner does not replay shots: shots_sync=0 or its damage drop is not installed)"
                                                   : "",
            shotw::BothHave(ours, g_peer_parts, shotw::kPartEnemyFire) ? "; enemy fire on" : "", hits ? "on" : "off",
            hits                         ? ""
            : !(ours & shotw::kPartHits) ? " (here: "
            : g_have_peer                ? " (the partner has not: enemy_hits=0 there, its hooks are off or its mirrors are off)"
                                         : " (no beacon yet)",
            hits || (ours & shotw::kPartHits) ? "" : !(g_enemy_parts & shotw::kPartMirror) ? "enemy sync off)" : g_hits_why);
        if (changed_ours) g_beacon_next = g_now;
    }
    // round 23 C: our player's down code, only to a round-23 partner (kPartRestrain seen on this link: its decoder, not its current
    // parts, so a fault there never turns our "down" into a 0 it would read as "back"; review); a change goes out at once, then every
    // 1 s for 5 s (the beacon is unreliable)
    const std::uint8_t down = g_peer_down_cap ? g_player_down : std::uint8_t{0};
    if (down != g_down_sent) {
        g_down_sent = down;
        g_beacon_next = g_now;
        if (g_beacon_fast_until < g_now + 5000) g_beacon_fast_until = g_now + 5000;
    }
    if (g_now < g_beacon_next) return;
    g_beacon_next = g_now + (g_now < g_beacon_fast_until ? 1000 : 5000);
    shotw::Beacon b;
    b.player_down = down;
    b.parts = ours;
    b.role = in.role;
    b.enemy_count = g_enemy_count;
    b.ids_fp = g_enemy_ids;
    b.guids_fp = g_enemy_guids;
    b.freeze_mode = g_enemy_freeze;
    std::uint8_t out[shotw::kBeaconSize];
    if (shotw::EncodeBeacon(b, out, sizeof(out))) link::SendCombatFrame(out, sizeof(out));
}

// ---------------------------------------------------------------- the receiver side (game thread)
void ReceiveTick(const TickIn& in) {
    std::uint8_t buf[1024];
    std::size_t n = 0;
    std::uint32_t fgen = 0;
    static shotw::Frame f;
    while (link::PopCombatFrame(link::CombatFrame::shots, buf, sizeof(buf), n, fgen)) {
        if (fgen != link::Generation()) continue;
        if (!shotw::DecodeShots(buf, n, f)) {
            ++g_rx_bad;
            continue;
        }
        if (f.flags & shotw::kEnemyShooters) {
            // the host's enemies (enemy sync P1a): held until the mirror shows the moment it fired (SEN1's clock)
            if (!shotw::EnemyReplayOn(g_enemy_role.load())) {
                g_e_ref_off += f.n;
                continue;
            }
            double render = 0;
            const bool clock = esync::RenderClock(&render);
            for (unsigned i = 0; i < f.n; ++i) {
                const shotw::Shot& s = f.shots[i];
                if (!g_dedupe.Accept(s.shot_id)) {
                    ++g_rx_dup;
                    continue;
                }
                ++g_e_rx;
                g_last_enemy_rx = s;
                g_have_enemy_rx = true;
                double nh[3] = {};
                if (!esync::MirrorEntity(s.shooter) && !esync::NotHoldingShooter(s.shooter, nh, nullptr)) {  // (round 23 B: hidden as not holding)
                    ++g_e_ref_shooter;
                    continue;
                }
                Held h;
                h.s = s;
                const std::uint32_t fired = f.sender_ms - s.age_ms;
                h.due = clock ? shotw::UnwrapNear(fired, render) : 0;
                h.rx = g_now;
                PushEnemyHeld(h);
            }
            continue;
        }
        if (!g_on.load()) continue;
        for (unsigned i = 0; i < f.n; ++i) {
            const shotw::Shot& s = f.shots[i];
            if (s.shooter != shotw::kShooterSam) continue;
            if (!g_dedupe.Accept(s.shot_id)) {
                ++g_rx_dup;
                continue;
            }
            ++g_rx;
            g_last_rx = s;
            g_have_rx = true;
            Held h;
            h.s = s;
            const std::uint32_t fired = f.sender_ms - s.age_ms;
            h.due = in.have_play ? shotw::UnwrapNear(fired, in.play_ms) : 0;
            h.rx = g_now;
            PushHeld(h);
        }
    }
}

bool BodyHand(game::Entity* body, double (&hand)[3], float* yaw, double (&root)[3]) {
    WorldTransform xf{};
    if (!body || !ReadXf(reinterpret_cast<std::uintptr_t>(body), xf)) return false;
    root[0] = xf.pos.x, root[1] = xf.pos.y, root[2] = xf.pos.z;
    const float fwd[3] = {xf.rot.m[1][0], xf.rot.m[1][1], xf.rot.m[1][2]};
    *yaw = shotw::YawOf(fwd);
    return realbody::JointWorld(body, kHandJoint, hand);
}

unsigned ReplayTick(const TickIn& in) {
    if (!g_nhold) return 0;
    double hand[3] = {}, root[3] = {};
    float yaw = 0;
    if (!CanReplay()) {
        // without the Sam damage filter (or the NPC drop) a replay could hurt our Sam or this world's MULEs: nothing is replayed
        g_ref_filter += g_nhold;
        g_nhold = 0;
        return 0;
    }
    const bool have_hand = BodyHand(in.body, hand, &yaw, root);
    unsigned done = 0;
    unsigned keep = 0;
    for (unsigned i = 0; i < g_nhold; ++i) {
        Held& h = g_hold[i];
        bool fire = false;
        if (h.echo || h.test) {
            fire = true;
        } else {
            const shotw::Gate g = shotw::PlayGate(in.body && in.have_play, in.play_ms, h.due);
            if (g == shotw::Gate::hold && g_now - h.rx < 1500) {
                g_hold[keep++] = h;
                continue;
            }
            fire = true;
            if (g == shotw::Gate::late) ++g_rep_late;
            else if (g != shotw::Gate::fire) ++g_rep_now;
        }
        if (!fire) continue;
        if (done >= 8) {  // the bullet system takes 16 a frame; leave the game its share
            g_hold[keep++] = h;
            continue;
        }
        double origin[3];
        if (h.echo) {
            std::memcpy(origin, h.s.origin, sizeof(origin));
        } else {
            // the partner's shots start at the partner body: none drawn here (another region, not spawned yet), none replayed
            if (!in.body || !g_have_body_xf) {
                ++g_ref_origin;
                continue;
            }
            shotw::AnchoredOrigin(h.s, have_hand, hand, yaw, 1.5, origin);
            // the shot must start near the partner body (a stale world, a teleport, a malformed peer: refused)
            const double b[3] = {g_body_xf.pos.x, g_body_xf.pos.y, g_body_xf.pos.z};
            if (Dist3(origin, b) > 30.0) {
                ++g_ref_origin;
                continue;
            }
        }
        ReplayShot(h.s, origin, h.test ? 3 : 1, true);
        ++done;
        if (g_stopped.load()) {
            g_nhold = 0;
            return done;
        }
    }
    g_nhold = keep;
    return done;
}

// The enemy shots (the guest, enemy sync P1a): after the partner's within the 8 a tick, each at the mirror's render time on the
// SEN1 clock, from the mirror's drawn hand (within 1.5 m of the shot's origin), refused beyond 30 m from the mirror or when the
// shooter is no live frozen mirror here.
void EnemyReplayTick(unsigned done) {
    if (!g_nehold) return;
    if (!CanReplay()) {
        g_ref_filter += g_nehold;
        g_nehold = 0;
        return;
    }
    double render = 0;
    const bool clock = esync::RenderClock(&render);
    unsigned keep = 0;
    for (unsigned i = 0; i < g_nehold; ++i) {
        Held& h = g_ehold[i];
        const shotw::Gate g = erules::EnemyShotGate(clock && h.due != 0, render, h.due);
        if (g == shotw::Gate::hold && g_now - h.rx < 1500) {
            g_ehold[keep++] = h;
            continue;
        }
        if (g == shotw::Gate::late) {
            // over 500 ms past its moment on the mirror: dropped, never fired from where the mirror stands now
            ++g_e_late;
            continue;
        }
        if (done >= 8) {
            g_ehold[keep++] = h;
            continue;
        }
        std::uintptr_t e = esync::MirrorEntity(h.s.shooter);
        WorldTransform xf{};
        if (!e || !ReadXf(e, xf)) {
            // round 23 B: a copy hidden here as not holding still fires, from the owner's place (its SEN1 track now)
            double nh[3] = {};
            if (e || !esync::NotHoldingShooter(h.s.shooter, nh, &e)) {
                ++g_e_ref_shooter;
                continue;
            }
            xf.pos.x = nh[0], xf.pos.y = nh[1], xf.pos.z = nh[2];
            ++g_e_nothold;
        }
        // the host's own muzzle place: the mirror stands where the host's MULE stood when it fired (within centimetres at the
        // play gate, run 20261124-102043's census), and an origin moved onto a mirror's drawn hand (an unposed mirror plays
        // its own idle) started inside MULE bodies (run 20261124-102419: 8 of 11 replay hits on NPCs)
        double origin[3] = {h.s.origin[0], h.s.origin[1], h.s.origin[2]};
        const double m[3] = {xf.pos.x, xf.pos.y, xf.pos.z};
        if (Dist3(origin, m) > 30.0) {
            ++g_e_ref_origin;
            continue;
        }
        for (int k = 0; k < 3; ++k) origin[k] += static_cast<double>(h.s.dir[k]) * g_enemy_ahead;  // shots 10 (hidden runs; 0 by default)
        if (ReplayShot(h.s, origin, shotw::kReplayKindEnemy, true, true)) {
            NoteEnemyReplay(e, h.s, origin, g_e_logged + 1);
            g_last_enemy_shooter = h.s.shooter;
            // one by one: the first 20 (hidden runs: 200, the scenarios' markers for the stills), then every 20th (Log flushes every line)
            if (++g_e_logged <= (automation::Enabled() ? 200u : 20u) || g_e_logged % 20 == 0)
                Log("[shots] enemy replay #%u: shooter %05X, weapon %u mag %u attack 0x%X, %.0f m/s, %.1f m from the mirror%s", g_e_logged, h.s.shooter, h.s.weapon_id,
                    h.s.magazine_id, h.s.attack_id, static_cast<double>(h.s.speed), Dist3(origin, m), (h.s.flags & shotw::kMuzzle) ? ", muzzle" : "");
        }
        ++done;
        if (g_stopped.load()) {
            g_nehold = 0;
            return;
        }
    }
    g_nehold = keep;
}


// ---------------------------------------------------------------- shots_test / the leak test (game thread)
bool TestOrigin(double dist, double up, double (&origin)[3], double (&target)[3], bool into_ground) {
    const std::uintptr_t sam = g_sam.load();
    WorldTransform xf{};
    if (!sam || !ReadXf(sam, xf)) return false;
    const double p[3] = {xf.pos.x, xf.pos.y, xf.pos.z};
    for (int k = 0; k < 3; ++k) origin[k] = p[k] + static_cast<double>(xf.rot.m[1][k]) * dist;
    origin[2] += up;
    if (into_ground) {
        // down into the ground further ahead, away from Sam
        for (int k = 0; k < 3; ++k) target[k] = p[k] + static_cast<double>(xf.rot.m[1][k]) * (dist + 6.0);
        target[2] -= 1.0;
    } else {
        target[0] = p[0], target[1] = p[1], target[2] = p[2] + 1.2;  // his chest
    }
    return true;
}

shotw::Shot TestShot(const double (&origin)[3], const double (&target)[3], int flags) {
    shotw::Shot s = g_last_shot;
    for (int k = 0; k < 3; ++k) s.origin[k] = origin[k];
    const double d[3] = {target[0] - origin[0], target[1] - origin[1], target[2] - origin[2]};
    const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int k = 0; k < 3; ++k) s.dir[k] = static_cast<float>(d[k] / (l > 1e-6 ? l : 1));
    s.flags = shotw::kMuzzle;
    if (flags & 2) s.weapon_id = 0;
    if (flags & 4) s.magazine_id = 0;
    return s;
}

SIZE_T WorkingSet() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? pmc.WorkingSetSize : 0;
}

void LeakTick() {
    if (g_leak_left <= 0) return;
    for (int i = 0; i < 4 && g_leak_left > 0; ++i) {
        double o[3], t[3];
        if (!TestOrigin(10.0, 1.5, o, t, true)) {
            g_leak_left = 0;
            break;
        }
        const shotw::Shot s = TestShot(o, t, 0);
        ReplayShot(s, o, 3, (g_leak_done % 8) == 0);
        --g_leak_left;
        ++g_leak_done;
        if (g_stopped.load()) {
            g_leak_left = 0;
            break;
        }
    }
    if (g_leak_left <= 0) {
        const SIZE_T ws1 = WorkingSet();
        Log("[shots] leak test: %d replays; working set %.1f MB -> %.1f MB (%+.1f MB)", g_leak_done, g_leak_ws0 / 1048576.0, ws1 / 1048576.0,
            (static_cast<double>(ws1) - static_cast<double>(g_leak_ws0)) / 1048576.0);
    }
}

float Health() noexcept;  // below
void EnemyTestTick() {
    if (!g_et_wait || g_now - g_et_t0 < 2000) return;
    g_et_wait = false;
    g_et_armed.store(false);
    const int match = g_et_match.load(), kept = g_et_kept.load();
    const float h1 = Health();
    Log("[enemy] shot test: ref match=%d %s amount %.1f health %.1f -> %.1f (mirror %05X)", match,
        kept == 1 ? "kept (enemy_shots_hurt)" : kept == 0 ? (g_et_nohurt.load() ? "dropped (nohurt)" : "dropped") : "no damage message within 2 s",
        static_cast<double>(g_et_amount.load()), static_cast<double>(g_et_h0), static_cast<double>(h1), g_et_id);
    g_et_nohurt.store(false);
}

void TestTick() {
    if (!g_test_wait || g_now - g_test_t0 < 2000) return;
    g_test_wait = false;
    const int ref = g_test_ref_match.load(), dm = g_test_dummy_match.load(), dropped = g_test_dropped.load();
    Log("[shots] test: ref match=%d (how %d, word %d), dummy owner seen=%d; %s", ref, g_test_how.load(), g_test_slot.load(), dm,
        dropped == 1                    ? "dropped by the filter (ref match)"
        : g_test_nofilter.load() && ref ? "kept (nofilter)"
        : dm                            ? "a dummy-owner hit only (never dropped)"
                                        : "no damage message of the replay within 2 s");
    g_test_armed.store(false);
    g_test_nofilter.store(false);  // a later leak test (kind 3) keeps the filters
}

// ---------------------------------------------------------------- the aimed test (hidden runs: shots 5 / 6; round 16)
// shots 5 <target> <n>: n (1..8) replays of the partner's last received shot (else our own last captured one) from 6 m in front
// of a target of this world, straight at it, kind 4 (a partner replay for every filter); a negative n is the control: their
// damage let through at the bullet. 2.5 s later one line: what the drop at the bullet counted and the target's value before
// and after (a vehicle's life, a structure's durability, a cargo piece's +0x34; the MULE: the counters only).
// shots 6: one piece our Sam carries put on the ground through the game's own script exports (the cargo target).
void HitDropText(char* out, std::size_t cap, const std::atomic<unsigned> (&c)[shotw::kHitClasses]);  // below
constexpr const char* kTargetName[5] = {"?", "vehicle", "structure", "cargo", "MULE"};
struct AimedTest {
    bool wait = false;
    int target = 0, stype = 0;  // stype: the structure's type (0 any)
    bool keep = false;
    double t0 = 0;
    int n = 0;
    unsigned enq = 0, num = 0;
    float before = 0;
    std::uintptr_t obj = 0;  // the vehicle entity, the cargo's DSBaggage2, the MULE entity
    std::uint32_t sid = 0;   // the structure's id
    char what[96] = {};
    unsigned drop0[shotw::kHitClasses] = {}, kept0[shotw::kHitClasses] = {};
    unsigned sam0 = 0, bt0 = 0, npc0 = 0, samf0 = 0;
} g_at;
std::uintptr_t g_drop_bag = 0;  // shots 6's piece

std::uintptr_t BagMgr() noexcept { return mem::Q(A(kBagMgrGlobal)); }
bool BagInUse(std::uintptr_t bag) noexcept {
    std::uint8_t u = 0;
    return bag && mem::Rd(bag + kBagInUse, u) && u;
}

// The target's aim point and value (NaN when it has none). obj / sid: 0 = pick the nearest (and keep it), else the same one.
bool TargetRead(int target, int stype, std::uintptr_t* obj, std::uint32_t* sid, double (&pt)[3], float* value, char* what, std::size_t cap) {
    *value = std::nanf("");
    switch (target) {
        case 1: {
            std::uintptr_t e = 0;
            double p[3];
            float life = 0;
            if (!vshare::NearestVehicleLife(40, &e, p, &life) || (*obj && e != *obj)) return std::snprintf(what, cap, "no vehicle within 40 m"), false;
            *obj = e;
            pt[0] = p[0], pt[1] = p[1], pt[2] = p[2] + 1.0;
            *value = life;
            const char* rn = mem::RttiName(mem::RttiOf(e));
            std::snprintf(what, cap, "%s, life", rn ? rn : "?");
            return true;
        }
        case 2: {
            std::uint32_t id = 0;
            int type = 0;
            double p[3];
            float dur = 0;
            if (!wshare::NearestStructure(40, stype, &id, &type, p, &dur) || (*sid && id != *sid))
                return std::snprintf(what, cap, "no structure%s within 40 m", stype ? " of that type" : ""), false;
            *sid = id;
            pt[0] = p[0], pt[1] = p[1], pt[2] = p[2] + 1.0;
            *value = dur;
            std::snprintf(what, cap, "id %u type %d, durability", id, type);
            return true;
        }
        case 3: {
            const std::uintptr_t bag = *obj ? *obj : g_drop_bag;
            double p[3];
            std::int32_t eidx = -2;
            float life = 0;
            if (!BagInUse(bag) || !mem::Read(bag + kBagPos, p, sizeof(p)) || !std::isfinite(p[0]) || !mem::Rd(bag + kBagLife, life))
                return std::snprintf(what, cap, "no dropped piece (shots 6 first)"), false;
            mem::Rd(bag + kBagEntityIdx, eidx);
            *obj = bag;
            pt[0] = p[0], pt[1] = p[1], pt[2] = p[2] + 0.15;
            *value = life;
            std::snprintf(what, cap, "DSBaggage2 %p, entity index %d, +0x34", reinterpret_cast<void*>(bag), eidx);
            return true;
        }
        case 4: {
            const std::uintptr_t e = esync::PickedEntity();
            WorldTransform xf{};
            if (!e || (*obj && e != *obj) || !ReadXf(e, xf)) return std::snprintf(what, cap, "no picked MULE (enemy_probe 0 first)"), false;
            *obj = e;
            pt[0] = xf.pos.x, pt[1] = xf.pos.y, pt[2] = xf.pos.z + 1.2;
            const char* rn = mem::RttiName(mem::RttiOf(e));
            std::snprintf(what, cap, "%s (no value: the counters)", rn ? rn : "?");
            return true;
        }
        default: return std::snprintf(what, cap, "target 1 vehicle, 2 structure, 3 cargo, 4 the enemy probe's MULE"), false;
    }
}

bool AimedStart(int target, int n, char* msg, std::size_t cap) {
    if (g_at.wait) return std::snprintf(msg, cap, "an aimed test is running"), false;
    if (!g_have_rx && !g_have_last) return std::snprintf(msg, cap, "no shot yet (the partner's or ours: fire once first)"), false;
    if (!g_hit_hooked) return std::snprintf(msg, cap, "the drop at the bullet is off"), false;
    const bool keep = n < 0;
    n = n < 0 ? -n : n;
    n = n < 1 ? 1 : n > 8 ? 8 : n;
    AimedTest t;
    if (target >= 100) t.stype = target - 100, target = 2;  // 100 + type: a structure of that type (106 a postbox)
    if (target < 1 || target > 4) return std::snprintf(msg, cap, "target 1 vehicle, 2 structure (100 + type: of that type), 3 cargo, 4 the enemy probe's MULE"), false;
    t.target = target, t.keep = keep, t.n = n;
    double pt[3];
    if (!TargetRead(target, t.stype, &t.obj, &t.sid, pt, &t.before, t.what, sizeof(t.what))) return std::snprintf(msg, cap, "%s", t.what), false;
    WorldTransform sx{};
    if (!ReadXf(g_sam.load(), sx)) return std::snprintf(msg, cap, "no local Sam"), false;
    const double from[3] = {sx.pos.x, sx.pos.y, sx.pos.z};
    double o[3];
    shotw::AimedOrigin(pt, from, 6.0, 0.4, o);
    shotw::Shot s = g_have_rx ? g_last_rx : g_last_shot;
    const double d[3] = {pt[0] - o[0], pt[1] - o[1], pt[2] - o[2]};
    const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int k = 0; k < 3; ++k) s.origin[k] = o[k], s.dir[k] = static_cast<float>(d[k] / (l > 1e-6 ? l : 1));
    s.flags = shotw::kMuzzle;
    for (unsigned i = 0; i < shotw::kHitClasses; ++i) t.drop0[i] = g_hd_drop[i].load(), t.kept0[i] = g_hd_kept_cls[i].load();
    t.sam0 = g_hd_sam.load(), t.bt0 = g_hd_bt.load(), t.npc0 = g_drop_npc.load(), t.samf0 = g_drop_sam_ref.load();
    g_aimed_keep.store(keep);
    for (int i = 0; i < n; ++i) {
        if (ReplayShot(s, o, 4, i == 0)) ++t.enq;
        if (g_stopped.load()) break;
    }
    t.t0 = g_now;
    t.wait = true;
    static unsigned num = 0;
    t.num = ++num;
    g_at = t;
    Log("[shots] aimed test #%u: %d replay(s) of %s shot at the %s (%s %.2f) from %.1f m%s: %u enqueued", t.num, n, g_have_rx ? "the partner's last" : "our last",
        kTargetName[target], t.what, static_cast<double>(t.before), l, keep ? " (control: their damage let through)" : "", t.enq);
    return std::snprintf(msg, cap, "aimed test #%u: %u enqueued", t.num, t.enq), t.enq > 0;
}

void AimedTick() {
    if (!g_at.wait || g_now - g_at.t0 < 2500) return;
    g_at.wait = false;
    g_aimed_keep.store(false);
    double pt[3];
    float after = std::nanf("");
    char what[96];
    const bool readable = TargetRead(g_at.target, g_at.stype, &g_at.obj, &g_at.sid, pt, &after, what, sizeof(what));
    std::atomic<unsigned> dd[shotw::kHitClasses], kk[shotw::kHitClasses];
    unsigned dsum = 0, ksum = 0;
    for (unsigned i = 0; i < shotw::kHitClasses; ++i) {
        dd[i].store(g_hd_drop[i].load() - g_at.drop0[i]), kk[i].store(g_hd_kept_cls[i].load() - g_at.kept0[i]);
        dsum += dd[i].load(), ksum += kk[i].load();
    }
    char dt[160], kt[160];
    HitDropText(dt, sizeof(dt), dd);
    HitDropText(kt, sizeof(kt), kk);
    const float eps = g_at.target == 1 ? 0.5f : g_at.target == 2 ? 5.0f : 0.0005f + std::fabs(g_at.before) * 1e-4f;
    const char* verdict = g_at.target == 4 || !std::isfinite(g_at.before) ? "n/a"
                          : !readable                                      ? "UNREADABLE"
                          : shotw::ValueUnchanged(g_at.before, after, eps) ? "unchanged"
                                                                           : "CHANGED";
    Log("[shots] aimed test #%u: %s (%s) x %d%s: enqueued %u; at the bullet dropped +%u (%s), let through +%u (%s), our Sam passed +%u, BT +%u; "
        "Sam filter +%u, NPC backstop +%u; value before %.3f after %.3f: %s",
        g_at.num, kTargetName[g_at.target], what, g_at.n, g_at.keep ? " (control)" : "", g_at.enq, dsum, dt, ksum, kt, g_hd_sam.load() - g_at.sam0,
        g_hd_bt.load() - g_at.bt0, g_drop_sam_ref.load() - g_at.samf0, g_drop_npc.load() - g_at.npc0, static_cast<double>(g_at.before), static_cast<double>(after),
        verdict);
}

// shots 6: the cargo target. A piece on the ground within 80 m (a DSBaggage2 with an entity of its own: lost, dropped or placed
// cargo) when there is one; else one piece our Sam carries (within 2.5 m of him, no entity of its own, on a holder) put on the
// ground through the game's own script exports (run 20261124-092442: the piece stayed without an entity: unproven).
using FnAddTemp = void (*)(std::uint32_t);
using FnPlace = void (*)(std::uint8_t);
bool CallDrop(std::uint32_t idx) noexcept {
    __try {
        reinterpret_cast<FnAddTemp>(A(kBagAddTemp))(idx);
        reinterpret_cast<FnPlace>(A(kBagPlaceThunk))(0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool DropCarried(char* msg, std::size_t cap) {
    if (!g_cargo_checks_ok) return std::snprintf(msg, cap, "the baggage exports' byte checks failed"), false;
    const std::uintptr_t mgr = BagMgr();
    std::int32_t listed = -1;
    if (!mgr || !mem::Rd(mgr + kBagTempList, listed)) return std::snprintf(msg, cap, "no baggage manager"), false;
    if (listed != 0) return std::snprintf(msg, cap, "the game's temporary baggage list holds %d: not touched", listed), false;
    WorldTransform sx{};
    if (!ReadXf(g_sam.load(), sx)) return std::snprintf(msg, cap, "no local Sam"), false;
    const double sp[3] = {sx.pos.x, sx.pos.y, sx.pos.z};
    constexpr unsigned kChunk = 64;
    static std::uint8_t buf[kChunk * kBagStride];
    std::uintptr_t best = 0, ground = 0;
    std::uint32_t best_i = 0, ground_i = 0;
    double best_z = -1e18, ground_d = 80.0;
    std::int32_t ground_e = -1;
    unsigned in_use = 0, close_by = 0, realized = 0;
    for (unsigned base = 0; base < kBagMax; base += kChunk) {
        const unsigned cnt = kBagMax - base < kChunk ? kBagMax - base : kChunk;
        if (!mem::Read(mgr + kBagArray + kBagStride * base, buf, kBagStride * cnt)) continue;
        for (unsigned j = 0; j < cnt; ++j) {
            const std::uint8_t* b = buf + kBagStride * j;
            const std::uint8_t t = b[kBagInUse];
            if (!t) continue;
            ++in_use;
            if (!(static_cast<std::uint8_t>(t - 1) < 8 || t > 10)) continue;  // the place step's own type check
            double p[3];
            std::int32_t eidx = 0;
            std::uint64_t holder = 0;
            std::memcpy(p, b + kBagPos, sizeof(p));
            std::memcpy(&eidx, b + kBagEntityIdx, 4);
            std::memcpy(&holder, b + kBagHolder, 8);
            if (!std::isfinite(p[0])) continue;
            const double dist = Dist3(p, sp);
            if (eidx >= 0) {
                // a piece of the world with an entity of its own (lost, dropped or placed cargo): the target as it is
                ++realized;
                if (dist > 3.0 && dist < ground_d) ground_d = dist, ground = mgr + kBagArray + kBagStride * (base + j), ground_i = base + j, ground_e = eidx;
                continue;
            }
            if (eidx != -1 || !holder || dist > 2.5) continue;
            ++close_by;
            if (p[2] > best_z) best_z = p[2], best = mgr + kBagArray + kBagStride * (base + j), best_i = base + j;
        }
    }
    if (ground) {
        g_drop_bag = ground;
        float life = 0;
        mem::Rd(ground + kBagLife, life);
        Log("[shots] test: the cargo target: a piece on the ground, DSBaggage2 #%u %p %.1f m away, entity index %d, +0x34 %.3f (%u pieces in use, %u with an entity)",
            ground_i, reinterpret_cast<void*>(ground), ground_d, ground_e, static_cast<double>(life), in_use, realized);
        return std::snprintf(msg, cap, "piece #%u on the ground, %.1f m", ground_i, ground_d), true;
    }
    if (!best) return std::snprintf(msg, cap, "no carried piece found (%u pieces in use, %u near our Sam)", in_use, close_by), false;
    float life0 = 0;
    mem::Rd(best + kBagLife, life0);
    if (!CallDrop(best_i)) return std::snprintf(msg, cap, "the drop faulted"), false;
    g_drop_bag = best;
    std::int32_t eidx = -2;
    mem::Rd(best + kBagEntityIdx, eidx);
    Log("[shots] test: a carried piece put on the ground: DSBaggage2 #%u %p (%.2f m above Sam's root; +0x34 %.3f), entity index now %d; %u pieces in use, %u carried near him",
        best_i, reinterpret_cast<void*>(best), best_z - sp[2], static_cast<double>(life0), eidx, in_use, close_by);
    return std::snprintf(msg, cap, "piece #%u dropped", best_i), true;
}

float Health() noexcept {
    game::Diagnostics d;
    game::ReadDiagnostics(&d);
    return d.health_valid ? d.health : -1.0f;
}

// The drop at the bullet's counters as one phrase: "vehicle n cargo n structure n bt n npc n player n other n".
void HitDropText(char* out, std::size_t cap, const std::atomic<unsigned> (&c)[shotw::kHitClasses]) {
    std::size_t k = 0;
    out[0] = 0;
    for (unsigned i = 0; i < shotw::kHitClasses && k < cap; ++i)
        k += static_cast<std::size_t>(std::snprintf(out + k, cap - k, "%s%s %u", i ? " " : "", shotw::HitClassName(static_cast<shotw::HitClass>(i)), c[i].load()));
}
unsigned HitDropTotal() noexcept {
    unsigned t = 0;
    for (const auto& c : g_hd_drop) t += c.load();
    return t;
}

void StatusLine(char* out, std::size_t cap) {
    unsigned seen = 0, faults = 0, dropped[samdmg::kMax] = {};
    samdmg::Counts(&seen, dropped, &faults);
    char hd[160];
    HitDropText(hd, sizeof(hd), g_hd_drop);
    const int k = std::snprintf(out, cap,
                  "%s%s; hooks: enqueue %s, sam damage %s, npc damage %s; captured %u (others %u, ring full %u), sent %u (pulls %u, frames %u, no peer %u, cap %u), "
                  "received %u (dup %u, bad %u), replayed %u (muzzle %u, mag %u, late %u, at once %u, not enqueued %u), refused weapon %u mag %u origin %u "
                  "filter %u category %u speed %u set full %u, faults %u; replay hits dropped: our Sam %u (dummy-owner hits seen in tests %u), NPCs %u, "
                  "kept %u; echo %s; peer parts 0x%02X; weapon %u mag %u; our health %.1f",
                  g_on.load() ? "ON" : "OFF: ", g_on.load() ? "" : g_off, g_hooked ? "on" : "off", g_filter_registered ? "on" : "off", g_npc_hooked ? "on" : "off",
                  g_captured.load(), g_others.load(), g_cap_full.load(), g_sent, g_sent_pulls, g_sent_frames, g_no_peer, g_cap_dropped, g_rx, g_rx_dup, g_rx_bad,
                  g_replayed, g_rep_muzzle, g_rep_mag, g_rep_late, g_rep_now, g_rep_no_enqueue, g_ref_weapon, g_ref_mag, g_ref_origin, g_ref_filter,
                  g_ref_category, g_ref_speed, g_ref_set_full, g_rep_faults + g_det_faults.load(), g_drop_sam_ref.load(), g_seen_sam_dummy.load(), g_drop_npc.load(), g_kept_ff.load() + g_kept_test.load(), g_echo ? "on" : "off", g_peer_parts,
                  g_cur_weapon, g_cur_mag, static_cast<double>(Health()));
    if (k > 0 && static_cast<std::size_t>(k) < cap)
        std::snprintf(out + k, cap - static_cast<std::size_t>(k),
                      "; at the bullet (%s): dropped %s; seen %u, our Sam passed %u (parts of him hit %u), test let through %u, BT "
                      "reactions dropped %u, faults %u; hits on frozen mirrors dropped %u; reused references kept on our Sam %u; refused (dummy unknown) %u",
                      g_hit_hooked ? (g_bt_hooked ? "on, BT on" : "on, BT off") : "off", hd, g_hd_seen.load(), g_hd_sam.load(), g_hd_part.load(),
                      g_hd_kept_test.load(), g_hd_bt.load(), g_hd_faults.load(), g_hd_mirror.load(), g_kept_reuse.load(), g_ref_dummy);
}

void LogTick() {
    if (g_now < g_next_log_ms) return;
    g_next_log_ms = g_now + 5000;
    static unsigned logged_others = 0;
    if (g_others.load() != logged_others && g_captured.load() == 0) {
        logged_others = g_others.load();
        Log("[shots] enqueues seen: %u, none of them our Sam's", logged_others);
    }
    if (g_sent != g_logged_sent) {
        g_logged_sent = g_sent;
        char w[96] = {}, m[96] = {};
        std::size_t k = 0;
        for (int i = 0; i < 8 && g_weapon_hist_n[i]; ++i) k += std::snprintf(w + k, sizeof(w) - k, "%s%u:%u", i ? " " : "", g_weapon_hist_id[i], g_weapon_hist_n[i]);
        k = 0;
        for (int i = 0; i < 8 && g_mag_hist_n[i]; ++i) k += std::snprintf(m + k, sizeof(m) - k, "%s%u:%u", i ? " " : "", g_mag_hist_id[i], g_mag_hist_n[i]);
        Log("[shots] sent n=%u (pulls %u, pellets %u, frames %u; attack 0x%X speed %.0f m/s) weapon ids {%s} mag ids {%s}; dropped (cap) %u, no peer %u", g_sent, g_sent_pulls,
            g_sent - g_sent_pulls, g_sent_frames, g_have_last ? g_last_shot.attack_id : 0, g_have_last ? static_cast<double>(g_last_shot.speed) : 0.0, w, m, g_cap_dropped,
            g_no_peer);
    }
    const unsigned refused = g_ref_origin + g_ref_attack + g_ref_filter + g_ref_category + g_ref_speed + g_ref_set_full + g_ref_dummy;
    if (g_replayed + refused + g_rep_no_enqueue != g_logged_rep) {
        g_logged_rep = g_replayed + refused + g_rep_no_enqueue;
        Log("[shots] replayed n=%u (muzzle %u, mag effect %u), late %u, at once %u, not enqueued %u, refused: weapon %u mag %u attack %u origin %u target %u "
            "filter %u category %u speed %u set full %u dummy unknown %u; received %u (dup %u, bad %u); faults %u",
            g_replayed, g_rep_muzzle, g_rep_mag, g_rep_late, g_rep_now, g_rep_no_enqueue, g_ref_weapon, g_ref_mag, g_ref_attack, g_ref_origin, g_ref_target,
            g_ref_filter, g_ref_category, g_ref_speed, g_ref_set_full, g_ref_dummy, g_rx, g_rx_dup, g_rx_bad, g_rep_faults + g_det_faults.load());
    }
    const unsigned hd = HitDropTotal() + g_hd_kept_test.load() + g_hd_bt.load() + g_kept_reuse.load();
    if (hd != g_logged_hd) {
        g_logged_hd = hd;
        char t[160];
        HitDropText(t, sizeof(t), g_hd_drop);
        Log("[shots] replay hits dropped at the bullet: %s (seen %u, our Sam passed %u, test let through %u, BT reactions dropped %u, faults %u; reused references "
            "kept on our Sam %u)",
            t, g_hd_seen.load(), g_hd_sam.load(), g_hd_kept_test.load(), g_hd_bt.load(), g_hd_faults.load(), g_kept_reuse.load());
    }
    const unsigned drops = g_drop_sam_ref.load() + g_drop_npc.load() + g_kept_ff.load() + g_kept_test.load();
    if (drops != g_logged_drop) {
        g_logged_drop = drops;
        Log("[shots] replay hits dropped: our Sam %u (replay set %u), enemies %u; kept %u (friendly fire %u, test %u); echo-guard skips %u",
            g_drop_sam_ref.load(), g_drop_sam_ref.load(), g_drop_npc.load(), g_kept_ff.load() + g_kept_test.load(), g_kept_ff.load(), g_kept_test.load(),
            g_echo_skips.load());
    }
}

void EnemyLogTick() {
    static double next = 0;
    static unsigned logged = 0;
    if (g_now < next) return;
    next = g_now + 5000;
    const unsigned sum = g_e_sent + g_e_rx + g_e_replayed + g_e_ref_shooter + g_e_ref_origin + g_kept_enemy.load() + g_drop_enemy.load();
    if (sum == logged) return;
    logged = sum;
    if (shotw::EnemyCaptureOn(g_enemy_role.load()) || g_e_sent)
        Log("[shots] enemy sent n=%u (shooters %u, pulls %u, frames %u, capped %u, no peer %u)", g_e_sent, g_shooters.Size(), g_e_pulls, g_e_frames, g_e_capped, g_e_no_peer);
    if (shotw::EnemyReplayOn(g_enemy_role.load()) || g_e_rx)
        Log("[shots] enemy replayed n=%u (muzzle %u, mag %u, from a not-holding copy's owner place %u), late dropped %u, refused: shooter %u origin %u category %u speed %u set full %u; received %u; "
            "replay hits on our Sam: enemy kept %u, dropped %u; first hits: shooter %u, other mirrors %u, partner body %u, our Sam %u, other %u (start %.2f m along)",
            g_e_replayed, g_e_muzzle, g_e_mag, g_e_nothold, g_e_late, g_e_ref_shooter, g_e_ref_origin, g_ref_category, g_ref_speed, g_ref_set_full, g_e_rx, g_kept_enemy.load(),
            g_drop_enemy.load(), g_eh[kEhShooter].load(), g_eh[kEhMirror].load(), g_eh[kEhBody].load(), g_eh[kEhSam].load(), g_eh[kEhOther].load(), g_enemy_ahead);
    if (g_oh_sam.load() || g_oh_body.load() || g_oh_other.load())
        Log("[shots] owned rounds' hits: our Sam %u, the partner body %u, other %u (the real rounds of the enemies this game owns, at the bullet)", g_oh_sam.load(),
            g_oh_body.load(), g_oh_other.load());
}

void RegistryLog() {
    if (g_reg_logged) return;
    const std::uintptr_t reg = Reg();
    std::int32_t nw = 0, nm = 0;
    if (!reg || !mem::Rd(reg + 0x10, nw) || !mem::Rd(reg + 0x20, nm) || nw <= 0) return;
    g_reg_logged = true;
    const std::uintptr_t dref = mem::Q(reg + kRegDummy);
    Log("[shots] registry: %d weapons, %d magazines; the bullet system's dummy owner %p", nw, nm, reinterpret_cast<void*>(dref ? dref - 0x20 : 0));
}

void TickBody(const TickIn& in, double now) {
    g_now = now;
    g_sam.store(in.local_ok ? reinterpret_cast<std::uintptr_t>(in.local) : 0, std::memory_order_relaxed);
    {
        const std::uintptr_t reg = Reg();
        const std::uintptr_t dref = reg ? mem::Q(reg + kRegDummy) : 0;
        const std::uintptr_t d = dref ? dref - 0x20 : 0;
        static std::uintptr_t logged = 0;
        static unsigned nlog = 0;
        if (d != logged && nlog < 4) ++nlog, logged = d, Log("[shots] the bullet system's dummy owner: %p (%s)", reinterpret_cast<void*>(d), d ? "the drop at the bullet's owner check" : "unknown: no replays while 0");
        const std::uintptr_t was = g_dummy.exchange(d, std::memory_order_relaxed);
        if (was != d) {
            // a load (a new bullet system): the old replays' references can be reused by the new world's bullets
            AcquireSRWLockExclusive(&g_rs_lock);
            g_rs.Clear();
            ReleaseSRWLockExclusive(&g_rs_lock);
            g_last_replay_ms.store(-1e18, std::memory_order_relaxed);
            // P1c: the attack registry read again for this world (a read that gave up before a load is tried anew; review
            // rv-correctness R8)
            if (g_attack_ok || g_attack_tries) {
                std::memset(g_attack_bits, 0, sizeof(g_attack_bits));
                g_attack_ok = false, g_attack_tries = 0, g_attack_next = 0;
            }
        }
    }
    RegistryLog();
    g_have_body_xf = in.body && ReadXf(reinterpret_cast<std::uintptr_t>(in.body), g_body_xf);
    g_body_ent.store(in.body ? reinterpret_cast<std::uintptr_t>(in.body) : 0, std::memory_order_relaxed);
    // a steered aim at enemy_look's enemy needs no partner body (P2-0, solo: her rifle at a MULE)
    if (g_aim.on && g_aim.steer && (g_have_body_xf || g_aim_override_until > g_now)) {
        const bool chest = g_have_body_xf && realbody::JointWorld(in.body, 4, g_body_chest) && g_body_chest[2] > g_body_xf.pos.z + 0.6 &&
                           g_body_chest[2] < g_body_xf.pos.z + 1.8;
        if (!chest && g_have_body_xf) g_body_chest[0] = g_body_xf.pos.x, g_body_chest[1] = g_body_xf.pos.y, g_body_chest[2] = g_body_xf.pos.z + 1.2;
        if (g_aim_override_until > g_now) {
            // enemy_look's enemy (hidden runs): a fixed point, or an entity followed live (a mirror walks while the aim settles:
            // run 20261124-114315's guard burst went over the mirror it had been aimed at 7 s before)
            WorldTransform ax{};
            if (g_aim_ent && ReadXf(g_aim_ent, ax)) g_aim_override[0] = ax.pos.x, g_aim_override[1] = ax.pos.y, g_aim_override[2] = ax.pos.z + g_aim_ent_up;
            std::memcpy(g_body_chest, g_aim_override, sizeof(g_body_chest));
        }
    }
    if (g_aim.on && in.local_ok) AimTick(in.local);
    BeaconTick(in);
    if (!g_on.load() && !g_enemy_role.load()) {
        AcquireSRWLockExclusive(&g_cap_lock);
        g_cap_count = 0;
        ReleaseSRWLockExclusive(&g_cap_lock);
        g_nhold = 0;
        g_nehold = 0;
        return;
    }
    ShooterTick(in);
    ReceiveTick(in);
    const unsigned done = g_on.load() ? ReplayTick(in) : (g_nhold = 0, 0u);
    EnemyReplayTick(done);
    EnemyTestTick();
    EnemyLogTick();
    if (!g_on.load()) return;
    LeakTick();
    TestTick();
    AimedTick();
    LogTick();
}
bool TickGuarded(const TickIn& in, double now) noexcept {
    __try {
        TickBody(in, now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

// ---------------------------------------------------------------- public
void Install(const Config& config) noexcept {
    g_cfg = config;
    // shots_sound: 1 (default) the magazine effect on every muzzle replay; auto only while sound_sync is off (the opt-in for the
    // two-game sound check: with sound_sync the partner's own gunshot plays only once this game learned that event, review
    // rv-behaviour 2)
    if (g_cfg.shots_sound < 0) g_cfg.shots_sound = config.sound_sync ? 0 : 1;
    g_enemy_want = config.enemy_sync && config.enemy_shots;
    g_attack_want = config.enemy_attack_registry;
    // enemy sync needs the hooks even without its shots: the frozen mirrors' own fire dropped in the Enqueue and their bullet hits
    // dropped at the bullet (the guard); it runs only with enemy_freeze=occupied and host_save=1 (enemy_sync.cpp Install)
    const bool esync_wanted = config.enemy_sync && config.enemy_freeze == 1 && config.host_save;
    if (!config.shots_sync && !esync_wanted) {
        std::snprintf(g_off, sizeof(g_off), "shots_sync=0");
        Log("[shots] off (shots_sync=0)");
        return;
    }
    // no partner can ever be in this session (no role, or the solo debug role): no hooks (hidden runs keep them: the echo)
    if ((config.role == Role::none || config.role == Role::solo) && !automation::Enabled()) {
        std::snprintf(g_off, sizeof(g_off), "no partner in this session (role %s)", RoleName(config.role));
        Log("[shots] off: %s", g_off);
        return;
    }
    unsigned n = 0, ok = 0, npc_bad = 0, hit_bad = 0, bt_bad = 0, cargo_bad = 0;
    const char* first_bad = nullptr;
    for (const Site& s : kSites) {
        ++n;
        if (mem::CodeMatches(A(s.va), s.sig)) {
            ++ok;
            continue;
        }
        if (s.group == Group::npc) ++npc_bad;
        else if (s.group == Group::hit) ++hit_bad;
        else if (s.group == Group::bt) ++bt_bad;
        else if (s.group == Group::cargo) ++cargo_bad;
        else if (!first_bad) first_bad = s.name;
        Log("[shots] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
    }
    // the globals (RIP targets of the checked instructions) and the five Enqueue callers
    n += 2;
    if (mem::RipTarget(A(kRegInsn), 3, 7) == A(kRegGlobal)) ++ok;
    else if (!first_bad) first_bad = "the registry global", Log("[shots] byte check FAILED: the registry global");
    if (mem::RipTarget(A(kSysInsn), 3, 7) == A(kSysGlobal)) ++ok;
    else if (!first_bad) first_bad = "the bullet system global", Log("[shots] byte check FAILED: the bullet system global");
    for (std::uintptr_t c : kEnqueueCallers) {
        ++n;
        std::uint8_t op = 0;
        if (mem::Rd(A(c), op) && op == 0xE8 && mem::RipTarget(A(c), 1, 5) == A(kEnqueue)) ++ok;
        else {
            if (!first_bad) first_bad = "an Enqueue caller";
            Log("[shots] byte check FAILED: the Enqueue call at 0x%llX", static_cast<unsigned long long>(c));
        }
    }
    // the RIP globals of the drop at the bullet's neighbours: the baggage manager (shots 6, hidden runs)
    n += 2;
    if (mem::RipTarget(A(kBagAddTemp) + 6, 3, 7) == A(kBagMgrGlobal)) ++ok;
    else ++cargo_bad, Log("[shots] byte check FAILED: the baggage manager global in AddBaggageToTemporaryList");
    if (mem::RipTarget(A(kBagPlaceBody) + 0x1D, 3, 7) == A(kBagMgrGlobal)) ++ok;
    else ++cargo_bad, Log("[shots] byte check FAILED: the baggage manager global in PlaceTemporaryListBaggagesAroundPlayer");
    {
        std::uint8_t op = 0;
        ++n;
        if (mem::Rd(A(kBagPlaceThunk), op) && op == 0xE9 && mem::RipTarget(A(kBagPlaceThunk), 1, 5) == A(kBagPlaceBody)) ++ok;
        else ++cargo_bad, Log("[shots] byte check FAILED: the PlaceTemporaryListBaggagesAroundPlayer export's jump");
    }
    g_checks_n = n, g_checks_pass = ok;
    g_checks_ok = first_bad == nullptr;
    g_npc_checks_ok = npc_bad == 0;
    g_hit_checks_ok = hit_bad == 0;
    g_bt_checks_ok = bt_bad == 0;
    g_cargo_checks_ok = cargo_bad == 0;
    Log("[shots] byte checks: %u/%u ok", ok, n);
    if (!g_checks_ok) {
        std::snprintf(g_off, sizeof(g_off), "this game build differs (%s)", first_bad);
        Log("[shots] OFF: %s", g_off);
        return;
    }
    void* t = reinterpret_cast<void*>(A(kEnqueue));
    MH_STATUS s = MH_CreateHook(t, reinterpret_cast<void*>(&H_Enqueue), reinterpret_cast<void**>(&o_enqueue));
    if (s == MH_OK) s = MH_EnableHook(t);
    g_hooked = s == MH_OK;
    if (!g_hooked) {
        std::snprintf(g_off, sizeof(g_off), "the bullet Enqueue not hooked (%s)", MH_StatusToString(s));
        Log("[shots] OFF: %s", g_off);
        return;
    }
    // the damage filters: our Sam (the shared detour) and the NPCs (shots_npc_drop)
    g_filter_registered = samdmg::Ensure("shots") && samdmg::Register(&SamFilter, "shots (replayed bullets on our Sam)");
    if (config.shots_npc_drop && g_npc_checks_ok) {
        // the shared Humanoid detour (npc_damage.hpp; round 16: enemy sync's filter shares it)
        g_npc_hooked = npcdmg::Ensure("shots") && npcdmg::Register(&NpcFilter, "shots (replayed bullets on NPCs: the backstop)");
        if (!g_npc_hooked) Log("[shots] the Humanoid damage filter is not in (%s)", npcdmg::OffReason());
    }
    // the drop at the bullet (shots_hit_drop): BulletDamageA / B, then the BT reaction (the hit processor and its call)
    if (config.shots_hit_drop && g_hit_checks_ok) {
        const struct {
            std::uintptr_t va;
            void* det;
            void** orig;
        } hk[2] = {{kBulletDmgA, reinterpret_cast<void*>(&H_BulletA), reinterpret_cast<void**>(&o_bullet_a)},
                   {kBulletDmgB, reinterpret_cast<void*>(&H_BulletB), reinterpret_cast<void**>(&o_bullet_b)}};
        MH_STATUS hs = MH_OK;
        int made = 0;
        for (const auto& h : hk) {
            hs = MH_CreateHook(reinterpret_cast<void*>(A(h.va)), h.det, h.orig);
            if (hs != MH_OK) break;
            ++made;
        }
        if (hs == MH_OK) {
            for (const auto& h : hk) {
                hs = MH_EnableHook(reinterpret_cast<void*>(A(h.va)));
                if (hs != MH_OK) break;
            }
        }
        g_hit_hooked = hs == MH_OK;
        if (!g_hit_hooked) {
            for (int i = 0; i < made; ++i) MH_RemoveHook(reinterpret_cast<void*>(A(hk[i].va)));
            Log("[shots] the drop at the bullet not hooked (%s)", MH_StatusToString(hs));
        }
        if (g_hit_hooked && g_bt_checks_ok && config.shots_bt_drop) {
            MH_STATUS bs = MH_CreateHook(reinterpret_cast<void*>(A(kBtReact)), reinterpret_cast<void*>(&H_BtReact), reinterpret_cast<void**>(&o_bt));
            if (bs == MH_OK) bs = MH_CreateHook(reinterpret_cast<void*>(A(kHitProc)), reinterpret_cast<void*>(&H_HitProc), reinterpret_cast<void**>(&o_hitproc));
            if (bs == MH_OK) bs = MH_EnableHook(reinterpret_cast<void*>(A(kBtReact)));
            if (bs == MH_OK) bs = MH_EnableHook(reinterpret_cast<void*>(A(kHitProc)));
            g_bt_hooked = bs == MH_OK;
            if (!g_bt_hooked) {
                MH_RemoveHook(reinterpret_cast<void*>(A(kHitProc)));
                MH_RemoveHook(reinterpret_cast<void*>(A(kBtReact)));
                Log("[shots] the BT reaction's drop not hooked (%s): a replay that hits a BT still makes it react", MH_StatusToString(bs));
            }
        } else if (g_hit_hooked && !config.shots_bt_drop) {
            Log("[shots] the BT reaction's drop off (shots_bt_drop=0, not yet run against a BT): a replay that hits a BT may make it react");
        } else if (g_hit_hooked) {
            Log("[shots] the BT reaction's byte checks failed: a replay that hits a BT still makes it react (the drop at the bullet is on)");
        }
    }
    if (config.shots_hit_drop && !g_hit_hooked) {
        // a replay could damage this world's vehicles, cargo and structures: replays stay off (only shots_hit_drop=0 allows them)
        Log("[shots] OFF for replays: the drop at the bullet is not installed (%s): the partner's shots are not replayed here (ours are still sent)",
            g_hit_checks_ok ? "the hook failed" : "its byte checks failed");
    }
    if (config.shots_npc_drop && !g_npc_hooked) {
        // a replay could hurt this world's MULEs: replays stay off (only shots_npc_drop=0 allows them); the capture and the sends stay on
        Log("[shots] OFF for replays: the NPC drop is not installed (%s): the partner's shots are not replayed here (ours are still sent)",
            g_npc_checks_ok ? "the hook failed" : "the Humanoid handler's byte check failed");
    }
    if (!g_filter_registered) {
        // without the filter a replay could hurt our Sam: replays stay off, the capture and the sends stay on
        Log("[shots] the Sam damage filter is off (%s): the partner's shots are not replayed here (ours are still sent)", samdmg::OffReason());
    }
    const bool on = config.shots_sync;
    g_on.store(on);
    if (on) g_off[0] = 0;
    else std::snprintf(g_off, sizeof(g_off), "shots_sync=0 (the hooks are in for enemy sync)");
    Log("[shots] %s: Enqueue hooked, Sam filter %s, NPC drop %s, drop at the bullet %s (BT reaction %s); muzzle %d, magazine effect %d, friendly fire %d",
        on ? "ON" : "off (enemy sync only)", g_filter_registered ? "on" : "OFF", g_npc_hooked ? "on" : config.shots_npc_drop ? "OFF (checks)" : "off (shots_npc_drop=0)",
        g_hit_hooked ? "on" : config.shots_hit_drop ? "OFF" : "off (shots_hit_drop=0)", g_bt_hooked ? "on" : "off", config.shots_muzzle ? 1 : 0,
        g_cfg.shots_sound ? 1 : 0, config.shots_friendly_fire ? 1 : 0);
}

bool Active() noexcept { return g_on.load(); }
const char* OffReason() noexcept {
    if (g_on.load() || !g_cfg.shots_sync) return "";
    return g_off;
}
bool EnqueueHooked() noexcept { return g_hooked; }

void Tick(const TickIn& in, double now) noexcept {
    LogFlushLater();  // the detours' lines (worker threads only format them)
    if (!g_hooked || g_stopped.load()) return;
    if (!TickGuarded(in, now)) Stop("the shots tick");
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    switch (op) {
        case 3: {
            // hidden runs: our Sam v[1] m (default 7) from the partner body, v[2] deg around its heading (0 in front of it, 90 to its
            // right), facing it: the stills frame the partner's gun
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            if (!g_have_body_xf) return std::snprintf(msg, cap, "no partner body"), false;
            const double d = v[1] > 0 ? v[1] : 7.0, a = v[2] * 3.14159265358979323846 / 180.0;
            const double fx = g_body_xf.rot.m[1][0], fy = g_body_xf.rot.m[1][1], rx = g_body_xf.rot.m[0][0], ry = g_body_xf.rot.m[0][1];
            const double dx = fx * std::cos(a) + rx * std::sin(a), dy = fy * std::cos(a) + ry * std::sin(a);
            const double l = std::sqrt(dx * dx + dy * dy) > 1e-6 ? std::sqrt(dx * dx + dy * dy) : 1.0;
            const double x = g_body_xf.pos.x + dx / l * d, y = g_body_xf.pos.y + dy / l * d;
            char why[160] = {};
            // the topmost ground there (the place's short probe kept a slope's z and dropped Sam through it: run 20261123-205419)
            const bool ok = game::TeleportLocalPlayerFacing(x, y, game::kSnapRayCentreZ, -dx / l, -dy / l, game::TeleportMode::snap_to_ground, why, sizeof(why));
            Log("[shots] test: our Sam %.1f m from the partner body at %.0f deg, facing it: %s", d, v[2], why);
            return std::snprintf(msg, cap, "%s", why), ok;
        }
        case 0: {
            char line[1400];
            StatusLine(line, sizeof(line));
            Log("[shots] status: %s", line);
            std::snprintf(msg, cap, "%s", line);
            return true;
        }
        case 4:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            if (!g_have_body_xf && !((v[2] == 1 || v[2] == 2) && g_aim_override_until > g_now))
                return std::snprintf(msg, cap, "no partner body (and no enemy_look target for a steered aim)"), false;
            g_aim.tol = v[1] >= 1 && v[1] <= 45 ? v[1] : 12;
            g_aim.steer = v[2] == 1 || v[2] == 2;
            g_aim.fire = v[2] == 2;  // 2: then fires 2 s while the aim follows (the enemy-mirror guard step)
            g_aim.fire_until = 0;
            g_aim.start = g_aim.win_t = g_now, g_aim.settle_until = 0;
            g_aim.yaw = AimAxis{}, g_aim.pitch = AimAxis{};
            g_aim.on = true;
            Log("[shots] camera aim at the partner body (within %.0f deg%s)", g_aim.tol,
                g_aim.steer ? (g_aim.fire ? "; steered: LT held, the right stick driven here, yaw and pitch at its chest; then RT 2 s" : "; steered: LT held, the right stick driven here, yaw and pitch at its chest") : "");
            return std::snprintf(msg, cap, "aiming"), true;
        case 1:
            if (!g_hooked || g_stopped.load()) return std::snprintf(msg, cap, "not possible: %s", g_off), false;
            g_on.store(true);
            g_off[0] = 0;
            Log("[shots] on (debug action)");
            return std::snprintf(msg, cap, "shots on"), true;
        case 2:
            g_on.store(false);
            std::snprintf(g_off, sizeof(g_off), "off (debug action)");
            Log("[shots] off (debug action)");
            return std::snprintf(msg, cap, "shots off"), true;
        case 5:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;  // a negative count lets damage through
            if (!g_on.load()) return std::snprintf(msg, cap, "shots are off: %s", g_off), false;
            if (!CanReplay()) return std::snprintf(msg, cap, "no replays here: %s", CannotReplayWhy()), false;
            return AimedStart(static_cast<int>(v[1]), static_cast<int>(v[2]), msg, cap);
        case 6:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            return DropCarried(msg, cap);
        case 8:
            // hidden runs: the muzzle (weapon id) and the magazine effect (magazine id) of the next replays, live (the sound check)
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            g_cfg.shots_muzzle = v[1] != 0, g_cfg.shots_sound = v[2] != 0 ? 1 : 0;
            Log("[shots] test: replays now with muzzle %d, magazine effect %d", g_cfg.shots_muzzle ? 1 : 0, g_cfg.shots_sound ? 1 : 0);
            return std::snprintf(msg, cap, "muzzle %d, magazine effect %d", g_cfg.shots_muzzle ? 1 : 0, g_cfg.shots_sound ? 1 : 0), true;
        case 9:
            // hidden runs: the steered aim (shots 4 <deg> 1) points the camera v[1] deg left of the body (it then shows right of the
            // centre, clear of our own body) and v[2] deg below it
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            g_aim_off_yaw = v[1], g_aim_off_pitch = v[2];
            Log("[shots] camera aim offset: yaw %.1f, pitch %.1f deg", g_aim_off_yaw, g_aim_off_pitch);
            return std::snprintf(msg, cap, "aim offset %.1f %.1f", g_aim_off_yaw, g_aim_off_pitch), true;
        case 10:
            // hidden runs: the enemy replays start v[1] m (0 to 3) along the shot from the host's muzzle place (a test of a start past
            // the shooting mirror's own body; 0, the default: at the muzzle place)
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            g_enemy_ahead = v[1] >= 0 && v[1] <= 3 ? v[1] : 0;
            Log("[shots] test: enemy replays start %.2f m along the shot", g_enemy_ahead);
            return std::snprintf(msg, cap, "enemy replays start %.2f m along", g_enemy_ahead), true;
        default:
            return std::snprintf(msg, cap,
                                 "unknown op %d (0 status, 1 on, 2 off, 3 m deg: beside the partner, 4 [deg] aim the camera at it, 5 target n: aimed test, 6 drop a "
                                 "carried piece, 8 muzzle sound, 9 yaw pitch: aim offset, 10 m: enemy replays' start along the shot)",
                                 op),
                   false;
    }
}

bool Test(double dist_m, int flags, int count, char* msg, std::size_t cap) noexcept {
    if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;  // flag 1 hurts our own Sam on purpose
    if (!g_on.load()) return std::snprintf(msg, cap, "shots are off: %s", g_off), false;
    if (!CanReplay()) return std::snprintf(msg, cap, "no replays here: %s", CannotReplayWhy()), false;
    if (!g_have_last) return std::snprintf(msg, cap, "no captured shot yet (fire once first: the test takes its ids)"), false;
    if (flags & 16) {
        g_leak_left = count > 0 ? (count > 400 ? 400 : count) : 200;
        g_leak_done = 0;
        g_leak_ws0 = WorkingSet();
        Log("[shots] leak test: %d replays from 10 m ahead into the ground (4 a tick); working set %.1f MB", g_leak_left, g_leak_ws0 / 1048576.0);
        return std::snprintf(msg, cap, "leak test: %d replays", g_leak_left), true;
    }
    double o[3], t[3];
    if (!TestOrigin(dist_m > 0.5 ? dist_m : 5.0, 1.2, o, t, false)) return std::snprintf(msg, cap, "no local Sam"), false;
    const shotw::Shot s = TestShot(o, t, flags);
    const shotw::IdCheck wc = WeaponCheck(s.weapon_id), mc = MagCheck(s.magazine_id);
    g_test_ref_match.store(0), g_test_dummy_match.store(0), g_test_how.store(-1), g_test_slot.store(-1), g_test_dropped.store(0);
    g_test_nofilter.store((flags & 1) != 0);
    g_test_armed.store(true);
    const bool enq = ReplayShot(s, o, 3, true);
    g_test_t0 = g_now;
    g_test_wait = true;
    std::uint8_t a = 0, b = 0, c = 0, d0 = 0, d1 = 0;
    std::uint16_t atk = 0;
    if (t_have_slot) a = t_slot_rec[0x04], b = t_slot_rec[0x05], c = t_slot_rec[0x5A], d0 = t_slot_rec[0xD0], d1 = t_slot_rec[0xD1], std::memcpy(&atk, t_slot_rec + 0x5C, 2);
    Log("[shots] test: CreateBullet from %.1f m ahead toward our Sam's chest: attack 0x%X weapon %u (%s%s) mag %u (%s), speed %.0f m/s%s: %s; R %llX %llX %llX %llX; "
        "the slot's record: +04 %u +05 %u +5A %u +5C 0x%X +D0 0x%02X +D1 0x%02X (wanted +05 %u +5A %u +D0 0x%02X)",
        dist_m, s.attack_id, s.weapon_id, wc.listed ? "found" : "NOT FOUND", wc.listed && !wc.child_ok ? ", no child list" : "", s.magazine_id,
        mc.listed ? "found" : "NOT FOUND", static_cast<double>(s.speed), (flags & 1) ? " (filter keeps it)" : "", enq ? "enqueued" : "NOT enqueued",
        static_cast<unsigned long long>(t_nrefs > 0 ? t_refs[0] : 0), static_cast<unsigned long long>(t_nrefs > 1 ? t_refs[1] : 0),
        static_cast<unsigned long long>(t_nrefs > 2 ? t_refs[2] : 0), static_cast<unsigned long long>(t_nrefs > 3 ? t_refs[3] : 0), a, b, c, atk, d0, d1, s.rec05,
        s.rec5A, s.recD0);
    return std::snprintf(msg, cap, "test replay %s (attack 0x%X, weapon %u, mag %u)", enq ? "enqueued" : "not enqueued", s.attack_id, s.weapon_id, s.magazine_id), enq;
}

bool Echo(bool on, double offset_m, char* msg, std::size_t cap) noexcept {
    if (on && !automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
    if (on && !CanReplay()) return std::snprintf(msg, cap, "no replays here: %s", CannotReplayWhy()), false;
    if (on && !g_on.load()) return std::snprintf(msg, cap, "shots are off: %s", g_off), false;
    g_echo = on;
    if (offset_m != 0) g_echo_off = offset_m;
    Log("[shots] echo %s (our own shots replayed %.1f m to the right)", on ? "on" : "off", g_echo_off);
    return std::snprintf(msg, cap, "echo %s, %.1f m", on ? "on" : "off", g_echo_off), true;
}


// ---------------------------------------------------------------- enemy sync's side (round 16, docs/enemy-sync.md 8)
void SetEnemyBeacon(std::uint8_t parts, std::uint16_t count, std::uint32_t ids_fp, std::uint32_t guids_fp, std::uint8_t freeze) noexcept {
    g_enemy_parts = parts, g_enemy_count = count, g_enemy_ids = ids_fp, g_enemy_guids = guids_fp, g_enemy_freeze = freeze;
}

bool PeerBeacon(shotw::Beacon* out, double* age_ms) noexcept {
    if (!g_have_peer) return false;
    if (out) *out = g_peer_beacon;
    if (age_ms) *age_ms = g_now - g_peer_ms;
    return true;
}

void SetEnemyShots(int mask) noexcept {
    const int r = g_enemy_want && g_hooked && !g_stopped.load() ? (mask & 3) : 0;
    const int was = g_enemy_role.exchange(r);
    if (was != r) {
        Log("[shots] enemy shots %s", r == 3 ? "on (capture + replay: the owned shooters' records captured and sent, the other game's replayed from the mirrors)"
                                      : r == 1 ? "on (host: the shooters' records captured and sent)"
                                      : r == 2 ? "on (guest: replayed from the mirrors)"
                                               : "off");
        if (!shotw::EnemyCaptureOn(r)) g_shooters.Clear();
        if (!shotw::EnemyReplayOn(r)) g_nehold = 0;
    }
}

void SetEnemyShooters(const std::uint64_t* entities, const std::uint32_t* ids, unsigned n) noexcept { g_shooters.Publish(entities, ids, n); }
unsigned EnemyShooterCount() noexcept { return g_shooters.Size(); }

std::uint32_t LastEnemyShooter() noexcept { return g_last_enemy_shooter; }

void SetAimTarget(const double (&p)[3], double for_ms) noexcept {
    std::memcpy(g_aim_override, p, sizeof(g_aim_override));
    g_aim_ent = 0;
    g_aim_override_until = g_now + for_ms;
}

void SetAimEntity(std::uintptr_t entity, double up_m, double for_ms) noexcept {
    WorldTransform x{};
    if (!entity || !ReadXf(entity, x)) return;
    g_aim_override[0] = x.pos.x, g_aim_override[1] = x.pos.y, g_aim_override[2] = x.pos.z + up_m;
    g_aim_ent = entity, g_aim_ent_up = up_m;
    g_aim_override_until = g_now + for_ms;
}

bool MirrorGuardReady() noexcept { return g_hooked && g_hit_hooked && !g_stopped.load(); }
bool EnemyReplayReady() noexcept { return g_hooked && CanReplay() && !g_stopped.load(); }
unsigned MirrorBulletDrops() noexcept { return g_hd_mirror.load(); }
void SetHitsPart(bool on, const char* why_off) noexcept {
    g_hits_why = why_off ? why_off : "";
    g_hits_part.store(on, std::memory_order_relaxed);
}
void SetRestrainPart(bool on) noexcept { g_restrain_part.store(on, std::memory_order_relaxed); }
bool PeerRestrain() noexcept { return g_have_peer && (g_peer_parts & shotw::kPartRestrain) != 0; }
std::uint8_t SetPlayerDown(std::uint8_t code) noexcept {
    g_player_down = code <= shotw::kBeaconDownMax ? code : std::uint8_t{0};
    return g_peer_down_cap ? g_player_down : std::uint8_t{0};
}
unsigned FlushEnemyReplays(std::uint32_t shooter, const char* why, bool quiet_if_none) noexcept {
    if (!shooter) return 0;
    unsigned keep = 0, n = 0;
    for (unsigned i = 0; i < g_nehold; ++i) {
        if (g_ehold[i].s.shooter == shooter) {
            ++n;
            continue;
        }
        if (keep != i) g_ehold[keep] = g_ehold[i];
        ++keep;
    }
    g_nehold = keep;
    g_flushed += n;
    if (!n && quiet_if_none) return 0;
    Log("[shots] flushed %u replays of id=%05X (down: %s; %u in all)", n, shooter, why ? why : "?", g_flushed);
    return n;
}
unsigned MirrorBulletPassed() noexcept { return g_hd_mirror_pass.load(); }
unsigned MirrorBulletDropsOurs() noexcept { return g_hd_mirror_ours.load(); }
unsigned MirrorBulletDropsReplays() noexcept { return g_hd_mirror_rep.load(); }

void MirrorShotCounts(unsigned* seen, unsigned* dropped) noexcept {
    if (seen) *seen = g_mirror_seen.load();
    if (dropped) *dropped = g_mirror_dropped.load();
}

bool IsReplayHit(const samdmg::View& v) noexcept {
    std::uint8_t kind = 0;
    int how = -1, slot = -1;
    return IsReplayHit(v, &kind, &how, &slot);
}

void EnemyShotStatus(char* out, std::size_t cap) noexcept {
    std::snprintf(out, cap,
                  "enemy shots %s: sent %u (pulls %u, frames %u, capped %u, no peer %u), received %u, replayed %u (muzzle %u, mag %u, late dropped %u), refused shooter %u "
                  "origin %u off %u; enemy rounds on our Sam kept %u dropped %u; first hits: shooter %u, other mirrors %u, partner body %u, our Sam %u, other %u",
                  g_enemy_role.load() == 3 ? "capture + replay" : g_enemy_role.load() == 1 ? "host" : g_enemy_role.load() == 2 ? "guest" : "off", g_e_sent, g_e_pulls, g_e_frames, g_e_capped, g_e_no_peer, g_e_rx,
                  g_e_replayed, g_e_muzzle, g_e_mag, g_e_late, g_e_ref_shooter, g_e_ref_origin, g_e_ref_off, g_kept_enemy.load(), g_drop_enemy.load(),
                  g_eh[kEhShooter].load(), g_eh[kEhMirror].load(), g_eh[kEhBody].load(), g_eh[kEhSam].load(), g_eh[kEhOther].load());
}

bool EnemyShotTest(std::uintptr_t mirror, std::uint32_t id, bool nohurt, char* msg, std::size_t cap) noexcept {
    if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
    if (!CanReplay()) return std::snprintf(msg, cap, "no replays here: %s", CannotReplayWhy()), false;
    if (g_et_wait) return std::snprintf(msg, cap, "a shot test is running"), false;
    WorldTransform sx{}, mx{};
    if (!ReadXf(g_sam.load(), sx) || !ReadXf(mirror, mx)) return std::snprintf(msg, cap, "unreadable"), false;
    double hand[3];
    if (!realbody::JointWorld(reinterpret_cast<game::Entity*>(mirror), kHandJoint, hand)) hand[0] = mx.pos.x, hand[1] = mx.pos.y, hand[2] = mx.pos.z + 1.3;
    const double target[3] = {sx.pos.x, sx.pos.y, sx.pos.z + 1.2};
    const double d3[3] = {target[0] - hand[0], target[1] - hand[1], target[2] - hand[2]};
    const double l = std::sqrt(d3[0] * d3[0] + d3[1] * d3[1] + d3[2] * d3[2]);
    if (l < 1.5) return std::snprintf(msg, cap, "too close"), false;
    // level, toward her from the mirror's side
    const double hl = std::sqrt(d3[0] * d3[0] + d3[1] * d3[1]);
    if (hl < 0.5) return std::snprintf(msg, cap, "the mirror stands over her"), false;
    const double fx = d3[0] / hl, fy = d3[1] / hl;
    // the 3 rounds level at her chest from 2 m: in front of her on the line to the mirror's hand, and 50 deg to either side of it
    // (runs 20261124-102419 / -122942: from the hand itself a truck between them took all three rounds, and from 3 m along a
    // sloped line all three were lost, likely in the camp's sandbag wall): the damage policy is the test, not the line of fire
    // the last received enemy round when it is a fast one (a rifle's), else EnemyAssaultRifle (0x56) at 300 m/s: run
    // 20261124-102043's last round was a 70 m/s one (attack 0x57) that never reached her Sam from 20 m
    shotw::Shot s;
    if (g_have_enemy_rx && g_last_enemy_rx.speed >= 150.0f) {
        s = g_last_enemy_rx;
    } else {
        s.attack_id = 0x56;  // EnemyAssaultRifle
        s.speed = 300.0f;
    }
    s.flags = shotw::kMuzzle;
    s.shooter = id;
    g_et_match.store(0), g_et_kept.store(-1), g_et_amount.store(0);
    g_et_nohurt.store(nohurt);
    g_et_armed.store(true);
    g_et_h0 = Health();
    g_et_id = id;
    bool enq = false;
    for (int i = 0; i < 3; ++i) {  // 3 rounds: one must reach her
        const double a = (i == 0 ? 0.0 : i == 1 ? 50.0 : -50.0) * 3.14159265358979323846 / 180.0;
        const double dx = fx * std::cos(a) - fy * std::sin(a), dy = fx * std::sin(a) + fy * std::cos(a);
        double origin[3] = {target[0] - dx * 2.0, target[1] - dy * 2.0, target[2]};
        for (int k = 0; k < 3; ++k) s.origin[k] = origin[k];
        s.dir[0] = static_cast<float>(dx), s.dir[1] = static_cast<float>(dy), s.dir[2] = 0.0f;
        enq = ReplayShot(s, origin, shotw::kReplayKindEnemy, i == 0, true) || enq;
    }
    g_et_t0 = g_now;
    g_et_wait = true;
    Log("[enemy] shot test%s: 3 enemy replays level at our Sam's chest from 2 m (toward mirror %05X's hand and 50 deg either side; %.1f m away; attack 0x%X weapon %u mag %u speed %.0f, %s): %s; health %.1f",
        nohurt ? " (nohurt)" : "", id, l, s.attack_id, s.weapon_id, s.magazine_id, static_cast<double>(s.speed), g_have_enemy_rx ? "the last received enemy shot's ids" : "attack 0x56",
        enq ? "enqueued" : "NOT enqueued", static_cast<double>(g_et_h0));
    return std::snprintf(msg, cap, "enemy shot test %s", enq ? "enqueued" : "not enqueued"), enq;
}

void ArmOwnerCounts(bool on) noexcept {
    AcquireSRWLockExclusive(&g_cap_lock);
    for (auto& o : g_own) o = OwnCount{};
    ReleaseSRWLockExclusive(&g_cap_lock);
    g_own_armed.store(on);
}

unsigned ShotsBy(std::uintptr_t entity) noexcept {
    unsigned n = 0;
    AcquireSRWLockExclusive(&g_cap_lock);
    for (const auto& o : g_own)
        if (o.e == entity) n = o.n;
    ReleaseSRWLockExclusive(&g_cap_lock);
    return n;
}

bool RoundOf(std::uintptr_t entity, unsigned n, double (&origin)[3], double (&dir)[3]) noexcept {
    bool ok = false;
    OwnRound r{};
    AcquireSRWLockExclusive(&g_cap_lock);
    for (const auto& o : g_own)
        if (o.e == entity && n >= 1 && n <= o.n && o.n - n < kOwnRing) r = o.ring[(n - 1) % kOwnRing], ok = true;
    ReleaseSRWLockExclusive(&g_cap_lock);
    if (!ok) return false;
    const double sp = std::sqrt(static_cast<double>(r.v[0]) * r.v[0] + static_cast<double>(r.v[1]) * r.v[1] + static_cast<double>(r.v[2]) * r.v[2]);
    if (!(sp > 0.1) || !std::isfinite(sp)) return false;
    for (int k = 0; k < 3; ++k) origin[k] = r.o[k], dir[k] = r.v[k] / sp;
    return std::isfinite(origin[0]) && std::isfinite(origin[1]) && std::isfinite(origin[2]);
}

}  // namespace st::shots
