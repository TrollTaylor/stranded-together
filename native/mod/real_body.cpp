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
#include "automation.hpp"
#include "partner_outfit.hpp"
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

// Self view (docs/real-partner-body.md section 8): the local Sam's mesh-visibility controller.
// DSPlayerEntity +0xA60 is the controller (its per-frame update is called with lea rcx,[rdi+0xA60]
// at 0x1427313D4); controller +0 is the entity, +0x338 the Root hide-reason mask (u64, any bit set
// hides every ArtPart of Sam, and the inventory component's visual update then hides the
// backpack sub-entities too: IsRootVisible 0x1426A0DE0, its only caller 0x1427F0B43). Photo mode's
// "hide Sam" sets reason 26 there (0x1426C1770); the game itself uses reasons 0, 1, 3, 8, 0xC,
// 0x17, 0x1D, 0x1E, 26, 27. Ours is 63.
constexpr std::uintptr_t kPlayerVtable = 0x3D7D0D8;   // DSPlayerEntity vtable ([+0] = 0x142711B40)
constexpr std::uintptr_t kPlayerVt0 = 0x2711B40;
constexpr std::uintptr_t kSetRootVisible = 0x26B4E40; // bts/btr of reason & 63 in this+0x338
constexpr std::uintptr_t kApplyMeshControl = 0x26D9120;
constexpr std::uintptr_t kMeshCtlCall = 0x27313D4;    // lea rcx,[rdi+0xA60]; vmovaps; call 0x1426C16D0
constexpr std::uintptr_t kMeshCtlUpdate = 0x26C16D0;
constexpr std::uintptr_t kPhotoHide = 0x26C1770;      // mov rcx,[rdx+0xD98]; btr/bts 26
constexpr std::uintptr_t kIsRootVisible = 0x26A0DE0;
constexpr std::uintptr_t kInventoryRootCheck = 0x27F0B40;  // mov rcx,rbx; call IsRootVisible; test al,al
constexpr std::uintptr_t kSetWorldTransform = 0x2361EA0;   // Entity_ExportedSetWorldTransform(e, wt)
constexpr std::uintptr_t kPlayerMeshCtl = 0xA60;
constexpr std::uintptr_t kPlayerRootMask = 0xA60 + 0x338;  // 0xD98
constexpr std::uint64_t kOurReason = 1ull << 63;

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
constexpr std::uintptr_t kEntResRef = 0x58, kEntModel = 0xB8, kEntWorld = 0xC8;
constexpr std::size_t kWorldBytes = 0x3C;  // +0xC8 position (3 doubles), +0xE0 rotation (3x3 floats)

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
    std::int64_t qpc = 0;       // QueryPerformanceCounter when captured (self view: pose age)
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
SeqLock<ApplyBuf> g_apply;  // the partner slot's published frame

// ---------------------------------------------------------------- detour state
using AnimateFn = void (*)(void* sm, float dt);
using PoseFn = void (*)(void* pose);
using BoundsFn = void (*)(void* pose, void* boxes, void* aabb);
using SetWtFn = void (*)(void* e, const void* wt);
AnimateFn g_orig = nullptr;
std::uintptr_t g_fn_model = 0, g_fn_bounds = 0, g_sm_vt = 0;
bool g_hook = false;
double g_qpc_per_ms = 0;

std::atomic<std::uintptr_t> g_sam_sm{0};
std::atomic<std::uintptr_t> g_sam_e{0};  // the entity of g_sam_sm (self view: its world transform)
std::atomic<std::uintptr_t> g_sam_skel{0};
std::atomic<int> g_inflight{0};
// The game does not update a cutscene-actor body's model by itself outside a sequence (run
// 20261116-183736: 10-13 updates after spawn, then none; it stays in the bind pose), so the
// tick drives it with the game's own forced model update. Detour calls for a body that do not
// come from that call are counted, and the tick only forces when there were none.
thread_local bool t_forcing = false;
std::uintptr_t g_fn_forced = 0;

struct Counters {
    std::atomic<std::uint32_t> calls{0}, captures{0}, applies{0}, rejects{0}, torn{0}, wpm_fail{0}, faults{0}, aba{0},
        observed{0}, nocap{0};  // nocap: the self view found no captured Sam pose to copy (not a torn read)
    std::atomic<std::int32_t> last_reject{0};
};

// ---------------------------------------------------------------- body slots
// Slot 0: the partner (or a debug body). Slot 1: the self view (this player's own character
// shown in place of the local Sam on this screen).
struct BodySlot {
    const char* tag = "real";  // log prefix
    // Published to the detour (game thread writes, detour reads).
    std::atomic<std::uintptr_t> sm{0};
    std::atomic<std::uintptr_t> e{0};
    std::atomic<std::uintptr_t> skel{0};
    std::uintptr_t e_vt = 0;  // the entity's vtable (set before `e` is published): the detour's ABA check
    std::atomic<bool> apply_on{false};  // override enabled (a debug body with mirror off only observes)
    std::atomic<std::uint32_t> game_updates{0};
    Counters ct;
    // Set by the game thread before `sm` is published (release), read by the detour.
    std::uint8_t map[kJ] = {};  // body joint -> Sam joint
    // Latch of the body's own pose (detour writes while latch_state == 0).
    std::atomic<int> latch_state{0};  // 0 observing, 1 done
    Joint latch[kJ] = {};
    bool latch_ok = false;
    float pelvis_ratio = 1.0f;
    unsigned latch_good = 0, latch_seen = 0;
    float latch_pelvis_len = 0, latch_sam_pelvis_len = 0;
    bool same_skel = false;  // set by Validate before the body is published
    float leg_body = 0, leg_sam = 0, leg_ratio = 0;  // 0 = not measured
    bool leg_used = false;                          // the pelvis ratio came from the legs
    // Game thread.
    Owner owner = Owner::none;
    Phase phase = Phase::idle;
    holo::Spawn spawn;
    const bodies::Body* body = nullptr;
    double started_ms = 0;
    double next_progress_ms = 0;
    std::uint8_t guid[16] = {};
    std::uintptr_t live_sm = 0;
    bool mirror = false;  // debug body: copy Sam's live pose (no delay)
    bool start_hidden = false;  // self view: invisible until posed (Show)
    ULONGLONG live_tick = 0;    // GetTickCount64 when the body became live
    bool logged_latch = false;
    // DriveModel
    ULONGLONG last_game_ms = 0;
    std::uint32_t forced = 0, forced_skipped = 0, forced_failed = 0;
    std::uint32_t prev_forced = 0, prev_skipped = 0;
    bool forced_off = false;
    std::uint32_t refused = 0;  // bit per body id that failed to load / validate in THIS slot (this session)
};
BodySlot g_slots[2];
BodySlot& g_p = g_slots[0];   // partner / debug
BodySlot& g_me = g_slots[1];  // self view
Counters& g_ct = g_slots[0].ct;  // also counts Sam's captures and every detour call

// Self view, detour side.
// How the self body follows Sam: 1 = Sam's own model update copies his world transform onto
// the body (the frame's transform, no tick jitter), 2 = the body's own model update copies it,
// 3 = the tick only (like the partner). Default 1.
std::atomic<int> g_self_follow{1};
std::uintptr_t g_fn_setwt = 0;  // Entity_ExportedSetWorldTransform, byte-checked (CheckMeshSites)
std::atomic<std::uint32_t> g_self_copies{0}, g_self_copy_fail{0}, g_self_copy_aba{0}, g_self_stale{0};
std::atomic<std::uint64_t> g_self_age_us{0}, g_self_age_n{0}, g_self_age_max_us{0};
std::atomic<std::uint32_t> g_self_last_frame{0};
std::atomic<ULONGLONG> g_self_last_apply{0};  // GetTickCount64 of the last pose written onto the self body
std::atomic<ULONGLONG> g_self_last_copy{0};   // ... of the last transform copied in a detour

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

std::int64_t Qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
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
    c.qpc = Qpc();
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

bool CallSetWt(std::uintptr_t e, const void* wt) {
    __try {
        reinterpret_cast<SetWtFn>(g_fn_setwt)(reinterpret_cast<void*>(e), wt);
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

// Thigh + shin (the local translations of the knee and ankle joints, both legs): bone lengths,
// which do not change with the pose. Sam-skeleton joint indices (the names logged by real_names).
constexpr unsigned kSamLegJoints[4] = {62, 63, 67, 68};  // R knee, R foot, L knee, L foot

float LegLength(const Joint (&j)[kJ], const unsigned (&idx)[4]) {
    float sum = 0;
    for (unsigned i : idx) {
        const float* t = j[i].t;
        sum += std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    }
    return sum / 2;  // per leg
}

// Runs in the detour: math only (no logging, no allocation).
void FinishLatch(BodySlot& b) {
    b.latch_ok = b.latch_good > 0;
    b.pelvis_ratio = 1.0f;
    b.latch_sam_pelvis_len = kSamRestPelvisLen;
    b.leg_body = b.leg_sam = b.leg_ratio = 0;
    b.leg_used = false;
    if (b.latch_ok && !b.same_skel) {
        float r = b.latch_pelvis_len / kSamRestPelvisLen;
        // The latch assumes the body's first frames are a standing rest pose. Heartman's are not:
        // pelvis 0.619 m (seated, run 20261116-194938), so the ratio clamped to 0.75 sank his feet
        // into the floor (run 20261116-194441). The leg bones give the scale whatever the pose; when
        // the two disagree by more than 8 % the legs win.
        unsigned body_idx[4] = {};
        bool mapped = true;
        for (unsigned k = 0; k < 4; ++k) {
            unsigned found = kJ;
            for (unsigned j = 0; j < kJ && found == kJ; ++j)
                if (b.map[j] == kSamLegJoints[k]) found = j;
            mapped = mapped && found < kJ;
            body_idx[k] = found < kJ ? found : 0;
        }
        Capture c;
        if (mapped && g_capture.Read(c)) {
            b.leg_body = LegLength(b.latch, body_idx);
            b.leg_sam = LegLength(c.j, kSamLegJoints);
            if (b.leg_body > 0.3f && b.leg_sam > 0.3f) {
                b.leg_ratio = b.leg_body / b.leg_sam;
                if (std::fabs(r / b.leg_ratio - 1.0f) > 0.08f) {
                    r = b.leg_ratio;
                    b.leg_used = true;
                }
            }
        }
        if (r < 0.75f) r = 0.75f;
        if (r > 1.33f) r = 1.33f;
        b.pelvis_ratio = r;
    }
    b.latch_state.store(1, std::memory_order_release);
}

// Self view: Sam's world transform (read fault-free, twice for a consistent copy) onto the self
// body, through the game's own exported setter (the body's transform lock and dirty flags).
void CopySamTransform() {
    const std::uintptr_t self_e = g_me.e.load(std::memory_order_seq_cst);
    const std::uintptr_t self_sm = g_me.sm.load(std::memory_order_seq_cst);
    const std::uintptr_t sam = g_sam_e.load(std::memory_order_acquire);
    if (!self_e || !self_sm || !sam || !g_fn_setwt) return;
    // ABA, as ApplyBody: the game may have freed the body (world streaming, a scripted cleanup)
    // since the tick last saw it. Only an entity that still has the vtable it was validated with
    // and still owns the published model (model vtable + back pointer) gets the game call.
    std::uintptr_t ev = 0, em = 0, smv = 0, sme = 0;
    if (!Rd(self_e, ev) || ev != g_me.e_vt || !Rd(self_e + kEntModel, em) || em != self_sm || !Rd(self_sm, smv) || smv != g_sm_vt ||
        !Rd(self_sm + kSmEntity, sme) || sme != self_e) {
        g_self_copy_aba.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    alignas(8) std::uint8_t a[kWorldBytes + 4] = {}, b2[kWorldBytes + 4] = {};
    if (!mem::Read(sam + kEntWorld, a, kWorldBytes) || !mem::Read(sam + kEntWorld, b2, kWorldBytes) ||
        std::memcmp(a, b2, kWorldBytes) != 0) {
        g_self_copy_fail.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    double p[3];
    std::memcpy(p, a, sizeof(p));
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
        g_self_copy_fail.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!CallSetWt(self_e, a)) {
        g_self_copy_fail.fetch_add(1, std::memory_order_relaxed);
        g_self_follow.store(3, std::memory_order_relaxed);  // never again from a detour this run
        return;
    }
    g_self_copies.fetch_add(1, std::memory_order_relaxed);
    g_self_last_copy.store(GetTickCount64(), std::memory_order_relaxed);
}

// Overwrites the body's finished pose. `live`: the self view, straight from the newest capture of
// the local Sam (no wire, no ring, no delay); otherwise the partner's published frame.
void ApplyBody(BodySlot& b, std::uintptr_t sm, bool live) {
    Counters& ct = b.ct;
    // ABA: the game may have freed our body and reused the memory for another model.
    std::uintptr_t id[2] = {};
    if (!mem::Read(sm, id, sizeof(id)) || id[0] != g_sm_vt) {
        ct.aba.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uintptr_t owner = 0;
    if (!Rd(sm + kSmEntity, owner) || owner != b.e.load(std::memory_order_acquire)) {
        ct.aba.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0, local = 0;
    if (!ReadPoseHdr(sm, hdr, &pose)) return;
    const int bad = CheckHdr(hdr, b.skel.load(std::memory_order_acquire), &local);
    if (bad) {
        ct.rejects.fetch_add(1, std::memory_order_relaxed);
        ct.last_reject.store(20 + bad, std::memory_order_relaxed);
        return;
    }
    Joint cur[kJ];
    if (b.latch_state.load(std::memory_order_acquire) == 0) {
        ct.observed.fetch_add(1, std::memory_order_relaxed);
        if (mem::Read(local, cur, sizeof(cur))) {
            float pl = 0;
            ++b.latch_seen;
            if (Plausible(cur, &pl)) {
                std::memcpy(b.latch, cur, sizeof(b.latch));
                b.latch_pelvis_len = pl;
                ++b.latch_good;
            }
        }
        if (b.latch_good >= kLatchGood || b.latch_seen >= kLatchMax) FinishLatch(b);
        return;
    }
    if (!b.apply_on.load(std::memory_order_acquire)) return;
    if (live) {
        Capture c;
        if (!g_capture.Read(c)) {
            ct.nocap.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        for (unsigned j = 0; j < kJ; ++j) {
            const unsigned sj = b.map[j];
            Joint& o = cur[j];
            std::memcpy(o.q, c.j[sj].q, sizeof(o.q));
            const Joint& src = b.latch_ok ? b.latch[j] : c.j[sj];
            std::memcpy(o.t, src.t, sizeof(o.t));
            std::memcpy(o.s, src.s, sizeof(o.s));
            if (sj == kSamRoot) {
                o.t[0] = c.j[kSamRoot].t[0], o.t[1] = c.j[kSamRoot].t[1], o.t[2] = c.j[kSamRoot].t[2];
            } else if (sj == kSamPelvis) {
                const float r = b.pelvis_ratio;
                o.t[0] = c.j[kSamPelvis].t[0] * r, o.t[1] = c.j[kSamPelvis].t[1] * r, o.t[2] = c.j[kSamPelvis].t[2] * r;
            }
        }
        // How old the pose is when it lands on the body (same frame: well under a frame time).
        if (g_qpc_per_ms > 0) {
            const double age_ms = static_cast<double>(Qpc() - c.qpc) / g_qpc_per_ms;
            const std::uint64_t us = age_ms > 0 ? static_cast<std::uint64_t>(age_ms * 1000.0) : 0;
            g_self_age_us.fetch_add(us, std::memory_order_relaxed);
            g_self_age_n.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t mx = g_self_age_max_us.load(std::memory_order_relaxed);
            while (us > mx && !g_self_age_max_us.compare_exchange_weak(mx, us, std::memory_order_relaxed)) {
            }
        }
        if (g_self_last_frame.exchange(c.frame, std::memory_order_relaxed) == c.frame)
            g_self_stale.fetch_add(1, std::memory_order_relaxed);
    } else {
        ApplyBuf a;
        if (!g_apply.Read(a)) {
            ct.torn.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        for (unsigned j = 0; j < kJ; ++j) {
            const unsigned sj = b.map[j];
            Joint& o = cur[j];
            o.q[0] = a.q[sj].x, o.q[1] = a.q[sj].y, o.q[2] = a.q[sj].z, o.q[3] = a.q[sj].w;
            const Joint& src = b.latch_ok ? b.latch[j] : a.fb[sj];
            std::memcpy(o.t, src.t, sizeof(o.t));
            std::memcpy(o.s, src.s, sizeof(o.s));
            if (sj == kSamRoot) {
                o.t[0] = a.troot[0], o.t[1] = a.troot[1], o.t[2] = a.troot[2];
            } else if (sj == kSamPelvis) {
                o.t[0] = a.tpelvis[0] * b.pelvis_ratio, o.t[1] = a.tpelvis[1] * b.pelvis_ratio,
                o.t[2] = a.tpelvis[2] * b.pelvis_ratio;
            }
        }
    }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(local), cur, sizeof(cur), &wrote) || wrote != sizeof(cur)) {
        ct.wpm_fail.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::int32_t bounds_on = 0;
    Rd(sm + kSmBoundsOn, bounds_on);
    if (!CallPoseFns(sm, pose, bounds_on != 0)) {
        ct.faults.fetch_add(1, std::memory_order_relaxed);
        b.apply_on.store(false, std::memory_order_release);  // never again this run
        return;
    }
    ct.applies.fetch_add(1, std::memory_order_relaxed);
    if (live) g_self_last_apply.store(GetTickCount64(), std::memory_order_relaxed);
}

void Detour(void* sm, float dt) {
    g_orig(sm, dt);
    const auto s = reinterpret_cast<std::uintptr_t>(sm);
    if (!s || (s != g_sam_sm.load(std::memory_order_relaxed) && s != g_p.sm.load(std::memory_order_relaxed) &&
               s != g_me.sm.load(std::memory_order_relaxed)))
        return;
    g_inflight.fetch_add(1, std::memory_order_seq_cst);
    g_ct.calls.fetch_add(1, std::memory_order_relaxed);
    if (s == g_sam_sm.load(std::memory_order_seq_cst)) {
        CaptureSam(s);
        if (g_self_follow.load(std::memory_order_relaxed) == 1) CopySamTransform();
    } else if (s == g_p.sm.load(std::memory_order_seq_cst)) {
        if (!t_forcing) g_p.game_updates.fetch_add(1, std::memory_order_relaxed);
        ApplyBody(g_p, s, false);
    } else if (s == g_me.sm.load(std::memory_order_seq_cst)) {
        if (!t_forcing) g_me.game_updates.fetch_add(1, std::memory_order_relaxed);
        if (g_self_follow.load(std::memory_order_relaxed) == 2) CopySamTransform();
        ApplyBody(g_me, s, true);
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

bool Check(const char* name, std::uintptr_t rva, const char* sig, char* why = g_fn.why, std::size_t cap = sizeof(g_fn.why)) {
    if (mem::CodeMatches(mem::Base() + rva, sig)) return true;
    std::snprintf(why, cap, "%s at ds.exe+0x%llX does not match", name, static_cast<unsigned long long>(rva));
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

// Self view: the code that proves the player's mesh controller offsets, and the transform setter.
struct MeshFns {
    bool checked = false, ok = false;
    char why[200] = "not checked";
    // IsRootVisible's second condition: a counter a scripted scene raises while it hides the player
    // (writer 0x1424B3EF0: +1 on hide, -1 on show, for the entity in 0x147BD1A40). Non-zero hides
    // Sam's whole model whatever the mask says, so the self body is not shown then either.
    std::uintptr_t scene_hide = 0;
} g_mesh;

bool CheckMeshSites() {
    if (g_mesh.checked) return g_mesh.ok;
    g_mesh.checked = true;
    const std::uintptr_t b = mem::Base();
    char* w = g_mesh.why;
    const std::size_t cap = sizeof(g_mesh.why);
    bool ok = Check("SetRootVisible", kSetRootVisible,
                    "4C 8B C9 B8 01 00 00 00 41 0F B6 C8 48 D3 E0 4D 8B 81 38 03 00 00 84 D2 74 0E 48 F7 D0 49 23 C0 49 89 81 38 03 00 "
                    "00 C3 4C 0B C0 4D 89 81 38 03 00 00 C3",
                    w, cap) &&
              Check("ApplyMeshControl", kApplyMeshControl, "48 89 5C 24 20 57 48 83 EC 20 33 FF 48 8B D9 F6 81 98 3A 00 00 02 75 7D",
                    w, cap) &&
              Check("mesh control update", kMeshCtlUpdate, "48 89 5C 24 10 57 48 83 EC 50 4C 8B 01 33 DB C5 F8 29 74 24 40 48 8B F9", w,
                    cap) &&
              Check("player mesh control call (+0xA60)", kMeshCtlCall, "48 8D 8F 60 0A 00 00 C5 F8 28 CE E8", w, cap) &&
              Check("photo-mode hide (+0xD98 bit 26)", kPhotoHide, "48 8B 8A 98 0D 00 00 48 8B C1 48 0F BA F1 1A 48 0F BA E8 1A", w,
                    cap) &&
              Check("IsRootVisible", kIsRootVisible, "48 83 B9 38 03 00 00 00 75 0C 83 3D ?? ?? ?? ?? 00 75 03 B0 01 C3 32 C0", w, cap) &&
              Check("inventory Root check", kInventoryRootCheck, "48 8B CB E8 ?? ?? ?? ?? 84 C0 75", w, cap) &&
              Check("Entity_ExportedSetWorldTransform", kSetWorldTransform,
                    "48 85 C9 74 1C 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 33 D2 48 8B CB E8", w, cap);
    if (ok && (mem::RipTarget(b + kMeshCtlCall + 11, 1, 5) != b + kMeshCtlUpdate ||
               mem::RipTarget(b + kInventoryRootCheck + 3, 1, 5) != b + kIsRootVisible)) {
        std::snprintf(w, cap, "the mesh-control / IsRootVisible calls do not lead where expected");
        ok = false;
    }
    if (ok) {
        g_mesh.scene_hide = mem::RipTarget(b + kIsRootVisible + 10, 2, 7);  // cmp dword [rip+x],0
        std::int32_t v = 0;
        if (!g_mesh.scene_hide || !Rd(g_mesh.scene_hide, v)) {
            std::snprintf(w, cap, "IsRootVisible's scene-hide counter is not readable");
            ok = false;
        }
    }
    if (ok && Q(b + kPlayerVtable) != b + kPlayerVt0) {
        std::snprintf(w, cap, "DSPlayerEntity vtable at ds.exe+0x%llX does not match", static_cast<unsigned long long>(kPlayerVtable));
        ok = false;
    }
    if (ok) {
        g_fn_setwt = b + kSetWorldTransform;
        std::snprintf(w, cap, "ok");
    }
    if (ok)
        Log("[self] mesh-control sites verified (player +0xA60 controller, Root hide mask +0xD98, reason 63 is ours; scene-hide "
            "counter ds.exe+0x%llX)",
            static_cast<unsigned long long>(g_mesh.scene_hide - b));
    else Log("[self] mesh-control sites NOT verified: %s", w);
    g_mesh.ok = ok;
    return ok;
}

// The address of the local Sam's Root hide-reason mask, or 0 when `sam` is not a live
// DSPlayerEntity whose controller points back at it.
std::uintptr_t SamMaskAddr(std::uintptr_t sam) {
    if (!sam || !g_mesh.ok) return 0;
    if (Q(sam) != mem::Base() + kPlayerVtable) return 0;
    if (Q(sam + kPlayerMeshCtl) != sam) return 0;
    std::uint64_t m = 0;
    if (!Rd(sam + kPlayerRootMask, m)) return 0;
    return sam + kPlayerRootMask;
}

// Sets / clears only our reason bit with a locked operation: a game bit can never be lost. (The
// game's own SetRootVisible is a plain read-modify-write, so ours can be: a lost SET is re-asserted
// every tick while shown, a lost CLEAR is repaired every tick while not shown, SelfRepair.) The
// entity is checked with fault-free reads first; the locked write itself is direct, so an entity
// freed on another thread between the check and the write is a small accepted window (the change-
// of-player and asleep paths).
bool SetOurReason(std::uintptr_t sam, bool on, std::uint64_t* mask_out = nullptr) {
    const std::uintptr_t a = SamMaskAddr(sam);
    if (!a) return false;
    std::uint64_t m = 0;
    if (!Rd(a, m)) return false;
    auto* p = reinterpret_cast<volatile LONG64*>(a);
    if (on && !(m & kOurReason)) InterlockedOr64(p, static_cast<LONG64>(kOurReason));
    if (!on && (m & kOurReason)) InterlockedAnd64(p, static_cast<LONG64>(~kOurReason));
    if (mask_out) Rd(a, *mask_out);
    return true;
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
void LogSpawnerRef(const char* tag, void* spv, const char* when) {
    const auto sp = reinterpret_cast<std::uintptr_t>(spv);
    RefInfo r;
    std::uint8_t fl[3] = {};
    mem::Read(sp + 0x168, fl, sizeof(fl));
    if (!sp || !ReadRef(sp + 0x188, &r)) {
        Log("[%s]   %s: spawner %p flags loaded=%u cancelled=%u job=%u; its reference is unreadable", tag, when, spv, fl[0], fl[1], fl[2]);
        return;
    }
    std::int32_t regs = 0;
    Rd(r.proxy + 0x28, regs);
    const char* on = r.obj ? partner::TypeName(reinterpret_cast<const void*>(r.obj)) : nullptr;
    Log("[%s]   %s: spawner %p flags loaded=%u cancelled=%u job=%u; ref box %p proxy %p '%s' guid %s box flags %016llX (prio %d) "
        "object %p (%s) registrations %d",
        tag, when, spv, fl[0], fl[1], fl[2], reinterpret_cast<void*>(r.box), reinterpret_cast<void*>(r.proxy), r.path.c_str(),
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

// ---------------------------------------------------------------- outfit (game thread)
// The partner Sam dressed like the other player's own Sam (partner_outfit.hpp, docs section 9).
wire::Outfit g_partner_outfit;      // the newest outfit the peer sent (or our own, echoed)
bool g_have_partner_outfit = false;
int g_outfit_override = 0;          // debug: 0 none, 1 our own outfit, 2 g_override_outfit
wire::Outfit g_override_outfit;
outfit::Dress g_dress;              // what was done to the partner slot's body

// ---------------------------------------------------------------- body slot (game thread)
double g_now = 0;                  // the tick's clock (link::NowMsPrecise), for debug-started spawns

void ResetSlot(BodySlot& b) {
    if (&b == &g_slots[0]) outfit::Forget(g_dress);
    b.owner = Owner::none;
    b.phase = Phase::idle;
    b.spawn = holo::Spawn{};
    b.body = nullptr;
    b.live_sm = 0;
    b.mirror = false;
    b.start_hidden = false;
    b.last_game_ms = 0;
}

void Refuse(BodySlot& slot, const bodies::Body* b, const char* why) {
    if (!b || b->id >= 32) return;
    if (!(slot.refused & (1u << b->id)))
        Log("[%s] body '%s' will not be tried again in this slot this session (%s)", slot.tag, b->key, why);
    slot.refused |= 1u << b->id;
}

// Unpublish the body's model and wait until no detour is inside it.
void Unpublish(BodySlot& b) {
    b.apply_on.store(false, std::memory_order_seq_cst);
    b.sm.store(0, std::memory_order_seq_cst);
    b.e.store(0, std::memory_order_seq_cst);
    const ULONGLONG until = GetTickCount64() + 50;
    while (g_inflight.load(std::memory_order_seq_cst) != 0 && GetTickCount64() < until) SwitchToThread();
    if (g_inflight.load(std::memory_order_seq_cst) != 0) Log("[%s] detour still busy after 50 ms (continuing)", b.tag);
}

void SelfUnshow(const char* why);  // self view: Sam back before the body goes

bool Teardown(BodySlot& b, const char* why) {
    if (&b == &g_me) SelfUnshow(why);
    if (b.phase == Phase::idle && !b.spawn.sp) {
        ResetSlot(b);
        return false;
    }
    Unpublish(b);
    char step[160] = {};
    holo::SpawnTeardown(b.spawn, why, step, sizeof(step));
    Log("[%s] teardown (%s): body '%s': %s", b.tag, why, b.body ? b.body->key : "?", step);
    ResetSlot(b);
    return true;
}

// Validates the realized entity and its model, builds the joint map, publishes it.
const char* Validate(BodySlot& b, std::uintptr_t e) {
    const std::uintptr_t rtti = mem::RttiOf(e);
    if (!rtti) return "entity type unknown";
    if (mem::DerivesFrom(rtti, "Humanoid") || mem::DerivesFrom(rtti, "DSPlayerEntity")) return "a Humanoid / player entity";
    RefInfo r;
    if (!ReadRef(e + kEntResRef, &r)) return "entity resource reference unreadable";
    if (std::memcmp(r.guid, b.guid, 16) != 0) return "entity resource GUID differs from the requested body";
    const char* rn = r.obj ? partner::TypeName(reinterpret_cast<const void*>(r.obj)) : nullptr;
    Log("[%s]   resource %p %s path '%s' guid %s", b.tag, reinterpret_cast<void*>(r.obj), rn ? rn : "?", r.path.c_str(),
        GuidText(r.guid).c_str());
    for (const char* bad : {"AIIndividualComponent", "AIBodyComponent", "DSPlayerControllerComponent", "HumanoidController"})
        if (partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), bad)) return "carries AI / controller components";
    const std::uintptr_t sm = ModelOf(e);
    if (!sm) return "no SkinnedModel at +0xB8 (vtable / back pointer)";
    const SkelInfo ps = PoseSkeleton(sm);
    const std::uintptr_t sam_sm = g_sam_sm.load();
    const SkelInfo ss = PoseSkeleton(sam_sm);
    Log("[%s]   model %p: skeleton %p joints %d layout %08X; Sam's skeleton %p joints %d layout %08X%s", b.tag, reinterpret_cast<void*>(sm),
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
            b.map[j] = static_cast<std::uint8_t>(j);
            continue;
        }
        const std::string n = JointName(ps, j);
        int found = -1;
        for (unsigned k = 0; k < kJ && found < 0; ++k)
            if (!n.empty() && sam_names[k] == n) found = static_cast<int>(k);
        if (found < 0) {
            ++missing;
            if (missing <= 4) Log("[%s]   body joint %u '%s' has no match on Sam's skeleton", b.tag, j, n.c_str());
            found = static_cast<int>(j);
        }
        if (found != static_cast<int>(j)) ++diff_order;
        b.map[j] = static_cast<std::uint8_t>(found);
    }
    Log("[%s]   joints: 0 '%s', 1 '%s', 2 '%s', 3 '%s' ... 76 '%s'; mapped by name: %u missing, %u in a different order", b.tag,
        sam_names[0].c_str(), sam_names[1].c_str(), sam_names[2].c_str(), sam_names[3].c_str(), sam_names[76].c_str(), missing,
        diff_order);
    if (missing) return "joint names do not match Sam's skeleton";
    b.live_sm = sm;
    b.same_skel = ps.skel == ss.skel;
    b.latch_good = b.latch_seen = 0;
    b.latch_ok = false;
    b.pelvis_ratio = 1.0f;
    b.latch_state.store(0, std::memory_order_release);
    b.skel.store(ps.skel, std::memory_order_release);
    b.e_vt = Q(e);
    b.e.store(e, std::memory_order_release);
    b.sm.store(sm, std::memory_order_seq_cst);
    return nullptr;
}

BeginResult BeginSlot(BodySlot& slot, const bodies::Body& body, const game::WorldTransform& at, Owner owner, double now_ms, char* why,
                      std::size_t cap) {
    if (slot.phase != Phase::idle) {
        Set(why, cap, "a body is already %s", slot.phase == Phase::live ? "live" : "loading");
        return BeginResult::busy;
    }
    if (!g_hook || !ResolveSpawn()) {
        Set(why, cap, "%s", g_hook ? g_fn.why : "pose hook not installed (partner_body=real or pose_hook=1 at start)");
        return BeginResult::unavailable;
    }
    if (body.id < 32 && (slot.refused & (1u << body.id))) {
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
    Log("[%s] spawning '%s' (%s): path '%s' guid %s; manager %p (reference priority %d%s)", slot.tag, body.key, body.label, body.path,
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
    Log("[%s]   proxy after the spawner ctor: hash %08X (the manager computed it), %s", slot.tag, static_cast<std::uint32_t>(fp->hash), w);
    CallStrDtor(&fp->str);  // Register copied the String (refcounted)
    VirtualFree(fp, 0, MEM_RELEASE);
    if (!ok) {
        Set(why, cap, "spawner: %s", w);
        return holo::Available() ? BeginResult::failed : BeginResult::unavailable;
    }
    LogSpawnerRef(slot.tag, sp.sp, "after StartSpawner");
    slot.spawn = sp;
    slot.owner = owner;
    slot.body = &body;
    std::memcpy(slot.guid, guid, 16);
    slot.phase = Phase::loading;
    slot.started_ms = now_ms;
    slot.next_progress_ms = now_ms + 2000;
    Set(why, cap, "loading '%s'", body.key);
    return BeginResult::started;
}

double g_next_stats_ms = 0;

PollResult PollSlot(BodySlot& slot, double now_ms, char* why, std::size_t cap) {
    if (slot.phase == Phase::idle) return PollResult::idle;
    if (slot.phase == Phase::live) {
        if (holo::SpawnAlive(slot.spawn)) return PollResult::live;
        Set(why, cap, "the real body entity %p was destroyed by the game", slot.spawn.e);
        Log("[%s] %s", slot.tag, why);
        Unpublish(slot);
        slot.spawn.e = nullptr;
        Teardown(slot, "entity gone");
        return PollResult::failed;
    }
    char w[200] = {};
    switch (holo::SpawnPoll(slot.spawn, w, sizeof(w))) {
        case holo::SpawnState::loading:
            if (now_ms - slot.started_ms > kLoadTimeoutMs) {
                const bodies::Body* b = slot.body;
                Set(why, cap, "body '%s' did not load within %.0f s", b ? b->key : "?", kLoadTimeoutMs / 1000);
                Log("[%s] %s", slot.tag, why);
                Teardown(slot, "load timeout");
                Refuse(slot, b, "load timeout");
                return PollResult::failed;
            }
            if (now_ms >= slot.next_progress_ms) {
                slot.next_progress_ms = now_ms + 2000;
                Log("[%s] still loading '%s' (%.1f s)", slot.tag, slot.body ? slot.body->key : "?", (now_ms - slot.started_ms) / 1000);
                LogSpawnerRef(slot.tag, slot.spawn.sp, "loading");
            }
            return PollResult::loading;
        case holo::SpawnState::cancelled: {
            LogSpawnerRef(slot.tag, slot.spawn.sp, "cancelled");
            const bodies::Body* b = slot.body;
            Set(why, cap, "the game cancelled loading '%s'", b ? b->key : "?");
            Log("[%s] %s", slot.tag, why);
            Teardown(slot, "cancelled");
            Refuse(slot, b, "cancelled");
            return PollResult::failed;
        }
        case holo::SpawnState::failed:
            Set(why, cap, "%s", w);
            Log("[%s] spawn failed: %s", slot.tag, w);
            ResetSlot(slot);
            return PollResult::failed;
        case holo::SpawnState::realized:
            break;
    }
    const bodies::Body* b = slot.body;
    const double took = now_ms - slot.started_ms;
    game::Entity* e = slot.spawn.e;
    LogSpawnerRef(slot.tag, slot.spawn.sp, "realized");
    Log("[%s] '%s' loaded after %.2f s; realized entity %p", slot.tag, b ? b->key : "?", took / 1000, e);
    if (!e) {
        Set(why, cap, "'%s' loaded but realized no entity", b ? b->key : "?");
        Teardown(slot, "no entity");
        Refuse(slot, b, "no entity");
        return PollResult::failed;
    }
    partner::DescribeEntity(e, "real body realized");
    if (const char* bad = Validate(slot, reinterpret_cast<std::uintptr_t>(e))) {
        Set(why, cap, "'%s' refused: %s", b ? b->key : "?", bad);
        Log("[%s] %s", slot.tag, why);
        Teardown(slot, "refused");
        Refuse(slot, b, bad);
        return PollResult::failed;
    }
    std::uint64_t f = 0;
    partner::ReadFlags(e, &f);
    bool shown = true, awake = true;
    // A self-view body stays invisible until it is posed (SelfTick shows it and hides Sam in the
    // same tick); every other body is shown at once.
    const bool want_visible = !slot.start_hidden;
    if (((f & 0x2) != 0) != want_visible) shown = partner::SetVisible(e, want_visible);
    awake = partner::WakeIfAsleep(e);
    std::uint64_t f2 = 0;
    partner::ReadFlags(e, &f2);
    Log("[%s] '%s' is live: entity %p model %p flags 0x%llX -> 0x%llX (set %s %s, awake %s)", slot.tag, b ? b->key : "?", e,
        reinterpret_cast<void*>(slot.live_sm), static_cast<unsigned long long>(f), static_cast<unsigned long long>(f2),
        want_visible ? "visible" : "invisible", shown ? "ok" : "unavailable", awake ? "yes" : "no");
    slot.phase = Phase::live;
    slot.live_tick = GetTickCount64();
    slot.logged_latch = false;
    // Without a body the stats ran every 10 s; a body removed within that wait (a debug tour
    // spends about 6 s on each) never got its latch line (run 20261116-194441: 9 of 16 missing).
    if (g_next_stats_ms > now_ms + 1000) g_next_stats_ms = now_ms + 1000;
    slot.apply_on.store(slot.owner == Owner::partner || slot.owner == Owner::self || slot.mirror, std::memory_order_release);
    return PollResult::became_live;
}

// ---------------------------------------------------------------- pose ring (game thread, partner)
struct RingSlot {
    double key_ms;
    wire::JointFrame f;
};
constexpr unsigned kRing = 256;
RingSlot g_ring[kRing];
unsigned g_ring_head = 0, g_ring_count = 0;
std::uint32_t g_apply_seq = 0;
ApplyBuf g_last_published;
bool g_have_published = false;

ULONGLONG g_last_push_tick = 0;  // GetTickCount64 of the newest PushFrame

RingSlot& RingAt(unsigned i) { return g_ring[(g_ring_head + i) % kRing]; }

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

// ---------------------------------------------------------------- self view (game thread)
const bodies::Body* g_self_body = nullptr;  // ConfigureSelf / self_view: the character shown in Sam's place
bool g_self_on = false;
std::uintptr_t g_self_sam = 0;  // the Sam entity whose mask carries our reason (0 = none)
std::uintptr_t g_self_spawn_sam = 0;  // the Sam the self body was spawned for (a new player: a new body)
bool g_self_shown = false;
double g_self_retry_ms = 0;
double g_self_asleep_since = 0;
const char* g_self_not_shown = "";  // the last reason the body is not shown (logged on change)
std::uint32_t g_self_tick_moves = 0;
double g_self_dist_sum = 0, g_self_dist_max = 0;
std::uint32_t g_self_dist_n = 0;
std::uint32_t g_self_shows = 0, g_self_unshows = 0, g_self_repairs = 0;

// Our reason bit must never outlive the shown body. SelfUnshow clears it once with a locked AND,
// but the game's own SetRootVisible (0x1426B4E40) is a plain read-modify-write of the same mask:
// one that overlaps our clear writes the old value back with bit 63 still set, and Sam would stay
// invisible with no body. So every tick in which the body is not shown, the current player's mask
// is checked and a stray bit 63 cleared (fault-free reads and the vtable / back-pointer check
// first, SamMaskAddr).
void SelfRepair(std::uintptr_t sam) {
    if (g_self_shown || !sam || !g_mesh.ok) return;
    const std::uintptr_t a = SamMaskAddr(sam);
    std::uint64_t m = 0;
    if (!a || !Rd(a, m) || !(m & kOurReason)) return;
    InterlockedAnd64(reinterpret_cast<volatile LONG64*>(a), static_cast<LONG64>(~kOurReason));
    std::uint64_t m2 = 0;
    Rd(a, m2);
    if (++g_self_repairs <= 20)
        Log("[self] reason 63 was still set on Sam while the body is not shown (a lost clear): cleared, mask 0x%llX -> 0x%llX "
            "(repair %u)",
            static_cast<unsigned long long>(m), static_cast<unsigned long long>(m2), g_self_repairs);
}

// Sam's entity flags and the scripted-scene hide counter (the other two ways the game hides him).
bool SamHiddenOtherwise(std::uintptr_t sam, const char** why) {
    std::uint64_t f = 0;
    std::int32_t scene = 0;
    if (!partner::ReadFlags(reinterpret_cast<game::Entity*>(sam), &f)) *why = "Sam's entity flags are unreadable";
    else if (!(f & 0x2)) *why = "the game hides Sam's entity (visible flag clear)";
    else if (!g_mesh.scene_hide || !Rd(g_mesh.scene_hide, scene)) *why = "the scene-hide counter is unreadable";
    else if (scene != 0) *why = "a scripted scene hides Sam (IsRootVisible's counter)";
    else return false;
    return true;
}

void SelfUnshow(const char* why) {
    if (!g_self_shown) return;
    // Sam first, then the body: Sam is never hidden by us while our body is not shown.
    std::uint64_t m = 0;
    const bool same = g_self_sam != 0;
    const bool cleared = same && SetOurReason(g_self_sam, false, &m);
    game::Entity* e = g_me.phase == Phase::live && holo::SpawnAlive(g_me.spawn) ? g_me.spawn.e : nullptr;
    const bool hid = e && partner::SetVisible(e, false);
    g_self_shown = false;
    ++g_self_unshows;
    Log("[self] Sam shown again (%s): reason 63 %s, mask now 0x%llX; body %s", why, cleared ? "cleared" : "not cleared (Sam gone)",
        static_cast<unsigned long long>(m), hid ? "hidden" : e ? "could not be hidden" : "gone");
    g_self_sam = 0;
}

bool SelfShow(std::uintptr_t sam) {
    game::Entity* e = g_me.spawn.e;
    if (!partner::SetVisible(e, true)) {
        Log("[self] the body could not be made visible: Sam stays");
        return false;
    }
    std::uint64_t m = 0;
    if (!SetOurReason(sam, true, &m)) {
        partner::SetVisible(e, false);
        Log("[self] Sam's mesh mask is not writable here: Sam stays");
        return false;
    }
    g_self_sam = sam;
    g_self_shown = true;
    ++g_self_shows;
    g_self_not_shown = "";
    Log("[self] showing '%s' in place of Sam (Sam's Root mask 0x%llX, reason 63 ours; follow mode %d; show %u)", g_me.body ? g_me.body->key : "?",
        static_cast<unsigned long long>(m), g_self_follow.load(), g_self_shows);
    return true;
}

void SelfNotShown(const char* why) {
    if (g_self_shown) SelfUnshow(why);
    if (std::strcmp(g_self_not_shown, why) != 0) {
        g_self_not_shown = why;
        Log("[self] body not shown: %s", why);
    }
}

void SelfTick(game::Entity* local, bool local_active, double now_ms) {
    const auto sam = reinterpret_cast<std::uintptr_t>(local);
    // Another player entity (load, return to title) while our reason was set on the old one: it is
    // cleared there only if the old entity still reads as a live DSPlayerEntity whose controller
    // points back at it (SamMaskAddr; fault-free reads first), else nothing is written. (No player
    // at all is the asleep path below, which shows Sam again the same way.)
    if (sam && g_self_sam && g_self_sam != sam) {
        const bool cleared = SetOurReason(g_self_sam, false);
        Log("[self] the local player changed while self view was shown: %s", cleared ? "reason 63 cleared on the old entity (still a player)"
                                                                                       : "the old entity is gone (not written)");
        g_self_sam = 0;
        g_self_shown = false;
    }
    SelfRepair(sam);
    if (sam && g_self_spawn_sam && sam != g_self_spawn_sam && (g_me.phase != Phase::idle || g_me.spawn.sp)) Teardown(g_me, "local player changed");
    if (!g_self_on || !g_self_body || !g_hook) {
        if (g_me.phase != Phase::idle || g_me.spawn.sp) Teardown(g_me, "self view off");
        return;
    }
    if (!sam || !local_active) {
        if (g_self_shown) SelfUnshow("Sam asleep (load, cutscene, knot space)");
        if (g_self_asleep_since == 0) g_self_asleep_since = now_ms > 0 ? now_ms : 1;
        if ((g_me.phase != Phase::idle || g_me.spawn.sp) && now_ms - g_self_asleep_since >= 1000) Teardown(g_me, "Sam asleep (self view)");
        return;
    }
    g_self_asleep_since = 0;
    if (!CheckMeshSites()) {
        Log("[self] self view unavailable: %s", g_mesh.why);
        g_self_on = false;
        return;
    }
    std::uint64_t mask = 0;
    const std::uintptr_t ma = SamMaskAddr(sam);
    if (!ma || !Rd(ma, mask)) {
        SelfNotShown("the local player's mesh controller does not check out (vtable / back pointer)");
        return;
    }
    if (g_me.phase == Phase::idle) {
        if (now_ms < g_self_retry_ms) return;
        if (g_self_body->id < 32 && (g_me.refused & (1u << g_self_body->id))) {
            Log("[self] self view off: body '%s' was refused", g_self_body->key);
            g_self_on = false;
            return;
        }
        game::WorldTransform at{};
        if (!game::TryGetTransform(local, &at)) return;
        char why[200] = {};
        g_me.start_hidden = true;
        const BeginResult r = BeginSlot(g_me, *g_self_body, at, Owner::self, now_ms, why, sizeof(why));
        if (r == BeginResult::started) g_self_spawn_sam = sam;
        else {
            g_me.start_hidden = false;
            Log("[self] could not start '%s': %s", g_self_body->key, why);
            g_self_retry_ms = now_ms + 10000;
            if (r == BeginResult::unavailable) g_self_on = false;
        }
        return;
    }
    char why[200] = {};
    const PollResult pr = PollSlot(g_me, now_ms, why, sizeof(why));
    if (pr == PollResult::failed) {
        g_self_retry_ms = now_ms + 10000;
        return;
    }
    if (g_me.phase != Phase::live) return;
    // Fallback: the transform copy in the detour did not run (follow 3, or Sam's model was not
    // updated): move the body from the tick.
    const ULONGLONG t = GetTickCount64();
    const ULONGLONG last_copy = g_self_last_copy.load(std::memory_order_relaxed);
    if (g_self_follow.load() == 3 || !last_copy || t - last_copy > 100) {
        game::WorldTransform wt{};
        if (game::TryGetTransform(local, &wt) && game::TrySetTransform(g_me.spawn.e, wt)) ++g_self_tick_moves;
    }
    // Where the body is against Sam right now (0 when the copy is from this frame).
    {
        double a[3] = {}, b[3] = {};
        if (mem::Read(sam + kEntWorld, a, sizeof(a)) && mem::Read(reinterpret_cast<std::uintptr_t>(g_me.spawn.e) + kEntWorld, b, sizeof(b))) {
            const double d = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
            g_self_dist_sum += d;
            ++g_self_dist_n;
            if (d > g_self_dist_max) g_self_dist_max = d;
        }
    }
    const ULONGLONG last_apply = g_self_last_apply.load(std::memory_order_relaxed);
    const char* hidden_why = nullptr;
    const char* not_ready = g_me.latch_state.load(std::memory_order_acquire) != 1 ? "latching the body's own pose"
                            : !g_me.apply_on.load()                                ? "the pose write was switched off (a fault)"
                            : !last_apply || t - last_apply > 250                  ? "no fresh pose written onto the body"
                            : (mask & ~kOurReason) != 0                            ? "the game hides Sam itself (photo mode, a scripted scene)"
                            : SamHiddenOtherwise(sam, &hidden_why)                 ? hidden_why
                                                                                   : nullptr;
    if (not_ready) {
        SelfNotShown(not_ready);
        return;
    }
    if (!g_self_shown) {
        if (!SelfShow(sam)) SelfNotShown("show failed");
        return;
    }
    SetOurReason(sam, true);  // re-assert (the game's own writes are not atomic)
}

// ---------------------------------------------------------------- stats
double g_next_self_stats_ms = 0;
std::uint32_t g_prev[8] = {};

void PartnerCheck(char* out, std::size_t cap) {
    // (a): the partner's committed pose vs what we published last (max / mean angle over 77 joints).
    out[0] = 0;
    const std::uintptr_t sm = g_p.sm.load();
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
            const float a = wire::AngleBetween(q, g_last_published.q[g_p.map[j]]);
            mx = a > mx ? a : mx;
            sum += a;
        }
        if (mx < best_max) best_max = mx, best_mean = sum / kJ;
    }
    if (best_max < 1e8f) std::snprintf(out, cap, " check: partner vs published max %.2f mean %.2f deg", best_max, best_mean);
}

void LogLatch(BodySlot& b) {
    if (b.latch_state.load(std::memory_order_acquire) != 1 || b.logged_latch || !b.sm.load()) return;
    b.logged_latch = true;
    Log("[%s] latched the body's own pose: %u plausible of %u frames -> %s; pelvis %.3f m vs Sam's rest %.3f m, ratio %.3f%s", b.tag,
        b.latch_good, b.latch_seen, b.latch_ok ? "using its own bone translations" : "using Sam's bone translations", b.latch_pelvis_len,
        b.latch_sam_pelvis_len, b.pelvis_ratio, b.same_skel ? " (Sam's own skeleton: not scaled)" : "");
    if (b.leg_ratio > 0)
        Log("[%s]   legs (thigh + shin) %.3f m vs Sam %.3f m: ratio %.3f%s", b.tag, b.leg_body, b.leg_sam, b.leg_ratio,
            b.leg_used ? "; the latched pelvis disagrees by more than 8 % (not a standing pose): the pelvis ratio uses the legs"
                       : "; agrees with the pelvis");
}

void SelfStats(double now) {
    LogLatch(g_me);
    if (g_me.phase == Phase::idle || now < g_next_self_stats_ms) return;
    g_next_self_stats_ms = now + 1000;
    static std::uint32_t prev_app = 0, prev_copies = 0, prev_fail = 0, prev_stale = 0, prev_rej = 0, prev_obs = 0, prev_moves = 0;
    static std::uint32_t prev_aba = 0, prev_nocap = 0, prev_wpm = 0, prev_caba = 0;
    const std::uint32_t app = g_me.ct.applies.load(), copies = g_self_copies.load(), fail = g_self_copy_fail.load(),
                        stale = g_self_stale.load(), rej = g_me.ct.rejects.load(), obs = g_me.ct.observed.load(), aba = g_me.ct.aba.load(),
                        nocap = g_me.ct.nocap.load(), wpm = g_me.ct.wpm_fail.load(), caba = g_self_copy_aba.load();
    const std::uint64_t n = g_self_age_n.exchange(0), us = g_self_age_us.exchange(0), mx = g_self_age_max_us.exchange(0);
    std::uint64_t mask = 0, sam_flags = 0;
    const std::uintptr_t sam_e = g_self_sam ? g_self_sam : g_sam_e.load();
    if (const std::uintptr_t a = SamMaskAddr(sam_e)) Rd(a, mask);
    if (sam_e) partner::ReadFlags(reinterpret_cast<game::Entity*>(sam_e), &sam_flags);
    std::int32_t scene = -1;
    if (g_mesh.scene_hide) Rd(g_mesh.scene_hide, scene);
    const std::uint32_t fz = g_me.forced - g_me.prev_forced, sk = g_me.forced_skipped - g_me.prev_skipped;
    g_me.prev_forced = g_me.forced, g_me.prev_skipped = g_me.forced_skipped;
    Log("[self] shown=%d '%s' app=%u obs=%u copies=%u (failed %u, aba %u) tick-moves=%u pose age avg %.2f max %.2f ms stale=%u "
        "body-Sam distance avg %.1f max %.1f cm; Sam's Root mask 0x%llX flags 0x%llX scene-hide %d repairs %u; rej=%u(last %d) aba=%u "
        "nocap=%u wpm=%u faults=%u forced=%u game-drove=%u",
        g_self_shown ? 1 : 0, g_me.body ? g_me.body->key : "?", app - prev_app, obs - prev_obs, copies - prev_copies, fail - prev_fail,
        caba - prev_caba, g_self_tick_moves - prev_moves, n ? static_cast<double>(us) / static_cast<double>(n) / 1000.0 : 0.0,
        static_cast<double>(mx) / 1000.0, stale - prev_stale, g_self_dist_n ? g_self_dist_sum / g_self_dist_n * 100.0 : 0.0,
        g_self_dist_max * 100.0, static_cast<unsigned long long>(mask), static_cast<unsigned long long>(sam_flags), scene, g_self_repairs,
        rej - prev_rej, g_me.ct.last_reject.load(), aba - prev_aba, nocap - prev_nocap, wpm - prev_wpm, g_me.ct.faults.load(), fz, sk);
    prev_app = app, prev_copies = copies, prev_fail = fail, prev_stale = stale, prev_rej = rej, prev_obs = obs, prev_moves = g_self_tick_moves;
    prev_aba = aba, prev_nocap = nocap, prev_wpm = wpm, prev_caba = caba;
    g_self_dist_sum = 0, g_self_dist_max = 0, g_self_dist_n = 0;
}

void Stats(double now) {
    SelfStats(now);
    if (now < g_next_stats_ms) return;
    const bool first = g_next_stats_ms == 0;
    g_next_stats_ms = now + (g_p.sm.load() || g_p.phase != Phase::idle ? 1000 : 10000);
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
    LogLatch(g_p);
    if (!any) return;
    char check[96] = {};
    if (d[2]) PartnerCheck(check, sizeof(check));
    const std::uint32_t fz = g_p.forced - g_p.prev_forced, sk = g_p.forced_skipped - g_p.prev_skipped;
    g_p.prev_forced = g_p.forced, g_p.prev_skipped = g_p.forced_skipped;
    Log("[pose] cap=%u app=%u obs=%u rej=%u(last %d) torn=%u aba=%u wpm=%u faults=%u calls=%u forced=%u game-drove=%u%s", d[1], d[2],
        d[6], d[3], g_ct.last_reject.load(), d[4], d[5], d[7], g_ct.faults.load(), d[0], fz, sk, check);
}

// Game thread: updates a live body's model when the game did not since the last tick (the
// same call the game makes for photo mode / teleports, 0x14239D890 callers 0x142495833,
// 0x142F115FC, 0x142F13938, 0x1436334D2: entity +0xB8, IsKindOf SkinnedModel, call).
// Never while Sam is not up (loads, cutscenes, knot space): the world may be changing and the
// body is about to be removed (player_sync after 1 s, the debug body in Tick).
void DriveModel(BodySlot& b, bool local_active) {
    const std::uintptr_t sm = b.sm.load(std::memory_order_acquire);
    if (!sm || b.phase != Phase::live || b.forced_off || !g_fn_forced || !local_active) return;
    // Never race the game: while it updates the model itself (seen within the last 100 ms), it
    // is left alone; only a model the game stopped updating is driven from here.
    if (b.game_updates.exchange(0, std::memory_order_relaxed) != 0) b.last_game_ms = GetTickCount64();
    if (b.last_game_ms && GetTickCount64() - b.last_game_ms < 100) {
        ++b.forced_skipped;
        return;
    }
    std::uintptr_t id[2] = {};
    if (!mem::Read(sm, id, sizeof(id)) || id[0] != g_sm_vt || Q(sm + kSmEntity) != b.e.load()) return;
    if (!holo::SpawnAlive(b.spawn)) return;
    t_forcing = true;
    const bool ok = CallForced(sm);
    t_forcing = false;
    if (!ok) {
        ++b.forced_failed;
        b.forced_off = true;
        Log("[%s] forced model update FAULTED; not called again this run", b.tag);
        return;
    }
    ++b.forced;
}

// Game thread: the partner slot's Sam body (demo_sam_clean, id 1) wears the outfit the other player
// sent (or, debug, our own / an explicit one). Only while our player is up, 0.5 s after the body
// became live; partner_outfit rate-limits and logs every step.
void DressPartner(game::Entity* local, bool local_active, double now_ms) {
    if (g_p.phase != Phase::live || !g_p.body || g_p.body->id != bodies::Default().id || !local || !local_active) return;
    if (GetTickCount64() - g_p.live_tick < 500) return;
    wire::Outfit want;
    if (g_outfit_override == 1) {
        if (!outfit::ReadLocal(reinterpret_cast<std::uintptr_t>(local), &want)) return;
    } else if (g_outfit_override == 2) {
        want = g_override_outfit;
    } else if (g_p.owner == Owner::partner && g_have_partner_outfit) {
        want = g_partner_outfit;
    } else {
        return;
    }
    if (!holo::SpawnAlive(g_p.spawn)) return;
    outfit::Apply(g_dress, reinterpret_cast<std::uintptr_t>(g_p.spawn.e), want, reinterpret_cast<std::uintptr_t>(local), now_ms);
}

}  // namespace

// ================================================================ public
bool InstallHook() noexcept {
    if (g_hook) return true;
    g_me.tag = "self";
    LARGE_INTEGER f;
    if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) g_qpc_per_ms = static_cast<double>(f.QuadPart) / 1000.0;
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

Phase GetPhase() noexcept { return g_p.phase; }
Owner GetOwner() noexcept { return g_p.owner; }
const char* PhaseName() noexcept { return g_p.phase == Phase::live ? "live" : g_p.phase == Phase::loading ? "loading" : "idle"; }
const bodies::Body* CurrentBody() noexcept { return g_p.body; }
bool Refused(std::uint8_t id) noexcept { return id < 32 && (g_p.refused & (1u << id)); }
void ResetRefused() noexcept { g_p.refused = 0; }

game::Entity* LiveEntity() noexcept {
    if (g_p.phase != Phase::live || !holo::SpawnAlive(g_p.spawn)) return nullptr;
    return g_p.spawn.e;
}

BeginResult Begin(const bodies::Body& body, const game::WorldTransform& at, Owner owner, double now_ms, char* why,
                  std::size_t cap) noexcept {
    return BeginSlot(g_p, body, at, owner, now_ms, why, cap);
}

PollResult Poll(double now_ms, char* why, std::size_t cap) noexcept { return PollSlot(g_p, now_ms, why, cap); }

bool Move(const game::WorldTransform& at) noexcept {
    game::Entity* e = LiveEntity();
    return e && game::TrySetTransform(e, at);
}

bool Remove(const char* why) noexcept { return Teardown(g_p, why); }

void ConfigureSelf(const bodies::Body* body) noexcept {
    g_self_body = body;
    g_self_on = body != nullptr;
    g_me.tag = "self";
    if (body) Log("[self] self view on: '%s' (%s) is shown in place of the local Sam on this screen", body->key, body->label);
}
bool SelfConfigured() noexcept { return g_self_on && g_self_body; }

void EmergencyStop() noexcept {
    // The detour stops touching anything (no capture, no transform copy, no pose write).
    g_sam_sm.store(0, std::memory_order_seq_cst);
    g_sam_e.store(0, std::memory_order_seq_cst);
    g_self_follow.store(3, std::memory_order_seq_cst);
    Unpublish(g_p);
    Unpublish(g_me);
    // Sam first (fault-free checked), then the body.
    std::uint64_t m = 0;
    const bool cleared = g_self_sam && SetOurReason(g_self_sam, false, &m);
    bool hid = false;
    if (g_self_shown && g_me.phase == Phase::live && holo::SpawnAlive(g_me.spawn)) hid = partner::SetVisible(g_me.spawn.e, false);
    const bool was_shown = g_self_shown;
    g_self_shown = false;
    g_self_on = false;
    g_self_sam = 0;
    Log("[self] emergency stop (the per-frame sync faulted): detour work off%s%s", was_shown ? "; Sam shown again: reason 63 " : "",
        was_shown ? (cleared ? (hid ? "cleared, body hidden" : "cleared, body NOT hidden") : "NOT cleared") : "");
}
bool SelfShown() noexcept { return g_self_shown; }
game::Entity* SelfEntity() noexcept {
    if (g_me.phase == Phase::idle) return nullptr;
    return g_me.spawn.e;
}

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
        g_sam_e.store(sm ? reinterpret_cast<std::uintptr_t>(local) : 0, std::memory_order_release);
        g_sam_sm.store(sm, std::memory_order_seq_cst);
        static std::uintptr_t logged = 1;
        if (sm != logged) {
            logged = sm;
            Log("[real] Sam's SkinnedModel for capture: %p (skeleton %p)", reinterpret_cast<void*>(sm), reinterpret_cast<void*>(skel));
        }
    }
    // A debug-owned body drives itself.
    if (g_p.owner == Owner::debug) {
        char why[200] = {};
        const PollResult r = PollSlot(g_p, now_ms, why, sizeof(why));
        if (r == PollResult::live && g_p.mirror) {
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
    if (g_p.owner == Owner::debug && asleep_since > 0 && now_ms - asleep_since >= 1000) Teardown(g_p, "Sam asleep (debug body)");
    if (g_hook) SelfTick(local, local_active, now_ms);
    DriveModel(g_p, local_active);
    DriveModel(g_me, local_active);
    DressPartner(local, local_active, now_ms);
    if (g_hook) Stats(now_ms);
}

bool CaptureFrame(wire::JointFrame* out) noexcept {
    Capture c;
    if (!g_capture.Read(c) || GetTickCount64() - c.tick_ms > 250) return false;
    return FrameFromCapture(c, out);
}

bool PushFrame(const wire::JointFrame& f, double key_ms) noexcept {
    if (f.joint_count != kJ || f.layout_hash != wire::kSamLayoutHash) return false;
    if (g_ring_count && key_ms <= RingAt(g_ring_count - 1).key_ms) return false;
    RingSlot& s = g_ring[(g_ring_head + g_ring_count) % kRing];
    g_last_push_tick = GetTickCount64();
    s.key_ms = key_ms;
    s.f = f;
    if (g_ring_count < kRing) ++g_ring_count;
    else g_ring_head = (g_ring_head + 1) % kRing;
    return true;
}

void ResetFrames() noexcept { g_ring_head = g_ring_count = 0; }

bool ReadOutfit(game::Entity* local, wire::Outfit* out) noexcept {
    return local && outfit::ReadLocal(reinterpret_cast<std::uintptr_t>(local), out);
}

void SetPartnerOutfit(const wire::Outfit* o) noexcept {
    if (!o) {
        g_have_partner_outfit = false;
        return;
    }
    if (!g_have_partner_outfit || *o != g_partner_outfit) {
        static unsigned logs = 0;
        if (logs < 20) {
            ++logs;
            char d[160];
            Log("[outfit] the partner's outfit: %s", outfit::Describe(*o, d, sizeof(d)));
        }
    }
    g_partner_outfit = *o;
    g_have_partner_outfit = true;
}

bool StarvedOfFrames(double limit_ms) noexcept {
    if (g_p.phase != Phase::live || !g_p.live_tick) return false;
    const ULONGLONG since = g_last_push_tick > g_p.live_tick ? g_last_push_tick : g_p.live_tick;
    return static_cast<double>(GetTickCount64() - since) > limit_ms;
}

bool SamplePublish(double play_ms) noexcept {
    if (!g_ring_count) return false;
    static wire::JointFrame out;
    const RingSlot& newest = RingAt(g_ring_count - 1);
    const RingSlot& oldest = RingAt(0);
    if (play_ms >= newest.key_ms || g_ring_count == 1) {
        out = newest.f;
    } else if (play_ms <= oldest.key_ms) {
        out = oldest.f;
    } else {
        unsigned i = g_ring_count - 1;
        while (i > 0 && RingAt(i - 1).key_ms > play_ms) --i;
        const RingSlot& a = RingAt(i - 1);
        const RingSlot& b = RingAt(i);
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
            if (g_p.owner == Owner::partner) {
                Set(msg, cap, "the partner owns the body slot");
                return false;
            }
            if (g_p.phase != Phase::idle) Teardown(g_p, "debug respawn");
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
            const BeginResult br = BeginSlot(g_p, *b, at, Owner::debug, g_now, why, sizeof(why));
            g_p.mirror = v[2] != 0;
            Set(msg, cap, "%s '%s' %.1f m ahead (mirror %s): %s", br == BeginResult::started ? "spawning" : "NOT spawning", b->key, dist,
                g_p.mirror ? "on" : "off", why);
            return br == BeginResult::started;
        }
        case 3: {
            const bool had = g_p.owner == Owner::debug && Teardown(g_p, "debug remove");
            Set(msg, cap, had ? "removed" : "no debug body");
            return true;
        }
        case 4: {  // joint names of Sam and the live body
            const SkelInfo ss = PoseSkeleton(have_sam ? ModelOf(reinterpret_cast<std::uintptr_t>(sam)) : 0);
            const SkelInfo ps = PoseSkeleton(g_p.live_sm);
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
            Set(msg, cap,
                "hook %d sam_sm %p partner_sm %p phase %s owner %d latch %d(%s) cap=%u app=%u rej=%u torn=%u aba=%u%s; self %s phase %s "
                "shown %d app=%u copies=%u",
                g_hook ? 1 : 0, reinterpret_cast<void*>(g_sam_sm.load()), reinterpret_cast<void*>(g_p.sm.load()), PhaseName(),
                static_cast<int>(g_p.owner), g_p.latch_state.load(), g_p.latch_ok ? "own t" : "Sam t", g_ct.captures.load(), g_ct.applies.load(),
                g_ct.rejects.load(), g_ct.torn.load(), g_ct.aba.load(), check, g_self_on && g_self_body ? g_self_body->key : "off",
                g_me.phase == Phase::live ? "live" : g_me.phase == Phase::loading ? "loading" : "idle", g_self_shown ? 1 : 0,
                g_me.ct.applies.load(), g_self_copies.load());
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
            g_p.refused &= ~(1u << 31);
            if (g_p.owner == Owner::partner) {
                Set(msg, cap, "the partner owns the body slot");
                return false;
            }
            if (g_p.phase != Phase::idle) Teardown(g_p, "debug respawn");
            game::WorldTransform at = swt;
            at.pos.x += swt.rot.m[1][0] * 4.0;
            at.pos.y += swt.rot.m[1][1] * 4.0;
            char why[200] = {};
            const BeginResult br = BeginSlot(g_p, control, at, Owner::debug, g_now, why, sizeof(why));
            Set(msg, cap, "control item %08X '%s' %s: %s", pick->hash, path, br == BeginResult::started ? "spawning" : "NOT spawning", why);
            return br == BeginResult::started;
        }
        case 6: {  // mirror on/off for the debug body
            g_p.mirror = v[0] != 0;
            g_p.apply_on.store(g_p.phase == Phase::live && (g_p.owner == Owner::partner || g_p.mirror), std::memory_order_release);
            Set(msg, cap, "mirror %s", g_p.mirror ? "on" : "off");
            return true;
        }
        case 8: {  // self view <0/1> [body id, 0 = the configured one] [follow 1 Sam's update / 2 the body's / 3 tick, 0 = unchanged]
            if (v[0] == 2) {  // hidden tests only: self_view 2 <reason 0..63> <0/1> sets / clears a hide reason on Sam's mask
                if (!automation::Enabled() || !automation::SavesRedirected()) {
                    Set(msg, cap, "refused: only in a hidden test run on scratch saves");
                    return false;
                }
                const int reason = static_cast<int>(v[1]);
                if (reason < 0 || reason > 63 || !have_sam || !CheckMeshSites()) {
                    Set(msg, cap, "no: reason 0..63, a local player and verified mesh sites needed");
                    return false;
                }
                const std::uintptr_t a = SamMaskAddr(reinterpret_cast<std::uintptr_t>(sam));
                std::uint64_t before = 0, after = 0;
                if (!a || !Rd(a, before)) {
                    Set(msg, cap, "Sam's mesh mask does not check out");
                    return false;
                }
                const std::uint64_t bit = 1ull << reason;
                if (v[2] != 0) InterlockedOr64(reinterpret_cast<volatile LONG64*>(a), static_cast<LONG64>(bit));
                else InterlockedAnd64(reinterpret_cast<volatile LONG64*>(a), static_cast<LONG64>(~bit));
                Rd(a, after);
                Set(msg, cap, "TEST: Sam's hide reason %d %s: mask 0x%llX -> 0x%llX", reason, v[2] != 0 ? "set" : "cleared",
                    static_cast<unsigned long long>(before), static_cast<unsigned long long>(after));
                Log("[self] %s", msg);
                return true;
            }
            const int follow = static_cast<int>(v[2]);
            if (follow >= 1 && follow <= 3) g_self_follow.store(follow);
            if (v[0] == 0) {
                const bool had = g_me.phase != Phase::idle || g_me.spawn.sp;
                g_self_on = false;
                Teardown(g_me, "self view off (debug)");
                Set(msg, cap, "self view off%s", had ? " (body removed, Sam shown)" : "");
                return true;
            }
            if (!g_hook) {
                Set(msg, cap, "the pose hook is not installed (partner_body=real or pose_hook=1 at start)");
                return false;
            }
            const bodies::Body* b = v[1] != 0 ? bodies::FindById(static_cast<std::uint8_t>(v[1])) : g_self_body;
            if (!b) {
                Set(msg, cap, v[1] != 0 ? "unknown body id %.0f" : "no character configured: give a body id", v[1]);
                return false;
            }
            if (b != g_self_body && (g_me.phase != Phase::idle || g_me.spawn.sp)) Teardown(g_me, "self view: another character");
            g_self_body = b;
            g_self_on = true;
            g_self_retry_ms = 0;
            g_me.tag = "self";
            Set(msg, cap, "self view on: '%s' in place of Sam (follow %d)", b->key, g_self_follow.load());
            return true;
        }
        case 9: {  // outfit <mode> <a> <b>: 0 log, 1 dress the slot body like our Sam, 2 exos a colour b, 3 override off
            const int mode = static_cast<int>(v[0]);
            char d[160];
            if (mode == 0) {
                outfit::LogLocal(have_sam ? reinterpret_cast<std::uintptr_t>(sam) : 0);
                if (game::Entity* e = g_p.phase == Phase::live ? g_p.spawn.e : nullptr) outfit::LogBody(reinterpret_cast<std::uintptr_t>(e), "slot body");
                if (game::Entity* e = g_me.phase == Phase::live ? g_me.spawn.e : nullptr) outfit::LogBody(reinterpret_cast<std::uintptr_t>(e), "self body");
                wire::Outfit o;
                const bool ok = have_sam && outfit::ReadLocal(reinterpret_cast<std::uintptr_t>(sam), &o);
                Set(msg, cap, "our outfit: %s; partner's: %s; dressed: v01 %s exos %u colour %u (%u calls)", ok ? outfit::Describe(o, d, sizeof(d)) : "unreadable",
                    g_have_partner_outfit ? "received" : "none", g_dress.v01_done ? "removed" : "kept", g_dress.exos, g_dress.colour, g_dress.applies);
                return ok;
            }
            if (mode == 1) {
                g_outfit_override = 1;
                Set(msg, cap, "the slot's Sam body is dressed like our own Sam");
                return outfit::Available();
            }
            if (mode == 2) {
                g_outfit_override = 2;
                g_override_outfit = wire::Outfit{};
                g_override_outfit.variant = wire::kPorterVariant;
                g_override_outfit.exos = v[1] >= 0 && v[1] < wire::kExosCount ? static_cast<std::uint8_t>(v[1]) : std::uint8_t{0xFF};
                g_override_outfit.suit_colour = v[2] >= 0 && v[2] <= 16 ? static_cast<std::uint8_t>(v[2]) : std::uint8_t{0xFF};
                Set(msg, cap, "the slot's Sam body: exos %u suit colour %u", g_override_outfit.exos, g_override_outfit.suit_colour);
                return outfit::Available();
            }
            if (mode == 4) {  // hidden tests only: change our own Sam's suit colour (scratch saves)
                if (!automation::Enabled() || !automation::SavesRedirected()) {
                    Set(msg, cap, "refused: only in a hidden test run on scratch saves");
                    return false;
                }
                const bool ok = have_sam && outfit::TestSetOwnSuitColour(reinterpret_cast<std::uintptr_t>(sam), static_cast<std::uint8_t>(v[1]));
                Set(msg, cap, "our own suit colour -> %.0f: %s", v[1], ok ? "written" : "not written");
                return ok;
            }
            g_outfit_override = 0;
            Set(msg, cap, "outfit override off (the partner's own outfit applies)");
            return true;
        }
        default:
            Set(msg, cap, "unknown op %d", op);
            return false;
    }
}

}  // namespace st::realbody
