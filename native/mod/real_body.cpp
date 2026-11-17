#include "real_body.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "MinHook.h"
#include "log.hpp"
#include "partner_holo.hpp"
#include "partner_source.hpp"

namespace st::realbody {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// Evidence: docs/real-partner-body.md. Everything is byte-checked before use.
constexpr std::uintptr_t kAnimate = 0x23AA540;       // void SkinnedModel::Animate(SkinnedModel*, float dt)
constexpr std::uintptr_t kAnimateTail = 0x23AA83A;   // its tail: model matrices, then bounds when [sm+0x338]
constexpr std::uintptr_t kModelMatrices = 0x1D8AA20;  // void (Pose*): local joints -> model matrices, state 4
constexpr std::uintptr_t kBounds = 0x1D8A660;        // void (Pose*, sm+0x308 bone boxes, sm+0x2E0 AABB)
constexpr std::uintptr_t kForcedUpdate = 0x239D890;  // void (SkinnedModel*): flip, reset, animate(dt 0), cloth, commit
constexpr std::uintptr_t kPoseGetter = 0x23A0740;    // SkinnedModel vt+0x70: sm+0x170+[sm+0x250]*0x70
constexpr std::uintptr_t kSmVtable = 0x3D0ADB8;      // SkinnedModel vtable (slot 0 = 0x1423A0950, +0x70 = 0x1423A0740)
constexpr std::uintptr_t kSmVtSlot0 = 0x23A0950;
constexpr std::uintptr_t kRegister = 0x1DE3F30;      // StreamingManager::Register(mgr, StreamingRef*, String* path, GGUUID*)
constexpr std::uintptr_t kMgrVtable = 0x3C1DEC8;     // streaming-manager interface vtable (+0x10 = Register)
constexpr std::uintptr_t kCopyRef = 0x1DDB460;       // StreamingRef copy (what the spawner ctor calls on our ref)
constexpr std::uintptr_t kSpawnerCtorCopy = 0x236D15C;  // lea rcx,[rbx+0x188]; call 0x141DDB460 in the spawner ctor 0x14236CFC0
constexpr std::uintptr_t kStrCtor = 0x190AA00;       // String::String(String* out, const char*)
constexpr std::uintptr_t kStrDtor = 0x190AD20;       // String::~String(String*)

// SkinnedModel (0x3B0)
constexpr std::uintptr_t kSmEntity = 0x48;
constexpr std::uintptr_t kSmPoses = 0x170;
constexpr std::uintptr_t kSmPoseIdx = 0x250;
constexpr std::uintptr_t kSmAabb = 0x2E0;
constexpr std::uintptr_t kSmBoneBoxes = 0x308;
constexpr std::uintptr_t kSmBoundsOn = 0x338;
constexpr std::uintptr_t kPoseSize = 0x70;
// Pose (0x70)
constexpr std::size_t kHdrSkel = 0x20, kHdrState = 0x28, kHdrLocal = 0x30, kHdrLocalEnd = 0x38, kHdrModel = 0x40,
                      kHdrModelEnd = 0x48;
constexpr std::int32_t kPoseStateModel = 4;
// Skeleton
constexpr std::uintptr_t kSkelCount = 0x20, kSkelJoints = 0x28, kSkelLayout = 0x70;
// Entity
constexpr std::uintptr_t kEntResRef = 0x58, kEntModel = 0xB8;

constexpr unsigned kJ = 77;  // joints on the shared human layout BE39C83F
constexpr unsigned kSamRoot = 0, kSamPelvis = 1;
constexpr double kLoadTimeoutMs = 20000;
constexpr unsigned kLatchGood = 10, kLatchMax = 120;

struct Joint {
    float q[4];
    float t[4];
    float s[4];
};
static_assert(sizeof(Joint) == 0x30, "local joint transform");

// ---------------------------------------------------------------- seqlock (one writer, readers retry)
template <class T>
class SeqLock {
public:
    // Writers that collide skip (the next frame writes again).
    bool Write(const T& v) {
        if (busy_.exchange(true, std::memory_order_acquire)) return false;
        const std::uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&data_, &v, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
        busy_.store(false, std::memory_order_release);
        return true;
    }
    bool Read(T& out) const {
        for (int i = 0; i < 3; ++i) {
            const std::uint32_t a = seq_.load(std::memory_order_acquire);
            if (a & 1) continue;
            std::memcpy(&out, &data_, sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == a) return a != 0;
        }
        return false;
    }

private:
    std::atomic<std::uint32_t> seq_{0};
    std::atomic<bool> busy_{false};
    T data_{};
};

struct Capture {
    std::uint64_t tick_ms = 0;  // GetTickCount64 when captured
    std::uint32_t frame = 0;
    Joint j[kJ];
};
struct ApplyBuf {
    std::uint32_t seq = 0;
    wire::Quat q[kJ];   // Sam-skeleton order
    float troot[3];
    float tpelvis[3];
    Joint fb[kJ];       // the local Sam's own full joints (translation/scale fallback, Sam order)
};

SeqLock<Capture> g_capture;
SeqLock<ApplyBuf> g_apply;

// ---------------------------------------------------------------- detour state
using AnimateFn = void (*)(void* sm, float dt);
using PoseFn = void (*)(void* pose);
using BoundsFn = void (*)(void* pose, void* boxes, void* aabb);
AnimateFn g_orig = nullptr;
std::uintptr_t g_fn_model = 0, g_fn_bounds = 0, g_sm_vt = 0;
bool g_hook = false;

std::atomic<std::uintptr_t> g_sam_sm{0};
std::atomic<std::uintptr_t> g_sam_skel{0};
std::atomic<std::uintptr_t> g_partner_sm{0};
std::atomic<std::uintptr_t> g_partner_e{0};
std::atomic<std::uintptr_t> g_partner_skel{0};
std::atomic<int> g_inflight{0};
std::atomic<bool> g_apply_on{false};  // partner override enabled (mirror off = observe only)
// The game does not update a cutscene-actor body's model by itself outside a sequence (run
// 20261116-183736: 10-13 updates after spawn, then none; it stays in the bind pose), so the
// tick drives it with the game's own forced model update. Detour calls for the partner that
// do not come from that call are counted, and the tick only forces when there were none.
thread_local bool t_forcing = false;
std::atomic<std::uint32_t> g_partner_game_updates{0};
std::uintptr_t g_fn_forced = 0;
std::uint32_t g_forced = 0, g_forced_skipped = 0, g_forced_failed = 0;  // game thread
bool g_forced_off = false;

// Set by the game thread before g_partner_sm is published (release), read by the detour.
std::uint8_t g_map[kJ];  // partner joint -> Sam joint
// Latch of the partner body's own pose (detour writes while g_latch_state == 0).
std::atomic<int> g_latch_state{0};  // 0 observing, 1 done
Joint g_latch[kJ];
bool g_latch_ok = false;
float g_pelvis_ratio = 1.0f;
unsigned g_latch_good = 0, g_latch_seen = 0;
float g_latch_pelvis_len = 0, g_latch_sam_pelvis_len = 0;

struct Counters {
    std::atomic<std::uint32_t> calls{0}, captures{0}, applies{0}, rejects{0}, torn{0}, wpm_fail{0}, faults{0}, aba{0},
        observed{0};
    std::atomic<std::int32_t> last_reject{0};
} g_ct;

bool ReadPoseHdr(std::uintptr_t sm, std::uint8_t (&hdr)[kPoseSize], std::uintptr_t* pose) {
    std::int32_t idx = -1;
    if (!Rd(sm + kSmPoseIdx, idx) || (idx != 0 && idx != 1)) return false;
    *pose = sm + kSmPoses + static_cast<std::uintptr_t>(idx) * kPoseSize;
    return mem::Read(*pose, hdr, sizeof(hdr));
}

template <class T>
T Field(const std::uint8_t* hdr, std::size_t off) {
    T v;
    std::memcpy(&v, hdr + off, sizeof(T));
    return v;
}

// 0 = ok, else a reason code (logged by the tick).
int CheckHdr(const std::uint8_t* hdr, std::uintptr_t skel, std::uintptr_t* local) {
    if (Field<std::uintptr_t>(hdr, kHdrSkel) != skel || !skel) return 1;
    if (Field<std::int32_t>(hdr, kHdrState) != kPoseStateModel) return 2;
    const std::uintptr_t lb = Field<std::uintptr_t>(hdr, kHdrLocal), le = Field<std::uintptr_t>(hdr, kHdrLocalEnd);
    const std::uintptr_t mb = Field<std::uintptr_t>(hdr, kHdrModel), me = Field<std::uintptr_t>(hdr, kHdrModelEnd);
    if (!lb || le - lb != kJ * sizeof(Joint)) return 3;
    if (!mb || me - mb != kJ * 0x40) return 4;
    *local = lb;
    return 0;
}

bool Plausible(const Joint (&j)[kJ], float* pelvis_len) {
    for (unsigned i = 0; i < kJ; ++i) {
        const float* q = j[i].q;
        const float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        if (!(n > 0.98f && n < 1.02f)) return false;
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(j[i].t[k]) || std::fabs(j[i].t[k]) > 2.5f) return false;
            if (!(j[i].s[k] > 0.05f && j[i].s[k] < 20.0f)) return false;
        }
    }
    const float* p = j[kSamPelvis].t;
    *pelvis_len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    return *pelvis_len > 0.5f && *pelvis_len < 1.3f;
}

void CaptureSam(std::uintptr_t sm) {
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0, local = 0;
    if (!ReadPoseHdr(sm, hdr, &pose)) return;
    const int bad = CheckHdr(hdr, g_sam_skel.load(std::memory_order_acquire), &local);
    if (bad) {
        g_ct.rejects.fetch_add(1, std::memory_order_relaxed);
        g_ct.last_reject.store(10 + bad, std::memory_order_relaxed);
        return;
    }
    Capture c;
    if (!mem::Read(local, c.j, sizeof(c.j))) return;
    c.tick_ms = GetTickCount64();
    c.frame = g_ct.captures.load(std::memory_order_relaxed) + 1;
    if (g_capture.Write(c)) g_ct.captures.fetch_add(1, std::memory_order_relaxed);
}

bool CallForced(std::uintptr_t sm) {
    __try {
        reinterpret_cast<PoseFn>(g_fn_forced)(reinterpret_cast<void*>(sm));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallPoseFns(std::uintptr_t sm, std::uintptr_t pose, bool bounds) {
    __try {
        reinterpret_cast<PoseFn>(g_fn_model)(reinterpret_cast<void*>(pose));
        if (bounds)
            reinterpret_cast<BoundsFn>(g_fn_bounds)(reinterpret_cast<void*>(pose), reinterpret_cast<void*>(sm + kSmBoneBoxes),
                                                    reinterpret_cast<void*>(sm + kSmAabb));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Pelvis scale = the body's rest (bind) pelvis length over Sam's. Sam's is a constant: his
// animated pelvis length moves with the motion (0.888-0.998 m measured at latch time in runs
// 20261116-184259 .. -185651, which gave the Sam body itself a ratio of up to 1.126 and lifted
// it off the floor), while demo_sam_clean, which uses Sam's very Skeleton object, latched
// 1.000 m in every run. A body on Sam's own Skeleton object is never scaled.
constexpr float kSamRestPelvisLen = 1.000f;
bool g_same_skel = false;  // set by Validate before the body is published

void FinishLatch() {
    g_latch_ok = g_latch_good > 0;
    g_pelvis_ratio = 1.0f;
    g_latch_sam_pelvis_len = kSamRestPelvisLen;
    if (g_latch_ok && !g_same_skel) {
        float r = g_latch_pelvis_len / kSamRestPelvisLen;
        if (r < 0.75f) r = 0.75f;
        if (r > 1.33f) r = 1.33f;
        g_pelvis_ratio = r;
    }
    g_latch_state.store(1, std::memory_order_release);
}

void ApplyPartner(std::uintptr_t sm) {
    // ABA: the game may have freed our body and reused the memory for another model.
    std::uintptr_t id[2] = {};
    if (!mem::Read(sm, id, sizeof(id)) || id[0] != g_sm_vt) {
        g_ct.aba.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uintptr_t owner = 0;
    if (!Rd(sm + kSmEntity, owner) || owner != g_partner_e.load(std::memory_order_acquire)) {
        g_ct.aba.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0, local = 0;
    if (!ReadPoseHdr(sm, hdr, &pose)) return;
    const int bad = CheckHdr(hdr, g_partner_skel.load(std::memory_order_acquire), &local);
    if (bad) {
        g_ct.rejects.fetch_add(1, std::memory_order_relaxed);
        g_ct.last_reject.store(20 + bad, std::memory_order_relaxed);
        return;
    }
    Joint cur[kJ];
    if (g_latch_state.load(std::memory_order_acquire) == 0) {
        g_ct.observed.fetch_add(1, std::memory_order_relaxed);
        if (mem::Read(local, cur, sizeof(cur))) {
            float pl = 0;
            ++g_latch_seen;
            if (Plausible(cur, &pl)) {
                std::memcpy(g_latch, cur, sizeof(g_latch));
                g_latch_pelvis_len = pl;
                ++g_latch_good;
            }
        }
        if (g_latch_good >= kLatchGood || g_latch_seen >= kLatchMax) FinishLatch();
        return;
    }
    if (!g_apply_on.load(std::memory_order_acquire)) return;
    ApplyBuf a;
    if (!g_apply.Read(a)) {
        g_ct.torn.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (unsigned j = 0; j < kJ; ++j) {
        const unsigned sj = g_map[j];
        Joint& o = cur[j];
        o.q[0] = a.q[sj].x, o.q[1] = a.q[sj].y, o.q[2] = a.q[sj].z, o.q[3] = a.q[sj].w;
        const Joint& src = g_latch_ok ? g_latch[j] : a.fb[sj];
        std::memcpy(o.t, src.t, sizeof(o.t));
        std::memcpy(o.s, src.s, sizeof(o.s));
        if (sj == kSamRoot) {
            o.t[0] = a.troot[0], o.t[1] = a.troot[1], o.t[2] = a.troot[2];
        } else if (sj == kSamPelvis) {
            o.t[0] = a.tpelvis[0] * g_pelvis_ratio, o.t[1] = a.tpelvis[1] * g_pelvis_ratio, o.t[2] = a.tpelvis[2] * g_pelvis_ratio;
        }
    }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(local), cur, sizeof(cur), &wrote) || wrote != sizeof(cur)) {
        g_ct.wpm_fail.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::int32_t bounds_on = 0;
    Rd(sm + kSmBoundsOn, bounds_on);
    if (!CallPoseFns(sm, pose, bounds_on != 0)) {
        g_ct.faults.fetch_add(1, std::memory_order_relaxed);
        g_apply_on.store(false, std::memory_order_release);  // never again this run
        return;
    }
    g_ct.applies.fetch_add(1, std::memory_order_relaxed);
}

void Detour(void* sm, float dt) {
    g_orig(sm, dt);
    const auto s = reinterpret_cast<std::uintptr_t>(sm);
    if (!s || (s != g_sam_sm.load(std::memory_order_relaxed) && s != g_partner_sm.load(std::memory_order_relaxed))) return;
    g_inflight.fetch_add(1, std::memory_order_seq_cst);
    g_ct.calls.fetch_add(1, std::memory_order_relaxed);
    if (s == g_sam_sm.load(std::memory_order_seq_cst)) CaptureSam(s);
    else if (s == g_partner_sm.load(std::memory_order_seq_cst)) {
        if (!t_forcing) g_partner_game_updates.fetch_add(1, std::memory_order_relaxed);
        ApplyPartner(s);
    }
    g_inflight.fetch_sub(1, std::memory_order_seq_cst);
}

// ---------------------------------------------------------------- resolution
struct Fns {
    bool hook_checked = false, hook_ok = false;
    bool spawn_resolved = false, spawn_ok = false;
    char why[200] = "not resolved";
    std::uintptr_t reg = 0, str_ctor = 0, str_dtor = 0, mgr_vt = 0;
} g_fn;

bool Check(const char* name, std::uintptr_t rva, const char* sig) {
    if (mem::CodeMatches(mem::Base() + rva, sig)) return true;
    std::snprintf(g_fn.why, sizeof(g_fn.why), "%s at ds.exe+0x%llX does not match", name, static_cast<unsigned long long>(rva));
    return false;
}

bool CheckHookSites() {
    if (g_fn.hook_checked) return g_fn.hook_ok;
    g_fn.hook_checked = true;
    const std::uintptr_t b = mem::Base();
    bool ok = Check("SkinnedModel animate", kAnimate,
                    "40 55 48 81 EC F0 00 00 00 48 8D 6C 24 20 48 89 9D E8 00 00 00 48 89 B5") &&
              Check("animate tail", kAnimateTail,
                    "48 8B CF E8 ?? ?? ?? ?? 83 BB 38 03 00 00 00 74 16 4C 8D 83 E0 02 00 00 48 8B CF 48 8D 93 08 03 00 00 E8") &&
              Check("model matrices", kModelMatrices, "4C 8B DC 57 41 57 48 81 EC 58 01 00 00 48 8B 05") &&
              Check("pose bounds", kBounds, "40 53 48 81 EC 90 00 00 00 48 8B 05") &&
              Check("current-pose getter", kPoseGetter, "48 63 81 50 02 00 00 48 6B D0 70 48 8D 81 70 01 00 00 48 03 C2 C3") &&
              Check("forced model update", kForcedUpdate, "48 89 5C 24 08 57 48 83 EC 30 C5 F8 29 74 24 20 48 8B F9 E8");
    if (ok && (mem::RipTarget(b + kAnimateTail + 3, 1, 5) != b + kModelMatrices ||
               mem::RipTarget(b + kAnimateTail + 0x22, 1, 5) != b + kBounds)) {
        std::snprintf(g_fn.why, sizeof(g_fn.why), "animate tail calls do not lead to the model-matrix / bounds functions");
        ok = false;
    }
    if (ok && (Q(b + kSmVtable) != b + kSmVtSlot0 || Q(b + kSmVtable + 0x70) != b + kPoseGetter)) {
        std::snprintf(g_fn.why, sizeof(g_fn.why), "SkinnedModel vtable at ds.exe+0x%llX does not match",
                      static_cast<unsigned long long>(kSmVtable));
        ok = false;
    }
    if (ok) {
        g_fn_model = b + kModelMatrices;
        g_fn_forced = b + kForcedUpdate;
        g_fn_bounds = b + kBounds;
        g_sm_vt = b + kSmVtable;
    }
    g_fn.hook_ok = ok;
    return ok;
}

bool ResolveSpawn() {
    if (g_fn.spawn_resolved) return g_fn.spawn_ok;
    g_fn.spawn_resolved = true;
    const std::uintptr_t b = mem::Base();
    bool ok = holo::Available();
    if (!ok) std::snprintf(g_fn.why, sizeof(g_fn.why), "spawner unavailable: %s", holo::UnavailableReason());
    ok = ok && Check("StreamingManager::Register", kRegister, "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57") &&
         Check("StreamingRef copy", kCopyRef, "48 89 5C 24 10 48 89 7C 24 18 41 56 48 83 EC 20") &&
         Check("spawner ctor ref copy", kSpawnerCtorCopy, "48 8D 8B 88 01 00 00 E8") && Check("String ctor", kStrCtor, "40 53 48 83 EC 20 48 8B D9 48 C7 01 00 00 00 00") &&
         Check("String dtor", kStrDtor, "40 53 48 83 EC 20 48 8B 19 48 8D 05 70 7F C5 02");
    if (ok && mem::RipTarget(b + kSpawnerCtorCopy + 7, 1, 5) != b + kCopyRef) {
        std::snprintf(g_fn.why, sizeof(g_fn.why), "the spawner ctor does not call the StreamingRef copy at ds.exe+0x%llX",
                      static_cast<unsigned long long>(kSpawnerCtorCopy));
        ok = false;
    }
    if (ok && Q(b + kMgrVtable + 0x10) != b + kRegister) {
        std::snprintf(g_fn.why, sizeof(g_fn.why), "streaming-manager vtable +0x10 is not Register");
        ok = false;
    }
    if (ok) {
        g_fn.reg = b + kRegister;
        g_fn.str_ctor = b + kStrCtor;
        g_fn.str_dtor = b + kStrDtor;
        g_fn.mgr_vt = b + kMgrVtable;
        std::snprintf(g_fn.why, sizeof(g_fn.why), "ok");
        Log("[real] spawn functions verified (Register ds.exe+0x%llX, String ctor/dtor, spawner ref copy)",
            static_cast<unsigned long long>(kRegister));
    } else {
        Log("[real] real body unavailable: %s", g_fn.why);
    }
    g_fn.spawn_ok = ok;
    return ok;
}

// ---------------------------------------------------------------- SEH thunks
using StrCtorFn = void* (*)(void* out, const char* s);
using StrDtorFn = void (*)(void* s);
bool CallStrCtor(void* out, const char* s) {
    __try {
        reinterpret_cast<StrCtorFn>(g_fn.str_ctor)(out, s);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallStrDtor(void* s) {
    __try {
        reinterpret_cast<StrDtorFn>(g_fn.str_dtor)(s);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Set(char* why, std::size_t cap, const char* fmt, ...) {
    if (!why || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}

std::string Str(std::uintptr_t p, std::size_t max = 120) {
    if (!p) return "";
    char buf[160] = {};
    if (max > sizeof(buf) - 1) max = sizeof(buf) - 1;
    std::int32_t len = -1;
    std::size_t n = 0;
    if (Rd(p - 8, len) && len >= 0 && len <= 65536) {
        n = static_cast<std::size_t>(len) < max ? static_cast<std::size_t>(len) : max;
        if (n && !mem::Read(p, buf, n)) return "?";
    }
    for (std::size_t i = 0; i < n; ++i)
        if (static_cast<unsigned char>(buf[i]) < 0x20) buf[i] = ' ';
    return std::string(buf, n);
}

std::string GuidText(const std::uint8_t* g) {
    char b[48];
    std::snprintf(b, sizeof(b), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2], g[1], g[0], g[5],
                  g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return b;
}

// An entity's resource StreamingRef -> {proxy, flags}; proxy +0 String path, +0x10 GUID.
struct RefInfo {
    std::uintptr_t box = 0, proxy = 0, flags = 0, mgr = 0, obj = 0;
    std::int8_t prio = 0;
    std::uint8_t guid[16] = {};
    std::string path;
};
bool ReadRef(std::uintptr_t field, RefInfo* r) {
    *r = RefInfo{};
    r->box = Q(field);
    std::uintptr_t blk[2] = {};
    if (!r->box || !mem::Read(r->box, blk, sizeof(blk)) || !blk[0]) return false;
    r->proxy = blk[0];
    r->flags = blk[1];
    r->mgr = blk[1] & 0xFFFFFFFFFFFFFull;
    r->prio = static_cast<std::int8_t>(static_cast<std::uint8_t>(blk[1] >> 52));
    if (!mem::Read(r->proxy + 0x10, r->guid, 16)) return false;
    r->obj = Q(r->proxy + 0x20);
    r->path = Str(Q(r->proxy));
    return true;
}

// The streaming manager, taken from the local player's own resource reference.
std::uintptr_t Manager(game::Entity* sam, RefInfo* sam_ref) {
    if (!sam || !ReadRef(reinterpret_cast<std::uintptr_t>(sam) + kEntResRef, sam_ref)) return 0;
    if (Q(sam_ref->mgr) != g_fn.mgr_vt) return 0;
    return sam_ref->mgr;
}

// The spawner's own StreamingRef (spawner +0x188, set by its ctor) and its flags, for the log.
void LogSpawnerRef(void* spv, const char* when) {
    const auto sp = reinterpret_cast<std::uintptr_t>(spv);
    RefInfo r;
    std::uint8_t fl[3] = {};
    mem::Read(sp + 0x168, fl, sizeof(fl));
    if (!sp || !ReadRef(sp + 0x188, &r)) {
        Log("[real]   %s: spawner %p flags loaded=%u cancelled=%u job=%u; its reference is unreadable", when, spv, fl[0], fl[1], fl[2]);
        return;
    }
    std::int32_t regs = 0;
    Rd(r.proxy + 0x28, regs);
    const char* on = r.obj ? partner::TypeName(reinterpret_cast<const void*>(r.obj)) : nullptr;
    Log("[real]   %s: spawner %p flags loaded=%u cancelled=%u job=%u; ref box %p proxy %p '%s' guid %s box flags %016llX (prio %d) "
        "object %p (%s) registrations %d",
        when, spv, fl[0], fl[1], fl[2], reinterpret_cast<void*>(r.box), reinterpret_cast<void*>(r.proxy), r.path.c_str(),
        GuidText(r.guid).c_str(), static_cast<unsigned long long>(r.flags), r.prio, reinterpret_cast<void*>(r.obj), on ? on : "-", regs);
}

// SkinnedModel of an entity (vtable + back pointer checked), or 0.
std::uintptr_t ModelOf(std::uintptr_t e) {
    const std::uintptr_t sm = Q(e + kEntModel);
    if (!sm || Q(sm) != mem::Base() + kSmVtable || Q(sm + kSmEntity) != e) return 0;
    return sm;
}

struct SkelInfo {
    std::uintptr_t skel = 0;
    std::int32_t count = -1;
    std::uint32_t layout = 0;
    std::uintptr_t joints = 0;
};
SkelInfo PoseSkeleton(std::uintptr_t sm) {
    SkelInfo s;
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0;
    if (!sm || !ReadPoseHdr(sm, hdr, &pose)) return s;
    s.skel = Field<std::uintptr_t>(hdr, kHdrSkel);
    if (!s.skel) return s;
    const char* tn = partner::TypeName(reinterpret_cast<const void*>(s.skel));
    if (!tn || std::strcmp(tn, "Skeleton") != 0) {
        s.skel = 0;
        return s;
    }
    Rd(s.skel + kSkelCount, s.count);
    Rd(s.skel + kSkelLayout, s.layout);
    s.joints = Q(s.skel + kSkelJoints);
    return s;
}
std::string JointName(const SkelInfo& s, unsigned j) {
    if (!s.joints || static_cast<std::int32_t>(j) >= s.count) return "";
    return Str(Q(s.joints + 0x10ull * j), 64);
}

// ---------------------------------------------------------------- body slot
Owner g_owner = Owner::none;
Phase g_phase = Phase::idle;
holo::Spawn g_spawn;
const bodies::Body* g_body = nullptr;
double g_started_ms = 0;
double g_next_progress_ms = 0;
std::uint8_t g_body_guid[16] = {};
std::uintptr_t g_live_sm = 0;
std::uint32_t g_refused_mask = 0;  // bit per body id
bool g_mirror = false;             // debug body: copy Sam's live pose (no delay)
double g_now = 0;                  // the tick's clock (link::NowMsPrecise), for debug-started spawns

void ResetSlot() {
    g_owner = Owner::none;
    g_phase = Phase::idle;
    g_spawn = holo::Spawn{};
    g_body = nullptr;
    g_live_sm = 0;
    g_mirror = false;
}

void Refuse(const bodies::Body* b, const char* why) {
    if (!b || b->id >= 32) return;
    if (!(g_refused_mask & (1u << b->id))) Log("[real] body '%s' will not be tried again this session (%s)", b->key, why);
    g_refused_mask |= 1u << b->id;
}

// Unpublish the partner model and wait until no detour is inside it.
void Unpublish() {
    g_apply_on.store(false, std::memory_order_seq_cst);
    g_partner_sm.store(0, std::memory_order_seq_cst);
    g_partner_e.store(0, std::memory_order_seq_cst);
    const ULONGLONG until = GetTickCount64() + 50;
    while (g_inflight.load(std::memory_order_seq_cst) != 0 && GetTickCount64() < until) SwitchToThread();
    if (g_inflight.load(std::memory_order_seq_cst) != 0) Log("[real] detour still busy after 50 ms (continuing)");
}

bool Teardown(const char* why) {
    if (g_phase == Phase::idle && !g_spawn.sp) {
        ResetSlot();
        return false;
    }
    Unpublish();
    char step[160] = {};
    holo::SpawnTeardown(g_spawn, why, step, sizeof(step));
    Log("[real] teardown (%s): body '%s': %s", why, g_body ? g_body->key : "?", step);
    ResetSlot();
    return true;
}

// Validates the realized entity and its model, builds the joint map, publishes it.
const char* Validate(std::uintptr_t e) {
    const std::uintptr_t rtti = mem::RttiOf(e);
    if (!rtti) return "entity type unknown";
    if (mem::DerivesFrom(rtti, "Humanoid") || mem::DerivesFrom(rtti, "DSPlayerEntity")) return "a Humanoid / player entity";
    RefInfo r;
    if (!ReadRef(e + kEntResRef, &r)) return "entity resource reference unreadable";
    if (std::memcmp(r.guid, g_body_guid, 16) != 0) return "entity resource GUID differs from the requested body";
    const char* rn = r.obj ? partner::TypeName(reinterpret_cast<const void*>(r.obj)) : nullptr;
    Log("[real]   resource %p %s path '%s' guid %s", reinterpret_cast<void*>(r.obj), rn ? rn : "?", r.path.c_str(),
        GuidText(r.guid).c_str());
    for (const char* bad : {"AIIndividualComponent", "AIBodyComponent", "DSPlayerControllerComponent", "HumanoidController"})
        if (partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), bad)) return "carries AI / controller components";
    const std::uintptr_t sm = ModelOf(e);
    if (!sm) return "no SkinnedModel at +0xB8 (vtable / back pointer)";
    const SkelInfo ps = PoseSkeleton(sm);
    const std::uintptr_t sam_sm = g_sam_sm.load();
    const SkelInfo ss = PoseSkeleton(sam_sm);
    Log("[real]   model %p: skeleton %p joints %d layout %08X; Sam's skeleton %p joints %d layout %08X%s", reinterpret_cast<void*>(sm),
        reinterpret_cast<void*>(ps.skel), ps.count, ps.layout, reinterpret_cast<void*>(ss.skel), ss.count, ss.layout,
        ps.skel && ps.skel == ss.skel ? " (the same object)" : "");
    if (!ps.skel || ps.count != static_cast<std::int32_t>(kJ)) return "body skeleton is not a 77-joint skeleton";
    if (ps.layout != wire::kSamLayoutHash) return "body skeleton layout is not BE39C83F";
    if (!ss.skel || ss.count != static_cast<std::int32_t>(kJ)) return "Sam's skeleton not available";
    // Joint map by name (identity when it is Sam's own Skeleton object).
    unsigned diff_order = 0, missing = 0;
    std::string sam_names[kJ];
    for (unsigned j = 0; j < kJ; ++j) sam_names[j] = JointName(ss, j);
    for (unsigned j = 0; j < kJ; ++j) {
        if (ps.skel == ss.skel) {
            g_map[j] = static_cast<std::uint8_t>(j);
            continue;
        }
        const std::string n = JointName(ps, j);
        int found = -1;
        for (unsigned k = 0; k < kJ && found < 0; ++k)
            if (!n.empty() && sam_names[k] == n) found = static_cast<int>(k);
        if (found < 0) {
            ++missing;
            if (missing <= 4) Log("[real]   body joint %u '%s' has no match on Sam's skeleton", j, n.c_str());
            found = static_cast<int>(j);
        }
        if (found != static_cast<int>(j)) ++diff_order;
        g_map[j] = static_cast<std::uint8_t>(found);
    }
    Log("[real]   joints: 0 '%s', 1 '%s', 2 '%s', 3 '%s' ... 76 '%s'; mapped by name: %u missing, %u in a different order",
        sam_names[0].c_str(), sam_names[1].c_str(), sam_names[2].c_str(), sam_names[3].c_str(), sam_names[76].c_str(), missing,
        diff_order);
    if (missing) return "joint names do not match Sam's skeleton";
    g_live_sm = sm;
    g_same_skel = ps.skel == ss.skel;
    g_latch_good = g_latch_seen = 0;
    g_latch_ok = false;
    g_pelvis_ratio = 1.0f;
    g_latch_state.store(0, std::memory_order_release);
    g_partner_skel.store(ps.skel, std::memory_order_release);
    g_partner_e.store(e, std::memory_order_release);
    g_partner_sm.store(sm, std::memory_order_seq_cst);
    return nullptr;
}

// ---------------------------------------------------------------- pose ring (game thread)
struct Slot {
    double key_ms;
    wire::JointFrame f;
};
constexpr unsigned kRing = 256;
Slot g_ring[kRing];
unsigned g_ring_head = 0, g_ring_count = 0;
std::uint32_t g_apply_seq = 0;
ApplyBuf g_last_published;
bool g_have_published = false;

ULONGLONG g_last_push_tick = 0;  // GetTickCount64 of the newest PushFrame
ULONGLONG g_live_tick = 0;       // GetTickCount64 when the body became live

Slot& RingAt(unsigned i) { return g_ring[(g_ring_head + i) % kRing]; }

void PublishFrame(const wire::JointFrame& f) {
    static ApplyBuf a;
    Capture c;
    if (!g_capture.Read(c)) return;  // no local Sam pose for the fallback translations yet
    a.seq = ++g_apply_seq;
    for (unsigned j = 0; j < kJ; ++j) a.q[j] = j < f.joint_count ? f.q[j] : wire::Quat{};
    std::memcpy(a.fb, c.j, sizeof(a.fb));
    std::memcpy(a.troot, c.j[kSamRoot].t, sizeof(a.troot));
    std::memcpy(a.tpelvis, c.j[kSamPelvis].t, sizeof(a.tpelvis));
    for (unsigned i = 0; i < f.t_count; ++i) {
        if (f.t_joint[i] == kSamRoot) std::memcpy(a.troot, f.t[i], sizeof(a.troot));
        if (f.t_joint[i] == kSamPelvis) std::memcpy(a.tpelvis, f.t[i], sizeof(a.tpelvis));
    }
    g_apply.Write(a);
    g_last_published = a;
    g_have_published = true;
}

bool FrameFromCapture(const Capture& c, wire::JointFrame* f) {
    f->joint_count = kJ;
    f->layout_hash = wire::kSamLayoutHash;
    f->flags = wire::kJointsValid;
    for (unsigned j = 0; j < kJ; ++j) f->q[j] = wire::Normalize(wire::Quat{c.j[j].q[0], c.j[j].q[1], c.j[j].q[2], c.j[j].q[3]});
    f->t_count = 2;
    f->t_joint[0] = kSamRoot;
    f->t_joint[1] = kSamPelvis;
    std::memcpy(f->t[0], c.j[kSamRoot].t, sizeof(f->t[0]));
    std::memcpy(f->t[1], c.j[kSamPelvis].t, sizeof(f->t[1]));
    return true;
}

// ---------------------------------------------------------------- stats
double g_next_stats_ms = 0;
std::uint32_t g_prev[8] = {};
bool g_logged_latch = false;

void PartnerCheck(char* out, std::size_t cap) {
    // (a): the partner's committed pose vs what we published last (max / mean angle over 77 joints).
    out[0] = 0;
    const std::uintptr_t sm = g_partner_sm.load();
    if (!sm || !g_have_published) return;
    float best_max = 1e9f, best_mean = 0;
    for (int b = 0; b < 2; ++b) {
        std::uint8_t hdr[kPoseSize];
        if (!mem::Read(sm + kSmPoses + static_cast<std::uintptr_t>(b) * kPoseSize, hdr, sizeof(hdr))) continue;
        const std::uintptr_t lb = Field<std::uintptr_t>(hdr, kHdrLocal), le = Field<std::uintptr_t>(hdr, kHdrLocalEnd);
        if (!lb || le - lb != kJ * sizeof(Joint)) continue;
        static Joint cur[kJ];
        if (!mem::Read(lb, cur, sizeof(cur))) continue;
        float mx = 0, sum = 0;
        for (unsigned j = 0; j < kJ; ++j) {
            const wire::Quat q{cur[j].q[0], cur[j].q[1], cur[j].q[2], cur[j].q[3]};
            const float a = wire::AngleBetween(q, g_last_published.q[g_map[j]]);
            mx = a > mx ? a : mx;
            sum += a;
        }
        if (mx < best_max) best_max = mx, best_mean = sum / kJ;
    }
    if (best_max < 1e8f) std::snprintf(out, cap, " check: partner vs published max %.2f mean %.2f deg", best_max, best_mean);
}

void Stats(double now) {
    if (now < g_next_stats_ms) return;
    const bool first = g_next_stats_ms == 0;
    g_next_stats_ms = now + (g_partner_sm.load() || g_phase != Phase::idle ? 1000 : 10000);
    if (first) return;
    const std::uint32_t v[8] = {g_ct.calls.load(), g_ct.captures.load(), g_ct.applies.load(), g_ct.rejects.load(),
                                g_ct.torn.load(),  g_ct.aba.load(),      g_ct.observed.load(), g_ct.wpm_fail.load()};
    std::uint32_t d[8];
    bool any = false;
    for (int i = 0; i < 8; ++i) {
        d[i] = v[i] - g_prev[i];
        g_prev[i] = v[i];
        any = any || d[i];
    }
    if (g_latch_state.load(std::memory_order_acquire) == 1 && !g_logged_latch && g_partner_sm.load()) {
        g_logged_latch = true;
        Log("[real] latched the body's own pose: %u plausible of %u frames -> %s; pelvis %.3f m vs Sam's rest %.3f m, ratio %.3f%s", g_latch_good,
            g_latch_seen, g_latch_ok ? "using its own bone translations" : "using Sam's bone translations", g_latch_pelvis_len,
            g_latch_sam_pelvis_len, g_pelvis_ratio, g_same_skel ? " (Sam's own skeleton: not scaled)" : "");
    }
    if (!any) return;
    char check[96] = {};
    if (d[2]) PartnerCheck(check, sizeof(check));
    static std::uint32_t prev_forced = 0, prev_skipped = 0;
    const std::uint32_t fz = g_forced - prev_forced, sk = g_forced_skipped - prev_skipped;
    prev_forced = g_forced, prev_skipped = g_forced_skipped;
    Log("[pose] cap=%u app=%u obs=%u rej=%u(last %d) torn=%u aba=%u wpm=%u faults=%u calls=%u forced=%u game-drove=%u%s", d[1], d[2],
        d[6], d[3], g_ct.last_reject.load(), d[4], d[5], d[7], g_ct.faults.load(), d[0], fz, sk, check);
}

// Game thread: updates the live body's model when the game did not since the last tick (the
// same call the game makes for photo mode / teleports, 0x14239D890 callers 0x142495833,
// 0x142F115FC, 0x142F13938, 0x1436334D2: entity +0xB8, IsKindOf SkinnedModel, call).
// Never while Sam is not up (loads, cutscenes, knot space): the world may be changing and the
// body is about to be removed (player_sync after 1 s, the debug body in Tick).
void DriveModel(bool local_active) {
    const std::uintptr_t sm = g_partner_sm.load(std::memory_order_acquire);
    if (!sm || g_phase != Phase::live || g_forced_off || !g_fn_forced || !local_active) return;
    // Never race the game: while it updates the model itself (seen within the last 100 ms), it
    // is left alone; only a model the game stopped updating is driven from here.
    static ULONGLONG last_game_ms = 0;
    if (g_partner_game_updates.exchange(0, std::memory_order_relaxed) != 0) last_game_ms = GetTickCount64();
    if (last_game_ms && GetTickCount64() - last_game_ms < 100) {
        ++g_forced_skipped;
        return;
    }
    std::uintptr_t id[2] = {};
    if (!mem::Read(sm, id, sizeof(id)) || id[0] != g_sm_vt || Q(sm + kSmEntity) != g_partner_e.load()) return;
    if (!holo::SpawnAlive(g_spawn)) return;
    t_forcing = true;
    const bool ok = CallForced(sm);
    t_forcing = false;
    if (!ok) {
        ++g_forced_failed;
        g_forced_off = true;
        Log("[real] forced model update FAULTED; not called again this run");
        return;
    }
    ++g_forced;
}

}  // namespace

// ================================================================ public
bool InstallHook() noexcept {
    if (g_hook) return true;
    if (!CheckHookSites()) {
        Log("[real] pose hook not installed: %s", g_fn.why);
        return false;
    }
    void* target = reinterpret_cast<void*>(mem::Base() + kAnimate);
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_orig));
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        Log("[real] pose hook not installed: MinHook %s", MH_StatusToString(s));
        return false;
    }
    g_hook = true;
    Log("[real] pose hook installed on SkinnedModel animate ds.exe+0x%llX (model matrices +0x%llX, bounds +0x%llX checked)",
        static_cast<unsigned long long>(kAnimate), static_cast<unsigned long long>(kModelMatrices),
        static_cast<unsigned long long>(kBounds));
    return true;
}
bool HookInstalled() noexcept { return g_hook; }
bool Available() noexcept { return g_hook && ResolveSpawn(); }
const char* UnavailableReason() noexcept { return g_hook ? g_fn.why : "pose hook not installed (partner_body=real or pose_hook=1 at start)"; }

Phase GetPhase() noexcept { return g_phase; }
Owner GetOwner() noexcept { return g_owner; }
const char* PhaseName() noexcept { return g_phase == Phase::live ? "live" : g_phase == Phase::loading ? "loading" : "idle"; }
const bodies::Body* CurrentBody() noexcept { return g_body; }
bool Refused(std::uint8_t id) noexcept { return id < 32 && (g_refused_mask & (1u << id)); }
void ResetRefused() noexcept { g_refused_mask = 0; }

game::Entity* LiveEntity() noexcept {
    if (g_phase != Phase::live || !holo::SpawnAlive(g_spawn)) return nullptr;
    return g_spawn.e;
}

BeginResult Begin(const bodies::Body& body, const game::WorldTransform& at, Owner owner, double now_ms, char* why,
                  std::size_t cap) noexcept {
    if (g_phase != Phase::idle) {
        Set(why, cap, "a body is already %s", PhaseName());
        return BeginResult::busy;
    }
    if (!Available()) {
        Set(why, cap, "%s", UnavailableReason());
        return BeginResult::unavailable;
    }
    if (Refused(body.id)) {
        Set(why, cap, "body '%s' was refused earlier", body.key);
        return BeginResult::failed;
    }
    std::uint8_t guid[16];
    if (!bodies::ParseGuid(body.guid, guid)) {
        Set(why, cap, "bad GUID text for '%s'", body.key);
        return BeginResult::failed;
    }
    game::Entity* sam = nullptr;
    if (!game::TryGetLocalPlayer(&sam) || !sam) {
        Set(why, cap, "no local player");
        return BeginResult::failed;
    }
    RefInfo sr;
    const std::uintptr_t mgr = Manager(sam, &sr);
    if (!mgr) {
        Set(why, cap, "streaming manager not found through the player's resource reference");
        return BeginResult::unavailable;
    }
    std::uint8_t stop = 0;
    if (Rd(mgr + 0x99, stop) && stop) {
        Set(why, cap, "streaming manager refuses loads now (+0x99 set)");
        return BeginResult::failed;
    }
    // A temporary proxy {String path, i32 hash = -1, GUID, object 0, count 0} and a box
    // {proxy, mgr | priority << 52} that reads as "registered, not loaded": the spawner's own
    // StreamingRef copy registers the path + GUID with the manager and queues the load.
    struct FakeProxy {
        void* str;
        std::int32_t hash;
        std::uint32_t pad;
        std::uint8_t guid[16];
        std::uint64_t obj;
        std::int32_t regs;
        std::int32_t pad2;
    };
    static_assert(sizeof(FakeProxy) == 0x30, "proxy layout");
    auto* fp = static_cast<FakeProxy*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!fp) {
        Set(why, cap, "out of memory");
        return BeginResult::failed;
    }
    std::memset(fp, 0, sizeof(*fp));
    fp->hash = -1;
    std::memcpy(fp->guid, guid, 16);
    if (!CallStrCtor(&fp->str, body.path) || !fp->str) {
        VirtualFree(fp, 0, MEM_RELEASE);
        g_fn.spawn_ok = false;
        Set(why, cap, "String ctor faulted; real body disabled");
        return BeginResult::unavailable;
    }
    // Priority 0, exactly like a registered-but-unloaded reference: the spawner's Copy then only
    // registers it (vt+0x38 with priority 0 is a no-op unload), and the spawner's own load step
    // (0x142379340) sees "not loaded, priority 0", registers ITS completion listener with the
    // streaming system ([0x147AE90E0] vt+0x28) and requests the load at priority 7 (vt+0x38).
    // A non-zero priority here queues the load without that listener: run 20261116-183514 showed
    // every such spawn "cancelled" (loaded=1, cancelled=1) one frame later.
    const int prio = 0;
    std::uint64_t box[2] = {reinterpret_cast<std::uint64_t>(fp), (static_cast<std::uint64_t>(static_cast<std::uint8_t>(prio)) << 52) | mgr};
    std::uint64_t field = reinterpret_cast<std::uint64_t>(box);
    const bool loaded_test = static_cast<std::int8_t>(static_cast<std::uint8_t>(box[1] >> 52)) < 0;
    Log("[real] spawning '%s' (%s): path '%s' guid %s; manager %p (reference priority %d%s)", body.key, body.label, body.path,
        GuidText(guid).c_str(), reinterpret_cast<void*>(mgr), prio, loaded_test ? " - BAD: reads as loaded" : "");
    if (loaded_test) {
        CallStrDtor(&fp->str);
        VirtualFree(fp, 0, MEM_RELEASE);
        Set(why, cap, "internal: priority bits read as loaded");
        return BeginResult::failed;
    }
    holo::Spawn sp;
    char w[200] = {};
    const bool ok = holo::SpawnBeginField(reinterpret_cast<std::uintptr_t>(&field), body.id, at, &sp, w, sizeof(w));
    Log("[real]   proxy after the spawner ctor: hash %08X (the manager computed it), %s", static_cast<std::uint32_t>(fp->hash), w);
    CallStrDtor(&fp->str);  // Register copied the String (refcounted)
    VirtualFree(fp, 0, MEM_RELEASE);
    if (!ok) {
        Set(why, cap, "spawner: %s", w);
        return holo::Available() ? BeginResult::failed : BeginResult::unavailable;
    }
    LogSpawnerRef(sp.sp, "after StartSpawner");
    g_spawn = sp;
    g_owner = owner;
    g_body = &body;
    std::memcpy(g_body_guid, guid, 16);
    g_phase = Phase::loading;
    g_started_ms = now_ms;
    g_next_progress_ms = now_ms + 2000;
    Set(why, cap, "loading '%s'", body.key);
    return BeginResult::started;
}

PollResult Poll(double now_ms, char* why, std::size_t cap) noexcept {
    if (g_phase == Phase::idle) return PollResult::idle;
    if (g_phase == Phase::live) {
        if (holo::SpawnAlive(g_spawn)) return PollResult::live;
        Set(why, cap, "the real body entity %p was destroyed by the game", g_spawn.e);
        Log("[real] %s", why);
        Unpublish();
        g_spawn.e = nullptr;
        Teardown("entity gone");
        return PollResult::failed;
    }
    char w[200] = {};
    switch (holo::SpawnPoll(g_spawn, w, sizeof(w))) {
        case holo::SpawnState::loading:
            if (now_ms - g_started_ms > kLoadTimeoutMs) {
                const bodies::Body* b = g_body;
                Set(why, cap, "body '%s' did not load within %.0f s", b ? b->key : "?", kLoadTimeoutMs / 1000);
                Log("[real] %s", why);
                Teardown("load timeout");
                Refuse(b, "load timeout");
                return PollResult::failed;
            }
            if (now_ms >= g_next_progress_ms) {
                g_next_progress_ms = now_ms + 2000;
                Log("[real] still loading '%s' (%.1f s)", g_body ? g_body->key : "?", (now_ms - g_started_ms) / 1000);
                LogSpawnerRef(g_spawn.sp, "loading");
            }
            return PollResult::loading;
        case holo::SpawnState::cancelled: {
            LogSpawnerRef(g_spawn.sp, "cancelled");
            const bodies::Body* b = g_body;
            Set(why, cap, "the game cancelled loading '%s'", b ? b->key : "?");
            Log("[real] %s", why);
            Teardown("cancelled");
            Refuse(b, "cancelled");
            return PollResult::failed;
        }
        case holo::SpawnState::failed:
            Set(why, cap, "%s", w);
            Log("[real] spawn failed: %s", w);
            ResetSlot();
            return PollResult::failed;
        case holo::SpawnState::realized:
            break;
    }
    const bodies::Body* b = g_body;
    const double took = now_ms - g_started_ms;
    game::Entity* e = g_spawn.e;
    LogSpawnerRef(g_spawn.sp, "realized");
    Log("[real] '%s' loaded after %.2f s; realized entity %p", b ? b->key : "?", took / 1000, e);
    if (!e) {
        Set(why, cap, "'%s' loaded but realized no entity", b ? b->key : "?");
        Teardown("no entity");
        Refuse(b, "no entity");
        return PollResult::failed;
    }
    partner::DescribeEntity(e, "real body realized");
    if (const char* bad = Validate(reinterpret_cast<std::uintptr_t>(e))) {
        Set(why, cap, "'%s' refused: %s", b ? b->key : "?", bad);
        Log("[real] %s", why);
        Teardown("refused");
        Refuse(b, bad);
        return PollResult::failed;
    }
    std::uint64_t f = 0;
    partner::ReadFlags(e, &f);
    bool shown = true, awake = true;
    if (!(f & 0x2)) shown = partner::SetVisible(e, true);
    awake = partner::WakeIfAsleep(e);
    std::uint64_t f2 = 0;
    partner::ReadFlags(e, &f2);
    Log("[real] '%s' is live: entity %p model %p flags 0x%llX -> 0x%llX (set visible %s, awake %s)", b ? b->key : "?", e,
        reinterpret_cast<void*>(g_live_sm), static_cast<unsigned long long>(f), static_cast<unsigned long long>(f2),
        shown ? "ok" : "unavailable", awake ? "yes" : "no");
    g_phase = Phase::live;
    g_live_tick = GetTickCount64();
    g_logged_latch = false;
    g_apply_on.store(g_owner == Owner::partner || g_mirror, std::memory_order_release);
    return PollResult::became_live;
}

bool Move(const game::WorldTransform& at) noexcept {
    game::Entity* e = LiveEntity();
    return e && game::TrySetTransform(e, at);
}

bool Remove(const char* why) noexcept { return Teardown(why); }

void Tick(game::Entity* local, bool local_active, double now_ms) noexcept {
    g_now = now_ms;
    // Publish Sam's model to the detour (0 while asleep / loading / absent).
    std::uintptr_t sm = 0;
    if (g_hook && local && local_active) sm = ModelOf(reinterpret_cast<std::uintptr_t>(local));
    if (sm != g_sam_sm.load(std::memory_order_relaxed)) {
        std::uintptr_t skel = 0;
        if (sm) {
            const SkelInfo s = PoseSkeleton(sm);
            if (s.skel && s.count == static_cast<std::int32_t>(kJ) && s.layout == wire::kSamLayoutHash) skel = s.skel;
            else sm = 0;
        }
        g_sam_skel.store(skel, std::memory_order_release);
        g_sam_sm.store(sm, std::memory_order_seq_cst);
        static std::uintptr_t logged = 1;
        if (sm != logged) {
            logged = sm;
            Log("[real] Sam's SkinnedModel for capture: %p (skeleton %p)", reinterpret_cast<void*>(sm), reinterpret_cast<void*>(skel));
        }
    }
    // A debug-owned body drives itself.
    if (g_owner == Owner::debug) {
        char why[200] = {};
        const PollResult r = Poll(now_ms, why, sizeof(why));
        if (r == PollResult::live && g_mirror) {
            Capture c;
            if (g_capture.Read(c)) {
                static wire::JointFrame f;
                FrameFromCapture(c, &f);
                PublishFrame(f);
            }
        }
    }
    // A debug body goes when Sam is not up for 1 s (load, cutscene, knot space), like the partner's
    // (player_sync TeardownTriggers): nothing of ours stays outstanding across a world change.
    static double asleep_since = 0;
    if (local_active) asleep_since = 0;
    else if (asleep_since == 0) asleep_since = now_ms > 0 ? now_ms : 1;
    if (g_owner == Owner::debug && asleep_since > 0 && now_ms - asleep_since >= 1000) Teardown("Sam asleep (debug body)");
    DriveModel(local_active);
    if (g_hook) Stats(now_ms);
}

bool CaptureFrame(wire::JointFrame* out) noexcept {
    Capture c;
    if (!g_capture.Read(c) || GetTickCount64() - c.tick_ms > 250) return false;
    return FrameFromCapture(c, out);
}

void PushFrame(const wire::JointFrame& f, double key_ms) noexcept {
    if (f.joint_count != kJ || f.layout_hash != wire::kSamLayoutHash) return;
    if (g_ring_count && key_ms <= RingAt(g_ring_count - 1).key_ms) return;
    Slot& s = g_ring[(g_ring_head + g_ring_count) % kRing];
    g_last_push_tick = GetTickCount64();
    s.key_ms = key_ms;
    s.f = f;
    if (g_ring_count < kRing) ++g_ring_count;
    else g_ring_head = (g_ring_head + 1) % kRing;
}

void ResetFrames() noexcept { g_ring_head = g_ring_count = 0; }

bool StarvedOfFrames(double limit_ms) noexcept {
    if (g_phase != Phase::live || !g_live_tick) return false;
    const ULONGLONG since = g_last_push_tick > g_live_tick ? g_last_push_tick : g_live_tick;
    return static_cast<double>(GetTickCount64() - since) > limit_ms;
}

bool SamplePublish(double play_ms) noexcept {
    if (!g_ring_count) return false;
    static wire::JointFrame out;
    const Slot& newest = RingAt(g_ring_count - 1);
    const Slot& oldest = RingAt(0);
    if (play_ms >= newest.key_ms || g_ring_count == 1) {
        out = newest.f;
    } else if (play_ms <= oldest.key_ms) {
        out = oldest.f;
    } else {
        unsigned i = g_ring_count - 1;
        while (i > 0 && RingAt(i - 1).key_ms > play_ms) --i;
        const Slot& a = RingAt(i - 1);
        const Slot& b = RingAt(i);
        const double span = b.key_ms - a.key_ms;
        wire::BlendJoints(a.f, b.f, span > 0 ? static_cast<float>((play_ms - a.key_ms) / span) : 1.0f, out);
    }
    PublishFrame(out);
    return true;
}

bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    game::Entity* sam = nullptr;
    game::WorldTransform swt{};
    const bool have_sam = game::TryGetLocalPlayer(&sam) && sam && game::TryGetTransform(sam, &swt);
    switch (op) {
        case 1: {  // probe: the player's resource reference (path + GUID order), the manager, the models
            if (!have_sam) {
                Set(msg, cap, "no local player");
                return false;
            }
            ResolveSpawn();
            RefInfo r;
            const std::uintptr_t mgr = Manager(sam, &r);
            std::uint8_t want[16];
            bodies::ParseGuid("9b58ada6-67ab-5734-b14a-43b6a94bd838", want);  // ds_player_character (game-data-index)
            const bool guid_ok = std::memcmp(r.guid, want, 16) == 0;
            std::uint8_t stop = 0;
            if (mgr) Rd(mgr + 0x99, stop);
            Log("[real] probe: player resource ref box %p proxy %p flags %016llX (prio %d, loaded %d) object %p (%s) refs %d",
                reinterpret_cast<void*>(r.box), reinterpret_cast<void*>(r.proxy), static_cast<unsigned long long>(r.flags), r.prio,
                r.prio < 0 ? 1 : 0, reinterpret_cast<void*>(r.obj),
                r.obj ? (partner::TypeName(reinterpret_cast<const void*>(r.obj)) ? partner::TypeName(reinterpret_cast<const void*>(r.obj)) : "?") : "-",
                static_cast<int>(Q(r.proxy + 0x28) & 0xFFFFFFFF));
            Log("[real] probe: path '%s' guid raw %s (as text %s): %s ds_player_character's decoded GUID", r.path.c_str(),
                [&] {
                    static char b[64];
                    for (int i = 0; i < 16; ++i) std::snprintf(b + i * 3, 4, "%02x ", r.guid[i]);
                    return b;
                }(),
                GuidText(r.guid).c_str(), guid_ok ? "MATCHES" : "does NOT match");
            Log("[real] probe: streaming manager %p vtable %s, +0x99 (refuse loads) = %u; spawn functions %s", reinterpret_cast<void*>(mgr),
                mgr ? "ok" : "not the expected one", stop, g_fn.spawn_ok ? "verified" : g_fn.why);
            const std::uintptr_t sm = ModelOf(reinterpret_cast<std::uintptr_t>(sam));
            const SkelInfo s = PoseSkeleton(sm);
            std::int32_t idx = -1;
            Rd(sm + kSmPoseIdx, idx);
            Log("[real] probe: Sam's model %p pose index %d skeleton %p joints %d layout %08X; hook %s; captures so far %u", reinterpret_cast<void*>(sm),
                idx, reinterpret_cast<void*>(s.skel), s.count, s.layout, g_hook ? "installed" : "NOT installed", g_ct.captures.load());
            Set(msg, cap, "player ref '%s' guid %s; manager %s", r.path.c_str(), guid_ok ? "matches (bytes_le order)" : "MISMATCH",
                mgr ? "ok" : "missing");
            return guid_ok && mgr;
        }
        case 2: {  // spawn <body id> <dist> <mirror>
            const bodies::Body* b = bodies::FindById(static_cast<std::uint8_t>(v[0]));
            if (!b) {
                Set(msg, cap, "unknown body id %.0f (1..%zu)", v[0], bodies::Count());
                return false;
            }
            if (!have_sam) {
                Set(msg, cap, "no local player");
                return false;
            }
            if (g_owner == Owner::partner) {
                Set(msg, cap, "the partner owns the body slot");
                return false;
            }
            if (g_phase != Phase::idle) Teardown("debug respawn");
            const double dist = v[1] != 0 ? v[1] : 4.0;  // < 0: behind Sam, facing the way he faces
            const float* fw = swt.rot.m[1];
            double fx = fw[0], fy = fw[1];
            const double fl = std::sqrt(fx * fx + fy * fy);
            if (fl < 1e-3) fx = 1, fy = 0;
            else fx /= fl, fy /= fl;
            game::WorldTransform at = swt;
            at.pos.x += fx * dist;
            at.pos.y += fy * dist;
            if (dist > 0)
                for (int r = 0; r < 2; ++r)
                    for (int c = 0; c < 3; ++c) at.rot.m[r][c] = -swt.rot.m[r][c];  // facing Sam
            char why[200] = {};
            const BeginResult br = Begin(*b, at, Owner::debug, g_now, why, sizeof(why));
            g_mirror = v[2] != 0;
            Set(msg, cap, "%s '%s' %.1f m ahead (mirror %s): %s", br == BeginResult::started ? "spawning" : "NOT spawning", b->key, dist,
                g_mirror ? "on" : "off", why);
            return br == BeginResult::started;
        }
        case 3: {
            const bool had = g_owner == Owner::debug && Teardown("debug remove");
            Set(msg, cap, had ? "removed" : "no debug body");
            return true;
        }
        case 4: {  // joint names of Sam and the live body
            const SkelInfo ss = PoseSkeleton(have_sam ? ModelOf(reinterpret_cast<std::uintptr_t>(sam)) : 0);
            const SkelInfo ps = PoseSkeleton(g_live_sm);
            std::string line;
            unsigned same = 0;
            for (unsigned j = 0; j < kJ; ++j) {
                const std::string a = JointName(ss, j), bn = ps.skel ? JointName(ps, j) : "";
                if (a == bn) ++same;
                char cell[160];
                std::snprintf(cell, sizeof(cell), "%u:%s%s%s", j, a.c_str(), ps.skel && a != bn ? "/" : "", ps.skel && a != bn ? bn.c_str() : "");
                line += line.empty() ? "" : " ";
                line += cell;
                if (line.size() > 300 || j + 1 == kJ) {
                    Log("[real] joints %s", line.c_str());
                    line.clear();
                }
            }
            Set(msg, cap, "Sam skeleton %d joints; body %s: %u of 77 names identical in order", ss.count, ps.skel ? "live" : "none", same);
            return ss.skel != 0;
        }
        case 5: {
            char check[96] = {};
            PartnerCheck(check, sizeof(check));
            Set(msg, cap, "hook %d sam_sm %p partner_sm %p phase %s owner %d latch %d(%s) cap=%u app=%u rej=%u torn=%u aba=%u%s", g_hook ? 1 : 0,
                reinterpret_cast<void*>(g_sam_sm.load()), reinterpret_cast<void*>(g_partner_sm.load()), PhaseName(), static_cast<int>(g_owner),
                g_latch_state.load(), g_latch_ok ? "own t" : "Sam t", g_ct.captures.load(), g_ct.applies.load(), g_ct.rejects.load(),
                g_ct.torn.load(), g_ct.aba.load(), check);
            Log("[real] stats: %s", msg);
            return true;
        }
        case 7: {  // control: collectors item #v[0] (its path + GUID) through the same fake-proxy route
            if (!have_sam) {
                Set(msg, cap, "no local player");
                return false;
            }
            std::uintptr_t db = 0;
            std::vector<holo::Item> items;
            if (!holo::ReadItems(&db, &items)) {
                Set(msg, cap, "collectors DB not readable");
                return false;
            }
            const int want = static_cast<int>(v[0]);
            int seen = 0;
            const holo::Item* pick = nullptr;
            for (const holo::Item& it : items) {
                if (it.empty || it.loaded) continue;  // registered but streamed out: a real load
                if (seen++ == want) {
                    pick = &it;
                    break;
                }
            }
            RefInfo r;
            if (!pick || !ReadRef(pick->ptr + 0x80, &r) || r.path.empty()) {
                Set(msg, cap, "no streamed-out collectors item #%d", want);
                return false;
            }
            static char path[160], guid[40];
            std::snprintf(path, sizeof(path), "%s", r.path.c_str());
            std::snprintf(guid, sizeof(guid), "%s", GuidText(r.guid).c_str());
            static bodies::Body control{31, "control", "collectors item (control)", path, guid};
            g_refused_mask &= ~(1u << 31);
            if (g_owner == Owner::partner) {
                Set(msg, cap, "the partner owns the body slot");
                return false;
            }
            if (g_phase != Phase::idle) Teardown("debug respawn");
            game::WorldTransform at = swt;
            at.pos.x += swt.rot.m[1][0] * 4.0;
            at.pos.y += swt.rot.m[1][1] * 4.0;
            char why[200] = {};
            const BeginResult br = Begin(control, at, Owner::debug, g_now, why, sizeof(why));
            Set(msg, cap, "control item %08X '%s' %s: %s", pick->hash, path, br == BeginResult::started ? "spawning" : "NOT spawning", why);
            return br == BeginResult::started;
        }
        case 6: {  // mirror on/off for the debug body
            g_mirror = v[0] != 0;
            g_apply_on.store(g_phase == Phase::live && (g_owner == Owner::partner || g_mirror), std::memory_order_release);
            Set(msg, cap, "mirror %s", g_mirror ? "on" : "off");
            return true;
        }
        default:
            Set(msg, cap, "unknown op %d", op);
            return false;
    }
}

}  // namespace st::realbody
