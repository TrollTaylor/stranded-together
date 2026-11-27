#include "real_body.hpp"

#include <windows.h>
#include <intrin.h>

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
#include "config.hpp"
#include "menu_figure.hpp"
#include "overlay/overlay.hpp"
#include "partner_collision.hpp"
#include "partner_outfit.hpp"
#include "partner_source.hpp"
#include "ride_pose_math.hpp"
#include "gear_extrap.hpp"
#include "ride_together.hpp"
#include "self_predict.hpp"
#include "pelvis_scale.hpp"
#include "suit_colour.hpp"
#include "coop_session.hpp"
#include "fragile_textures.hpp"
#include "texture_looks.hpp"
#include "texture_share.hpp"
#include "boots_look.hpp"
#include "partner_gear.hpp"
#include "suit_colour_math.hpp"
#include "game_actions.hpp"
#include "room_outfit_rules.hpp"
#include "face_sync.hpp"
#include "hood_fold.hpp"
#include "enemy_l1_probe.hpp"
#include "enemy_target.hpp"
#include "entity_remove_hook.hpp"
#include "wardrobe.hpp"

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
// The controller's part masks start at +0x348 (part k at +0x348 + 8k, k < 89); mask 0 is never written
// by SetPartVisible (part 0 goes to the Root mask) and ApplyMeshControl reads it as a second global
// condition (0x1426D91EF: cmp qword [rbx+0x348],0): any bit hides every ArtPart of Sam (FUN_1422B25F0
// on the top-level parts, then each mapped part), but IsRootVisible looks only at +0x338, so the
// inventory keeps every gear entity (backpack and what hangs on it, weapons, BB pod, Odradek,
// accessories) shown and attached to Sam's animated, invisible skeleton (docs/real-partner-body.md
// section 8.1). Our bit 63 lives there.
constexpr std::uintptr_t kPlayerGlobalMask = 0xA60 + 0x348;  // 0xDA8
constexpr std::uintptr_t kApplyGlobalCheck = 0x26D91E2;      // cmp [rbx+0x338],0; mov sil,1; jnz; cmp [rbx+0x348],0; jnz
constexpr std::uintptr_t kApplyMaskLoop = 0x26D915A;         // add rcx,0x348; mov rax,[rcx+0x2C8]; cmp [rcx],rax (the diff check)
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
    double play = 0;    // the play time (sender timeline) it was sampled at (0: a mirror frame)
};

// Smooth animation above 60 fps (docs/real-partner-body.md section 3.5). The tick publishes the
// bracketing ring frames (up to 3, keyed on the sender timeline) and the play time at a QPC
// reference; the partner body's own model update interpolates them at the moment it runs.
struct InterpBuf {
    std::uint32_t n = 0;  // frames 1..3
    double ref_ms = 0;    // QPC milliseconds (link::NowMsPrecise's clock) when published
    double play_ref = 0;  // the play time at ref_ms (sender timeline)
    double key[3] = {};
    wire::Quat q[3][kJ];  // Sam-skeleton order
    float troot[3][3] = {};
    float tpelvis[3][3] = {};
    Joint fb[kJ];  // the local Sam's own full joints (translation/scale fallback)
};

SeqLock<Capture> g_capture;
SeqLock<ApplyBuf> g_apply;  // the partner slot's published frame
SeqLock<InterpBuf> g_interp;  // the partner slot's bracketing frames (smooth bit kSmoothDetourInterp)
std::atomic<bool> g_interp_on{false};  // the newest publish was an InterpBuf (a mirror frame clears it)

// Smooth-animation features (bits of realbody::kSmooth*), session.ini smooth_pose / the `smooth` test action.
std::atomic<unsigned> g_smooth{kSmoothAll};
// Detour-side counters of the smooth path (read and reset by the tick's meter).
std::atomic<std::uint32_t> g_interp_applies{0}, g_interp_holds{0}, g_interp_torn{0};
std::atomic<double> g_last_play{0};  // the play time of the newest pose written onto the partner body
std::atomic<float> g_frame_ms{16.7f};  // the tick's frame time (EMA), for the self body's age buckets
std::atomic<std::uint32_t> g_self_age_bucket[3] = {};  // self pose writes: < 0.5 frame old, < 1.5, older

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
    std::atomic<std::uint32_t> updates{0};  // every detour call on the body (the game's and the forced ones): the meter
    Counters ct;
    // Set by the game thread before `sm` is published (release), read by the detour.
    std::uint8_t map[kJ] = {};  // body joint -> Sam joint
    std::uint8_t inv[kJ] = {};  // Sam joint -> body joint (gear anchors)
    // Latch of the body's own pose (detour writes while latch_state == 0).
    std::atomic<int> latch_state{0};  // 0 observing, 1 done
    Joint latch[kJ] = {};
    bool latch_ok = false;
    float pelvis_ratio = 1.0f;
    unsigned latch_good = 0, latch_seen = 0;
    float latch_pelvis_len = 0, latch_sam_pelvis_len = 0;
    float pelvis_off[3] = {};  // the far hips rule's rest offset (FinishLatch; pelvis_scale.hpp)
    bool pelvis_off_ok = false;
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
    std::uint32_t lives = 0;    // bodies that became live in this slot (the face sync's epoch: a respawn gives the clip anew)
    bool logged_latch = false;
    // DriveModel
    ULONGLONG last_game_ms = 0;
    std::uint32_t forced = 0, forced_skipped = 0, forced_failed = 0;
    std::uint32_t prev_forced = 0, prev_skipped = 0;
    bool forced_off = false;
    std::uint32_t refused = 0;  // bit per body id that failed to load / validate in THIS slot (this session)
    // Update frequency (DriveFreq): the entity it was read on, its own value then, attempts.
    std::uintptr_t freq_e = 0;
    std::uint8_t freq_orig = 0xFF;
    unsigned freq_calls = 0;
};
BodySlot g_slots[2];
BodySlot& g_p = g_slots[0];   // partner / debug
BodySlot& g_me = g_slots[1];  // self view
Counters& g_ct = g_slots[0].ct;  // also counts Sam's captures and every detour call

// Self view, detour side.
// How the self body follows Sam: 1 = Sam's own model update copies his world transform onto
// the body (the frame's transform, no tick jitter), 2 = the body's own model update copies it,
// 3 = the tick only (like the partner), 4 = mode 1 plus, in the body's own update, Sam's world
// predicted for this frame (PredictSelfWorld, section 26). Default 4.
std::atomic<int> g_self_follow{4};
std::uintptr_t g_fn_setwt = 0;  // Entity_ExportedSetWorldTransform, byte-checked (CheckMeshSites)
std::atomic<std::uint32_t> g_self_copies{0}, g_self_copy_fail{0}, g_self_copy_aba{0}, g_self_stale{0};
std::atomic<std::uint64_t> g_self_age_us{0}, g_self_age_n{0}, g_self_age_max_us{0};
std::atomic<std::uint32_t> g_self_last_frame{0};
std::atomic<ULONGLONG> g_self_last_apply{0};  // GetTickCount64 of the last pose written onto the self body
std::atomic<ULONGLONG> g_self_last_copy{0};   // ... of the last transform copied in a detour

// Self view fit (docs/real-partner-body.md section 16). While the self body is shown (Sam hidden by
// reason 63), Sam's own update ends with his skeleton in the body's proportions: every local joint
// but the root takes the body's latched translation and scale, the pelvis Sam's own translation times
// the pelvis ratio (exactly what the body's pose gets), the rotations stay Sam's; then the game's
// model-matrix and bounds steps run again, as for the bodies. Everything the game hangs on Sam's
// skeleton (helper-attached gear, the cargo slots, the boots that copy his leg matrices) then sits on
// the character. Sam's clean pose still feeds the capture (the wire, the self body, the meter): a
// translation still bit-identical to the one written last (the game did not rewrite it) is read as
// its clean value. Off: nothing is written; a translation the game left as written is put back.
// Same pose (default, `self_view 6 <0/1>`): the rotations, root and pelvis come from the capture the
// self body last applied, not Sam's newest: the body's update runs before Sam's in a frame (section
// 8.2), so it shows the previous frame's pose, and Sam's gear must hang on that same pose or it parts
// from the body while moving (18 cm at the hand running at 30 fps in run 20261117-212222).
// Head lift (`self_view 7 <percent>`): Sam's head joint (and so HLP_Head: the cap, the glasses) sits
// that share of the way from the character's head back to where Sam's own head would be, measured once
// in the head's parent frame: Sam's cap is made for his head and sank over her eyes at 0 %.
struct FitShape {
    std::uint8_t on[kJ] = {};  // Sam joint -> takes t / s below
    float t[kJ][4] = {};
    float s[kJ][4] = {};
    float pelvis_ratio = 1.0f;
    float pelvis_off[3] = {};        // the far hips rule's rest offset (pelvis_scale.hpp; section 38.3)
    std::uint8_t pelvis_off_ok = 0;
};
// Round 12 (docs/real-partner-body.md section 38.3), `self_view 22 <0/1>`: the far hips rule (pelvis_scale.hpp), 1 the
// default, 0 v0.26's plain ratio.
std::atomic<bool> g_far_hips_rule{true};
// The body's hips translation from Sam's `t` (pelvis_scale.hpp): the ratio, or with the far hips rule on (and a rest
// offset) the constant offset far from the root.
std::atomic<std::uint32_t> g_far_hips_n{0};  // self view pose writes with Sam's hips farther than pelvis::kNearM from his root
inline void HipsT(const float* t, float ratio, const float* off, bool off_ok, float* o) {
    pelvis::Scaled(t, ratio, off_ok && g_far_hips_rule.load(std::memory_order_relaxed) ? off : nullptr, o);
}
constexpr unsigned kSamNeck2 = 30, kSamHeadJ = 31;  // JNT_C_B_007_Neck, JNT_C_B_008_Head (its subtree: 31-34)
struct LiftPub {
    float local[4] = {};  // the applied lift in the neck joint's frame (Sam's skeleton), 0 when none
    std::uint32_t on = 0;
};
SeqLock<FitShape> g_fit_shape;
SeqLock<Capture> g_self_applied;       // the capture the self body's pose was last written from
SeqLock<LiftPub> g_lift_pub;           // for the gear sender (partner_gear): the head items' anchor without the lift
std::atomic<bool> g_fit_cfg{true};     // session.ini self_fit / `self_view 5 <0/1>`
std::atomic<bool> g_fit_sync{true};    // `self_view 6 <0/1>`: same pose as the body
std::atomic<int> g_lift_pct{kDefaultHeadLiftPct};  // `self_view 7 <percent>`
std::atomic<bool> g_lift_reset{false};  // FitOn: measure the lift again (another body)
std::atomic<bool> g_fit_want{false};   // game thread: the self body is shown and fitted (cleared before Sam is shown)
std::atomic<bool> g_fit_busy{false};   // one Sam update at a time inside the fit and the capture
std::atomic<bool> g_fit_faulted{false};  // a model-matrix call faulted: no fit for the session
// Sam's update only (under g_fit_busy).
bool g_fit_active = false;  // the last update wrote the shape
Joint g_fit_written[kJ];    // what it wrote
float g_fit_clean_q[kJ][4], g_fit_clean_t[kJ][4], g_fit_clean_s[kJ][4];  // Sam's own joints then
int g_lift_state = 0;           // 0 to measure, 1 measured, 2 refused (the frame convention did not check out)
float g_lift_local[3] = {};     // the full lift (100 %) in the neck joint's frame
int g_lift_pct_pub = -1;        // the percentage last published to g_lift_pub
float g_fit_ref_t[kJ][3] = {};  // Sam's clean translations just before the first write after FitOn
bool g_fit_ref_ok = false;
std::atomic<std::uint32_t> g_fit_writes{0}, g_fit_updates_held{0}, g_fit_joints_held{0}, g_fit_updates_fresh{0}, g_fit_restores{0},
    g_fit_wpm{0}, g_fit_faults{0}, g_fit_torn{0}, g_fit_skips{0}, g_fit_qheld{0}, g_fit_sheld{0}, g_fit_sync_used{0}, g_fit_sync_fallback{0},
    g_fit_toward_max{0};
// GetTickCount64 of the last capture of Sam whose clean joints (the fit's held values restored) differ from the previous
// capture's: the game moved him (section 33's stall guard; the same signal with the fit on or off)
std::atomic<ULONGLONG> g_sam_last_change{0};
// The leak check by bucket (0: the fit off, 1: on): joints nearer the shape than Sam's reference, updates
// measured, the largest change from the reference (um), and which joints (bits 0-63, 64-76).
std::atomic<std::uint32_t> g_fit_toward[2] = {}, g_fit_leak_upd[2] = {}, g_fit_dev_max_um[2] = {};
std::atomic<std::uint64_t> g_fit_toward_bits[2][2] = {};
FitShape g_fit_last_shape;   // Sam's update only (under g_fit_busy): the last shape read
bool g_fit_last_shape_ok = false;
std::atomic<bool> g_fit_shown{false};  // the self body is shown (the leak check's fit-off bucket); SelfShow / FitOffWait

// The review's leak check (Sam's update, under g_fit_busy): of the joints the shape changes (the body's
// translation more than 2 mm from Sam's reference), how many came back from the game's animate nearer the
// shape than the reference. Measured with the fit on and off: a leak shows only with it on.
void LeakCheck(const FitShape& sh, const Capture& c, int bucket) {
    unsigned toward = 0;
    double dev_max = 0;
    for (unsigned j = 2; j < kJ; ++j) {
        if (!sh.on[j]) continue;
        double ds = 0, dr = 0, dd = 0;
        for (int k = 0; k < 3; ++k) {
            ds += (c.j[j].t[k] - sh.t[j][k]) * (c.j[j].t[k] - sh.t[j][k]);
            dr += (c.j[j].t[k] - g_fit_ref_t[j][k]) * (c.j[j].t[k] - g_fit_ref_t[j][k]);
            dd += (sh.t[j][k] - g_fit_ref_t[j][k]) * (sh.t[j][k] - g_fit_ref_t[j][k]);
        }
        if (dd > 0.002 * 0.002 && ds < dr) {
            ++toward;
            g_fit_toward_bits[bucket][j / 64].fetch_or(1ull << (j % 64), std::memory_order_relaxed);
        }
        if (dr > dev_max) dev_max = dr;
    }
    g_fit_toward[bucket].fetch_add(toward, std::memory_order_relaxed);
    g_fit_leak_upd[bucket].fetch_add(1, std::memory_order_relaxed);
    if (bucket == 1 && toward > g_fit_toward_max.load(std::memory_order_relaxed)) g_fit_toward_max.store(toward, std::memory_order_relaxed);
    const auto um = static_cast<std::uint32_t>(std::sqrt(dev_max) * 1e6);
    if (um > g_fit_dev_max_um[bucket].load(std::memory_order_relaxed)) g_fit_dev_max_um[bucket].store(um, std::memory_order_relaxed);
}
// Captured pelvis length (Sam's clean pose) while the fit is on / off, in micrometres (the review's check:
// a fit leaking into the capture would shrink it by the pelvis ratio).
std::atomic<std::uint64_t> g_fit_pel_sum[2] = {}, g_fit_pel_n[2] = {};
std::atomic<float> g_lift_cm{0}, g_lift_err_mm{-1};
std::atomic<int> g_lift_diag{0};  // g_lift_state, for the log

// Self view headgear (docs/real-partner-body.md section 20; session.ini self_headgear, default on). One head lift
// cannot fit both items: her eyes sit 1.1 cm higher above her head joint than Sam's, her scalp and hair need about
// 4.5 cm for the cap, and the lift (6.1 cm, in the neck joint's frame) made both slide on her skull whenever her head
// turned against her neck (walking). Instead each of Sam's head accessories gets a constant offset of its own in his
// head helper's frame (HLP_Head on joint 31: x = -lateral, y = forward, z = up), written into the accessory's own
// mover (partner_gear.cpp), and the lift is 0. Rows by the self body's head rig; a body without a row keeps the lift.
struct HeadgearRow {
    std::uint8_t body_id;
    std::int16_t mm[kHeadItems][3];  // per item (eyewear0, glasses00-02, cap00-03), helper frame, mm
};
// Chosen from 2560x1440 front and back close-ups (run 20261118-105952-self-headgear): the cap 5.0 cm up and 1.2 cm back
// (snug: no hair through its band from behind, the brim on her brow, eyes free; 4.5 / 4.8 cm let strands through), the
// glasses 1.1 cm up and 0.4 cm back (her eye-lid joints sit 1.1 cm higher above the head joint than Sam's: lenses on her
// eyes; 0.7 / 1.5 cm checked).
// v0.19 (section 26.3): one offset per item. The four items of the cap class are four different hats (the accessory type
// picks one; run 20261120-024449, front and back at 5.0 / 4.3 / 3.6 cm): cap00 the Bridges cap 4.2 cm (5.0 left a gap
// over her brow, 3.6 put the brim on her eyes), cap01 the Santa hat 3.5 cm (its white band sat a forehead above her brows
// at 5.0, the user's "a little high"), cap02 the headcrab 4.0 cm, cap03 the hair ornament 3.8 cm (on her hair at the back,
// not floating). The glasses keep 1.1 cm up, 0.4 cm back (the user: fine). Both Fragile bodies share fgl_head_def.
#define ST_FGL_HEAD {{0, -4, 11}, {0, -4, 11}, {0, -4, 11}, {0, -4, 11}, {0, -12, 42}, {0, -12, 35}, {0, -12, 40}, {0, -12, 38}}
constexpr HeadgearRow kHeadgearRows[] = {
    {3, ST_FGL_HEAD},   // fragile (demo_fgl_new): fgl_head_def
    {16, ST_FGL_HEAD},  // fragile_suit (demo_fgl_body_suiti): fgl_head_def
};
#undef ST_FGL_HEAD
std::atomic<bool> g_hg_cfg{true};  // session.ini self_headgear / `self_view 10 <0/1>`
std::atomic<int> g_hg_row{-1};     // the self body's row (FitOn), -1 none
std::atomic<int> g_hg_mm[kHeadItems][3] = {};  // the offsets in use (the row's, or `self_view 8/9`)
std::atomic<bool> g_hg_tuned{false};  // `self_view 8/9` set them: FitOn keeps them
// partner_gear.cpp says every tick whether it writes the offsets (its site check passed, `gear 18` on, no shown head
// item of ours refused or unknown): GetTickCount64 of the last "ready", 0 = not ready. Stale after 500 ms (gear off,
// partner_gear=0, the tick stopped), so the lift comes back whenever nothing else holds the items up.
std::atomic<ULONGLONG> g_hg_ready_ms{0};
bool HeadgearRowWanted() {
    return g_hg_cfg.load(std::memory_order_relaxed) && g_hg_row.load(std::memory_order_relaxed) >= 0;
}
bool HeadgearReadyNow() {
    const ULONGLONG t = g_hg_ready_ms.load(std::memory_order_acquire);
    return t != 0 && GetTickCount64() - t < 500;
}
// The head lift that applies now: none while the per-item offsets do the job.
int EffectiveLiftPct() {
    if (HeadgearRowWanted() && HeadgearReadyNow()) return 0;
    return g_lift_pct.load(std::memory_order_relaxed);
}

// Self view feet (section 21; session.ini self_feet_hide, default on): her foot joints (Sam layout 63, 68; their
// children, the toes and MTP_*_Foot_a, follow through the model matrices) take this scale on the self body while the
// game draws Sam's equipped boots on his fitted skeleton, so her own boots vanish inside his. Percent, 100 = off.
std::atomic<bool> g_feet_cfg{true};
std::atomic<int> g_feet_pct{1};
std::atomic<bool> g_feet_boots{false};  // the tick: Sam's boots entity is shown (inventory +0x4D80, visible flag)
std::atomic<std::uint32_t> g_feet_writes{0};  // self body pose writes with the feet scaled
// The partner's feet (boots_sync): scaled while the boots prop on it is live, shown and posed (partner_gear.cpp).
std::atomic<bool> g_pfeet_on{false};
std::atomic<int> g_pfeet_pct{1};
std::atomic<std::uint32_t> g_pfeet_writes{0};
constexpr unsigned kSamFootR = 63, kSamFootL = 68;

// The cargo menu's Sam figure (docs/real-partner-body.md section 15). While the on-foot cargo menu is
// open the local Sam is asleep in the field and a second Sam (DSPlayerAKukanEntity) stands in the
// menu's room; the self body then stands on that figure and plays its pose. The figure's model is
// published here; its own capture is kept apart from Sam's (Sam's feeds the wire).
std::atomic<std::uintptr_t> g_menu_sm{0}, g_menu_e{0}, g_menu_skel{0};
std::atomic<int> g_self_src{0};  // the self body's pose and transform source: 0 the local Sam, 1 the menu figure
SeqLock<Capture> g_menu_capture;
std::atomic<std::uint32_t> g_menu_caps{0}, g_menu_rej{0}, g_menu_copies{0}, g_menu_copy_fail{0}, g_menu_copy_aba{0};
std::atomic<ULONGLONG> g_menu_last_apply{0};  // GetTickCount64 of the last figure pose written onto the self body
std::atomic<ULONGLONG> g_menu_last_copy{0};   // ... of the last figure transform copied onto it

// Gear props (partner_gear.cpp publishes them, the detour and the tick place them).
struct GearSet {
    std::uint32_t n = 0;
    GearProp p[kMaxGearProps];
};
SeqLock<GearSet> g_gear[2];
std::atomic<std::uint32_t> g_gear_n[2] = {};  // 0: nothing published (the detour skips at once)
std::atomic<std::uint32_t> g_gear_placed[2] = {}, g_gear_aba[2] = {}, g_gear_bad[2] = {};
// The placement fixes of v0.21 (docs/real-partner-body.md section 26.7; `self_view 14 <mask>`, default 3, 0 = v0.20): bit 1
// the self view's props are also placed in the self body's own update (they update after it and before Sam: the BB pod drew
// the previous frame's tick placement, 9 cm running and 34-37 cm driving at 11 m/s), bit 2 the tick's placement is
// extrapolated one frame in the body's frame (the props whose update races the body's on another worker thread).
std::atomic<int> g_gear_fix{3};
std::atomic<std::uint32_t> g_gear_gen[2] = {};  // PublishGear bumps it: a new list starts the history again
struct GearHistSet {
    gearx::Hist h[kMaxGearProps];
    gearx::Hist body;  // the body's own world at its updates (the self slot's tick extrapolates it; section 26.7)
};
SeqLock<GearHistSet> g_gear_hist[2];       // written in the body's own update, read by the tick
GearHistSet g_gear_hist_w[2];              // the writer's copy, under g_gear_hist_busy
std::atomic<bool> g_gear_hist_busy[2] = {};  // a forced update colliding with the game's: that record is skipped
std::atomic<std::uint32_t> g_gear_body_pl[2] = {}, g_gear_xtrap[2] = {}, g_gear_xplain[2] = {};
std::atomic<std::uint32_t> g_gear_xbody{0};  // self slot ticks placed on the body's extrapolated world

// Gear skinned poses (partner_gear.cpp publishes them): the partner's backpack pose written onto our
// copies of it (the backpack prop, the rain cover ...) in their own model updates, after the original
// animate, like the bodies (docs/real-partner-body.md section 13).
constexpr unsigned kMaxGearPoseJ = wire::kMaxGearPoseJoints;  // 80: a held item's own skeleton too (section 31)
struct GearPoseBuf {
    std::uint32_t n = 0;
    float q[kMaxGearPoseJ][4];
    float t[kMaxGearPoseJ][3];
    // Interpolated in the prop's own model update (the vehicle copy, smooth bit kSmoothDetourInterp): q / t is the pose
    // at key_a, q2 / t2 the pose at key_b (n2 == n; 0 = no interpolation); play_ref = the play time at ref_ms (QPC ms).
    std::uint32_t n2 = 0;
    float q2[kMaxGearPoseJ][4];
    float t2[kMaxGearPoseJ][3];
    // the frame after key_b (n3 == n, else 0): the update may come after key_b (it runs later in the frame than the tick)
    std::uint32_t n3 = 0;
    float q3[kMaxGearPoseJ][4];
    float t3[kMaxGearPoseJ][3];
    double key_a = 0, key_b = 0, key_c = 0, play_ref = 0, ref_ms = 0;
};
struct PoseTarget {
    std::atomic<std::uintptr_t> sm{0};  // published last (release); 0 = off
    // Written by the game thread only while sm is 0 (and no detour is inside: ClearGearPose waits);
    // the detour reads each once (ApplyGearPose), so one call never mixes two props' values.
    std::atomic<std::uintptr_t> e{0}, e_vt{0}, skel{0};
    std::atomic<std::uint32_t> n{0};
    holo::Spawn spawn;  // the prop's spawn (game thread: SpawnAlive before a forced update)
    std::atomic<std::uint32_t> game_updates{0};
    ULONGLONG last_game_ms = 0;
    std::uint32_t forced = 0;
};
PoseTarget g_pt[kMaxPoseTargets];
SeqLock<GearPoseBuf> g_pt_buf[kMaxPoseTargets];
std::atomic<std::uint32_t> g_pt_live{0};  // targets published (the detour's fast check)
std::atomic<std::uint32_t> g_pt_applies{0}, g_pt_rejects{0}, g_pt_wpm{0}, g_pt_faults{0}, g_pt_torn{0};
std::atomic<bool> g_pt_faulted{false};  // a pose write faulted: gear poses off for the session
bool g_pt_forced_off = false;
// The vehicle copy's pose writes (target kVehiclePoseTarget), frame-aligned: how far the written joints turned since
// the previous write (the largest joint angle), so a stepping wheel shows as repeats and uneven steps. Detour side
// (one prop: its updates never overlap); the tick reads and resets the sums (VehiclePoseMeter).
float g_vm_prev[kMaxGearPoseJ][4];
std::uint32_t g_vm_prev_n = 0;
std::atomic<std::uint32_t> g_vm_writes{0}, g_vm_repeats{0}, g_vm_interp{0}, g_vm_holds{0};
std::atomic<std::uint64_t> g_vm_step_sum{0}, g_vm_step_sq{0};  // millidegrees, millidegrees^2 / 1000
std::atomic<std::uint32_t> g_vm_step_max{0};                     // millidegrees
std::atomic<bool> g_vm_reset{false};
std::atomic<bool> g_vm_gate{false};  // the partner's vehicle moves: the writes count
// The published-target count for the detour's fast check (0 for good after a fault).
void RecountPoseTargets() {
    std::uint32_t live = 0;
    for (const PoseTarget& x : g_pt) live += x.sm.load(std::memory_order_relaxed) != 0;
    g_pt_live.store(g_pt_faulted.load(std::memory_order_relaxed) ? 0u : live, std::memory_order_seq_cst);
}

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

bool CallPoseFns(std::uintptr_t sm, std::uintptr_t pose, bool bounds);

// The fit's write (Sam's update, under g_fit_busy): `cur` over Sam's local joints, then the game's
// model-matrix and bounds steps. False: not written (counted).
bool FitWrite(std::uintptr_t sm, std::uintptr_t pose, std::uintptr_t local, const Joint (&cur)[kJ], bool pose_fns) {
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(local), cur, sizeof(cur), &wrote) || wrote != sizeof(cur)) {
        g_fit_wpm.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!pose_fns) return true;
    std::int32_t bounds_on = 0;
    Rd(sm + kSmBoundsOn, bounds_on);
    if (!CallPoseFns(sm, pose, bounds_on != 0)) {
        g_fit_faults.fetch_add(1, std::memory_order_relaxed);
        g_fit_faulted.store(true, std::memory_order_seq_cst);  // never again this session
        return false;
    }
    return true;
}

// Riding together (ride_together.cpp; docs/real-partner-body.md section 24, decision D3-2): while the local Sam is carried as
// a passenger, his own update ends with the DRIVER's seated pose: every joint's rotation and the root / pelvis translations
// of the source (Sam's layout), his own bone lengths kept, then the game's model-matrix and bounds steps (as the fit). The
// capture then holds that pose, so the wire (STJ1: the partner body on the driver's screen sits too), the self view and the
// gear on Sam all follow it. Source 1: the partner's published frame (the driver's STJ1 at the body's play time, under
// 500 ms old); source 2: a snapshot of our own Sam's pose taken while he drove (hidden tests: `seat_measure`). As the fit, a
// value the game left as we wrote it reads as Sam's clean one, and Sam's clean values go back when the override ends.
std::atomic<int> g_ride_src{0};
std::atomic<std::uint64_t> g_apply_tick{0};  // GetTickCount64 of the newest PublishFrame
SeqLock<Capture> g_ride_snap;
std::atomic<bool> g_ride_have_snap{false};
std::atomic<std::uint32_t> g_ride_writes{0}, g_ride_nosrc{0}, g_ride_restores{0}, g_ride_failed{0};
bool g_ride_active = false;  // Sam's update only (under g_fit_busy)
Joint g_ride_written[kJ] = {};
Joint g_ride_clean[kJ] = {};

// ---- source 3: the seated idle (play test 3 of v0.20: "the passenger is doing the exact same animation as the driver even
// when turning"; docs/real-partner-body.md section 24.14). The passenger sits still in his seat whatever the driver does:
//   the base  every joint's rotation and the root / pelvis translations of ONE seated driver frame of this vehicle kind,
//             held: the driver's published frame (STJ1) latched while the vehicle drives calmly (3 - 30 m/s, its yaw rate
//             under 3 deg/s for 0.5 s: no steering, no lean, no foot down), else (until then) the ride's first frame; our
//             own Sam's calm driving fills the same cache. Kept per kind for the session; a change of base blends in.
//   the arms  the upper arms and forearms (7 / 8, 36 / 37) by two-bone IK: the hands rest on the thighs, the elbows out and
//             back (ride_pose_math.hpp), from the model matrices the game builds of the base (its model step, the
//             byte-checked g_fn_model), its joint convention measured once (the gate) on that very pass.
//   the hands the hands and fingers (9 - 28, 38 - 57) of our own Sam's pose as the carry started (he stood at the door).
// Then the game's model and bounds steps as for source 1; the capture carries it (the wire, the self view, the gear).
constexpr unsigned kSeatHandA0 = 9, kSeatHandA1 = 28, kSeatHandB0 = 38, kSeatHandB1 = 57;
bool IsSeatHand(unsigned j) { return (j >= kSeatHandA0 && j <= kSeatHandA1) || (j >= kSeatHandB0 && j <= kSeatHandB1); }
struct SeatBase {
    bool have = false, final_ = false;
    float q[kJ][4] = {};
    float troot[3] = {}, tpel[3] = {};
};
SeatBase g_seat[4];                                     // per vehicle kind (Sam's update only)
std::atomic<int> g_seat_kind{0};                        // the carried vehicle's kind (the ride tick)
std::atomic<std::uint32_t> g_seat_calm_cms{0};          // the carried vehicle drives calmly: its speed (cm/s), else 0
std::atomic<int> g_own_kind{0};                         // our own Sam drives this kind ...
std::atomic<std::uint32_t> g_own_calm_cms{0};           // ... calmly (cm/s), else 0
std::atomic<int> g_snap_kind{0};                        // the kind SnapshotRidePose was taken in (seat_measure)
std::atomic<bool> g_relax_want{false};                  // take the relaxed hands on the next override
float g_relax_q[kJ][4] = {};
bool g_relax_ok = false;
float g_seat_out[kJ][4] = {}, g_seat_out_root[3] = {}, g_seat_out_pel[3] = {};
std::int64_t g_seat_out_qpc = 0;
// the arms (IK) and its tuning (ride_test 22 / 23)
std::atomic<bool> g_ik_want{true};
std::atomic<bool> g_ik_off{false};                      // the hand error stayed over 5 cm: off for the session
int g_ik_conv = -1;                                     // -1 not measured, 0 / 1 the convention, 2 refused (Sam's update only)
unsigned g_ik_bad_run = 0;
std::atomic<int> g_ik_state{-1};                        // g_ik_conv for the tick's log
std::atomic<float> g_ik_gate_err[2] = {};
std::atomic<float> g_rest_along{0.60f}, g_rest_up{0.09f}, g_rest_in{0.03f}, g_rest_twist_deg{0.0f};
std::atomic<std::uint32_t> g_ik_solves{0}, g_ik_fails{0}, g_ik_err_n{0}, g_ik_err_um_max{0};
std::atomic<std::uint64_t> g_ik_err_um_sum{0};
std::atomic<std::uint32_t> g_seat_latches{0}, g_seat_latch_info{0};  // info: kind | from << 4 | final << 8 | speed cm/s << 12
// the hands' travel in Sam's model space (both the driver and the passenger: the proof's "hands still" metric)
struct HandBox {
    std::uint32_t gen = 0, n = 0;
    float lo[2][3] = {}, hi[2][3] = {};
};
SeqLock<HandBox> g_hand_box;
std::atomic<bool> g_hand_meter{false};
std::atomic<std::uint32_t> g_hand_gen{1};
HandBox g_hand_acc;  // Sam's update only

void NoteLatch(int kind, int from, bool final_, float speed) {
    const std::uint32_t sp = static_cast<std::uint32_t>(std::fmin(std::fmax(speed, 0.0f), 600.0f) * 100.0f);
    g_seat_latch_info.store(static_cast<std::uint32_t>(kind) | (static_cast<std::uint32_t>(from) << 4) | ((final_ ? 1u : 0u) << 8) | (sp << 12),
                            std::memory_order_relaxed);
    g_seat_latches.fetch_add(1, std::memory_order_relaxed);
}
bool LatchQuat(const float (&src)[4], float (&dst)[4]) {
    const float n = std::sqrt(src[0] * src[0] + src[1] * src[1] + src[2] * src[2] + src[3] * src[3]);
    if (!(n > 0.5f && n < 1.5f)) return false;
    for (int k = 0; k < 4; ++k) dst[k] = src[k] / n;
    return true;
}
// A base from the driver's published frame (Sam's layout); joints the frame does not carry: Sam's own (`own`).
void LatchFromApply(const ApplyBuf& a, const Joint (&own)[kJ], SeatBase& b) {
    for (unsigned j = 0; j < kJ; ++j) {
        const float q[4] = {a.q[j].x, a.q[j].y, a.q[j].z, a.q[j].w};
        if (!LatchQuat(q, b.q[j])) LatchQuat(own[j].q, b.q[j]);
    }
    for (int k = 0; k < 3; ++k) {
        b.troot[k] = std::isfinite(a.troot[k]) && std::fabs(a.troot[k]) < 3.0f ? a.troot[k] : own[kSamRoot].t[k];
        b.tpel[k] = std::isfinite(a.tpelvis[k]) && std::fabs(a.tpelvis[k]) < 3.0f ? a.tpelvis[k] : own[kSamPelvis].t[k];
    }
    b.have = true;
}
// Our own Sam drives calmly (Sam's update, before the capture is written): his pose is the base of that kind.
void LatchFromOwn(const Capture& c) {
    const int kind = g_own_kind.load(std::memory_order_relaxed);
    const std::uint32_t calm = g_own_calm_cms.load(std::memory_order_relaxed);
    if (kind < 1 || kind > 3 || !calm || g_ride_active || g_ride_src.load(std::memory_order_relaxed)) return;
    SeatBase& b = g_seat[kind];
    if (b.final_) return;
    for (unsigned j = 0; j < kJ; ++j)
        if (!LatchQuat(c.j[j].q, b.q[j])) return;
    for (int k = 0; k < 3; ++k) b.troot[k] = c.j[kSamRoot].t[k], b.tpel[k] = c.j[kSamPelvis].t[k];
    b.have = b.final_ = true;
    NoteLatch(kind, 2, true, static_cast<float>(calm) / 100.0f);
}
// Source 3's base is there (latched now if due). `c` holds Sam's own (clean) pose of this update.
bool SeatedReady(const Capture& c) {
    if (g_relax_want.exchange(false, std::memory_order_acq_rel)) {
        g_relax_ok = !g_ride_active;  // Sam's own pose, not one of ours
        for (unsigned j = 0; j < kJ && g_relax_ok; ++j)
            if (IsSeatHand(j) && !LatchQuat(c.j[j].q, g_relax_q[j])) g_relax_ok = false;
    }
    const int kind = g_seat_kind.load(std::memory_order_relaxed);
    if (kind < 1 || kind > 3) return false;
    SeatBase& b = g_seat[kind];
    const std::uint32_t calm = g_seat_calm_cms.load(std::memory_order_relaxed);
    if (!b.have && g_snap_kind.load(std::memory_order_relaxed) == kind && g_ride_have_snap.load(std::memory_order_acquire)) {
        // the solo seat test: our own driving pose kept by seat_measure
        static thread_local Capture snap;
        if (g_ride_snap.Read(snap)) {
            bool ok = true;
            for (unsigned j = 0; j < kJ && ok; ++j) ok = LatchQuat(snap.j[j].q, b.q[j]);
            if (ok) {
                for (int k = 0; k < 3; ++k) b.troot[k] = snap.j[kSamRoot].t[k], b.tpel[k] = snap.j[kSamPelvis].t[k];
                b.have = b.final_ = true;
                NoteLatch(kind, 3, true, 0);
            }
        }
    }
    if ((!b.have || (!b.final_ && calm)) && GetTickCount64() - g_apply_tick.load(std::memory_order_acquire) < 200) {
        static thread_local ApplyBuf a;
        if (g_apply.Read(a)) {
            LatchFromApply(a, c.j, b);
            b.final_ = calm != 0;
            NoteLatch(kind, 1, b.final_, static_cast<float>(calm) / 100.0f);
        }
    }
    return b.have;
}

using ridepose::V;
V MdlPos(const float (&m)[4][4]) { return {m[3][0], m[3][1], m[3][2]}; }
bool MdlRows(const float (&m)[4][4], ridepose::M3* r) {
    for (int k = 0; k < 3; ++k) {
        const V a{m[k][0], m[k][1], m[k][2]};
        const double n = ridepose::Len(a);
        if (!(n > 0.2 && n < 5.0)) return false;
        ridepose::SetRow(*r, k, a * (1.0 / n));
    }
    return true;
}
void QuatToJoint(const double (&q)[4], float (&dst)[4]) {
    // the hemisphere of the joint's previous value (the partner body interpolates frames)
    const double d = q[0] * dst[0] + q[1] * dst[1] + q[2] * dst[2] + q[3] * dst[3];
    const double s = d < 0 ? -1.0 : 1.0;
    for (int k = 0; k < 4; ++k) dst[k] = static_cast<float>(q[k] * s);
}

// The arms by IK on the base (Sam's update; the base and the hands are in `cur`). Writes `cur` and runs the game's model
// step on it (no bounds), reads the model matrices, measures the convention once, solves both arms into cur[7 / 8 / 36 /
// 37]. `tw` gets the wrists' targets. False: nothing solved (cur unchanged but for what was written).
bool SeatedArms(std::uintptr_t sm, std::uintptr_t pose, std::uintptr_t local, std::uintptr_t mb, Joint (&cur)[kJ], V (&tw)[2]) {
    if (!mb || g_ik_conv == 2 || !g_ik_want.load(std::memory_order_relaxed) || g_ik_off.load(std::memory_order_relaxed)) return false;
    if (!FitWrite(sm, pose, local, cur, false)) return false;
    if (!CallPoseFns(sm, pose, false)) {
        g_fit_faults.fetch_add(1, std::memory_order_relaxed);
        g_fit_faulted.store(true, std::memory_order_seq_cst);
        return false;
    }
    static thread_local float m[kJ][4][4];
    if (!mem::Read(mb, m, sizeof(m))) return false;
    ridepose::M3 r[kJ];
    for (unsigned j : {1u, 2u, 5u, 6u, 7u, 8u, 9u, 35u, 36u, 37u, 38u, 61u, 62u, 66u, 67u})
        if (!MdlRows(m[j], &r[j])) return false;
    if (g_ik_conv < 0) {
        // the gate: the child's rows predicted from the parent's and the local quaternion we wrote, both conventions
        static const unsigned kPairs[][2] = {{6, 7}, {7, 8}, {36, 37}, {61, 62}, {1, 2}};
        double err[2] = {0, 0};
        for (int conv = 0; conv < 2; ++conv)
            for (const auto& p : kPairs) {
                const double q[4] = {cur[p[1]].q[0], cur[p[1]].q[1], cur[p[1]].q[2], cur[p[1]].q[3]};
                err[conv] = std::fmax(err[conv], ridepose::MaxDiff(ridepose::Mul(ridepose::LocalRows(q, conv), r[p[0]]), r[p[1]]));
            }
        g_ik_gate_err[0].store(static_cast<float>(err[0]), std::memory_order_relaxed);
        g_ik_gate_err[1].store(static_cast<float>(err[1]), std::memory_order_relaxed);
        g_ik_conv = err[0] < 1e-3 && err[0] <= err[1] ? 0 : err[1] < 1e-3 ? 1 : 2;
        g_ik_state.store(g_ik_conv, std::memory_order_relaxed);
        if (g_ik_conv == 2) return false;
    }
    ridepose::HandRest rest;
    rest.along = g_rest_along.load(std::memory_order_relaxed);
    rest.up = g_rest_up.load(std::memory_order_relaxed);
    rest.in = g_rest_in.load(std::memory_order_relaxed);
    rest.twist = g_rest_twist_deg.load(std::memory_order_relaxed) * 0.017453292519943295;
    const V p1 = MdlPos(m[1]), p5 = MdlPos(m[5]), p61 = MdlPos(m[61]), p62 = MdlPos(m[62]), p66 = MdlPos(m[66]), p67 = MdlPos(m[67]);
    const V up = ridepose::Norm(p5 - p1);
    const V lat = ridepose::Norm(MdlPos(m[7]) - MdlPos(m[36]));  // to the right
    V fwd = (p62 - p61) + (p67 - p66);
    fwd = ridepose::Norm(fwd - up * ridepose::Dot(fwd, up));
    if (ridepose::Len(up) < 0.5 || ridepose::Len(lat) < 0.5 || ridepose::Len(fwd) < 0.5) return false;
    const V bend = ridepose::Cross(up * -1.0, fwd);  // an arm hanging down, bending forward
    bool any = false;
    for (int side = 0; side < 2; ++side) {
        const unsigned js = side ? 36 : 7, je = side ? 37 : 8, jw = side ? 38 : 9, jp = side ? 35 : 6;
        const V hip = side ? p66 : p61, knee = side ? p67 : p62, other = side ? p61 : p66;
        ridepose::ArmIn in;
        in.s = MdlPos(m[js]), in.e = MdlPos(m[je]), in.w = MdlPos(m[jw]);
        in.parent = r[jp], in.upper = r[js], in.fore = r[je];
        in.target = ridepose::RestTarget(hip, knee, up, ridepose::Norm(other - hip), rest);
        in.pole = ridepose::PoleDir(side ? lat * -1.0 : lat, fwd, up, rest);
        in.fallback_n = bend;
        in.twist = side ? -rest.twist : rest.twist;
        in.conv = g_ik_conv;
        ridepose::ArmOut o;
        if (!ridepose::SolveArm(in, &o)) {
            g_ik_fails.fetch_add(1, std::memory_order_relaxed);
            tw[side] = in.w;
            continue;
        }
        QuatToJoint(o.q_upper, cur[js].q);
        QuatToJoint(o.q_fore, cur[je].q);
        tw[side] = o.wrist;
        any = true;
    }
    if (any) g_ik_solves.fetch_add(1, std::memory_order_relaxed);
    return any;
}

// Source 3 into `cur` (Sam's own joints): the smoothed base, the relaxed hands, the arms; then the game's model and bounds
// steps. False: not written.
bool SeatedIdle(std::uintptr_t sm, std::uintptr_t pose, std::uintptr_t local, std::uintptr_t mb, Joint (&cur)[kJ]) {
    const int kind = g_seat_kind.load(std::memory_order_relaxed);
    if (kind < 1 || kind > 3 || !g_seat[kind].have) return false;
    const SeatBase& b = g_seat[kind];
    // a new base (the ride's first frame, then the calm one) blends in over ~0.3 s; the first update of a ride snaps
    const std::int64_t now = Qpc();
    double k = 1.0;
    if (g_ride_active && g_seat_out_qpc && g_qpc_per_ms > 0) {
        const double dt = static_cast<double>(now - g_seat_out_qpc) / g_qpc_per_ms / 1000.0;
        k = dt > 0 && dt < 1.0 ? 1.0 - std::exp(-dt / 0.12) : 1.0;
    }
    g_seat_out_qpc = now;
    for (unsigned j = 0; j < kJ; ++j) {
        float* o = g_seat_out[j];
        const float* t = b.q[j];
        const float d = o[0] * t[0] + o[1] * t[1] + o[2] * t[2] + o[3] * t[3];
        const float s = d < 0 ? -1.0f : 1.0f;
        float n2 = 0;
        for (int i = 0; i < 4; ++i) {
            o[i] = k >= 1.0 ? t[i] : static_cast<float>(o[i] + (s * t[i] - o[i]) * k);
            n2 += o[i] * o[i];
        }
        const float n = std::sqrt(n2);
        if (!(n > 0.5f)) std::memcpy(o, t, sizeof(float) * 4);
        else
            for (int i = 0; i < 4; ++i) o[i] /= n;
    }
    for (int i = 0; i < 3; ++i) {
        g_seat_out_root[i] = k >= 1.0 ? b.troot[i] : static_cast<float>(g_seat_out_root[i] + (b.troot[i] - g_seat_out_root[i]) * k);
        g_seat_out_pel[i] = k >= 1.0 ? b.tpel[i] : static_cast<float>(g_seat_out_pel[i] + (b.tpel[i] - g_seat_out_pel[i]) * k);
    }
    for (unsigned j = 0; j < kJ; ++j) std::memcpy(cur[j].q, IsSeatHand(j) && g_relax_ok ? g_relax_q[j] : g_seat_out[j], sizeof(cur[j].q));
    for (int i = 0; i < 3; ++i) cur[kSamRoot].t[i] = g_seat_out_root[i], cur[kSamPelvis].t[i] = g_seat_out_pel[i];
    V tw[2];
    const bool arms = SeatedArms(sm, pose, local, mb, cur, tw);
    if (g_fit_faulted.load(std::memory_order_relaxed)) return false;
    if (!FitWrite(sm, pose, local, cur, true)) return false;
    if (arms) {
        // the hands where the IK put them (the model step of what we wrote): a mismatch means the frames are not what the
        // gate measured; 30 updates in a row over 5 cm: the arms off for the session
        float w0[4][4], w1[4][4];
        if (mem::Read(mb + 0x40ull * 9, w0, sizeof(w0)) && mem::Read(mb + 0x40ull * 38, w1, sizeof(w1))) {
            const double e = std::fmax(ridepose::Len(MdlPos(w0) - tw[0]), ridepose::Len(MdlPos(w1) - tw[1]));
            if (std::isfinite(e)) {
                const std::uint32_t um = static_cast<std::uint32_t>(std::fmin(e, 10.0) * 1e6);
                g_ik_err_um_sum.fetch_add(um, std::memory_order_relaxed);
                g_ik_err_n.fetch_add(1, std::memory_order_relaxed);
                std::uint32_t mx = g_ik_err_um_max.load(std::memory_order_relaxed);
                while (um > mx && !g_ik_err_um_max.compare_exchange_weak(mx, um, std::memory_order_relaxed)) {
                }
                g_ik_bad_run = e > 0.05 ? g_ik_bad_run + 1 : 0;
                if (g_ik_bad_run >= 30) g_ik_off.store(true, std::memory_order_relaxed);
            }
        }
    }
    return true;
}

// The hands' place in Sam's model space, accumulated per window (the tick takes it).
void HandMeter(std::uintptr_t mb) {
    if (!g_hand_meter.load(std::memory_order_relaxed) || !mb) return;
    float w[2][4][4];
    if (!mem::Read(mb + 0x40ull * 9, w[0], sizeof(w[0])) || !mem::Read(mb + 0x40ull * 38, w[1], sizeof(w[1]))) return;
    const std::uint32_t gen = g_hand_gen.load(std::memory_order_acquire);
    if (g_hand_acc.gen != gen) g_hand_acc = HandBox{}, g_hand_acc.gen = gen;
    for (int s = 0; s < 2; ++s)
        for (int k = 0; k < 3; ++k) {
            const float v = w[s][3][k];
            if (!std::isfinite(v)) return;
            if (g_hand_acc.n == 0 || v < g_hand_acc.lo[s][k]) g_hand_acc.lo[s][k] = v;
            if (g_hand_acc.n == 0 || v > g_hand_acc.hi[s][k]) g_hand_acc.hi[s][k] = v;
        }
    ++g_hand_acc.n;
    g_hand_box.Write(g_hand_acc);
}

void RideOverride(std::uintptr_t sm, std::uintptr_t pose, std::uintptr_t local, std::uintptr_t mb, Capture& c) {
    unsigned held = 0;
    if (g_ride_active) {
        for (unsigned j = 0; j < kJ; ++j) {
            if (std::memcmp(c.j[j].q, g_ride_written[j].q, 4 * sizeof(float)) == 0 &&
                std::memcmp(c.j[j].q, g_ride_clean[j].q, 4 * sizeof(float)) != 0) {
                std::memcpy(c.j[j].q, g_ride_clean[j].q, 4 * sizeof(float));
                ++held;
            }
            if (std::memcmp(c.j[j].t, g_ride_written[j].t, 3 * sizeof(float)) == 0 &&
                std::memcmp(c.j[j].t, g_ride_clean[j].t, 3 * sizeof(float)) != 0) {
                std::memcpy(c.j[j].t, g_ride_clean[j].t, 3 * sizeof(float));
                ++held;
            }
        }
    }
    const int src = g_ride_src.load(std::memory_order_acquire);
    static thread_local ApplyBuf a;
    static thread_local Capture snap;
    bool have = false;
    if (src == 1) have = GetTickCount64() - g_apply_tick.load(std::memory_order_acquire) < 500 && g_apply.Read(a);
    else if (src == 2) have = g_ride_have_snap.load(std::memory_order_acquire) && g_ride_snap.Read(snap);
    else if (src == 3) have = SeatedReady(c);
    if (!have) {
        if (src) g_ride_nosrc.fetch_add(1, std::memory_order_relaxed);
        if (g_ride_active) {
            if (held && FitWrite(sm, pose, local, c.j, true)) g_ride_restores.fetch_add(1, std::memory_order_relaxed);
            g_ride_active = false;
        }
        return;
    }
    std::memcpy(g_ride_clean, c.j, sizeof(g_ride_clean));
    Joint cur[kJ];
    std::memcpy(cur, c.j, sizeof(cur));
    if (src == 3) {
        if (!SeatedIdle(sm, pose, local, mb, cur)) {
            g_ride_failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::memcpy(g_ride_written, cur, sizeof(g_ride_written));
        std::memcpy(c.j, cur, sizeof(c.j));
        g_ride_active = true;
        g_ride_writes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (unsigned j = 0; j < kJ; ++j) {
        float q[4];
        if (src == 1) q[0] = a.q[j].x, q[1] = a.q[j].y, q[2] = a.q[j].z, q[3] = a.q[j].w;
        else std::memcpy(q, snap.j[j].q, sizeof(q));
        const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (!(n > 0.5f && n < 1.5f)) continue;  // a joint the frame does not carry: Sam's own
        for (float& v : q) v /= n;
        std::memcpy(cur[j].q, q, sizeof(q));
    }
    const float* troot = src == 1 ? a.troot : snap.j[kSamRoot].t;
    const float* tpel = src == 1 ? a.tpelvis : snap.j[kSamPelvis].t;
    for (int k = 0; k < 3; ++k) {
        if (std::isfinite(troot[k]) && std::fabs(troot[k]) < 3.0f) cur[kSamRoot].t[k] = troot[k];
        if (std::isfinite(tpel[k]) && std::fabs(tpel[k]) < 3.0f) cur[kSamPelvis].t[k] = tpel[k];
    }
    if (!FitWrite(sm, pose, local, cur, true)) {
        g_ride_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::memcpy(g_ride_written, cur, sizeof(g_ride_written));
    std::memcpy(c.j, cur, sizeof(c.j));
    g_ride_active = true;
    g_ride_writes.fetch_add(1, std::memory_order_relaxed);
}

// Round 12 (section 38.2): Sam's root joint of this update, for TrackSam in the same detour call (the drawn meter, the
// trace): its local translation from the capture (the root has no parent: this is its model point; the
// self body copies exactly this one) and the translation row of its model matrix as his animate left it (the trace compares
// the two: a model matrix a frame old shows). CaptureSam clears it first, so a skipped capture leaves none.
struct SamModelPts {
    bool ok = false, m_ok = false;
    float rl[3] = {}, m0[3] = {};
};
thread_local SamModelPts t_sam_m;

void CaptureSam(std::uintptr_t sm) {
    t_sam_m.ok = false;
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
    // One Sam update at a time through the guard, the capture and the fit: a second one at the same
    // moment skips its capture (counted `busy`), so no unguarded pose ever reaches the wire.
    if (g_fit_busy.exchange(true, std::memory_order_acquire)) {
        g_fit_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The fit (section 16): values the game left as we wrote them read as Sam's clean ones.
    unsigned held = 0, qheld = 0, sheld = 0;
    const bool was_active = g_fit_active;
    if (was_active) {
        for (unsigned j = 0; j < kJ; ++j) {
            if (std::memcmp(c.j[j].t, g_fit_written[j].t, 3 * sizeof(float)) == 0 &&
                std::memcmp(c.j[j].t, g_fit_clean_t[j], 3 * sizeof(float)) != 0) {
                std::memcpy(c.j[j].t, g_fit_clean_t[j], 3 * sizeof(float));
                ++held;
            }
            if (std::memcmp(c.j[j].q, g_fit_written[j].q, 4 * sizeof(float)) == 0 &&
                std::memcmp(c.j[j].q, g_fit_clean_q[j], 4 * sizeof(float)) != 0) {
                std::memcpy(c.j[j].q, g_fit_clean_q[j], 4 * sizeof(float));
                ++qheld;
            }
            if (std::memcmp(c.j[j].s, g_fit_written[j].s, 3 * sizeof(float)) == 0 &&
                std::memcmp(c.j[j].s, g_fit_clean_s[j], 3 * sizeof(float)) != 0) {
                std::memcpy(c.j[j].s, g_fit_clean_s[j], 3 * sizeof(float));
                ++sheld;
            }
        }
        (held || qheld || sheld ? g_fit_updates_held : g_fit_updates_fresh).fetch_add(1, std::memory_order_relaxed);
        g_fit_joints_held.fetch_add(held, std::memory_order_relaxed);
        g_fit_qheld.fetch_add(qheld, std::memory_order_relaxed);
        g_fit_sheld.fetch_add(sheld, std::memory_order_relaxed);
    }
    // Did the game move Sam? His clean joints against the previous capture's (an update that left every joint as it was, or
    // only as the fit wrote it, is no progress: the stall guard's signal, section 33).
    for (unsigned j = 0; j < kJ; ++j) {
        if (std::memcmp(c.j[j].q, g_fit_clean_q[j], 4 * sizeof(float)) != 0 || std::memcmp(c.j[j].t, g_fit_clean_t[j], 3 * sizeof(float)) != 0 ||
            std::memcmp(c.j[j].s, g_fit_clean_s[j], 3 * sizeof(float)) != 0) {
            g_sam_last_change.store(GetTickCount64(), std::memory_order_relaxed);
            break;
        }
    }
    if (g_ride_src.load(std::memory_order_relaxed) || g_ride_active) RideOverride(sm, pose, local, Field<std::uintptr_t>(hdr, kHdrModel), c);
    else LatchFromOwn(c);
    HandMeter(Field<std::uintptr_t>(hdr, kHdrModel));
    {
        // The review's leak check: the captured pelvis length with the fit on / off, and (fit on) how
        // many of the translations the fit changes came back nearer the shape than Sam's own.
        const float* p = c.j[kSamPelvis].t;
        const double len = std::sqrt(static_cast<double>(p[0]) * p[0] + static_cast<double>(p[1]) * p[1] + static_cast<double>(p[2]) * p[2]);
        if (std::isfinite(len) && len < 3.0) {
            g_fit_pel_sum[was_active ? 1 : 0].fetch_add(static_cast<std::uint64_t>(len * 1e6), std::memory_order_relaxed);
            g_fit_pel_n[was_active ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_self_follow.load(std::memory_order_relaxed) == 4) {  // his root joint (section 38.2)
        SamModelPts& sp = t_sam_m;
        sp.ok = true;
        for (int k = 0; k < 3; ++k) sp.ok = sp.ok && std::isfinite(c.j[kSamRoot].t[k]) && std::fabs(c.j[kSamRoot].t[k]) < 50.0f;
        if (sp.ok) std::memcpy(sp.rl, c.j[kSamRoot].t, sizeof(sp.rl));
        const std::uintptr_t mb = Field<std::uintptr_t>(hdr, kHdrModel);
        float m[4][4];
        sp.m_ok = sp.ok && mb && mem::Read(mb + 0x40ull * kSamRoot, m, sizeof(m)) && std::isfinite(m[3][0]) && std::isfinite(m[3][1]) &&
                  std::isfinite(m[3][2]);
        if (sp.m_ok) sp.m0[0] = m[3][0], sp.m0[1] = m[3][1], sp.m0[2] = m[3][2];
    }
    c.tick_ms = GetTickCount64();
    c.qpc = Qpc();
    c.frame = g_ct.captures.load(std::memory_order_relaxed) + 1;
    if (g_capture.Write(c)) g_ct.captures.fetch_add(1, std::memory_order_relaxed);
    for (unsigned j = 0; j < kJ; ++j) {
        std::memcpy(g_fit_clean_q[j], c.j[j].q, sizeof(g_fit_clean_q[j]));
        std::memcpy(g_fit_clean_t[j], c.j[j].t, sizeof(g_fit_clean_t[j]));
        std::memcpy(g_fit_clean_s[j], c.j[j].s, sizeof(g_fit_clean_s[j]));
    }
    const bool want = g_fit_want.load(std::memory_order_acquire) && g_fit_cfg.load(std::memory_order_relaxed) &&
                      !g_fit_faulted.load(std::memory_order_relaxed);
    if (want) {
        static thread_local FitShape sh;
        if (!g_fit_shape.Read(sh)) {
            g_fit_torn.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (g_lift_reset.exchange(false, std::memory_order_acq_rel)) g_lift_state = 0, g_fit_ref_ok = false;
            if (!was_active && !g_fit_ref_ok) {
                for (unsigned j = 0; j < kJ; ++j) std::memcpy(g_fit_ref_t[j], c.j[j].t, sizeof(g_fit_ref_t[j]));
                g_fit_ref_ok = true;
            }
            g_fit_last_shape = sh;
            g_fit_last_shape_ok = true;
            if (was_active && g_fit_ref_ok) LeakCheck(sh, c, 1);
            // The pose: the one the self body was last written from (same pose), else Sam's newest.
            // (The lift is measured on Sam's newest pose: the same rotations as the clean head below.)
            const Joint* src = c.j;
            if (g_lift_state != 0 && g_fit_sync.load(std::memory_order_relaxed) && g_self_src.load(std::memory_order_acquire) == 0 &&
                g_qpc_per_ms > 0) {
                static thread_local Capture ap;
                if (g_self_applied.Read(ap) && ap.frame != 0 && ap.frame <= c.frame &&
                    static_cast<double>(c.qpc - ap.qpc) / g_qpc_per_ms < 150.0) {
                    src = ap.j;
                    g_fit_sync_used.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_fit_sync_fallback.fetch_add(1, std::memory_order_relaxed);
                }
            }
            Joint cur[kJ];
            for (unsigned j = 0; j < kJ; ++j) {
                std::memcpy(cur[j].q, src[j].q, sizeof(cur[j].q));
                if (j == kSamRoot) {
                    std::memcpy(cur[j].t, src[j].t, sizeof(cur[j].t));
                    std::memcpy(cur[j].s, c.j[j].s, sizeof(cur[j].s));
                } else if (j == kSamPelvis) {
                    HipsT(src[j].t, sh.pelvis_ratio, sh.pelvis_off, sh.pelvis_off_ok != 0, cur[j].t);
                    cur[j].t[3] = c.j[j].t[3];
                    std::memcpy(cur[j].s, c.j[j].s, sizeof(cur[j].s));
                } else if (sh.on[j]) {
                    std::memcpy(cur[j].t, sh.t[j], sizeof(cur[j].t));
                    std::memcpy(cur[j].s, sh.s[j], sizeof(cur[j].s));
                } else {
                    std::memcpy(cur[j].t, c.j[j].t, sizeof(cur[j].t));
                    std::memcpy(cur[j].s, c.j[j].s, sizeof(cur[j].s));
                }
            }
            const int pct = EffectiveLiftPct();
            if (g_lift_state == 1)
                for (int k = 0; k < 3; ++k) cur[kSamHeadJ].t[k] += g_lift_local[k] * static_cast<float>(pct) / 100.0f;
            // The lift's measure (once per body): Sam's own head (the original animate's model matrices,
            // before our write) against the fitted one, in the neck joint's frame.
            const std::uintptr_t mb = Field<std::uintptr_t>(hdr, kHdrModel);
            float clean_head[4][4] = {};
            const bool measure = g_lift_state == 0 && mb && mem::Read(mb + 0x40ull * kSamHeadJ, clean_head, sizeof(clean_head));
            if (FitWrite(sm, pose, local, cur, true)) {
                std::memcpy(g_fit_written, cur, sizeof(g_fit_written));
                g_fit_active = true;
                g_fit_writes.fetch_add(1, std::memory_order_relaxed);
                float neck[4][4], head[4][4];
                if (measure && mem::Read(mb + 0x40ull * kSamNeck2, neck, sizeof(neck)) && mem::Read(mb + 0x40ull * kSamHeadJ, head, sizeof(head))) {
                    // The convention (rows = the parent's scaled axes, row 3 its position) checked on this
                    // very write: the head must sit at neck + t . axes.
                    double err = 0, lift = 0, d[3];
                    bool ok = true;
                    for (int k = 0; k < 3; ++k) {
                        const double pred = neck[3][k] + cur[kSamHeadJ].t[0] * neck[0][k] + cur[kSamHeadJ].t[1] * neck[1][k] + cur[kSamHeadJ].t[2] * neck[2][k];
                        err += (pred - head[3][k]) * (pred - head[3][k]);
                        d[k] = static_cast<double>(clean_head[3][k]) - head[3][k];
                        lift += d[k] * d[k];
                        ok = ok && std::isfinite(pred) && std::isfinite(d[k]);
                    }
                    err = std::sqrt(err), lift = std::sqrt(lift);
                    float loc[3] = {};
                    for (int a = 0; a < 3 && ok; ++a) {
                        const double n2 = static_cast<double>(neck[a][0]) * neck[a][0] + static_cast<double>(neck[a][1]) * neck[a][1] +
                                          static_cast<double>(neck[a][2]) * neck[a][2];
                        if (!(n2 > 0.04 && n2 < 25.0)) ok = false;
                        else loc[a] = static_cast<float>((d[0] * neck[a][0] + d[1] * neck[a][1] + d[2] * neck[a][2]) / n2);
                    }
                    ok = ok && err < 0.002 && lift < 0.20;
                    if (ok) std::memcpy(g_lift_local, loc, sizeof(g_lift_local));
                    g_lift_state = ok ? 1 : 2;
                    g_lift_cm.store(static_cast<float>(lift * 100.0), std::memory_order_relaxed);
                    g_lift_err_mm.store(static_cast<float>(err * 1000.0), std::memory_order_relaxed);
                    g_lift_diag.store(g_lift_state, std::memory_order_relaxed);
                    g_lift_pct_pub = -1;  // publish below
                }
                const int pub_pct = g_lift_state == 1 ? pct : 0;
                if (pub_pct != g_lift_pct_pub) {
                    LiftPub lp;
                    for (int k = 0; k < 3; ++k) lp.local[k] = g_lift_local[k] * static_cast<float>(pub_pct) / 100.0f;
                    lp.on = pub_pct != 0;
                    if (g_lift_pub.Write(lp)) g_lift_pct_pub = pub_pct;
                }
            }
        }
    } else if (!g_fit_active && g_fit_ref_ok && g_fit_last_shape_ok && g_fit_shown.load(std::memory_order_relaxed)) {
        LeakCheck(g_fit_last_shape, c, 0);   // the self body shown, the fit off (`self_view 5 0`): the comparison
    } else if (g_fit_active) {
        // Off: Sam's clean values back where the game left ours (it rewrote the rest itself).
        if (held || qheld || sheld) {
            Joint clean[kJ];
            std::memcpy(clean, c.j, sizeof(clean));
            if (FitWrite(sm, pose, local, clean, !g_fit_faulted.load(std::memory_order_relaxed)))
                g_fit_restores.fetch_add(1, std::memory_order_relaxed);
        }
        g_fit_active = false;
        if (g_lift_pct_pub != 0) {
            LiftPub lp;
            if (g_lift_pub.Write(lp)) g_lift_pct_pub = 0;
        }
    }
    g_fit_busy.store(false, std::memory_order_release);
}

// The cargo menu's figure: the same capture, into its own buffer (section 15).
void CaptureMenu(std::uintptr_t sm) {
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0, local = 0;
    if (!ReadPoseHdr(sm, hdr, &pose)) return;
    if (CheckHdr(hdr, g_menu_skel.load(std::memory_order_acquire), &local)) {
        g_menu_rej.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Capture c;
    if (!mem::Read(local, c.j, sizeof(c.j))) return;
    c.tick_ms = GetTickCount64();
    c.qpc = Qpc();
    c.frame = g_menu_caps.load(std::memory_order_relaxed) + 1;
    if (g_menu_capture.Write(c)) g_menu_caps.fetch_add(1, std::memory_order_relaxed);
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

// ---------------------------------------------------------------- gear placement (detour or tick)
struct Xf {
    double p[3];
    float r[3][3];  // rows: the frame's right, forward, up axes in the parent frame
};
#pragma pack(push, 4)
struct PackedWt {  // the entity world transform layout (+0xC8 position, +0xE0 rotation rows)
    double p[3];
    float r[3][3];
};
#pragma pack(pop)
static_assert(sizeof(PackedWt) == kWorldBytes, "world transform layout");

// child (in parent's frame) -> world
Xf Compose(const Xf& parent, const double (&p)[3], const float (&r)[3][3]) {
    Xf o;
    for (int k = 0; k < 3; ++k) {
        o.p[k] = parent.p[k] + p[0] * parent.r[0][k] + p[1] * parent.r[1][k] + p[2] * parent.r[2][k];
        for (int c = 0; c < 3; ++c) o.r[k][c] = r[k][0] * parent.r[0][c] + r[k][1] * parent.r[1][c] + r[k][2] * parent.r[2][c];
    }
    return o;
}

// A pose model matrix (4 x float4: three axes, then the translation) as a frame: the axes are
// normalised (bone scale), and a matrix that is not finite / not a rotation is refused.
bool JointFrame(const float (&m)[4][4], double (&p)[3], float (&r)[3][3]) {
    for (int k = 0; k < 3; ++k) {
        const float* a = m[k];
        const float len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (!std::isfinite(len) || len < 0.2f || len > 5.0f) return false;
        for (int c = 0; c < 3; ++c) r[k][c] = a[c] / len;
        if (!std::isfinite(m[3][k]) || std::fabs(m[3][k]) > 10.0f) return false;
        p[k] = m[3][k];
    }
    const float d = r[0][0] * r[1][0] + r[0][1] * r[1][1] + r[0][2] * r[1][2];
    return std::fabs(d) < 0.2f;
}

// ---- The drawn-vs-anchor meter (hidden test runs only, `self_view 15 1`; docs/real-partner-body.md section 26.7). The game
// stamps every entity update with the TSC: +0x250 at the start of Entity::Update (0x14234D7A9), +0x258 at its end
// (0x14234DA87). Each of our gear placements is stamped too, with its kind, and each body update's world is kept. The tick
// then finds, for each prop's newest update, the placement it had when it ran (what the game draws for it) and how far that
// is from the prop's anchor on the body as the body drew it: the body's world in its update times its pose, times the
// prop's offset. The tick-time meters ([headgear], `Sam vs body`) cannot see this: at tick time everything agrees.
constexpr std::uintptr_t kEntStampStart = 0x250, kEntStampEnd = 0x258;
constexpr std::uintptr_t kStampStartSite = 0x234D7A9, kStampEndSite = 0x234DA87;
enum GearPlaceKind : int { kPlaceTick = 0, kPlaceBody = 1, kPlaceSam = 2 };
struct GmPlace {
    std::uint64_t tsc = 0;
    int kind = 0;
    double p[3] = {};
};
constexpr unsigned kGmRing = 8;
struct GmRing {
    GmPlace e[kGmRing];
    std::uint32_t n = 0;
};
struct GmBody {
    std::uint64_t tsc = 0;
    Xf w{};
    bool ok = false;
};
std::atomic<bool> g_gm_on{false};
std::atomic<bool> g_gm_lock{false};
GmRing g_gm_ring[2][kMaxGearProps];
GmBody g_gm_body[2][4];
std::uint32_t g_gm_body_n[2] = {};
void GmLock() {
    while (g_gm_lock.exchange(true, std::memory_order_acquire)) _mm_pause();
}
void GmUnlock() { g_gm_lock.store(false, std::memory_order_release); }
void GmRecPlace(int slot, unsigned i, int kind, const double (&p)[3]) {
    const std::uint64_t t = __rdtsc();
    GmLock();
    GmRing& r = g_gm_ring[slot][i];
    GmPlace& d = r.e[r.n++ % kGmRing];
    d.tsc = t, d.kind = kind;
    std::memcpy(d.p, p, sizeof(d.p));
    GmUnlock();
}
void GmRecBody(int slot, const Xf& w) {
    const std::uint64_t t = __rdtsc();
    GmLock();
    GmBody& d = g_gm_body[slot][g_gm_body_n[slot]++ % 4];
    d.tsc = t, d.w = w, d.ok = true;
    GmUnlock();
}

double QpcMs() { return g_qpc_per_ms > 0 ? static_cast<double>(Qpc()) / g_qpc_per_ms : 0; }

// Places the slot's published props on a body whose world transform is `world` and whose pose's
// model matrices start at `mb` (77 x 0x40 bytes). Fault-free reads; the game's own setter per prop
// after its vtable + GUID check. Returns the props placed. `kind`: who places (the meter's stamp).
// From the body's own update each prop's placement is also kept in the body's frame; from the tick
// (fix bit 2) a prop with two such records from consecutive body updates is placed one body update
// ahead of the newest (section 26.7).
unsigned PlaceGear(int slot, const Xf& world, std::uintptr_t mb, const std::uint8_t* inv, int kind = kPlaceTick) {
    if (!g_fn_setwt || !mb || g_gear_n[slot].load(std::memory_order_acquire) == 0) return 0;
    // the list's generation before the list: PublishGear bumps it before and after writing, so a record never pairs a
    // list with the generation of a later one
    const std::uint32_t gen = g_gear_gen[slot].load(std::memory_order_acquire);
    static thread_local GearSet set;
    if (!g_gear[slot].Read(set) || set.n == 0 || set.n > kMaxGearProps) return 0;
    Xf w[kMaxGearProps];
    bool have[kMaxGearProps] = {};
    unsigned placed = 0;
    const bool meter = g_gm_on.load(std::memory_order_relaxed);
    const double now_ms = QpcMs();
    // the tick: the body-update records (fix bit 2)
    static thread_local GearHistSet xh;
    const bool xtrap = kind == kPlaceTick && (g_gear_fix.load(std::memory_order_relaxed) & 2) && now_ms > 0 && g_gear_hist[slot].Read(xh);
    const double frame_ms = g_frame_ms.load(std::memory_order_relaxed);
    std::uint32_t xt = 0, xp = 0;
    // the body's own update: keep the placements (one writer; a colliding forced update skips)
    const bool record = kind == kPlaceBody && now_ms > 0 && !g_gear_hist_busy[slot].exchange(true, std::memory_order_acquire);
    GearHistSet* hw = record ? &g_gear_hist_w[slot] : nullptr;
    // v0.21: the self body moves in its own update, after the tick (follow 4), so the tick's world is the previous
    // update's: the self slot's tick composes the extrapolated locals with the body's world one update ahead too (a prop
    // racing the body's update otherwise drew one frame of motion behind: 30-41 cm driving, run 20261120-204717)
    Xf xworld = world;
    bool xbody = false;
    if (xtrap && slot == 1) {
        double bp[3];
        float br[3][3];
        if (gearx::Matches(xh.body, gen, 1, 0, 0) && gearx::Extrapolate(xh.body, now_ms, frame_ms, bp, br, gearx::kMaxBodyStepM) == gearx::Why::ok) {
            std::memcpy(xworld.p, bp, sizeof(bp));
            std::memcpy(xworld.r, br, sizeof(br));
            xbody = true;
        }
    }
    if (xbody) g_gear_xbody.fetch_add(1, std::memory_order_relaxed);
    if (hw) {
        gearx::Rec br;
        std::memcpy(br.p, world.p, sizeof(br.p));
        std::memcpy(br.r, world.r, sizeof(br.r));
        br.t_ms = now_ms;
        gearx::Push(hw->body, gen, 1, 0, 0, br);
    }
    for (unsigned i = 0; i < set.n; ++i) {
        const GearProp& g = set.p[i];
        Xf anchor;
        if (g.parent >= 0) {
            if (static_cast<unsigned>(g.parent) >= i || !have[g.parent]) continue;
            anchor = w[g.parent];
        } else {
            if (g.joint >= kJ) continue;
            float m[4][4];
            double jp[3];
            float jr[3][3];
            if (!mem::Read(mb + 0x40ull * inv[g.joint], m, sizeof(m)) || !JointFrame(m, jp, jr)) {
                g_gear_bad[slot].fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            anchor = Compose(world, jp, jr);
        }
        const double gp[3] = {g.p[0], g.p[1], g.p[2]};
        w[i] = Compose(anchor, gp, g.r);
        have[i] = true;
        if (!g.e) continue;  // no prop for this item (yet): its frame still anchors the items on it
        std::uintptr_t vt = 0;
        game::Guid id;
        if (!Rd(g.e, vt) || vt != g.e_vt || !Rd(g.e + game::kEntityGuid, id.lo) || !Rd(g.e + game::kEntityGuid + 8, id.hi) ||
            id != g.guid) {
            g_gear_aba[slot].fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        Xf put = w[i];
        if (xtrap) {
            const gearx::Hist& h = xh.h[i];
            double lp[3];
            float lr[3][3];
            if (gearx::Matches(h, gen, g.e, id.lo, id.hi) && gearx::Extrapolate(h, now_ms, frame_ms, lp, lr) == gearx::Why::ok) {
                put = Compose(xworld, lp, lr);
                ++xt;
            } else {
                ++xp;
            }
        }
        PackedWt wt;
        std::memcpy(wt.p, put.p, sizeof(wt.p));
        std::memcpy(wt.r, put.r, sizeof(wt.r));
        if (!CallSetWt(g.e, &wt)) {
            g_gear_bad[slot].fetch_add(1, std::memory_order_relaxed);
            g_gear_n[slot].store(0, std::memory_order_release);  // never again until republished
            if (hw) g_gear_hist_busy[slot].store(false, std::memory_order_release);
            return placed;
        }
        ++placed;
        if (meter) GmRecPlace(slot, i, kind, put.p);
        if (hw) {
            gearx::Rec r;
            gearx::ToLocal(world.p, world.r, put.p, put.r, r.p, r.r);
            r.t_ms = now_ms;
            gearx::Push(hw->h[i], gen, g.e, id.lo, id.hi, r);
        }
    }
    if (hw) {
        g_gear_hist[slot].Write(*hw);
        g_gear_hist_busy[slot].store(false, std::memory_order_release);
        g_gear_body_pl[slot].fetch_add(placed, std::memory_order_relaxed);
    }
    if (xt) g_gear_xtrap[slot].fetch_add(xt, std::memory_order_relaxed);
    if (xp) g_gear_xplain[slot].fetch_add(xp, std::memory_order_relaxed);
    g_gear_placed[slot].fetch_add(placed, std::memory_order_relaxed);
    return placed;
}

bool ReadWorld(std::uintptr_t e, Xf* out) {
    alignas(8) std::uint8_t a[kWorldBytes + 4] = {}, b[kWorldBytes + 4] = {};
    if (!mem::Read(e + kEntWorld, a, kWorldBytes) || !mem::Read(e + kEntWorld, b, kWorldBytes) || std::memcmp(a, b, kWorldBytes) != 0)
        return false;
    PackedWt wt;
    std::memcpy(&wt, a, sizeof(wt));
    if (!std::isfinite(wt.p[0]) || !std::isfinite(wt.p[1]) || !std::isfinite(wt.p[2])) return false;
    std::memcpy(out->p, wt.p, sizeof(out->p));
    std::memcpy(out->r, wt.r, sizeof(out->r));
    return true;
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
    b.pelvis_off_ok = false;
    b.pelvis_off[0] = b.pelvis_off[1] = b.pelvis_off[2] = 0;
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
        // the far hips rule's rest offset (section 38.3): along her own latched hips direction
        const unsigned bp = b.inv[kSamPelvis];
        b.pelvis_off_ok = bp < kJ && pelvis::RestOffset(b.latch[bp].t, r, kSamRestPelvisLen, b.pelvis_off);
    }
    b.latch_state.store(1, std::memory_order_release);
}

// Self view: Sam's world transform (read fault-free, twice for a consistent copy) onto the self
// body, through the game's own exported setter (the body's transform lock and dirty flags). `menu`:
// the cargo menu's figure is the source instead (section 15; no gear props there).
void CopySamTransform(bool menu = false) {
    const std::uintptr_t self_e = g_me.e.load(std::memory_order_seq_cst);
    const std::uintptr_t self_sm = g_me.sm.load(std::memory_order_seq_cst);
    const std::uintptr_t sam = menu ? g_menu_e.load(std::memory_order_acquire) : g_sam_e.load(std::memory_order_acquire);
    if (!self_e || !self_sm || !sam || !g_fn_setwt) return;
    std::atomic<std::uint32_t>& fail_ct = menu ? g_menu_copy_fail : g_self_copy_fail;
    // ABA, as ApplyBody: the game may have freed the body (world streaming, a scripted cleanup)
    // since the tick last saw it. Only an entity that still has the vtable it was validated with
    // and still owns the published model (model vtable + back pointer) gets the game call.
    std::uintptr_t ev = 0, em = 0, smv = 0, sme = 0;
    if (!Rd(self_e, ev) || ev != g_me.e_vt || !Rd(self_e + kEntModel, em) || em != self_sm || !Rd(self_sm, smv) || smv != g_sm_vt ||
        !Rd(self_sm + kSmEntity, sme) || sme != self_e) {
        (menu ? g_menu_copy_aba : g_self_copy_aba).fetch_add(1, std::memory_order_relaxed);
        return;
    }
    alignas(8) std::uint8_t a[kWorldBytes + 4] = {}, b2[kWorldBytes + 4] = {};
    if (!mem::Read(sam + kEntWorld, a, kWorldBytes) || !mem::Read(sam + kEntWorld, b2, kWorldBytes) ||
        std::memcmp(a, b2, kWorldBytes) != 0) {
        fail_ct.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    double p[3];
    std::memcpy(p, a, sizeof(p));
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
        fail_ct.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!CallSetWt(self_e, a)) {
        fail_ct.fetch_add(1, std::memory_order_relaxed);
        g_self_follow.store(3, std::memory_order_relaxed);  // never again from a detour this run
        return;
    }
    if (menu) {
        g_menu_copies.fetch_add(1, std::memory_order_relaxed);
        g_menu_last_copy.store(GetTickCount64(), std::memory_order_relaxed);
        return;
    }
    g_self_copies.fetch_add(1, std::memory_order_relaxed);
    g_self_last_copy.store(GetTickCount64(), std::memory_order_relaxed);
    // Gear props on the self body: this frame's transform, the body's current pose.
    if (g_gear_n[1].load(std::memory_order_acquire)) {
        std::uint8_t hdr[kPoseSize];
        std::uintptr_t pose = 0, local = 0;
        if (ReadPoseHdr(self_sm, hdr, &pose) && CheckHdr(hdr, g_me.skel.load(std::memory_order_acquire), &local) == 0) {
            Xf w;
            std::memcpy(w.p, p, sizeof(w.p));
            std::memcpy(w.r, a + 0x18, sizeof(w.r));
            PlaceGear(1, w, Field<std::uintptr_t>(hdr, kHdrModel), g_me.inv, kPlaceSam);
        }
    }
}

// Self view follow mode 4 (the default since v0.19; docs/real-partner-body.md section 26). The game updates the self body
// before Sam in a frame (section 8.2: `capture age ~1 frame` in every `[smooth]` line), and what it latches for the
// body's drawing in that update uses the world transform the body has then: in mode 1 that is Sam's of the previous frame
// (the copy comes later, in Sam's own update), while everything the game hangs on Sam (his cap, glasses, backpack, cargo,
// hip cases, boots) is drawn at Sam's world of this frame. Moving, all of it ran ahead of her by Sam's step in one frame
// (about 15-20 cm running at the 30 fps cap of two games). Mode 4 keeps the mode 1 copy and, in the body's own update
// before its pose write, puts the body where Sam will be after his update of this frame: his world now plus his last
// per-frame step (the position delta and the rotation delta, applied once). No new game call and no new address: the
// same exported setter as modes 1 and 2, the same identity checks.
struct SamWorldRec {
    std::int64_t qpc = 0;
    float dt = 0;            // the game's dt of that Sam update
    float dtsum = 0;         // the game's dts of Sam's updates in this frame up to this one
    std::uint32_t bseq = 0;  // the self body's update counter at this record (g_body_seq)
    double p[3] = {};
    float r[3][3] = {};
    bool vok = false;   // round 12: his root joint was captured in this update (t_sam_m)
    float rl[3] = {};   // ... its local translation (= its model point: the root has no parent)
    double vr[3] = {};  // his drawn root as vanilla Sam's: his world plus rl in his world's axes
    bool mok = false;   // ... and its model matrix's translation row was read
    float m0[3] = {};
};
struct SamTrack {
    SamWorldRec cur, prev, prev2;  // Sam's world after his last update, and after his last update of the two frames before
    std::uint32_t n = 0;           // records kept (3 = all valid)
};
SeqLock<SamTrack> g_sam_track;
SamTrack g_sam_track_w;  // Sam's update only (under g_fit_busy's sibling below)
std::atomic<bool> g_sam_track_busy{false};
std::atomic<std::uint32_t> g_body_seq{0};  // the self body's own updates in follow mode 4: a Sam update after one starts his next frame
std::atomic<std::uint32_t> g_pred_n{0}, g_pred_after{0}, g_pred_skip_hist{0}, g_pred_skip_stale{0}, g_pred_skip_jump{0}, g_pred_skip_read{0},
    g_pred_fail{0}, g_pred_aba{0}, g_pred_seated{0}, g_pred_skip_gate{0};
std::atomic<std::uint64_t> g_pred_step_um{0}, g_pred_step_max_um{0}, g_pred_pre_um{0}, g_pred_pre_n{0};
std::atomic<std::uint32_t> g_pred_scale_n{0};
std::atomic<std::uint64_t> g_pred_scale_sum{0};  // the span between Sam's two frame records, us
std::atomic<std::uint64_t> g_pred_k_sum{0};      // the step's dt scale x 1000
// Something outside Sam's own update moving him (the ride carry's teleport from the game tick) lands between his last record
// and the body's next update and is already in his world now: adding his last step on top would put the body a step ahead
// of the gear hung on him. Then the plain copy (mode 2). g_pred_skip_moved: Sam's world now differs from his newest record
// by more than selfpred::kMovedM; g_pred_skip_ride: carried as a passenger (ride::Carried, published by SelfTick). A
// vehicle he drives as his parent does NOT move him outside his update (section 26.6: 0.00 cm in every seated second): the
// step applies while seated since v0.20, with the consistency gate for the seat snaps (self_predict.hpp).
constexpr std::uintptr_t kEntParent = 0x70;  // Entity: parent (Sam's vehicle while he drives / rides it in the game)
std::atomic<bool> g_sam_carried{false};      // game thread (SelfTick) -> the body's update
std::atomic<std::uint32_t> g_pred_skip_moved{0}, g_pred_skip_ride{0};
std::atomic<int> g_pred_rule{1};  // `self_view 13 <0|1>`: 0 applies v0.19's rule (seated: copy; no gate, no scale), 1 v0.20's

// The meter (section 26.6): every candidate of a body update scored at the body's next update against Sam's world then (his
// world after his last update of the frame the prediction was for: where the game drew his gear). Moving frames only (Sam's
// world moved more than 5 mm in the frame). Candidates: 0 the v0.20 rule, 1 v0.19's, 2 the plain copy (his previous
// frame's world). Classes: 0 on foot, 1 seated (driving in the game), 2 carried as a passenger. Carried, the carry's
// teleport from the game tick lands between Sam's update and the body's next one (it places him for the NEXT frame, one
// frame ahead along the vehicle's velocity), so there the reference is his world right after his own last update of the
// frame (`own`: where his update put his gear; the copy reads 0.0 cm against it, run 20261120-111415), and the carry's move
// is metered (`moved`).
constexpr int kPmCand = 3, kPmCls = 3;
struct PredMeterCls {
    std::uint32_t n = 0;
    double sum[kPmCand] = {}, mx[kPmCand] = {}, asum[kPmCand] = {}, amx[kPmCand] = {};
    std::uint32_t o3[kPmCand] = {}, o8[kPmCand] = {};
    double own_sum[kPmCand] = {}, own_mx[kPmCand] = {}, moved_sum = 0, moved_mx = 0;
};
struct PredMeter {
    PredMeterCls c[kPmCls];
};
PredMeter g_pm;  // under g_pm_busy
struct PredLast {
    bool have = false;
    std::int64_t qpc = 0;
    int cls = 0;
    Xf cand[kPmCand] = {};
};
PredLast g_pm_last;  // the body's update only (under g_pm_busy)
std::atomic<bool> g_pm_busy{false}, g_pm_drop{false};

// Round 12, the drawn meter (section 38.2): where she is drawn against where vanilla Sam would be drawn. Her drawn root (each
// candidate world plus the root translation of the pose she was given) against Sam's drawn root of the same frame (his world
// after his last update of the frame plus his root joint's translation then), scored at the body's next update. The entity
// meter above scores worlds only; in the private room Sam's camera reactions move his root joint against his world (the
// lunge to the camera, then the world snapped back 2 m and the root joint holding him there), so only this one tells what
// is seen. Per candidate: cm mean / max, and the jitter (the change of that error from one frame to the next: a constant
// offset is not seen, a change is).
struct DrawnRec {
    bool have = false;
    std::int64_t qpc = 0;
    std::uint32_t bseq = 0;
    std::uint8_t why = 0;          // selfpred::Why of the applied rule
    double root[kPmCand][3] = {};  // her drawn root per candidate world
    double world[3] = {};          // her applied world
    float rl[3] = {};              // the root translation of her pose (the capture she was given)
};
struct DrawnMeter {
    std::uint32_t n = 0, jn = 0;  // frames scored, of them with a previous frame (the jitter)
    double sum[kPmCand] = {}, mx[kPmCand] = {}, jsum[kPmCand] = {}, jmx[kPmCand] = {};
    double wstep_mx = 0, rstep_mx = 0, stale_mx = 0;  // Sam's world / drawn root steps; his root model row against his local root
};
DrawnRec g_dm_last;  // under g_dm_busy
DrawnMeter g_dm;     // under g_dm_busy
bool g_dm_prev_ok = false;
double g_dm_prev_e[kPmCand][3] = {};
std::atomic<bool> g_dm_busy{false};
// The trace (`self_view 23 1`): the scored frames around a frame where Sam's world or his root joint moved over 0.5 m (a
// snap, a re-anchor), 8 before and 8 after, logged by the tick as `[self] trace` lines.
struct TraceRow {
    double ms = 0;            // QPC ms of Sam's record
    double sw[3] = {};        // Sam's world
    float srl[3] = {};        // his root joint's local translation
    float sm0[3] = {};        // his root model matrix's translation row (as his animate left it)
    std::uint8_t smok = 0;
    double bw[3] = {};        // her applied world
    float brl[3] = {};        // her pose's root translation
    std::uint8_t why = 0;
    double e[kPmCand] = {};   // her drawn root against his, per candidate (cm)
};
constexpr int kTrPre = 8, kTrPost = 8, kTrRows = kTrPre + 1 + kTrPost;
struct TraceBlock {
    int n = 0;
    int trig = 0;  // the trigger row's index
    TraceRow r[kTrRows];
};
std::atomic<bool> g_tr_on{false};
TraceRow g_tr_ring[kTrPre];  // under g_dm_busy
int g_tr_ring_n = 0, g_tr_ring_i = 0, g_tr_post = -1;
TraceBlock g_tr_build;        // under g_dm_busy
SeqLock<TraceBlock> g_tr_ready;
std::atomic<std::uint32_t> g_tr_seq{0}, g_tr_dropped{0};
// PredictSelfWorld -> ApplyBody in the same detour call: this update's candidate worlds, the applied one and why.
thread_local Xf t_pred_cand[kPmCand];
thread_local bool t_pred_cand_ok = false;
thread_local int t_pred_pick = 0;
thread_local std::uint8_t t_pred_why = 0;

// A world transform (+0xC8 position, +0xE0 rotation) read fault-free twice and compared, finite.
bool ReadWorldTwice(std::uintptr_t e, std::uint8_t (&a)[kWorldBytes + 4]) {
    alignas(8) std::uint8_t b2[kWorldBytes + 4] = {};
    if (!mem::Read(e + kEntWorld, a, kWorldBytes) || !mem::Read(e + kEntWorld, b2, kWorldBytes) || std::memcmp(a, b2, kWorldBytes) != 0) return false;
    double p[3];
    float r[9];
    std::memcpy(p, a, sizeof(p));
    std::memcpy(r, a + 0x18, sizeof(r));
    for (double v : p)
        if (!std::isfinite(v)) return false;
    for (float v : r)
        if (!std::isfinite(v)) return false;
    return true;
}

// Sam's update (after the capture): his world for this frame. Frames are told apart by the self body's update counter: a
// Sam update after a body update starts his next frame (the newest record becomes the previous one); another Sam update
// before the body's next replaces the newest record, and its dt is added to the frame's (v0.19 used "within half a frame
// of the last", which disagreed with the body's updates in a quarter of the frames at an uneven pace: section 26.6).
void TrackSam(float dt) {
    const std::uintptr_t sam = g_sam_e.load(std::memory_order_acquire);
    if (!sam || g_qpc_per_ms <= 0) return;
    alignas(8) std::uint8_t a[kWorldBytes + 4] = {};
    if (!ReadWorldTwice(sam, a)) return;
    if (g_sam_track_busy.exchange(true, std::memory_order_acquire)) return;
    SamWorldRec r;
    r.qpc = Qpc();
    r.dt = dt;
    r.bseq = g_body_seq.load(std::memory_order_acquire);
    std::memcpy(r.p, a, sizeof(r.p));
    std::memcpy(r.r, a + 0x18, sizeof(r.r));
    if (t_sam_m.ok) {
        r.vok = true;
        std::memcpy(r.rl, t_sam_m.rl, sizeof(r.rl));
        for (int k = 0; k < 3; ++k) r.vr[k] = r.p[k] + r.rl[0] * r.r[0][k] + r.rl[1] * r.r[1][k] + r.rl[2] * r.r[2][k];
        r.mok = t_sam_m.m_ok;
        std::memcpy(r.m0, t_sam_m.m0, sizeof(r.m0));
    }
    const float d = std::isfinite(dt) && dt > 0 && dt < 1 ? dt : 0;
    SamTrack& t = g_sam_track_w;
    if (t.n == 0) {
        r.dtsum = d;
        t.cur = r, t.n = 1;
    } else if (r.bseq != t.cur.bseq) {
        r.dtsum = d;
        t.prev2 = t.prev, t.prev = t.cur, t.cur = r, t.n = t.n < 3 ? t.n + 1 : 3;
    } else {
        r.dtsum = t.cur.dtsum + d;
        t.cur = r;  // the same frame's second update: its newest world (the earlier frames' records stay)
    }
    g_sam_track.Write(t);
    g_sam_track_busy.store(false, std::memory_order_release);
}

// The meter's score of the last body update's candidates against Sam's world now (body update, under g_pm_busy).
void PredMeterScore(const PackedWt& now, const SamTrack& t) {
    if (!g_pm_last.have || t.n < 1 || t.cur.qpc <= g_pm_last.qpc) return;  // Sam not updated since: not a frame's end yet
    PredMeterCls& c = g_pm.c[g_pm_last.cls];
    double e[kPmCand], an[kPmCand];
    for (int k = 0; k < kPmCand; ++k) {
        double d2 = 0;
        for (int i = 0; i < 3; ++i) d2 += (g_pm_last.cand[k].p[i] - now.p[i]) * (g_pm_last.cand[k].p[i] - now.p[i]);
        e[k] = std::sqrt(d2) * 100.0;
        an[k] = selfpred::RotAngleDeg(g_pm_last.cand[k].r, now.r);
    }
    g_pm_last.have = false;
    if (!(e[2] > 0.5) || !(e[2] < 300.0)) return;  // not moving, or a teleport / load
    ++c.n;
    double mv2 = 0;
    for (int i = 0; i < 3; ++i) mv2 += (now.p[i] - t.cur.p[i]) * (now.p[i] - t.cur.p[i]);
    const double mv = std::sqrt(mv2) * 100.0;
    c.moved_sum += mv, c.moved_mx = (std::max)(c.moved_mx, mv);
    for (int k = 0; k < kPmCand; ++k) {
        c.sum[k] += e[k], c.asum[k] += an[k];
        if (e[k] > c.mx[k]) c.mx[k] = e[k];
        if (an[k] > c.amx[k]) c.amx[k] = an[k];
        if (e[k] > 3.0) ++c.o3[k];
        if (e[k] > 8.0) ++c.o8[k];
        double d2 = 0;
        for (int i = 0; i < 3; ++i) d2 += (g_pm_last.cand[k].p[i] - t.cur.p[i]) * (g_pm_last.cand[k].p[i] - t.cur.p[i]);
        const double eo = std::sqrt(d2) * 100.0;
        c.own_sum[k] += eo, c.own_mx[k] = (std::max)(c.own_mx[k], eo);
    }
}

// The drawn meter's score of the last body update's record against vanilla Sam's drawn root of that frame (his newest
// record, when it belongs to the record's frame: his update after that body update). Body update, under g_dm_busy.
void DrawnMeterScore(const SamTrack& t) {
    DrawnRec& d = g_dm_last;
    if (!d.have) return;
    d.have = false;
    if (t.n < 1 || !t.cur.vok || t.cur.bseq != d.bseq || t.cur.qpc <= d.qpc) {  // Sam did not update in that frame
        g_dm_prev_ok = false;
        return;
    }
    DrawnMeter& m = g_dm;
    ++m.n;
    double e[kPmCand][3], el[kPmCand];
    for (int k = 0; k < kPmCand; ++k) {
        for (int i = 0; i < 3; ++i) e[k][i] = d.root[k][i] - t.cur.vr[i];
        el[k] = std::sqrt(e[k][0] * e[k][0] + e[k][1] * e[k][1] + e[k][2] * e[k][2]) * 100.0;
        m.sum[k] += el[k], m.mx[k] = (std::max)(m.mx[k], el[k]);
    }
    if (g_dm_prev_ok) {
        ++m.jn;
        for (int k = 0; k < kPmCand; ++k) {
            double j2 = 0;
            for (int i = 0; i < 3; ++i) j2 += (e[k][i] - g_dm_prev_e[k][i]) * (e[k][i] - g_dm_prev_e[k][i]);
            const double j = std::sqrt(j2) * 100.0;
            m.jsum[k] += j, m.jmx[k] = (std::max)(m.jmx[k], j);
        }
    }
    std::memcpy(g_dm_prev_e, e, sizeof(e));
    g_dm_prev_ok = true;
    if (t.cur.mok) {
        double s2 = 0;
        for (int i = 0; i < 3; ++i) s2 += (static_cast<double>(t.cur.m0[i]) - t.cur.rl[i]) * (static_cast<double>(t.cur.m0[i]) - t.cur.rl[i]);
        m.stale_mx = (std::max)(m.stale_mx, std::sqrt(s2) * 100.0);
    }
    if (t.n >= 2 && t.prev.vok) {
        double w2 = 0, v2 = 0;
        for (int i = 0; i < 3; ++i) {
            w2 += (t.cur.p[i] - t.prev.p[i]) * (t.cur.p[i] - t.prev.p[i]);
            v2 += (t.cur.vr[i] - t.prev.vr[i]) * (t.cur.vr[i] - t.prev.vr[i]);
        }
        const double w = std::sqrt(w2), v = std::sqrt(v2);
        m.wstep_mx = (std::max)(m.wstep_mx, w * 100.0), m.rstep_mx = (std::max)(m.rstep_mx, v * 100.0);
    }
    if (!g_tr_on.load(std::memory_order_relaxed)) return;
    // the trace: this frame's row; a trigger when Sam's world or his root joint moved over 0.5 m since the previous row
    TraceRow row;
    row.ms = g_qpc_per_ms > 0 ? static_cast<double>(t.cur.qpc) / g_qpc_per_ms : 0;
    std::memcpy(row.sw, t.cur.p, sizeof(row.sw));
    std::memcpy(row.srl, t.cur.rl, sizeof(row.srl));
    std::memcpy(row.sm0, t.cur.m0, sizeof(row.sm0));
    row.smok = t.cur.mok ? 1 : 0;
    std::memcpy(row.bw, d.world, sizeof(row.bw));
    std::memcpy(row.brl, d.rl, sizeof(row.brl));
    row.why = d.why;
    for (int k = 0; k < kPmCand; ++k) row.e[k] = el[k];
    bool trig = false;
    if (g_tr_ring_n > 0) {
        const TraceRow& pr = g_tr_ring[(g_tr_ring_i + kTrPre - 1) % kTrPre];
        double w2 = 0, r2 = 0;
        for (int i = 0; i < 3; ++i) w2 += (row.sw[i] - pr.sw[i]) * (row.sw[i] - pr.sw[i]), r2 += (row.srl[i] - pr.srl[i]) * (row.srl[i] - pr.srl[i]);
        trig = w2 > 0.25 || r2 > 0.25;
    }
    if (g_tr_post >= 0) {  // collecting the rows after a trigger
        g_tr_build.r[g_tr_build.n++] = row;
        if (--g_tr_post == 0 || g_tr_build.n >= kTrRows) {
            if (g_tr_ready.Write(g_tr_build)) g_tr_seq.fetch_add(1, std::memory_order_release);
            else g_tr_dropped.fetch_add(1, std::memory_order_relaxed);
            g_tr_post = -1;
        }
    } else if (trig) {
        g_tr_build = TraceBlock{};
        const int pre = g_tr_ring_n < kTrPre ? g_tr_ring_n : kTrPre;
        for (int i = 0; i < pre; ++i) g_tr_build.r[g_tr_build.n++] = g_tr_ring[(g_tr_ring_i + kTrPre - pre + i) % kTrPre];
        g_tr_build.trig = g_tr_build.n;
        g_tr_build.r[g_tr_build.n++] = row;
        g_tr_post = kTrPost;
    }
    g_tr_ring[g_tr_ring_i] = row;
    g_tr_ring_i = (g_tr_ring_i + 1) % kTrPre;
    if (g_tr_ring_n < kTrPre) ++g_tr_ring_n;
}

// Her drawn root per candidate world (the self body's live update, right after its pose write). `rl`: the root translation
// of the pose just written (the capture's: ComposeLive copies it as is).
void DrawnRecord(const float* rl) {
    if (!t_pred_cand_ok) return;
    t_pred_cand_ok = false;
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(rl[k])) return;
    DrawnRec r;
    r.have = true;
    r.qpc = Qpc();
    r.bseq = g_body_seq.load(std::memory_order_acquire);
    r.why = t_pred_why;
    std::memcpy(r.rl, rl, sizeof(r.rl));
    for (int c = 0; c < kPmCand; ++c) {
        const Xf& w = t_pred_cand[c];
        for (int k = 0; k < 3; ++k) r.root[c][k] = w.p[k] + rl[0] * w.r[0][k] + rl[1] * w.r[1][k] + rl[2] * w.r[2][k];
    }
    std::memcpy(r.world, t_pred_cand[t_pred_pick].p, sizeof(r.world));
    if (!g_dm_busy.exchange(true, std::memory_order_acquire)) {
        g_dm_last = r;
        g_dm_busy.store(false, std::memory_order_release);
    }
}

// The body's own update, before its pose write (follow mode 4): this frame's world for the body. Sam already updated in
// this frame (his newest record is under half a frame old): his world now is this frame's, copied as is. Otherwise his
// world now plus his last per-frame step, scaled by this frame's dt against his last frame's (selfpred::Decide: no step
// without a previous frame, with records over 2.5 frames old, after a jump over 3 m, when something outside his update
// moved him, while carried, or when his newest step is not consistent with the one before: a seat snap); then the plain
// copy (mode 2), which mode 1's copy in his update corrects anyway. `dt_body`: the game's dt of this body update.
void PredictSelfWorld(float dt_body) {
    t_pred_cand_ok = false;
    const std::uintptr_t self_e = g_me.e.load(std::memory_order_seq_cst);
    const std::uintptr_t self_sm = g_me.sm.load(std::memory_order_seq_cst);
    const std::uintptr_t sam = g_sam_e.load(std::memory_order_acquire);
    if (!self_e || !self_sm || !sam || !g_fn_setwt || g_qpc_per_ms <= 0) return;
    std::uintptr_t ev = 0, em = 0, smv = 0, sme = 0;
    if (!Rd(self_e, ev) || ev != g_me.e_vt || !Rd(self_e + kEntModel, em) || em != self_sm || !Rd(self_sm, smv) || smv != g_sm_vt ||
        !Rd(self_sm + kSmEntity, sme) || sme != self_e) {
        g_pred_aba.fetch_add(1, std::memory_order_relaxed);
        g_pm_drop.store(true, std::memory_order_relaxed);
        return;
    }
    alignas(8) std::uint8_t a[kWorldBytes + 4] = {};
    if (!ReadWorldTwice(sam, a)) {
        g_pred_skip_read.fetch_add(1, std::memory_order_relaxed);
        g_pm_drop.store(true, std::memory_order_relaxed);
        return;
    }
    const std::int64_t qnow = Qpc();
    PackedWt now;
    std::memcpy(&now, a, kWorldBytes);
    SamTrack t;
    if (!g_sam_track.Read(t)) t.n = 0;
    std::uintptr_t parent = 0;
    selfpred::In in;
    in.carried = g_sam_carried.load(std::memory_order_relaxed);
    in.seated = !Rd(sam + kEntParent, parent) || parent != 0;
    in.records = static_cast<int>(t.n);
    in.frame_ms = g_frame_ms.load(std::memory_order_relaxed);
    in.dt_body = std::isfinite(dt_body) && dt_body > 0 && dt_body < 1 ? dt_body : 0;
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (t.n >= 1) {
        in.age_ms = static_cast<double>(qnow - t.cur.qpc) / g_qpc_per_ms;
        double d2 = 0;
        for (int k = 0; k < 3; ++k) d2 += (now.p[k] - t.cur.p[k]) * (now.p[k] - t.cur.p[k]);
        in.moved_m = std::sqrt(d2);
    }
    if (t.n >= 2) {
        in.span_ms = static_cast<double>(t.cur.qpc - t.prev.qpc) / g_qpc_per_ms;
        for (int k = 0; k < 3; ++k) in.dp[k] = t.cur.p[k] - t.prev.p[k];
        in.dt_sam = t.cur.dtsum;
        selfpred::RotStep(t.prev.r, t.cur.r, m);
    }
    if (t.n >= 3) {
        in.span2_ms = static_cast<double>(t.prev.qpc - t.prev2.qpc) / g_qpc_per_ms;
        for (int k = 0; k < 3; ++k) in.dp_prev[k] = t.prev.p[k] - t.prev2.p[k];
    }
    const selfpred::Out o_new = selfpred::Decide(in), o_old = selfpred::Decide(in, true);
    Xf cand[kPmCand];
    for (Xf& x : cand) std::memcpy(x.p, now.p, sizeof(x.p)), std::memcpy(x.r, now.r, sizeof(x.r));
    if (o_new.why == selfpred::Why::step) selfpred::Apply(now.p, now.r, in.dp, m, o_new.k, cand[0].p, cand[0].r);
    if (o_old.why == selfpred::Why::step) selfpred::Apply(now.p, now.r, in.dp, m, o_old.k, cand[1].p, cand[1].r);
    const bool rule_new = g_pred_rule.load(std::memory_order_relaxed) != 0;
    const int pick_i = rule_new ? 0 : 1;
    const selfpred::Out& o = rule_new ? o_new : o_old;
    const Xf& pick = cand[pick_i];
    PackedWt w = now;
    std::memcpy(w.p, pick.p, sizeof(w.p));
    std::memcpy(w.r, pick.r, sizeof(w.r));
    if (!CallSetWt(self_e, &w)) {
        g_pred_fail.fetch_add(1, std::memory_order_relaxed);
        g_self_follow.store(3, std::memory_order_relaxed);  // never again from a detour this run
        return;
    }
    switch (o.why) {
        case selfpred::Why::step: {
            const double len = selfpred::Len(in.dp);
            const auto um = static_cast<std::uint64_t>(len * 1e6);
            g_pred_n.fetch_add(1, std::memory_order_relaxed);
            if (in.seated) g_pred_seated.fetch_add(1, std::memory_order_relaxed);
            g_pred_step_um.fetch_add(um, std::memory_order_relaxed);
            std::uint64_t mx = g_pred_step_max_um.load(std::memory_order_relaxed);
            while (um > mx && !g_pred_step_max_um.compare_exchange_weak(mx, um, std::memory_order_relaxed)) {
            }
            g_pred_scale_sum.fetch_add(static_cast<std::uint64_t>(in.span_ms * 1000.0), std::memory_order_relaxed);
            g_pred_k_sum.fetch_add(static_cast<std::uint64_t>(o.k * 1000.0), std::memory_order_relaxed);
            g_pred_scale_n.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case selfpred::Why::after: g_pred_after.fetch_add(1, std::memory_order_relaxed); break;
        case selfpred::Why::ride: g_pred_skip_ride.fetch_add(1, std::memory_order_relaxed); break;
        case selfpred::Why::no_history: g_pred_skip_hist.fetch_add(1, std::memory_order_relaxed); break;
        case selfpred::Why::stale: g_pred_skip_stale.fetch_add(1, std::memory_order_relaxed); break;
        case selfpred::Why::moved: g_pred_skip_moved.fetch_add(1, std::memory_order_relaxed); break;
        case selfpred::Why::jump: g_pred_skip_jump.fetch_add(1, std::memory_order_relaxed); break;
        case selfpred::Why::gate: g_pred_skip_gate.fetch_add(1, std::memory_order_relaxed); break;
    }
    // Sam's world now against his newest record (he should not have moved since his update): the premise's meter
    if (o.why == selfpred::Why::step || o.why == selfpred::Why::moved || o.why == selfpred::Why::jump || o.why == selfpred::Why::gate) {
        g_pred_pre_um.fetch_add(static_cast<std::uint64_t>(in.moved_m * 1e6), std::memory_order_relaxed);
        g_pred_pre_n.fetch_add(1, std::memory_order_relaxed);
    }
    // the meter (never blocks the body: a busy logger drops this frame)
    if (!g_pm_busy.exchange(true, std::memory_order_acquire)) {
        if (g_pm_drop.exchange(false, std::memory_order_relaxed)) g_pm_last.have = false;
        PredMeterScore(now, t);
        g_pm_last.have = true;
        g_pm_last.qpc = qnow;
        g_pm_last.cls = in.carried ? 2 : in.seated ? 1 : 0;
        for (int k = 0; k < kPmCand; ++k) g_pm_last.cand[k] = cand[k];
        g_pm_busy.store(false, std::memory_order_release);
    } else {
        g_pm_drop.store(true, std::memory_order_relaxed);
    }
    // the drawn meter (section 38.2): the last record scored, this update's candidates handed to the pose write
    if (!g_dm_busy.exchange(true, std::memory_order_acquire)) {
        DrawnMeterScore(t);
        g_dm_busy.store(false, std::memory_order_release);
    }
    for (int k = 0; k < kPmCand; ++k) t_pred_cand[k] = cand[k];
    t_pred_pick = pick_i;
    t_pred_why = static_cast<std::uint8_t>(o.why);
    t_pred_cand_ok = true;
}

// ---- Sam's head items on her drawn head (docs/real-partner-body.md section 26.8, v0.22). The self view draws Sam's cap
// and glasses on HIS head helper (HLP_Head), and they update after Sam: they are drawn at Sam's world of this frame. Her
// body is drawn at the world it had in its own update, earlier in the frame (follow 4's prediction): a stop, a turn or a
// fast slide leaves the two apart by the prediction's error (the user's v0.21 log: 2.0 cm mean, single frames of 13 cm
// and up to 71 cm). Each head item's mover takes a full local matrix (+0x60 rows x, y, z, +0x90 the translation; the
// mover's update multiplies it by the helper's world, 0x14195DF70, every row of the product kept), so Sam's own update,
// once his world is final, writes L = O * Gb * Gs^-1 into each: O the item's offset (helper frame), Gb her HLP_Head as
// her body drew it this frame (its world in its update, its joint 31 just posed, the helper's axes), Gs Sam's HLP_Head
// now. The item's world is then L * Gs = O * Gb: exactly on her drawn head. No new game address, call or hook: the
// mover fields and the site 0x1423A7773 are byte-checked by partner_gear (section 20.2); the writes go only into
// movers partner_gear published, each checked (vtables, back pointers, flag 0) before the write.
constexpr float kHlp[3][3] = {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}};  // HLP_Head's axes in joint 31's frame (section 20.1)
constexpr std::uintptr_t kHmEntMover = 0xB0, kHmMoverEntity = 0x48, kHmMoverLocal = 0x60, kHmMoverFlag = 0xA0;
struct HeadMoverSet {
    std::uint32_t n = 0, gen = 0;  // gen: bumped by every publish (Sam's update forgets what it wrote before)
    HeadMoverPub m[kMaxHeadMovers];
};
SeqLock<HeadMoverSet> g_hms;               // partner_gear's published movers (game thread writes)
std::atomic<std::uint32_t> g_hms_n{0};     // their count (0: nothing to do in Sam's update)
struct HeadRec {
    std::int64_t qpc = 0;
    Xf g{};  // her HLP_Head in world space as her body's update drew it
};
SeqLock<HeadRec> g_head_rec;               // the self body's live update -> Sam's update and the meter
std::atomic<int> g_hcorr_on{1};            // `self_view 16 <0/1>`: the correction (0: v0.21, the plain offset)
std::atomic<bool> g_hc_busy{false};
struct HeadWritten {
    std::uintptr_t mover = 0;
    float m[4][4] = {};
};
HeadWritten g_hc_last[kMaxHeadMovers];     // Sam's update only (under g_hc_busy): what each mover was given last
std::atomic<std::uint32_t> g_hc_corr{0}, g_hc_plain{0}, g_hc_stale{0}, g_hc_big{0}, g_hc_aba{0}, g_hc_wpm{0}, g_hc_skip{0}, g_hc_recs{0};
std::atomic<std::uint64_t> g_hc_um_sum{0}, g_hc_um_max{0}, g_hc_um_n{0};  // the correction's size (micrometres) per corrected update

// Her drawn HLP_Head (the self body's live update, after its pose write): its world now (follow 4's prediction) and joint
// 31's model matrix just written.
void HeadRecord(BodySlot& b, std::uintptr_t owner, std::uintptr_t mb) {
    Xf w;
    float m[4][4];
    double jp[3];
    float jr[3][3];
    const unsigned bj = b.inv[kSamHeadJ];
    if (!mb || bj >= kJ || !ReadWorld(owner, &w) || !mem::Read(mb + 0x40ull * bj, m, sizeof(m)) || !JointFrame(m, jp, jr)) return;
    const double zero[3] = {0, 0, 0};
    HeadRec r;
    r.qpc = Qpc();
    r.g = Compose(Compose(w, jp, jr), zero, kHlp);
    if (g_head_rec.Write(r)) g_hc_recs.fetch_add(1, std::memory_order_relaxed);
}

// L = O * Gb * Gs^-1 (rows; O = identity rotation with translation off): R = Rb Rs^T, t = (off Rb + pb - ps) Rs^T.
void HeadLocal(const float (&off)[3], const Xf* gb, const Xf* gs, float (&m)[4][4]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[r][c] = r == c ? 1.0f : 0.0f;
    if (!gb || !gs) {
        m[3][0] = off[0], m[3][1] = off[1], m[3][2] = off[2];
        return;
    }
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c)
            m[i][c] = gb->r[i][0] * gs->r[c][0] + gb->r[i][1] * gs->r[c][1] + gb->r[i][2] * gs->r[c][2];
    double v[3];
    for (int c = 0; c < 3; ++c) v[c] = off[0] * gb->r[0][c] + off[1] * gb->r[1][c] + off[2] * gb->r[2][c] + (gb->p[c] - gs->p[c]);
    for (int c = 0; c < 3; ++c) m[3][c] = static_cast<float>(v[0] * gs->r[c][0] + v[1] * gs->r[c][1] + v[2] * gs->r[c][2]);
}

// Sam's update, after the capture, the fit and the transform copy: his world and his fitted joint 31 are what his head
// items will hang on this frame. Each published mover gets the corrected matrix (her body updated earlier in this frame:
// a record under 0.75 of a frame old), else the plain offset (no record of this frame, the correction off, the cargo
// menu's figure as the source, a jump over 1.5 m or 45 degrees: a seat snap, a teleport).
void HeadCorrect(std::uintptr_t sam_sm) {
    if (!g_hms_n.load(std::memory_order_acquire)) return;
    static thread_local HeadMoverSet set;
    if (!g_hms.Read(set) || set.n == 0 || set.n > kMaxHeadMovers) return;
    if (g_hc_busy.exchange(true, std::memory_order_acquire)) {
        g_hc_skip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static std::uint32_t s_gen = 0;
    if (set.gen != s_gen) {
        s_gen = set.gen;
        for (HeadWritten& h : g_hc_last) h = HeadWritten{};
    }
    Xf gb, gs;
    bool corr = false;
    if (g_hcorr_on.load(std::memory_order_relaxed) && g_self_src.load(std::memory_order_relaxed) == 0 && g_qpc_per_ms > 0) {
        HeadRec r;
        const double fr = g_frame_ms.load(std::memory_order_relaxed);
        const double age = g_head_rec.Read(r) ? static_cast<double>(Qpc() - r.qpc) / g_qpc_per_ms : 1e9;
        const std::uintptr_t sam = g_sam_e.load(std::memory_order_acquire);
        std::uint8_t hdr[kPoseSize];
        std::uintptr_t pose = 0, local = 0;
        alignas(8) std::uint8_t a[kWorldBytes + 4] = {};
        float m[4][4];
        double jp[3];
        float jr[3][3];
        if (!(age >= 0 && age < 0.75 * (fr > 1 ? fr : 16.7))) {
            g_hc_stale.fetch_add(1, std::memory_order_relaxed);
        } else if (sam && ReadPoseHdr(sam_sm, hdr, &pose) && CheckHdr(hdr, g_sam_skel.load(std::memory_order_acquire), &local) == 0 &&
                   ReadWorldTwice(sam, a) && mem::Read(Field<std::uintptr_t>(hdr, kHdrModel) + 0x40ull * kSamHeadJ, m, sizeof(m)) && JointFrame(m, jp, jr)) {
            PackedWt ws;
            std::memcpy(&ws, a, kWorldBytes);
            Xf w;
            std::memcpy(w.p, ws.p, sizeof(w.p));
            std::memcpy(w.r, ws.r, sizeof(w.r));
            const double zero[3] = {0, 0, 0};
            gs = Compose(Compose(w, jp, jr), zero, kHlp);
            gb = r.g;
            double d2 = 0;
            for (int k = 0; k < 3; ++k) d2 += (gb.p[k] - gs.p[k]) * (gb.p[k] - gs.p[k]);
            const double d = std::sqrt(d2);
            if (std::isfinite(d) && d < 1.5 && selfpred::RotAngleDeg(gb.r, gs.r) < 45.0) {
                corr = true;
                const auto um = static_cast<std::uint64_t>(d * 1e6);
                g_hc_um_sum.fetch_add(um, std::memory_order_relaxed);
                g_hc_um_n.fetch_add(1, std::memory_order_relaxed);
                std::uint64_t mx = g_hc_um_max.load(std::memory_order_relaxed);
                while (um > mx && !g_hc_um_max.compare_exchange_weak(mx, um, std::memory_order_relaxed)) {
                }
            } else {
                g_hc_big.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    (corr ? g_hc_corr : g_hc_plain).fetch_add(1, std::memory_order_relaxed);
    for (unsigned i = 0; i < set.n; ++i) {
        const HeadMoverPub& p = set.m[i];
        float m[4][4];
        HeadLocal(p.off, corr ? &gb : nullptr, corr ? &gs : nullptr, m);
        bool finite = true;
        for (auto& row : m)
            for (float f : row) finite = finite && std::isfinite(f);
        HeadWritten* last = nullptr;
        for (HeadWritten& h : g_hc_last)
            if (h.mover == p.mover) last = &h;
        if (!finite || (last && std::memcmp(last->m, m, sizeof(m)) == 0)) continue;
        std::uintptr_t evt = 0, em = 0, mvt = 0, me = 0;
        std::uint8_t flag = 1;
        if (!Rd(p.e, evt) || evt != p.e_vt || !Rd(p.e + kHmEntMover, em) || em != p.mover || !Rd(p.mover, mvt) || mvt != p.mover_vt ||
            !Rd(p.mover + kHmMoverEntity, me) || me != p.e || !Rd(p.mover + kHmMoverFlag, flag) || flag != 0) {
            g_hc_aba.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        SIZE_T wrote = 0;
        if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p.mover + kHmMoverLocal), m, sizeof(m), &wrote) || wrote != sizeof(m)) {
            g_hc_wpm.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!last) {
            for (HeadWritten& h : g_hc_last) {
                bool used = false;
                for (unsigned k = 0; k < set.n && !used; ++k) used = h.mover && h.mover == set.m[k].mover;
                if (!used) {
                    last = &h;
                    break;
                }
            }
        }
        if (last) last->mover = p.mover, std::memcpy(last->m, m, sizeof(m));
    }
    g_hc_busy.store(false, std::memory_order_release);
}

// The partner (or debug) body's pose now (not the self view's live capture): the bracketing frames interpolated at this
// moment's play time, or the published frame; latch translations and scales, the pelvis times the ratio. Pure math on
// the published buffers (the body's own update and the boots prop's, section 21). `stats`: the body's counters.
bool ComposePartner(BodySlot& b, Joint (&cur)[kJ], bool stats) {
    if (&b == &g_slots[0] && g_interp_on.load(std::memory_order_acquire) &&
        (g_smooth.load(std::memory_order_relaxed) & kSmoothDetourInterp) && g_qpc_per_ms > 0) {
        // Interpolated now, at this update's own time: play = the published play time advanced by the
        // QPC time since it was published; never past the newest frame (held, counted).
        static thread_local InterpBuf ib;
        if (!g_interp.Read(ib) || ib.n == 0 || ib.n > 3) {
            if (stats) g_interp_torn.fetch_add(1, std::memory_order_relaxed);
            if (stats) b.ct.torn.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const double play = ib.play_ref + (static_cast<double>(Qpc()) / g_qpc_per_ms - ib.ref_ms);
        unsigned s = 0;
        float alpha = 0;
        if (ib.n >= 2) {
            if (ib.n == 3 && play > ib.key[1]) s = 1;
            const double span = ib.key[s + 1] - ib.key[s];
            const double a = span > 0 ? (play - ib.key[s]) / span : 1.0;
            alpha = a <= 0 ? 0.0f : a >= 1 ? 1.0f : static_cast<float>(a);
        }
        if (stats && play > ib.key[ib.n - 1]) g_interp_holds.fetch_add(1, std::memory_order_relaxed);
        const unsigned s2 = ib.n >= 2 ? s + 1 : s;
        float troot[3], tpel[3];
        for (int k = 0; k < 3; ++k) {
            troot[k] = ib.troot[s][k] + alpha * (ib.troot[s2][k] - ib.troot[s][k]);
            tpel[k] = ib.tpelvis[s][k] + alpha * (ib.tpelvis[s2][k] - ib.tpelvis[s][k]);
        }
        for (unsigned j = 0; j < kJ; ++j) {
            const unsigned sj = b.map[j];
            Joint& o = cur[j];
            const wire::Quat& qa = ib.q[s][sj];
            wire::Quat qb = ib.q[s2][sj];
            if (qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w < 0) qb = wire::Quat{-qb.x, -qb.y, -qb.z, -qb.w};
            float x = qa.x + alpha * (qb.x - qa.x), y = qa.y + alpha * (qb.y - qa.y), z = qa.z + alpha * (qb.z - qa.z),
                  w = qa.w + alpha * (qb.w - qa.w);
            const float n = std::sqrt(x * x + y * y + z * z + w * w);
            if (n > 1e-6f) x /= n, y /= n, z /= n, w /= n;
            else x = qa.x, y = qa.y, z = qa.z, w = qa.w;
            o.q[0] = x, o.q[1] = y, o.q[2] = z, o.q[3] = w;
            const Joint& src = b.latch_ok ? b.latch[j] : ib.fb[sj];
            std::memcpy(o.t, src.t, sizeof(o.t));
            std::memcpy(o.s, src.s, sizeof(o.s));
            if (sj == kSamRoot) {
                o.t[0] = troot[0], o.t[1] = troot[1], o.t[2] = troot[2];
            } else if (sj == kSamPelvis) {
                HipsT(tpel, b.pelvis_ratio, b.pelvis_off, b.pelvis_off_ok, o.t);
            }
        }
        if (stats) g_interp_applies.fetch_add(1, std::memory_order_relaxed);
        if (stats) g_last_play.store(play < ib.key[0] ? ib.key[0] : play > ib.key[ib.n - 1] ? ib.key[ib.n - 1] : play, std::memory_order_relaxed);
    } else {
        ApplyBuf a;
        if (!g_apply.Read(a)) {
            if (stats) b.ct.torn.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (stats && &b == &g_slots[0]) g_last_play.store(a.play, std::memory_order_relaxed);
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
                HipsT(a.tpelvis, b.pelvis_ratio, b.pelvis_off, b.pelvis_off_ok, o.t);
            }
        }
    }
    return true;
}

// The self view's pose from a capture of the local Sam (ApplyBody's live branch; the exo carrier's set 1, section 32):
// Sam's rotations, the body's latched translations and scales, the pelvis times the ratio.
void ComposeLive(const BodySlot& b, const Capture& c, Joint (&cur)[kJ]) {
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
            HipsT(c.j[kSamPelvis].t, b.pelvis_ratio, b.pelvis_off, b.pelvis_off_ok, o.t);
            const float* h = c.j[kSamPelvis].t;
            if (h[0] * h[0] + h[1] * h[1] + h[2] * h[2] > pelvis::kNearM * pelvis::kNearM) g_far_hips_n.fetch_add(1, std::memory_order_relaxed);
        }
    }
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
    const bool from_menu = live && g_self_src.load(std::memory_order_acquire) == 1;
    static thread_local Capture c;  // live: the capture written (the fit's same pose, section 16.1)
    if (live) {
        if (!(from_menu ? g_menu_capture : g_capture).Read(c)) {
            ct.nocap.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ComposeLive(b, c, cur);
        // How old the pose is when it lands on the body (same frame: well under a frame time).
        if (g_qpc_per_ms > 0) {
            const double age_ms = static_cast<double>(Qpc() - c.qpc) / g_qpc_per_ms;
            const std::uint64_t us = age_ms > 0 ? static_cast<std::uint64_t>(age_ms * 1000.0) : 0;
            g_self_age_us.fetch_add(us, std::memory_order_relaxed);
            g_self_age_n.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t mx = g_self_age_max_us.load(std::memory_order_relaxed);
            while (us > mx && !g_self_age_max_us.compare_exchange_weak(mx, us, std::memory_order_relaxed)) {
            }
            const double fr = g_frame_ms.load(std::memory_order_relaxed);
            g_self_age_bucket[age_ms < 0.5 * fr ? 0 : age_ms < 1.5 * fr ? 1 : 2].fetch_add(1, std::memory_order_relaxed);
        }
        if (!from_menu && g_self_last_frame.exchange(c.frame, std::memory_order_relaxed) == c.frame)
            g_self_stale.fetch_add(1, std::memory_order_relaxed);
    } else if (!ComposePartner(b, cur, true)) {
        return;
    }
    // Her feet scaled away under boots drawn over them (section 21): the self view under Sam's equipped boots (on
    // his fitted skeleton, Sam's own update), the partner under our boots prop. Only the body's pose: never the latch,
    // the fit's shape, the capture or the wire. The scale reaches the toes through the model matrices.
    int feet_pct = 100;
    if (live && !from_menu) {
        if (g_feet_cfg.load(std::memory_order_relaxed) && g_feet_boots.load(std::memory_order_relaxed) && g_fit_want.load(std::memory_order_relaxed))
            feet_pct = g_feet_pct.load(std::memory_order_relaxed);
    } else if (!live && &b == &g_slots[0] && g_pfeet_on.load(std::memory_order_acquire)) {
        feet_pct = g_pfeet_pct.load(std::memory_order_relaxed);
    }
    if (feet_pct >= 0 && feet_pct < 100) {
        const float k = feet_pct < 1 ? 0.01f : static_cast<float>(feet_pct) / 100.0f;
        for (unsigned j = 0; j < kJ; ++j)
            if (b.map[j] == kSamFootR || b.map[j] == kSamFootL) cur[j].s[0] = cur[j].s[1] = cur[j].s[2] = k;
        (live ? g_feet_writes : g_pfeet_writes).fetch_add(1, std::memory_order_relaxed);
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
    if (live) (from_menu ? g_menu_last_apply : g_self_last_apply).store(GetTickCount64(), std::memory_order_relaxed);
    if (live && !from_menu) g_self_applied.Write(c);  // the fit hangs Sam's gear on this same pose
    // her drawn head for Sam's head items (section 26.8): only the game's own updates (a forced one from the tick is not
    // what this frame draws)
    if (live && !from_menu && !t_forcing && g_hms_n.load(std::memory_order_relaxed)) HeadRecord(b, owner, Field<std::uintptr_t>(hdr, kHdrModel));
    if (live && !from_menu && !t_forcing) DrawnRecord(c.j[kSamRoot].t);  // the drawn meter (section 38.2)
    const bool meter = g_gm_on.load(std::memory_order_relaxed) && !from_menu;
    // Gear props on the partner (or debug) body: its world transform now, the pose just written.
    if (!live && (g_gear_n[0].load(std::memory_order_acquire) || meter)) {
        Xf w;
        if (ReadWorld(owner, &w)) {
            if (meter) GmRecBody(0, w);
            PlaceGear(0, w, Field<std::uintptr_t>(hdr, kHdrModel), b.inv, kPlaceBody);
        }
    }
    // The self view's props (the BB pod) update after the self body and before Sam (section 26.7): only a placement made
    // here, on the world the body has now (follow 4's prediction) and the pose just written, is what they draw with it.
    // CopySamTransform's and the tick's placements stay (they come after the props' update in a frame).
    if (live && !from_menu && (meter || ((g_gear_fix.load(std::memory_order_relaxed) & 1) && g_gear_n[1].load(std::memory_order_acquire)))) {
        Xf w;
        if (ReadWorld(owner, &w)) {
            if (meter) GmRecBody(1, w);
            if (g_gear_fix.load(std::memory_order_relaxed) & 1) PlaceGear(1, w, Field<std::uintptr_t>(hdr, kHdrModel), b.inv, kPlaceBody);
        }
    }
}

// A gear prop's pose (target k): the published joints' rotations and translations over its finished
// pose (scale kept), then the game's model-matrix and bounds steps, as ApplyBody does for a body.
void ApplyGearPose(unsigned k, std::uintptr_t sm) {
    PoseTarget& t = g_pt[k];
    if (g_pt_faulted.load(std::memory_order_relaxed)) return;
    // each published value once: the size checks and the write below use the same joint count
    const std::uintptr_t te = t.e.load(std::memory_order_acquire), tvt = t.e_vt.load(std::memory_order_relaxed),
                         tskel = t.skel.load(std::memory_order_relaxed);
    const std::uint32_t tn = t.n.load(std::memory_order_relaxed);
    std::uintptr_t id[2] = {};
    std::uintptr_t owner = 0, evt = 0;
    if (!te || !mem::Read(sm, id, sizeof(id)) || id[0] != g_sm_vt || !Rd(sm + kSmEntity, owner) || owner != te || !Rd(owner, evt) || evt != tvt) {
        g_pt_rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0;
    if (!ReadPoseHdr(sm, hdr, &pose) || Field<std::uintptr_t>(hdr, kHdrSkel) != tskel || Field<std::int32_t>(hdr, kHdrState) != kPoseStateModel) {
        g_pt_rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uintptr_t lb = Field<std::uintptr_t>(hdr, kHdrLocal), le = Field<std::uintptr_t>(hdr, kHdrLocalEnd);
    const std::uintptr_t mb = Field<std::uintptr_t>(hdr, kHdrModel), me = Field<std::uintptr_t>(hdr, kHdrModelEnd);
    if (!lb || tn == 0 || tn > kMaxGearPoseJ || le - lb != tn * sizeof(Joint) || !mb || me - mb != tn * 0x40ull) {
        g_pt_rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static thread_local GearPoseBuf b;
    if (!g_pt_buf[k].Read(b) || b.n != tn) {
        g_pt_torn.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Joint cur[kMaxGearPoseJ];
    if (!mem::Read(lb, cur, tn * sizeof(Joint))) return;
    if (b.n2 == tn && g_qpc_per_ms > 0 && b.key_b > b.key_a) {
        // interpolated at this update's own time: the published play time advanced by the QPC time since it was
        // published, clamped to [key_a, key_b] (held at key_b: counted)
        const double play = b.play_ref + (static_cast<double>(Qpc()) / g_qpc_per_ms - b.ref_ms);
        // the segment holding the play time: a..b, or b..c once past b (when the frame after b was published)
        const bool second = play > b.key_b && b.n3 == tn && b.key_c > b.key_b;
        const double a = second ? (play - b.key_b) / (b.key_c - b.key_b) : (play - b.key_a) / (b.key_b - b.key_a);
        const float u = a <= 0 ? 0.0f : a >= 1 ? 1.0f : static_cast<float>(a);
        if (k == kVehiclePoseTarget) {
            g_vm_interp.fetch_add(1, std::memory_order_relaxed);
            if (a > 1) g_vm_holds.fetch_add(1, std::memory_order_relaxed);
        }
        const float(*sq0)[4] = second ? b.q2 : b.q;
        const float(*sq1)[4] = second ? b.q3 : b.q2;
        const float(*st0)[3] = second ? b.t2 : b.t;
        const float(*st1)[3] = second ? b.t3 : b.t2;
        for (unsigned j = 0; j < tn; ++j) {
            const float* qa = sq0[j];
            float qb[4] = {sq1[j][0], sq1[j][1], sq1[j][2], sq1[j][3]};
            if (qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3] < 0)
                for (float& c : qb) c = -c;
            float q[4];
            for (int c = 0; c < 4; ++c) q[c] = qa[c] + u * (qb[c] - qa[c]);
            const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (n > 1e-6f) {
                for (float& c : q) c /= n;
            } else {
                std::memcpy(q, qa, sizeof(q));
            }
            std::memcpy(cur[j].q, q, sizeof(cur[j].q));
            for (int c = 0; c < 3; ++c) cur[j].t[c] = st0[j][c] + u * (st1[j][c] - st0[j][c]);
        }
    } else {
        for (unsigned j = 0; j < tn; ++j) {
            std::memcpy(cur[j].q, b.q[j], sizeof(cur[j].q));
            cur[j].t[0] = b.t[j][0], cur[j].t[1] = b.t[j][1], cur[j].t[2] = b.t[j][2];
        }
    }
    if (k == kVehiclePoseTarget) {
        // the meter: the largest joint turn since the previous write (a new prop or a reset starts over)
        if (g_vm_reset.exchange(false, std::memory_order_acq_rel)) g_vm_prev_n = 0;
        if (g_vm_prev_n == tn && g_vm_gate.load(std::memory_order_relaxed)) {
            float step = 0;
            for (unsigned j = 0; j < tn; ++j) {
                const float* p = g_vm_prev[j];
                float d = std::fabs(p[0] * cur[j].q[0] + p[1] * cur[j].q[1] + p[2] * cur[j].q[2] + p[3] * cur[j].q[3]);
                if (d > 1.0f) d = 1.0f;
                const float deg = 2.0f * std::acos(d) * 57.29578f;
                if (deg > step) step = deg;
            }
            const auto md = static_cast<std::uint32_t>(step * 1000.0f);
            g_vm_writes.fetch_add(1, std::memory_order_relaxed);
            if (md < 10) g_vm_repeats.fetch_add(1, std::memory_order_relaxed);  // under 0.01 deg: the same pose again
            g_vm_step_sum.fetch_add(md, std::memory_order_relaxed);
            g_vm_step_sq.fetch_add(static_cast<std::uint64_t>(md) * md / 1000u, std::memory_order_relaxed);
            std::uint32_t mx = g_vm_step_max.load(std::memory_order_relaxed);
            while (md > mx && !g_vm_step_max.compare_exchange_weak(mx, md, std::memory_order_relaxed)) {
            }
        }
        for (unsigned j = 0; j < tn; ++j) std::memcpy(g_vm_prev[j], cur[j].q, sizeof(g_vm_prev[j]));
        g_vm_prev_n = tn;
    }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(lb), cur, tn * sizeof(Joint), &wrote) || wrote != tn * sizeof(Joint)) {
        g_pt_wpm.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::int32_t bounds_on = 0;
    Rd(sm + kSmBoundsOn, bounds_on);
    if (!CallPoseFns(sm, pose, bounds_on != 0)) {
        g_pt_faults.fetch_add(1, std::memory_order_relaxed);
        g_pt_faulted.store(true, std::memory_order_seq_cst);  // never again this session (SetGearPose refuses)
        g_pt_live.store(0, std::memory_order_seq_cst);
        return;
    }
    g_pt_applies.fetch_add(1, std::memory_order_relaxed);
}

// The partner's boots prop (docs/real-partner-body.md section 21): ds/entities/cinematics/demo_apv_sam_foot_def_v01, the
// equipped boots' own ArtParts on their own 10-joint skeleton (Root, Hips, R / L 100 Leg, 101 Leg, 102 Foot, 103 Toe;
// layout 0x9B68A8AD), the same parent chain as the bodies' 77 joints. In its own model update it takes the partner
// body's local joints, composed exactly as the body's own update composes them at this moment (ComposePartner), with
// an identity root: placed on the body's root joint frame (a gear item on joint 0, identity pose), its model matrices
// are the body's. That is what the game's DSPlayerBootsComponent does for Sam's own boots (0x1427C4880).
constexpr unsigned kBootsJ = 10;
constexpr std::uint32_t kBootsLayout = 0x9B68A8ADu;
constexpr std::int16_t kBootsParent[kBootsJ] = {-1, 0, 1, 2, 3, 4, 1, 6, 7, 8};
constexpr std::uint8_t kBootsSamJ[kBootsJ] = {0, 1, 61, 62, 63, 64, 66, 67, 68, 69};
struct BootsTarget {
    std::atomic<std::uintptr_t> sm{0};  // published last (release); 0 = off
    std::atomic<std::uintptr_t> e{0}, e_vt{0}, skel{0};
    std::uint8_t bj[kBootsJ] = {};     // boots joint -> the partner body's joint (written while sm is 0)
    holo::Spawn spawn;
    std::atomic<std::uint32_t> game_updates{0}, applies{0}, rejects{0}, torn{0}, wpm{0};
    std::atomic<ULONGLONG> last_apply{0};
    ULONGLONG last_game_ms = 0;
    std::uint32_t forced = 0;
    bool forced_off = false;
} g_bt;
std::atomic<bool> g_bt_faulted{false};

void ApplyBootsPose(std::uintptr_t sm) {
    if (g_bt_faulted.load(std::memory_order_relaxed)) return;
    const std::uintptr_t te = g_bt.e.load(std::memory_order_acquire), tvt = g_bt.e_vt.load(std::memory_order_relaxed),
                         tskel = g_bt.skel.load(std::memory_order_relaxed);
    std::uintptr_t id[2] = {};
    std::uintptr_t owner = 0, evt = 0;
    if (!te || !mem::Read(sm, id, sizeof(id)) || id[0] != g_sm_vt || !Rd(sm + kSmEntity, owner) || owner != te || !Rd(owner, evt) || evt != tvt) {
        g_bt.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0;
    if (!ReadPoseHdr(sm, hdr, &pose) || Field<std::uintptr_t>(hdr, kHdrSkel) != tskel || Field<std::int32_t>(hdr, kHdrState) != kPoseStateModel) {
        g_bt.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uintptr_t lb = Field<std::uintptr_t>(hdr, kHdrLocal), le = Field<std::uintptr_t>(hdr, kHdrLocalEnd);
    const std::uintptr_t mb = Field<std::uintptr_t>(hdr, kHdrModel), me = Field<std::uintptr_t>(hdr, kHdrModelEnd);
    if (!lb || le - lb != kBootsJ * sizeof(Joint) || !mb || me - mb != kBootsJ * 0x40ull) {
        g_bt.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    BodySlot& b = g_p;
    if (b.latch_state.load(std::memory_order_acquire) != 1 || !b.apply_on.load(std::memory_order_acquire) || !b.sm.load(std::memory_order_acquire)) {
        g_bt.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static thread_local Joint body[kJ];
    if (!ComposePartner(b, body, false)) {
        g_bt.torn.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Joint cur[kBootsJ];
    if (!mem::Read(lb, cur, sizeof(cur))) return;
    cur[0].q[0] = cur[0].q[1] = cur[0].q[2] = 0, cur[0].q[3] = 1;
    cur[0].t[0] = cur[0].t[1] = cur[0].t[2] = 0;
    for (unsigned k = 1; k < kBootsJ; ++k) {
        const Joint& s = body[g_bt.bj[k]];
        std::memcpy(cur[k].q, s.q, sizeof(cur[k].q));
        std::memcpy(cur[k].t, s.t, 3 * sizeof(float));
    }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(lb), cur, sizeof(cur), &wrote) || wrote != sizeof(cur)) {
        g_bt.wpm.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::int32_t bounds_on = 0;
    Rd(sm + kSmBoundsOn, bounds_on);
    if (!CallPoseFns(sm, pose, bounds_on != 0)) {
        g_bt_faulted.store(true, std::memory_order_seq_cst);  // never again this session
        g_bt.sm.store(0, std::memory_order_seq_cst);
        return;
    }
    g_bt.applies.fetch_add(1, std::memory_order_relaxed);
    g_bt.last_apply.store(GetTickCount64(), std::memory_order_relaxed);
}

// The exo carrier (docs/real-partner-body.md section 32): a demo_sam_clean body that shows only Sam's skeleton pieces, one
// per slot (0 the partner body, 1 the self body). In its own model update it takes the body's local joints, composed as the
// body's own update composes them (set 0 ComposePartner; set 1 the capture the self body was last written from), with an
// identity root: Sam joint k of the carrier gets the body's joint inv[k]. Placed as a gear item on the body's joint 0
// (identity pose), its model matrices are the body's, as for the boots prop (section 21). No feet scale.
struct ExoTarget {
    std::atomic<std::uintptr_t> sm{0};  // published last (release); 0 = off
    std::atomic<std::uintptr_t> e{0}, e_vt{0}, skel{0};
    std::uint8_t bj[kJ] = {};  // carrier (Sam) joint -> the body's joint (written while sm is 0)
    holo::Spawn spawn;
    std::atomic<std::uint32_t> game_updates{0}, applies{0}, rejects{0}, torn{0}, wpm{0};
    std::uint32_t applies_at_set = 0;  // `applies` when it became the target (ExoPosed wants a few writes since)
    std::atomic<ULONGLONG> last_apply{0};
    ULONGLONG last_game_ms = 0;
    std::uint32_t forced = 0, forced_skipped = 0;
    bool forced_off = false;
    std::atomic<bool> want_pose{false};  // partner_gear: the carrier is meant to be seen (its body shown): forced updates only then
};
ExoTarget g_xt[2];
std::atomic<bool> g_xt_faulted{false};
// The self carrier's source (`gear 27 3 / 4`): 1 (default) Sam's newest capture, what the self body's own update takes next:
// the carrier updates before the self body in a frame (run 20261122-181504, running 9 m: pose 0.00 cm with it, 1-3 cm
// with 0), 0 the capture the self body was last written from (the previous frame's then).
std::atomic<int> g_xt_self_src{1};

// The self body's pose (the same capture the body shows, or the one it takes next).
bool ComposeSelf(Joint (&cur)[kJ]) {
    static thread_local Capture c;
    const bool newest = g_xt_self_src.load(std::memory_order_relaxed) == 1 && g_self_src.load(std::memory_order_acquire) == 0;
    if (!(newest ? g_capture : g_self_applied).Read(c) || c.frame == 0) return false;
    ComposeLive(g_me, c, cur);
    return true;
}

void ApplyExoPose(int x, std::uintptr_t sm) {
    ExoTarget& t = g_xt[x];
    if (g_xt_faulted.load(std::memory_order_relaxed)) return;
    const std::uintptr_t te = t.e.load(std::memory_order_acquire), tvt = t.e_vt.load(std::memory_order_relaxed),
                         tskel = t.skel.load(std::memory_order_relaxed);
    std::uintptr_t id[2] = {};
    std::uintptr_t owner = 0, evt = 0;
    if (!te || !mem::Read(sm, id, sizeof(id)) || id[0] != g_sm_vt || !Rd(sm + kSmEntity, owner) || owner != te || !Rd(owner, evt) || evt != tvt) {
        t.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0;
    if (!ReadPoseHdr(sm, hdr, &pose) || Field<std::uintptr_t>(hdr, kHdrSkel) != tskel || Field<std::int32_t>(hdr, kHdrState) != kPoseStateModel) {
        t.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uintptr_t lb = Field<std::uintptr_t>(hdr, kHdrLocal), le = Field<std::uintptr_t>(hdr, kHdrLocalEnd);
    const std::uintptr_t mb = Field<std::uintptr_t>(hdr, kHdrModel), me = Field<std::uintptr_t>(hdr, kHdrModelEnd);
    if (!lb || le - lb != kJ * sizeof(Joint) || !mb || me - mb != kJ * 0x40ull) {
        t.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    BodySlot& b = g_slots[x];
    if (b.latch_state.load(std::memory_order_acquire) != 1 || !b.apply_on.load(std::memory_order_acquire) || !b.sm.load(std::memory_order_acquire)) {
        t.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static thread_local Joint body[kJ];
    if (!(x == 0 ? ComposePartner(b, body, false) : ComposeSelf(body))) {
        t.torn.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static thread_local Joint cur[kJ];
    if (!mem::Read(lb, cur, sizeof(cur))) return;
    cur[0].q[0] = cur[0].q[1] = cur[0].q[2] = 0, cur[0].q[3] = 1;
    cur[0].t[0] = cur[0].t[1] = cur[0].t[2] = 0;
    for (unsigned k = 1; k < kJ; ++k) {
        const Joint& s = body[t.bj[k]];
        std::memcpy(cur[k].q, s.q, sizeof(cur[k].q));
        std::memcpy(cur[k].t, s.t, 3 * sizeof(float));
        std::memcpy(cur[k].s, s.s, 3 * sizeof(float));
    }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(lb), cur, sizeof(cur), &wrote) || wrote != sizeof(cur)) {
        t.wpm.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::int32_t bounds_on = 0;
    Rd(sm + kSmBoundsOn, bounds_on);
    if (!CallPoseFns(sm, pose, bounds_on != 0)) {
        g_xt_faulted.store(true, std::memory_order_seq_cst);  // never again this session
        g_xt[0].sm.store(0, std::memory_order_seq_cst);
        g_xt[1].sm.store(0, std::memory_order_seq_cst);
        return;
    }
    t.applies.fetch_add(1, std::memory_order_relaxed);
    t.last_apply.store(GetTickCount64(), std::memory_order_relaxed);
}

// ---------------------------------------------------------------- the enemy pose slots (docs/enemy-sync.md 8; P1-0's probe on slot 7)
// Eight watched SkinnedModels (enemies': the same skeleton object as Sam's, 77 joints, layout BE39C83F, checked when set and on
// every call). Per slot: count counts its animate calls; capture copies its finished local joints after each of its own animates
// (the host: the SEP1 frames); apply writes the newest pushed frame onto it after each of its own animates (the guest's mirrors:
// local rotations 1..76 from the frame, its own root, the pelvis translation from the frame); probe_sam writes our Sam's live
// local pose (the P1-0 pose test) and measures the largest joint rotation difference to Sam's model pose. A fault in the pose
// functions turns every slot off for the session.
constexpr unsigned kEnemySlots = 8;
struct EnemyCap {
    double ms = 0;        // QPC ms when captured
    std::uint32_t n = 0;  // captures so far (0: none)
    Joint j[kJ];
};
struct EnemyPush {
    std::uint32_t seq = 0;  // 0: no frame
    wire::Quat q[kJ];
    float tpelvis[3] = {};
    bool have_pelvis = false;
};
struct EnemySlot {
    std::atomic<std::uintptr_t> sm{0};
    std::atomic<int> mode{0};
    std::atomic<std::uint32_t> calls{0}, writes{0}, rejects{0}, faults{0}, captures{0}, applies{0}, no_frame{0};
    std::atomic<std::uint32_t> err_mdeg{0};  // probe_sam: the largest rotation error since the last reset (millidegrees)
    std::atomic<int> busy{0};                // the detour inside this slot now (SetEnemySlot waits on it alone)
    SeqLock<EnemyCap> cap;
    SeqLock<EnemyPush> push;
};
EnemySlot g_es[kEnemySlots];
std::atomic<unsigned> g_et_any{0};  // slots with a model
std::atomic<bool> g_et_faulted{false};

// The enemy model's pose header, checked: our SkinnedModel vtable, the local and model arrays of 77; `model_state` also asks for
// the finished model pose (the writers).
bool EnemyHdr(std::uintptr_t sm, std::uintptr_t* pose, std::uintptr_t* lb, std::uintptr_t* mb, bool model_state) {
    std::uintptr_t vt = 0;
    if (!Rd(sm, vt) || vt != g_sm_vt) return false;
    std::uint8_t hdr[kPoseSize];
    if (!ReadPoseHdr(sm, hdr, pose)) return false;
    if (model_state && Field<std::int32_t>(hdr, kHdrState) != kPoseStateModel) return false;
    const std::uintptr_t l = Field<std::uintptr_t>(hdr, kHdrLocal), le = Field<std::uintptr_t>(hdr, kHdrLocalEnd);
    const std::uintptr_t m = Field<std::uintptr_t>(hdr, kHdrModel), me = Field<std::uintptr_t>(hdr, kHdrModelEnd);
    if (!l || le - l != kJ * sizeof(Joint) || !m || me - m != kJ * 0x40ull) return false;
    *lb = l, *mb = m;
    return true;
}

void EnemyFault(EnemySlot& s) {
    g_et_faulted.store(true, std::memory_order_seq_cst);
    s.faults.fetch_add(1, std::memory_order_relaxed);
    for (EnemySlot& x : g_es)
        if (x.sm.exchange(0, std::memory_order_seq_cst)) g_et_any.fetch_sub(1, std::memory_order_seq_cst);
}

void CaptureEnemyPose(EnemySlot& s, std::uintptr_t sm) {
    std::uintptr_t pose = 0, lb = 0, mb = 0;
    if (!EnemyHdr(sm, &pose, &lb, &mb, false)) {
        s.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static thread_local EnemyCap c;
    if (!mem::Read(lb, c.j, sizeof(c.j))) {
        s.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    c.ms = QpcMs();
    c.n = s.captures.load(std::memory_order_relaxed) + 1;
    if (s.cap.Write(c)) s.captures.fetch_add(1, std::memory_order_relaxed);
}

void ApplyEnemyFrame(EnemySlot& s, std::uintptr_t sm) {
    if (g_et_faulted.load(std::memory_order_relaxed)) return;
    static thread_local EnemyPush p;
    if (!s.push.Read(p) || !p.seq) {
        s.no_frame.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uintptr_t pose = 0, lb = 0, mb = 0;
    if (!EnemyHdr(sm, &pose, &lb, &mb, true)) {
        s.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static thread_local Joint cur[kJ];
    if (!mem::Read(lb, cur, sizeof(cur))) return;
    for (unsigned k = 1; k < kJ; ++k) {  // joint 0 (the root) stays its own: its place is the entity's
        const wire::Quat& q = p.q[k];
        cur[k].q[0] = q.x, cur[k].q[1] = q.y, cur[k].q[2] = q.z, cur[k].q[3] = q.w;
    }
    if (p.have_pelvis) std::memcpy(cur[kSamPelvis].t, p.tpelvis, sizeof(p.tpelvis));
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(lb), cur, sizeof(cur), &wrote) || wrote != sizeof(cur)) {
        s.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::int32_t bounds_on = 0;
    Rd(sm + kSmBoundsOn, bounds_on);
    if (!CallPoseFns(sm, pose, bounds_on != 0)) {
        EnemyFault(s);
        return;
    }
    s.applies.fetch_add(1, std::memory_order_relaxed);
}

// probe_sam (the P1-0 pose test): our Sam's live local pose onto the enemy, then the largest model-space rotation difference.
void ApplyEnemyPose(EnemySlot& es, std::uintptr_t sm) {
    if (g_et_faulted.load(std::memory_order_relaxed)) return;
    const std::uintptr_t ssm = g_sam_sm.load(std::memory_order_acquire), sskel = g_sam_skel.load(std::memory_order_acquire);
    std::uintptr_t vt = 0;
    if (!ssm || !sskel || !Rd(sm, vt) || vt != g_sm_vt) {
        es.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint8_t hdr[kPoseSize], shdr[kPoseSize];
    std::uintptr_t pose = 0, spose = 0;
    if (!ReadPoseHdr(sm, hdr, &pose) || !ReadPoseHdr(ssm, shdr, &spose) || Field<std::uintptr_t>(hdr, kHdrSkel) != sskel ||
        Field<std::uintptr_t>(shdr, kHdrSkel) != sskel || Field<std::int32_t>(hdr, kHdrState) != kPoseStateModel) {
        es.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uintptr_t lb = Field<std::uintptr_t>(hdr, kHdrLocal), le = Field<std::uintptr_t>(hdr, kHdrLocalEnd);
    const std::uintptr_t mb = Field<std::uintptr_t>(hdr, kHdrModel), me = Field<std::uintptr_t>(hdr, kHdrModelEnd);
    const std::uintptr_t slb = Field<std::uintptr_t>(shdr, kHdrLocal), sle = Field<std::uintptr_t>(shdr, kHdrLocalEnd);
    const std::uintptr_t smb = Field<std::uintptr_t>(shdr, kHdrModel), sme = Field<std::uintptr_t>(shdr, kHdrModelEnd);
    if (!lb || le - lb != kJ * sizeof(Joint) || !mb || me - mb != kJ * 0x40ull || !slb || sle - slb != kJ * sizeof(Joint) || !smb ||
        sme - smb != kJ * 0x40ull) {
        es.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static thread_local Joint cur[kJ];
    if (!mem::Read(slb, cur, sizeof(cur))) return;
    Joint root{};
    if (!mem::Read(lb, &root, sizeof(root))) return;
    cur[0] = root;  // its own root: the place stays the enemy's
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(lb), cur, sizeof(cur), &wrote) || wrote != sizeof(cur)) {
        es.rejects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::int32_t bounds_on = 0;
    Rd(sm + kSmBoundsOn, bounds_on);
    if (!CallPoseFns(sm, pose, bounds_on != 0)) {
        EnemyFault(es);
        return;
    }
    es.writes.fetch_add(1, std::memory_order_relaxed);
    // the model-space rotations against Sam's (the same local pose: the same model pose expected)
    static thread_local float m[kJ][16], sm2[kJ][16];
    if (!mem::Read(mb, m, sizeof(m)) || !mem::Read(smb, sm2, sizeof(sm2))) return;
    double worst = 0;
    for (unsigned j = 1; j < kJ; ++j) {
        // trace of A * B^T over the 3x3 rotation rows (normalised rows: the model matrices may carry scale)
        double tr = 0;
        for (int r = 0; r < 3; ++r) {
            double la = 0, lbn = 0, d = 0;
            for (int c = 0; c < 3; ++c) {
                la += static_cast<double>(m[j][r * 4 + c]) * m[j][r * 4 + c];
                lbn += static_cast<double>(sm2[j][r * 4 + c]) * sm2[j][r * 4 + c];
                d += static_cast<double>(m[j][r * 4 + c]) * sm2[j][r * 4 + c];
            }
            if (la <= 1e-12 || lbn <= 1e-12) continue;
            tr += d / std::sqrt(la * lbn);
        }
        double c = (tr - 1.0) / 2.0;
        c = c > 1 ? 1 : c < -1 ? -1 : c;
        const double deg = std::acos(c) * 180.0 / 3.14159265358979323846;
        if (deg > worst) worst = deg;
    }
    const std::uint32_t mdeg = static_cast<std::uint32_t>(worst * 1000.0);
    std::uint32_t prev = es.err_mdeg.load(std::memory_order_relaxed);
    while (mdeg > prev && !es.err_mdeg.compare_exchange_weak(prev, mdeg, std::memory_order_relaxed)) {
    }
}

// The detour's enemy part (after g_orig): true when `s` is an enemy slot's model (handled).
bool EnemyDetour(std::uintptr_t s) {
    if (!s || !g_et_any.load(std::memory_order_relaxed)) return false;
    for (unsigned k = 0; k < kEnemySlots; ++k) {
        EnemySlot& e = g_es[k];
        if (e.sm.load(std::memory_order_relaxed) != s) continue;
        g_inflight.fetch_add(1, std::memory_order_seq_cst);
        e.busy.fetch_add(1, std::memory_order_seq_cst);
        if (e.sm.load(std::memory_order_seq_cst) == s) {
            e.calls.fetch_add(1, std::memory_order_relaxed);
            switch (static_cast<EnemySlotMode>(e.mode.load(std::memory_order_relaxed))) {
                case EnemySlotMode::capture: CaptureEnemyPose(e, s); break;
                case EnemySlotMode::apply: ApplyEnemyFrame(e, s); break;
                case EnemySlotMode::probe_sam: ApplyEnemyPose(e, s); break;
                default: break;
            }
        }
        e.busy.fetch_sub(1, std::memory_order_seq_cst);
        g_inflight.fetch_sub(1, std::memory_order_seq_cst);
        return true;
    }
    return false;
}

void Detour(void* sm, float dt) {
    g_orig(sm, dt);
    const auto s = reinterpret_cast<std::uintptr_t>(sm);
    if (EnemyDetour(s)) return;
    if (s && s == g_bt.sm.load(std::memory_order_relaxed)) {
        g_inflight.fetch_add(1, std::memory_order_seq_cst);
        if (g_bt.sm.load(std::memory_order_seq_cst) == s) {
            if (!t_forcing) g_bt.game_updates.fetch_add(1, std::memory_order_relaxed);
            ApplyBootsPose(s);
        }
        g_inflight.fetch_sub(1, std::memory_order_seq_cst);
        return;
    }
    for (int x = 0; x < 2; ++x) {
        if (!s || s != g_xt[x].sm.load(std::memory_order_relaxed)) continue;
        g_inflight.fetch_add(1, std::memory_order_seq_cst);
        if (g_xt[x].sm.load(std::memory_order_seq_cst) == s) {
            if (!t_forcing) g_xt[x].game_updates.fetch_add(1, std::memory_order_relaxed);
            ApplyExoPose(x, s);
        }
        g_inflight.fetch_sub(1, std::memory_order_seq_cst);
        return;
    }
    if (s && g_pt_live.load(std::memory_order_relaxed)) {
        for (unsigned k = 0; k < kMaxPoseTargets; ++k) {
            if (g_pt[k].sm.load(std::memory_order_relaxed) != s) continue;
            g_inflight.fetch_add(1, std::memory_order_seq_cst);
            if (g_pt_live.load(std::memory_order_seq_cst) && g_pt[k].sm.load(std::memory_order_seq_cst) == s) {
                if (!t_forcing) g_pt[k].game_updates.fetch_add(1, std::memory_order_relaxed);
                ApplyGearPose(k, s);
            }
            g_inflight.fetch_sub(1, std::memory_order_seq_cst);
            return;
        }
    }
    if (!s || (s != g_sam_sm.load(std::memory_order_relaxed) && s != g_p.sm.load(std::memory_order_relaxed) &&
               s != g_me.sm.load(std::memory_order_relaxed) && s != g_menu_sm.load(std::memory_order_relaxed)))
        return;
    g_inflight.fetch_add(1, std::memory_order_seq_cst);
    g_ct.calls.fetch_add(1, std::memory_order_relaxed);
    const bool menu_src = g_self_src.load(std::memory_order_seq_cst) == 1;
    if (s == g_sam_sm.load(std::memory_order_seq_cst)) {
        CaptureSam(s);
        const int follow = g_self_follow.load(std::memory_order_relaxed);
        if (follow == 4) TrackSam(dt);
        if ((follow == 1 || follow == 4) && !menu_src) CopySamTransform();
        if (!t_forcing) HeadCorrect(s);  // his head items onto her drawn head (section 26.8)
    } else if (s == g_menu_sm.load(std::memory_order_seq_cst)) {
        // The cargo menu's figure: its pose for the self body, then its transform onto it (follow 1 and 2
        // alike; follow 3 leaves it to the tick).
        CaptureMenu(s);
        if (menu_src && g_self_follow.load(std::memory_order_relaxed) != 3) CopySamTransform(true);
    } else if (s == g_p.sm.load(std::memory_order_seq_cst)) {
        if (!t_forcing) g_p.game_updates.fetch_add(1, std::memory_order_relaxed);
        g_p.updates.fetch_add(1, std::memory_order_relaxed);
        ApplyBody(g_p, s, false);
    } else if (s == g_me.sm.load(std::memory_order_seq_cst)) {
        if (!t_forcing) g_me.game_updates.fetch_add(1, std::memory_order_relaxed);
        g_me.updates.fetch_add(1, std::memory_order_relaxed);
        const int follow = g_self_follow.load(std::memory_order_relaxed);
        if (follow == 2 && !menu_src) CopySamTransform();
        else if (follow == 4 && !menu_src && !t_forcing) {
            g_body_seq.fetch_add(1, std::memory_order_acq_rel);
            PredictSelfWorld(dt);
        }
        ApplyBody(g_me, s, true);
        t_pred_cand_ok = false;
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
                    "48 85 C9 74 1C 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 33 D2 48 8B CB E8", w, cap) &&
              Check("ApplyMeshControl global mask check (+0x348)", kApplyGlobalCheck,
                    "48 83 BB 38 03 00 00 00 40 B6 01 75 46 48 83 BB 48 03 00 00 00 75 3C", w, cap) &&
              Check("ApplyMeshControl mask diff loop (+0x348)", kApplyMaskLoop, "48 81 C1 48 03 00 00 48 8B 81 C8 02 00 00 48 39 01 75", w, cap);
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
    if (!Rd(sam + kPlayerRootMask, m) || !Rd(sam + kPlayerGlobalMask, m)) return 0;
    return sam + kPlayerRootMask;
}

// Where our hide bit lives: the controller's global part mask (+0x348, see kPlayerGlobalMask), not
// the Root mask, so the game keeps showing Sam's gear entities. 0 when SamMaskAddr refuses.
std::uintptr_t SamHideAddr(std::uintptr_t sam) { return SamMaskAddr(sam) ? sam + kPlayerGlobalMask : 0; }

// Sets / clears only our reason bit with a locked operation: a game bit can never be lost. (The
// game's own SetRootVisible is a plain read-modify-write, so ours can be: a lost SET is re-asserted
// every tick while shown, a lost CLEAR is repaired every tick while not shown, SelfRepair.) The
// entity is checked with fault-free reads first; the locked write itself is direct, so an entity
// freed on another thread between the check and the write is a small accepted window (the change-
// of-player and asleep paths).
bool SetOurReason(std::uintptr_t sam, bool on, std::uint64_t* mask_out = nullptr) {
    const std::uintptr_t a = SamHideAddr(sam);
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
// Fragile's porter suit in Sam's suit colours (suit_colour.hpp, docs section 27): the partner slot's and the self view's
// fragile_suit bodies; -1 = follow the colour, 0..16 the debug override (real_outfit 5).
suitcol::Dress g_suit_p, g_suit_me;
int g_suit_override = -1;
// Round 6 T2: the user's edited textures on the same bodies (fragile_textures.hpp, docs section 29); one owner per body:
// ftex while a look of edited textures exists, else the v0.23 tint (fragile_suit_colour=1), else her own suit.
ftex::Dress g_tex_p, g_tex_me;
// Round 12 (section 37.5): the colour row her raised hood is drawn in, per slot body (suit_colour_math HoodRow; -1 unknown):
// from what DressFragile binds on it. The partner's own row comes over STC1 (partner_gear); this one is its fallback there
// (a partner's painted look counts as the colour it was painted for). Kept between dressings (HoodRow checks
// that the slot's body is still a live fragile_suit; forgotten with the slot's body in ResetSlot, so a new body is not
// dressed in the previous one's row until DressFragile has run for it).
int g_hood_row[2] = {-1, -1};       // [0] partner slot, [1] self
bool g_hood_row_look[2] = {};       // the row is a bound look's (edited / ready-made textures)

// ---------------------------------------------------------------- body slot (game thread)
double g_now = 0;                  // the tick's clock (link::NowMsPrecise), for debug-started spawns

void ResetSlot(BodySlot& b) {
    if (&b == &g_slots[0]) outfit::Forget(g_dress);
    suitcol::Forget(&b == &g_slots[0] ? g_suit_p : g_suit_me);
    ftex::Forget(&b == &g_slots[0] ? g_tex_p : g_tex_me);
    g_hood_row[&b == &g_slots[0] ? 0 : 1] = -1;
    g_hood_row_look[&b == &g_slots[0] ? 0 : 1] = false;
    b.owner = Owner::none;
    b.phase = Phase::idle;
    b.spawn = holo::Spawn{};
    b.body = nullptr;
    b.live_sm = 0;
    b.mirror = false;
    b.start_hidden = false;
    b.last_game_ms = 0;
    b.freq_e = 0;  // a new body (even at the same address) is read and boosted afresh
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
void MenuEnd(const char* why);     // the cargo menu's figure: its parts back, its model unpublished

bool Teardown(BodySlot& b, const char* why) {
    if (&b == &g_me) {
        MenuEnd(why);
        SelfUnshow(why);
    }
    if (b.phase == Phase::idle && !b.spawn.sp) {
        ResetSlot(b);
        return false;
    }
    // round 27 M4: the destroy guard no longer watches it
    if (&b == &g_p) rmhook::Watch(rmhook::kWatchBody, 0);
    // Player collision: the partner body's capsule leaves the world while the body is still live
    // (a teardown that cannot remove the entity must not leave an invisible wall).
    if (&b == &g_p && b.spawn.e) collision::BodyGoing(b.spawn.e);
    // her folded hood (section 37.8): our view masks and LOD pin written back while the body is still live
    if (b.spawn.e) hoodfold::BodyGoing(reinterpret_cast<std::uintptr_t>(b.spawn.e));
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
    // Sam's model; while the cargo menu's figure is the source (Sam asleep, section 15), the figure's: the
    // same 77-joint layout, Sam's skeleton resource.
    const std::uintptr_t sam_sm = g_sam_sm.load() ? g_sam_sm.load() : g_menu_sm.load();
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
    for (unsigned j = 0; j < kJ; ++j) b.inv[j] = static_cast<std::uint8_t>(j);
    for (unsigned j = 0; j < kJ; ++j) b.inv[b.map[j]] = static_cast<std::uint8_t>(j);
    b.live_sm = sm;
    b.same_skel = ps.skel == ss.skel;
    b.latch_good = b.latch_seen = 0;
    b.latch_ok = false;
    b.pelvis_ratio = 1.0f;
    b.pelvis_off_ok = false;
    b.latch_state.store(0, std::memory_order_release);
    b.skel.store(ps.skel, std::memory_order_release);
    b.e_vt = Q(e);
    b.e.store(e, std::memory_order_release);
    b.sm.store(sm, std::memory_order_seq_cst);
    return nullptr;
}

// Starts the spawner for a DSControlledEntity by path + GUID (the bodies and, quietly, the gear props).
BeginResult StartByPath(const char* tag, const char* key, const char* label, const char* path, const std::uint8_t* guid,
                        std::uint32_t spawn_tag, const game::WorldTransform& at, bool quiet, holo::Spawn* out, char* why,
                        std::size_t cap) {
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
    if (!CallStrCtor(&fp->str, path) || !fp->str) {
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
    if (!quiet) Log("[%s] spawning '%s' (%s): path '%s' guid %s; manager %p (reference priority %d%s)", tag, key, label, path,
        GuidText(guid).c_str(), reinterpret_cast<void*>(mgr), prio, loaded_test ? " - BAD: reads as loaded" : "");
    if (loaded_test) {
        CallStrDtor(&fp->str);
        VirtualFree(fp, 0, MEM_RELEASE);
        Set(why, cap, "internal: priority bits read as loaded");
        return BeginResult::failed;
    }
    char w[200] = {};
    const bool ok = holo::SpawnBeginField(reinterpret_cast<std::uintptr_t>(&field), spawn_tag, at, out, w, sizeof(w));
    if (!quiet) Log("[%s]   proxy after the spawner ctor: hash %08X (the manager computed it), %s", tag, static_cast<std::uint32_t>(fp->hash), w);
    CallStrDtor(&fp->str);  // Register copied the String (refcounted)
    VirtualFree(fp, 0, MEM_RELEASE);
    if (!ok) {
        Set(why, cap, "spawner: %s", w);
        return holo::Available() ? BeginResult::failed : BeginResult::unavailable;
    }
    if (!quiet) LogSpawnerRef(tag, out->sp, "after StartSpawner");
    return BeginResult::started;
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
    holo::Spawn sp;
    const BeginResult r = StartByPath(slot.tag, body.key, body.label, body.path, guid, body.id, at, false, &sp, why, cap);
    if (r != BeginResult::started) return r;
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
        // round 27 M4 (docs/enemy-sync.md 16.2): the destroy guard saw the game begin removing the partner entity (its
        // RemoveEntityNow detour, before the entity can be freed): the slot is dropped without touching the entity (the game is
        // removing it already: no remove of ours, no collision or hood write-back)
        if (&slot == &g_p && rmhook::Removed(rmhook::kWatchBody, reinterpret_cast<std::uintptr_t>(slot.spawn.e))) {
            Set(why, cap, "the real body entity %p is being removed by the game (seen before the free)", slot.spawn.e);
            Log("[%s] the real body entity %p is being removed by the game (seen before the free): its slot dropped, the entity not touched", slot.tag,
                slot.spawn.e);
            l1probe::ReleaseBeforeFree("a real body being removed by the game");
            etarget::ReleaseBeforeFree("a real body being removed by the game");
            Unpublish(slot);
            slot.spawn.e = nullptr;
            Teardown(slot, "entity being removed by the game");
            return PollResult::failed;
        }
        if (holo::SpawnAlive(slot.spawn)) return PollResult::live;
        Set(why, cap, "the real body entity %p was destroyed by the game", slot.spawn.e);
        Log("[%s] %s", slot.tag, why);
        // round 27 M1: only the partner slot's body can carry her probe stimulus (review S9: never the self view's). The entity
        // is already gone here: the release is after the free (enemy_l1_probe.hpp's residual)
        if (&slot == &g_p) l1probe::ReleaseBeforeFree("a real body destroyed by the game");
        if (&slot == &g_p) etarget::ReleaseBeforeFree("a real body destroyed by the game");  // round 27 M2
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
    ++slot.lives;
    // round 27 M4: the destroy guard watches the partner entity (the game's removal seen before the free; PollSlot drops it)
    if (&slot == &g_p) rmhook::Watch(rmhook::kWatchBody, reinterpret_cast<std::uintptr_t>(e));
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

void PublishFrame(const wire::JointFrame& f, double play = 0) {
    static ApplyBuf a;
    Capture c;
    if (!g_capture.Read(c)) return;  // no local Sam pose for the fallback translations yet
    a.seq = ++g_apply_seq;
    a.play = play;
    for (unsigned j = 0; j < kJ; ++j) a.q[j] = j < f.joint_count ? f.q[j] : wire::Quat{};
    std::memcpy(a.fb, c.j, sizeof(a.fb));
    std::memcpy(a.troot, c.j[kSamRoot].t, sizeof(a.troot));
    std::memcpy(a.tpelvis, c.j[kSamPelvis].t, sizeof(a.tpelvis));
    for (unsigned i = 0; i < f.t_count; ++i) {
        if (f.t_joint[i] == kSamRoot) std::memcpy(a.troot, f.t[i], sizeof(a.troot));
        if (f.t_joint[i] == kSamPelvis) std::memcpy(a.tpelvis, f.t[i], sizeof(a.tpelvis));
    }
    g_apply.Write(a);
    g_apply_tick.store(GetTickCount64(), std::memory_order_release);
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
// Section 33 (round 7, the private room's black screen): the load / post-load Setup the self view waits out, the post-load
// window of the stall guard and its hold.
const char* g_self_load = nullptr;  // LoadOrSetup's reason last tick (nullptr: none)
double g_self_load_from_ms = 0, g_self_load_end_ms = -1e18;
ULONGLONG g_self_shown_at = 0;      // GetTickCount64 of the last show
double g_self_stall_until_ms = 0;  // the stall guard's hold: not shown again before this ...
std::uint64_t g_self_stall_state = 0;  // ... while the game state flags are still these
std::uint32_t g_self_load_unshows = 0, g_self_stalls = 0, g_self_room_holds = 0;
double g_self_room_hold_for = -2e18;  // the load end (g_self_load_end_ms) the private room's close-up hold was taken for
const char* g_self_hold_why = "";     // why the hold (g_self_stall_until_ms) keeps the body off
// `self_view 17 <mask>` (before / after runs): bit 1 the load rule (not shown through a load or Setup, the show waits for
// a fresh game update of Sam), bit 2 the private room's close-up hold and the stall guard (section 33; retired in round 11,
// off by default), bit 4 the clothes rule (section 34); 5 the default, 3 v0.25, 0 v0.24's behaviour.
std::atomic<int> g_self_load_rules{roomoutfit::kDefaultRules};
constexpr double kSelfPostLoadMs = 8000;     // the stall guard's window after a load or Setup ends
constexpr ULONGLONG kSelfStallMs = 1000;     // no game rewrite of Sam for this long, shown, in the window: unshown
constexpr double kSelfStallHoldMs = 20000;   // then not shown again while the game state stays the same, at most this long
constexpr ULONGLONG kSelfShowFreshMs = 100;  // the show needs a capture of Sam the game updated this recently
constexpr const char* kSelfLoadWhy = "the game is loading / in its post-load setup (reason 63 is never held through it)";
constexpr const char* kSelfStaleWhy = "Sam's model was not updated by the game in the last 100 ms";
constexpr const char* kSelfStallWhy =
    "the game stopped updating Sam's pose after a load (stall guard: Sam stays until the game state changes)";
constexpr const char* kSelfRoomWhy =
    "the private room's close-up after a load (Sam stays until the game state changes: hidden there, the game never draws the room)";
constexpr std::uint64_t kSelfRoomCut = game::kGameStatePrivateRoom | game::kGameStateCutscene;

// Section 34 (round 11): Sam's clothes change (his body variant switch waits for his paint effect, which only progresses
// while he is drawn: hidden by reason 63 it waited for the game's 60 s fallback, the private room's black screens) and
// her room outfit (her black suit in the private room).
roomoutfit::ClothesWatch g_clothes;
std::uint32_t g_clothes_n = 0;       // switches seen this session
bool g_clothes_long = false;         // the "still changing after 10 s" line was written for this switch
bool g_clothes_was_shown = false;    // the self body was shown when the switch started
constexpr double kClothesLongMs = 10000;
constexpr const char* kSelfClothesWhy = "the game is changing Sam's clothes (his paint effect waits for him drawn)";
constexpr const char* kSelfRedressWhy = "putting her suit look on after the room outfit (at most 3 s)";
roomoutfit::Choice g_room_choice = roomoutfit::Choice::black;  // session.ini room_outfit
std::atomic<int> g_room_force{static_cast<int>(roomoutfit::Force::rule)};  // `self_view 18 <mode>`
const bodies::Body* g_self_look = nullptr;  // the body the self view wants now (nullptr: g_self_body)
double g_self_look_since = 0;               // ... since (a change waits for a hidden moment)
const char* g_self_look_wait = "";          // the last "waiting" reason logged
std::uint32_t g_self_look_swaps = 0;
double g_self_redress_until = 0;            // the re-dress gate's end (a swap back to her suit)
bool g_self_has_look = false;               // DressFragile: her suit has a look of edited / ready-made textures

// Our reason bit must never outlive the shown body. SelfUnshow clears it once with a locked AND,
// but the game's own SetRootVisible (0x1426B4E40) is a plain read-modify-write of the same mask:
// one that overlaps our clear writes the old value back with bit 63 still set, and Sam would stay
// invisible with no body. So every tick in which the body is not shown, the current player's mask
// is checked and a stray bit 63 cleared (fault-free reads and the vtable / back-pointer check
// first, SamMaskAddr).
void SelfRepair(std::uintptr_t sam) {
    if (g_self_shown || !sam || !g_mesh.ok) return;
    // Builds before this one put the bit in the Root mask: a stray one there is cleared too.
    if (const std::uintptr_t r = SamMaskAddr(sam)) {
        std::uint64_t rm = 0;
        if (Rd(r, rm) && (rm & kOurReason)) InterlockedAnd64(reinterpret_cast<volatile LONG64*>(r), static_cast<LONG64>(~kOurReason));
    }
    const std::uintptr_t a = SamHideAddr(sam);
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

// The fit (section 16) off: Sam's next update writes nothing and puts back any translation the game
// left as ours. Always before reason 63 is cleared.
void FitOff() { g_fit_want.store(false, std::memory_order_seq_cst); }

// The fit on: the body's latched translations and scales by Sam joint, its pelvis ratio. Only for a
// latched body on its own skeleton (a body on Sam's skeleton has his proportions).
const char* FitOn() {
    FitOff();
    if (!g_fit_cfg.load()) return "off (self_fit=0)";
    if (g_fit_faulted.load()) return "off (a model-matrix call faulted earlier)";
    if (g_me.latch_state.load(std::memory_order_acquire) != 1 || !g_me.latch_ok) return "off (the body's own pose was not latched)";
    if (g_me.same_skel) return "not needed (the body is on Sam's skeleton)";
    FitShape sh;
    for (unsigned bj = 0; bj < kJ; ++bj) {
        const unsigned sj = g_me.map[bj];
        if (sj == kSamRoot || sj == kSamPelvis || sj >= kJ) continue;
        sh.on[sj] = 1;
        std::memcpy(sh.t[sj], g_me.latch[bj].t, sizeof(sh.t[sj]));
        std::memcpy(sh.s[sj], g_me.latch[bj].s, sizeof(sh.s[sj]));
    }
    sh.pelvis_ratio = g_me.pelvis_ratio;
    std::memcpy(sh.pelvis_off, g_me.pelvis_off, sizeof(sh.pelvis_off));
    sh.pelvis_off_ok = g_me.pelvis_off_ok ? 1 : 0;
    if (!g_fit_shape.Write(sh)) return "off (the shape could not be published)";
    {
        int row = -1;
        for (unsigned i = 0; i < sizeof(kHeadgearRows) / sizeof(kHeadgearRows[0]); ++i)
            if (g_me.body && kHeadgearRows[i].body_id == g_me.body->id) row = static_cast<int>(i);
        if (row >= 0 && !g_hg_tuned.load())
            for (int c = 0; c < kHeadItems; ++c)
                for (int k = 0; k < 3; ++k) g_hg_mm[c][k].store(kHeadgearRows[row].mm[c][k]);
        g_hg_row.store(row, std::memory_order_seq_cst);
    }
    g_lift_reset.store(true, std::memory_order_seq_cst);  // the head lift is measured again on this body
    g_fit_want.store(true, std::memory_order_seq_cst);
    return nullptr;
}

// FitOff, then wait (at most 5 ms) for a Sam update already inside the fit: one that read "on" before
// FitOff must finish its write before reason 63 is cleared, so the next update is the one that ends
// the fit (review: an in-flight write could land after the clear).
void FitOffWait() {
    FitOff();
    g_fit_shown.store(false, std::memory_order_seq_cst);
    const ULONGLONG until = GetTickCount64() + 5;
    while (g_fit_busy.load(std::memory_order_seq_cst) && GetTickCount64() < until) SwitchToThread();
}

void SelfUnshow(const char* why) {
    FitOffWait();
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
    g_self_shown_at = GetTickCount64();
    ++g_self_shows;
    g_self_not_shown = "";
    const char* fit = FitOn();
    g_fit_shown.store(true, std::memory_order_seq_cst);
    Log("[self] showing '%s' in place of Sam (Sam's global part mask 0x%llX, reason 63 ours: his gear stays; follow mode %d; show %u); "
        "fit %s",
        g_me.body ? g_me.body->key : "?", static_cast<unsigned long long>(m), g_self_follow.load(), g_self_shows,
        fit ? fit : "on (Sam's hidden skeleton takes the body's bone lengths: his gear sits on the body)");
    return true;
}

void SelfNotShown(const char* why) {
    if (g_self_shown) SelfUnshow(why);
    if (std::strcmp(g_self_not_shown, why) != 0) {
        g_self_not_shown = why;
        Log("[self] body not shown: %s", why);
    }
}

// ---------------------------------------------------------------- section 34: Sam's clothes change, her room outfit
bool IsFragileSuit(const bodies::Body* b);
const bodies::Body* SelfLook() { return g_self_look ? g_self_look : g_self_body; }

// Sam's variant fields (no paint), read once per tick (g_now) for the clothes watch, the room outfit and STJ1.
bool ReadVariantCached(std::uintptr_t sam, outfit::VariantState* out) {
    static std::uintptr_t s_sam = 0;
    static double s_ms = -1;
    static bool s_ok = false;
    static outfit::VariantState s_vs;
    if (sam != s_sam || g_now != s_ms) {
        s_sam = sam, s_ms = g_now;
        s_vs = outfit::VariantState{};
        s_ok = sam && outfit::ReadVariantState(sam, &s_vs, false);
    }
    *out = s_vs;
    return s_ok;
}

// The body the room-outfit rule (room_outfit_rules.hpp) wants for `base` now. `slot`: a body refused there is not wanted.
const bodies::Body* RoomLook(const bodies::Body* base, std::uintptr_t sam, std::uint64_t gstate, const bodies::Body* current, const BodySlot* slot,
                             outfit::VariantState* vs_out) {
    outfit::VariantState vs;
    roomoutfit::LookInput in;
    if (!base) return nullptr;
    in.base_id = base->id;
    in.choice = g_room_choice;
    in.force = static_cast<roomoutfit::Force>(g_room_force.load(std::memory_order_relaxed));
    in.in_private_room = (gstate & game::kGameStatePrivateRoom) != 0;
    in.variant_ok = ReadVariantCached(sam, &vs);
    in.cur = vs.cur, in.wanted = vs.wanted, in.state = vs.state;
    in.current_id = current ? current->id : 0;
    if (vs_out) *vs_out = vs;
    const std::uint8_t id = roomoutfit::WantedBodyId(in);
    if (id == base->id) return base;
    const bodies::Body* b = bodies::FindById(id);
    if (!b || (slot && b->id < 32 && (slot->refused & (1u << b->id)))) return base;
    return b;
}

// Every tick with a local player (also with the self view off: the log lines are the before / after evidence). Logs the
// start, each state step and the end of a switch with its duration, the variants, the paint effect and
// DSPlayerLoadingModel. True while the clothes rule keeps the body off (`self_view 17` bit 4).
bool ClothesTick(std::uintptr_t sam, double now_ms, int rules) {
    const bool on = (rules & roomoutfit::kRuleClothes) != 0;
    outfit::VariantState vs;
    const bool ok = ReadVariantCached(sam, &vs);
    const roomoutfit::ClothesEvent ev = roomoutfit::ClothesStep(g_clothes, ok, vs.state, vs.cur, vs.wanted, now_ms);
    const bool long_now = ok && g_clothes.busy && !g_clothes_long && now_ms - g_clothes.from_ms >= kClothesLongMs;
    if (ev != roomoutfit::ClothesEvent::none || long_now) {
        outfit::VariantState full = vs;
        char d[240] = "unreadable (no player, or its streaming control does not check out)";
        if (ok && outfit::ReadVariantState(sam, &full, true)) outfit::DescribeVariantState(full, d, sizeof(d));
        std::uint64_t gs = 0;
        game::LoadOrSetup(&gs, nullptr);
        char gd[96] = {};
        game::DescribeGameStateFlags(gs, gd, sizeof(gd));
        if (ev == roomoutfit::ClothesEvent::start) {
            ++g_clothes_n;
            g_clothes_long = false;
            g_clothes_was_shown = g_self_shown;
            Log("[clothes] the game is changing Sam's clothes (#%u, state %s): %s; self body %s", g_clothes_n, gd, d,
                !g_self_on      ? "off"
                : !g_self_shown ? "not shown"
                : on            ? "shown: Sam shown for it"
                                : "shown and kept (the clothes rule is off: the game waits up to 60 s)");
        } else if (ev == roomoutfit::ClothesEvent::step) {
            Log("[clothes] state %d %.2f s into the change (#%u): %s", vs.state, (now_ms - g_clothes.from_ms) / 1000.0, g_clothes_n, d);
        } else if (ev == roomoutfit::ClothesEvent::end) {
            Log("[clothes] Sam's clothes changed in %.2f s (#%u, variant %u -> %u, state %s): %s", g_clothes.last_ms / 1000.0, g_clothes_n,
                g_clothes.from_variant, ok ? vs.cur : g_clothes.to_variant, gd, d);
        }
        if (long_now) {
            g_clothes_long = true;
            Log("[clothes] still changing after %.0f s (#%u, state %s): %s; self body %s", kClothesLongMs / 1000.0, g_clothes_n, gd, d,
                g_self_shown ? "SHOWN" : "not shown");
        }
    }
    return on && roomoutfit::ClothesHold(g_clothes, now_ms);
}

// The re-dress gate: after a swap back to her porter suit, the body waits (at most 4 s from the swap, her respawn
// included, so about 3 s once she is live) until her suit look is bound
// (DressFragile starts 0.5 s after the body is live), so she never shows raw for a moment.
bool RedressWait(double now_ms) {
    if (now_ms >= g_self_redress_until || !IsFragileSuit(g_me.body) || !ftex::Enabled()) return false;
    if (GetTickCount64() - g_me.live_tick < 700) return true;
    if (!g_self_has_look) return false;
    return !(g_tex_me.e == reinterpret_cast<std::uintptr_t>(g_me.spawn.e) && g_tex_me.bound);
}

// ---------------------------------------------------------------- the cargo menu's figure (game thread)
// docs/real-partner-body.md section 15. While the on-foot cargo menu is open, the subspace work object
// wakes its own Sam (DSPlayerAKukanEntity) in a room of the world and puts the local Sam to sleep. The
// self body then stays (it is not removed as on other sleeps), stands on the figure and plays its pose
// (the figure's model is captured by the detour, its transform copied in the figure's own update), and
// the figure's ArtParts are hidden (menu_figure.cpp: the top-level parts; the subspace gear and the
// cargo are entities of their own on its skeleton and stay). Round 15 (section 40): the private room's colour menu
// (kind 6) too, with her porter suit in the colour being previewed; kind 7 keeps Sam. When the menu closes (or anything does not check out) the figure's
// parts are put back first, then the body hidden; the field self view takes over when Sam wakes.
bool g_menu_on = true;      // `self_view 3 <0/1>` (on with the self view)
// Round 15 (section 40): the private room's uniform menu (kind 6) too: her porter suit on its figure, in the colour being
// previewed (`real_outfit 9` bit 2).
bool g_wardrobe_on = false;  // round 15 review: off until stills show her on the wardrobe's figure (`real_outfit 9 3` turns it on)
bool g_menu_wardrobe = false;  // the active menu is the wardrobe
wardrobe::State g_wd;          // the wardrobe's state this tick (WardrobeTick)
std::uint8_t g_wd_shown = 0xFF;  // the colour her self body was last given in the wardrobe (0xFF: none)
bool g_menu_active = false;  // the figure's model is published, the self body follows it
bool g_menu_shown = false;   // our body shown, the figure's top-level parts hidden by us
menufig::Hidden g_menu_hidden;
const char* g_menu_not_shown = "";
double g_menu_retry_ms = 0, g_menu_rehide_ms = 0;
std::uint32_t g_menu_shows = 0, g_menu_unshows = 0, g_menu_tick_moves = 0, g_menu_rehide_ticks = 0;
// A figure whose parts we hid but could not put back (a fault, or it stopped checking out): from then
// on our body stands in for it whenever the menu shows it, and is never hidden there (the menu never
// shows nobody because of us).
std::uintptr_t g_menu_lost_e = 0;

void MenuUnshow(const char* why) {
    if (!g_menu_shown) return;
    // The figure first, then our body.
    char w[160] = {};
    const std::uintptr_t fig = g_menu_hidden.e;
    const bool back = menufig::Restore(&g_menu_hidden, w, sizeof(w));
    if (!back && fig) g_menu_lost_e = fig;
    game::Entity* e = g_me.phase == Phase::live && holo::SpawnAlive(g_me.spawn) ? g_me.spawn.e : nullptr;
    const bool keep = fig ? !back : g_menu_lost_e != 0;  // the figure stays hidden: our body stays
    const bool hid = e && !keep && partner::SetVisible(e, false);
    g_menu_shown = false;
    ++g_menu_unshows;
    Log("[menu] the menu's Sam figure shown again (%s): %s; body %s", why, w,
        keep ? "left visible (the figure's parts could not be put back)" : hid ? "hidden" : e ? "could not be hidden" : "gone");
}

void MenuEnd(const char* why) {
    MenuUnshow(why);
    if (!g_menu_active && !g_menu_sm.load(std::memory_order_relaxed)) return;
    g_self_src.store(0, std::memory_order_seq_cst);
    g_menu_sm.store(0, std::memory_order_seq_cst);
    g_menu_e.store(0, std::memory_order_seq_cst);
    g_menu_skel.store(0, std::memory_order_seq_cst);
    const ULONGLONG until = GetTickCount64() + 50;
    while (g_inflight.load(std::memory_order_seq_cst) != 0 && GetTickCount64() < until) SwitchToThread();
    g_menu_active = false;
    g_menu_wardrobe = false;
    g_menu_not_shown = "";
    Log("[menu] the self body follows the local Sam again (%s)", why);
}

void MenuNotShown(const char* why) {
    if (g_menu_shown) MenuUnshow(why);
    if (std::strcmp(g_menu_not_shown, why) != 0) {
        g_menu_not_shown = why;
        Log("[menu] body not shown in the menu: %s", why);
    }
}

void MenuShow(const menufig::View& v, double now_ms) {
    char w[160] = {};
    if (v.e && v.e == g_menu_lost_e) {
        // the figure is still hidden from an earlier fault: only our body is shown
        if (partner::SetVisible(g_me.spawn.e, true)) {
            g_menu_shown = true;
            g_menu_not_shown = "";
            ++g_menu_shows;
            Log("[menu] showing '%s' on the menu's figure, whose parts stayed hidden after a fault (show %u)", g_me.body ? g_me.body->key : "?", g_menu_shows);
        } else {
            g_menu_retry_ms = now_ms + 5000;
        }
        return;
    }
    if (!menufig::Hide(v, &g_menu_hidden, w, sizeof(w))) {
        g_menu_retry_ms = now_ms + 5000;
        Log("[menu] the figure's parts could not be hidden (%s): the menu shows Sam", w);
        MenuNotShown("the figure's parts could not be hidden");
        return;
    }
    if (!partner::SetVisible(g_me.spawn.e, true)) {
        char r[160] = {};
        menufig::Restore(&g_menu_hidden, r, sizeof(r));
        g_menu_retry_ms = now_ms + 5000;
        Log("[menu] the body could not be made visible: the figure %s", r);
        MenuNotShown("show failed");
        return;
    }
    g_menu_shown = true;
    g_menu_not_shown = "";
    ++g_menu_shows;
    Log("[menu] showing '%s' in place of the menu's Sam figure: %s (kind %d/%d, show %u)", g_me.body ? g_me.body->key : "?", w, v.kind,
        v.kind_next, g_menu_shows);
}

// True while the cargo menu's figure is the self body's source (the body is kept, whatever Sam does).
bool MenuTick(std::uintptr_t sam, double now_ms) {
    menufig::View v;
    const bool read = menufig::Read(&v);
    const char* out = !g_menu_on               ? "the menu figure is switched off"
                      : !read                  ? "the subspace work object is not readable"
                      : !menufig::Open(v)      ? "the cargo menu closed"
                      : !menufig::KindAllowed(v, g_wardrobe_on) ? "a menu other than Manage Cargo or the wardrobe (the figure stays Sam)"
                      : v.player != sam        ? "the menu serves another player entity"
                                               : nullptr;
    if (out) {
        if (g_menu_active) MenuEnd(out);
        return false;
    }
    if (g_menu_active && v.e != g_menu_e.load(std::memory_order_relaxed)) MenuEnd("another menu figure");
    if (g_self_shown) SelfUnshow("the cargo menu opened");
    if (!g_menu_active) {
        g_menu_skel.store(v.skel, std::memory_order_release);
        g_menu_e.store(v.e, std::memory_order_release);
        g_menu_sm.store(v.sm, std::memory_order_seq_cst);
        g_self_src.store(1, std::memory_order_seq_cst);
        g_menu_active = true;
        g_menu_wardrobe = menufig::IsWardrobe(v);
        g_menu_not_shown = "";
        Log("[menu] the %s is open (kind %d/%d, state %d): the self body follows its Sam figure %p (model %p, skeleton %p%s)",
            g_menu_wardrobe ? "wardrobe (the private room's uniform menu)" : "cargo menu", v.kind, v.kind_next, v.state, reinterpret_cast<void*>(v.e),
            reinterpret_cast<void*>(v.sm), reinterpret_cast<void*>(v.skel), v.skel == g_sam_skel.load() ? ", Sam's own" : ", not Sam's object");
    }
    // Round 15 (section 40): the wardrobe previews Sam in his porter suit (the game switches him to it), so she wears her
    // base body there (her porter suit), also where the room outfit has her black suit: swapped while the menu opens.
    const bodies::Body* menu_look = g_menu_wardrobe ? g_self_body : SelfLook();
    if (g_menu_wardrobe && menu_look && g_me.phase != Phase::idle && g_me.body && g_me.body != menu_look) {
        Log("[menu] wardrobe: '%s' -> '%s' (her porter suit for the preview; state %d)", g_me.body->key, menu_look->key, v.state);
        MenuUnshow("wardrobe: her porter suit");
        Teardown(g_me, "wardrobe: her porter suit for the preview");
        g_self_retry_ms = 0;
        return true;
    }
    // The body normally lives on from the field; it is spawned on the figure when it does not.
    if (g_me.phase == Phase::idle) {
        const bodies::Body* look = menu_look;
        if (now_ms < g_self_retry_ms || (look->id < 32 && (g_me.refused & (1u << look->id)))) return true;
        game::WorldTransform at{};
        if (!game::TryGetTransform(reinterpret_cast<game::Entity*>(v.e), &at)) return true;
        char why[200] = {};
        g_me.start_hidden = true;
        const BeginResult r = BeginSlot(g_me, *look, at, Owner::self, now_ms, why, sizeof(why));
        if (r == BeginResult::started) {
            g_self_spawn_sam = sam;
            Log("[menu] spawning '%s' on the menu's figure", look->key);
        } else {
            g_me.start_hidden = false;
            Log("[menu] could not start '%s': %s", look->key, why);
            g_self_retry_ms = now_ms + 10000;
        }
        return true;
    }
    char why[200] = {};
    if (PollSlot(g_me, now_ms, why, sizeof(why)) == PollResult::failed) {
        g_self_retry_ms = now_ms + 10000;
        return true;
    }
    if (g_me.phase != Phase::live) return true;
    // Fallback: the copy in the figure's update did not run (follow 3, or the figure's model was not
    // updated): move the body from the tick.
    const ULONGLONG t = GetTickCount64();
    const ULONGLONG last_copy = g_menu_last_copy.load(std::memory_order_relaxed);
    if (g_self_follow.load() == 3 || !last_copy || t - last_copy > 100) {
        game::WorldTransform wt{};
        if (game::TryGetTransform(reinterpret_cast<game::Entity*>(v.e), &wt) && game::TrySetTransform(g_me.spawn.e, wt)) ++g_menu_tick_moves;
    }
    const ULONGLONG last_apply = g_menu_last_apply.load(std::memory_order_relaxed);
    const char* not_ready = g_me.latch_state.load(std::memory_order_acquire) != 1 ? "latching the body's own pose"
                            : !g_me.apply_on.load()                                ? "the pose write was switched off (a fault)"
                            : v.state == 3                                         ? "the menu is still opening"
                            : !last_apply || t - last_apply > 250                  ? "no fresh figure pose written onto the body"
                            : now_ms < g_menu_retry_ms                             ? "waiting to try again"
                                                                                   : nullptr;
    if (not_ready) {
        MenuNotShown(not_ready);
        return true;
    }
    if (!g_menu_shown) {
        MenuShow(v, now_ms);
        g_menu_rehide_ms = now_ms + 250;
    } else if (now_ms >= g_menu_rehide_ms) {
        // the game re-asking a top-level part visible was never seen; checked 4 times a second
        g_menu_rehide_ms = now_ms + 250;
        if (menufig::Rehide(&g_menu_hidden)) ++g_menu_rehide_ticks;
    }
    return true;
}

// Every tick (hidden-test evidence and the support log): the subspace state on change, and once a
// second while the menu is anywhere but idle, what the figure and the self body are doing.
void MenuWatch(double now_ms) {
    static menufig::View last;
    static double next_ms = 0;
    static std::uint32_t pc = 0, prj = 0, pco = 0, pfail = 0, paba = 0, papp = 0;
    menufig::View v;
    if (!menufig::Read(&v) && !v.w) return;
    static unsigned opens = 0;  // times the menu reached state 5 (hidden tests wait on it)
    if (v.state == 5 && last.state != 5) ++opens;
    if (v.state != last.state || v.req != last.req || v.kind != last.kind || v.kind_next != last.kind_next || v.e != last.e || v.awake != last.awake)
        Log("[menu] subspace state %d (asked %d) kind %d/%d flags 0x%02X figure %p %s; model %p skeleton %p (%s) parts %d (opens %u)", v.state, v.req,
            v.kind, v.kind_next, v.flags, reinterpret_cast<void*>(v.e), v.e ? (v.awake ? "awake" : "asleep") : "-", reinterpret_cast<void*>(v.sm),
            reinterpret_cast<void*>(v.skel), v.skel && v.skel == g_sam_skel.load() ? "Sam's" : v.skel ? "77 joints, Sam's layout" : "none", v.parts, opens);
    last = v;
    if (v.state == 0 || v.state == 2 || v.state < 0 || now_ms < next_ms) return;
    next_ms = now_ms + 1000;
    const std::uint32_t c = g_menu_caps.load(), rj = g_menu_rej.load(), co = g_menu_copies.load(), f = g_menu_copy_fail.load(),
                        ab = g_menu_copy_aba.load(), ap = g_me.ct.applies.load();
    double bp[3] = {};
    const bool have_body = g_me.phase == Phase::live && mem::Read(reinterpret_cast<std::uintptr_t>(g_me.spawn.e) + kEntWorld, bp, sizeof(bp));
    const double d = have_body ? std::sqrt((bp[0] - v.pos[0]) * (bp[0] - v.pos[0]) + (bp[1] - v.pos[1]) * (bp[1] - v.pos[1]) +
                                           (bp[2] - v.pos[2]) * (bp[2] - v.pos[2]))
                               : -1;
    Log("[menu] figure %p at (%.2f, %.2f, %.2f) %s kind %d/%d; captures %u rej %u copies %u (fail %u aba %u) tick-moves %u; body %s shown %d "
        "pose writes %u, body-figure distance %.1f cm; rehides %u; source %s",
        reinterpret_cast<void*>(v.e), v.pos[0], v.pos[1], v.pos[2], v.awake ? "awake" : "asleep", v.kind, v.kind_next, c - pc, rj - prj, co - pco,
        f - pfail, ab - paba, g_menu_tick_moves, g_me.phase == Phase::live ? "live" : g_me.phase == Phase::loading ? "loading" : "none",
        g_menu_shown ? 1 : 0, ap - papp, d * 100.0, g_menu_hidden.rehides, g_self_src.load() == 1 ? "figure" : "Sam");
    pc = c, prj = rj, pco = co, pfail = f, paba = ab, papp = ap;
}

void SelfTick(game::Entity* local, bool local_active, double now_ms) {
    const auto sam = reinterpret_cast<std::uintptr_t>(local);
    g_sam_carried.store(ride::Carried(), std::memory_order_relaxed);  // follow mode 4: no step while the carry moves Sam
    {
        // Sam's parent entity (a vehicle he drives / sits in, in the game): every change logged, numbered (follow mode 4
        // predicts him seated since v0.20; hidden scenarios wait on the "out of the vehicle" line, section 26.6)
        static std::uintptr_t s_parent = 0;
        static unsigned s_in = 0, s_out = 0;
        std::uintptr_t parent = 0;
        if (sam && g_self_on && Rd(sam + kEntParent, parent) && parent != s_parent) {
            if (parent) Log("[self] Sam in a vehicle (parent %p, #%u)%s", reinterpret_cast<void*>(parent), ++s_in, s_parent ? " (another)" : "");
            else Log("[self] Sam out of the vehicle (#%u)", ++s_out);
            s_parent = parent;
        }
    }
    // Another player entity (load, return to title) while our reason was set on the old one: it is
    // cleared there only if the old entity still reads as a live DSPlayerEntity whose controller
    // points back at it (SamMaskAddr; fault-free reads first), else nothing is written. (No player
    // at all is the asleep path below, which shows Sam again the same way.)
    if (sam && g_self_sam && g_self_sam != sam) {
        FitOffWait();
        const bool cleared = SetOurReason(g_self_sam, false);
        Log("[self] the local player changed while self view was shown: %s", cleared ? "reason 63 cleared on the old entity (still a player)"
                                                                                       : "the old entity is gone (not written)");
        g_self_sam = 0;
        g_self_shown = false;
    }
    SelfRepair(sam);
    if (sam && g_self_spawn_sam && sam != g_self_spawn_sam && (g_me.phase != Phase::idle || g_me.spawn.sp)) Teardown(g_me, "local player changed");
    // Section 34.6: the clothes rule unavailable (a byte check failed) brings the hold and the stall guard back.
    const int rules =
        roomoutfit::EffectiveRules(g_self_load_rules.load(std::memory_order_relaxed), outfit::ClothesAvailable());
    // Section 34: while the game changes Sam's clothes he is drawn (the clothes rule), and 0.5 s after.
    const bool clothes = ClothesTick(sam, now_ms, rules);
    if (!g_self_on || !g_self_body || !g_hook) {
        MenuEnd("self view off");
        if (g_me.phase != Phase::idle || g_me.spawn.sp) Teardown(g_me, "self view off");
        return;
    }
    // The cargo menu: the body stays and stands on the menu's figure (section 15).
    if (sam && MenuTick(sam, now_ms)) {
        g_self_asleep_since = 0;
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
    // Section 33: never hold reason 63 through a load or the post-load Setup. With Sam hidden by it there the game stopped
    // updating his model and waited on him: the private room's black screen (40+ s loading straight into it, about 9 s
    // entering it from the field). Sam is shown, the body stays spawned and is shown again once the game has settled.
    std::uint64_t gstate = 0;
    std::uint32_t lmask = 0;
    const char* load_now = game::LoadOrSetup(&gstate, &lmask);
    const char* loading = (rules & 1) ? load_now : nullptr;  // `self_view 17`: bit 1 off, the body stays shown (v0.24)
    if (load_now != g_self_load) {
        char d[96] = {};
        game::DescribeGameStateFlags(gstate, d, sizeof(d));
        if (load_now && !g_self_load) {
            g_self_load_from_ms = now_ms;
            Log("[self] the game is loading or settling: %s (state %s, load mask 0x%X)%s", load_now, d, lmask,
                g_self_shown && loading ? ": Sam shown for it" : "");
        } else if (!load_now) {
            g_self_load_end_ms = now_ms;
            Log("[self] the load / post-load setup is over after %.2f s (state %s)", (now_ms - g_self_load_from_ms) / 1000.0, d);
        }
        g_self_load = load_now;
    }
    if (loading && g_self_shown) {
        ++g_self_load_unshows;
        SelfUnshow(kSelfLoadWhy);
    }
    if (clothes && g_self_shown) SelfUnshow(kSelfClothesWhy);
    // Section 33 (review): the private room's close-up after a load (PrivateRoom|Cutscene in the 8 s after a load / Setup ends).
    // With Sam hidden there the game never draws the room after it (the user's black screen from the field), so the body is not
    // shown at all in it: Sam stays from the start until the game state changes (at most 20 s), instead of Fragile showing for a
    // second and the stall guard turning her into Sam mid-shot (run 20261122-191916). Once per load.
    if (now_ms < g_self_stall_until_ms && gstate != g_self_stall_state) g_self_stall_until_ms = 0;  // the state changed: the hold ends
    if ((rules & 2) && !load_now && (gstate & kSelfRoomCut) == kSelfRoomCut && now_ms - g_self_load_end_ms < kSelfPostLoadMs &&
        g_self_room_hold_for != g_self_load_end_ms) {
        g_self_room_hold_for = g_self_load_end_ms;
        ++g_self_room_holds;
        g_self_stall_until_ms = now_ms + kSelfStallHoldMs;
        g_self_stall_state = gstate;
        g_self_hold_why = kSelfRoomWhy;
        Log("[self] the private room's close-up %.2f s after a load (state 0x%llX): Sam stays until the game state changes (hold %u)",
            (now_ms - g_self_load_end_ms) / 1000.0, static_cast<unsigned long long>(gstate), g_self_room_holds);
        if (g_self_shown) SelfUnshow(kSelfRoomWhy);
    }
    // Section 34: her room outfit. The rule (room_outfit_rules.hpp) picks her black suit in the private room; the body is
    // changed only while it is not shown (a load, the clothes change, Sam asleep), else after 3 s outside cutscenes and
    // menus with Sam shown for about 0.12 s (`self_view 18 2/3`: at once).
    {
        outfit::VariantState vs;
        const bodies::Body* cur = g_me.phase != Phase::idle && g_me.body ? g_me.body : SelfLook();
        const bodies::Body* want = RoomLook(g_self_body, sam, gstate, cur, &g_me, &vs);
        char vd[112] = {};
        std::snprintf(vd, sizeof(vd), "Sam's variant %u, wanted %u, state %d; PrivateRoom %s; room_outfit=%s", vs.cur, vs.wanted, vs.state,
                      (gstate & game::kGameStatePrivateRoom) ? "yes" : "no", roomoutfit::ChoiceName(g_room_choice));
        if (want && want != SelfLook()) {
            Log("[self] room outfit: '%s' wanted now (was '%s'; %s)", want->key, SelfLook()->key, vd);
            g_self_look = want == g_self_body ? nullptr : want;
            g_self_look_since = now_ms;
            g_self_look_wait = "";
        }
        if (want && g_me.phase != Phase::idle && g_me.body && g_me.body != want) {
            constexpr std::uint64_t kSubspace = 0x100ull;
            const bool blocked =
                g_menu_active || (gstate & (game::kGameStateCutscene | kSubspace | game::kGameStatePauseMenu | game::kGameStateSystemPause)) != 0;
            const int force = g_room_force.load(std::memory_order_relaxed);
            const roomoutfit::Swap sw = roomoutfit::SwapDecision(g_self_shown, now_ms - g_self_look_since, blocked, force == 2 || force == 3);
            if (sw == roomoutfit::Swap::wait) {
                const char* w = blocked ? "waits (a cutscene or a menu: changed when the body is not shown)" : "waits for a moment the body is not shown";
                if (std::strcmp(g_self_look_wait, w) != 0) {
                    g_self_look_wait = w;
                    Log("[self] room outfit: '%s' -> '%s' %s", g_me.body->key, want->key, w);
                }
            } else {
                ++g_self_look_swaps;
                const bool back = want == g_self_body;
                Log("[self] room outfit: '%s' -> '%s' (%s): %s swap (#%u)", g_me.body->key, want->key, vd,
                    sw == roomoutfit::Swap::hidden ? "hidden" : "visible", g_self_look_swaps);
                if (sw == roomoutfit::Swap::visible) SelfUnshow("room outfit: her clothes change (a visible swap)");
                Teardown(g_me, "room outfit");
                g_self_redress_until = back ? now_ms + 4000 : 0;
                g_self_retry_ms = 0;
                return;
            }
        }
    }
    if (g_me.phase == Phase::idle) {
        if (now_ms < g_self_retry_ms) return;
        const bodies::Body* look = SelfLook();
        if (look->id < 32 && (g_me.refused & (1u << look->id))) {
            if (look != g_self_body) {
                Log("[self] room outfit: '%s' was refused: '%s' stays", look->key, g_self_body->key);
                g_self_look = nullptr;
                return;
            }
            Log("[self] self view off: body '%s' was refused", g_self_body->key);
            g_self_on = false;
            return;
        }
        game::WorldTransform at{};
        if (!game::TryGetTransform(local, &at)) return;
        char why[200] = {};
        g_me.start_hidden = true;
        const BeginResult r = BeginSlot(g_me, *look, at, Owner::self, now_ms, why, sizeof(why));
        if (r == BeginResult::started) g_self_spawn_sam = sam;
        else {
            g_me.start_hidden = false;
            Log("[self] could not start '%s': %s", look->key, why);
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
    // Section 33: the stall guard (the fallback of the close-up hold above). Shown in the private room's PrivateRoom|Cutscene in
    // the 8 s after a load: when the game has not moved Sam's joints for 1 s (his clean joints unchanged from update to update,
    // fit on or off, or no update of his model at all), Sam is shown until the game state changes (at most 20 s). Only that
    // state: elsewhere a cinematic that leaves a few joints alone is normal (the bed scene before the load, 8 s of it).
    if ((rules & 2) && g_self_shown && now_ms - g_self_load_end_ms < kSelfPostLoadMs && (gstate & kSelfRoomCut) == kSelfRoomCut &&
        !(gstate & (game::kGameStatePauseMenu | game::kGameStateSystemPause))) {
        ULONGLONG progress = g_self_shown_at;
        Capture cap;
        const bool have_cap = g_capture.Read(cap) && cap.frame != 0;
        const bool fit_on = g_fit_want.load(std::memory_order_relaxed) && g_fit_cfg.load(std::memory_order_relaxed) &&
                            !g_fit_faulted.load(std::memory_order_relaxed);
        const ULONGLONG moved = g_sam_last_change.load(std::memory_order_relaxed);
        if (moved > progress) progress = moved;
        if (have_cap && cap.tick_ms + kSelfStallMs < t && cap.tick_ms < progress) progress = cap.tick_ms;  // no update at all
        if (t > progress + kSelfStallMs) {
            ++g_self_stalls;
            Log("[self] stall guard: the game has not updated Sam's pose for %llu ms in a cutscene %.1f s after a load (state 0x%llX, fit %s): Sam shown "
                "(stall %u)",
                static_cast<unsigned long long>(t - progress), (now_ms - g_self_load_end_ms) / 1000.0, static_cast<unsigned long long>(gstate),
                fit_on ? "on" : "off", g_self_stalls);
            g_self_stall_until_ms = now_ms + kSelfStallHoldMs;
            g_self_stall_state = gstate;
            g_self_hold_why = kSelfStallWhy;
            SelfNotShown(kSelfStallWhy);
            return;
        }
    }
    bool cap_fresh = true;
    if (!g_self_shown && (rules & 1)) {
        Capture cap;
        cap_fresh = g_capture.Read(cap) && cap.frame != 0 && t - cap.tick_ms <= kSelfShowFreshMs;
    }
    const char* hidden_why = nullptr;
    const char* not_ready = g_me.latch_state.load(std::memory_order_acquire) != 1 ? "latching the body's own pose"
                            : !g_me.apply_on.load()                                ? "the pose write was switched off (a fault)"
                            : loading                                              ? kSelfLoadWhy
                            : clothes                                              ? kSelfClothesWhy
                            : now_ms < g_self_stall_until_ms                       ? g_self_hold_why
                            : RedressWait(now_ms)                                  ? kSelfRedressWhy
                            : !last_apply || t - last_apply > 250                  ? "no fresh pose written onto the body"
                            : !cap_fresh                                           ? kSelfStaleWhy
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
    if (b.pelvis_off_ok)
        Log("[%s]   far hips rule (section 38.3): beyond %.2f m from the root her hips keep Sam's minus her rest offset (%.1f, %.1f, %.1f) cm "
            "(%.2f m and nearer: the ratio)",
            b.tag, pelvis::kFarM, b.pelvis_off[0] * 100.0, b.pelvis_off[1] * 100.0, b.pelvis_off[2] * 100.0, pelvis::kNearM);
    if (b.leg_ratio > 0)
        Log("[%s]   legs (thigh + shin) %.3f m vs Sam %.3f m: ratio %.3f%s", b.tag, b.leg_body, b.leg_sam, b.leg_ratio,
            b.leg_used ? "; the latched pelvis disagrees by more than 8 % (not a standing pose): the pelvis ratio uses the legs"
                       : "; agrees with the pelvis");
}

// Where Sam's hidden joints are against the body's (model space; both stand on the same world transform
// in follow mode 1), sampled every tick while the self body is shown, mean / max over a second: neck 30,
// right hand 9, left hand 38, right foot 63 (what the user sees: the gear and boots on Sam's skeleton
// against the drawn body), and the head 31 (with the lift: about the lift).
constexpr unsigned kFitMeterJ[5] = {30, 9, 38, 63, 31};
double g_fit_d_sum[5] = {}, g_fit_d_max[5] = {};
unsigned g_fit_d_n = 0;
void FitSample() {
    if (!g_self_shown || g_me.phase != Phase::live) return;
    const std::uintptr_t sam_e = g_sam_e.load();
    if (!sam_e) return;
    double d[5];
    for (unsigned i = 0; i < 5; ++i) {
        float a[4][4], b[4][4];
        const unsigned sj = kFitMeterJ[i];
        if (!ReadJointModel(reinterpret_cast<game::Entity*>(sam_e), sj, a) || !ReadJointModel(g_me.spawn.e, g_me.inv[sj], b)) return;
        const double dx = a[3][0] - b[3][0], dy = a[3][1] - b[3][1], dz = a[3][2] - b[3][2];
        d[i] = std::sqrt(dx * dx + dy * dy + dz * dz) * 100.0;
        if (!std::isfinite(d[i])) return;
    }
    for (unsigned i = 0; i < 5; ++i) {
        g_fit_d_sum[i] += d[i];
        if (d[i] > g_fit_d_max[i]) g_fit_d_max[i] = d[i];
    }
    ++g_fit_d_n;
}

// The fit's counters (section 16) and the meter above.
void FitStats() {
    static std::uint32_t pw = 0, ph = 0, pj = 0, pf = 0, pr = 0, pwpm = 0, pt = 0, ps = 0, pq = 0, pss = 0, psu = 0, psf = 0;
    static std::uint64_t ppel[2] = {}, ppn[2] = {};
    static std::uint32_t pto[2] = {}, plu[2] = {};
    const std::uint32_t w = g_fit_writes.load(), h = g_fit_updates_held.load(), jh = g_fit_joints_held.load(), f = g_fit_updates_fresh.load(),
                        r = g_fit_restores.load(), wpm = g_fit_wpm.load(), t = g_fit_torn.load(), sk = g_fit_skips.load(), q = g_fit_qheld.load(),
                        ss = g_fit_sheld.load(), su = g_fit_sync_used.load(), sf = g_fit_sync_fallback.load();
    std::uint64_t pel[2], pn[2];
    for (int i = 0; i < 2; ++i) pel[i] = g_fit_pel_sum[i].load(), pn[i] = g_fit_pel_n[i].load();
    char dist[220] = "";
    if (g_fit_d_n) {
        int n = 0;
        static const char* const kName[5] = {"neck", "R hand", "L hand", "R foot", "head"};
        for (unsigned i = 0; i < 5 && n >= 0 && n < static_cast<int>(sizeof(dist)); ++i)
            n += std::snprintf(dist + n, sizeof(dist) - n, " %s %.1f/%.1f", kName[i], g_fit_d_sum[i] / g_fit_d_n, g_fit_d_max[i]);
    }
    // the leak check by bucket: joints nearer the shape per update, the largest change, which joints
    char leak[200] = "";
    std::uint32_t to[2], lu[2];
    for (int i = 0; i < 2; ++i) {
        to[i] = g_fit_toward[i].load(), lu[i] = g_fit_leak_upd[i].load();
        const std::uint32_t du = lu[i] - plu[i];
        const std::uint64_t b0 = g_fit_toward_bits[i][0].exchange(0), b1 = g_fit_toward_bits[i][1].exchange(0);
        char joints[64] = "";
        int jn = 0;
        for (unsigned j = 0; j < kJ && jn >= 0 && jn < static_cast<int>(sizeof(joints)) - 4; ++j)
            if ((j < 64 ? b0 >> j : b1 >> (j - 64)) & 1) jn += std::snprintf(joints + jn, sizeof(joints) - jn, "%s%u", jn ? "," : "", j);
        const std::size_t used = std::strlen(leak);
        std::snprintf(leak + used, sizeof(leak) - used, " %s %.2f/update over %u (largest change %.1f mm, joints %s)", i ? "on" : "off",
                      du ? static_cast<double>(to[i] - pto[i]) / du : 0.0, du, g_fit_dev_max_um[i].exchange(0) / 1000.0, joints[0] ? joints : "-");
    }
    char pels[96] = "";
    for (int i = 0; i < 2; ++i) {
        const std::uint64_t dn = pn[i] - ppn[i];
        const std::size_t used = std::strlen(pels);
        std::snprintf(pels + used, sizeof(pels) - used, " %s %s", i ? "on" : "off",
                      dn ? std::to_string(static_cast<double>(pel[i] - ppel[i]) / dn / 1e4).substr(0, 6).c_str() : "-");
    }
    // the per-item headgear offsets (section 26.3): caps cap00-03, then glasses eyewear0, glasses00-02, x/forward/up mm each
    char hgc[96] = "", hgg[96] = "";
    for (int i = 0; i < kHeadItems; ++i) {
        char* o = i >= kHeadItemFirstCap ? hgc : hgg;
        const std::size_t used = std::strlen(o);
        std::snprintf(o + used, 96 - used, "%s%d/%d/%d", used ? "," : "", g_hg_mm[i][0].load(), g_hg_mm[i][1].load(), g_hg_mm[i][2].load());
    }
    if (w != pw || r != pr || g_fit_want.load() || lu[0] != plu[0])
        Log("[self] fit %s: writes=%u same-pose=%u (newest %u) updates game-rewrote=%u held=%u (t joints %u, q %u, s %u) restores=%u wpm=%u torn=%u "
            "busy=%u faults=%u; leak check (translations nearer the shape than Sam's own):%s, max %u per update; captured pelvis cm%s; "
            "head lift %d%% (measured %.1f cm, check %.2f mm, state %d); headgear %s row %d cap %s glasses %s mm; feet %s %d%% boots %s "
            "writes %u; Sam vs body cm mean/max over %u ticks:%s",
            g_fit_want.load() ? "on" : "off", w - pw, su - psu, sf - psf, f - pf, h - ph, jh - pj, q - pq, ss - pss, r - pr, wpm - pwpm, t - pt, sk - ps,
            g_fit_faults.load(), leak, g_fit_toward_max.exchange(0), pels, EffectiveLiftPct(),
            static_cast<double>(g_lift_cm.load()), static_cast<double>(g_lift_err_mm.load()), g_lift_diag.load(), g_hg_cfg.load() ? "on" : "off",
            g_hg_row.load(), hgc, hgg, g_feet_cfg.load() ? "on" : "off", g_feet_pct.load(), g_feet_boots.load() ? "shown" : "not shown", g_feet_writes.load(),
            g_fit_d_n, dist[0] ? dist : " -");
    pw = w, ph = h, pj = jh, pf = f, pr = r, pwpm = wpm, pt = t, ps = sk, pq = q, pss = ss, psu = su, psf = sf;
    for (int i = 0; i < 2; ++i) ppel[i] = pel[i], ppn[i] = pn[i], pto[i] = to[i], plu[i] = lu[i];
    for (unsigned i = 0; i < 5; ++i) g_fit_d_sum[i] = 0, g_fit_d_max[i] = 0;
    g_fit_d_n = 0;
}

void SelfStats(double now) {
    LogLatch(g_me);
    FitSample();
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
    std::uint64_t gmask = 0;
    if (const std::uintptr_t a = SamMaskAddr(sam_e)) Rd(a, mask);
    if (const std::uintptr_t a = SamHideAddr(sam_e)) Rd(a, gmask);
    if (sam_e) partner::ReadFlags(reinterpret_cast<game::Entity*>(sam_e), &sam_flags);
    std::int32_t scene = -1;
    if (g_mesh.scene_hide) Rd(g_mesh.scene_hide, scene);
    const std::uint32_t fz = g_me.forced - g_me.prev_forced, sk = g_me.forced_skipped - g_me.prev_skipped;
    g_me.prev_forced = g_me.forced, g_me.prev_skipped = g_me.forced_skipped;
    Log("[self] shown=%d '%s' app=%u obs=%u copies=%u (failed %u, aba %u) tick-moves=%u pose age avg %.2f max %.2f ms stale=%u "
        "body-Sam distance avg %.1f max %.1f cm; Sam's Root mask 0x%llX global mask 0x%llX flags 0x%llX scene-hide %d repairs %u; rej=%u(last %d) aba=%u "
        "nocap=%u wpm=%u faults=%u forced=%u game-drove=%u",
        g_self_shown ? 1 : 0, g_me.body ? g_me.body->key : "?", app - prev_app, obs - prev_obs, copies - prev_copies, fail - prev_fail,
        caba - prev_caba, g_self_tick_moves - prev_moves, n ? static_cast<double>(us) / static_cast<double>(n) / 1000.0 : 0.0,
        static_cast<double>(mx) / 1000.0, stale - prev_stale, g_self_dist_n ? g_self_dist_sum / g_self_dist_n * 100.0 : 0.0,
        g_self_dist_max * 100.0, static_cast<unsigned long long>(mask), static_cast<unsigned long long>(gmask), static_cast<unsigned long long>(sam_flags), scene, g_self_repairs,
        rej - prev_rej, g_me.ct.last_reject.load(), aba - prev_aba, nocap - prev_nocap, wpm - prev_wpm, g_me.ct.faults.load(), fz, sk);
    prev_app = app, prev_copies = copies, prev_fail = fail, prev_stale = stale, prev_rej = rej, prev_obs = obs, prev_moves = g_self_tick_moves;
    prev_aba = aba, prev_nocap = nocap, prev_wpm = wpm, prev_caba = caba;
    // Follow mode 4 (section 26): the body's updates that got this frame's predicted world (of them seated), those after Sam's
    // update (his world as is), the skips, the step put in (cm), its dt scale, and how far Sam's world moved between his update
    // and the body's next one (0: nothing moves him outside his update; the prediction's premise).
    {
        static std::uint32_t pp = 0, pa = 0, ph = 0, ps = 0, pj = 0, pr = 0, pf = 0, pab = 0, psn = 0, pmv = 0, prd = 0, pse = 0, pg = 0;
        static std::uint64_t pum = 0, ppre = 0, ppren = 0, pss = 0, pk = 0;
        const std::uint32_t pn = g_pred_n.load(), an = g_pred_after.load(), hn = g_pred_skip_hist.load(), sn = g_pred_skip_stale.load(),
                            jn = g_pred_skip_jump.load(), rn = g_pred_skip_read.load(), fn = g_pred_fail.load(), abn = g_pred_aba.load(),
                            scn = g_pred_scale_n.load(), mvn = g_pred_skip_moved.load(), rdn = g_pred_skip_ride.load(), sen = g_pred_seated.load(),
                            gn = g_pred_skip_gate.load();
        const std::uint64_t um = g_pred_step_um.load(), pre = g_pred_pre_um.load(), pren = g_pred_pre_n.load(), ss = g_pred_scale_sum.load(),
                            kk = g_pred_k_sum.load();
        const int follow = g_self_follow.load();
        if (follow == 4 || pn != pp || an != pa) {
            const std::uint32_t dn = pn - pp, dsn = scn - psn;
            const std::uint64_t dpre = pren - ppren;
            Log("[self] follow %d (rule %s): predicted %u (seated %u), after Sam %u, skipped %u (no history %u, stale %u, jump %u, read %u, moved %u, "
                "ride %u, gate %u), failed %u, aba %u; step cm mean %.1f max %.1f, scale %.2f; Sam's frame span ms %.1f; Sam moved outside his update "
                "cm %.2f; far hips %u (rule %s)",
                follow, g_pred_rule.load() ? "v0.20" : "v0.19", dn, sen - pse, an - pa,
                (hn - ph) + (sn - ps) + (jn - pj) + (rn - pr) + (mvn - pmv) + (rdn - prd) + (gn - pg), hn - ph, sn - ps, jn - pj, rn - pr, mvn - pmv,
                rdn - prd, gn - pg, fn - pf, abn - pab, dn ? static_cast<double>(um - pum) / dn / 1e4 : 0.0,
                static_cast<double>(g_pred_step_max_um.exchange(0)) / 1e4, dsn ? static_cast<double>(kk - pk) / dsn / 1000.0 : 0.0,
                dsn ? static_cast<double>(ss - pss) / dsn / 1000.0 : 0.0, dpre ? static_cast<double>(pre - ppre) / dpre / 1e4 : 0.0,
                g_far_hips_n.exchange(0), g_far_hips_rule.load() ? "on" : "off");
        }
        pp = pn, pa = an, ph = hn, ps = sn, pj = jn, pr = rn, pf = fn, pab = abn, psn = scn, pmv = mvn, prd = rdn, pum = um, ppre = pre,
        ppren = pren, pss = ss, pse = sen, pg = gn, pk = kk;
        // The meter (section 26.6): the body against Sam's world after the frame, moving frames, per class: the v0.20 rule, v0.19's,
        // and the plain copy (cm mean / max, frames over 3 and 8 cm, degrees mean / max).
        if (!g_pm_busy.exchange(true, std::memory_order_acquire)) {
            const PredMeter pmv2 = g_pm;
            g_pm = PredMeter{};
            g_pm_busy.store(false, std::memory_order_release);
            static const char* const kCls[kPmCls] = {"on foot", "seated", "carried"};
            static const char* const kCand[kPmCand] = {"v0.20", "v0.19", "copy"};
            for (int c = 0; c < kPmCls; ++c) {
                const PredMeterCls& q = pmv2.c[c];
                if (!q.n) continue;
                char txt[900];
                std::size_t o = 0;
                for (int k = 0; k < kPmCand && o + 140 < sizeof(txt); ++k)
                    o += static_cast<std::size_t>(std::snprintf(txt + o, sizeof(txt) - o, "%s%s %.1f/%.1f cm (>3 cm %u, >8 cm %u) %.2f/%.1f deg", k ? "; " : "",
                                                                kCand[k], q.sum[k] / q.n, q.mx[k], q.o3[k], q.o8[k], q.asum[k] / q.n, q.amx[k]));
                if (c == 2 && o + 200 < sizeof(txt))
                    o += static_cast<std::size_t>(std::snprintf(txt + o, sizeof(txt) - o,
                                                                " | vs his own update's world: v0.20 %.1f/%.1f cm; the carry moved him after it %.1f/%.1f cm",
                                                                q.own_sum[0] / q.n, q.own_mx[0], q.moved_sum / q.n, q.moved_mx));
                Log("[self] follow 4 meter %s, %u moving frames, body vs Sam after the frame: %s", kCls[c], q.n, txt);
            }
        }
        // The drawn meter (section 38.2): her drawn root per candidate against vanilla Sam's drawn root, in seconds where Sam's
        // world or his drawn root moved: cm mean / max and the jitter mean / max; his steps; his root model row against the local.
        if (!g_dm_busy.exchange(true, std::memory_order_acquire)) {
            const DrawnMeter q = g_dm;
            g_dm = DrawnMeter{};
            g_dm_busy.store(false, std::memory_order_release);
            if (q.n && (q.wstep_mx > 0.5 || q.rstep_mx > 0.5)) {
                static const char* const kCand[kPmCand] = {"v0.20", "v0.19", "copy"};
                const int applied = g_pred_rule.load() ? 0 : 1;
                char txt[600];
                std::size_t o = 0;
                for (int k = 0; k < kPmCand && o + 120 < sizeof(txt); ++k) {
                    o += static_cast<std::size_t>(std::snprintf(txt + o, sizeof(txt) - o, "%s%s%s %.1f/%.1f cm jitter %.1f/%.1f", k ? "; " : "", kCand[k],
                                                                k == applied ? " (applied)" : "", q.sum[k] / q.n, q.mx[k], q.jn ? q.jsum[k] / q.jn : 0.0,
                                                                q.jmx[k]));
                }
                Log("[self] follow 4 drawn root vs Sam's own (his world + his root joint), %u frames: %s | Sam's step max: world %.1f cm, his drawn "
                    "root %.1f; his root model row vs local max %.1f cm",
                    q.n, txt, q.wstep_mx, q.rstep_mx, q.stale_mx);
            }
        }
        // The trace (`self_view 23 1`): the frames around a snap or a re-anchor
        {
            static std::uint32_t seen = 0;
            const std::uint32_t sq = g_tr_seq.load(std::memory_order_acquire);
            static thread_local TraceBlock tb;
            if (sq != seen && g_tr_ready.Read(tb)) {
                seen = sq;
                Log("[self] trace #%u: %d frames around a move over 0.5 m of Sam's world or root joint (row %d); cm, world m", sq, tb.n, tb.trig);
                const double t0 = tb.trig < tb.n ? tb.r[tb.trig].ms : 0;
                for (int i = 0; i < tb.n && i < kTrRows; ++i) {
                    const TraceRow& r = tb.r[i];
                    Log("[self] trace %+7.1f ms: Sam world (%.3f, %.3f, %.3f) root (%.1f, %.1f, %.1f) model %s(%.1f, %.1f, %.1f) | her world (%.3f, %.3f, "
                        "%.3f) root (%.1f, %.1f, %.1f) %s | her drawn root vs his: v0.20 %.1f, copy %.1f",
                        r.ms - t0, r.sw[0], r.sw[1], r.sw[2], r.srl[0] * 100.0, r.srl[1] * 100.0, r.srl[2] * 100.0, r.smok ? "" : "? ",
                        r.sm0[0] * 100.0, r.sm0[1] * 100.0, r.sm0[2] * 100.0, r.bw[0], r.bw[1], r.bw[2], r.brl[0] * 100.0, r.brl[1] * 100.0,
                        r.brl[2] * 100.0, selfpred::WhyName(static_cast<selfpred::Why>(r.why)), r.e[0], r.e[2]);
                }
            }
        }
    }
    FitStats();
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

// Gear pose targets the game does not update by itself (a prop standing still): the forced model
// update, as DriveModel does for a body, only while the game has not updated it for 100 ms.
void DriveBootsTarget(bool local_active) {
    const std::uintptr_t sm = g_bt.sm.load(std::memory_order_acquire);
    if (!local_active || !sm || !g_fn_forced || g_bt.forced_off) return;
    if (g_bt.game_updates.exchange(0, std::memory_order_relaxed) != 0) g_bt.last_game_ms = GetTickCount64();
    if (g_bt.last_game_ms && GetTickCount64() - g_bt.last_game_ms < 100) return;
    const std::uintptr_t te = g_bt.e.load();
    if (!holo::SpawnAlive(g_bt.spawn) || reinterpret_cast<std::uintptr_t>(g_bt.spawn.e) != te) return;
    std::uintptr_t id[2] = {};
    if (!mem::Read(sm, id, sizeof(id)) || id[0] != g_sm_vt || Q(sm + kSmEntity) != te || Q(te) != g_bt.e_vt.load()) return;
    t_forcing = true;
    const bool ok = CallForced(sm);
    t_forcing = false;
    if (!ok) {
        g_bt.forced_off = true;
        Log("[boots] forced model update of the boots prop FAULTED; not called again this run");
        return;
    }
    ++g_bt.forced;
}

// The exo carriers the game does not update by itself (a cutscene body standing still): the forced model update, as for
// the boots prop.
void DriveExoTargets(bool local_active) {
    for (ExoTarget& t : g_xt) {
        const std::uintptr_t sm = t.sm.load(std::memory_order_acquire);
        if (!local_active || !sm || !g_fn_forced || t.forced_off) continue;
        if (t.game_updates.exchange(0, std::memory_order_relaxed) != 0) t.last_game_ms = GetTickCount64();
        if (t.last_game_ms && GetTickCount64() - t.last_game_ms < 100) continue;
        // a hidden carrier (its body hidden for a load or the self view's unshow, the grace after unequipping) is not posed:
        // nothing draws it, and those windows (a load) must stay as fast as vanilla
        if (!t.want_pose.load(std::memory_order_relaxed)) {
            ++t.forced_skipped;
            continue;
        }
        const std::uintptr_t te = t.e.load();
        if (!holo::SpawnAlive(t.spawn) || reinterpret_cast<std::uintptr_t>(t.spawn.e) != te) continue;
        std::uintptr_t id[2] = {};
        if (!mem::Read(sm, id, sizeof(id)) || id[0] != g_sm_vt || Q(sm + kSmEntity) != te || Q(te) != t.e_vt.load()) continue;
        t_forcing = true;
        const bool ok = CallForced(sm);
        t_forcing = false;
        if (!ok) {
            t.forced_off = true;
            Log("[exo] forced model update of the exo carrier FAULTED; not called again this run");
            continue;
        }
        ++t.forced;
    }
}

void DrivePoseTargets(bool local_active) {
    if (!local_active || !g_fn_forced || g_pt_forced_off || g_pt_faulted.load() || !g_pt_live.load(std::memory_order_acquire)) return;
    for (PoseTarget& t : g_pt) {
        const std::uintptr_t sm = t.sm.load(std::memory_order_acquire);
        if (!sm) continue;
        if (t.game_updates.exchange(0, std::memory_order_relaxed) != 0) t.last_game_ms = GetTickCount64();
        if (t.last_game_ms && GetTickCount64() - t.last_game_ms < 100) continue;
        // This runs before the gear tick notices a prop the game destroyed (player_sync: realbody::Tick
        // before gear::Tick): the prop's spawn is checked here too, as DriveModel checks a body's.
        const std::uintptr_t te = t.e.load();
        if (!holo::SpawnAlive(t.spawn) || reinterpret_cast<std::uintptr_t>(t.spawn.e) != te) continue;
        std::uintptr_t id[2] = {};
        if (!mem::Read(sm, id, sizeof(id)) || id[0] != g_sm_vt || Q(sm + kSmEntity) != te || Q(te) != t.e_vt.load()) continue;
        t_forcing = true;
        const bool ok = CallForced(sm);
        t_forcing = false;
        if (!ok) {
            g_pt_forced_off = true;
            Log("[gear] forced model update of a gear prop FAULTED; not called again this run");
            return;
        }
        ++t.forced;
    }
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

bool IsFragileSuit(const bodies::Body* b) { return b && std::strcmp(b->key, "fragile_suit") == 0; }

// Game thread: Fragile's porter suit in the suit colour of the player she stands for (docs section 27): the partner
// slot's fragile_suit body wears the colour the other player sent (STJ1 outfit: that player's own Sam's suit colour),
// the self view's wears our own Sam's colour, so both screens show the same. Only while our player is up, 0.5 s after the
// body became live; suit_colour rate-limits, waits for a change to hold and logs every step.
// Round 6 T2: one body's suit from the edited textures when `look` has any (taking the v0.23 tint off first), else the tint
// (fragile_suit_colour=1) or her own suit. `have_look`: false while the look is not known yet (the worker rebuilds it
// after an edit or a boots change): nothing changes meanwhile, the bound look stays on until the new one is ready.
// `tint`: the v0.23 tint for an empty look (-1 this game's fragile_suit_colour, 0 off, 1 on: the guest's choice, LOOK bit 1).
void DressOne(ftex::Dress& td, suitcol::Dress& sd, std::uintptr_t e, bool have_look, const ftex::Look& look, std::uint8_t colour, double now_ms,
              int tint = -1) {
    if (ftex::Enabled() && !have_look) {
        ftex::Keep(td, e, now_ms);
        return;
    }
    if (ftex::Enabled() && !look.Empty()) {
        if (sd.nh) return suitcol::Undo(sd, "edited textures");
        ftex::Apply(td, e, look, now_ms);
        return;
    }
    if (td.nh || td.bound) {
        ftex::Apply(td, e, ftex::Look{}, now_ms);  // takes ours off first (one step), then the tint may apply
        if (td.nh) return;
    }
    suitcol::Apply(sd, e, colour, now_ms, tint);
}

// Round 6 T3 (section 30): the boots look she wears: our Sam's boot type's folder, worn (a broken or loose-sole part shows)
// and the gold / silver suit (Gold\ / Silver\ inside it). -1 while the type is unknown (no boots pictures then).
int OwnBootsKey(int colour, int* type_out, bool* worn_out, bool* own_out) {
    int type = -1;
    std::uint32_t visible = 0;
    bool own = true;
    gear::OwnBoots(&type, &visible, &own);
    const int forced = ftex::ForcedBoots();
    if (forced >= 0) type = forced;
    const bool worn = bootslook::Worn(visible);
    *type_out = type, *worn_out = worn, *own_out = own;
    return bootslook::Key(bootslook::FolderForType(type), worn, bootslook::MetalOfColour(colour));
}

// The guest's own look for colour c and her boots (the test pattern while tex 9 is on). False while the worker has not
// settled.
bool OwnTextureLook(int c, ftex::Look* l) {
    if (!ftex::Enabled()) return false;
    const int forced = ftex::ForcedColour();
    if (forced >= 0) c = forced;
    int type = -1;
    bool worn = false, own = true;
    const int bk = OwnBootsKey(c, &type, &worn, &own);
    ftex::looks::SetWantedColour(c);
    ftex::looks::SetWantedBoots(bk);
    static int logged = -2;
    if (bk != logged) {
        logged = bk;
        Log("[tex] her boots: type %d%s (look key %d, boots_look=%s)", type, worn ? ", worn" : "", bk, own ? "own" : "prop");
    }
    if (ftex::PatternOn()) return ftex::looks::PatternLook(l);
    return ftex::looks::OwnLook(c, bk, l);
}

void DressFragile(game::Entity* local, bool local_active, double now_ms) {
    if (!local || !local_active) return;
    ftex::NoteGameThread();
    ftex::FreeTick(now_ms, coop::WorldReady(nullptr) || !coop::Active());  // the local player is up here
    g_tex_p.who = "partner";
    g_tex_me.who = "self";
    if (g_p.phase == Phase::live && IsFragileSuit(g_p.body) && GetTickCount64() - g_p.live_tick >= 500 && holo::SpawnAlive(g_p.spawn)) {
        std::uint8_t c = 0xFF;
        wire::Outfit o;
        if (g_p.owner == Owner::partner && g_have_partner_outfit) c = g_partner_outfit.suit_colour;
        else if (g_p.owner == Owner::debug && outfit::ReadLocal(reinterpret_cast<std::uintptr_t>(local), &o)) c = o.suit_colour;
        ftex::Look l;
        bool have = false;
        int tint = -1;
        if (g_p.owner == Owner::partner) {
            have = texshare::PartnerLook(&l);  // the guest's look over the link (host side)
            tint = texshare::PartnerTint();    // and the guest's own fragile_suit_colour choice (-1: none received)
        } else if (c != 0xFF) {
            g_tex_p.who = "debug";
            have = OwnTextureLook(suitcol::Wanted(g_suit_override, c), &l);
        }
        if (c != 0xFF || g_suit_override >= 0) {
            DressOne(g_tex_p, g_suit_p, reinterpret_cast<std::uintptr_t>(g_p.spawn.e), have, l, suitcol::Wanted(g_suit_override, c), now_ms, tint);
            const bool bound = ftex::Enabled() && have && !l.Empty();
            // (the partner's look: the colour it was made for, which the guest picks apart from her Sam's suit colour c; the
            // painted row itself comes over STC1)
            const int prow = l.colour >= 0 && l.colour <= 16 ? l.colour : suitcol::Wanted(g_suit_override, c);
            g_hood_row[0] = suitcol::HoodRow(bound, g_p.owner == Owner::partner ? prow : l.hood_row, g_suit_p.applied);
            g_hood_row_look[0] = bound;
        }
    }
    if (g_me.phase == Phase::live && IsFragileSuit(g_me.body) && GetTickCount64() - g_me.live_tick >= 500 && holo::SpawnAlive(g_me.spawn)) {
        wire::Outfit o;
        const bool have = outfit::ReadLocal(reinterpret_cast<std::uintptr_t>(local), &o);
        // round 5: on a host-held save guest, her remembered pick (suit_colour.hpp GuestColour) when she has not picked since this load
        if (have || g_suit_override >= 0) {
            const std::uint8_t worn = suitcol::Wanted(g_suit_override, have ? suitcol::GuestColour(o.suit_colour) : 0);
            // Round 15 (section 40): in the wardrobe her self body shows the colour being previewed (only here: the host,
            // the shared look, her boots and her hood row stay on what she wears)
            const bool in_wardrobe = g_suit_override < 0 && g_menu_active && g_menu_wardrobe && g_wd.open == 1;
            const std::uint8_t colour = suitcol::WardrobeShown(in_wardrobe, wardrobe::PreviewColour(g_wd), g_wd.staging, worn);
            const std::uint8_t shown_now = in_wardrobe ? colour : std::uint8_t{0xFF};
            if (shown_now != g_wd_shown) {
                if (shown_now != 0xFF)
                    Log("[suit] wardrobe: her self body shows colour %u (%s; she wears %u)", colour,
                        wardrobe::PreviewColour(g_wd) >= 0 ? "previewed" : colour == worn ? "what she wears" : "the menu's choice", worn);
                else if (g_wd_shown != 0xFF) Log("[suit] wardrobe: her self body back on what she wears (%u)", worn);
                g_wd_shown = shown_now;
            }
            ftex::Look l;
            const bool have_look = OwnTextureLook(colour, &l);
            g_self_has_look = have_look && !l.Empty();
            if (colour == worn) {
                texshare::SetOwnLook(have_look, l, colour);
            } else if (ftex::Enabled() && !ftex::PatternOn()) {
                int wt = -1;
                bool ww = false, wo = true;
                ftex::Look wl;
                if (ftex::looks::OwnLook(worn, OwnBootsKey(worn, &wt, &ww, &wo), &wl)) texshare::SetOwnLook(true, wl, worn);
            }
            int btype = -1;
            bool bworn = false, bown = true;
            OwnBootsKey(worn, &btype, &bworn, &bown);
            texshare::SetOwnBoots(btype, bworn, bown);
            DressOne(g_tex_me, g_suit_me, reinterpret_cast<std::uintptr_t>(g_me.spawn.e), have_look, l, colour, now_ms);
            if (colour == worn) {
                g_hood_row[1] = have_look || !ftex::Enabled() ? suitcol::HoodRow(ftex::Enabled() && have_look && !l.Empty(), l.hood_row, g_suit_me.applied) : -1;
                g_hood_row_look[1] = ftex::Enabled() && have_look && !l.Empty();
            }
        }
    }
}

// ---------------------------------------------------------------- round 15 (section 40): the wardrobe
// Every tick: the uniform menu's state (wardrobe.hpp; logged on every change, hidden-test evidence), then the guest's pick
// rule and her Sam's byte (suit_colour.hpp GuestTick).
void WardrobeTick(game::Entity* local, bool local_active, double now_ms) {
    const bool wr = wardrobe::Read(&g_wd);
    menufig::View v;
    const bool vr = menufig::Read(&v) || v.w;
    const int kind6 = !vr ? -1 : (v.kind == 6 || v.kind_next == 6) ? 1 : 0;
    std::uintptr_t addr = 0;
    std::uint8_t b = 0xFF;
    const bool have_b = local && outfit::OwnSuitColour(reinterpret_cast<std::uintptr_t>(local), &addr, &b);
    const int byte_now = have_b ? b : -1;
    static wardrobe::State last;
    static int last_b = -2, last_kind = -2, last_next = -2;
    if (wr && (g_wd.open != last.open || g_wd.category != last.category || g_wd.value != last.value || g_wd.staging != last.staging ||
               byte_now != last_b || (g_wd.open == 1 && (v.kind != last_kind || v.kind_next != last_next)))) {
        // (her Sam's byte alone changing outside the menu is logged too: a load, a script, our alignment)
        Log("[wardrobe] menu %s, category %d, previewed %d, staging %d; Sam's suit colour byte %d; subspace kind %d/%d state %d",
            g_wd.open == 1 ? "OPEN" : "closed", g_wd.category, g_wd.value, g_wd.staging, byte_now, v.kind, v.kind_next, v.state);
        last = g_wd;
        last_b = byte_now, last_kind = v.kind, last_next = v.kind_next;
    }
    suitcol::GuestTick(local && local_active ? reinterpret_cast<std::uintptr_t>(local) : 0, addr, byte_now, kind6, now_ms);
}

// ---------------------------------------------------------------- update frequency (smooth bit kSmoothBoost)
// The game updates an entity at the rate its resource asks for (EUpdateFrequency, entity +0x204: 3 =
// 59.94 Hz for every cutscene body and gear prop we spawn, 6 = 240 Hz for the player). Above 60 fps
// the bodies would repeat a pose every other frame. The game's own setter (the one it uses to boost
// carried cargo to 240 Hz, 0x142CBF860, and on the player, 0x143740100) moves our entities to the
// 240 Hz group, so their models are updated (and posed by the detour) every frame up to 240 fps.
// Only entities we spawned; a fault turns it off for the session.
constexpr std::uintptr_t kSetUpdateFrequency = 0x235F5A0;  // void Entity::SetUpdateFrequency(Entity*, u8 idx)
constexpr std::uintptr_t kSchedRegroup = 0x23FC0E0;        // (World+0x130, Entity*): new interval, move to its group
constexpr std::uintptr_t kFreqLock = 0x7BC4828;            // the global SRW lock the setter takes
constexpr std::uintptr_t kWorldGlobal = 0x7BC4E70;         // World* (scheduler at +0x130)
constexpr std::uintptr_t kEntFreq = 0x204, kEntGroup = 0x20A;
constexpr std::uint8_t kFreq240Hz = 6;
const char* const kFreqNames[7] = {"9.99 Hz", "14.99 Hz", "29.97 Hz", "59.94 Hz", "120 Hz", "144 Hz", "240 Hz"};
const char* FreqName(std::uint8_t f) { return f < 7 ? kFreqNames[f] : "?"; }

struct FreqState {
    bool checked = false, ok = false, off = false;
    char why[160] = "not checked";
    std::uintptr_t fn = 0;
    std::uint32_t calls = 0, faults = 0;
} g_freq;

bool CheckFreqSites() {
    if (g_freq.checked) return g_freq.ok;
    g_freq.checked = true;
    const std::uintptr_t b = mem::Base();
    bool ok = Check("Entity SetUpdateFrequency", kSetUpdateFrequency,
                    "48 89 5C 24 08 57 48 83 EC 20 0F B6 FA 48 8B D9 38 91 04 02 00 00 74 4D 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? "
                    "84 C0 75 0D 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 40 88 BB 04 02 00 00 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 0F 48 "
                    "81 C1 30 01 00 00 48 8B D3 E8",
                    g_freq.why, sizeof(g_freq.why)) &&
              Check("scheduler regroup", kSchedRegroup, "48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC 20 66 83 BA 0A 02 00 00 00", g_freq.why,
                    sizeof(g_freq.why));
    if (ok && (mem::RipTarget(b + kSetUpdateFrequency + 0x18, 3, 7) != b + kFreqLock ||
               mem::RipTarget(b + kSetUpdateFrequency + 0x3D, 3, 7) != b + kWorldGlobal ||
               mem::RipTarget(b + kSetUpdateFrequency + 0x53, 1, 5) != b + kSchedRegroup)) {
        std::snprintf(g_freq.why, sizeof(g_freq.why), "SetUpdateFrequency's lock / World / regroup references do not lead where expected");
        ok = false;
    }
    if (ok) g_freq.fn = b + kSetUpdateFrequency;
    Log("[smooth] update-frequency setter %s", ok ? "verified (ds.exe+0x235F5A0: lock, World, regroup call)" : g_freq.why);
    g_freq.ok = ok;
    return ok;
}

bool CallSetFreq(std::uintptr_t e, std::uint8_t idx) {
    using Fn = void (*)(void*, std::uint8_t);
    __try {
        reinterpret_cast<Fn>(g_freq.fn)(reinterpret_cast<void*>(e), idx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Sets `e`'s update frequency to `want` when it differs (fault-free read first). 0 = unchanged / done,
// 1 = changed, -1 = refused or faulted. `tag` / `what` only for the log line.
int SetFreq(std::uintptr_t e, std::uint8_t want, const char* tag, const char* what, bool log) {
    std::uint8_t cur = 0;
    std::int16_t g0 = -1;
    if (!e || !Rd(e + kEntFreq, cur) || !Rd(e + kEntGroup, g0)) return -1;
    if (cur == want) return 0;
    if (g_freq.off || !CheckFreqSites()) return -1;
    if (cur > 6 || want > 6) return -1;
    ++g_freq.calls;
    if (!CallSetFreq(e, want)) {
        ++g_freq.faults;
        g_freq.off = true;
        Log("[smooth] SetUpdateFrequency FAULTED on %s %p: the boost is off for this session", what, reinterpret_cast<void*>(e));
        return -1;
    }
    std::uint8_t after = 0xFF;
    std::int16_t g1 = -1;
    Rd(e + kEntFreq, after);
    Rd(e + kEntGroup, g1);
    if (log)
        Log("[%s] %s update frequency %u (%s) -> %u (%s), scheduler group %d -> %d", tag, what, cur, FreqName(cur), after, FreqName(after),
            g0, g1);
    return after == want ? 1 : -1;
}

// Game thread: a live body at 240 Hz while the boost bit is set, else back at its own frequency.
void DriveFreq(BodySlot& b, bool local_active) {
    if (b.phase != Phase::live || !holo::SpawnAlive(b.spawn) || !local_active) return;
    const auto e = reinterpret_cast<std::uintptr_t>(b.spawn.e);
    if (b.freq_e != e) {
        b.freq_e = e;
        b.freq_orig = 0xFF;
        b.freq_calls = 0;
    }
    std::uint8_t cur = 0;
    if (!Rd(e + kEntFreq, cur)) return;
    if (b.freq_orig == 0xFF) b.freq_orig = cur;
    const std::uint8_t want = (g_smooth.load(std::memory_order_relaxed) & kSmoothBoost) && !g_freq.off ? kFreq240Hz : b.freq_orig;
    if (cur == want || b.freq_calls >= 8) return;  // a value the game keeps changing back is left alone after 8 tries
    ++b.freq_calls;
    SetFreq(e, want, b.tag, "body", true);
}

// Game thread, every 250 ms: the published gear props of both slots, the same way (their own
// frequency is remembered per prop to put it back).
// An entry not seen in 8 passes (2 s: its prop is gone) is freed (review: the table filled up and the
// "start over" then remembered an already boosted rate as the original). A prop first seen at 240 Hz
// (its entry was lost) goes back to 59.94 Hz, what the cinematic resources ask for.
struct PropFreq {
    std::uintptr_t e = 0;
    game::Guid guid;
    std::uint8_t orig = 0xFF;
    std::uint8_t misses = 0;
};
PropFreq g_prop_freq[2 * kMaxGearProps];
double g_next_prop_freq_ms = 0;
std::uint32_t g_prop_boosts = 0, g_prop_restores = 0;

void DriveGearFreq(bool local_active, double now_ms) {
    if (!local_active || now_ms < g_next_prop_freq_ms) return;
    g_next_prop_freq_ms = now_ms + 250;
    const bool boost = (g_smooth.load(std::memory_order_relaxed) & kSmoothBoost) && !g_freq.off;
    static GearSet set;
    for (PropFreq& p : g_prop_freq)
        if (p.e && p.misses < 255) ++p.misses;  // cleared below for every prop still published
    for (int slot = 0; slot < 2; ++slot) {
        if (g_gear_n[slot].load(std::memory_order_acquire) == 0 || !g_gear[slot].Read(set) || set.n > kMaxGearProps) {
            for (PropFreq& p : g_prop_freq) p.misses = 0;  // a torn or empty read: no conclusions this pass
            continue;
        }
        for (unsigned i = 0; i < set.n; ++i) {
            const GearProp& g = set.p[i];
            if (!g.e) continue;
            std::uintptr_t vt = 0;
            game::Guid id;
            if (!Rd(g.e, vt) || vt != g.e_vt || !Rd(g.e + game::kEntityGuid, id.lo) || !Rd(g.e + game::kEntityGuid + 8, id.hi) || id != g.guid)
                continue;
            PropFreq* pf = nullptr;
            PropFreq* free_pf = nullptr;
            for (PropFreq& p : g_prop_freq) {
                if (p.e == g.e && p.guid == g.guid) pf = &p;
                else if (!free_pf && (p.e == 0 || p.orig == 0xFF || p.misses >= 8)) free_pf = &p;
            }
            std::uint8_t cur = 0;
            if (!Rd(g.e + kEntFreq, cur)) continue;
            if (!pf) {
                if (!free_pf) {  // the table is full of props that may be gone: start over
                    for (PropFreq& p : g_prop_freq) p = PropFreq{};
                    free_pf = &g_prop_freq[0];
                }
                pf = free_pf;
                pf->e = g.e;
                pf->guid = g.guid;
                pf->orig = cur == kFreq240Hz ? std::uint8_t{3} : cur;
            }
            pf->misses = 0;
            const std::uint8_t want = boost ? kFreq240Hz : pf->orig;
            if (cur == want) continue;
            const int r = SetFreq(g.e, want, slot ? "gear-self" : "gear", "prop", g_prop_boosts + g_prop_restores < 6);
            if (r == 1) ++(boost ? g_prop_boosts : g_prop_restores);
            if (g_freq.off) return;
        }
    }
}

// ---------------------------------------------------------------- smoothness meter (tick)
// What the player sees, measured once per tick (one tick per presented frame): four joints (hips,
// both thighs, the right forearm) of each body's current pose and of Sam's newest capture. Per
// second: frames, frames that showed a NEW pose while moving vs repeats (exactly the same pose),
// "stutter" (a step under 10 % of the running mean step), the jerk ratio mean|w_k - w_k-1| / mean w
// of the joints' angular speed w = step / frame time (0 for perfectly even motion; a 60 Hz body at
// 120 fps alternates w and 0: about 1), pose writes per frame (0 / 1 / 2+), and for the partner the
// end-to-end latency, frames held past the newest received one and (echo only, where the replayed
// pose is our own Sam's of echo_delay_s ago) the error against Sam's captured pose at that time.
constexpr unsigned kMeterJ[4] = {1, 61, 66, 8};  // JNT_C_B_001_Hips, JNT_R_B_100_Leg, JNT_L_B_100_Leg, JNT_R_B_002_Arm

struct Series {
    bool have = false;
    wire::Quat prev[4];
    double ema_rate = 0;   // deg/s, running
    double prev_rate = -1;
    // per second
    std::uint32_t frames = 0, moving = 0, repeat = 0, stutter = 0;
    double jerk_sum = 0, rate_sum = 0;
    void Reset() { frames = moving = repeat = stutter = 0, jerk_sum = rate_sum = 0; }
    void Lost() { have = false, prev_rate = -1; }
    void Add(const wire::Quat (&q)[4], double dt_ms) {
        if (!have || dt_ms <= 0.05) {
            std::memcpy(prev, q, sizeof(prev));
            have = true;
            prev_rate = -1;
            return;
        }
        double step = 0;
        for (unsigned k = 0; k < 4; ++k) step += wire::AngleBetween(prev[k], q[k]);
        const bool same = std::memcmp(prev, q, sizeof(prev)) == 0;
        std::memcpy(prev, q, sizeof(prev));
        const double rate = step * 1000.0 / dt_ms;
        ++frames;
        const bool mov = ema_rate > 30.0;  // deg/s over the 4 joints: walking is several hundred
        if (mov) {
            ++moving;
            if (same) ++repeat;
            if (rate < 0.1 * ema_rate) ++stutter;
            if (prev_rate >= 0) {
                jerk_sum += std::fabs(rate - prev_rate);
                rate_sum += rate;
            }
        }
        prev_rate = rate;
        ema_rate += (rate - ema_rate) * 0.1;
    }
    double Jerk() const { return rate_sum > 0 ? jerk_sum / rate_sum : 0; }
};

struct SamHist {  // Sam's captures (echo ground truth): QPC ms + the meter joints
    double ms;
    wire::Quat q[4];
};
constexpr unsigned kHist = 2048;  // > 8 s at 240 Hz
SamHist g_hist[kHist];
unsigned g_hist_head = 0, g_hist_n = 0;
std::uint32_t g_hist_frame = 0;

bool HistAt(double ms, wire::Quat (&out)[4]) {
    if (g_hist_n < 2) return false;
    const SamHist& oldest = g_hist[(g_hist_head + kHist - g_hist_n) % kHist];
    const SamHist& newest = g_hist[(g_hist_head + kHist - 1) % kHist];
    if (ms < oldest.ms || ms > newest.ms) return false;
    for (unsigned i = g_hist_n - 1; i > 0; --i) {
        const SamHist& a = g_hist[(g_hist_head + kHist - g_hist_n + i - 1) % kHist];
        const SamHist& b = g_hist[(g_hist_head + kHist - g_hist_n + i) % kHist];
        if (a.ms <= ms && ms <= b.ms) {
            const float t = b.ms > a.ms ? static_cast<float>((ms - a.ms) / (b.ms - a.ms)) : 1.0f;
            for (unsigned k = 0; k < 4; ++k) {
                wire::Quat qb = b.q[k];
                const wire::Quat& qa = a.q[k];
                if (qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w < 0) qb = wire::Quat{-qb.x, -qb.y, -qb.z, -qb.w};
                out[k] = wire::Normalize(wire::Quat{qa.x + t * (qb.x - qa.x), qa.y + t * (qb.y - qa.y), qa.z + t * (qb.z - qa.z),
                                                    qa.w + t * (qb.w - qa.w)});
            }
            return true;
        }
    }
    return false;
}

// The meter joints of a live body's current pose (Sam-skeleton joints through the body's map).
// Read twice and compared (a write in progress on another thread is skipped).
bool ReadBodyJoints(BodySlot& b, wire::Quat (&q)[4]) {
    const std::uintptr_t sm = b.sm.load(std::memory_order_acquire);
    if (!sm || b.phase != Phase::live || b.latch_state.load(std::memory_order_acquire) != 1) return false;
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0, local = 0;
    if (!ReadPoseHdr(sm, hdr, &pose) || CheckHdr(hdr, b.skel.load(std::memory_order_acquire), &local) != 0) return false;
    for (unsigned k = 0; k < 4; ++k) {
        const std::uintptr_t a = local + sizeof(Joint) * b.inv[kMeterJ[k]];
        float x[4], y[4];
        if (!mem::Read(a, x, sizeof(x)) || !mem::Read(a, y, sizeof(y)) || std::memcmp(x, y, sizeof(x)) != 0) return false;
        q[k] = wire::Quat{x[0], x[1], x[2], x[3]};
    }
    return true;
}

struct SlotMeter {
    Series s;
    std::uint32_t prev_app = 0, prev_upd = 0;
    std::uint32_t per_tick[3] = {};  // ticks with 0, 1, 2+ pose writes
    std::uint32_t upd = 0, app = 0;
    double err_sum = 0, err_max = 0, lat_sum = 0;
    std::uint32_t err_n = 0, lat_n = 0;
    void Reset() {
        s.Reset();
        per_tick[0] = per_tick[1] = per_tick[2] = 0;
        upd = app = 0;
        err_sum = err_max = lat_sum = 0;
        err_n = lat_n = 0;
    }
};
SlotMeter g_meter[2];
Series g_sam_series;
std::uint32_t g_sam_caps = 0, g_prev_cap_frame = 0;
double g_meter_prev_ms = 0, g_meter_next_ms = 0, g_meter_start_ms = 0;
unsigned long g_meter_presents = 0;
std::uint32_t g_meter_ticks = 0;
double g_meter_dt_min = 1e9, g_meter_dt_max = 0;
bool g_meter_echo = false;  // the partner replays our own Sam (echo role): ground truth available
double g_meter_echo_delay_ms = 0;

// Frame sequence recording (smooth 3 <n>): per tick, the frame time and each series' step.
struct SeqRow {
    float dt, step_p, step_s, step_sam;
    std::uint8_t app_p, app_s;
};
constexpr unsigned kSeqMax = 600;
SeqRow g_seq[kSeqMax];
unsigned g_seq_n = 0, g_seq_want = 0;
wire::Quat g_seq_prev[3][4];
bool g_seq_have[3] = {};

float SeqStep(int i, bool ok, const wire::Quat (&q)[4]) {
    if (!ok) {
        g_seq_have[i] = false;
        return -1;
    }
    float st = 0;
    if (g_seq_have[i])
        for (unsigned k = 0; k < 4; ++k) st += wire::AngleBetween(g_seq_prev[i][k], q[k]);
    else st = -1;
    std::memcpy(g_seq_prev[i], q, sizeof(q));
    g_seq_have[i] = true;
    return st;
}

void LogSeq() {
    Log("[smooth] frame sequence: %u frames; per frame: dt ms / partner step / self step / Sam step (deg, 4 joints) / partner writes "
        "/ self writes",
        g_seq_n);
    char line[1024];
    for (unsigned i = 0; i < g_seq_n; i += 12) {
        std::size_t o = 0;
        for (unsigned k = i; k < g_seq_n && k < i + 12 && o + 64 < sizeof(line); ++k) {
            const SeqRow& r = g_seq[k];
            o += static_cast<std::size_t>(std::snprintf(line + o, sizeof(line) - o, " %.1f/%.1f/%.1f/%.1f/%u/%u", r.dt, r.step_p, r.step_s,
                                                        r.step_sam, r.app_p, r.app_s));
        }
        Log("[smooth] seq %3u:%s", i, line);
    }
}

void Meter(double now_ms) {
    if (!g_hook) return;
    const bool any = g_p.phase == Phase::live || g_me.phase == Phase::live;
    const double dt = g_meter_prev_ms > 0 ? now_ms - g_meter_prev_ms : 0;
    g_meter_prev_ms = now_ms;
    if (dt > 0 && dt < 200) {
        const float fr = g_frame_ms.load(std::memory_order_relaxed);
        g_frame_ms.store(fr + (static_cast<float>(dt) - fr) * 0.1f, std::memory_order_relaxed);
    }
    if (!any) {
        g_meter_next_ms = 0;
        g_sam_series.Lost();
        g_meter[0].s.Lost(), g_meter[1].s.Lost();
        return;
    }
    if (g_meter_next_ms == 0) {
        g_meter_next_ms = now_ms + 1000;
        g_meter_start_ms = now_ms;
        g_meter_presents = overlay::Installed() ? overlay::GetStats().presents : 0;
        g_meter_ticks = 0;
        for (SlotMeter& m : g_meter) {
            m.Reset();
            m.prev_app = 0, m.prev_upd = 0;
        }
        g_meter[0].prev_app = g_p.ct.applies.load(), g_meter[0].prev_upd = g_p.updates.load();
        g_meter[1].prev_app = g_me.ct.applies.load(), g_meter[1].prev_upd = g_me.updates.load();
        g_sam_series.Reset();
        g_sam_caps = 0;
    }
    ++g_meter_ticks;
    if (dt > 0) {
        if (dt < g_meter_dt_min) g_meter_dt_min = dt;
        if (dt > g_meter_dt_max) g_meter_dt_max = dt;
    }
    // Sam's newest capture: the reference series and the ground-truth history.
    Capture c;
    wire::Quat samq[4];
    bool sam_ok = false;
    if (g_capture.Read(c) && GetTickCount64() - c.tick_ms < 250) {
        for (unsigned k = 0; k < 4; ++k) samq[k] = wire::Quat{c.j[kMeterJ[k]].q[0], c.j[kMeterJ[k]].q[1], c.j[kMeterJ[k]].q[2], c.j[kMeterJ[k]].q[3]};
        sam_ok = true;
        g_sam_series.Add(samq, dt);
        if (c.frame != g_prev_cap_frame) {
            ++g_sam_caps;
            g_prev_cap_frame = c.frame;
        }
        if (c.frame != g_hist_frame && g_qpc_per_ms > 0) {
            g_hist_frame = c.frame;
            SamHist& h = g_hist[g_hist_head];
            h.ms = static_cast<double>(c.qpc) / g_qpc_per_ms;
            std::memcpy(h.q, samq, sizeof(h.q));
            g_hist_head = (g_hist_head + 1) % kHist;
            if (g_hist_n < kHist) ++g_hist_n;
        }
    } else {
        g_sam_series.Lost();
    }
    wire::Quat bq[2][4];
    bool body_ok[2] = {};
    std::uint8_t apps[2] = {};
    for (int i = 0; i < 2; ++i) {
        BodySlot& b = g_slots[i];
        SlotMeter& m = g_meter[i];
        const std::uint32_t app = b.ct.applies.load(), upd = b.updates.load();
        const std::uint32_t da = app - m.prev_app, du = upd - m.prev_upd;
        m.prev_app = app, m.prev_upd = upd;
        apps[i] = static_cast<std::uint8_t>(da > 9 ? 9 : da);
        if (b.phase != Phase::live) {
            m.s.Lost();
            continue;
        }
        m.app += da, m.upd += du;
        ++m.per_tick[da > 2 ? 2 : da];
        body_ok[i] = ReadBodyJoints(b, bq[i]);
        if (body_ok[i]) m.s.Add(bq[i], dt);
        else m.s.Lost();
    }
    // Partner: latency of the pose on screen (now - its play time, on one PC the same clock in echo)
    // and, in echo, its error against our own Sam at that play time.
    if (g_p.phase == Phase::live && g_p.owner == Owner::partner) {
        const double play = g_last_play.load(std::memory_order_relaxed);
        SlotMeter& m = g_meter[0];
        if (play > 0) {
            m.lat_sum += now_ms - play;
            ++m.lat_n;
        }
        wire::Quat ref[4];
        if (g_meter_echo && body_ok[0] && m.s.ema_rate > 30.0 && play > 0 && HistAt(play, ref)) {
            double e = 0;
            for (unsigned k = 0; k < 4; ++k) e += wire::AngleBetween(ref[k], bq[0][k]);
            e /= 4;
            m.err_sum += e;
            ++m.err_n;
            if (e > m.err_max) m.err_max = e;
        }
    }
    if (g_seq_want && g_seq_n < g_seq_want && g_seq_n < kSeqMax) {
        SeqRow& r = g_seq[g_seq_n++];
        r.dt = static_cast<float>(dt);
        r.step_p = SeqStep(0, body_ok[0], bq[0]);
        r.step_s = SeqStep(1, body_ok[1], bq[1]);
        r.step_sam = SeqStep(2, sam_ok, samq);
        r.app_p = apps[0], r.app_s = apps[1];
        if (g_seq_n >= g_seq_want) {
            LogSeq();
            g_seq_want = 0;
        }
    }
    if (now_ms < g_meter_next_ms) return;
    const double span = (now_ms - g_meter_start_ms) / 1000.0;
    const unsigned long presents = overlay::Installed() ? overlay::GetStats().presents : 0;
    const double fps = span > 0 ? static_cast<double>(presents - g_meter_presents) / span : 0;
    std::uint8_t sam_f = 0xFF, pf = 0xFF, sf = 0xFF;
    if (const std::uintptr_t se = g_sam_e.load()) Rd(se + kEntFreq, sam_f);
    if (g_p.phase == Phase::live && holo::SpawnAlive(g_p.spawn)) Rd(reinterpret_cast<std::uintptr_t>(g_p.spawn.e) + kEntFreq, pf);
    if (g_me.phase == Phase::live && holo::SpawnAlive(g_me.spawn)) Rd(reinterpret_cast<std::uintptr_t>(g_me.spawn.e) + kEntFreq, sf);
    char part[400] = "partner -", self[400] = "self -";
    const std::uint32_t ia = g_interp_applies.exchange(0), ih = g_interp_holds.exchange(0), it = g_interp_torn.exchange(0);
    if (g_p.phase == Phase::live) {
        const SlotMeter& m = g_meter[0];
        std::snprintf(part, sizeof(part),
                      "partner upd %u app %u (interp %u hold %u torn %u) writes/frame 0:%u 1:%u 2+:%u | moving %u new %u repeat %u stutter %u "
                      "jerk %.2f | latency %.1f ms | err avg %.2f max %.2f deg (n %u) | freq %u",
                      m.upd, m.app, ia, ih, it, m.per_tick[0], m.per_tick[1], m.per_tick[2], m.s.moving, m.s.moving - m.s.repeat, m.s.repeat,
                      m.s.stutter, m.s.Jerk(), m.lat_n ? m.lat_sum / m.lat_n : 0.0, m.err_n ? m.err_sum / m.err_n : 0.0, m.err_max, m.err_n,
                      pf);
    }
    if (g_me.phase == Phase::live) {
        const SlotMeter& m = g_meter[1];
        const std::uint32_t b0 = g_self_age_bucket[0].exchange(0), b1 = g_self_age_bucket[1].exchange(0), b2 = g_self_age_bucket[2].exchange(0);
        std::snprintf(self, sizeof(self),
                      "self upd %u app %u writes/frame 0:%u 1:%u 2+:%u | moving %u new %u repeat %u stutter %u jerk %.2f | capture age "
                      "<0.5 frame %u, ~1 frame %u, older %u | freq %u",
                      m.upd, m.app, m.per_tick[0], m.per_tick[1], m.per_tick[2], m.s.moving, m.s.moving - m.s.repeat, m.s.repeat, m.s.stutter,
                      m.s.Jerk(), b0, b1, b2, sf);
    }
    // Every second in hidden tests; in a player's game one second in ten (the same one-second numbers),
    // and not at all with smooth_pose=0 (the v0.7 behaviour logs nothing new).
    static unsigned windows = 0;
    if (automation::Enabled() || (g_smooth.load(std::memory_order_relaxed) != 0 && windows++ % 10 == 0))
        Log("[smooth] fps %.1f (ticks %u, frame %.1f-%.1f ms, cap %u, mask %u) | %s | %s | Sam caps %u moving %u repeat %u stutter %u jerk "
            "%.2f freq %u",
            fps, g_meter_ticks, g_meter_dt_min < 1e8 ? g_meter_dt_min : 0.0, g_meter_dt_max, RuntimeFrameCap(), g_smooth.load(), part, self,
            g_sam_caps, g_sam_series.moving, g_sam_series.repeat, g_sam_series.stutter, g_sam_series.Jerk(), sam_f);
    g_meter_next_ms = now_ms + 1000;
    g_meter_start_ms = now_ms;
    g_meter_presents = presents;
    g_meter_ticks = 0;
    g_meter_dt_min = 1e9, g_meter_dt_max = 0;
    g_meter[0].Reset(), g_meter[1].Reset();
    g_sam_series.Reset();
    g_sam_caps = 0;
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
    // round 27 M4: none once the game began removing it (the destroy guard; PollSlot drops the slot on its next poll). Residual
    // (docs/enemy-sync.md 16.7): a pose write of player_sync's already past this test when another thread begins the removal
    if (rmhook::Removed(rmhook::kWatchBody, reinterpret_cast<std::uintptr_t>(g_p.spawn.e))) return nullptr;
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

void PublishGear(GearSlot slot, const GearProp* props, unsigned n) noexcept {
    const int i = slot == GearSlot::self ? 1 : 0;
    if (!g_fn_setwt && mem::CodeMatches(mem::Base() + kSetWorldTransform, "48 85 C9 74 1C 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 33 D2 48 8B CB E8"))
        g_fn_setwt = mem::Base() + kSetWorldTransform;
    static GearSet set;
    set.n = props && g_fn_setwt ? (n < kMaxGearProps ? n : kMaxGearProps) : 0;
    for (unsigned k = 0; k < set.n; ++k) set.p[k] = props[k];
    g_gear_gen[i].fetch_add(1, std::memory_order_acq_rel);  // the extrapolation's history starts again (section 26.7)
    if (set.n == 0) {
        // Unpublish, then wait until no detour can still be placing the old list.
        g_gear_n[i].store(0, std::memory_order_seq_cst);
        g_gear[i].Write(set);
        g_gear_gen[i].fetch_add(1, std::memory_order_acq_rel);
        const ULONGLONG until = GetTickCount64() + 50;
        while (g_inflight.load(std::memory_order_seq_cst) != 0 && GetTickCount64() < until) SwitchToThread();
        if (g_inflight.load(std::memory_order_seq_cst) != 0)
            Log("[%s] gear unpublish: detour still busy after 50 ms (continuing)", i ? "gear-self" : "gear");
        return;
    }
    g_gear[i].Write(set);
    g_gear_gen[i].fetch_add(1, std::memory_order_acq_rel);
    g_gear_n[i].store(set.n, std::memory_order_release);
}

unsigned PlaceGearNow(GearSlot slot) noexcept {
    const int i = slot == GearSlot::self ? 1 : 0;
    BodySlot& b = g_slots[i];
    const std::uintptr_t sm = b.sm.load(std::memory_order_acquire), e = b.e.load(std::memory_order_acquire);
    if (!sm || !e || b.phase != Phase::live || g_gear_n[i].load(std::memory_order_acquire) == 0) return 0;
    if (Q(sm) != g_sm_vt || Q(sm + kSmEntity) != e || Q(e) != b.e_vt) return 0;
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0, local = 0;
    if (!ReadPoseHdr(sm, hdr, &pose) || CheckHdr(hdr, b.skel.load(std::memory_order_acquire), &local) != 0) return 0;
    Xf w;
    if (!ReadWorld(e, &w)) return 0;
    return PlaceGear(i, w, Field<std::uintptr_t>(hdr, kHdrModel), b.inv, kPlaceTick);
}

void GearCounters(GearSlot slot, std::uint32_t* placed, std::uint32_t* aba, std::uint32_t* bad) noexcept {
    const int i = slot == GearSlot::self ? 1 : 0;
    if (placed) *placed = g_gear_placed[i].load();
    if (aba) *aba = g_gear_aba[i].load();
    if (bad) *bad = g_gear_bad[i].load();
}

void GearPlacementInfo(GearSlot slot, char* out, std::size_t cap) noexcept {
    const int i = slot == GearSlot::self ? 1 : 0;
    const int fix = g_gear_fix.load(std::memory_order_relaxed);
    std::snprintf(out, cap, "fix %d: %u in the body's update, the tick's %u extrapolated / %u plain", fix, g_gear_body_pl[i].load(),
                  g_gear_xtrap[i].load(), g_gear_xplain[i].load());
    if (i == 1) {
        const std::size_t n = std::strlen(out);
        if (n < cap) std::snprintf(out + n, cap - n, " (%u on the body's extrapolated world)", g_gear_xbody.load());
    }
}

// ---- the drawn-vs-anchor meter's evaluation (see GmRecPlace): the tick, before its own placements.
namespace {
struct GmStat {
    std::uint32_t n = 0, other = 0, nobody = 0, used[3] = {}, race = 0, o3 = 0, o8 = 0;
    double sum = 0, mx = 0, ksum[3] = {}, kmx[3] = {}, rsum = 0, rmx = 0;
};
GmStat g_gm_st[2];
// Head items against her drawn head (section 26.8): per slot the props anchored on the head joints (31-34: the partner's
// cap and glasses props, the self view's hat in the rain), and on the self slot Sam's own head items (the movers
// partner_gear published), by class (0 caps, 1 glasses). Moving frames; the all-frames max beside.
struct HdStat {
    std::uint32_t n = 0, o1 = 0, o3 = 0, o8 = 0, all = 0;
    double sum = 0, mx = 0, asum = 0, amx = 0, all_mx = 0;
    void Add(double cm, double deg, bool moving) {
        ++all;
        if (cm > all_mx) all_mx = cm;
        if (!moving) return;
        ++n, sum += cm, asum += deg;
        if (cm > mx) mx = cm;
        if (deg > amx) amx = deg;
        o1 += cm > 1.0, o3 += cm > 3.0, o8 += cm > 8.0;
    }
};
HdStat g_hd_props[2], g_hd_sam[2];
std::uint64_t g_hd_last_sp[kMaxHeadMovers] = {};
std::uintptr_t g_hd_last_e[kMaxHeadMovers] = {};
std::uint64_t g_gm_last_sp[2][kMaxGearProps] = {};
double g_gm_prev_p[2][3] = {};
bool g_gm_prev_ok[2] = {};
double g_gm_tsc_ms = 0;
std::uint64_t g_gm_tsc0 = 0;
std::int64_t g_gm_qpc0 = 0;
double g_gm_next_ms = 0;
std::atomic<int> g_gm_sites{-1};  // -1 not checked, 0 mismatch, 1 ok
}  // namespace

// The two stamp sites the meter reads (Entity::Update's rdtsc writes into the entity).
bool GearMeterSites(char* why, std::size_t cap) {
    int s = g_gm_sites.load();
    if (s < 0) {
        const bool a = mem::CodeMatches(mem::Base() + kStampStartSite, "48 89 81 50 02 00 00");
        const bool b = mem::CodeMatches(mem::Base() + kStampEndSite, "48 89 87 58 02 00 00");
        s = a && b ? 1 : 0;
        g_gm_sites.store(s);
        if (!s) Log("[gear] drawn-vs-anchor meter off: Entity::Update's stamp sites do not match (start %s, end %s)", a ? "ok" : "MISMATCH", b ? "ok" : "MISMATCH");
    }
    if (!s && why) std::snprintf(why, cap, "Entity::Update's stamp sites (0x14234D7A9 / 0x14234DA87) do not match this game build");
    return s == 1;
}

void GearMeterTick(double now_ms) noexcept {
    if (!g_gm_on.load(std::memory_order_relaxed)) return;
    const std::uint64_t tnow = __rdtsc();
    if (!g_gm_tsc0) {
        g_gm_tsc0 = tnow, g_gm_qpc0 = Qpc();
        return;
    }
    if (g_gm_tsc_ms <= 0) {
        if (g_qpc_per_ms <= 0) return;
        const double ms = static_cast<double>(Qpc() - g_gm_qpc0) / g_qpc_per_ms;
        if (ms < 1000) return;
        g_gm_tsc_ms = static_cast<double>(tnow - g_gm_tsc0) / ms;
    }
    const double fr = g_frame_ms.load(std::memory_order_relaxed);
    const auto half = static_cast<std::uint64_t>(0.5 * fr * g_gm_tsc_ms);
    static GearSet set;
    static GmRing rings[kMaxGearProps];
    for (int s = 0; s < 2; ++s) {
        BodySlot& b = g_slots[s];
        const std::uintptr_t e = b.e.load(std::memory_order_acquire), sm = b.sm.load(std::memory_order_acquire);
        if (!e || !sm || b.phase != Phase::live) continue;
        if (Q(sm) != g_sm_vt || Q(sm + kSmEntity) != e || Q(e) != b.e_vt) continue;
        const std::uint64_t Sb = Q(e + kEntStampStart), Eb = Q(e + kEntStampEnd);
        GmStat& st = g_gm_st[s];
        GmBody br{};
        GmLock();
        for (const GmBody& r : g_gm_body[s])
            if (r.ok && r.tsc >= Sb && r.tsc <= Eb && r.tsc > br.tsc) br = r;
        std::memcpy(rings, g_gm_ring[s], sizeof(rings));
        GmUnlock();
        if (!br.ok) {
            ++st.nobody;
            continue;
        }
        bool moving = false;
        if (g_gm_prev_ok[s]) {
            double d2 = 0;
            for (int k = 0; k < 3; ++k) d2 += (br.w.p[k] - g_gm_prev_p[s][k]) * (br.w.p[k] - g_gm_prev_p[s][k]);
            moving = d2 > 0.005 * 0.005;
        }
        std::memcpy(g_gm_prev_p[s], br.w.p, sizeof(br.w.p));
        g_gm_prev_ok[s] = true;
        std::uint8_t hdr[kPoseSize];
        std::uintptr_t pose = 0, local = 0;
        if (!ReadPoseHdr(sm, hdr, &pose) || CheckHdr(hdr, b.skel.load(std::memory_order_acquire), &local) != 0) continue;
        const std::uintptr_t mb = Field<std::uintptr_t>(hdr, kHdrModel);
        if (g_gear_n[s].load(std::memory_order_acquire) == 0 || !g_gear[s].Read(set) || set.n == 0 || set.n > kMaxGearProps) continue;
        // each prop's anchor as the body drew it: its world in its update, its pose (the tick runs before this frame's)
        Xf w[kMaxGearProps];
        bool have[kMaxGearProps] = {};
        for (unsigned i = 0; i < set.n; ++i) {
            const GearProp& gp = set.p[i];
            Xf anchor;
            if (gp.parent >= 0) {
                if (static_cast<unsigned>(gp.parent) >= i || !have[gp.parent]) continue;
                anchor = w[gp.parent];
            } else {
                if (gp.joint >= kJ) continue;
                float m[4][4];
                double jp[3];
                float jr[3][3];
                if (!mem::Read(mb + 0x40ull * b.inv[gp.joint], m, sizeof(m)) || !JointFrame(m, jp, jr)) continue;
                anchor = Compose(br.w, jp, jr);
            }
            const double p3[3] = {gp.p[0], gp.p[1], gp.p[2]};
            w[i] = Compose(anchor, p3, gp.r);
            have[i] = true;
        }
        for (unsigned i = 0; i < set.n; ++i) {
            const GearProp& gp = set.p[i];
            if (!gp.e || !have[i] || Q(gp.e) != gp.e_vt) continue;
            const std::uint64_t Sp = Q(gp.e + kEntStampStart);
            if (!Sp || Sp == g_gm_last_sp[s][i]) continue;
            g_gm_last_sp[s][i] = Sp;
            if (!moving) continue;
            if ((Sp > Sb ? Sp - Sb : Sb - Sp) > half) {  // not this frame's prop update
                ++st.other;
                continue;
            }
            // the placement it had when its update started: the newest one stamped before it
            const GmRing& r = rings[i];
            const GmPlace* used = nullptr;
            const unsigned cnt = r.n < kGmRing ? r.n : kGmRing;
            for (unsigned k = 0; k < cnt; ++k) {
                const GmPlace& d = r.e[k];
                if (d.tsc < Sp && (!used || d.tsc > used->tsc)) used = &d;
            }
            if (!used) continue;
            double d2 = 0;
            for (int k = 0; k < 3; ++k) d2 += (used->p[k] - w[i].p[k]) * (used->p[k] - w[i].p[k]);
            const double err = std::sqrt(d2) * 100.0;
            if (gp.parent < 0 && gp.joint >= kSamHeadJ && gp.joint <= kSamHeadJ + 3) g_hd_props[s].Add(err, 0.0, true);
            ++st.n;
            st.sum += err;
            if (err > st.mx) st.mx = err;
            st.o3 += err > 3.0, st.o8 += err > 8.0;
            const int k = used->kind >= 0 && used->kind < 3 ? used->kind : 0;
            ++st.used[k], st.ksum[k] += err;
            if (err > st.kmx[k]) st.kmx[k] = err;
            if (Sp >= Sb && Sp <= Eb) {  // the prop's update started during the body's (another worker thread)
                ++st.race, st.rsum += err;
                if (err > st.rmx) st.rmx = err;
            }
        }
    }
    if (now_ms < g_gm_next_ms) return;
    g_gm_next_ms = now_ms + 1000;
    for (int s = 0; s < 2; ++s) {
        GmStat& st = g_gm_st[s];
        if (st.n || st.other || st.nobody)
            Log("[gear] drawn vs anchor %s (fix %d): %u moving prop updates, cm mean %.2f max %.2f (>3 %u, >8 %u); drew the tick's %u (%.2f/%.2f), the "
                "body update's %u (%.2f/%.2f), Sam's update's %u (%.2f/%.2f); raced the body's update %u (%.2f/%.2f); other frame %u, no body record %u",
                s ? "self" : "partner", g_gear_fix.load(), st.n, st.n ? st.sum / st.n : 0, st.mx, st.o3, st.o8, st.used[0],
                st.used[0] ? st.ksum[0] / st.used[0] : 0, st.kmx[0], st.used[1], st.used[1] ? st.ksum[1] / st.used[1] : 0, st.kmx[1], st.used[2],
                st.used[2] ? st.ksum[2] / st.used[2] : 0, st.kmx[2], st.race, st.race ? st.rsum / st.race : 0, st.rmx, st.other, st.nobody);
        st = GmStat{};
    }
    auto hd = [](const char* what, HdStat& h) {
        if (h.all)
            Log("[headgear] drawn vs her drawn head, %s (corr %d): %u moving updates, cm mean %.2f max %.2f (>1 %u, >3 %u, >8 %u), deg mean %.2f max %.2f; "
                "all %u updates max %.2f cm",
                what, g_hcorr_on.load(), h.n, h.n ? h.sum / h.n : 0, h.mx, h.o1, h.o3, h.o8, h.n ? h.asum / h.n : 0, h.amx, h.all, h.all_mx);
        h = HdStat{};
    };
    hd("self: Sam's caps", g_hd_sam[0]);
    hd("self: Sam's glasses", g_hd_sam[1]);
    hd("self: head props", g_hd_props[1]);
    hd("partner: head props", g_hd_props[0]);
}

// Sam's head items against her drawn head (section 26.8), hidden test runs (`self_view 15 1`): at the very start of the tick,
// before anything of this tick moves Sam (the ride carry teleports him from the tick, and a teleport moves the world fields of
// his attached children at once: read later, they show the next frame's place, not what was drawn). Each published item's
// world after its own update of the last frame against her HLP_Head as her body drew it in that frame (its world in its
// update, the pose the tick still finds) times the item's offset.
void HeadMeterSam() noexcept {
    if (!g_gm_on.load(std::memory_order_relaxed) || !g_hms_n.load(std::memory_order_acquire) || g_gm_tsc_ms <= 0) return;
    BodySlot& b = g_slots[1];
    const std::uintptr_t e = b.e.load(std::memory_order_acquire), sm = b.sm.load(std::memory_order_acquire);
    if (!e || !sm || b.phase != Phase::live || Q(sm) != g_sm_vt || Q(sm + kSmEntity) != e || Q(e) != b.e_vt) return;
    const std::uint64_t Sb = Q(e + kEntStampStart), Eb = Q(e + kEntStampEnd);
    GmBody br{};
    GmLock();
    for (const GmBody& r : g_gm_body[1])
        if (r.ok && r.tsc >= Sb && r.tsc <= Eb && r.tsc > br.tsc) br = r;
    GmUnlock();
    if (!br.ok) return;
    static double prev[3] = {};
    static bool have_prev = false;
    double d2 = 0;
    for (int k = 0; k < 3; ++k) d2 += (br.w.p[k] - prev[k]) * (br.w.p[k] - prev[k]);
    const bool moving = have_prev && d2 > 0.005 * 0.005;
    std::memcpy(prev, br.w.p, sizeof(prev));
    have_prev = true;
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0, local = 0;
    if (!ReadPoseHdr(sm, hdr, &pose) || CheckHdr(hdr, b.skel.load(std::memory_order_acquire), &local) != 0) return;
    static HeadMoverSet hset;
    float m[4][4];
    double jp[3];
    float jr[3][3];
    const unsigned bj = b.inv[kSamHeadJ];
    if (!g_hms.Read(hset) || hset.n > kMaxHeadMovers || bj >= kJ || !mem::Read(Field<std::uintptr_t>(hdr, kHdrModel) + 0x40ull * bj, m, sizeof(m)) ||
        !JointFrame(m, jp, jr))
        return;
    const double fr = g_frame_ms.load(std::memory_order_relaxed);
    const auto frame = static_cast<std::uint64_t>(fr * g_gm_tsc_ms);
    const double zero[3] = {0, 0, 0};
    const Xf g = Compose(Compose(br.w, jp, jr), zero, kHlp);
    for (unsigned i = 0; i < hset.n; ++i) {
        const HeadMoverPub& hp = hset.m[i];
        if (Q(hp.e) != hp.e_vt) continue;
        const std::uint64_t Sp = Q(hp.e + kEntStampStart);
        if (!Sp || (g_hd_last_e[i] == hp.e && Sp == g_hd_last_sp[i])) continue;
        g_hd_last_e[i] = hp.e, g_hd_last_sp[i] = Sp;
        if ((Sp > Sb ? Sp - Sb : Sb - Sp) > frame) continue;  // not the item update of the body's frame
        Xf iw;
        if (!ReadWorld(hp.e, &iw)) continue;
        const double o3[3] = {hp.off[0], hp.off[1], hp.off[2]};
        const float id[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const Xf want = Compose(g, o3, id);
        double e2 = 0;
        for (int k = 0; k < 3; ++k) e2 += (iw.p[k] - want.p[k]) * (iw.p[k] - want.p[k]);
        g_hd_sam[HeadItemClass(hp.item) == kHeadItemCap ? 0 : 1].Add(std::sqrt(e2) * 100.0, selfpred::RotAngleDeg(iw.r, want.r), moving);
    }
}

void ClearBootsProp() noexcept {
    if (!g_bt.sm.load(std::memory_order_acquire)) return;
    g_bt.sm.store(0, std::memory_order_seq_cst);
    const ULONGLONG until = GetTickCount64() + 50;
    while (g_inflight.load(std::memory_order_seq_cst) != 0 && GetTickCount64() < until) SwitchToThread();
    g_bt.e.store(0);
    g_bt.spawn = holo::Spawn{};
    g_bt.last_apply.store(0);
}
bool SetBootsProp(const holo::Spawn* sp, char* why, std::size_t cap) noexcept {
    if (why && cap) why[0] = 0;
    if (!sp || !sp->e) {
        ClearBootsProp();
        return true;
    }
    const auto e = reinterpret_cast<std::uintptr_t>(sp->e);
    if (g_bt.sm.load(std::memory_order_acquire) && g_bt.e.load() == e) return true;  // the same prop
    ClearBootsProp();
    if (!g_hook || g_bt_faulted.load()) return Set(why, cap, "%s", g_hook ? "a boots pose write faulted earlier" : "no pose hook"), false;
    if (!holo::SpawnAlive(*sp)) return Set(why, cap, "the prop is gone"), false;
    if (g_p.phase != Phase::live || g_p.latch_state.load() != 1) return Set(why, cap, "the partner body is not live and latched"), false;
    const std::uintptr_t sm = ModelOf(e);
    const SkelInfo si = sm ? PoseSkeleton(sm) : SkelInfo{};
    if (!si.skel || si.count != static_cast<std::int32_t>(kBootsJ) || si.layout != kBootsLayout || !si.joints)
        return Set(why, cap, "its skeleton is not the boots' (%d joints, layout %08X)", si.count, si.layout), false;
    for (unsigned k = 0; k < kBootsJ; ++k) {
        std::int16_t par = -2;
        if (!Rd(si.joints + 0x10ull * k + 8, par) || par != kBootsParent[k]) return Set(why, cap, "joint %u's parent is %d, not %d", k, par, kBootsParent[k]), false;
        const unsigned bj = g_p.inv[kBootsSamJ[k]];
        if (bj >= kJ || g_p.map[bj] != kBootsSamJ[k]) return Set(why, cap, "the body has no joint for Sam joint %u", kBootsSamJ[k]), false;
        g_bt.bj[k] = static_cast<std::uint8_t>(bj);
    }
    std::uintptr_t vt = 0;
    if (!Rd(e, vt) || !vt) return Set(why, cap, "unreadable entity"), false;
    g_bt.e.store(e), g_bt.e_vt.store(vt), g_bt.skel.store(si.skel);
    g_bt.spawn = *sp;
    g_bt.game_updates.store(0), g_bt.last_game_ms = 0, g_bt.last_apply.store(0);
    g_bt.sm.store(sm, std::memory_order_seq_cst);
    return true;
}
bool BootsPosed(double within_ms) noexcept {
    const ULONGLONG t = g_bt.last_apply.load(std::memory_order_relaxed);
    return g_bt.sm.load(std::memory_order_acquire) && t && static_cast<double>(GetTickCount64() - t) < within_ms;
}
void BootsCounters(char* out, std::size_t cap) noexcept {
    std::snprintf(out, cap, "boots prop %p: %u pose writes (%u refused, %u torn, %u wpm), %u forced updates%s", reinterpret_cast<void*>(g_bt.e.load()),
                  g_bt.applies.load(), g_bt.rejects.load(), g_bt.torn.load(), g_bt.wpm.load(), g_bt.forced, g_bt_faulted.load() ? ", FAULTED: off" : "");
}

void ClearExoCarrier(GearSlot slot) noexcept {
    ExoTarget& t = g_xt[slot == GearSlot::self ? 1 : 0];
    if (!t.sm.load(std::memory_order_acquire) && !t.e.load()) return;
    t.sm.store(0, std::memory_order_seq_cst);
    const ULONGLONG until = GetTickCount64() + 50;
    while (g_inflight.load(std::memory_order_seq_cst) != 0 && GetTickCount64() < until) SwitchToThread();
    t.e.store(0);
    t.spawn = holo::Spawn{};
    t.last_apply.store(0);
    t.want_pose.store(false, std::memory_order_relaxed);
}
bool SetExoCarrier(GearSlot slot, const holo::Spawn* sp, char* why, std::size_t cap) noexcept {
    if (why && cap) why[0] = 0;
    const int x = slot == GearSlot::self ? 1 : 0;
    ExoTarget& t = g_xt[x];
    BodySlot& b = g_slots[x];
    if (!sp || !sp->e) {
        ClearExoCarrier(slot);
        return true;
    }
    const auto e = reinterpret_cast<std::uintptr_t>(sp->e);
    if (t.sm.load(std::memory_order_acquire) && t.e.load() == e) return true;  // the same carrier
    ClearExoCarrier(slot);
    if (!g_hook || g_xt_faulted.load()) return Set(why, cap, "%s", g_hook ? "an exo pose write faulted earlier" : "no pose hook"), false;
    if (!holo::SpawnAlive(*sp)) return Set(why, cap, "the carrier is gone"), false;
    if (b.phase != Phase::live || b.latch_state.load() != 1 || !b.apply_on.load()) return Set(why, cap, "the body is not live, latched and posed"), false;
    const std::uintptr_t sm = ModelOf(e);
    const SkelInfo si = sm ? PoseSkeleton(sm) : SkelInfo{};
    if (!si.skel || si.count != static_cast<std::int32_t>(kJ) || si.layout != wire::kSamLayoutHash || !si.joints)
        return Set(why, cap, "its skeleton is not Sam's 77-joint layout (%d joints, layout %08X)", si.count, si.layout), false;
    // Sam's joint names in order (the carrier is a Sam body: its joint k is Sam's joint k)
    const std::uintptr_t sam_sm = g_sam_sm.load() ? g_sam_sm.load() : g_menu_sm.load();
    const SkelInfo ss = PoseSkeleton(sam_sm);
    if (ss.skel != si.skel) {
        if (!ss.skel || ss.count != static_cast<std::int32_t>(kJ)) return Set(why, cap, "Sam's skeleton is not available"), false;
        for (unsigned k = 0; k < kJ; ++k)
            if (JointName(si, k) != JointName(ss, k)) return Set(why, cap, "its joint %u is not Sam's joint %u", k, k), false;
    }
    for (unsigned k = 0; k < kJ; ++k) {
        const unsigned bj = b.inv[k];
        if (bj >= kJ || b.map[bj] != k) return Set(why, cap, "the body has no joint for Sam joint %u", k), false;
        t.bj[k] = static_cast<std::uint8_t>(bj);
    }
    std::uintptr_t vt = 0;
    if (!Rd(e, vt) || !vt) return Set(why, cap, "unreadable entity"), false;
    t.e.store(e), t.e_vt.store(vt), t.skel.store(si.skel);
    t.spawn = *sp;
    t.game_updates.store(0), t.last_game_ms = 0, t.last_apply.store(0);
    t.applies_at_set = t.applies.load();
    t.forced_off = false;
    t.sm.store(sm, std::memory_order_seq_cst);
    return true;
}
void SetExoPoseWanted(GearSlot slot, bool on) noexcept { g_xt[slot == GearSlot::self ? 1 : 0].want_pose.store(on, std::memory_order_relaxed); }
bool ExoPosed(GearSlot slot, double within_ms) noexcept {
    const ExoTarget& t = g_xt[slot == GearSlot::self ? 1 : 0];
    const ULONGLONG at = t.last_apply.load(std::memory_order_relaxed);
    // three writes since it became the target (the first one came out 13-29 cm off in run 20261122-181043: shown too early)
    return t.sm.load(std::memory_order_acquire) && at && static_cast<double>(GetTickCount64() - at) < within_ms &&
           t.applies.load(std::memory_order_relaxed) - t.applies_at_set >= 3;
}
void SetExoSelfSource(int src) noexcept { g_xt_self_src.store(src == 1 ? 1 : 0, std::memory_order_relaxed); }
int ExoSelfSource() noexcept { return g_xt_self_src.load(std::memory_order_relaxed); }
std::uintptr_t ExoCarrierEntity(GearSlot slot) noexcept {
    const ExoTarget& t = g_xt[slot == GearSlot::self ? 1 : 0];
    return t.sm.load(std::memory_order_acquire) ? t.e.load() : 0;
}
void ExoCounters(GearSlot slot, char* out, std::size_t cap) noexcept {
    const ExoTarget& t = g_xt[slot == GearSlot::self ? 1 : 0];
    std::snprintf(out, cap, "exo carrier %p: %u pose writes (%u refused, %u torn, %u wpm), %u forced updates (%u skipped: hidden)%s",
                  reinterpret_cast<void*>(t.e.load()), t.applies.load(), t.rejects.load(), t.torn.load(), t.wpm.load(), t.forced, t.forced_skipped,
                  g_xt_faulted.load() ? ", FAULTED: off" : "");
}
bool PartnerOutfit(wire::Outfit* out) noexcept {
    if (!g_have_partner_outfit || !out) return false;
    *out = g_partner_outfit;
    return true;
}

bool SlotIsFragileSuit(GearSlot slot) noexcept {
    const BodySlot& b = g_slots[slot == GearSlot::self ? 1 : 0];
    return b.phase == Phase::live && IsFragileSuit(b.body) && holo::SpawnAlive(b.spawn);
}

bool HoodRow(GearSlot slot, std::uint8_t* row, bool* from_look) noexcept {
    const int r = g_hood_row[slot == GearSlot::self ? 1 : 0];
    if (!row || r < 0 || r > 16 || !SlotIsFragileSuit(slot)) return false;
    *row = static_cast<std::uint8_t>(r);
    if (from_look) *from_look = g_hood_row_look[slot == GearSlot::self ? 1 : 0];
    return true;
}

bool PoseInterpOn() noexcept {
    return g_hook && g_qpc_per_ms > 0 && (g_smooth.load(std::memory_order_relaxed) & kSmoothDetourInterp) != 0;
}

bool SetGearPose(unsigned k, const holo::Spawn* sp, const wire::GearPose* pose, const GearPoseInterp* ip) noexcept {
    if (k >= kMaxPoseTargets || !g_hook) return false;
    PoseTarget& t = g_pt[k];
    if (!sp || !sp->e || !pose) {
        ClearGearPose(k);
        return true;
    }
    if (g_pt_faulted.load()) return false;  // a pose write faulted: off for the session
    if (!holo::SpawnAlive(*sp)) return false;
    const auto e = reinterpret_cast<std::uintptr_t>(sp->e);
    if (pose->n == 0 || pose->n > kMaxGearPoseJ) return false;
    static GearPoseBuf b;  // game thread only (large: two poses)
    b.n = pose->n;
    b.n2 = 0;
    for (unsigned j = 0; j < pose->n; ++j) {
        const wire::Quat q = wire::Normalize(pose->q[j]);
        b.q[j][0] = q.x, b.q[j][1] = q.y, b.q[j][2] = q.z, b.q[j][3] = q.w;
        b.t[j][0] = pose->t[j][0], b.t[j][1] = pose->t[j][1], b.t[j][2] = pose->t[j][2];
    }
    if (ip && ip->b && ip->b->n == pose->n && ip->b->skeleton == pose->skeleton && ip->key_b > ip->key_a && PoseInterpOn()) {
        for (unsigned j = 0; j < pose->n; ++j) {
            const wire::Quat q = wire::Normalize(ip->b->q[j]);
            b.q2[j][0] = q.x, b.q2[j][1] = q.y, b.q2[j][2] = q.z, b.q2[j][3] = q.w;
            b.t2[j][0] = ip->b->t[j][0], b.t2[j][1] = ip->b->t[j][1], b.t2[j][2] = ip->b->t[j][2];
        }
        b.key_a = ip->key_a, b.key_b = ip->key_b, b.play_ref = ip->play_ms;
        b.ref_ms = static_cast<double>(Qpc()) / g_qpc_per_ms;
        b.n2 = pose->n;
        b.n3 = 0;
        if (ip->c && ip->c->n == pose->n && ip->c->skeleton == pose->skeleton && ip->key_c > ip->key_b) {
            for (unsigned j = 0; j < pose->n; ++j) {
                const wire::Quat q = wire::Normalize(ip->c->q[j]);
                b.q3[j][0] = q.x, b.q3[j][1] = q.y, b.q3[j][2] = q.z, b.q3[j][3] = q.w;
                b.t3[j][0] = ip->c->t[j][0], b.t3[j][1] = ip->c->t[j][1], b.t3[j][2] = ip->c->t[j][2];
            }
            b.key_c = ip->key_c;
            b.n3 = pose->n;
        }
    }
    if (t.sm.load(std::memory_order_acquire) && t.e.load() == e) {
        // the same prop: only the joints change
        for (int i = 0; i < 4 && !g_pt_buf[k].Write(b); ++i) SwitchToThread();
        return true;
    }
    // a new prop: checked (its model, skeleton and joint count), then published
    ClearGearPose(k);
    const std::uintptr_t sm = ModelOf(e);
    if (!sm) return false;
    const SkelInfo si = PoseSkeleton(sm);
    if (!si.skel || si.count != static_cast<std::int32_t>(pose->n) || si.layout != pose->skeleton) return false;
    std::uintptr_t vt = 0;
    if (!Rd(e, vt) || !vt) return false;
    t.e.store(e), t.e_vt.store(vt), t.skel.store(si.skel), t.n.store(pose->n);
    t.spawn = *sp;
    t.game_updates.store(0), t.last_game_ms = 0;
    if (k == kVehiclePoseTarget) g_vm_reset.store(true, std::memory_order_release);  // its meter starts over
    for (int i = 0; i < 4 && !g_pt_buf[k].Write(b); ++i) SwitchToThread();
    t.sm.store(sm, std::memory_order_seq_cst);
    RecountPoseTargets();
    return true;
}

void ClearGearPose(unsigned k) noexcept {
    if (k >= kMaxPoseTargets) return;
    PoseTarget& t = g_pt[k];
    if (!t.sm.load(std::memory_order_acquire)) return;
    t.sm.store(0, std::memory_order_seq_cst);
    RecountPoseTargets();
    // wait until no detour can still be writing this prop's pose
    const ULONGLONG until = GetTickCount64() + 50;
    while (g_inflight.load(std::memory_order_seq_cst) != 0 && GetTickCount64() < until) SwitchToThread();
    t.e.store(0);
    t.spawn = holo::Spawn{};
}

bool VehiclePoseMeter(VehicleMeter* out) noexcept {
    if (!out) return false;
    out->writes = g_vm_writes.exchange(0), out->repeats = g_vm_repeats.exchange(0);
    out->interp = g_vm_interp.exchange(0), out->holds = g_vm_holds.exchange(0);
    const std::uint64_t sum = g_vm_step_sum.exchange(0), sq = g_vm_step_sq.exchange(0);
    out->step_max_deg = static_cast<float>(g_vm_step_max.exchange(0)) / 1000.0f;
    double mean = 0, var = 0;
    if (out->writes) {
        mean = static_cast<double>(sum) / out->writes / 1000.0;
        var = static_cast<double>(sq) / out->writes / 1000.0 - mean * mean;  // sq is in millidegrees^2 / 1000
    }
    out->step_mean_deg = static_cast<float>(mean);
    out->step_sd_deg = var > 0 ? static_cast<float>(std::sqrt(var)) : 0.0f;
    return out->writes > 0;
}

int BoostEntity(game::Entity* e, const char* what, std::uint8_t* orig) noexcept {
    std::uint8_t cur = 0;
    if (!e || !orig || !Rd(reinterpret_cast<std::uintptr_t>(e) + kEntFreq, cur) || cur > 6) return -1;
    if (*orig == 0xFF) *orig = cur == kFreq240Hz ? std::uint8_t{3} : cur;
    const std::uint8_t want = (g_smooth.load(std::memory_order_relaxed) & kSmoothBoost) && !g_freq.off ? kFreq240Hz : *orig;
    if (cur == want) return 0;
    return SetFreq(reinterpret_cast<std::uintptr_t>(e), want, "smooth", what, true);
}

void VehicleMeterGate(bool moving) noexcept { g_vm_gate.store(moving, std::memory_order_relaxed); }

void GearPoseCounters(char* out, std::size_t cap) noexcept {
    std::uint32_t forced = 0, live = 0;
    for (const PoseTarget& t : g_pt) {
        forced += t.forced;
        live += t.sm.load() != 0;
    }
    std::snprintf(out, cap, "%u target(s), %u pose writes, %u forced updates, rejects %u, torn %u, write failures %u, faults %u%s", live, g_pt_applies.load(),
                  forced, g_pt_rejects.load(), g_pt_torn.load(), g_pt_wpm.load(), g_pt_faults.load(), g_pt_faulted.load() ? " (OFF for this session)" : "");
}

game::Entity* SlotEntity(GearSlot slot) noexcept {
    BodySlot& b = g_slots[slot == GearSlot::self ? 1 : 0];
    if (b.phase != Phase::live || !holo::SpawnAlive(b.spawn)) return nullptr;
    return b.spawn.e;
}

bool BeginByPath(const char* path, const char* guid_text, const game::WorldTransform& at, std::uint32_t tag, holo::Spawn* out, char* why,
                 std::size_t cap) noexcept {
    if (!g_hook || !ResolveSpawn()) {
        Set(why, cap, "%s", g_hook ? g_fn.why : "pose hook not installed");
        return false;
    }
    std::uint8_t guid[16];
    if (!path || !bodies::ParseGuid(guid_text, guid)) {
        Set(why, cap, "bad GUID text");
        return false;
    }
    return StartByPath("gear", path, "prop", path, guid, tag, at, true, out, why, cap) == BeginResult::started;
}

bool BeginByPathGuid(const char* path, const std::uint8_t (&guid)[16], const game::WorldTransform& at, std::uint32_t tag, holo::Spawn* out,
                     char* why, std::size_t cap) noexcept {
    if (!g_hook || !ResolveSpawn()) {
        Set(why, cap, "%s", g_hook ? g_fn.why : "pose hook not installed");
        return false;
    }
    if (!path || !*path) {
        Set(why, cap, "no path");
        return false;
    }
    return StartByPath("gear", path, "prop", path, guid, tag, at, true, out, why, cap) == BeginResult::started;
}

unsigned GearPublished(GearSlot slot) noexcept { return g_gear_n[slot == GearSlot::self ? 1 : 0].load(std::memory_order_acquire); }

bool ResourceIdentity(game::Entity* e, char* path, std::size_t cap, std::uint8_t (&guid)[16], const char** type) noexcept {
    if (type) *type = nullptr;
    if (path && cap) path[0] = 0;
    RefInfo r;
    if (!e || !ReadRef(reinterpret_cast<std::uintptr_t>(e) + kEntResRef, &r)) return false;
    std::memcpy(guid, r.guid, 16);
    if (path && cap) std::snprintf(path, cap, "%s", r.path.c_str());
    if (type && r.obj) *type = partner::TypeName(reinterpret_cast<const void*>(r.obj));
    return !r.path.empty();
}

bool ReadJointModel(game::Entity* e, unsigned j, float (&m)[4][4]) noexcept {
    const std::uintptr_t sm = e ? ModelOf(reinterpret_cast<std::uintptr_t>(e)) : 0;
    if (!sm || j >= kJ) return false;
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0;
    if (!ReadPoseHdr(sm, hdr, &pose) || Field<std::int32_t>(hdr, kHdrState) != kPoseStateModel) return false;
    const std::uintptr_t mb = Field<std::uintptr_t>(hdr, kHdrModel), me = Field<std::uintptr_t>(hdr, kHdrModelEnd);
    if (!mb || me - mb != kJ * 0x40) return false;
    return mem::Read(mb + 0x40ull * j, m, sizeof(m));
}

bool JointWorldN(game::Entity* e, unsigned j, unsigned n, double (&pos)[3]) noexcept {
    const std::uintptr_t sm = e ? ModelOf(reinterpret_cast<std::uintptr_t>(e)) : 0;
    if (!sm || j >= n || n == 0 || n > 512) return false;
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0;
    if (!ReadPoseHdr(sm, hdr, &pose) || Field<std::int32_t>(hdr, kHdrState) != kPoseStateModel) return false;
    const std::uintptr_t mb = Field<std::uintptr_t>(hdr, kHdrModel), me = Field<std::uintptr_t>(hdr, kHdrModelEnd);
    float m[4][4];
    double jp[3];
    float jr[3][3];
    Xf w;
    if (!mb || me - mb != n * 0x40ull || !mem::Read(mb + 0x40ull * j, m, sizeof(m)) || !ReadWorld(reinterpret_cast<std::uintptr_t>(e), &w)) return false;
    // position only: the row 3 of the model matrix (a joint whose axes are scaled away still has its position)
    for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(m[3][k]) || std::fabs(m[3][k]) > 10.0f) return false;
        jp[k] = m[3][k];
    }
    for (int a = 0; a < 3; ++a)
        for (int c = 0; c < 3; ++c) jr[a][c] = a == c ? 1.0f : 0.0f;
    const Xf h = Compose(w, jp, jr);
    std::memcpy(pos, h.p, sizeof(pos));
    return true;
}

bool HeadWorld(game::Entity* e, double (&pos)[3]) noexcept {
    constexpr unsigned kSamHead = 31;  // the head joint of the 77-joint layout (1.59 m above the root standing)
    if (!e) return false;
    unsigned j = kSamHead;
    for (BodySlot* b : {&g_p, &g_me})
        if (b->phase == Phase::live && b->spawn.e == e) j = b->inv[kSamHead];
    float m[4][4];
    double jp[3];
    float jr[3][3];
    Xf w;
    if (!ReadJointModel(e, j, m) || !JointFrame(m, jp, jr) || !ReadWorld(reinterpret_cast<std::uintptr_t>(e), &w)) return false;
    const Xf h = Compose(w, jp, jr);
    const double dx = h.p[0] - w.p[0], dy = h.p[1] - w.p[1], dz = h.p[2] - w.p[2];
    if (!(dz > 0.3 && dz < 2.4) || dx * dx + dy * dy > 1.0) return false;  // not a head over its feet
    pos[0] = h.p[0], pos[1] = h.p[1], pos[2] = h.p[2];
    return true;
}

bool JointWorld(game::Entity* e, unsigned sam_joint, double (&pos)[3]) noexcept {
    if (!e || sam_joint >= kJ) return false;
    unsigned j = sam_joint;
    for (BodySlot* b : {&g_p, &g_me})
        if (b->phase == Phase::live && b->spawn.e == e) j = b->inv[sam_joint];
    float m[4][4];
    double jp[3];
    float jr[3][3];
    Xf w;
    if (j >= kJ || !ReadJointModel(e, j, m) || !ReadWorld(reinterpret_cast<std::uintptr_t>(e), &w)) return false;
    // The position only (row 3): a foot scaled away under boots (section 21) keeps its position but not its axes.
    for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(m[3][k]) || std::fabs(m[3][k]) > 10.0f) return false;
        jp[k] = m[3][k];
        for (int c = 0; c < 3; ++c) jr[k][c] = k == c ? 1.0f : 0.0f;
    }
    const Xf h = Compose(w, jp, jr);
    const double dx = h.p[0] - w.p[0], dy = h.p[1] - w.p[1], dz = h.p[2] - w.p[2];
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) || dx * dx + dy * dy + dz * dz > 9.0) return false;
    pos[0] = h.p[0], pos[1] = h.p[1], pos[2] = h.p[2];
    return true;
}

void SetRidePose(int src) noexcept {
    if (src == 3) g_relax_want.store(true, std::memory_order_release);
    const int was = g_ride_src.exchange(src >= 1 && src <= 3 ? src : 0, std::memory_order_acq_rel);
    if (was != g_ride_src.load(std::memory_order_relaxed))
        Log("[real] riding: Sam's pose override %s", src == 1   ? "ON (the driver's seated pose from the partner's frames)"
                                                     : src == 2 ? "ON (the driver pose snapshot)"
                                                     : src == 3 ? "ON (seated idle: a held seated base, the hands on the thighs)"
                                                                : "off");
}
void SetRideSeat(int kind, double calm_mps) noexcept {
    g_seat_kind.store(kind >= 1 && kind <= 3 ? kind : 0, std::memory_order_relaxed);
    g_seat_calm_cms.store(calm_mps > 0 ? static_cast<std::uint32_t>(std::fmin(calm_mps, 600.0) * 100.0 + 1.0) : 0u, std::memory_order_relaxed);
}
void SetOwnDriving(int kind, double calm_mps) noexcept {
    g_own_kind.store(kind >= 1 && kind <= 3 ? kind : 0, std::memory_order_relaxed);
    g_own_calm_cms.store(calm_mps > 0 ? static_cast<std::uint32_t>(std::fmin(calm_mps, 600.0) * 100.0 + 1.0) : 0u, std::memory_order_relaxed);
}
void SetHandMeter(bool on) noexcept {
    if (on && !g_hand_meter.exchange(true, std::memory_order_acq_rel)) g_hand_gen.fetch_add(1, std::memory_order_acq_rel);  // a new window
    if (!on) g_hand_meter.store(false, std::memory_order_relaxed);
}
bool TakeHandTravel(float* right_cm, float* left_cm, unsigned* updates) noexcept {
    HandBox b;
    const bool ok = g_hand_box.Read(b) && b.gen == g_hand_gen.load(std::memory_order_acquire) && b.n > 0;
    g_hand_gen.fetch_add(1, std::memory_order_acq_rel);
    if (!ok) return false;
    float* out[2] = {right_cm, left_cm};
    for (int s = 0; s < 2; ++s) {
        double d2 = 0;
        for (int k = 0; k < 3; ++k) d2 += static_cast<double>(b.hi[s][k] - b.lo[s][k]) * (b.hi[s][k] - b.lo[s][k]);
        if (out[s]) *out[s] = static_cast<float>(std::sqrt(d2) * 100.0);
    }
    if (updates) *updates = b.n;
    return true;
}
void RidePoseTune(int what, double v) noexcept {
    switch (what) {
        case 0: g_ik_want.store(v != 0); if (v != 0) g_ik_off.store(false); break;
        case 1: if (v > 0.1 && v < 1.0) g_rest_along.store(static_cast<float>(v)); break;
        case 2: if (v > -0.2 && v < 0.3) g_rest_up.store(static_cast<float>(v)); break;
        case 3: if (v > -0.2 && v < 0.2) g_rest_in.store(static_cast<float>(v)); break;
        case 4: if (v > -120 && v < 120) g_rest_twist_deg.store(static_cast<float>(v)); break;
        default: break;
    }
}
bool RidePoseStatus(RidePoseStats* s, bool take) noexcept {
    if (!s) return false;
    s->ik_state = g_ik_state.load(std::memory_order_relaxed);
    s->ik_on = g_ik_want.load(std::memory_order_relaxed) && !g_ik_off.load(std::memory_order_relaxed);
    s->ik_off_by_error = g_ik_off.load(std::memory_order_relaxed);
    s->gate_err[0] = g_ik_gate_err[0].load(std::memory_order_relaxed);
    s->gate_err[1] = g_ik_gate_err[1].load(std::memory_order_relaxed);
    s->solves = g_ik_solves.load(std::memory_order_relaxed);
    s->fails = g_ik_fails.load(std::memory_order_relaxed);
    if (take) {
        const std::uint32_t n = g_ik_err_n.exchange(0, std::memory_order_relaxed);
        const std::uint64_t sum = g_ik_err_um_sum.exchange(0, std::memory_order_relaxed);
        s->err_n = n;
        s->err_mean_cm = n ? static_cast<float>(static_cast<double>(sum) / n / 1e4) : 0.0f;
        s->err_max_cm = static_cast<float>(g_ik_err_um_max.exchange(0, std::memory_order_relaxed) / 1e4);
    }
    s->latches = g_seat_latches.load(std::memory_order_relaxed);
    const std::uint32_t info = g_seat_latch_info.load(std::memory_order_relaxed);
    s->latch_kind = static_cast<int>(info & 0xF);
    s->latch_from = static_cast<int>((info >> 4) & 0xF);
    s->latch_final = ((info >> 8) & 1) != 0;
    s->latch_speed = static_cast<float>(info >> 12) / 100.0f;
    s->along = g_rest_along.load(), s->up = g_rest_up.load(), s->in = g_rest_in.load(), s->twist_deg = g_rest_twist_deg.load();
    return true;
}
int RidePose() noexcept { return g_ride_src.load(std::memory_order_relaxed); }
bool SnapshotRidePose(int kind) noexcept {
    Capture c;
    if (!g_capture.Read(c) || c.frame == 0) return false;
    if (!g_ride_snap.Write(c)) return false;
    g_snap_kind.store(kind, std::memory_order_relaxed);
    g_ride_have_snap.store(true, std::memory_order_release);
    return true;
}
void RidePoseCounters(std::uint32_t* writes, std::uint32_t* nosrc, std::uint32_t* restores, std::uint32_t* failed) noexcept {
    if (writes) *writes = g_ride_writes.load(std::memory_order_relaxed);
    if (nosrc) *nosrc = g_ride_nosrc.load(std::memory_order_relaxed);
    if (restores) *restores = g_ride_restores.load(std::memory_order_relaxed);
    if (failed) *failed = g_ride_failed.load(std::memory_order_relaxed);
}
bool PartnerFrameFresh(unsigned max_age_ms) noexcept { return GetTickCount64() - g_apply_tick.load(std::memory_order_acquire) < max_age_ms; }
bool ReadJointModelAny(game::Entity* e, unsigned j, unsigned* count, float (&m)[4][4], char* name, std::size_t name_cap) noexcept {
    const std::uintptr_t ent = reinterpret_cast<std::uintptr_t>(e);
    const std::uintptr_t sm = e ? ModelOf(ent) : 0;
    if (!sm) return false;
    std::uint8_t hdr[kPoseSize];
    std::uintptr_t pose = 0;
    if (!ReadPoseHdr(sm, hdr, &pose) || Field<std::int32_t>(hdr, kHdrState) != kPoseStateModel) return false;
    const std::uintptr_t mb = Field<std::uintptr_t>(hdr, kHdrModel), me = Field<std::uintptr_t>(hdr, kHdrModelEnd);
    if (!mb || me <= mb || (me - mb) % 0x40 != 0 || (me - mb) / 0x40 > 1024) return false;
    const unsigned n = static_cast<unsigned>((me - mb) / 0x40);
    if (count) *count = n;
    if (j >= n || !mem::Read(mb + 0x40ull * j, m, sizeof(m))) return false;
    if (name && name_cap) {
        name[0] = 0;
        const SkelInfo si = PoseSkeleton(sm);
        if (si.skel && si.count > static_cast<std::int32_t>(j)) std::snprintf(name, name_cap, "%s", JointName(si, j).c_str());
    }
    return true;
}

void ConfigureSelf(const bodies::Body* body) noexcept {
    g_self_body = body;
    g_self_look = nullptr;

    g_self_on = body != nullptr;
    g_me.tag = "self";
    if (body) Log("[self] self view on: '%s' (%s) is shown in place of the local Sam on this screen", body->key, body->label);
}
bool SelfConfigured() noexcept { return g_self_on && g_self_body; }
// The body she is wearing (a live or loading one), else the one she will wear: a wanted room-outfit change may wait
// (up to 3 s, or through a cutscene or menu) and her boots must follow the worn body, not the wanted one.
const bodies::Body* SelfBody() noexcept {
    if (!g_self_on || !g_self_body) return nullptr;
    return g_me.phase != Phase::idle && g_me.body ? g_me.body : SelfLook();
}
void ConfigureRoomOutfit(bool black) noexcept {
    g_room_choice = black ? roomoutfit::Choice::black : roomoutfit::Choice::suit;
    Log("[self] room outfit: room_outfit=%s (%s)", roomoutfit::ChoiceName(g_room_choice),
        black ? "Fragile in her porter suit wears her black suit in the private room, on both screens" : "her porter suit in the private room too");
}
std::uint8_t LookBodyId(const bodies::Body* base, game::Entity* local) noexcept {
    if (!base) return 0;
    if (g_self_on && g_self_body == base) return SelfLook()->id;  // what her own screen shows (or is about to)
    // the self view off: the same rule from the game's state and Sam's variant (game thread)
    static const bodies::Body* last = nullptr;
    static const bodies::Body* last_base = nullptr;
    if (last_base != base) last = base, last_base = base;
    std::uint64_t gs = 0;
    game::LoadOrSetup(&gs, nullptr);
    const bodies::Body* b = RoomLook(base, reinterpret_cast<std::uintptr_t>(local), gs, last, nullptr, nullptr);
    if (b && b != last) {
        Log("[sync] room outfit (self view off): sending '%s' (was '%s')", b->key, last ? last->key : "?");
        last = b;
    }
    return last ? last->id : base->id;
}
void ConfigureSelfFit(bool on) noexcept {
    g_fit_cfg.store(on);
    if (!on) FitOff();
    Log("[self] fit %s (session.ini self_fit)", on ? "on: Sam's hidden skeleton takes the character's bone lengths while the self body is shown"
                                                    : "off: Sam's gear stays on his own proportions");
}
bool HeadLift(game::Entity* sam, float (&local)[3]) noexcept {
    local[0] = local[1] = local[2] = 0;
    if (!sam || reinterpret_cast<std::uintptr_t>(sam) != g_sam_e.load(std::memory_order_acquire) || !g_fit_want.load(std::memory_order_acquire))
        return false;
    LiftPub lp;
    if (!g_lift_pub.Read(lp) || !lp.on) return false;
    for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(lp.local[k]) || std::fabs(lp.local[k]) > 0.2f) return false;
        local[k] = lp.local[k];
    }
    return true;
}

bool SelfHeadItemOffset(int item, float (&off)[3]) noexcept {
    off[0] = off[1] = off[2] = 0;
    if (item < 0 || item >= kHeadItems || !g_hg_cfg.load(std::memory_order_relaxed) || g_hg_row.load(std::memory_order_relaxed) < 0 ||
        !g_fit_want.load(std::memory_order_acquire) || !g_fit_cfg.load(std::memory_order_relaxed) || g_fit_faulted.load(std::memory_order_relaxed))
        return false;
    for (int k = 0; k < 3; ++k) off[k] = static_cast<float>(g_hg_mm[item][k].load(std::memory_order_relaxed)) / 1000.0f;
    return true;
}
void SetHeadgearReady(bool ready) noexcept { g_hg_ready_ms.store(ready ? GetTickCount64() : 0, std::memory_order_release); }
void PublishHeadMovers(const HeadMoverPub* m, unsigned n) noexcept {
    static HeadMoverSet s_pub;  // the last published (game thread)
    if (n > kMaxHeadMovers) n = kMaxHeadMovers;
    bool same = s_pub.n == n;
    for (unsigned i = 0; i < n && same; ++i)
        same = s_pub.m[i].mover == m[i].mover && s_pub.m[i].e == m[i].e && std::memcmp(s_pub.m[i].off, m[i].off, sizeof(m[i].off)) == 0;
    if (same) return;
    bool dropped = false;
    for (unsigned i = 0; i < s_pub.n; ++i) {
        bool kept = false;
        for (unsigned k = 0; k < n && !kept; ++k) kept = m[k].mover == s_pub.m[i].mover;
        dropped = dropped || !kept;
    }
    HeadMoverSet next;
    next.n = n;
    next.gen = s_pub.gen + 1;
    for (unsigned i = 0; i < n; ++i) next.m[i] = m[i];
    g_hms_n.store(0, std::memory_order_seq_cst);
    for (int tries = 0; tries < 1000 && !g_hms.Write(next); ++tries) SwitchToThread();
    s_pub = next;
    g_hms_n.store(n, std::memory_order_seq_cst);
    if (dropped) {
        // a mover leaves the set: no Sam update may still write into it once this returns (the caller restores it next)
        const ULONGLONG until = GetTickCount64() + 50;
        while ((g_hc_busy.load(std::memory_order_seq_cst) || g_inflight.load(std::memory_order_seq_cst) != 0) && GetTickCount64() < until) SwitchToThread();
    }
}
bool ReadHeadMoverLocal(std::uintptr_t local, float (&m)[4][4]) noexcept {
    // Sam's update writes these 64 bytes under g_hc_busy (HeadCorrect): the read takes the same flag, so it never sees half
    // of one frame's matrix and half of the next. Busy for over 2 ms: no read (the caller skips this tick).
    const ULONGLONG until = GetTickCount64() + 2;
    while (g_hc_busy.exchange(true, std::memory_order_acquire)) {
        if (GetTickCount64() > until) return false;
        SwitchToThread();
    }
    const bool ok = mem::Read(local, m, sizeof(m));
    g_hc_busy.store(false, std::memory_order_release);
    return ok;
}
void HeadCorrInfo(char* out, std::size_t cap) noexcept {
    const std::uint64_t n = g_hc_um_n.exchange(0);
    const std::uint64_t sum = g_hc_um_sum.exchange(0), mx = g_hc_um_max.exchange(0);
    std::snprintf(out, cap, "her drawn head %s: %u corrected, %u plain (%u no fresh body update, %u over 1.5 m / 45 deg), %u busy, %u aba, %u failed writes; "
                  "body records %u; correction cm mean %.2f max %.2f over %llu",
                  g_hcorr_on.load() ? "on" : "OFF (self_view 16 0)", g_hc_corr.exchange(0), g_hc_plain.exchange(0), g_hc_stale.exchange(0), g_hc_big.exchange(0),
                  g_hc_skip.exchange(0), g_hc_aba.exchange(0), g_hc_wpm.exchange(0), g_hc_recs.exchange(0), n ? static_cast<double>(sum) / n / 1e4 : 0.0,
                  static_cast<double>(mx) / 1e4, static_cast<unsigned long long>(n));
}
void HeadItemOnJoint(const float (&off)[3], float (&p)[3], float (&r)[3][3]) noexcept {
    for (int c = 0; c < 3; ++c) {
        p[c] = off[0] * kHlp[0][c] + off[1] * kHlp[1][c] + off[2] * kHlp[2][c];
        for (int k = 0; k < 3; ++k) r[c][k] = kHlp[c][k];
    }
}
bool HeadgearWanted() noexcept {
    return HeadgearRowWanted() && g_fit_want.load(std::memory_order_acquire) && g_fit_cfg.load(std::memory_order_relaxed) &&
           !g_fit_faulted.load(std::memory_order_relaxed);
}
void ConfigureSelfHeadgear(bool on) noexcept {
    g_hg_cfg.store(on);
    Log("[self] headgear %s (session.ini self_headgear)", on ? "on: Sam's cap and glasses get offsets of their own for the character's head (no head lift)"
                                                            : "off: the head lift (v0.11)");
}
void ConfigureSelfFeet(bool on) noexcept {
    g_feet_cfg.store(on);
    Log("[self] feet %s (session.ini self_feet_hide)", on ? "hidden under Sam's equipped boots" : "kept (her own boots under his)");
}
void SetSamBootsShown(bool shown) noexcept { g_feet_boots.store(shown, std::memory_order_relaxed); }
void SetPartnerFeetHidden(bool on, int pct) noexcept {
    g_pfeet_pct.store(pct < 1 ? 1 : pct > 100 ? 100 : pct, std::memory_order_relaxed);
    g_pfeet_on.store(on, std::memory_order_release);
}
void FeetCounters(std::uint32_t* self_writes, std::uint32_t* partner_writes) noexcept {
    if (self_writes) *self_writes = g_feet_writes.load();
    if (partner_writes) *partner_writes = g_pfeet_writes.load();
}
bool JointFrameWorld(game::Entity* e, unsigned sam_joint, double (&p)[3], float (&r)[3][3]) noexcept {
    if (!e || sam_joint >= kJ) return false;
    unsigned j = sam_joint;
    for (BodySlot* b : {&g_p, &g_me})
        if (b->phase == Phase::live && b->spawn.e == e) j = b->inv[sam_joint];
    float m[4][4];
    double jp[3];
    float jr[3][3];
    Xf w;
    if (j >= kJ || !ReadJointModel(e, j, m) || !JointFrame(m, jp, jr) || !ReadWorld(reinterpret_cast<std::uintptr_t>(e), &w)) return false;
    const Xf h = Compose(w, jp, jr);
    std::memcpy(p, h.p, sizeof(p));
    std::memcpy(r, h.r, sizeof(r));
    return true;
}

void EmergencyStop() noexcept {
    // The detour stops touching anything (no capture, no transform copy, no pose write, no fit).
    FitOffWait();
    g_ride_src.store(0, std::memory_order_seq_cst);  // riding: no pose override (Sam's own pose stays as the game wrote it)
    g_sam_sm.store(0, std::memory_order_seq_cst);
    g_sam_e.store(0, std::memory_order_seq_cst);
    g_self_follow.store(3, std::memory_order_seq_cst);
    g_self_src.store(0, std::memory_order_seq_cst);
    g_menu_sm.store(0, std::memory_order_seq_cst);
    g_menu_e.store(0, std::memory_order_seq_cst);
    g_gear_n[0].store(0, std::memory_order_seq_cst);
    g_gear_n[1].store(0, std::memory_order_seq_cst);
    g_pt_live.store(0, std::memory_order_seq_cst);
    g_bt.sm.store(0, std::memory_order_seq_cst);
    g_xt[0].sm.store(0, std::memory_order_seq_cst);
    g_xt[1].sm.store(0, std::memory_order_seq_cst);
    g_pfeet_on.store(false, std::memory_order_seq_cst);
    g_hms_n.store(0, std::memory_order_seq_cst);  // Sam's update stops correcting head items (partner_gear puts the movers back)
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
    // The cargo menu's figure: its parts back (fault-free checks, SEH-guarded calls), our body hidden.
    if (g_menu_shown) {
        char w[160] = {};
        menufig::Restore(&g_menu_hidden, w, sizeof(w));
        if (g_me.phase == Phase::live && holo::SpawnAlive(g_me.spawn)) partner::SetVisible(g_me.spawn.e, false);
        g_menu_shown = false;
        Log("[menu] emergency stop: the figure %s", w);
    }
    g_menu_active = false;
    Log("[self] emergency stop (the per-frame sync faulted): detour work off%s%s", was_shown ? "; Sam shown again: reason 63 " : "",
        was_shown ? (cleared ? (hid ? "cleared, body hidden" : "cleared, body NOT hidden") : "NOT cleared") : "");
}
bool SelfShown() noexcept { return g_self_shown; }
std::uintptr_t MenuSelfFigure() noexcept { return g_menu_active ? g_menu_e.load(std::memory_order_relaxed) : 0; }
game::Entity* SelfEntity() noexcept {
    if (g_me.phase == Phase::idle) return nullptr;
    return g_me.spawn.e;
}

bool SetEnemySlot(unsigned k, std::uintptr_t sm, EnemySlotMode m) noexcept {
    if (!g_hook || k >= kEnemySlots) return false;
    EnemySlot& e = g_es[k];
    if (e.sm.exchange(0, std::memory_order_seq_cst)) g_et_any.fetch_sub(1, std::memory_order_seq_cst);
    // a detour inside THIS slot finishes (its own counter: the partner body's and the other slots' work never stall the game
    // thread here; review rv-safety 12)
    for (int i = 0; i < 200 && e.busy.load(std::memory_order_seq_cst) != 0; ++i) Sleep(0);
    e.mode.store(static_cast<int>(m), std::memory_order_relaxed);
    // a new model starts with nothing captured and no frame (never another enemy's pose)
    static const EnemyCap kNoCap{};
    static const EnemyPush kNoPush{};
    e.cap.Write(kNoCap);
    e.push.Write(kNoPush);
    if (!sm || m == EnemySlotMode::off || g_et_faulted.load()) return !sm || m == EnemySlotMode::off;
    std::uintptr_t vt = 0;
    if (!Rd(sm, vt) || vt != g_sm_vt) return false;
    const SkelInfo s = PoseSkeleton(sm);
    if (!s.skel || s.count != static_cast<std::int32_t>(kJ) || s.layout != wire::kSamLayoutHash) return false;
    e.sm.store(sm, std::memory_order_seq_cst);
    g_et_any.fetch_add(1, std::memory_order_seq_cst);
    return true;
}

std::uintptr_t EnemySlotModel(unsigned k) noexcept { return k < kEnemySlots ? g_es[k].sm.load() : 0; }

bool CaptureEnemyFrame(unsigned k, wire::JointFrame* out, double* captured_ms) noexcept {
    if (k >= kEnemySlots || !out) return false;
    static EnemyCap c;  // game thread
    if (!g_es[k].cap.Read(c) || !c.n) return false;
    out->seq = c.n;
    out->joint_count = kJ;
    out->layout_hash = wire::kSamLayoutHash;
    out->body_id = 0;
    out->flags = wire::kJointsValid;
    for (unsigned j = 0; j < kJ; ++j) out->q[j] = wire::Normalize(wire::Quat{c.j[j].q[0], c.j[j].q[1], c.j[j].q[2], c.j[j].q[3]});
    out->t_count = 2;
    out->t_joint[0] = kSamRoot;
    out->t_joint[1] = kSamPelvis;
    std::memcpy(out->t[0], c.j[kSamRoot].t, sizeof(out->t[0]));
    std::memcpy(out->t[1], c.j[kSamPelvis].t, sizeof(out->t[1]));
    if (captured_ms) *captured_ms = c.ms;
    return true;
}

bool PushEnemyFrame(unsigned k, const wire::JointFrame& f) noexcept {
    if (k >= kEnemySlots || f.joint_count != kJ || f.layout_hash != wire::kSamLayoutHash) return false;
    static EnemyPush p;  // game thread
    static std::uint32_t seq = 0;
    p.seq = ++seq ? seq : ++seq;
    for (unsigned j = 0; j < kJ; ++j) p.q[j] = f.q[j];
    p.have_pelvis = false;
    for (unsigned i = 0; i < f.t_count && i < wire::kMaxJointTranslations; ++i)
        if (f.t_joint[i] == kSamPelvis) std::memcpy(p.tpelvis, f.t[i], sizeof(p.tpelvis)), p.have_pelvis = true;
    return g_es[k].push.Write(p);
}

EnemySlotStats GetEnemySlotStats(unsigned k, bool reset) noexcept {
    EnemySlotStats s;
    if (k >= kEnemySlots) return s;
    EnemySlot& e = g_es[k];
    s.calls = e.calls.load(), s.writes = e.writes.load(), s.rejects = e.rejects.load(), s.faults = e.faults.load();
    s.captures = e.captures.load(), s.applies = e.applies.load(), s.no_frame = e.no_frame.load();
    s.max_err_deg = static_cast<float>(e.err_mdeg.load()) / 1000.0f;
    if (reset) {
        e.calls.store(0), e.writes.store(0), e.rejects.store(0), e.err_mdeg.store(0), e.applies.store(0), e.no_frame.store(0);
    }
    return s;
}

bool EnemyPoseFaulted() noexcept { return g_et_faulted.load(); }

// The P1-0 probe's single slot (slot 7): mode 1 count, 3 our Sam's pose (probe_sam), 0 off.
bool SetEnemyPoseSlot(std::uintptr_t sm, int mode) noexcept {
    return SetEnemySlot(kEnemySlots - 1, sm, mode == 1 ? EnemySlotMode::count : mode == 3 ? EnemySlotMode::probe_sam : EnemySlotMode::off);
}

EnemySlotStats GetEnemySlotStats(bool reset) noexcept { return GetEnemySlotStats(kEnemySlots - 1, reset); }

void Tick(game::Entity* local, bool local_active, double now_ms) noexcept {
    g_now = now_ms;
    HeadMeterSam();  // hidden tests: before this tick moves Sam (section 26.8)
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
                g_interp_on.store(false, std::memory_order_release);  // a mirror frame: the detour plays it as is
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
    if (g_hook) {
        // Section 36: Sam's facial clip on her self view's face (shown in his place) and the partner's received one on the
        // partner body.
        facesync::TickIn fi;
        fi.sam = local && local_active ? reinterpret_cast<std::uintptr_t>(local) : 0;
        // Only a body whose spawn is still alive (the check DriveModel makes before its own game calls): a body the game
        // destroyed is not given to the face sync, which then drops its rig.
        if (g_self_on && g_self_shown && g_me.phase == Phase::live && g_me.body && holo::SpawnAlive(g_me.spawn)) {
            fi.self_body = reinterpret_cast<std::uintptr_t>(g_me.spawn.e);
            fi.self_epoch = g_self_shows * 0x10000u + (g_me.lives & 0xFFFFu);
            fi.self_key = g_me.body->key;
        }
        if (g_p.phase == Phase::live && g_p.owner == Owner::partner && g_p.body && holo::SpawnAlive(g_p.spawn)) {
            fi.partner_body = reinterpret_cast<std::uintptr_t>(g_p.spawn.e);
            fi.partner_epoch = g_p.lives;
            fi.partner_key = g_p.body->key;
        }
        facesync::Tick(fi, now_ms);
    }
    if (g_hook && (g_self_on || automation::Enabled())) MenuWatch(now_ms);  // hidden tests: also with the self view off
    DriveModel(g_p, local_active);
    DriveModel(g_me, local_active || g_menu_active);
    DriveFreq(g_p, local_active);
    DriveFreq(g_me, local_active);
    DriveGearFreq(local_active, now_ms);
    DrivePoseTargets(local_active);
    DriveBootsTarget(local_active);
    DriveExoTargets(local_active);
    DressPartner(local, local_active, now_ms);
    WardrobeTick(local, local_active, now_ms);
    DressFragile(local, local_active, now_ms);
    if (g_hook) Stats(now_ms);
    Meter(now_ms);
}

bool CaptureFrame(wire::JointFrame* out, double* capture_ms) noexcept {
    Capture c;
    if (!g_capture.Read(c) || GetTickCount64() - c.tick_ms > 250) return false;
    if (capture_ms) *capture_ms = g_qpc_per_ms > 0 ? static_cast<double>(c.qpc) / g_qpc_per_ms : 0;
    return FrameFromCapture(c, out);
}

void ConfigureSmooth(unsigned mask, bool echo, double echo_delay_ms) noexcept {
    g_smooth.store(mask & kSmoothAll);
    g_meter_echo = echo;
    g_meter_echo_delay_ms = echo_delay_ms;
    Log("[smooth] smooth_pose mask %u: 240 Hz body/prop updates %s, per-update interpolation %s, capture-time stamps %s%s", mask & kSmoothAll,
        (mask & kSmoothBoost) ? "on" : "off", (mask & kSmoothDetourInterp) ? "on" : "off", (mask & kSmoothCaptureStamp) ? "on" : "off",
        echo ? " (echo: the meter compares the partner with our own Sam's captured pose)" : "");
}

unsigned SmoothMask() noexcept { return g_smooth.load(std::memory_order_relaxed); }

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
    if (!local || !outfit::ReadLocal(reinterpret_cast<std::uintptr_t>(local), out)) return false;
    out->suit_colour = suitcol::GuestColour(out->suit_colour);  // round 5: a host-held save guest's remembered pick (else unchanged)
    return true;
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
    PublishFrame(out, play_ms);
    // Smooth path: the bracketing frames themselves, for the detour to interpolate at its own time.
    if (!(g_smooth.load(std::memory_order_relaxed) & kSmoothDetourInterp) || g_qpc_per_ms <= 0) {
        g_interp_on.store(false, std::memory_order_release);
        return true;
    }
    Capture c;
    if (!g_capture.Read(c)) return true;  // no local Sam pose for the fallback translations yet
    static InterpBuf ib;
    unsigned first = 0, n = 1;
    if (g_ring_count == 1 || play_ms >= newest.key_ms) {
        first = g_ring_count - 1;
    } else {
        unsigned i = 1;  // the first frame keyed after play_ms (play before the oldest: 1, alpha clamps to 0)
        while (i < g_ring_count - 1 && RingAt(i).key_ms <= play_ms) ++i;
        first = i - 1;
        n = i + 1 < g_ring_count ? 3 : 2;
    }
    ib.n = n;
    for (unsigned k = 0; k < n; ++k) {
        const RingSlot& r = RingAt(first + k);
        ib.key[k] = r.key_ms;
        for (unsigned j = 0; j < kJ; ++j) ib.q[k][j] = j < r.f.joint_count ? r.f.q[j] : wire::Quat{};
        std::memcpy(ib.troot[k], c.j[kSamRoot].t, sizeof(ib.troot[k]));
        std::memcpy(ib.tpelvis[k], c.j[kSamPelvis].t, sizeof(ib.tpelvis[k]));
        for (unsigned t = 0; t < r.f.t_count; ++t) {
            if (r.f.t_joint[t] == kSamRoot) std::memcpy(ib.troot[k], r.f.t[t], sizeof(ib.troot[k]));
            if (r.f.t_joint[t] == kSamPelvis) std::memcpy(ib.tpelvis[k], r.f.t[t], sizeof(ib.tpelvis[k]));
        }
    }
    std::memcpy(ib.fb, c.j, sizeof(ib.fb));
    ib.play_ref = play_ms;
    ib.ref_ms = static_cast<double>(Qpc()) / g_qpc_per_ms;
    if (g_interp.Write(ib)) g_interp_on.store(true, std::memory_order_release);
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
        case 8: {  // self view <0/1> [body id, 0 = the configured one] [follow 1 Sam's update / 2 the body's / 3 tick / 4 predicted, 0 = unchanged]
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
                // reason 63 is ours: it goes where the self view puts it (the global part mask); any
                // other reason is a game's (the Root mask)
                const std::uintptr_t a = reason == 63 ? SamHideAddr(reinterpret_cast<std::uintptr_t>(sam)) : SamMaskAddr(reinterpret_cast<std::uintptr_t>(sam));
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
            if (v[0] == 3) {  // self_view 3 <0/1>: the cargo menu's figure replaced (on) or kept Sam (off)
                g_menu_on = v[1] != 0;
                if (!g_menu_on) MenuEnd("the menu figure switched off (debug)");
                Set(msg, cap, "cargo menu figure: %s", g_menu_on ? "the self body in its place" : "Sam");
                return true;
            }
            if (v[0] == 5) {  // self_view 5 <0/1>: the fit (section 16) on / off
                g_fit_cfg.store(v[1] != 0);
                const char* fit = nullptr;
                if (v[1] == 0) FitOff();
                else if (g_self_shown) fit = FitOn();
                Set(msg, cap, "self view fit %s%s%s", v[1] != 0 ? "on" : "off", fit ? ": " : "", fit ? fit : "");
                Log("[self] %s", msg);
                return true;
            }
            if (v[0] == 6) {  // self_view 6 <0/1>: the fit's same pose as the body (section 16.1) on / off
                g_fit_sync.store(v[1] != 0);
                Set(msg, cap, "self view fit: %s", v[1] != 0 ? "same pose as the body (the pose it was last written from)" : "Sam's newest pose");
                Log("[self] %s", msg);
                return true;
            }
            if (v[0] == 7) {  // self_view 7 <percent>: the fit's head lift (section 16.1), 0-150
                const int pct = v[1] < 0 ? 0 : v[1] > 150 ? 150 : static_cast<int>(v[1]);
                g_lift_pct.store(pct);
                Set(msg, cap, "self view fit: head lift %d%% (measured %.1f cm, state %d)", pct, static_cast<double>(g_lift_cm.load()), g_lift_diag.load());
                Log("[self] %s", msg);
                return true;
            }
            if (v[0] == 4) {  // self_view 4: the menu figure's state and parts in the log
                menufig::View mv;
                const bool ok = menufig::Read(&mv);
                menufig::LogParts(mv);
                Set(msg, cap, "subspace %s: state %d kind %d/%d figure %p %s, %d parts; %s, shown %d", ok ? "read" : menufig::Why(), mv.state, mv.kind,
                    mv.kind_next, reinterpret_cast<void*>(mv.e), mv.awake ? "awake" : "asleep", mv.parts, g_menu_active ? "the self body follows it" : "not followed",
                    g_menu_shown ? 1 : 0);
                Log("[menu] %s", msg);
                return ok;
            }
            if (v[0] == 8 || v[0] == 9) {
                // self_view 8|9 <which> <mm>: the headgear offset up (8) / forward (9); which: 0 every cap, 1 every pair of
                // glasses, 10-17 one item (10 eyewear0, 11-13 glasses00-02, 14-17 cap00-03)
                const int which = static_cast<int>(v[1]);
                const bool one = which >= 10 && which < 10 + kHeadItems;
                if (!(which == 0 || which == 1 || one) || !(v[2] >= -100 && v[2] <= 100)) {
                    Set(msg, cap, "self_view %d <0 caps / 1 glasses / 10-17 one item> <mm -100..100>", static_cast<int>(v[0]));
                    return false;
                }
                int first = 0, last = kHeadItems - 1;
                if (one) first = last = which - 10;
                else if (which == kHeadItemCap) first = kHeadItemFirstCap;
                else last = kHeadItemFirstCap - 1;
                for (int i = first; i <= last; ++i) g_hg_mm[i][v[0] == 8 ? 2 : 1].store(static_cast<int>(v[2]));
                g_hg_tuned.store(true);
                static const char* const kItemName[kHeadItems] = {"eyewear0", "glasses00", "glasses01", "glasses02", "cap00", "cap01", "cap02", "cap03"};
                Set(msg, cap, "self view headgear: %s offset (x %d, forward %d, up %d mm)%s", one ? kItemName[first] : which == kHeadItemCap ? "every cap" : "every glasses",
                    g_hg_mm[first][0].load(), g_hg_mm[first][1].load(), g_hg_mm[first][2].load(),
                    g_hg_row.load() >= 0 ? "" : " (this body has no headgear row: not used)");
                Log("[self] %s", msg);
                return true;
            }
            if (v[0] == 10) {  // self_view 10 <0/1>: the headgear offsets (on) or the v0.11 head lift (off)
                g_hg_cfg.store(v[1] != 0);
                Set(msg, cap, "self view headgear %s (head lift now %d%%)", v[1] != 0 ? "on" : "off", EffectiveLiftPct());
                Log("[self] %s", msg);
                return true;
            }
            if (v[0] == 11) {  // self_view 11 <percent>: her feet's scale under Sam's boots (100: not scaled)
                const int pct = v[1] < 1 ? 1 : v[1] > 100 ? 100 : static_cast<int>(v[1]);
                g_feet_pct.store(pct);
                Set(msg, cap, "self view feet: scale %d%% (%s; Sam's boots %s)", pct, g_feet_cfg.load() ? "on" : "off (self_feet_hide=0)",
                    g_feet_boots.load() ? "shown" : "not shown");
                Log("[self] %s", msg);
                return true;
            }
            if (v[0] == 12) {  // self_view 12 <0/1>: her feet hidden under Sam's boots on / off
                g_feet_cfg.store(v[1] != 0);
                Set(msg, cap, "self view feet %s", v[1] != 0 ? "hidden under Sam's boots" : "kept");
                Log("[self] %s", msg);
                return true;
            }
            if (v[0] == 13) {  // self_view 13 <0/1>: follow mode 4's rule: 1 v0.20 (default), 0 v0.19's (for before / after pictures)
                g_pred_rule.store(v[1] != 0 ? 1 : 0);
                Set(msg, cap, "self view follow 4 rule %s", v[1] != 0 ? "v0.20 (the step also seated, the consistency gate, the dt scale)"
                                                                       : "v0.19 (no step while seated, no gate, no scale)");
                Log("[self] %s", msg);
                return true;
            }
            if (v[0] == 22) {  // self_view 22 <0/1>: the far hips rule (section 38.3): 1 (default), 0 v0.26's plain ratio
                g_far_hips_rule.store(v[1] != 0);
                Set(msg, cap, "self view far hips rule %s", v[1] != 0 ? "on (beyond 1.45 m from the root: Sam's hips minus her rest offset)"
                                                                      : "off (v0.26: Sam's hips times the ratio everywhere)");
                Log("[self] %s", msg);
                return true;
            }
            if (v[0] == 23) {  // self_view 23 <0/1>: the follow 4 trace around snaps (section 38.2), off by default
                g_tr_on.store(v[1] != 0);
                Set(msg, cap, "self view follow 4 trace %s", v[1] != 0 ? "on" : "off");
                Log("[self] %s", msg);
                return true;
            }
            if (v[0] == 14) {  // self_view 14 <mask>: the gear placement fixes (section 26.7): 3 v0.21 (default), 0 v0.20
                const int mask = static_cast<int>(v[1]) & 3;
                g_gear_fix.store(mask);
                Set(msg, cap, "gear placement fixes %d: self props in the self body's update %s, the tick's placement extrapolated %s", mask,
                    mask & 1 ? "on" : "off", mask & 2 ? "on" : "off");
                Log("[gear] %s", msg);
                return true;
            }
            if (v[0] == 15) {  // hidden tests only: self_view 15 <0/1>: the drawn-vs-anchor meter (section 26.7)
                if (v[1] != 0) {
                    char why[160] = "";
                    if (!automation::Enabled() || !automation::SavesRedirected()) {
                        Set(msg, cap, "refused: only in a hidden test run on scratch saves");
                        return false;
                    }
                    if (!GearMeterSites(why, sizeof(why))) {
                        Set(msg, cap, "drawn-vs-anchor meter off: %s", why);
                        Log("[gear] %s", msg);
                        return false;
                    }
                }
                g_gm_on.store(v[1] != 0);
                Set(msg, cap, "drawn-vs-anchor meter %s", v[1] != 0 ? "on" : "off");
                Log("[gear] %s", msg);
                return true;
            }
            if (v[0] == 17) {  // self_view 17 <mask>: the load rule (1), the room close-up hold and stall guard (2), the clothes rule (4); 5 default, 3 v0.25, 0 v0.24
                const int m = static_cast<int>(v[1]) & 7;
                g_self_load_rules.store(m);
                Set(msg, cap, "self view over loads: load rule %s, private room close-up hold and stall guard %s, clothes rule %s", (m & 1) ? "on" : "off (v0.24)",
                    (m & 2) ? "on (v0.25)" : "off", (m & 4) ? "on" : "off (v0.25)");
                Log("[self] %s", msg);
                return true;
            }
            if (v[0] == 18) {  // self_view 18 <mode>: her room outfit: 0 never, 1 the rule (default), 2 always the room body, 3 always the base
                const int m = static_cast<int>(v[1]);
                if (m < 0 || m > 3) {
                    Set(msg, cap, "self_view 18 <0 never / 1 the rule / 2 always the room body / 3 always the base>");
                    return false;
                }
                g_room_force.store(m);
                Set(msg, cap, "room outfit: %s (room_outfit=%s; wanted now '%s', worn '%s')",
                    m == 0 ? "never" : m == 1 ? "the rule" : m == 2 ? "always the room body (forced)" : "always the base (forced)",
                    roomoutfit::ChoiceName(g_room_choice), g_self_body ? SelfLook()->key : "-", g_me.body ? g_me.body->key : "-");
                Log("[self] %s", msg);
                return true;
            }
            if (v[0] == 19) {  // hidden tests only: self_view 19 <request>: Sam's body variant request (0..7, 8 the game's own choice)
                if (!automation::Enabled() || !automation::SavesRedirected()) {
                    Set(msg, cap, "refused: only in a hidden test run on scratch saves");
                    return false;
                }
                if (!have_sam || v[1] < 0 || v[1] > 8) {
                    Set(msg, cap, "self_view 19 <0..7 a variant / 8 the game's own choice> needs a local player");
                    return false;
                }
                return outfit::TestRequestVariant(reinterpret_cast<std::uintptr_t>(sam), static_cast<std::uint8_t>(v[1]), msg, cap);
            }
            if (v[0] == 20) return facesync::SetMode(static_cast<int>(v[1]), msg, cap);  // self_view 20 <mask>: her face (1), the partner's (2)
            if (v[0] == 21) {  // self_view 21: the face sync's status line
                facesync::Status(msg, cap);
                return true;
            }
            if (v[0] == 16) {  // self_view 16 <0/1>: Sam's head items onto her drawn head (section 26.8): 1 v0.22 (default), 0 v0.21
                g_hcorr_on.store(v[1] != 0 ? 1 : 0);
                Set(msg, cap, "self view head items %s", v[1] != 0 ? "on her drawn head (corrected every frame in Sam's update)"
                                                                    : "on Sam's head with the plain offset (v0.21)");
                Log("[headgear] %s", msg);
                return true;
            }
            const int follow = static_cast<int>(v[2]);
            if (follow >= 1 && follow <= 4) g_self_follow.store(follow);
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
            g_self_look = nullptr;
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
                // round 15: a pick, as the wardrobe's close (b = 1: only the byte, as a load or a script's ApplyInventorySetting
                // would write it: not a pick)
                if (ok && v[2] == 0) suitcol::NotePick(static_cast<std::uint8_t>(v[1]));
                Set(msg, cap, "our own suit colour -> %.0f: %s%s", v[1], ok ? "written" : "not written", v[2] != 0 ? " (not a pick)" : "");
                return ok;
            }
            if (mode == 9) {  // round 15 (section 40): bit 1 her Sam's byte follows her pick, bit 2 Fragile in the wardrobe
                const int m = v[1] >= 0 && v[1] <= 3 ? static_cast<int>(v[1]) : 3;
                if (((m & 1) != 0) != suitcol::AlignOn()) suitcol::SetAlign((m & 1) != 0);
                g_wardrobe_on = (m & 2) != 0;
                if (!g_wardrobe_on && g_menu_wardrobe) MenuEnd("the wardrobe figure switched off (debug)");
                Set(msg, cap, "wardrobe: her Sam's byte follows her pick %d, Fragile in the wardrobe %d", (m & 1) ? 1 : 0, (m & 2) ? 1 : 0);
                Log("[suit] %s", msg);
                return true;
            }
            if (mode == 11) {  // round 15, hidden tests only: open the wardrobe with the call the room makes (private room)
                std::uint64_t gs = 0;
                game::LoadOrSetup(&gs, nullptr);
                if (!automation::Enabled() || !automation::SavesRedirected() || !(gs & game::kGameStatePrivateRoom)) {
                    Set(msg, cap, "refused: only in a hidden test run on scratch saves, in the private room");
                    return false;
                }
                return wardrobe::TestOpen(msg, cap);
            }
            if (mode == 12) {  // round 15 review, hidden tests only: the open wardrobe counts as if a suit colour were previewed
                if (!automation::Enabled() || !automation::SavesRedirected()) {
                    Set(msg, cap, "refused: only in a hidden test run on scratch saves");
                    return false;
                }
                const bool ok = suitcol::MarkPreview();
                Set(msg, cap, ok ? "a suit colour preview noted in the open wardrobe" : "the wardrobe is not open");
                return ok;
            }
            if (mode == 10) {  // round 15: the wardrobe and the guest's pick to the log
                char gd[200];
                wardrobe::State ws;
                const bool wr = wardrobe::Read(&ws);
                Set(msg, cap, "wardrobe %s: open %d category %d previewed %d staging %d; %s; menu active %d wardrobe %d shown %d", wr ? "read" : wardrobe::Why(), ws.open,
                    ws.category, ws.value, ws.staging, suitcol::DescribeGuest(gd, sizeof(gd)), g_menu_active ? 1 : 0, g_menu_wardrobe ? 1 : 0, g_menu_shown ? 1 : 0);
                Log("[suit] %s", msg);
                return wr;
            }
            if (mode == 5) {  // Fragile's porter suit: colour a (0..16) on our fragile_suit bodies, -1 = follow the suit colour again
                g_suit_override = v[1] >= 0 && v[1] <= suitcol::kGold ? static_cast<int>(v[1]) : -1;
                Set(msg, cap, g_suit_override >= 0 ? "Fragile's suit: colour %d (debug override)" : "Fragile's suit follows the suit colour",
                    g_suit_override);
                return suitcol::Available();
            }
            if (mode == 6) {  // Fragile's suit: state and drawables to the log
                char a[220], b[220];
                if (game::Entity* e = g_p.phase == Phase::live && IsFragileSuit(g_p.body) ? g_p.spawn.e : nullptr)
                    suitcol::LogBody(reinterpret_cast<std::uintptr_t>(e), "slot body");
                if (game::Entity* e = g_me.phase == Phase::live && IsFragileSuit(g_me.body) ? g_me.spawn.e : nullptr)
                    suitcol::LogBody(reinterpret_cast<std::uintptr_t>(e), "self body");
                Set(msg, cap, "suit: slot %s; self %s; %s", suitcol::Describe(g_suit_p, a, sizeof(a)), suitcol::Describe(g_suit_me, b, sizeof(b)),
                    suitcol::Available() ? "available" : suitcol::Why());
                Log("[suit] %s", msg);
                return true;
            }
            if (mode == 8) {  // Fragile's suit, hidden tests: metal variant a, roughness b (-1 the variant's); rebinds
                suitcol::SetTestMetal(static_cast<int>(v[1]), static_cast<int>(v[2]));
                suitcol::Undo(g_suit_p, "test metal");
                suitcol::Undo(g_suit_me, "test metal");
                Set(msg, cap, "suit test: metal variant %.0f roughness %.0f", v[1], v[2]);
                return true;
            }
            if (mode == 7) {  // Fragile's suit, hidden tests: meshes a (-1 the product's, -2 the whole model, else a bit mask), channels b
                suitcol::SetTest(static_cast<long long>(v[1]), v[2] > 0 ? static_cast<unsigned>(v[2]) : 0u);
                suitcol::Undo(g_suit_p, "test choice");
                suitcol::Undo(g_suit_me, "test choice");
                Set(msg, cap, "suit test: meshes %.0f channels %.0f", v[1], v[2]);
                return true;
            }
            g_outfit_override = 0;
            Set(msg, cap, "outfit override off (the partner's own outfit applies)");
            return true;
        }
        case 10: {  // smooth <op> <a>: 0 log, 1 feature mask a (0..7), 2 frame cap a fps (0 off), 3 record a frame sequence of a ticks
            const int sub = static_cast<int>(v[0]);
            if (sub == 1) {
                const unsigned mask = v[1] >= 0 && v[1] <= kSmoothAll ? static_cast<unsigned>(v[1]) : kSmoothAll;
                ConfigureSmooth(mask, g_meter_echo, g_meter_echo_delay_ms);
                Set(msg, cap, "smooth mask %u", mask);
                return true;
            }
            if (sub == 2) {
                const unsigned fps = v[1] > 0 ? static_cast<unsigned>(v[1]) : 0;
                SetRuntimeFrameCap(fps);
                Set(msg, cap, "frame cap %u", RuntimeFrameCap());
                Log("[smooth] frame cap now %u fps (0 = off)", RuntimeFrameCap());
                return true;
            }
            if (sub == 3) {
                const unsigned n = v[1] > 0 ? static_cast<unsigned>(v[1]) : 120;
                g_seq_n = 0;
                g_seq_have[0] = g_seq_have[1] = g_seq_have[2] = false;
                g_seq_want = n > kSeqMax ? kSeqMax : n;
                Set(msg, cap, "recording %u frames", g_seq_want);
                return true;
            }
            std::uint8_t pf = 0xFF, sf = 0xFF, samf = 0xFF;
            if (g_p.phase == Phase::live) Rd(reinterpret_cast<std::uintptr_t>(g_p.spawn.e) + kEntFreq, pf);
            if (g_me.phase == Phase::live) Rd(reinterpret_cast<std::uintptr_t>(g_me.spawn.e) + kEntFreq, sf);
            if (have_sam) Rd(reinterpret_cast<std::uintptr_t>(sam) + kEntFreq, samf);
            Set(msg, cap, "mask %u cap %u; update frequency: partner %u self %u Sam %u; setter %s (%u calls, %u faults%s); props boosted %u restored %u",
                g_smooth.load(), RuntimeFrameCap(), pf, sf, samf, g_freq.checked ? (g_freq.ok ? "verified" : g_freq.why) : "not checked yet",
                g_freq.calls, g_freq.faults, g_freq.off ? ", OFF" : "", g_prop_boosts, g_prop_restores);
            Log("[smooth] %s", msg);
            return true;
        }
        case 11: {  // tex <mode> <a> <b>: Fragile's edited textures (fragile_textures.hpp DebugCommand)
            const std::uintptr_t es = g_me.phase == Phase::live && IsFragileSuit(g_me.body) ? reinterpret_cast<std::uintptr_t>(g_me.spawn.e) : 0;
            const std::uintptr_t ep = g_p.phase == Phase::live && IsFragileSuit(g_p.body) ? reinterpret_cast<std::uintptr_t>(g_p.spawn.e) : 0;
            const int mode = static_cast<int>(v[0]);
            if (mode == 0) {
                char a[200], c2[200];
                Log("[tex] bodies: %s; %s", ftex::Describe(g_tex_me, a, sizeof(a)), ftex::Describe(g_tex_p, c2, sizeof(c2)));
                texshare::LogStatus();
            }
            if (mode == 4 && v[1] <= 0) {
                ftex::Undo(g_tex_me, "tex 4 0");
                ftex::Undo(g_tex_p, "tex 4 0");
            }
            return ftex::DebugCommand(mode, v[1], v[2], es, ep, msg, cap);
        }
        default:
            Set(msg, cap, "unknown op %d", op);
            return false;
    }
}

}  // namespace st::realbody
