// Vehicle hits (design M4). See partner_tackle.hpp, tackle_rules.hpp and docs/real-partner-body.md section 25.
#include "partner_tackle.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "automation.hpp"
#include "coop_session.hpp"
#include "game_actions.hpp"
#include "link.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "ride_camera.hpp"
#include "ride_together.hpp"
#include "sam_damage.hpp"
#include "tackle_rules.hpp"
#include "vehicle_drive.hpp"
#include "vehicle_share.hpp"
#include "vehicle_wire.hpp"

namespace st::vhit {
namespace {
namespace mem = partner::mem;
using game::WorldTransform;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut)
constexpr std::uintptr_t kTackleEmit = 0x142F8ABF0;    // VehicleTackleEmit(vehicle): the vehicle's attack, every frame from its update
constexpr std::uintptr_t kTackleGates = 0x142F8AC10;   // its gates: +0x560 & 0x10, [global]+8 bit 6, driver / state 4, +0x560 bit 8, speed > 15
constexpr std::uintptr_t kTackleGlobalInsn = 0x142F8AC1F;  // mov rax, [rip+..]: the global whose +8 bit 6 stops every vehicle attack
constexpr std::uintptr_t kTackleChoice = 0x142F8ACB9;  // the attack: NPC driver -> +0x9E0 (local player within 50 m), else +0xA00
constexpr std::uintptr_t kTackleSlots = 0x142F8AD61;   // mov edi, 0x9E0 / mov edi, 0xA00
constexpr std::uintptr_t kTackleFilter = 0x142F8AC94;  // the filter on its stack: lea r15, [vtable]; 2; 0xFF; zeros
constexpr std::uintptr_t kTackleFilterInit = 0x142F8AD82;  // mov r8d, 0x36 (layer 54) ... call 0x14382FD30
constexpr std::uintptr_t kTackleFilterCall = 0x142F8ADA6;  // call 0x14382FD30; ...; filter+0xC = 1
constexpr std::uintptr_t kTackleAmount = 0x142F8AE19;  // amount 100 / 10000 by the speed field; the sphere's local offset sign
constexpr std::uintptr_t kTackleCentre = 0x142F8AE85;  // vmulps xmm0, xmm2, [rbx+0xA70]: the sphere's local offset
constexpr std::uintptr_t kTackleRadius = 0x142F8AEB0;  // vmovss xmm2, [rbx+0x57C]: the sphere's radius
constexpr std::uintptr_t kTackleArgs = 0x142F8AEBB;    // the wrapper's stack arguments: 0x20, 1.0f, slot - 0x20, 0, &zero, &filter
constexpr std::uintptr_t kTackleFree = 0x142F8AFFF;    // after the call: the filter's list freed with 0x1419D5870
constexpr std::uintptr_t kHitWrapper = 0x142993440;    // the wrapper: ([ctx+0x38] - 0x20, centre, radius, amount, filter, callback 0, ...)
constexpr std::uintptr_t kHitQuery = 0x1429934E0;      // the attack sphere query: overlap, one DamageInfo per hit owner, callback, delivery
constexpr std::uintptr_t kHitCallbackSite = 0x1429938B2;  // its callback call: cb(ctx, receiver, &DamageInfo, user), delivered when true
constexpr std::uintptr_t kHitPosition = 0x14231881F;   // DamageInfo+0x80: the hit position (three doubles)
constexpr std::uintptr_t kDamageInfoInit = 0x142314242;  // DamageInfo+0x78 the amount, +0x28 the attack reference
constexpr std::uintptr_t kFilterInit = 0x14382FD30;    // filter init (filter, entity, layer): the entity's own bodies left out
constexpr std::uintptr_t kFree = 0x1419D5870;          // the game's free (the filter's list)
constexpr std::uintptr_t kFilterVt = 0x143BA8938;      // the filter's vtable
constexpr std::uintptr_t kSlotMule = 0x142F7500D;      // the truck's add-to-world: VehicleTackle 0x157 into +0x9E0
constexpr std::uintptr_t kSlotPlayer = 0x142F751E3;    // every vehicle's add-to-world: VehicleTacklePlayer 0x158 into +0xA00
constexpr std::uintptr_t kSamDamage = 0x14271BC00;     // DSPlayerEntity's MsgDamage handler (detoured: counts, caps)

constexpr std::uintptr_t kVFlags560 = 0x560, kVDriver = 0x8B0, kVRideState = 0x4CC, kVHavok = 0x330, kVSpeedField = 0xF0;
constexpr std::uintptr_t kVSignedSpeed = 0x580, kVRadius = 0x57C, kVSphereLocal = 0xA70;
constexpr std::uintptr_t kVSlotMule = 0x9E0, kVSlotPlayer = 0xA00, kVSlotPrelim = 0xA20;
constexpr std::uint32_t kNoActor = 0x1FFFFF;
constexpr std::uintptr_t kEParent = 0x70;

enum Group : unsigned { kGCommon = 1u, kGDirect = 2u, kGBracket = 4u, kGHook = 8u };
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    {kTackleEmit,       "40 55 53 48 8D 6C 24 88 48 81 EC 78 01 00 00 48 8B 05 2A 64 FD 01 48 33 C4 48 89 45 20 48 8B D9", "VehicleTackleEmit", kGCommon},
    {kTackleGates,      "8B 89 60 05 00 00 F6 C1 10 0F 85 31 04 00 00 48 8B 05 62 C2 C6 04 0F B6 50 08 C0 EA 06 F6 C2 01 0F 85 1A 04 00 00 "
                        "8B 93 B0 08 00 00 81 FA FF FF 1F 00 75 0D 83 BB CC 04 00 00 04 0F 85 FF 03 00 00 C1 E9 08 F6 C1 01 0F 85 F3 03 00 00 "
                        "48 8B 83 30 03 00 00 C5 FA 10 80 F0 00 00 00 C5 F8 2F 05 E0 12 C4 00 0F 86 D6 03 00 00",
                        "the attack's gates (flags, global, driver / state 4, speed > 15)", kGCommon},
    {kTackleChoice,     "81 FA FF FF 1F 00 74 0C F7 C2 00 E0 1F 00 0F 84 9B 00 00 00 83 BB CC 04 00 00 04 0F 84 8E 00 00 00",
                        "the attack choice (NPC driver -> MULE slot, state 4 -> player slot)", kGBracket | kGDirect},
    {kTackleSlots,      "BF E0 09 00 00 EB 05 BF 00 0A 00 00", "the attack slots +0x9E0 / +0xA00", kGBracket | kGDirect},
    {kSlotPlayer,       "BA 58 01 00 00 48 8B CF E8 A0 5A A1 FF 48 8D 8F 00 0A 00 00 48 8B 90 E8 00 00 00 E8 0D E2 4E FF",
                        "VehicleTacklePlayer 0x158 into +0xA00 (every vehicle)", kGBracket | kGDirect},
    {kSlotMule,         "BA 57 01 00 00 48 8B CF E8 76 5C A1 FF 48 8D 8F E0 09 00 00 48 8B 90 E8 00 00 00 E8 E3 E3 4E FF",
                        "VehicleTackle 0x157 into +0x9E0 (trucks)", kGDirect},
    {kTackleFilter,     "4C 8D 3D 9D DC C1 00 4C 89 7D B0 C7 45 B8 02 00 00 00 48 C7 45 BC FF 00 00 00 48 89 75 C4 89 75 CC 48 89 75 D0",
                        "the attack's filter (vtable, 2, 0xFF, zeros)", kGDirect},
    {kTackleFilterInit, "41 B8 36 00 00 00", "the filter's layer 54", kGDirect},
    {kTackleFilterCall, "E8 85 4F 8A 00 8B 93 B0 08 00 00 41 BE 38 00 00 00 C7 45 BC 01 00 00 00", "the filter init call, filter+0xC = 1", kGDirect},
    {kTackleAmount,     "48 8B 83 30 03 00 00 48 8D 54 24 58 C5 FA 10 05 5B E0 BF 00 C5 FA 10 0D 3F 82 C0 00 48 8B 8B 40 03 00 00 "
                        "C5 FA C2 90 F0 00 00 00 01 C5 FA 10 83 80 05 00 00",
                        "the amount by speed, the sphere's direction by +0x580", kGDirect},
    {kTackleCentre,     "C5 E8 59 83 70 0A 00 00", "the sphere's local offset +0xA70", kGDirect},
    {kTackleRadius,     "C5 FA 10 93 7C 05 00 00", "the sphere's radius +0x57C", kGDirect},
    {kTackleArgs,       "C7 44 24 40 20 00 00 00 C5 FA 11 7C 24 38 48 8D 48 E0 89 74 24 30 48 8D 45 A0 48 0F 44 CE 48 89 44 24 28 48 8D 45 B0",
                        "the hit call's arguments", kGDirect},
    {kTackleFree,       "48 8B 4D D0 4C 89 7D B0 E8 64 A8 A4 FE", "the filter's list freed", kGDirect},
    {kHitWrapper,       "4C 8B DC 48 81 EC 98 00 00 00 4C 8B 41 38 45 33 C9 48 8B 84 24 C8 00 00 00 4D 85 C0 45 89 4B D0 4D 89 4B C8 49 8D 48 E0 "
                        "C5 F8 10 00 8B 84 24 E0 00 00 00 49 0F 44 C9 89 44 24 58 8B 84 24 D0 00 00 00 C5 F9 7F 44 24 70 C5 FA 10 84 24 D8 00 00 00 "
                        "C5 FA 11 44 24 50 89 44 24 48 49 8D 43 D8 49 89 43 A8 49 8D 43 E8 49 89 43 A0 48 8B 84 24 C0 00 00 00 4D 89 4B 98 "
                        "C5 F0 57 C9 4D 89 4B 90 49 89 43 88 C4 C1 78 11 4B E8 E8 12 00 00 00",
                        "the hit wrapper (the query's argument layout)", kGDirect},
    {kHitQuery,         "40 55 53 56 57 41 56 41 57 48 8D AC 24 C8 F4 FF FF 48 81 EC 38 0C 00 00", "the attack sphere query", kGDirect},
    {kHitCallbackSite,  "48 8B 45 88 48 85 C0 74 2A 4C 8B 4D 90 4C 8D 85 C0 00 00 00 48 8B 4C 24 78 48 8B D6 FF D0 84 C0 75 11",
                        "the query's hit callback (ctx, receiver, DamageInfo, user)", kGDirect},
    {kHitPosition,      "C5 F8 10 42 08 C5 F8 11 81 80 00 00 00 C5 FB 10 4A 18 C5 FB 11 89 90 00 00 00", "DamageInfo+0x80 the hit position", kGDirect},
    {kFilterInit,       "48 89 5C 24 18 57 48 83 EC 20 44 89 41 08 48 8B DA 48 8B F9 48 85 D2 74 30", "the filter init", kGDirect},
    {kFree,             "48 83 EC 28 4C 8B C1 48 85 C9 0F 84 BB 00 00 00", "the game's free", kGDirect},
    {kSamDamage,        "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 8B 89 20 05 00 00", "Sam's MsgDamage handler", kGHook},
    {kDamageInfoInit,   "C5 FB 11 43 70 C5 FA 11 73 78 48 89 AB 80 00 00 00 48 8D 4B 28", "DamageInfo +0x78 amount, +0x28 attack reference", kGHook},
};
// clang-format on

using FnVoid1 = void (*)(std::uintptr_t);
using FnFilterInit = void (*)(void*, std::uintptr_t, std::uint32_t);
using HitCb = char (*)(std::uintptr_t ctx, std::uintptr_t receiver, std::uintptr_t dinfo, std::uintptr_t user);
using FnHitQuery = std::uint64_t (*)(std::uintptr_t ctx, const double* centre, float radius, float amount, void* filter, HitCb cb, std::uintptr_t user,
                                     const float* zero16, const float* impulse16, std::int32_t directional, float impulse_scale, std::uint32_t flags,
                                     std::uintptr_t out_hits, std::int32_t out_cap);

// ---------------------------------------------------------------- state
unsigned g_groups = 0;                 // the groups whose checks passed
bool g_cfg_damage = true;
std::atomic<bool> g_on{false};
std::atomic<bool> g_stopped{false};
char g_off[200] = "not installed";
int g_role = 0;
bool g_hooked = false;
bool g_notice_done = false;
std::uintptr_t g_global = 0;           // the attack-stop global (RIP target, checked)
bool g_direct_broken = false, g_bracket_broken = false;
int g_forced[4] = {};                  // per kind: 0 auto, 1 direct, 2 bracket (tests)
double g_now = 0;
TickIn g_in;

// the damage hook's view (worker threads): which attack reference we just used, and what it saw
std::atomic<std::uintptr_t> g_sam{0};             // our Sam (the tick)
std::atomic<std::uintptr_t> g_want_ref{0};        // the attack reference ([ctx+0xE8]) of our newest call
std::atomic<std::uintptr_t> g_prelim_ref{0};      // the preliminary tackle's (+0xA20; the bracket's second query, 0 damage)
std::atomic<std::uint64_t> g_call_tick{0};        // GetTickCount64 of our newest call
std::atomic<std::uint32_t> g_hit_seq{0};          // damaging messages of our attack (amount > 0)
std::atomic<std::uint32_t> g_hit_amount_bits{0};  // the newest one's amount (as received)
std::atomic<std::uint32_t> g_msgs_all{0}, g_msgs_ours{0}, g_msgs_prelim{0}, g_msgs_capped{0}, g_msgs_other{0};
std::atomic<std::uint32_t> g_msgs_logged{0};
std::atomic<std::uintptr_t> g_want_ctx{0};        // the context we passed (diagnostics: where the message holds it)
std::atomic<std::uint32_t> g_diag_logged{0};

// per vehicle (the host's id)
struct Veh {
    trules::HitLimiter lim;
    std::uint32_t seen_seq = 0;
    float health_pass0 = -1;           // health when the pass began
    bool pending_log = false;
    double log_at = 0;
    float log_speed = 0;
    std::uint16_t log_attack = 0;
    trules::Path log_path = trules::Path::none;
    unsigned log_calls = 0;
    std::uint32_t msgs_at_pass = 0, prelim_at_pass = 0, capped_at_pass = 0;
    bool log_crash = false;
    const char* log_how = "";
    vwire::VKey log_key;
    bool no_ctx_logged = false;        // "no attack context" said once until a call is made again
};
std::map<std::uint32_t, Veh> g_veh;

trules::Block g_last_block = trules::Block::count;
double g_block_log_ms = -1e18;
unsigned g_calls = 0, g_call_faults = 0, g_hits = 0, g_sent = 0, g_rx = 0;
unsigned g_kind_calls[4][3] = {};      // [kind][path]
bool g_kind_logged[4][3] = {};
float g_health = -1;
double g_health_ms = 0;

// the crash hit (D3-3)
struct Crash {
    bool on = false;
    std::uintptr_t entity = 0;
    vwire::VKey key;
    double due = 0, until = 0;
    double dv = 0, speed = 0;
    std::uint32_t seq0 = 0;
    float health0 = -1;
    unsigned tries = 0;
    bool gate_logged = false;
};
Crash g_crash;

// the callback's view (game thread, during our call)
struct CbView {
    double sam_c[3] = {};
    unsigned seen = 0, kept = 0, dropped = 0;
    double first_drop[3] = {};
    std::uintptr_t first_receiver = 0, kept_receiver = 0;
};

// the partner capsule's vehicle boxes (M4.4)
struct Track {
    double c[3] = {};
    double ms = 0;                     // the poll time of c
    double speed = 0;
    bool have = false;
    bool have_speed = false;
    bool moving = false;
};
std::map<std::uint32_t, Track> g_track;
trules::VehicleBox g_boxes[32];
std::size_t g_nboxes = 0;
double g_boxes_ms = -1e18;
double g_boxes_poll = -1e18;           // the drive module's poll the boxes were built from

// ---------------------------------------------------------------- reads
bool ReadXf(std::uintptr_t e, WorldTransform& out) noexcept {
    std::uint8_t b1[0x3C], b2[0x3C];
    if (!e || !mem::Read(e + 0xC8, b1, sizeof(b1)) || !mem::Read(e + 0xC8, b2, sizeof(b2)) || std::memcmp(b1, b2, sizeof(b1)) != 0) return false;
    std::memcpy(&out.pos, b1, 0x18);
    std::memcpy(&out.rot, b1 + 0x18, 0x24);
    if (!std::isfinite(out.pos.x) || !std::isfinite(out.pos.y) || !std::isfinite(out.pos.z) || std::fabs(out.pos.x) > 1e7 || std::fabs(out.pos.y) > 1e7 ||
        std::fabs(out.pos.z) > 1e7)
        return false;
    for (const auto& row : out.rot.m)
        for (float v : row)
            if (!std::isfinite(v) || std::fabs(v) > 1.5f) return false;
    return true;
}
const float (&Rows(const WorldTransform& t))[9] { return *reinterpret_cast<const float(*)[9]>(&t.rot); }
void PosOf(const WorldTransform& t, double (&p)[3]) noexcept { p[0] = t.pos.x, p[1] = t.pos.y, p[2] = t.pos.z; }
double Dist(const double (&a)[3], const double (&b)[3]) noexcept {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

float SpeedField(std::uintptr_t v) noexcept {
    const std::uintptr_t hk = mem::Q(v + kVHavok);
    float s = 0;
    if (!hk || !mem::Rd(hk + kVSpeedField, s) || !std::isfinite(s)) return 0;
    return s;
}

// The attack context the hit query takes for a slot: [[V+slot]+0x18] - 0x20 (the wrapper's own step), 0 when the slot is empty.
std::uintptr_t SlotCtx(std::uintptr_t v, std::uintptr_t slot) noexcept {
    const std::uintptr_t s = mem::Q(v + slot);
    if (!s) return 0;
    const std::uintptr_t y = mem::Q(s + 0x18);
    return y ? y - 0x20 : 0;
}
// The attack reference DamageInfo+0x28 gets from that context ([ctx+0xE8]), what the damage hook compares.
std::uintptr_t CtxRef(std::uintptr_t ctx) noexcept { return ctx ? mem::Q(ctx + 0xE8) : 0; }

// The game-wide stop of every vehicle attack, and the vehicle's own (the gates VehicleTackleEmit checks first).
bool AttacksStopped(std::uintptr_t v) noexcept {
    std::uint32_t f = 0;
    if (!mem::Rd(v + kVFlags560, f)) return true;
    if ((f & 0x10) || ((f >> 8) & 1)) return true;
    const std::uintptr_t g = g_global ? mem::Q(g_global) : 0;
    std::uint8_t b = 0;
    if (!g || !mem::Rd(g + 8, b)) return true;
    return ((b >> 6) & 1) != 0;
}

float Health() noexcept {
    game::Diagnostics d;
    game::ReadDiagnostics(&d);
    return d.health_valid ? d.health : -1.0f;
}

// ---------------------------------------------------------------- the damage hook (worker threads: atomics and reads only)
std::atomic<unsigned> g_ride_blocked{0};
// An observer of the shared detour (sam_damage.hpp): it runs before the original handler, as the own detour did before R1.
void H_Damage(std::uintptr_t sam, std::uintptr_t msg) {
    g_msgs_all.fetch_add(1, std::memory_order_relaxed);
    const std::uintptr_t want = g_want_ref.load(std::memory_order_acquire);
    if (ride::ShieldCovers(sam)) {
        // v0.21 (play test 3): our Sam carried as a passenger (or just put off) takes no damage: his capsule is off and the
        // game's own checks see our seat teleports as falls and blows (hidden run 20261120-155755: 71 damage while carried
        // down the road). Only our own vehicle hit (a jump-off at speed, D3-3) gets through. Logged with its source.
        std::uintptr_t ref = 0, ctx_field = 0;
        float amount = 0;
        mem::Rd(msg + 0x20 + 0x28, ref);
        mem::Rd(msg + 0x20 + 0x40, ctx_field);
        const std::uintptr_t want_ctx = g_want_ctx.load(std::memory_order_relaxed);
        const bool recent = GetTickCount64() - g_call_tick.load(std::memory_order_relaxed) < 1500;
        const bool ours = recent && ((want_ctx && ctx_field == want_ctx) || (want && ref && (ref == want || ref == want + 0x20)));
        // our own hit: the health guard (ride_together's setter hook) lets its amount (capped as below) through
        if (ours && mem::Rd(msg + 0x20 + 0x78, amount) && std::isfinite(amount) && amount > 0.0f) ride::AllowOwnHit(trules::CapAmount(amount));
        if (!ours && mem::Rd(msg + 0x20 + 0x78, amount) && std::isfinite(amount) && amount > 0.0f) {
            *reinterpret_cast<volatile float*>(msg + 0x20 + 0x78) = 0.0f;
            if (g_ride_blocked.fetch_add(1, std::memory_order_relaxed) < 24) {
                const char* rn = ref ? mem::RttiName(mem::RttiOf(ref)) : nullptr;
                const char* rn2 = !rn && ref ? mem::RttiName(mem::RttiOf(ref - 0x20)) : nullptr;
                const char* cn = ctx_field ? mem::RttiName(mem::RttiOf(ctx_field)) : nullptr;
                Log("[ride] damage to our Sam while carried (or just off) blocked: amount %.1f; attack %p (%s), context %p (%s)", amount, reinterpret_cast<void*>(ref),
                    rn ? rn : rn2 ? rn2 : "?", reinterpret_cast<void*>(ctx_field), cn ? cn : "?");
            }
        }
    }
    if (want && !g_stopped.load(std::memory_order_relaxed)) {
        std::uintptr_t ref = 0;
        float amount = 0;
        mem::Rd(msg + 0x20 + 0x28, ref);
        // the message (run 20261119-153401): +0x20 the DamageInfo; its +0x28 the attack reference [ctx+0xE8] + 0x20, its +0x40
        // the context the query was called with, its +0x78 the amount
        std::uintptr_t ctx_field = 0;
        mem::Rd(msg + 0x20 + 0x40, ctx_field);
        const std::uintptr_t want_ctx = g_want_ctx.load(std::memory_order_relaxed);
        const bool match = (want_ctx && ctx_field == want_ctx) || (ref && (ref == want || ref == want + 0x20));
        {
            const bool recent = GetTickCount64() - g_call_tick.load(std::memory_order_relaxed) < 1500;
            if (match && recent && mem::Rd(msg + 0x20 + 0x78, amount)) {
                g_msgs_ours.fetch_add(1, std::memory_order_relaxed);
                const float capped = trules::CapAmount(amount);
                if (capped != amount && std::isfinite(amount)) {
                    *reinterpret_cast<volatile float*>(msg + 0x20 + 0x78) = capped;  // D4-2: the message we caused, before Sam handles it
                    g_msgs_capped.fetch_add(1, std::memory_order_relaxed);
                }
                if (amount > 0.0f) {
                    std::uint32_t bits;
                    std::memcpy(&bits, &amount, 4);
                    g_hit_amount_bits.store(bits, std::memory_order_relaxed);
                    g_hit_seq.fetch_add(1, std::memory_order_release);
                }
                if (g_msgs_logged.fetch_add(1, std::memory_order_relaxed) < 24) {
                    const std::uintptr_t ours = g_sam.load(std::memory_order_relaxed);
                    Log("[vhit] Sam's damage handler: a message of our attack, amount %.1f%s (handler's entity %s our Sam)", amount,
                        capped != amount ? " (capped to 100)" : "", sam == ours ? "is" : "is NOT");
                }
            } else if (ref && (ref == g_prelim_ref.load(std::memory_order_relaxed) || ref == g_prelim_ref.load(std::memory_order_relaxed) + 0x20) && recent) {
                g_msgs_prelim.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_msgs_other.fetch_add(1, std::memory_order_relaxed);
                if (recent && g_diag_logged.fetch_add(1, std::memory_order_relaxed) < 6) {
                    // where the message holds our reference / context, and an amount of 100 (the layout, for the log)
                    const std::uintptr_t ctx = g_want_ctx.load(std::memory_order_relaxed);
                    std::uintptr_t q[64] = {};
                    mem::Read(msg, q, sizeof(q));
                    char where[200] = {};
                    std::size_t k = 0;
                    for (int i = 0; i < 64 && k < sizeof(where) - 24; ++i) {
                        if (q[i] && (q[i] == want || q[i] == ctx || q[i] == ctx + 0x20)) k += std::snprintf(where + k, sizeof(where) - k, " +0x%X=%s", i * 8, q[i] == want ? "ref" : q[i] == ctx ? "ctx" : "ctx+0x20");
                        float lo, hi;
                        std::memcpy(&lo, &q[i], 4), std::memcpy(&hi, reinterpret_cast<const char*>(&q[i]) + 4, 4);
                        if (lo == 100.0f) k += std::snprintf(where + k, sizeof(where) - k, " +0x%X=100f", i * 8);
                        if (hi == 100.0f) k += std::snprintf(where + k, sizeof(where) - k, " +0x%X=100f", i * 8 + 4);
                    }
                    float a98 = 0;
                    mem::Rd(msg + 0x98, a98);
                    Log("[vhit] Sam's damage handler: a message within 1.5 s of our call that is not ours by +0x48 (ref %p, want %p, +0x98 %.1f, entity %s our Sam):%s",
                        reinterpret_cast<void*>(ref), reinterpret_cast<void*>(want), a98, sam == g_sam.load(std::memory_order_relaxed) ? "is" : "is NOT",
                        where[0] ? where : " nothing of ours in its first 0x200 bytes");
                }
            }
        }
    }
}

// ---------------------------------------------------------------- the calls (game thread, SEH, POD locals only)
char HitFilter(std::uintptr_t /*ctx*/, std::uintptr_t receiver, std::uintptr_t dinfo, std::uintptr_t user) {
    auto* c = reinterpret_cast<CbView*>(user);
    double h[3] = {};
    const bool ok = mem::Read(dinfo + 0x80, h, sizeof(h));
    ++c->seen;
    if (ok && trules::HitIsSams(h, c->sam_c)) {
        ++c->kept;
        c->kept_receiver = receiver;
        return 1;
    }
    if (c->dropped++ == 0) {
        std::memcpy(c->first_drop, h, sizeof(h));
        c->first_receiver = receiver;
    }
    return 0;
}

// The direct path: the game's attack sphere query with the slot's context, the game's filter and our callback. 1 ok, 0 not
// called (no context), -1 faulted.
int CallDirect(std::uintptr_t v, std::uintptr_t ctx, const double* centre, float radius, float amount, CbView* view) noexcept {
    if (!ctx) return 0;
    alignas(16) std::uint8_t f[0x30] = {};
    alignas(16) const float zero_a[4] = {0, 0, 0, 0};
    alignas(16) const float zero_b[4] = {0, 0, 0, 0};
    __try {
        *reinterpret_cast<std::uintptr_t*>(f) = A(kFilterVt);
        *reinterpret_cast<std::uint32_t*>(f + 0x08) = 2;
        *reinterpret_cast<std::uint64_t*>(f + 0x0C) = 0xFF;
        reinterpret_cast<FnFilterInit>(A(kFilterInit))(f, v, 0x36);
        *reinterpret_cast<std::uint32_t*>(f + 0x0C) = 1;
        reinterpret_cast<FnHitQuery>(A(kHitQuery))(ctx, centre, radius, amount, f, &HitFilter, reinterpret_cast<std::uintptr_t>(view), zero_a, zero_b, 0, 1.0f,
                                                   0x20, 0, 0);
        *reinterpret_cast<std::uintptr_t*>(f) = A(kFilterVt);
        reinterpret_cast<FnVoid1>(A(kFree))(*reinterpret_cast<std::uintptr_t*>(f + 0x20));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The bracket: ride state 4 for one call of the game's VehicleTackleEmit (and the speed field held at 29.5 km/h above 30),
// both restored. 1 ok, -1 faulted.
int CallBracket(std::uintptr_t v, float hold_speed) noexcept {
    auto* rs = reinterpret_cast<volatile std::int32_t*>(v + kVRideState);
    const std::uintptr_t hk = mem::Q(v + kVHavok);
    auto* sp = hk ? reinterpret_cast<volatile float*>(hk + kVSpeedField) : nullptr;
    std::int32_t old_rs = 0;
    float old_sp = 0;
    bool rs_set = false, sp_set = false;
    __try {
        old_rs = *rs;
        if (sp) old_sp = *sp;
        *rs = 4;
        rs_set = true;
        if (sp && hold_speed > 0) {
            *sp = hold_speed;
            sp_set = true;
        }
        reinterpret_cast<FnVoid1>(A(kTackleEmit))(v);
        if (sp_set) *sp = old_sp;
        *rs = old_rs;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        __try {
            if (sp_set) *sp = old_sp;
            if (rs_set) *rs = old_rs;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return -1;
    }
}

// ---------------------------------------------------------------- messages
std::uint16_t OurWorld16() noexcept {
    std::uint32_t c = 0;
    vshare::WorldReadyNow(&c);
    return static_cast<std::uint16_t>(c & 0xFFFF);
}

void SendHit(const vwire::VKey& key, trules::Path path, std::uint16_t attack, bool crash, float speed, float before, float after) {
    trules::HitMsg m;
    m.from = g_role == 1 ? 0 : 1;
    m.kind = key.kind;
    m.path = static_cast<std::uint8_t>(path);
    m.crash = crash ? 1 : 0;
    m.vehicle_id = key.id;
    m.attack = attack;
    m.world = OurWorld16();
    m.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(g_now));
    m.speed_kmh = std::isfinite(speed) ? speed : 0.0f;
    m.health_before = std::isfinite(before) ? before : -1.0f;
    m.health_after = std::isfinite(after) ? after : -1.0f;
    std::uint8_t buf[trules::kMessageSize];
    const std::size_t n = trules::EncodeHit(m, buf, sizeof(buf));
    if (n && vshare::SendEvent(buf, n)) ++g_sent;
}

void OnMsg(std::uint8_t /*type*/, const std::uint8_t* msg, std::size_t size, std::uint32_t /*generation*/, double now) {
    g_now = now;
    trules::HitMsg m;
    if (!trules::DecodeHit(msg, size, m) || m.from == (g_role == 1 ? 0 : 1)) return;
    ++g_rx;
    vdrive::DrivenView v;
    const bool ours = vdrive::LocalDriving(&v) && v.key.id == m.vehicle_id;
    Log("[vhit] the partner's game reports (#%u) %s %s 0x%06X hit their Sam%s: attack 0x%X (%s) speed %.1f km/h health %.1f -> %.1f%s", g_rx, ours ? "our" : "the",
        vwire::KindName(m.kind), m.vehicle_id, m.crash ? " (thrown off in a crash)" : "", m.attack, trules::PathName(static_cast<trules::Path>(m.path)),
        m.speed_kmh, m.health_before, m.health_after, ours ? "; they go down on our screen through the pose stream" : "");
}

void Notice(const char* text, bool warn, double ms) { coop::SetVehicleNotice(text, warn, g_now + ms); }

// ---------------------------------------------------------------- the vehicle boxes (M4.4)
// Once per poll of the drive module (its places change only then, every 100 ms): each vehicle's speed from its place at
// this poll and the last, over the polls' own time apart (sampling faster read every other sample as standing still or
// twice as fast), with the moving flag's hysteresis (on over 1 m/s, off at 0.6 m/s).
void BoxesTick() {
    const double poll = vshare::FootprintsMs();
    if (poll == g_boxes_poll) return;
    g_boxes_poll = poll;
    g_boxes_ms = g_now;
    vwire::Footprint fp[32];
    std::uint32_t ids[32] = {};
    const std::size_t n = vshare::Footprints(0, fp, 32, ids);
    g_nboxes = 0;
    for (std::size_t i = 0; i < n && g_nboxes < 32; ++i) {
        const std::uint32_t key = ids[i] ? ids[i] : 0x80000000u | static_cast<std::uint32_t>(i);
        Track& t = g_track[key];
        const double dt = poll - t.ms;
        if (t.have && dt > 0 && dt <= 500) {
            t.speed = trules::SpeedSample(t.speed, t.have_speed, t.c, fp[i].c, dt);
            t.have_speed = true;
        } else {
            t.speed = 0;  // the first sample, or after a gap: from here on
            t.have_speed = false;
        }
        t.moving = trules::Moving(t.moving, t.speed);
        std::memcpy(t.c, fp[i].c, sizeof(t.c));
        t.ms = poll;
        t.have = true;
        trules::VehicleBox& b = g_boxes[g_nboxes++];
        std::memcpy(b.c, fp[i].c, sizeof(b.c));
        std::memcpy(b.rot, fp[i].rot, sizeof(b.rot));
        std::memcpy(b.half, fp[i].half, sizeof(b.half));
        b.speed_mps = t.speed;
        b.moving = t.moving;
    }
    if (g_track.size() > 64) g_track.clear();
}

// ---------------------------------------------------------------- the hit seen / its log line
void HealthTick() {
    if (g_now - g_health_ms < 50) return;
    g_health_ms = g_now;
    g_health = Health();
}

void StartLog(Veh& r, const vwire::VKey& key, trules::Path path, std::uint16_t attack, float speed, bool crash, const char* how) {
    r.pending_log = true;
    r.log_at = g_now + 400;  // the health drop lands within a few frames
    r.log_speed = speed;
    r.log_attack = attack;
    r.log_path = path;
    r.log_crash = crash;
    r.log_how = how;
    r.log_key = key;
}

void FinishLogs() {
    for (auto& kv : g_veh) {
        Veh& r = kv.second;
        if (!r.pending_log || g_now < r.log_at) continue;
        r.pending_log = false;
        const vwire::VKey key = r.log_key;
        const float after = Health();
        const std::uint32_t ours = g_msgs_ours.load() - r.msgs_at_pass, prelim = g_msgs_prelim.load() - r.prelim_at_pass,
                            capped = g_msgs_capped.load() - r.capped_at_pass;
        ++g_hits;
        Log("[vhit] partner vehicle %s 0x%06X hit Sam (#%u): attack 0x%X speed %.1f km/h health %.1f -> %.1f (%s%s; %u call(s) this pass, %u damage "
            "message(s) of the attack, %u capped, %u preliminary; seen by %s)",
            vwire::KindName(key.kind), key.id, g_hits, r.log_attack, r.log_speed, r.health_pass0, after, trules::PathName(r.log_path),
            r.log_crash ? ", thrown off in a crash" : "", r.log_calls, ours, capped, prelim, r.log_how);
        if (key.kind) SendHit(key, r.log_path, r.log_attack, r.log_crash, r.log_speed, r.health_pass0, after);
        char text[160];
        std::snprintf(text, sizeof(text), "%s", r.log_crash ? "Hurt jumping off the moving vehicle." :"Hit by your partner's vehicle.");
        Notice(text, true, 2500);
    }
}

// A hit from this vehicle: the damage handler saw a damaging message of our attack, or the health dropped since the pass began.
bool HitSeen(Veh& r, const char** how) {
    const std::uint32_t seq = g_hit_seq.load(std::memory_order_acquire);
    if (seq != r.seen_seq) {
        r.seen_seq = seq;
        *how = "Sam's damage handler";
        return true;
    }
    if (r.health_pass0 > 0 && g_health > 0 && g_health < r.health_pass0 - 1.0f) {
        *how = "the health drop";
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- PartnerTackle (M4.2)
// One line per change of the gate's answer (a flapping answer: at most one a second, "ok" always).
void LogBlock(trules::Block b, const vwire::VKey& key, double speed, double dist) {
    if (b == g_last_block) return;
    if (b == trules::Block::no_vehicle && g_last_block == trules::Block::count) {
        g_last_block = b;
        return;
    }
    if (b != trules::Block::ok && g_now - g_block_log_ms < 1000) return;
    g_last_block = b;
    g_block_log_ms = g_now;
    if (b == trules::Block::no_vehicle) Log("[vhit] the partner drives no vehicle we follow now");
    else if (b == trules::Block::ok) Log("[vhit] partner vehicle %s 0x%06X: in reach of our Sam: calling its attack (%.1f km/h, %.1f m)", vwire::KindName(key.kind), key.id, speed, dist);
    else Log("[vhit] partner vehicle %s 0x%06X: %s (%.1f km/h, %.1f m)", vwire::KindName(key.kind), key.id, trules::BlockName(b), speed, dist);
}

void TackleTick() {
    vdrive::DrivenView view;
    trules::GateIn g;
    g.feature_on = g_on.load();
    g.friendly_fire = g_cfg_damage;
    g.have_vehicle = vdrive::PartnerDriving(&view);
    const std::uintptr_t v = g.have_vehicle ? view.entity : 0;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    g_sam.store(sam, std::memory_order_relaxed);
    if (!g.have_vehicle) {
        LogBlock(trules::Block::no_vehicle, view.key, 0, 0);
        return;
    }
    Veh& r = g_veh[view.key.id];
    // a pass without a call for a second is over (a later hit or health drop is not its)
    for (auto& kv : g_veh) kv.second.lim.Expire(g_now);
    // a hit seen since the last tick ends the pass
    const char* how = nullptr;
    if (r.lim.InPass() && HitSeen(r, &how)) {
        r.lim.HitSeen(g_now);
        StartLog(r, view.key, r.log_path, r.log_attack, r.log_speed, false, how);
        r.log_calls = r.lim.CallsThisPass();
    } else if (!r.lim.InPass()) {
        r.seen_seq = g_hit_seq.load(std::memory_order_acquire);
    }
    std::uint32_t drv = 0;
    std::int32_t rs = 0;
    mem::Rd(v + kVDriver, drv), mem::Rd(v + kVRideState, rs);
    g.partner_drives = view.have_frame && view.frame_seated && drv == kNoActor && rs == 2;
    g.local_ok = sam && g_in.local_ok;
    g.local_awake = g_in.local_active;
    g.local_on_foot = sam && mem::Q(sam + kEParent) == 0;
    g.carried = ride::Carried() || ride::JustRode(v);  // v0.21: nor the vehicle he just got off
    static double next_seq_ms = 0;
    static const char* seq_block = nullptr;
    {
        const bool seq = g_now >= next_seq_ms;
        if (seq) next_seq_ms = g_now + 250;
        const char* b2 = game::LocalCollisionBlock(seq);
        if (seq || b2) seq_block = b2;
        g.game_blocked = seq_block != nullptr || AttacksStopped(v);
    }
    WorldTransform vx{}, sx{};
    double vp[3] = {}, sp[3] = {}, sc[3] = {}, centre[3] = {};
    const bool have_xf = ReadXf(v, vx) && sam && ReadXf(sam, sx);
    if (have_xf) {
        PosOf(vx, vp), PosOf(sx, sp);
        trules::SamCentre(sp, sc);
        g.dist_m = Dist(vp, sp);
    }
    const float speed = SpeedField(v);
    g.speed_kmh = speed;
    g.now_ms = g_now;
    g.hold_off_until_ms = r.lim.HoldOffUntil();
    float local[3] = {}, radius = 0, signed_speed = 0;
    const bool have_sphere = mem::Read(v + kVSphereLocal, local, sizeof(local)) && mem::Rd(v + kVRadius, radius) && mem::Rd(v + kVSignedSpeed, signed_speed) &&
                             std::isfinite(local[0]) && std::isfinite(local[1]) && std::isfinite(local[2]) && std::isfinite(radius) && radius > 0.05f &&
                             radius < 10.0f && std::isfinite(signed_speed);
    if (have_xf && have_sphere) {
        trules::SphereCentre(vp, Rows(vx), local, signed_speed, centre);
        g.sam_to_sphere_m = Dist(sc, centre);
        g.sphere_radius_m = radius;
    }
    const std::uintptr_t mule_ctx = SlotCtx(v, kVSlotMule);
    const bool has_mule = mule_ctx != 0;
    const std::uint8_t kind = view.key.kind;
    g.path = trules::PathFor(kind, has_mule, (g_groups & kGDirect) && !g_direct_broken, (g_groups & kGBracket) && !g_bracket_broken,
                             kind <= 3 ? g_forced[kind] : 0);
    const trules::Block b = trules::Gate(g);
    if (b != trules::Block::ok) {
        if (b == trules::Block::not_near && g_last_block != b && g_now - g_block_log_ms >= 1000)
            Log("[vhit] partner vehicle %s 0x%06X: our Sam's centre %.2f m from its attack sphere's centre (radius %.2f m)", vwire::KindName(view.key.kind),
                view.key.id, g.sam_to_sphere_m, g.sphere_radius_m);
        LogBlock(b, view.key, speed, g.dist_m);
        return;
    }
    // the attack slots the call reads, before any call: the game empties them when the vehicle leaves the world (0x142F76180
    // clears +0x9E0 / +0xA00 / +0xA20 / +0xA40 together), and VehicleTackleEmit reads through them without a null check
    const std::uintptr_t ctx = g.path == trules::Path::direct ? (has_mule ? mule_ctx : SlotCtx(v, kVSlotPlayer)) : SlotCtx(v, kVSlotPlayer);
    const bool slots_ok = ctx != 0 && (g.path != trules::Path::bracket || (mem::Q(v + kVSlotPlayer) != 0 && mem::Q(v + kVSlotPrelim) != 0));
    if (!slots_ok) {
        if (!r.no_ctx_logged) {
            r.no_ctx_logged = true;
            Log("[vhit] %s 0x%06X: no attack context in its slot (%s): not called", vwire::KindName(kind), view.key.id,
                g.path == trules::Path::bracket ? "+0xA00 / +0xA20" : has_mule ? "+0x9E0" : "+0xA00");
        }
        return;
    }
    r.no_ctx_logged = false;
    LogBlock(b, view.key, speed, g.dist_m);
    const std::uint16_t attack = trules::AttackOf(g.path, has_mule);
    if (!r.lim.InPass()) {
        r.health_pass0 = g_health;
        r.msgs_at_pass = g_msgs_ours.load(), r.prelim_at_pass = g_msgs_prelim.load(), r.capped_at_pass = g_msgs_capped.load();
        r.seen_seq = g_hit_seq.load(std::memory_order_acquire);
    }
    r.log_path = g.path, r.log_attack = attack, r.log_speed = speed;
    r.lim.Called(g_now);
    ++g_calls;
    const int pi = static_cast<int>(g.path);
    if (kind <= 3) ++g_kind_calls[kind][pi];
    if (kind <= 3 && !g_kind_logged[kind][pi]) {
        g_kind_logged[kind][pi] = true;
        Log("[vhit] the partner's %s uses the %s path: attack 0x%X (%s), sphere radius %.2f m at local (%.2f, %.2f, %.2f)", vwire::KindName(kind),
            trules::PathName(g.path), attack, attack == trules::kAttackMule ? "VehicleTackle, the MULE truck attack" : "VehicleTacklePlayer", radius, local[0],
            local[1], local[2]);
    }
    if (g.path == trules::Path::direct) {
        g_want_ctx.store(ctx, std::memory_order_relaxed);
        g_want_ref.store(CtxRef(ctx), std::memory_order_release);
        g_prelim_ref.store(0, std::memory_order_relaxed);
        g_call_tick.store(GetTickCount64(), std::memory_order_relaxed);
        CbView cv;
        std::memcpy(cv.sam_c, sc, sizeof(sc));
        const int res = CallDirect(v, ctx, centre, radius, trules::kHitAmount, &cv);
        if (res < 0) {
            g_direct_broken = true;
            ++g_call_faults;
            Log("[vhit] FAULT in the direct hit call (%s 0x%06X): the direct path is off for this session", vwire::KindName(kind), view.key.id);
        } else if (res == 0) {
            Log("[vhit] %s 0x%06X: no attack context in its slot: not called", vwire::KindName(kind), view.key.id);
        } else if (cv.seen && (cv.kept || cv.dropped)) {
            static unsigned logged = 0;
            if (logged++ < 12)
                Log("[vhit] direct call: %u hit(s) in the sphere, %u on our Sam kept, %u dropped%s (first dropped at %.2f m from his centre)", cv.seen, cv.kept,
                    cv.dropped, cv.dropped ? " (D4-3: not ours)" : "",
                    cv.dropped ? std::sqrt((cv.first_drop[0] - sc[0]) * (cv.first_drop[0] - sc[0]) + (cv.first_drop[1] - sc[1]) * (cv.first_drop[1] - sc[1]) +
                                           (cv.first_drop[2] - sc[2]) * (cv.first_drop[2] - sc[2]))
                               : 0.0);
        }
    } else if (g.path == trules::Path::bracket) {
        g_want_ctx.store(ctx, std::memory_order_relaxed);
        g_want_ref.store(CtxRef(ctx), std::memory_order_release);
        g_prelim_ref.store(CtxRef(SlotCtx(v, kVSlotPrelim)), std::memory_order_relaxed);
        g_call_tick.store(GetTickCount64(), std::memory_order_relaxed);
        const float hold = trules::BracketSpeed(speed);
        const int res = CallBracket(v, hold);
        if (res < 0) {
            g_bracket_broken = true;
            ++g_call_faults;
            Log("[vhit] FAULT in the state-4 bracket (%s 0x%06X): the bracket is off for this session", vwire::KindName(kind), view.key.id);
        }
    }
}

// ---------------------------------------------------------------- the crash hit (D3-3)
bool OnCrash(std::uintptr_t vehicle_entity, double dv_mps, double speed_mps) noexcept {
    if (!g_on.load()) {
        Log("[vhit] crash exit (dv %.1f m/s): no hit, %s", dv_mps, g_off[0] ? g_off : "off");
        return false;
    }
    if (!g_cfg_damage) {
        Log("[vhit] crash exit (dv %.1f m/s): no hit, partner_vehicle_damage=0", dv_mps);
        return false;
    }
    if (!(g_groups & kGDirect) || g_direct_broken) {
        Log("[vhit] crash exit (dv %.1f m/s): no hit, the direct path is off (its checks or a fault)", dv_mps);
        return false;
    }
    vshare::Unit u;
    if (!vshare::FindByEntity(vehicle_entity, &u)) {
        Log("[vhit] crash exit: the vehicle is not a shared vehicle of this world: no hit");
        return false;
    }
    g_crash = Crash{};
    g_crash.on = true;
    g_crash.entity = vehicle_entity;
    g_crash.key = u.key;
    g_crash.due = g_now + trules::kCrashDelayMs;
    g_crash.until = g_crash.due + trules::kCrashWindowMs;
    g_crash.dv = dv_mps;
    g_crash.speed = speed_mps;
    g_crash.health0 = Health();
    g_crash.seq0 = g_hit_seq.load(std::memory_order_acquire);
    Log("[vhit] crash exit from the %s 0x%06X (dv %.1f m/s at %.1f m/s): the game's hit on our Sam in %.0f ms", vwire::KindName(u.key.kind), u.key.id, dv_mps,
        speed_mps, trules::kCrashDelayMs);
    return true;
}
bool CrashThunk(std::uintptr_t vehicle_entity, double dv_mps, double speed_mps) noexcept { return OnCrash(vehicle_entity, dv_mps, speed_mps); }

// One hit on our Sam from vehicle `v` through the direct path with the crash sphere (D3-3 and the debug action). 1 called,
// 0 not possible, -1 faulted.
int HitSamNow(std::uintptr_t v, std::uintptr_t sam, std::uint16_t* attack_out, CbView* cv) {
    WorldTransform vx{}, sx{};
    if (!ReadXf(v, vx) || !ReadXf(sam, sx)) return 0;
    double vp[3], sp[3], centre[3];
    PosOf(vx, vp), PosOf(sx, sp);
    trules::CrashSphere(sp, vp, centre);
    trules::SamCentre(sp, cv->sam_c);
    const std::uintptr_t mule = SlotCtx(v, kVSlotMule);
    const std::uintptr_t ctx = mule ? mule : SlotCtx(v, kVSlotPlayer);
    *attack_out = mule ? trules::kAttackMule : trules::kAttackPlayer;
    g_want_ctx.store(ctx, std::memory_order_relaxed);
    g_want_ref.store(CtxRef(ctx), std::memory_order_release);
    g_prelim_ref.store(0, std::memory_order_relaxed);
    g_call_tick.store(GetTickCount64(), std::memory_order_relaxed);
    const int r = CallDirect(v, ctx, centre, static_cast<float>(trules::kCrashSphereM), trules::kHitAmount, cv);
    if (r < 0) {
        g_direct_broken = true;
        ++g_call_faults;
        Log("[vhit] FAULT in the direct hit call (our Sam's own hit): the direct path is off for this session");
    }
    return r;
}

void CrashTick() {
    if (!g_crash.on || g_now < g_crash.due) return;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    Veh& r = g_veh[g_crash.key.id];
    const std::uint32_t seq = g_hit_seq.load(std::memory_order_acquire);
    const bool health_drop = g_crash.health0 > 0 && g_health > 0 && g_health < g_crash.health0 - 1.0f;
    if (g_crash.tries > 0 && (seq != g_crash.seq0 || health_drop)) {
        r.health_pass0 = g_crash.health0;
        r.log_calls = g_crash.tries;
        StartLog(r, g_crash.key, trules::Path::direct, r.log_attack, static_cast<float>(g_crash.speed * 3.6), true,
                 seq != g_crash.seq0 ? "Sam's damage handler" : "the health drop");
        g_crash.on = false;
        return;
    }
    if (g_now > g_crash.until) {
        Log("[vhit] crash exit: no hit was seen in %.0f ms (%u call(s)); our Sam stays as he was put", trules::kCrashWindowMs, g_crash.tries);
        g_crash.on = false;
        return;
    }
    if (!sam || !g_in.local_ok || !g_in.local_active || mem::Q(sam + kEParent) != 0 || ride::Carried()) return;
    vshare::Unit u;
    if (!vshare::FindByEntity(g_crash.entity, &u) || u.key.id != g_crash.key.id) {
        Log("[vhit] crash exit: the vehicle is gone: no hit");
        g_crash.on = false;
        return;
    }
    // the same gates as PartnerTackle's: no load / menu / cutscene / post-load setup / snap / sequence, and not while the
    // game stops vehicle attacks; the window waits for them (and ends without a hit at its end)
    {
        const char* gb = game::LocalCollisionBlock(true);
        const bool stopped = AttacksStopped(g_crash.entity);
        if (gb || stopped) {
            if (!g_crash.gate_logged) {
                g_crash.gate_logged = true;
                Log("[vhit] crash exit: the hit waits: %s", gb ? gb : "the game stops vehicle attacks now");
            }
            return;
        }
    }
    if (g_crash.tries == 0) {
        r.msgs_at_pass = g_msgs_ours.load(), r.prelim_at_pass = g_msgs_prelim.load(), r.capped_at_pass = g_msgs_capped.load();
    }
    std::uint16_t attack = 0;
    CbView cv;
    const int res = HitSamNow(g_crash.entity, sam, &attack, &cv);
    ++g_crash.tries;
    r.log_attack = attack;
    if (res <= 0) {
        if (res == 0) Log("[vhit] crash exit: the vehicle or our Sam is unreadable: no hit");
        g_crash.on = false;
        return;
    }
    if (g_crash.tries <= 2)
        Log("[vhit] crash hit call #%u: attack 0x%X, %u hit(s) in the sphere, %u on our Sam", g_crash.tries, attack, cv.seen, cv.kept);
}

void TickBody(const TickIn& in, double now) {
    g_in = in;
    g_now = now;
    BoxesTick();
    if (!g_on.load()) return;
    HealthTick();
    TackleTick();
    CrashTick();
    FinishLogs();
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
    g_cfg_damage = config.partner_vehicle_damage;
    unsigned bad_groups = 0;
    const char* first_bad = nullptr;
    for (const Site& s : kSites) {
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            if (s.va == kFilterInit && ridecam::FilterInitHooked()) {
                // ridecam's camera filter detour went in first (its own byte check covers these bytes); our calls pass through it
                Log("[vhit] the filter init at 0x%llX carries the ride camera's detour (checked before it went in): ok",
                    static_cast<unsigned long long>(s.va));
                continue;
            }
            bad_groups |= s.group;
            if (!first_bad) first_bad = s.name;
            Log("[vhit] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
        }
    }
    // the filter's vtable: in the image, its first entry in .text; the global from the checked instruction
    if (!(mem::InImage(A(kFilterVt)) && mem::InText(mem::Q(A(kFilterVt))) && mem::RipTarget(A(kTackleFilter), 3, 7) == A(kFilterVt))) {
        bad_groups |= kGDirect;
        if (!first_bad) first_bad = "the filter's vtable";
        Log("[vhit] byte check FAILED: the filter's vtable 0x%llX", static_cast<unsigned long long>(kFilterVt));
    }
    g_global = mem::RipTarget(A(kTackleGlobalInsn), 3, 7);
    if (!g_global || !mem::InImage(g_global)) {
        bad_groups |= kGCommon;
        if (!first_bad) first_bad = "the attack-stop global";
    }
    unsigned groups = kGCommon | kGDirect | kGBracket | kGHook;
    groups &= ~bad_groups;
    if (!(groups & kGCommon)) groups = 0;  // the gates both paths mirror
    g_groups = groups;
    const unsigned n = static_cast<unsigned>(sizeof(kSites) / sizeof(kSites[0])) + 2;
    if (!(groups & (kGDirect | kGBracket))) {
        std::snprintf(g_off, sizeof(g_off), "this game build differs (%s)", first_bad ? first_bad : "?");
        Log("[vhit] vehicle hits OFF: %s", g_off);
        return;
    }
    if (!vshare::Active() || !vdrive::Active()) {
        std::snprintf(g_off, sizeof(g_off), "%s", !vshare::Active() ? "the real shared vehicles are off" : "remote driving is off");
        Log("[vhit] vehicle hits OFF: %s (%u checks: direct %s, bracket %s)", g_off, n, (groups & kGDirect) ? "ok" : "FAILED", (groups & kGBracket) ? "ok" : "FAILED");
        return;
    }
    g_role = vshare::Role();
    vshare::RegisterHandler(trules::kSvmType, &OnMsg);  // the partner's reports (logged whatever our own setting)
    if (!g_cfg_damage) {
        std::snprintf(g_off, sizeof(g_off), "session.ini partner_vehicle_damage=0");
        Log("[vhit] vehicle hits OFF on this game: %s (the partner's vehicles never hurt our Sam; %u checks ok)", g_off, n);
        ride::SetCrashHitHandler(&CrashThunk);  // logs "no hit" on a crash exit
        return;
    }
    // Sam's damage handler: counts, caps the messages of our attack (start-up only: enabling a hook suspends every thread)
    if (groups & kGHook) {
        // R1 (docs/enemy-sync.md): the one shared detour (sam_damage.cpp) with this module as its first observer
        g_hooked = samdmg::Ensure("vhit") && samdmg::Observe(&H_Damage);
        if (!g_hooked) {
            Log("[vhit] Sam's damage handler not hooked (%s): hits are seen by the health drop only; the bracket's speed hold still caps",
                samdmg::Installed() ? "observer table full" : samdmg::OffReason());
            g_groups &= ~kGHook;
        }
    }
    ride::SetCrashHitHandler(&CrashThunk);
    g_off[0] = 0;
    g_on.store(true);
    Log("[vhit] vehicle hits ON (%s): %u checks; direct path %s (trucks: VehicleTackle 0x157, trike / roadster: VehicleTacklePlayer 0x158; only our "
        "Sam), state-4 bracket %s (the fallback without the direct path), damage handler %s; no instant kill (amount capped at 100)",
        g_role == 1 ? "host" : "guest", n, (groups & kGDirect) ? "ok" : "OFF (checks)", (groups & kGBracket) ? "ok" : "OFF (checks)",
        g_hooked ? "hooked" : "not hooked");
}

bool Active() noexcept { return g_on.load(); }
const char* OffReason() noexcept { return g_on.load() ? "" : g_off; }

void Tick(const TickIn& in, double now) noexcept {
    if (g_stopped.load()) return;
    if (!g_on.load()) {
        if (!g_notice_done && g_cfg_damage && g_off[0] && std::strcmp(g_off, "not installed") != 0 && coop::Active() && vdrive::Active()) {
            g_notice_done = true;
            char text[220];
            std::snprintf(text, sizeof(text), "Vehicle hits off: %s.", g_off);
            coop::SetVehicleNotice(text, true, now + 60000);
        }
    }
    if (!TickGuarded(in, now)) {
        Log("[vhit] FAULT in the vehicle-hit tick: stopped (no more calls)");
        EmergencyStop();
    }
}

void EmergencyStop() noexcept {
    g_stopped.store(true);
    g_on.store(false);
    g_want_ref.store(0);
    std::snprintf(g_off, sizeof(g_off), "stopped");
}

const char* CapsuleVehicleBlock(const double (&body)[3], bool enabling) noexcept {
    if (g_stopped.load() || g_nboxes == 0 || g_now - g_boxes_ms > 500) return nullptr;
    return trules::CapsuleVehicleBlock(body, g_boxes, g_nboxes, enabling);
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    switch (op) {
        case 0: {
            char full[900];
            std::snprintf(full, sizeof(full),
                          "%s%s; direct %s%s, bracket %s%s, hook %s; calls %u (truck d/b %u/%u, trike %u/%u, roadster %u/%u), faults %u, hits %u; damage "
                          "messages all %u, of our attack %u, capped %u, preliminary %u, other %u; STH1 sent %u received %u; forced paths roadster %d truck %d "
                          "trike %d; capsule boxes %zu; our health %.1f",
                          g_on.load() ? "ON" : "OFF: ", g_on.load() ? "" : g_off, (g_groups & kGDirect) ? "ok" : "off", g_direct_broken ? " (faulted)" : "",
                          (g_groups & kGBracket) ? "ok" : "off", g_bracket_broken ? " (faulted)" : "", g_hooked ? "on" : "off", g_calls, g_kind_calls[2][1],
                          g_kind_calls[2][2], g_kind_calls[3][1], g_kind_calls[3][2], g_kind_calls[1][1], g_kind_calls[1][2], g_call_faults, g_hits,
                          g_msgs_all.load(), g_msgs_ours.load(), g_msgs_capped.load(), g_msgs_prelim.load(), g_msgs_other.load(), g_sent, g_rx, g_forced[1],
                          g_forced[2], g_forced[3], g_nboxes, static_cast<double>(Health()));
            Log("[vhit] state: %s", full);
            std::snprintf(msg, cap, "%s", full);
            return true;
        }
        case 1: {
            const int k = static_cast<int>(v[1]), p = static_cast<int>(v[2]);
            if (k < 1 || k > 3 || p < 0 || p > 2) return std::snprintf(msg, cap, "usage: 1 <kind 1..3> <0 auto | 1 direct | 2 bracket>"), false;
            g_forced[k] = p;
            Log("[vhit] test: the %s's path is now %s", vwire::KindName(static_cast<std::uint8_t>(k)), p == 0 ? "auto" : p == 1 ? "direct" : "the bracket");
            return std::snprintf(msg, cap, "%s path %d", vwire::KindName(static_cast<std::uint8_t>(k)), p), true;
        }
        case 2: {
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            if (!g_on.load()) return std::snprintf(msg, cap, "vehicle hits are off: %s", g_off), false;
            if (!(g_groups & kGDirect) || g_direct_broken) return std::snprintf(msg, cap, "the direct path is off"), false;
            vdrive::DrivenView d;
            if (!vdrive::PartnerDriving(&d)) return std::snprintf(msg, cap, "the partner drives nothing we follow"), false;
            const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
            if (!sam) return std::snprintf(msg, cap, "no local Sam"), false;
            Veh& r = g_veh[d.key.id];
            r.health_pass0 = Health();
            r.msgs_at_pass = g_msgs_ours.load(), r.prelim_at_pass = g_msgs_prelim.load(), r.capped_at_pass = g_msgs_capped.load();
            std::uint16_t attack = 0;
            CbView cv;
            const int res = HitSamNow(d.entity, sam, &attack, &cv);
            if (res > 0) {
                r.log_calls = 1;
                StartLog(r, d.key, trules::Path::direct, attack, SpeedField(d.entity), false, "the test (one call)");
            }
            return std::snprintf(msg, cap, "one hit call (attack 0x%X): %s, %u hit(s) in the sphere, %u on our Sam", attack, res > 0 ? "called" : res == 0 ? "not possible" : "FAULT",
                                 cv.seen, cv.kept),
                   res > 0;
        }
        case 8: {
            // hidden tests: our Sam v[1] m (default 15) in front of the vehicle the partner drives here (v[2] m to its right), facing it
            vdrive::DrivenView d;
            if (!vdrive::PartnerDriving(&d)) return std::snprintf(msg, cap, "the partner drives nothing we follow"), false;
            WorldTransform xf{};
            if (!ReadXf(d.entity, xf)) return std::snprintf(msg, cap, "unreadable"), false;
            const double ahead = v[1] > 0 ? v[1] : 15.0, right = v[2];
            const double x = xf.pos.x + xf.rot.m[1][0] * ahead + xf.rot.m[0][0] * right;
            const double y = xf.pos.y + xf.rot.m[1][1] * ahead + xf.rot.m[0][1] * right;
            const double z = xf.pos.z + xf.rot.m[1][2] * ahead + xf.rot.m[0][2] * right + 0.5;
            char why[160] = {};
            const bool ok = game::TeleportLocalPlayerFacing(x, y, z, -xf.rot.m[1][0], -xf.rot.m[1][1], game::TeleportMode::place, why, sizeof(why));
            Log("[vhit] test: our Sam %.1f m in front of the partner's %s 0x%06X (%.1f m right): %s", ahead, vwire::KindName(d.key.kind), d.key.id, right, why);
            return std::snprintf(msg, cap, "%.1f m in front of the %s: %s", ahead, vwire::KindName(d.key.kind), why), ok;
        }
        case 9: {
            char buf[700];
            std::size_t k = 0;
            k += std::snprintf(buf + k, sizeof(buf) - k, "%zu vehicle box(es):", g_nboxes);
            for (std::size_t i = 0; i < g_nboxes && i < 8 && k < sizeof(buf); ++i)
                k += std::snprintf(buf + k, sizeof(buf) - k, " (%.1f, %.1f, %.1f) %.1f m/s;", g_boxes[i].c[0], g_boxes[i].c[1], g_boxes[i].c[2], g_boxes[i].speed_mps);
            Log("[vhit] %s", buf);
            std::snprintf(msg, cap, "%s", buf);
            return true;
        }
        case 10: {
            // hidden tests (M4.4 bump): the nearest shared vehicle of kind v[1] that nobody drives (within 500 m), put by the game's
            // RequestTeleport |v[2]| m (default 18) in front of the vehicle the partner drives here (else of our Sam), at its height,
            // facing it (turned 180 deg); a negative v[2]: broadside (turned 90 deg)
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            const int kind = static_cast<int>(v[1]);
            double ahead = v[2] != 0 ? std::fabs(v[2]) : 18.0;
            const bool broadside = v[2] < 0;
            WorldTransform ref{};
            vdrive::DrivenView d;
            std::uintptr_t ref_e = 0;
            if (vdrive::PartnerDriving(&d)) ref_e = d.entity;
            else ref_e = reinterpret_cast<std::uintptr_t>(g_in.local);
            if (!ref_e || !ReadXf(ref_e, ref)) return std::snprintf(msg, cap, "no reference (partner vehicle or our Sam)"), false;
            vshare::Unit units[64];
            const std::size_t n = vshare::SharedUnits(units, 64);
            const vshare::Unit* best = nullptr;
            double bd = 500;
            double rp[3];
            PosOf(ref, rp);
            for (std::size_t i = 0; i < n; ++i) {
                const vshare::Unit& u = units[i];
                if (!u.entity || !u.has_xf || u.entity == ref_e || (kind && u.key.kind != kind) || u.rider != vwire::kNoRider) continue;
                double up[3];
                PosOf(u.xf, up);
                const double dd = Dist(up, rp);
                if (dd < bd) bd = dd, best = &u;
            }
            if (!best) return std::snprintf(msg, cap, "no free %s within 500 m", kind ? vwire::KindName(static_cast<std::uint8_t>(kind)) : "vehicle"), false;
            WorldTransform to = ref;
            to.pos.x = ref.pos.x + ref.rot.m[1][0] * ahead, to.pos.y = ref.pos.y + ref.rot.m[1][1] * ahead, to.pos.z = ref.pos.z + ref.rot.m[1][2] * ahead + 0.3;
            for (int i = 0; i < 3; ++i) {
                if (broadside) {
                    to.rot.m[0][i] = ref.rot.m[1][i];   // right = the reference's forward
                    to.rot.m[1][i] = -ref.rot.m[0][i];  // forward = the reference's left
                } else {
                    to.rot.m[0][i] = -ref.rot.m[0][i];  // facing it: 180 deg about up
                    to.rot.m[1][i] = -ref.rot.m[1][i];
                }
            }
            const bool ok = vshare::RequestTeleport(best->entity, to);
            Log("[vhit] test: the %s 0x%06X put %.1f m in front of the %s, %s: RequestTeleport %s", vwire::KindName(best->key.kind), best->key.id, ahead,
                ref_e == reinterpret_cast<std::uintptr_t>(g_in.local) ? "our Sam" : "partner's vehicle", broadside ? "broadside" : "facing it",
                ok ? "requested" : "FAILED");
            return std::snprintf(msg, cap, "%s 0x%06X: %s", vwire::KindName(best->key.kind), best->key.id, ok ? "requested" : "failed"), ok;
        }
        case 12: {
            // hidden tests: the nearest free shared vehicle of kind v[1] (within 500 m) put by RequestTeleport v[2] m along the
            // nearest other free truck's forward (negative: behind it), on its heading (the trike on the trucks' straight road)
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            const int kind = static_cast<int>(v[1]);
            WorldTransform sx{};
            if (!ReadXf(reinterpret_cast<std::uintptr_t>(g_in.local), sx)) return std::snprintf(msg, cap, "no local Sam"), false;
            double sp[3];
            PosOf(sx, sp);
            vshare::Unit units[64];
            const std::size_t n = vshare::SharedUnits(units, 64);
            const vshare::Unit* mv = nullptr;
            const vshare::Unit* ref = nullptr;
            double bm = 500, br = 500;
            for (std::size_t i = 0; i < n; ++i) {
                const vshare::Unit& u = units[i];
                if (!u.entity || !u.has_xf || u.rider != vwire::kNoRider) continue;
                double up[3];
                PosOf(u.xf, up);
                const double dd = Dist(up, sp);
                if (u.key.kind == kind && dd < bm) bm = dd, mv = &u;
                if (u.key.kind == 2 && dd < br) br = dd, ref = &u;
            }
            if (!mv || !ref || mv == ref) return std::snprintf(msg, cap, "no free %s and truck within 500 m", vwire::KindName(static_cast<std::uint8_t>(kind))), false;
            WorldTransform rx{};
            if (!ReadXf(ref->entity, rx)) return std::snprintf(msg, cap, "unreadable"), false;
            WorldTransform to = rx;
            to.pos.x += rx.rot.m[1][0] * v[2], to.pos.y += rx.rot.m[1][1] * v[2], to.pos.z += rx.rot.m[1][2] * v[2] + 0.3;
            const bool ok = vshare::RequestTeleport(mv->entity, to);
            Log("[vhit] test: the %s 0x%06X put %.1f m along the truck 0x%06X: RequestTeleport %s", vwire::KindName(mv->key.kind), mv->key.id, v[2], ref->key.id,
                ok ? "requested" : "FAILED");
            return std::snprintf(msg, cap, "%s 0x%06X: %s", vwire::KindName(mv->key.kind), mv->key.id, ok ? "requested" : "failed"), ok;
        }
        case 11: {
            // every shared vehicle within 300 m of our Sam: its life (+0x6E8 / +0x6EC), owner-authoritative (M2.8), for the
            // cross-world comparison of the hidden runs
            vshare::Unit units[64];
            const std::size_t n = vshare::SharedUnits(units, 64);
            WorldTransform sx{};
            const bool have_sam = ReadXf(reinterpret_cast<std::uintptr_t>(g_in.local), sx);
            double sp[3] = {};
            if (have_sam) PosOf(sx, sp);
            char buf[900];
            std::size_t k = std::snprintf(buf, sizeof(buf), "life:");
            unsigned shown = 0;
            for (std::size_t i = 0; i < n && k < sizeof(buf) - 80; ++i) {
                const vshare::Unit& u = units[i];
                if (!u.entity || !u.has_xf) continue;
                double up[3];
                PosOf(u.xf, up);
                if (have_sam && Dist(up, sp) > 300) continue;
                float life = -1, max = -1;
                mem::Rd(u.entity + 0x6E8, life), mem::Rd(u.entity + 0x6EC, max);
                k += std::snprintf(buf + k, sizeof(buf) - k, " %s 0x%06X %.1f/%.1f at (%.1f, %.1f);", vwire::KindName(u.key.kind), u.key.id, life, max, up[0], up[1]);
                ++shown;
            }
            Log("[vhit] %s (%u vehicle(s))", buf, shown);
            std::snprintf(msg, cap, "%s", buf);
            return true;
        }
        default: return std::snprintf(msg, cap, "unknown op %d", op), false;
    }
}

}  // namespace st::vhit
