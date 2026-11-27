// Round 28 P4: the Odradek probe (odradek_probe=1), and O3: the partner's scan shown here (odradek_scan_sync=1)
// (odradek_sync.hpp). Logs "[odra] ...".
#include "odradek_sync.hpp"

#include <windows.h>

#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "automation.hpp"
#include "config.hpp"
#include "dev_tools.hpp"
#include "enemy_sync.hpp"
#include "game_actions.hpp"
#include "game_api.hpp"
#include "link.hpp"
#include "log.hpp"
#include "odra_rules.hpp"
#include "partner_source.hpp"
#include "shot_sync.hpp"

namespace st::odra {
namespace {
namespace mem = partner::mem;
namespace rules = odrarules;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---- addresses (every one byte-checked in Install)
constexpr std::uintptr_t kStateVa = 0x14282B530;      // FUN_14282B530(inventory, float dt): the scan state machine
constexpr std::uintptr_t kWaveXformVa = 0x141E0B020;  // (wave, const WorldTransform*): the wave's SetWorldTransform
constexpr std::uintptr_t kGetCompVa = 0x1422B2A60;    // (entity + 0x90, rtti*) -> component (a read-only binary search)
constexpr std::uintptr_t kSurveyRttiVa = 0x144DBE7D0; // DSOdradekSurveyEffectComponent
constexpr std::uintptr_t kSurveyVa = 0x1433123C0;     // (survey comp, const float4* origin): start the survey there
constexpr std::uintptr_t kDecalFindVa = 0x141A8F4E0;  // ([0x14578C120], rtti 0x1448779A0) -> DecalManager
constexpr std::uintptr_t kDecalRootVa = 0x14578C120, kDecalRttiVa = 0x1448779A0;
constexpr std::uintptr_t kPlayerSysVa = 0x147BE7980;  // DSPlayerSystem*: +0x205D8 the last scan origin f64[3], +0x205D4
// O3: the visible scan (the pulse's wave steps 0x14282BEC6..0x14282BF6D, the trigger's sensor writes 0x14282BA38..0x14282BB0B)
constexpr std::uintptr_t kWaveResetVa = 0x1432FC7A0;  // (wave): +0x298 = 1, +0x290 / +0x2A4 / +0x2AC / +0x2C2 / +0x2BD = 0
constexpr std::uintptr_t kSensorPosVa = 0x142F4B5A0;  // (sensor, const float4*): +0x50 = pos, +0x78 = 1
constexpr std::uintptr_t kSensorRadVa = 0x141DD08C0;  // (sensor, float): +0x70 = radius
constexpr std::uintptr_t kSensorDirVa = 0x142F4CF60;  // (sensor, const float4*, float angle): +0x60 = dir, +0x74 = angle
constexpr std::uintptr_t kSensor2GetVa = 0x142F40090; // () -> [0x147BFD940] (created there when null; the tail calls it every frame)
constexpr std::uintptr_t kYawVa = 0x1425E48D0;        // (const float* dir) -> the yaw (DSPlayerSystem +0x205F4)
constexpr std::uintptr_t kSensorVa = 0x147BFDFF0;     // the sensor object [I] (the trigger's r14, the tail's rdi)
constexpr std::uintptr_t kSensor2Va = 0x147BFD940;    // the second sensor block [I] (+0x5FB0 pos, +0x5FC0 dir, +0x5FD0 radius)
constexpr std::uintptr_t kFrontVa = 0x147BF09D8;      // +0x1B8 the wave front's 60-byte transform, +0x1F8 its radius

// ---- offsets
constexpr std::uintptr_t kPInv = 0x54C8, kPCfgOwner = 0x54C0;  // player -> inventory; player -> [+0x30] the Odradek config
constexpr std::uintptr_t kIOwner48 = 0x48, kIEnt58 = 0x58, kIComps60 = 0x60, kIState = 0x4850, kICharge = 0x4934, kIRange = 0x493C,
                         kIDir = 0x4880, kIReq = 0x4C79, kIWaveSel = 0x4C7C, kIAct = 0x4C7F, kIArm = 0x4C88, kIBody = 0x4CA0,
                         kIWave0 = 0x4EF8, kIWave1 = 0x4F00;
constexpr std::uintptr_t kRefState = 0x328, kRefFlags = 0x3C4;  // ref = entity + 0x20: entity +0x348 / +0x3E4
constexpr std::uintptr_t kW20 = 0x20, kW2AC = 0x2AC, kW2BD = 0x2BD, kW2C0 = 0x2C0, kW2C1 = 0x2C1, kW250 = 0x250, kW28C = 0x28C;
constexpr std::uintptr_t kIPulses = 0x4874;  // the pulse counter (its parity picks the shader slot _0 / _1)
constexpr std::uintptr_t kFront = 0x1B8;     // [kFrontVa] +0x1B8..+0x1F3
constexpr std::uintptr_t kPsOrigin = 0x205D8, kPsYaw = 0x205F4, kPsAngle = 0x205F8;
constexpr std::uintptr_t kS2Pos = 0x5FB0, kS2Dir = 0x5FC0, kS2Radius = 0x5FD0, kS2Angle = 0x5FD4, kS2On = 0x5FD8;
constexpr std::uintptr_t kCOwner = 0x48, kCRes = 0x30, kCOrigin = 0xC0, kCDir = 0xF0, kCAngle = 0x100, kCTimer = 0x228, kC22C = 0x22C,
                         kC244 = 0x244, kC245 = 0x245, kC246 = 0x246;
constexpr std::uintptr_t kDecalCentre = 0xE0;

// ---- state
bool g_probe = false;
std::atomic<bool> g_installed{false};
unsigned g_checks_ok = 0, g_checks_n = 0;
using FnState = std::uintptr_t (*)(std::uintptr_t, float, std::uintptr_t, std::uintptr_t);
FnState o_state = nullptr;
double g_qpc_ms = 0;  // QPC ticks per ms

std::atomic<std::uintptr_t> g_local_inv{0}, g_local_player{0};
std::atomic<std::uint64_t> g_calls{0}, g_local_calls{0}, g_contended{0}, g_dropped{0};
std::atomic<unsigned long> g_detour_tid{0}, g_tick_tid{0};
std::atomic<bool> g_busy{false};
std::atomic<bool> g_stopped{false};  // EmergencyStop: the detour passes through, no request is taken or queued
std::atomic<bool> g_watch{true};
std::atomic<int> g_mode{0};
std::atomic<std::uintptr_t> g_comp_seen{0};  // the last validated survey comp (for the dump)
// O3 (odradek_scan_sync=1)
bool g_sync = false;                                        // the key
std::atomic<bool> g_sync_fault{false};                      // a guarded start faulted: the start path is off from then on
std::atomic<std::uint32_t> g_start_mask{rules::kStartAll};  // odra 6 <mask> (hidden runs: bisect the steps)

std::int64_t Qpc() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// ---- one frame's view of the machine (POD: filled inside __try)
struct Snap {
    std::uint8_t ok = 0;  // the inventory reads succeeded
    std::int32_t state = 0;
    std::uint8_t req = 0, sel = 0, act = 0;
    std::uintptr_t arm = 0, body = 0;
    std::uint32_t body_flags = 0;
    std::uint8_t body_state = 0, arm_state = 0;
    float charge = 0, dir[2] = {0, 0};
    std::uintptr_t wave = 0;
    std::uint8_t w2bd = 0, w2c0 = 0, w2c1 = 0;
    float w2ac = 0;
    double wpos[3] = {0, 0, 0};
    std::uintptr_t comp = 0;
    std::uint8_t c244 = 0, c245 = 0, c246 = 0;
    float c228 = 0, c0[3] = {0, 0, 0};
    std::uint8_t decal_ok = 0;
    double decal[3] = {0, 0, 0};
};

enum Kind : std::uint8_t { kChange = 1, kBeat, kScan, kReplay };
enum Changed : std::uint32_t {
    chState = 1, chSurvey = 2, chSurveyOrigin = 4, chWave = 8, chWaveFlags = 16, chDecal = 32, chComp = 64, chFirst = 128
};

struct Sample {
    std::uint8_t kind = 0, verdict = 0, mode = 0, moved = 0, fault = 0;
    std::uint32_t changed = 0;
    unsigned long tid = 0;
    std::int64_t qpc = 0;
    Snap prev, cur;
    double origin[3] = {0, 0, 0};  // the edge origin (DSPlayerSystem) or the replay origin
    std::uint32_t pulses = 0;
    double dist = 0;
    std::uint16_t seq = 0;
};

// single producer (the detour, serialized by g_busy), single consumer (Tick)
constexpr unsigned kRing = 256;
Sample g_ring[kRing];
std::atomic<unsigned> g_head{0}, g_tail{0};

void Push(const Sample& s) noexcept {
    const unsigned h = g_head.load(std::memory_order_relaxed);
    if (h - g_tail.load(std::memory_order_acquire) >= kRing) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_ring[h % kRing] = s;
    g_head.store(h + 1, std::memory_order_release);
}

// ---- O3: the partner's scan for the machine. Tick writes the payload while st is 0 and publishes 1; the detour takes 1 -> 2 and
// either puts it back (1: the machine is busy; the reason in g_remote_wait) or starts it and leaves the result (3) for Tick, which
// frees it (0). Tick retracts 1 -> 0 to replace it with a newer scan or to expire it. One slot: the latest scan wins.
struct RemoteReq {
    std::atomic<int> st{0};
    double origin[3] = {0, 0, 0};
    float dir[2] = {0, 1};
    std::uint16_t seq = 0;
    std::uint8_t test = 0;
    std::uint32_t mask = 0;
    // the detour's result (written while 2)
    std::uint8_t verdict = 0, fault = 0;
    std::uint32_t steps = 0, done = 0;
    std::int32_t state = 0;
    unsigned long tid = 0;
};
RemoteReq g_remote;
std::atomic<int> g_remote_wait{-1};  // verdict | state << 8, the last time the detour put the published scan back

// ---- O3: the local pulses for the sender (single producer: the detour, serialized by g_busy; single consumer: Tick)
struct OutScan {
    double origin[3];
    float dir[2];
};
constexpr unsigned kOut = 8;
OutScan g_out[kOut];
std::atomic<unsigned> g_out_head{0}, g_out_tail{0};
std::atomic<std::uint64_t> g_out_dropped{0};

void PushOut(const OutScan& o) noexcept {
    const unsigned h = g_out_head.load(std::memory_order_relaxed);
    if (h - g_out_tail.load(std::memory_order_acquire) >= kOut) {
        g_out_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_out[h % kOut] = o;
    g_out_head.store(h + 1, std::memory_order_release);
}

// ---- the requests (Command writes the payload while the slot is free, then publishes 1; the detour takes 1 -> 2 and frees
// it; Tick expires 1 -> 0 after 3 s)
struct ReplayReq {
    std::atomic<int> st{0};
    double origin[3] = {0, 0, 0};
    game::WorldTransform xf{};
    int mode = 0;
    double dist = 0;
    std::uint16_t seq = 0;
    double t_ms = 0;
};
ReplayReq g_replay;
std::atomic<int> g_scan_st{0};
double g_scan_t_ms = 0;
std::uint16_t g_next_seq = 1;
// detour-side
Snap g_prev{};
bool g_have_prev = false;
std::int64_t g_last_beat = 0, g_last_accept = 0;
bool g_have_accept = false, g_have_seq = false;
std::uint16_t g_last_seq = 0;
std::uintptr_t g_comp_ok = 0;  // the comp whose RTTI was checked

// ---- guarded reads and calls (POD only inside __try; the seh_guard detour leaves these faults to our __except)
template <class T>
T V(std::uintptr_t a) noexcept {
    return *reinterpret_cast<volatile T*>(a);
}

bool SehSnapInv(std::uintptr_t inv, Snap* s) noexcept {
    __try {
        s->state = V<std::int32_t>(inv + kIState);
        s->req = V<std::uint8_t>(inv + kIReq);
        s->sel = V<std::uint8_t>(inv + kIWaveSel);
        s->act = V<std::uint8_t>(inv + kIAct);
        s->charge = V<float>(inv + kICharge);
        s->dir[0] = V<float>(inv + kIDir), s->dir[1] = V<float>(inv + kIDir + 4);
        s->arm = V<std::uintptr_t>(inv + kIArm);
        s->body = V<std::uintptr_t>(inv + kIBody);
        if (s->arm) s->arm_state = V<std::uint8_t>(s->arm + kRefState);
        if (s->body) s->body_state = V<std::uint8_t>(s->body + kRefState), s->body_flags = V<std::uint32_t>(s->body + kRefFlags);
        s->wave = V<std::uintptr_t>(inv + ((s->sel & 0x40) ? kIWave1 : kIWave0));
        if (s->wave) {
            s->w2ac = V<float>(s->wave + kW2AC);
            s->w2bd = V<std::uint8_t>(s->wave + kW2BD);
            s->w2c0 = V<std::uint8_t>(s->wave + kW2C0);
            s->w2c1 = V<std::uint8_t>(s->wave + kW2C1);
            for (int i = 0; i < 3; ++i) s->wpos[i] = V<double>(s->wave + kW20 + 8 * i);
        }
        s->ok = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s->ok = 0;
        return false;
    }
}

// The survey comp, as the pulse finds it: FUN_1422B2A60([inv + 0x60] + 0x90, DSOdradekSurveyEffectComponent), owner checked.
std::uintptr_t SehComp(std::uintptr_t inv) noexcept {
    __try {
        const std::uintptr_t owner = V<std::uintptr_t>(inv + kIComps60);
        if (!owner) return 0;
        const std::uintptr_t c =
            reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uintptr_t)>(A(kGetCompVa))(owner + 0x90, A(kSurveyRttiVa));
        if (!c || V<std::uintptr_t>(c + kCOwner) != owner) return 0;
        return c;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool SehSnapComp(std::uintptr_t c, Snap* s) noexcept {
    __try {
        s->c244 = V<std::uint8_t>(c + kC244);
        s->c245 = V<std::uint8_t>(c + kC245);
        s->c246 = V<std::uint8_t>(c + kC246);
        s->c228 = V<float>(c + kCTimer);
        for (int i = 0; i < 3; ++i) s->c0[i] = V<float>(c + kCOrigin + 4 * i);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// the DecalManager the machine writes (0x14282C768): its +0xE0 centre
bool SehDecal(Snap* s) noexcept {
    __try {
        const std::uintptr_t root = V<std::uintptr_t>(A(kDecalRootVa));
        if (!root) return false;
        const std::uintptr_t m =
            reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uintptr_t)>(A(kDecalFindVa))(root, A(kDecalRttiVa));
        if (!m) return false;
        for (int i = 0; i < 3; ++i) s->decal[i] = V<double>(m + kDecalCentre + 8 * i);
        s->decal_ok = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s->decal_ok = 0;
        return false;
    }
}

bool SehEdgeOrigin(double (&o)[3], std::uint32_t* pulses) noexcept {
    __try {
        const std::uintptr_t ps = V<std::uintptr_t>(A(kPlayerSysVa));
        if (!ps) return false;
        for (int i = 0; i < 3; ++i) o[i] = V<double>(ps + 0x205D8 + 8 * i);
        *pulses = V<std::uint32_t>(ps + 0x205D4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// the native trigger's writes (0x142805F80)
bool SehTrigger(std::uintptr_t inv) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t*>(inv + kIReq) = static_cast<std::uint8_t>(V<std::uint8_t>(inv + kIReq) | 0x20);
        *reinterpret_cast<volatile std::uint32_t*>(inv + kICharge) = 0x3ECCCCCDu;  // 0.4f
        *reinterpret_cast<volatile std::uint32_t*>(inv + kIRange) = 0x40800000u;   // 4.0f
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// mode 1: the finished wave moved to the origin first (FUN_141E0B020; never +0x2C0 / +0x2C1)
bool SehWaveMove(std::uintptr_t wave, const game::WorldTransform* xf) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, const game::WorldTransform*)>(A(kWaveXformVa))(wave, xf);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// the pulse's survey steps (0x14282C000..0x14282C075): +0x246 = 1, +0xF0 = dir, FUN_1433123C0(comp, origin)
bool SehSurvey(std::uintptr_t c, const float* dir4, const float* origin4) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t*>(c + kC246) = 1;
        for (int i = 0; i < 4; ++i) *reinterpret_cast<volatile float*>(c + kCDir + 4 * i) = dir4[i];
        reinterpret_cast<void (*)(std::uintptr_t, const float*)>(A(kSurveyVa))(c, origin4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// O3: the scan shown at another origin (odra_rules.hpp kStart*), by `steps`: the pulse's wave steps (0x14282BEC6..0x14282BF5E),
// the trigger's sensor writes (0x14282BA93..0x14282BB0B), the pulse's wave front (0x14282BF66), DSPlayerSystem (0x14282C07A..
// 0x14282C0C7, never +0x205D4) and survey steps. Never the AI event (0x14282BB50), the +0x4868 stimulus timer, FUN_14284A7D0,
// the arm / body animation bits, the arm's +0x4D8, the hit gathers or the state. `done` gets the steps that ran.
bool SehStartRemote(std::uintptr_t inv, std::uintptr_t wave, std::uintptr_t comp, const game::WorldTransform* xf, const float* dir4,
                    std::uint32_t steps, std::uint32_t* done) noexcept {
    __try {
        alignas(16) float pos4[4] = {static_cast<float>(xf->pos.x), static_cast<float>(xf->pos.y), static_cast<float>(xf->pos.z), 0.0f};
        if (steps & rules::kStartEmitDir) {
            *reinterpret_cast<volatile float*>(inv + kIDir) = dir4[0];
            *reinterpret_cast<volatile float*>(inv + kIDir + 4) = dir4[1];
            *done |= rules::kStartEmitDir;
        }
        if (steps & rules::kStartSlot) {
            *reinterpret_cast<volatile std::uint32_t*>(inv + kIPulses) = V<std::uint32_t>(inv + kIPulses) + 1u;
            *done |= rules::kStartSlot;
        }
        if (steps & rules::kStartWave) {
            reinterpret_cast<void (*)(std::uintptr_t, const game::WorldTransform*)>(A(kWaveXformVa))(wave, xf);
            *reinterpret_cast<volatile std::uint32_t*>(wave + kW28C) = 0x4016CBE4u;  // kScanAngleRad
            for (int i = 0; i < 4; ++i) *reinterpret_cast<volatile float*>(wave + kW250 + 4 * i) = dir4[i];
            *reinterpret_cast<volatile std::uint8_t*>(wave + kW2C0) = 1;
            reinterpret_cast<void (*)(std::uintptr_t)>(A(kWaveResetVa))(wave);
            *reinterpret_cast<volatile std::uint8_t*>(wave + kW2C1) = 1;
            *done |= rules::kStartWave;
        }
        if (steps & rules::kStartSensor) {
            const std::uintptr_t g = V<std::uintptr_t>(A(kSensorVa));
            if (g) {
                reinterpret_cast<void (*)(std::uintptr_t, const float*)>(A(kSensorPosVa))(g, pos4);
                reinterpret_cast<void (*)(std::uintptr_t, float)>(A(kSensorRadVa))(g, 0.0f);
                reinterpret_cast<void (*)(std::uintptr_t, const float*, float)>(A(kSensorDirVa))(g, dir4, rules::kScanAngleRad);
            }
            const std::uintptr_t g2 = V<std::uintptr_t>(A(kSensor2Va));
            if (g2) {
                for (int i = 0; i < 4; ++i) *reinterpret_cast<volatile float*>(g2 + kS2Pos + 4 * i) = pos4[i];
                *reinterpret_cast<volatile std::uint8_t*>(g2 + kS2On) = 1;
                *reinterpret_cast<volatile float*>(g2 + kS2Radius) = 0.0f;
                for (int i = 0; i < 4; ++i) *reinterpret_cast<volatile float*>(g2 + kS2Dir + 4 * i) = dir4[i];
                *reinterpret_cast<volatile float*>(g2 + kS2Angle) = rules::kScanAngleRad;
            }
            if (g || g2) *done |= rules::kStartSensor;
        }
        if (steps & rules::kStartFront) {
            const std::uintptr_t f = V<std::uintptr_t>(A(kFrontVa));
            if (f) {
                const auto* src = reinterpret_cast<const std::uint32_t*>(xf);
                for (int i = 0; i < 15; ++i) *reinterpret_cast<volatile std::uint32_t*>(f + kFront + 4 * i) = src[i];  // 0x3C bytes
                *done |= rules::kStartFront;
            }
        }
        if (steps & rules::kStartSystem) {
            const std::uintptr_t ps = V<std::uintptr_t>(A(kPlayerSysVa));
            if (ps) {
                *reinterpret_cast<volatile double*>(ps + kPsOrigin) = xf->pos.x;
                *reinterpret_cast<volatile double*>(ps + kPsOrigin + 8) = xf->pos.y;
                *reinterpret_cast<volatile double*>(ps + kPsOrigin + 16) = xf->pos.z;
                *reinterpret_cast<volatile float*>(ps + kPsAngle) = rules::kScanAngleRad;
                const float yaw = reinterpret_cast<float (*)(const float*)>(A(kYawVa))(dir4);
                *reinterpret_cast<volatile float*>(ps + kPsYaw) = yaw;
                *done |= rules::kStartSystem;
            }
        }
        if ((steps & rules::kStartSurvey) && comp) {
            *reinterpret_cast<volatile std::uint8_t*>(comp + kC246) = 1;
            for (int i = 0; i < 4; ++i) *reinterpret_cast<volatile float*>(comp + kCDir + 4 * i) = dir4[i];
            reinterpret_cast<void (*)(std::uintptr_t, const float*)>(A(kSurveyVa))(comp, pos4);
            *done |= rules::kStartSurvey;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Moved(const float (&a)[3], const float (&b)[3], float m) noexcept {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz > m * m;
}
bool Moved(const double (&a)[3], const double (&b)[3], double m) noexcept {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz > m * m;
}

// The full snap after the machine ran (the comp found and validated once per pointer; the decal only while a scan or a
// survey runs, where the machine writes it).
void Fill(std::uintptr_t inv, Snap* s) noexcept {
    SehSnapInv(inv, s);
    s->comp = SehComp(inv);
    if (s->comp && s->comp != g_comp_ok) {
        if (mem::DerivesFrom(mem::RttiOf(s->comp), "DSOdradekSurveyEffectComponent")) g_comp_ok = s->comp;
        else s->comp = 0;
    }
    if (s->comp) {
        g_comp_seen.store(s->comp, std::memory_order_relaxed);
        SehSnapComp(s->comp, s);
    }
    if (g_watch.load(std::memory_order_relaxed) && (s->state != 0 || rules::SurveyActive(s->c244, s->c245))) SehDecal(s);
}

std::uint32_t Diff(const Snap& a, const Snap& b) noexcept {
    std::uint32_t c = 0;
    if (a.state != b.state) c |= chState;
    if (a.c244 != b.c244 || a.c245 != b.c245 || a.c246 != b.c246) c |= chSurvey;
    if (Moved(a.c0, b.c0, 0.1f)) c |= chSurveyOrigin;
    if (a.wave != b.wave) c |= chWave;
    if (a.w2bd != b.w2bd || a.w2c1 != b.w2c1 || a.w2c0 != b.w2c0) c |= chWaveFlags;
    if (b.decal_ok && (!a.decal_ok || Moved(a.decal, b.decal, 0.5))) c |= chDecal;
    if (a.comp != b.comp) c |= chComp;
    return c;
}

void PreRun(std::uintptr_t inv) noexcept {
    int expect = 1;
    if (!g_scan_st.compare_exchange_strong(expect, 2, std::memory_order_acquire)) return;
    Sample out;
    out.kind = kScan, out.tid = GetCurrentThreadId(), out.qpc = Qpc();
    SehSnapInv(inv, &out.cur);
    rules::TriggerIn t;
    t.state = out.cur.state, t.arm_ref = out.cur.arm != 0, t.body_ref = out.cur.body != 0, t.activated = (out.cur.act & 1) != 0;
    t.body_ready = out.cur.body && rules::BodyReady(out.cur.body_flags, out.cur.body_state);
    t.wave = out.cur.wave != 0;
    rules::TriggerVerdict v = out.cur.ok ? rules::ScanRequest(t) : rules::TriggerVerdict::no_odradek;
    if (v == rules::TriggerVerdict::accept && !SehTrigger(inv)) out.fault = 1;
    out.verdict = static_cast<std::uint8_t>(v);
    Push(out);
    g_scan_st.store(0, std::memory_order_release);
}

// O3, after the machine ran: the published partner scan started here when the machine is idle (state 0: every step of the
// mask) or shows marks (state 5: the mask less the DSPlayerSystem writes), else put back with the reason (odra_rules.hpp
// RemoteStart). The machine's own tail 0x14282C542 draws it from the next run on (the wave's +0x2AC grows, +0x2BD ends it).
bool StartPathOn() noexcept { return (g_sync || g_probe) && !g_sync_fault.load(std::memory_order_relaxed); }

void RemoteRun(std::uintptr_t inv, Snap& s) noexcept {
    int expect = 1;
    if (!g_remote.st.compare_exchange_strong(expect, 2, std::memory_order_acquire)) return;
    rules::StartIn in;
    in.ok = s.ok != 0, in.state = s.state, in.wave = s.wave != 0, in.wave_done = s.w2bd != 0;
    in.survey_active = rules::SurveyActive(s.c244, s.c245);
    const rules::StartVerdict v = rules::RemoteStart(in);
    if (rules::Waits(v)) {
        g_remote_wait.store(static_cast<int>(v) | ((s.state & 0xFF) << 8), std::memory_order_relaxed);
        g_remote.st.store(1, std::memory_order_release);
        return;
    }
    g_remote.verdict = static_cast<std::uint8_t>(v), g_remote.state = s.state, g_remote.tid = GetCurrentThreadId();
    g_remote.steps = rules::StartSteps(v, g_remote.mask), g_remote.done = 0, g_remote.fault = 0;
    if (rules::Started(v)) {
        game::WorldTransform xf{};
        xf.pos.x = g_remote.origin[0], xf.pos.y = g_remote.origin[1], xf.pos.z = g_remote.origin[2];
        rules::RotFromDir(g_remote.dir[0], g_remote.dir[1], xf.rot.m);
        alignas(16) float dir4[4] = {g_remote.dir[0], g_remote.dir[1], 0.0f, 0.0f};
        if (!SehStartRemote(inv, s.wave, s.comp, &xf, dir4, g_remote.steps, &g_remote.done)) {
            g_remote.fault = 1;
            g_sync_fault.store(true);
        }
        Fill(inv, &s);  // the snap after the start (the watch's next diff sees the wave restarted)
    }
    g_remote.st.store(3, std::memory_order_release);
}

void PostRun(std::uintptr_t inv) noexcept {
    const std::int64_t now = Qpc();
    Snap s;
    Fill(inv, &s);
    int expect = 1;
    if (g_replay.st.compare_exchange_strong(expect, 2, std::memory_order_acquire)) {
        Sample out;
        out.kind = kReplay, out.tid = GetCurrentThreadId(), out.qpc = now, out.prev = s;
        for (int i = 0; i < 3; ++i) out.origin[i] = g_replay.origin[i];
        out.mode = static_cast<std::uint8_t>(g_replay.mode), out.dist = g_replay.dist, out.seq = g_replay.seq;
        rules::ScanIn in;
        in.state = s.state, in.b244 = s.c244, in.b245 = s.c245, in.wave = s.wave != 0, in.comp = s.comp != 0;
        for (int i = 0; i < 3; ++i) in.origin[i] = g_replay.origin[i];
        in.dist_m = g_replay.dist;
        in.since_last_ms = g_have_accept && g_qpc_ms > 0 ? static_cast<double>(now - g_last_accept) / g_qpc_ms : 1e9;
        in.have_last_seq = g_have_seq, in.seq = g_replay.seq, in.last_seq = g_last_seq;
        const rules::ScanVerdict v = s.ok ? rules::AcceptScan(in) : rules::ScanVerdict::no_comp;
        out.verdict = static_cast<std::uint8_t>(v);
        if (v == rules::ScanVerdict::accept) {
            if (g_replay.mode == 1) {
                if (!s.w2bd) out.moved = 2;  // the wave is not finished: not moved
                else if (SehWaveMove(s.wave, &g_replay.xf)) out.moved = 1;
                else out.fault = 1;
            }
            alignas(16) float dir4[4];
            rules::SurveyDir(s.dir[0], s.dir[1], dir4);
            alignas(16) float o4[4] = {static_cast<float>(g_replay.origin[0]), static_cast<float>(g_replay.origin[1]),
                                       static_cast<float>(g_replay.origin[2]), 0.0f};
            if (!out.fault && !SehSurvey(s.comp, dir4, o4)) out.fault = 1;
            g_last_accept = now, g_have_accept = true, g_last_seq = g_replay.seq, g_have_seq = true;
            Fill(inv, &out.cur);
            s = out.cur;
        } else {
            out.cur = s;
        }
        Push(out);
        g_replay.st.store(0, std::memory_order_release);
    }
    if (StartPathOn()) RemoteRun(inv, s);
    // O3 sender: the local pulse (3 -> 4; only the game's own pulse makes it, never a partner scan shown here)
    if (g_sync && !g_sync_fault.load(std::memory_order_relaxed) && g_have_prev && rules::ScanEdge(g_prev.state, s.state)) {
        OutScan o{};
        std::uint32_t pulses = 0;
        o.dir[0] = s.dir[0], o.dir[1] = s.dir[1];
        if (SehEdgeOrigin(o.origin, &pulses)) PushOut(o);
        else g_out_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    if (!g_watch.load(std::memory_order_relaxed)) {
        g_prev = s, g_have_prev = true;
        return;
    }
    const std::uint32_t ch = g_have_prev ? Diff(g_prev, s) : chFirst;
    if (ch) {
        Sample out;
        out.kind = kChange, out.changed = ch, out.tid = GetCurrentThreadId(), out.qpc = now, out.prev = g_prev, out.cur = s;
        if ((ch & chState) && rules::ScanEdge(g_prev.state, s.state)) SehEdgeOrigin(out.origin, &out.pulses);
        Push(out);
        g_last_beat = now;
    } else if ((s.state != 0 || rules::SurveyActive(s.c244, s.c245)) && g_qpc_ms > 0 && (now - g_last_beat) > static_cast<std::int64_t>(500 * g_qpc_ms)) {
        Sample out;
        out.kind = kBeat, out.tid = GetCurrentThreadId(), out.qpc = now, out.prev = g_prev, out.cur = s;
        Push(out);
        g_last_beat = now;
    }
    g_prev = s, g_have_prev = true;
}

std::uintptr_t H_State(std::uintptr_t inv, float dt, std::uintptr_t r8, std::uintptr_t r9) {
    if (g_stopped.load(std::memory_order_relaxed)) return o_state(inv, dt, r8, r9);  // after the emergency stop: no game calls, no samples
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const std::uintptr_t local = g_local_inv.load(std::memory_order_acquire);
    if (!local || inv != local) return o_state(inv, dt, r8, r9);
    if (g_busy.exchange(true, std::memory_order_acquire)) {  // never two at once (one producer for the ring)
        g_contended.fetch_add(1, std::memory_order_relaxed);
        return o_state(inv, dt, r8, r9);
    }
    g_local_calls.fetch_add(1, std::memory_order_relaxed);
    g_detour_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    PreRun(inv);
    const std::uintptr_t r = o_state(inv, dt, r8, r9);
    PostRun(inv);
    g_busy.store(false, std::memory_order_release);
    return r;
}

// ---- the game-thread side
double g_cache_ms = -1e9;
std::uintptr_t g_cache_player = 0;
double g_last_change_ms = 0;
int g_cycle[16];
int g_cycle_n = 0;
bool g_replay_survey = false;  // a replay started the survey: log its end
double g_replay_done_ms = 0;
unsigned g_replay_done_seq = 0;  // the replay that started it (its end line names it: scenarios wait for one pass's end)
int g_replay_done_mode = 0;
double g_now_ms = 0;

double SampleMs(const Sample& s) noexcept { return g_qpc_ms > 0 ? static_cast<double>(s.qpc) / g_qpc_ms : 0.0; }

void CycleStep(int from, int to) noexcept {
    if (from == 0 && to != 0) g_cycle_n = 0, g_cycle[g_cycle_n++] = 0;
    if (g_cycle_n == 0) return;  // joined mid-cycle
    if (g_cycle_n < 16) g_cycle[g_cycle_n++] = to;
    if (to != 0) return;
    char path[96] = {};
    std::size_t at = 0;
    bool seen[8] = {};
    bool once = true;
    for (int i = 0; i < g_cycle_n && at + 4 < sizeof(path); ++i) {
        at += static_cast<std::size_t>(std::snprintf(path + at, sizeof(path) - at, i ? ">%d" : "%d", g_cycle[i]));
        if (i && i + 1 < g_cycle_n) {
            const int k = g_cycle[i];
            if (k < 0 || k > 7 || seen[k]) once = false;
            else seen[k] = true;
        }
    }
    const bool full = once && g_cycle_n == 7 && seen[1] && seen[2] && seen[3] && seen[4] && seen[5];
    Log("[odra] scan cycle %s %s", path, full ? "complete (0>1>2>3>4>5>0, each once)" : "(not the full cycle)");
    g_cycle_n = 0;
}

void LogSample(const Sample& s) noexcept {
    const Snap& a = s.prev;
    const Snap& b = s.cur;
    const double ms = SampleMs(s);
    if (s.kind == kScan) {
        const auto v = static_cast<rules::TriggerVerdict>(s.verdict);
        if (s.fault) Log("[odra] scan FAULT writing the trigger (tid %lu)", s.tid);
        else if (v == rules::TriggerVerdict::accept)
            Log("[odra] scan triggered in state %d (tid %lu; +0x4C79 |= 0x20, +0x4934 = 0.4, +0x493C = 4.0)", b.state, s.tid);
        else
            Log("[odra] scan refused: %s (state %d arm %p body %p act 0x%02X body state 0x%02X flags 0x%X wave %p)", rules::TriggerName(v),
                b.state, reinterpret_cast<void*>(b.arm), reinterpret_cast<void*>(b.body), static_cast<unsigned>(b.act), b.body_state,
                b.body_flags, reinterpret_cast<void*>(b.wave));
        return;
    }
    if (s.kind == kReplay) {
        const auto v = static_cast<rules::ScanVerdict>(s.verdict);
        if (v != rules::ScanVerdict::accept) {
            Log("[odra] replay refused: %s (seq %u origin %.2f %.2f %.2f dist %.1f m; state %d survey 244=%u 245=%u wave %p comp %p)",
                rules::ScanName(v), s.seq, s.origin[0], s.origin[1], s.origin[2], s.dist, a.state, a.c244, a.c245,
                reinterpret_cast<void*>(a.wave), reinterpret_cast<void*>(a.comp));
            return;
        }
        if (s.fault) {
            Log("[odra] replay FAULT (seq %u mode %d moved %u; comp %p wave %p)", s.seq, s.mode, s.moved, reinterpret_cast<void*>(a.comp),
                reinterpret_cast<void*>(a.wave));
            return;
        }
        Log("[odra] replay done seq %u origin %.2f %.2f %.2f dist %.1f m mode %d (wave %s) survey 244 %u -> %u 245 %u -> %u "
            "c0 %.2f %.2f %.2f t228 %.2f decal %s %.2f %.2f %.2f (tid %lu)",
            s.seq, s.origin[0], s.origin[1], s.origin[2], s.dist, s.mode,
            s.mode != 1 ? "not moved" : s.moved == 1 ? "moved" : "not finished, not moved", a.c244, b.c244, a.c245, b.c245, b.c0[0],
            b.c0[1], b.c0[2], b.c228, b.decal_ok ? "at" : "unread", b.decal[0], b.decal[1], b.decal[2], s.tid);
        if (b.c244 && !b.c245) g_replay_survey = true, g_replay_done_ms = ms, g_replay_done_seq = s.seq, g_replay_done_mode = s.mode;
        return;
    }
    if (s.kind == kBeat) {
        Log("[odra] beat state %d r %.1f t228 %.2f survey %u/%u c0 %.1f %.1f %.1f decal %s %.1f %.1f %.1f", b.state, b.w2ac, b.c228,
            b.c244, b.c245, b.c0[0], b.c0[1], b.c0[2], b.decal_ok ? "at" : "unread", b.decal[0], b.decal[1], b.decal[2]);
        return;
    }
    // kChange
    if (s.changed & chFirst) {
        Log("[odra] first sample (tid %lu): state %d wave %p 2bd=%u 2c0=%u 2c1=%u at %.1f %.1f %.1f survey comp %p 244=%u 245=%u 246=%u "
            "c0 %.1f %.1f %.1f",
            s.tid, b.state, reinterpret_cast<void*>(b.wave), b.w2bd, b.w2c0, b.w2c1, b.wpos[0], b.wpos[1], b.wpos[2],
            reinterpret_cast<void*>(b.comp), b.c244, b.c245, b.c246, b.c0[0], b.c0[1], b.c0[2]);
        g_last_change_ms = ms;
        return;
    }
    if (s.changed & chState) {
        Log("[odra] state %d -> %d (%.0f ms after the last change, tid %lu; wave %p r %.1f 2bd=%u 2c1=%u; survey 244=%u 245=%u "
            "t228 %.2f; charge %.2f arm 0x%02X body 0x%02X)",
            a.state, b.state, ms - g_last_change_ms, s.tid, reinterpret_cast<void*>(b.wave), b.w2ac, b.w2bd, b.w2c1, b.c244, b.c245,
            b.c228, b.charge, b.arm_state, b.body_state);
        if (rules::ScanEdge(a.state, b.state))
            Log("[odra] scan edge 3 -> 4: origin %.2f %.2f %.2f pulses %u (DSPlayerSystem +0x205D8 / +0x205D4); dir %.3f %.3f wave at "
                "%.2f %.2f %.2f",
                s.origin[0], s.origin[1], s.origin[2], s.pulses, b.dir[0], b.dir[1], b.wpos[0], b.wpos[1], b.wpos[2]);
        CycleStep(a.state, b.state);
        g_last_change_ms = ms;
    }
    if (s.changed & chSurvey) {
        Log("[odra] survey 244 %u -> %u 245 %u -> %u 246 %u -> %u (state %d; c0 %.2f %.2f %.2f t228 %.2f)", a.c244, b.c244, a.c245, b.c245,
            a.c246, b.c246, b.state, b.c0[0], b.c0[1], b.c0[2], b.c228);
        if (g_replay_survey && !rules::SurveyActive(b.c244, b.c245)) {
            Log("[odra] replay survey ended seq %u mode %d %.1f s after the replay (244 %u 245 %u)", g_replay_done_seq, g_replay_done_mode,
                (ms - g_replay_done_ms) / 1000.0, b.c244, b.c245);
            g_replay_survey = false;
        }
    }
    if ((s.changed & chSurveyOrigin) && !(s.changed & chSurvey))
        Log("[odra] survey origin %.2f %.2f %.2f -> %.2f %.2f %.2f", a.c0[0], a.c0[1], a.c0[2], b.c0[0], b.c0[1], b.c0[2]);
    if (s.changed & (chWave | chWaveFlags))
        Log("[odra] wave %p -> %p 2bd %u -> %u 2c0 %u -> %u 2c1 %u -> %u (state %d; at %.2f %.2f %.2f)", reinterpret_cast<void*>(a.wave),
            reinterpret_cast<void*>(b.wave), a.w2bd, b.w2bd, a.w2c0, b.w2c0, a.w2c1, b.w2c1, b.state, b.wpos[0], b.wpos[1], b.wpos[2]);
    if (s.changed & chDecal)
        Log("[odra] decal centre %.2f %.2f %.2f (state %d survey %u/%u)", b.decal[0], b.decal[1], b.decal[2], b.state, b.c244, b.c245);
    if (s.changed & chComp) Log("[odra] survey comp %p -> %p", reinterpret_cast<void*>(a.comp), reinterpret_cast<void*>(b.comp));
}

// ---- O3 on the game thread: the link (SOD1 HELLO / SCAN), the slot, the "[odra] partner scan ..." lines
std::uint32_t g_gen = 0;
bool g_have_gen = false;
std::uint8_t g_peer_caps = 0;
bool g_peer_hello = false;   // a HELLO arrived on this link generation
bool g_hello_sent = false;   // ours went out on this generation (with g_caps_sent)
bool g_announced = false;    // one of them carried the bit (a later caps 0 must follow)
std::uint8_t g_caps_sent = 0;
bool g_have_rx_seq = false;
std::uint16_t g_rx_seq = 0, g_tx_seq = 1;
double g_link_ms = -1e9;
bool g_connected = false;
// the published scan (st 1..3) and the one waiting for the slot
double g_pub_ms = 0, g_pub_dist = 0;
std::uint32_t g_pub_world = 0;  // our world fingerprint when the published scan was admitted
int g_pub_wait_logged = -1;
bool g_pub_counted = false;
struct Pend {
    bool have = false;
    rules::SodScan s;
    bool test = false;
    std::uint32_t mask = 0;
    double t_ms = 0, dist = 0;
    std::uint32_t world = 0;  // our world fingerprint when it was admitted
};
Pend g_next;
struct SyncCounts {
    unsigned hello_tx = 0, hello_rx = 0, sent = 0, send_fail = 0, not_sent = 0, received = 0, bad = 0, shown = 0, shown_full = 0,
             shown_visual = 0, queued = 0, dropped = 0, faults = 0, drained_off = 0, dropped_moved = 0;
} g_sc;

bool SyncOn() noexcept {
    return g_sync && g_installed.load(std::memory_order_relaxed) && !g_stopped.load(std::memory_order_relaxed) &&
           !g_sync_fault.load(std::memory_order_relaxed);
}

rules::SendIn SendGate() noexcept {
    rules::SendIn in;
    in.on = SyncOn(), in.connected = g_connected, in.channel_ok = shots::PeerBeacon(nullptr, nullptr);
    in.peer_cap = g_peer_hello && (g_peer_caps & rules::kCapOdradekScan);
    in.announced = g_announced;
    return in;
}

const char* NotSentWhy(const rules::SendIn& in) noexcept {
    if (!in.on) return "scan sync off here";
    if (!in.connected) return "no link";
    if (!in.channel_ok) return "no SEH1 beacon from the partner yet (its channel 6 unknown)";
    if (!in.peer_cap) return g_peer_hello ? "the partner's HELLO has no scan sync" : "no HELLO from the partner yet";
    return "?";
}

const char* g_not_sent_why = nullptr;  // the last "not sent" reason logged

// one SCAN to the partner (game thread): the local pulse, or odra 8's test pulse
void SendScan(const double (&o)[3], const float (&d)[2], std::uint8_t flags) noexcept {
    const rules::SendIn in = SendGate();
    if (!rules::MaySendScan(in)) {
        ++g_sc.not_sent;
        // players scan often (an older or key-off partner never says hello): the first 5 and each new reason logged, the rest
        // counted (the status line's "not sent")
        const char* why = NotSentWhy(in);
        if (g_sc.not_sent <= 5 || why != g_not_sent_why)
            Log("[odra] partner scan not sent: %s (origin=%.2f %.2f %.2f; not sent %u)", why, o[0], o[1], o[2], g_sc.not_sent);
        g_not_sent_why = why;
        return;
    }
    rules::SodScan sc;
    sc.seq = g_tx_seq, sc.flags = flags;
    for (int i = 0; i < 3; ++i) sc.origin[i] = o[i];
    sc.dir[0] = d[0], sc.dir[1] = d[1];
    sc.world = esync::Gate().guids;  // the receiver refuses a scan from another world (0: unknown, not compared)
    std::uint8_t buf[rules::kSodScanSize];
    const std::size_t n = rules::EncodeSodScan(sc, buf, sizeof(buf));
    if (!n) {
        ++g_sc.not_sent;
        Log("[odra] partner scan not sent: bad origin / dir (origin=%.2f %.2f %.2f dir=%.3f %.3f)", o[0], o[1], o[2], d[0], d[1]);
        return;
    }
    if (!link::SendCombat(buf, n)) {
        ++g_sc.send_fail;
        Log("[odra] partner scan not sent: the link refused it (channel 6 window full or not connected) seq=%u", sc.seq);
        return;
    }
    ++g_tx_seq;
    ++g_sc.sent;
    Log("[odra] partner scan sent seq=%u origin=%.2f %.2f %.2f dir=%.3f %.3f%s (sent %u)", sc.seq, o[0], o[1], o[2], d[0], d[1],
        (flags & rules::kSodFlagTest) ? " (test)" : "", g_sc.sent);
}

void DropLog(const char* why, std::uint16_t seq, const double (&o)[3], bool test) noexcept {
    ++g_sc.dropped;
    Log("[odra] partner scan dropped: %s seq=%u origin=%.2f %.2f %.2f%s (dropped %u)", why, seq, o[0], o[1], o[2], test ? " (test)" : "",
        g_sc.dropped);
}

// Tick only, while st is 0
void Publish(const Pend& p) noexcept {
    for (int i = 0; i < 3; ++i) g_remote.origin[i] = p.s.origin[i];
    g_remote.dir[0] = p.s.dir[0], g_remote.dir[1] = p.s.dir[1];
    g_remote.seq = p.s.seq, g_remote.test = p.test ? 1 : 0, g_remote.mask = p.mask;
    g_remote_wait.store(-1, std::memory_order_relaxed);
    g_pub_ms = p.t_ms, g_pub_dist = p.dist, g_pub_world = p.world, g_pub_wait_logged = -1, g_pub_counted = false;
    g_remote.st.store(1, std::memory_order_release);
}

void PendFromSlot(Pend* p) noexcept {
    p->have = true;
    for (int i = 0; i < 3; ++i) p->s.origin[i] = g_remote.origin[i];
    p->s.dir[0] = g_remote.dir[0], p->s.dir[1] = g_remote.dir[1];
    p->s.seq = g_remote.seq, p->test = g_remote.test != 0, p->mask = g_remote.mask, p->t_ms = g_pub_ms, p->dist = g_pub_dist;
    p->world = g_pub_world;
}

// A partner scan waiting here, re-checked each tick until the detour takes it: a load, another world fingerprint, or the local
// player out of range drops it (it must not start at the old origin after a load or fast travel).
rules::KeepVerdict KeepCheck(const double (&o)[3], std::uint32_t world_at) noexcept {
    rules::KeepIn k;
    std::uint64_t flags = 0;
    std::uint32_t mask = 0;
    k.loading = game::ReadStopGates(&flags, &mask) && mask != 0;
    k.world_at = world_at, k.world_now = esync::Gate().guids;
    game::Entity* p = nullptr;
    game::WorldTransform wt{};
    k.have_player = game::TryGetLocalPlayer(&p) && p && game::TryGetTransform(p, &wt);
    if (k.have_player) {
        const double dx = o[0] - wt.pos.x, dy = o[1] - wt.pos.y, dz = o[2] - wt.pos.z;
        k.dist_m = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return rules::KeepPending(k);
}

// A received (or odra 5's) scan: admitted, then published, or kept for the slot (latest wins).
void OnScan(const rules::SodScan& sc, bool test, std::uint32_t mask, double now_ms) noexcept {
    game::Entity* p = nullptr;
    game::WorldTransform wt{};
    rules::AdmitIn in;
    in.on = test ? (g_installed.load() && !g_stopped.load() && !g_sync_fault.load()) : SyncOn();
    in.peer_cap = g_peer_hello && (g_peer_caps & rules::kCapOdradekScan);
    in.test = test;
    in.have_player = game::TryGetLocalPlayer(&p) && p && game::TryGetTransform(p, &wt);
    const double dx = sc.origin[0] - wt.pos.x, dy = sc.origin[1] - wt.pos.y, dz = sc.origin[2] - wt.pos.z;
    in.dist_m = std::sqrt(dx * dx + dy * dy + dz * dz);
    in.have_last = g_have_rx_seq, in.seq = sc.seq, in.last_seq = g_rx_seq;
    const std::uint32_t our_world = esync::Gate().guids;
    in.world = sc.world, in.our_world = our_world;
    const rules::AdmitVerdict v = rules::AdmitRemote(in);
    if (v != rules::AdmitVerdict::queue) {
        DropLog(rules::AdmitName(v), sc.seq, sc.origin, test);
        return;
    }
    if (!test) g_have_rx_seq = true, g_rx_seq = sc.seq;
    Log("[odra] partner scan received seq=%u origin=%.2f %.2f %.2f dir=%.3f %.3f dist=%.1f m%s", sc.seq, sc.origin[0], sc.origin[1],
        sc.origin[2], sc.dir[0], sc.dir[1], in.dist_m, test ? " (test)" : "");
    Pend np;
    np.have = true, np.s = sc, np.test = test, np.mask = mask, np.t_ms = now_ms, np.dist = in.dist_m, np.world = our_world;
    int pub = 1;
    if (g_remote.st.load(std::memory_order_acquire) == 0) {
        Publish(np);
    } else if (g_remote.st.compare_exchange_strong(pub, 0, std::memory_order_acq_rel)) {
        Pend old;
        PendFromSlot(&old);
        if (rules::Supersedes(test, old.test, sc.seq, old.s.seq)) {
            DropLog("superseded (a newer scan arrived)", old.s.seq, old.s.origin, old.test);
            Publish(np);
        } else {
            const int logged = g_pub_wait_logged;
            const bool counted = g_pub_counted;
            Publish(old);
            g_pub_wait_logged = logged, g_pub_counted = counted;
            DropLog("stale (an older seq than the queued one)", sc.seq, sc.origin, test);
        }
    } else {
        // the detour holds the slot (or its result waits for this tick): the scan waits here
        if (g_next.have) DropLog("superseded (a newer scan arrived)", g_next.s.seq, g_next.s.origin, g_next.test);
        g_next = np;
    }
}

// the partner's scans waiting here (not odra 5's tests) dropped
void DropPartnerPending(const char* why) noexcept {
    int pub = 1;
    if (g_remote.st.load() == 1 && !g_remote.test && g_remote.st.compare_exchange_strong(pub, 0)) DropLog(why, g_remote.seq, g_remote.origin, false);
    if (g_next.have && !g_next.test) DropLog(why, g_next.s.seq, g_next.s.origin, false), g_next.have = false;
}

void SyncReset(const char* why) noexcept {
    g_peer_caps = 0, g_peer_hello = false, g_hello_sent = false, g_announced = false, g_caps_sent = 0;
    g_have_rx_seq = false;
    DropPartnerPending(why);
}

void SyncTick(double now_ms) noexcept {
    const bool on = SyncOn();
    // the link: its generation every tick, its state every 0.5 s
    const std::uint32_t gen = link::Generation();
    // only a newer generation resets (a message of the new one may already have moved g_gen on: see the receive loop)
    if (!g_have_gen || static_cast<std::int32_t>(gen - g_gen) > 0) {
        if (g_have_gen && g_sync) Log("[odra] scan sync: link generation %u -> %u (the partner's HELLO is awaited again)", g_gen, gen);
        if (g_have_gen) SyncReset("new link generation");
        g_gen = gen, g_have_gen = true;
    }
    if (now_ms - g_link_ms > 500.0) {
        g_link_ms = now_ms;
        g_connected = link::GetInfo().connected;
    }
    // receive (the key off: drained unread, so nothing piles up; the partner never sends scans to a game that sent no HELLO)
    std::uint8_t buf[link::kOdradekMax];
    std::size_t n = 0;
    std::uint32_t mgen = 0;
    for (int k = 0; k < 16 && link::PopOdradek(buf, sizeof(buf), n, mgen); ++k) {
        if (!g_sync) {
            ++g_sc.drained_off;
            continue;
        }
        // the message's generation is the authority: one of a newer session (queued before Generation() moved on) starts that
        // session here first, so its once-only HELLO is not lost; only older ones are dropped
        if (static_cast<std::int32_t>(mgen - g_gen) < 0) continue;
        if (mgen != g_gen) {
            Log("[odra] scan sync: link generation %u -> %u (from a message of the new session)", g_gen, mgen);
            SyncReset("new link generation");
            g_gen = mgen;
        }
        std::uint8_t caps = 0;
        rules::SodScan sc;
        if (rules::DecodeSodHello(buf, n, &caps)) {
            ++g_sc.hello_rx;
            const bool was = g_peer_hello && (g_peer_caps & rules::kCapOdradekScan);
            g_peer_caps = caps, g_peer_hello = true;
            const bool now_cap = (caps & rules::kCapOdradekScan) != 0;
            Log("[odra] partner hello caps=0x%02X: the partner %s (here %s)", caps,
                now_cap ? "shows our scans and sends its own" : "has no scan sync (odradek_scan_sync=0 there, or its detour is off)",
                on ? "on" : "off");
            if (was && !now_cap) DropPartnerPending("the partner's scan sync went off");
        } else if (rules::DecodeSodScan(buf, n, sc)) {
            ++g_sc.received;
            OnScan(sc, false, g_start_mask.load(), now_ms);
        } else {
            ++g_sc.bad;
            if (g_sc.bad <= 5) Log("[odra] partner message refused: not a valid SOD1 HELLO / SCAN (%zu bytes, bad %u)", n, g_sc.bad);
        }
    }
    if (!g_sync) return;
    // our HELLO: once per link generation, and on a change (a fault turns the bit off)
    const rules::SendIn gate = SendGate();
    const std::uint8_t caps = on ? rules::kCapOdradekScan : 0;
    if (rules::MaySendHello(gate) && (!g_hello_sent || caps != g_caps_sent)) {
        std::uint8_t h[rules::kSodHelloSize];
        if (rules::EncodeSodHello(caps, h, sizeof(h)) && link::SendCombat(h, sizeof(h))) {
            g_hello_sent = true, g_caps_sent = caps;
            if (caps) g_announced = true;
            ++g_sc.hello_tx;
            Log("[odra] hello sent caps=0x%02X (link generation %u)", caps, g_gen);
        }
    }
    // the local pulses
    unsigned t = g_out_tail.load(std::memory_order_relaxed);
    const unsigned h = g_out_head.load(std::memory_order_acquire);
    for (; t != h; ++t) {
        const OutScan o = g_out[t % kOut];
        SendScan(o.origin, o.dir, 0);
    }
    g_out_tail.store(t, std::memory_order_release);
}

// the slot (game thread; probe and sync alike): results, waits, expiry, the next scan
void SlotTick(double now_ms) noexcept {
    const int st = g_remote.st.load(std::memory_order_acquire);
    if (st == 3) {
        const auto v = static_cast<rules::StartVerdict>(g_remote.verdict);
        if (g_remote.fault) {
            ++g_sc.faults;
            Log("[odra] partner scan FAULT seq=%u (steps 0x%X, done 0x%X; tid %lu): the start path is off from now on", g_remote.seq,
                g_remote.steps, g_remote.done, g_remote.tid);
        } else if (rules::Started(v)) {
            ++g_sc.shown;
            if (v == rules::StartVerdict::start_full) ++g_sc.shown_full;
            else ++g_sc.shown_visual;
            Log("[odra] partner scan shown origin=%.2f %.2f %.2f seq=%u mode=%s steps=0x%X dist=%.1f m state=%d after=%.1f s tid=%lu%s "
                "(shown %u)",
                g_remote.origin[0], g_remote.origin[1], g_remote.origin[2], g_remote.seq, rules::StartName(v), g_remote.done, g_pub_dist,
                g_remote.state, (now_ms - g_pub_ms) / 1000.0, g_remote.tid, g_remote.test ? " (test)" : "", g_sc.shown);
        } else {
            DropLog(rules::StartName(v), g_remote.seq, g_remote.origin, g_remote.test != 0);
        }
        g_remote.st.store(0, std::memory_order_release);
    } else if (st == 1) {
        if (!g_remote.test) {
            const rules::KeepVerdict kv = KeepCheck(g_remote.origin, g_pub_world);
            int pub = 1;
            if (kv != rules::KeepVerdict::keep && g_remote.st.compare_exchange_strong(pub, 0)) {
                ++g_sc.dropped_moved;
                DropLog(rules::KeepName(kv), g_remote.seq, g_remote.origin, false);
            }
        }
        const int w = g_remote_wait.load(std::memory_order_relaxed);
        if (w >= 0 && w != g_pub_wait_logged) {
            g_pub_wait_logged = w;
            if (!g_pub_counted) ++g_sc.queued, g_pub_counted = true;
            Log("[odra] partner scan queued: %s seq=%u (local state %d; shown when the machine is idle; queued %u)",
                rules::StartName(static_cast<rules::StartVerdict>(w & 0xFF)), g_remote.seq, (w >> 8) & 0xFF, g_sc.queued);
        }
        int pub = 1;
        if (g_remote.st.load(std::memory_order_acquire) == 1 && rules::PendingExpired(now_ms - g_pub_ms) &&
            g_remote.st.compare_exchange_strong(pub, 0)) {
            DropLog(w >= 0 ? "stale (the machine stayed busy for 10 s)" : "stale (the machine did not run for 10 s)", g_remote.seq,
                    g_remote.origin, g_remote.test != 0);
        }
    }
    if (g_next.have && !g_next.test) {
        const rules::KeepVerdict kv = KeepCheck(g_next.s.origin, g_next.world);
        if (kv != rules::KeepVerdict::keep) {
            ++g_sc.dropped_moved;
            DropLog(rules::KeepName(kv), g_next.s.seq, g_next.s.origin, false);
            g_next.have = false;
        }
    }
    if (g_next.have) {
        if (rules::PendingExpired(now_ms - g_next.t_ms)) {
            DropLog("stale (waited 10 s for the slot)", g_next.s.seq, g_next.s.origin, g_next.test);
            g_next.have = false;
        } else {
            // the slot is free, or still holds an older queued scan (the detour had it when g_next came): the newer one wins
            int pub = 1;
            if (g_remote.st.load(std::memory_order_acquire) == 1 && g_remote.st.compare_exchange_strong(pub, 0, std::memory_order_acq_rel)) {
                Pend old;
                PendFromSlot(&old);
                if (rules::Supersedes(g_next.test, old.test, g_next.s.seq, old.s.seq)) {
                    DropLog("superseded (a newer scan arrived)", old.s.seq, old.s.origin, old.test);
                } else {
                    DropLog("stale (an older seq than the queued one)", g_next.s.seq, g_next.s.origin, g_next.test);
                    g_next = old;
                }
            }
            if (g_remote.st.load(std::memory_order_acquire) == 0) {
                Publish(g_next);
                g_next.have = false;
            }
        }
    }
}

void SyncLine() noexcept {
    std::uint64_t link_dropped = 0;
    link::OdradekCounts(nullptr, &link_dropped);
    Log("[odra] scan sync: key %d, start path %s (mask 0x%X), here caps 0x%02X, partner %s caps 0x%02X, link %s gen %u, channel 6 %s; "
        "hello tx %u rx %u, sent %u (refused %u, not sent %u), received %u (bad %u), shown %u (full %u, visual %u), queued %u, "
        "dropped %u, faults %u, outbox dropped %llu; rechecked drops %u (a load, another world, out of range); link drops %llu (oversize %llu)",
        g_sync ? 1 : 0, StartPathOn() ? "on" : "off", g_start_mask.load(), SyncOn() ? rules::kCapOdradekScan : 0,
        g_peer_hello ? "hello" : "no hello", g_peer_caps, g_connected ? "up" : "down", g_gen,
        shots::PeerBeacon(nullptr, nullptr) ? "known (SEH1 seen)" : "unknown", g_sc.hello_tx, g_sc.hello_rx, g_sc.sent, g_sc.send_fail,
        g_sc.not_sent, g_sc.received, g_sc.bad, g_sc.shown, g_sc.shown_full, g_sc.shown_visual, g_sc.queued, g_sc.dropped, g_sc.faults,
        static_cast<unsigned long long>(g_out_dropped.load()), g_sc.dropped_moved, static_cast<unsigned long long>(link_dropped),
        static_cast<unsigned long long>(link::OdradekOversize()));
}

// sync_scan: op 2 (this game's own scan, the native trigger) also with odradek_scan_sync alone in a hidden run, so the shipped
// config's sender is driven by the defaults scenario (two-games-round28-defaults)
bool MayAct(char* msg, std::size_t cap, bool sync_scan = false) noexcept {
    if (!g_probe && !(sync_scan && g_sync && automation::Enabled())) return std::snprintf(msg, cap, "odradek_probe is off"), false;
    if (g_stopped.load()) return std::snprintf(msg, cap, "the [odra] probe is stopped (emergency stop)"), false;
    if (!g_installed.load()) return std::snprintf(msg, cap, "the [odra] detour is not installed (byte checks %u/%u)", g_checks_ok, g_checks_n), false;
    if (!automation::Enabled() && !dev::Enabled()) return std::snprintf(msg, cap, "scan / replay: hidden runs or dev_tools only"), false;
    if (!g_local_inv.load()) return std::snprintf(msg, cap, "no local inventory yet"), false;
    return true;
}

bool QueueReplay(const double (&o)[3], bool z_from_sam, int mode, char* msg, std::size_t cap) noexcept {
    if (!MayAct(msg, cap)) return false;
    game::Entity* p = nullptr;
    game::WorldTransform wt{};
    if (!game::TryGetLocalPlayer(&p) || !p || !game::TryGetTransform(p, &wt)) return std::snprintf(msg, cap, "no local player transform"), false;
    if (g_replay.st.load(std::memory_order_acquire) != 0) {
        Log("[odra] replay refused: a replay is still pending");
        return std::snprintf(msg, cap, "refused: a replay is still pending"), true;
    }
    double org[3] = {o[0], o[1], z_from_sam ? wt.pos.z : o[2]};
    const double dx = org[0] - wt.pos.x, dy = org[1] - wt.pos.y, dz = org[2] - wt.pos.z;
    g_replay.origin[0] = org[0], g_replay.origin[1] = org[1], g_replay.origin[2] = org[2];
    g_replay.xf = wt;
    g_replay.xf.pos.x = org[0], g_replay.xf.pos.y = org[1], g_replay.xf.pos.z = org[2];
    g_replay.mode = mode;
    g_replay.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    g_replay.seq = g_next_seq++;
    g_replay.t_ms = g_now_ms;
    g_replay.st.store(1, std::memory_order_release);
    Log("[odra] replay requested seq %u origin %.2f %.2f %.2f (Sam at %.2f %.2f %.2f, %.1f m) mode %d", g_replay.seq, org[0], org[1], org[2],
        wt.pos.x, wt.pos.y, wt.pos.z, g_replay.dist, mode);
    std::snprintf(msg, cap, "replay seq %u requested at %.1f %.1f %.1f (%.1f m)", g_replay.seq, org[0], org[1], org[2], g_replay.dist);
    return true;
}

float RF(std::uintptr_t a) noexcept {
    float f = 0;
    mem::Rd(a, f);
    return f;
}
unsigned RB(std::uintptr_t a) noexcept {
    std::uint8_t b = 0;
    mem::Rd(a, b);
    return b;
}
unsigned RD(std::uintptr_t a) noexcept {
    std::uint32_t d = 0;
    mem::Rd(a, d);
    return d;
}

void LogNativeGates(std::uintptr_t inv) noexcept;

void Dump() noexcept {
    const std::uintptr_t player = g_local_player.load(), inv = g_local_inv.load();
    Log("[odra] dump: byte checks %u/%u, detour %s, calls %llu (local %llu, contended %llu), ring dropped %llu, watch %d, mode %d",
        g_checks_ok, g_checks_n, g_installed.load() ? "on" : "off", static_cast<unsigned long long>(g_calls.load()),
        static_cast<unsigned long long>(g_local_calls.load()), static_cast<unsigned long long>(g_contended.load()),
        static_cast<unsigned long long>(g_dropped.load()), g_watch.load() ? 1 : 0, g_mode.load());
    Log("[odra] dump: threads: detour %lu, tick %lu, this %lu (detour %s the game thread)", g_detour_tid.load(), g_tick_tid.load(),
        GetCurrentThreadId(), g_detour_tid.load() == g_tick_tid.load() ? "ON" : "NOT on");
    SyncLine();
    if (!player || !inv) {
        Log("[odra] dump: no local player / inventory (player %p inv %p)", reinterpret_cast<void*>(player), reinterpret_cast<void*>(inv));
        return;
    }
    Log("[odra] dump: player %p inv %p ([inv+0x48] %p [inv+0x58] %p [inv+0x60] %p [inv+0x68] %p)", reinterpret_cast<void*>(player),
        reinterpret_cast<void*>(inv), reinterpret_cast<void*>(mem::Q(inv + kIOwner48)), reinterpret_cast<void*>(mem::Q(inv + kIEnt58)),
        reinterpret_cast<void*>(mem::Q(inv + kIComps60)), reinterpret_cast<void*>(mem::Q(inv + 0x68)));
    const std::uintptr_t arm = mem::Q(inv + kIArm), body = mem::Q(inv + kIBody);
    const unsigned bstate = body ? RB(body + kRefState) : 0, bflags = body ? RD(body + kRefFlags) : 0;
    Log("[odra] dump: state %u req 0x%02X sel 0x%02X act 0x%02X charge %.2f range %.2f dir %.3f %.3f; arm %p (0x%02X) body %p (0x%02X flags "
        "0x%X ready %d)",
        RD(inv + kIState), RB(inv + kIReq), RB(inv + kIWaveSel), RB(inv + kIAct), RF(inv + kICharge), RF(inv + kIRange), RF(inv + kIDir),
        RF(inv + kIDir + 4), reinterpret_cast<void*>(arm), arm ? RB(arm + kRefState) : 0u, reinterpret_cast<void*>(body), bstate, bflags,
        body && rules::BodyReady(bflags, static_cast<std::uint8_t>(bstate)) ? 1 : 0);
    for (std::uintptr_t off : {kIWave0, kIWave1}) {
        const std::uintptr_t w = mem::Q(inv + off);
        double pos[3] = {};
        if (w) mem::Read(w + kW20, pos, sizeof(pos));
        Log("[odra] dump: wave +0x%llX %p r %.1f 2bd=%u 2c0=%u 2c1=%u at %.2f %.2f %.2f", static_cast<unsigned long long>(off),
            reinterpret_cast<void*>(w), w ? RF(w + kW2AC) : 0.0f, w ? RB(w + kW2BD) : 0u, w ? RB(w + kW2C0) : 0u, w ? RB(w + kW2C1) : 0u,
            pos[0], pos[1], pos[2]);
    }
    const std::uintptr_t c = g_comp_seen.load();
    if (c) {
        float o[3] = {}, d[4] = {};
        mem::Read(c + kCOrigin, o, sizeof(o));
        mem::Read(c + kCDir, d, sizeof(d));
        const std::uintptr_t res = mem::Q(c + kCRes);
        Log("[odra] dump: survey comp %p owner %p 244=%u 245=%u 246=%u t228 %.2f 22c %.2f c0 %.2f %.2f %.2f dir %.3f %.3f %.3f %.3f angle %.2f",
            reinterpret_cast<void*>(c), reinterpret_cast<void*>(mem::Q(c + kCOwner)), RB(c + kC244), RB(c + kC245), RB(c + kC246),
            RF(c + kCTimer), RF(c + kC22C), o[0], o[1], o[2], d[0], d[1], d[2], d[3], RF(c + kCAngle));
        if (res)
            Log("[odra] dump: survey resource %p ShapeMode %u DirectionalAngle %.2f Range %.2f BaseTime %.2f FadeTime %.2f",
                reinterpret_cast<void*>(res), RD(res + 0x140), RF(res + 0x144), RF(res + 0x1C8), RF(res + 0x1D0), RF(res + 0x1D4));
    } else {
        Log("[odra] dump: no survey comp seen yet");
    }
    const std::uintptr_t cfg = mem::Q(mem::Q(player + kPCfgOwner) + 0x30);
    Log("[odra] dump: odradek config %p +0x3320 %.3f +0x3324 %.3f (DisplayMarkTime / DisplayOutlineTime [I])", reinterpret_cast<void*>(cfg),
        cfg ? RF(cfg + 0x3320) : 0.0f, cfg ? RF(cfg + 0x3324) : 0.0f);
    const std::uintptr_t ps = mem::Q(A(kPlayerSysVa));
    double po[3] = {};
    if (ps) mem::Read(ps + 0x205D8, po, sizeof(po));
    Log("[odra] dump: DSPlayerSystem %p last scan origin %.2f %.2f %.2f pulses %u", reinterpret_cast<void*>(ps), po[0], po[1], po[2],
        ps ? RD(ps + 0x205D4) : 0u);
    LogNativeGates(inv);
}

// The non-input conditions of the native trigger 0x142805CE0 (0x142805ED9..0x142805F7A), read only: they say whether Sam's own R1
// would be taken here (the probe's trigger skips them; round 28 p4: is a scan allowed at this spot, e.g. inside a terminal room?).
//   g1 [inv+0x70]+0x428 = h non-null and h+0x5CC & 0x100; g2 [inv+0x68]+0x54BC = n: n == 0, or h+0x5B8 & 0x400000, or n in 1..3;
//   g3 state not 3 / 4; g4 [inv+0x68]+0x2B4 & 2, !(+0x52F4 & 2), +0x4914 & 2, and 0.35 > +0x11A8.
void LogNativeGates(std::uintptr_t inv) noexcept {
    const std::uintptr_t o70 = mem::Q(inv + 0x70), h = o70 ? mem::Q(o70 + 0x428) : 0, p = mem::Q(inv + 0x68);
    const unsigned h5cc = h ? RD(h + 0x5CC) : 0u, h5b8 = h ? RD(h + 0x5B8) : 0u;
    const unsigned n = p ? RD(p + 0x54BC) : 0u, b2b4 = p ? RB(p + 0x2B4) : 0u, d52f4 = p ? RD(p + 0x52F4) : 0u,
                   b4914 = p ? RB(p + 0x4914) : 0u, st = RD(inv + kIState);
    const float f11a8 = p ? RF(p + 0x11A8) : 0.0f;
    const bool g1 = h && (h5cc & 0x100u), g2 = n == 0 || (h5b8 & 0x400000u) || (n - 1u) <= 2u, g3 = st != 3 && st != 4,
               g4 = p && (b2b4 & 2u) && !(d52f4 & 2u) && (b4914 & 2u) && 0.35f > f11a8;
    Log("[odra] dump: native trigger gates %s: g1 %d (h %p +0x5CC 0x%X) g2 %d (n %u +0x5B8 0x%X) g3 %d (state %u) g4 %d ([inv+0x68] %p "
        "+0x2B4 0x%02X +0x52F4 0x%X +0x4914 0x%02X +0x11A8 %.3f)",
        g1 && g2 && g3 && g4 ? "all pass (an R1 press would scan here)" : "BLOCKED (Sam's own R1 would not scan here)", g1 ? 1 : 0,
        reinterpret_cast<void*>(h), h5cc, g2 ? 1 : 0, n, h5b8, g3 ? 1 : 0, st, g4 ? 1 : 0, reinterpret_cast<void*>(p), b2b4, d52f4, b4914,
        f11a8);
}

struct Check {
    std::uintptr_t va;
    const char* sig;
    const char* name;
};
struct RipCheck {
    std::uintptr_t insn;
    unsigned off, len;
    std::uintptr_t want;
    const char* name;
};
}  // namespace

void Install(const Config& config) noexcept {
    g_probe = config.odradek_probe;
    g_sync = config.odradek_scan_sync;
    if (g_sync && config.role != Role::host && config.role != Role::guest) {
        Log("[odra] scan sync off: needs a two-game session (role %s)", RoleName(config.role));
        g_sync = false;
    }
    if (!g_probe && !g_sync) return;
    g_watch.store(g_probe);  // the transition lines are the probe's; scan sync alone logs only its own lines
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_ms = static_cast<double>(f.QuadPart) / 1000.0;
    static const Check kChecks[] = {
        {0x14282B530, "4C 8B DC 53 56 48 81 EC 38 03 00 00 C4 C1 78 29 7B B8 C4 41 78 29 4B 98 48 8B 05", "machine prologue"},
        {0x1427D3A6F, "C5 F8 28 CE 48 8B CE E8", "caller (dt in xmm1)"},
        {0x14282B5E2, "F6 86 7C 4C 00 00 40 BA 4B 03 00 00 89 86 D8 4A 00 00 B9 00 4F 00 00 B8 F8 4E 00 00", "wave select"},
        {0x14282B60B, "4C 8B 2C 30 48 8B 86 88 4C 00 00", "wave / arm ref"},
        {0x14282B7B2, "C4 C1 7A 10 85 AC 02 00 00", "wave +0x2AC"},
        {0x14282B7C7, "45 38 B5 BD 02 00 00", "wave +0x2BD"},
        {0x14282B7F8, "0F B6 86 79 4C 00 00 A8 20 0F 84", "request bit"},
        {0x142805F80, "80 8E 79 4C 00 00 20 C7 86 34 49 00 00 CD CC CC 3E C7 86 3C 49 00 00 00 00 80 40", "native trigger"},
        {0x14282BEC6, "49 8B CD C5 F8 77 E8", "pulse wave move"},
        {kWaveXformVa, "40 53 48 83 EC 70 48 8B 05", "wave SetWorldTransform"},
        {0x14282BFB1, "48 8B 4E 60 48 8D 15 ?? ?? ?? ?? 48 81 C1 90 00 00 00 C5 F8 77 E8", "pulse GetComponent"},
        {0x14282C000, "C6 80 46 02 00 00 01", "pulse +0x246"},
        {0x14282C024, "C5 FA 7F 98 F0 00 00 00", "pulse +0xF0"},
        {kSurveyVa,
         "40 53 48 83 EC 20 33 C0 66 C7 81 44 02 00 00 01 00 89 81 28 02 00 00 48 8B D9 89 81 38 02 00 00 89 81 20 02 00 00 48 C7 81 2C "
         "02 00 00 00 00 80 3F C5 F8 10 02 C5 F9 7F 81 C0 00 00 00",
         "survey start"},
        {kGetCompVa, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 48 8B 59 18 4C 8B DA", "GetComponent"},
        {0x14282C768, "48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? E8", "decal lookup"},
        {0x14282C78E, "C4 C1 78 10 4D 20", "decal wave +0x20"},
        {kDecalFindVa, "40 53 48 83 EC 30 48 8D 59 68", "DecalManager find"},
        {0x14295BA70, "F6 81 E4 03 00 00 04 74 1B 0F B6 81 48 03 00 00 3C 1E 77 10 0F B6 C0 B9 C2 D3 08 64", "body ready"},
        {0x14271FC22, "48 89 83 C8 54 00 00", "player +0x54C8"},
        {0x1427C0BC8, "4C 89 AF A0 4C 00 00", "inv +0x4CA0"},
        {0x14282C091, "C5 F8 11 80 D8 05 02 00", "DSPlayerSystem +0x205D8"},
        // O3: the visible scan's steps
        {kWaveResetVa,
         "33 C0 C7 81 98 02 00 00 00 00 80 3F 48 89 81 90 02 00 00 66 89 81 C2 02 00 00 88 81 BD 02 00 00 89 81 A4 02 00 00 89 81 AC 02 "
         "00 00 C3",
         "wave reset"},
        {0x14282BF31, "49 8B CD 41 C7 85 8C 02 00 00 E4 CB 16 40 C4 C1 7A 7F 9D 50 02 00 00 41 C6 85 C0 02 00 00 01 E8", "pulse wave start"},
        {0x14282BF5E, "41 C6 85 C1 02 00 00 01 48 8B 05 ?? ?? ?? ?? C5 F8 11 80 B8 01 00 00", "pulse +0x2C1 / wave front"},
        {0x14282BF7E, "48 8D 88 D0 01 00 00 C5 FB 11 88 C8 01 00 00", "wave front +0x1C8 / +0x1D0"},
        {0x14282BA38, "4C 8B 35 ?? ?? ?? ?? E8", "trigger sensor"},
        {0x14282BA93, "48 8D 94 24 60 02 00 00 49 8B CE E8 ?? ?? ?? ?? C5 F8 28 CF 49 8B CE E8", "trigger sensor calls"},
        {0x14282BAE6,
         "C5 FA 7F 9F B0 5F 00 00 88 9F D8 5F 00 00 44 89 B7 D0 5F 00 00 C5 F8 10 86 80 48 00 00 C5 FA 7F 87 C0 5F 00 00 C7 87 D4 5F 00 "
         "00 E4 CB 16 40",
         "trigger sensor block"},
        {kSensorPosVa, "C5 F8 10 02 C5 F9 7F 41 50 C6 41 78 01 C3", "sensor pos"},
        {kSensorRadVa, "C5 FA 11 49 70 C3", "sensor radius"},
        {kSensorDirVa, "C5 F8 10 02 C5 F9 7F 41 60 C5 FA 11 51 74 C3", "sensor dir"},
        {kSensor2GetVa, "48 81 EC E8 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 D0 00 00 00 48 8B 05", "sensor block getter"},
        {0x14282C0AA, "48 8B 05 ?? ?? ?? ?? C7 80 F8 05 02 00 E4 CB 16 40 E8", "DSPlayerSystem +0x205F8"},
        {0x14282C0C0, "48 8B 05 ?? ?? ?? ?? C5 FA 11 80 F4 05 02 00", "DSPlayerSystem +0x205F4"},
        {kYawVa, "48 83 EC 58 C4 E2 79 18 41 04 C5 F8 11 44 24 20 C5 FA 10 01", "yaw from dir"},
        {0x14282C528,
         "FF 86 74 48 00 00 C7 86 68 48 00 00 CD CC 4C 3F C7 86 50 48 00 00 04 00 00 00 C5 F8 28 F7 4D 85 ED 0F 84 ?? ?? ?? ?? 41 80 BD "
         "BD 02 00 00 00 0F 85",
         "pulse end / tail wave test"},
        {0x14282C55D, "48 8B 3D ?? ?? ?? ?? C5 F8 77 E8", "tail sensor"},
    };
    static const RipCheck kRips[] = {
        {0x1427D3A76, 1, 5, kStateVa, "call -> machine"},
        {0x14282BECC, 1, 5, kWaveXformVa, "call -> wave move"},
        {0x14282BFB5, 3, 7, kSurveyRttiVa, "survey rtti"},
        {0x14282BFC6, 1, 5, kGetCompVa, "call -> GetComponent"},
        {0x14282C075, 1, 5, kSurveyVa, "call -> survey start"},
        {0x14282C768, 3, 7, kDecalRootVa, "decal root"},
        {0x14282C76F, 3, 7, kDecalRttiVa, "decal rtti"},
        {0x14282C776, 1, 5, kDecalFindVa, "call -> DecalManager find"},
        {0x14282C07A, 3, 7, kPlayerSysVa, "DSPlayerSystem global"},
        // O3
        {0x14282BF50, 1, 5, kWaveResetVa, "call -> wave reset"},
        {0x14282BF66, 3, 7, kFrontVa, "wave front global"},
        {0x14282BA38, 3, 7, kSensorVa, "sensor global (trigger)"},
        {0x14282C55D, 3, 7, kSensorVa, "sensor global (tail)"},
        {0x14282BA3F, 1, 5, kSensor2GetVa, "call -> sensor block getter"},
        {0x14282C567, 1, 5, kSensor2GetVa, "call -> sensor block getter (tail)"},
        {0x142F400A9, 3, 7, kSensor2Va, "sensor block global"},
        {0x14282BA9E, 1, 5, kSensorPosVa, "call -> sensor pos"},
        {0x14282BAAA, 1, 5, kSensorRadVa, "call -> sensor radius"},
        {0x14282BAD0, 1, 5, kSensorDirVa, "call -> sensor dir"},
        {0x14282C0AA, 3, 7, kPlayerSysVa, "DSPlayerSystem global (+0x205F8)"},
        {0x14282C0BB, 1, 5, kYawVa, "call -> yaw"},
    };
    char bad[512] = {};
    std::size_t at = 0;
    auto note = [&](bool ok, const char* name) {
        ++g_checks_n;
        if (ok) ++g_checks_ok;
        else if (at + 2 < sizeof(bad)) at += static_cast<std::size_t>(std::snprintf(bad + at, sizeof(bad) - at, "%s%s", at ? ", " : "", name));
    };
    for (const Check& c : kChecks) note(mem::CodeMatches(A(c.va), c.sig), c.name);
    for (const RipCheck& r : kRips) note(mem::RipTarget(A(r.insn), r.off, r.len) == A(r.want), r.name);
    if (g_checks_ok != g_checks_n) {
        Log("[odra] byte checks: %u of %u failed (%s); the probe and scan sync are off", g_checks_n - g_checks_ok, g_checks_n, bad);
        return;
    }
    Log("[odra] byte checks %u/%u ok", g_checks_ok, g_checks_n);
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        Log("[odra] detour off: MH_Initialize %s", MH_StatusToString(init));
        return;
    }
    void* target = reinterpret_cast<void*>(A(kStateVa));
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&H_State), reinterpret_cast<void**>(&o_state));
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        Log("[odra] detour off: %s", MH_StatusToString(s));
        return;
    }
    g_installed.store(true);
    Log("[odra] detour installed on FUN_14282B530 (the scan state machine); probe %s (watch %s, replay mode 0), scan sync %s",
        g_probe ? "on" : "off", g_watch.load() ? "on" : "off", g_sync ? "on (odradek_scan_sync=1: SOD1 HELLO / SCAN on channel 6)" : "off");
}

bool Installed() noexcept { return g_installed.load(); }

void EmergencyStop() noexcept {
    g_stopped.store(true);
    g_scan_st.store(0);  // a request queued just before the fault is dropped, not taken by a later machine run
    g_replay.st.store(0);
    int pub = 1;
    g_remote.st.compare_exchange_strong(pub, 0);  // a published partner scan is dropped (Tick announces caps 0)
}

void Tick(game::Entity* local, double now_ms) noexcept {
    if ((!g_probe && !g_sync) || !g_installed.load(std::memory_order_relaxed)) return;
    g_now_ms = now_ms;
    g_tick_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const std::uintptr_t player = reinterpret_cast<std::uintptr_t>(local);
    if (player != g_cache_player || now_ms - g_cache_ms > 1000.0) {
        g_cache_player = player, g_cache_ms = now_ms;
        std::uintptr_t inv = player ? mem::Q(player + kPInv) : 0;
        // the inventory's entity (+0x58: the pulse reads its +0xC8 position) or its owner (+0x48) is the player
        if (inv && mem::Q(inv + kIEnt58) != player && mem::Q(inv + kIOwner48) != player) inv = 0;
        const std::uintptr_t was = g_local_inv.exchange(inv, std::memory_order_acq_rel);
        g_local_player.store(player, std::memory_order_relaxed);
        if (was != inv) {
            Log("[odra] local inventory %p (player %p)", reinterpret_cast<void*>(inv), reinterpret_cast<void*>(player));
            if (was) DropPartnerPending("the local inventory changed (a load)");  // the partner's scans waiting here
        }
    }
    // the requests the machine did not take (it did not run for the local player)
    int pending = 1;
    if (now_ms - g_scan_t_ms > 3000.0 && g_scan_st.load() == 1 && g_scan_st.compare_exchange_strong(pending, 0))
        Log("[odra] scan request expired: the machine did not run for 3 s");
    pending = 1;
    if (now_ms - g_replay.t_ms > 3000.0 && g_replay.st.load() == 1 && g_replay.st.compare_exchange_strong(pending, 0))
        Log("[odra] replay request expired: the machine did not run for 3 s");
    // O3: the link (HELLO, the partner's scans, our pulses), then the slot
    SyncTick(now_ms);
    SlotTick(now_ms);
    // the ring
    const bool watch = g_watch.load(std::memory_order_relaxed);
    unsigned t = g_tail.load(std::memory_order_relaxed);
    const unsigned h = g_head.load(std::memory_order_acquire);
    for (int n = 0; t != h && n < 64; ++n, ++t) {
        const Sample s = g_ring[t % kRing];
        if (watch || s.kind == kScan || s.kind == kReplay) LogSample(s);
    }
    g_tail.store(t, std::memory_order_release);
}

bool Command(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    const int op = static_cast<int>(v[0]);
    if (op == 7 && (g_probe || g_sync)) {
        SyncLine();
        std::snprintf(msg, cap, "scan sync: shown %u queued %u dropped %u sent %u received %u", g_sc.shown, g_sc.queued, g_sc.dropped,
                      g_sc.sent, g_sc.received);
        return true;
    }
    if (!g_probe && !(op == 2 && g_sync && automation::Enabled())) return std::snprintf(msg, cap, "odradek_probe is off"), false;
    switch (op) {
        case 0:
            Dump();
            std::snprintf(msg, cap, "dump logged ([odra] dump), byte checks %u/%u", g_checks_ok, g_checks_n);
            return true;
        case 1:
            g_watch.store(v[1] != 0);
            Log("[odra] watch %s", v[1] != 0 ? "on" : "off");
            std::snprintf(msg, cap, "watch %s", v[1] != 0 ? "on" : "off");
            return true;
        case 2: {
            if (!MayAct(msg, cap, true)) return false;
            int expect = 0;
            if (!g_scan_st.compare_exchange_strong(expect, 1)) {
                Log("[odra] scan refused: a scan request is still pending");
                return std::snprintf(msg, cap, "refused: a scan request is still pending"), true;
            }
            g_scan_t_ms = g_now_ms;
            Log("[odra] scan requested");
            std::snprintf(msg, cap, "scan requested");
            return true;
        }
        case 3: {
            game::Entity* p = nullptr;
            game::WorldTransform wt{};
            if (!game::TryGetLocalPlayer(&p) || !p || !game::TryGetTransform(p, &wt))
                return std::snprintf(msg, cap, "no local player transform"), false;
            const double m = v[1] > 0 ? v[1] : 20.0;
            // forward: the rotation's second row (right, forward, up), flattened; the Odradek's own dir (inv +0x4880) if degenerate
            double fx = wt.rot.m[1][0], fy = wt.rot.m[1][1];
            double l = std::sqrt(fx * fx + fy * fy);
            if (!(l > 1e-3)) {
                const std::uintptr_t inv = g_local_inv.load();
                fx = inv ? RF(inv + kIDir) : 0.0, fy = inv ? RF(inv + kIDir + 4) : 1.0;
                l = std::sqrt(fx * fx + fy * fy);
                if (!(l > 1e-3)) fx = 0, fy = 1, l = 1;
            }
            const double o[3] = {wt.pos.x + fx / l * m, wt.pos.y + fy / l * m, 0.0};
            const int mode = v[2] != 0 ? static_cast<int>(v[2]) : g_mode.load();  // 1 forces mode 1; 0 / absent = op 4's mode
            return QueueReplay(o, true, mode == 1 ? 1 : 0, msg, cap);
        }
        case 5:
        case 8: {
            // 5 <m> [mask]: a partner scan m metres ahead of Sam (default 20) at Sam's height, shown here through the O3 path (no
            // link); 8 <m>: a test pulse there sent to the partner (SOD1 flag test; odradek_scan_sync=1 and the partner's HELLO)
            if (!MayAct(msg, cap)) return false;
            game::Entity* p = nullptr;
            game::WorldTransform wt{};
            if (!game::TryGetLocalPlayer(&p) || !p || !game::TryGetTransform(p, &wt))
                return std::snprintf(msg, cap, "no local player transform"), false;
            const double m = v[1] > 0 ? v[1] : 20.0;
            double fx = wt.rot.m[1][0], fy = wt.rot.m[1][1];
            double l = std::sqrt(fx * fx + fy * fy);
            if (!(l > 1e-3)) fx = 0, fy = 1, l = 1;
            fx /= l, fy /= l;
            rules::SodScan sc;
            sc.origin[0] = wt.pos.x + fx * m, sc.origin[1] = wt.pos.y + fy * m, sc.origin[2] = wt.pos.z;
            sc.dir[0] = static_cast<float>(fx), sc.dir[1] = static_cast<float>(fy);
            if (op == 8) {
                SendScan(sc.origin, sc.dir, rules::kSodFlagTest);
                std::snprintf(msg, cap, "test pulse %.0f m ahead: sent %u, not sent %u", m, g_sc.sent, g_sc.not_sent);
                return true;
            }
            const std::uint32_t mask = v[2] > 0 ? static_cast<std::uint32_t>(v[2]) & rules::kStartAll : g_start_mask.load();
            sc.seq = g_next_seq++;
            Log("[odra] partner scan test requested seq=%u %.1f m ahead at %.2f %.2f %.2f mask 0x%X", sc.seq, m, sc.origin[0], sc.origin[1],
                sc.origin[2], mask);
            OnScan(sc, true, mask, g_now_ms);
            std::snprintf(msg, cap, "test scan seq %u %.0f m ahead (mask 0x%X)", sc.seq, m, mask);
            return true;
        }
        case 6: {
            const std::uint32_t mask = v[1] > 0 ? static_cast<std::uint32_t>(v[1]) & rules::kStartAll : rules::kStartAll;
            g_start_mask.store(mask);
            Log("[odra] start mask 0x%X (1 wave, 2 emit dir, 4 slot, 8 sensor, 16 front, 32 DSPlayerSystem, 64 survey)", mask);
            std::snprintf(msg, cap, "start mask 0x%X", mask);
            return true;
        }
        case 4:
            g_mode.store(v[1] == 1 ? 1 : 0);
            Log("[odra] replay mode %d (%s)", g_mode.load(), g_mode.load() ? "the finished wave moved first" : "the survey component only");
            std::snprintf(msg, cap, "replay mode %d", g_mode.load());
            return true;
        default:
            std::snprintf(msg, cap,
                          "odra: 0 dump, 1 <0|1> watch, 2 scan, 3 <m> [1] replay ahead, 4 <mode> replay mode, 5 <m> [mask] partner scan "
                          "ahead (O3 path), 6 [mask] start mask, 7 scan sync status, 8 <m> test pulse to the partner");
            return false;
    }
}

bool Replay(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!g_probe) return std::snprintf(msg, cap, "odradek_probe is off"), false;
    return QueueReplay(v, v[2] == 0, g_mode.load(), msg, cap);
}

}  // namespace st::odra
