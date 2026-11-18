#include "vehicle_sync.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "automation.hpp"
#include "body_table.hpp"
#include "game_actions.hpp"
#include "gear_look.hpp"
#include "link.hpp"
#include "log.hpp"
#include "partner_holo.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"

namespace st::vsync {
namespace {

namespace mem = partner::mem;
using game::Entity;
using game::WorldTransform;

// ---------------------------------------------------------------- game facts (byte-checked once, docs section 22.1)
constexpr std::uintptr_t kEntParent = 0x70;      // Entity: parent entity (Entity_ExportedGetParent 0x142361B80 reads +0x70)
constexpr std::uintptr_t kVehType = 0x4C4;       // VehicleEntity: EVehicleType, written by each constructor
constexpr std::uintptr_t kVehRideState = 0x4CC;  // VehicleEntity: ride state (the state machine 0x142F9BC20)

struct VehClass {
    const char* name;
    std::uint8_t kind;          // wire::VehicleKind
    std::uintptr_t vt_rva;      // its vtable
    std::uintptr_t getter_rva;  // vtable slot 0: GetRTTI (lea rax, [rip + rel]; ret)
    const char* getter_sig;
    std::uintptr_t type_write_rva;  // the constructor's `mov dword [rbx + 0x4C4], kind`
    const char* type_write_sig;
};
constexpr VehClass kClasses[3] = {
    {"DSVehicleSpeedCar", 1, 0x3E274D8, 0x2F6BC10, "48 8D 05 49 BA D3 01 C3", 0x2F62201, "C7 83 C4 04 00 00 01 00 00 00"},
    {"DSVehicleTruck", 2, 0x3E26790, 0x2F6BCB0, "48 8D 05 A9 B0 D3 01 C3", 0x2F62571, "C7 83 C4 04 00 00 02 00 00 00"},
    {"DSVehicleMotorbike", 3, 0x3E25DB8, 0x2F6BB60, "48 8D 05 39 AA D3 01 C3", 0x2F61CFC, "C7 83 C4 04 00 00 03 00 00 00"},
};
// The getter of the ride state reads +0x4C4 / +0x4CC at these sites (VehicleEntity_sExportedGetVehicleType /
// IsDriving): checked so a game build with another layout fails closed.
constexpr std::uintptr_t kGetTypeRead = 0x2FA73B6;   // mov ecx, [rbx + 0x4C4]
constexpr std::uintptr_t kIsDrivingRead = 0x2FA74B8;  // mov eax, [rbx + 0x4CC]
constexpr std::uintptr_t kGetParent = 0x2361B80;      // test rcx, rcx; je; mov rax, [rcx + 0x70]; ret

// ---------------------------------------------------------------- the copies (receiver)
struct CopyDef {
    const char* key;
    const char* path;
    const char* guid;
    std::uint32_t layout;
    std::int32_t joints;
    bool welcome;  // a welcome / construction hologram (StartVisible; its own checks)
};
constexpr unsigned kCopyCount = 3;
constexpr CopyDef kCopies[kCopyCount] = {
    // the roadster: no cutscene body exists; its construction hologram (ForConstruction: no fade near the player),
    // spawned safely in the catalog (docs/character-catalog.md #89), the drivable roadster's 23-joint layout
    {"roadster_hologram", "ds/entities/npc/customize_holo/construction/construction_holo_roadster", "8b3517fe-5706-d73b-b6c8-3427b5af39a6",
     0xECF43324u, 23, true},
    // the reverse trike's cutscene body: the drivable trike's art (part_bkst_main_def) and Skeleton
    {"trike", "ds/entities/cinematics/demo_bkst_main_def_pro_new", "161eefa7-9549-c74f-a8b4-550e9eedfb85", 0x8DABA9B0u, 40, false},
    // the truck's cutscene body: the drivable truck's art (part_cald_main_def_game) and Skeleton
    {"truck", "ds/entities/cinematics/demo_cald_main_def_game", "58c639c3-76ef-3446-bfae-fda96943f5d8", 0xB609EC0Du, 47, false},
};
int CopyForKind(std::uint8_t kind) {
    switch (kind) {
        case 1: return 0;
        case 3: return 1;
        case 2: return 2;
        default: return -1;
    }
}

// ---------------------------------------------------------------- state
int g_mode = 1;  // 0 off, 1 the roadster's copy only (proven), 2 solid copies only (trike, truck), 3 all three
bool g_checked = false, g_ok = false;
char g_why[160] = "not checked";

// sender
struct Ours {
    std::uintptr_t e = 0, vt = 0;
    game::Guid guid;
    int cls = -1;
    const wire::VehicleResource* res = nullptr;
    std::uint8_t res_guid[16] = {};
    bool seated = false;
    double left_ms = 0;  // when Sam got off (0 while seated)
    double last_ms = 0;
    double last_pos[3] = {};
    float speed = 0;
    std::uint32_t sent = 0, joint_frames = 0, parked_sent = 0;
};
Ours g_ours;
std::uintptr_t g_logged_parents[8] = {};
unsigned g_logged_parent_n = 0;
double g_next_ours_log_ms = 0;
std::uint32_t g_refused_res = 0, g_refused_type = 0, g_refused_state = 0, g_no_transform = 0, g_pose_unread = 0;

// receiver: frames on the sender's axis
struct Rx {
    double key = 0;
    wire::VehicleFrame f;
};
constexpr unsigned kRing = 160;  // > 5 s at 30 Hz (echo plays 2 s behind)
Rx g_ring[kRing];
unsigned g_head = 0, g_count = 0;
double g_last_rx_ms = 0;
std::uint32_t g_rx_frames = 0, g_rx_dropped = 0;
const Rx& At(unsigned i) { return g_ring[(g_head + i) % kRing]; }

struct Copy {
    holo::Spawn sp;
    int def = -1;
    bool loading = false, live = false, shown = false, placed = false, posed = false;
    std::uintptr_t vt = 0;
    game::Guid guid;
    double begin_ms = 0, live_ms = 0;
    unsigned fails = 0;
    double retry_ms = 0;
};
Copy g_copy;
// A copy refused until this time (ms on the tick clock; kForever: for the game run, a structural reason: its class,
// components or skeleton, or a set-transform fault). A transient refusal (load timeout, no pose yet at realize, not
// shown) backs off 30 s, doubling up to 8 min, and is forgotten at a new link session.
constexpr double kForever = 1e300;
double g_refused_until[kCopyCount] = {};
unsigned g_refusals[kCopyCount] = {};
bool Refused(int def, double now) { return now < g_refused_until[def]; }
void Refuse(int def, bool forever, double now) {
    const unsigned k = g_refusals[def] < 4 ? g_refusals[def] : 4;
    ++g_refusals[def];
    g_refused_until[def] = forever ? kForever : now + 30000.0 * static_cast<double>(1u << k);
}
int g_override = 0;  // 0 auto, 1..3 = copy 0..2
float g_off[3] = {};  // copy offset in the vehicle's frame (m): right, forward, up
double g_broken_since = 0;
std::uint32_t g_spawns = 0, g_removes = 0, g_place_faults = 0, g_pose_sets = 0, g_pose_skips = 0;
char g_last_remove[96] = "-";

// meters: the partner body against the copy's seat (V o seat), while seated
struct Meter {
    double sum = 0, max = 0;
    unsigned n = 0;
    void Add(double v) {
        sum += v, ++n;
        if (v > max) max = v;
    }
    void Reset() { *this = Meter{}; }
};
Meter g_seat_m, g_seat_total, g_delay_m;
double g_next_meter_ms = 0;
wire::GearPose g_win_pose;      // the first pose of the meter window (how far the wheels / steering turned in it)
float g_joint_turn = 0;         // deg
float g_copy_pose_deg = 0, g_copy_pose_m = 0;  // the copy's own pose read back against the pose given one tick earlier
unsigned g_copy_pose_reads = 0;
wire::GearPose g_last_given[6];  // the last poses given to the copy
unsigned g_last_given_i = 0;
double g_trace_until = 0;

// ---------------------------------------------------------------- helpers
bool Rd32(std::uintptr_t a, std::uint32_t& v) { return mem::Read(a, &v, sizeof(v)); }

bool Check() {
    if (g_checked) return g_ok;
    g_checked = true;
    const std::uintptr_t b = mem::Base();
    if (!b) return std::snprintf(g_why, sizeof(g_why), "ds.exe base unknown"), false;
    for (const VehClass& c : kClasses) {
        if (mem::Q(b + c.vt_rva) != b + c.getter_rva || !mem::CodeMatches(b + c.getter_rva, c.getter_sig))
            return std::snprintf(g_why, sizeof(g_why), "%s's vtable / RTTI getter differ", c.name), false;
        if (!mem::CodeMatches(b + c.type_write_rva, c.type_write_sig))
            return std::snprintf(g_why, sizeof(g_why), "%s's constructor does not write kind %u at +0x4C4", c.name, c.kind), false;
    }
    if (!mem::CodeMatches(b + kGetTypeRead, "8B 8B C4 04 00 00") || !mem::CodeMatches(b + kIsDrivingRead, "8B 83 CC 04 00 00") ||
        !mem::CodeMatches(b + kGetParent, "48 85 C9 74 05 48 8B 41 70 C3"))
        return std::snprintf(g_why, sizeof(g_why), "the vehicle type / ride state / parent reads differ"), false;
    g_ok = true;
    std::snprintf(g_why, sizeof(g_why), "ok");
    Log("[vehicle] game checks ok: 3 vehicle classes (vtables, RTTI getters, +0x4C4 kind writes), ride state +0x4CC, parent +0x70");
    return true;
}

int ClassOf(std::uintptr_t e) {
    if (!e) return -1;
    const std::uintptr_t vt = mem::Q(e);
    const std::uintptr_t b = mem::Base();
    for (int i = 0; i < 3; ++i)
        if (vt == b + kClasses[i].vt_rva) return i;
    return -1;
}

void MatMul(const float (&a)[3][3], const float (&b)[3][3], float (&out)[3][3]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
}
// a * b^T
void MatMulT(const float (&a)[3][3], const float (&b)[3][3], float (&out)[3][3]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r][c] = a[r][0] * b[c][0] + a[r][1] * b[c][1] + a[r][2] * b[c][2];
}
// Rows are the frame's axes in world space: world = pos + p * R.
void LocalOf(const WorldTransform& parent, const WorldTransform& child, float (&p)[3], float (&r)[3][3]) {
    const double d[3] = {child.pos.x - parent.pos.x, child.pos.y - parent.pos.y, child.pos.z - parent.pos.z};
    for (int i = 0; i < 3; ++i)
        p[i] = static_cast<float>(d[0] * parent.rot.m[i][0] + d[1] * parent.rot.m[i][1] + d[2] * parent.rot.m[i][2]);
    MatMulT(child.rot.m, parent.rot.m, r);
}
void Compose(const WorldTransform& parent, const float (&p)[3], const float (&r)[3][3], WorldTransform& out) {
    out.pos.x = parent.pos.x + p[0] * parent.rot.m[0][0] + p[1] * parent.rot.m[1][0] + p[2] * parent.rot.m[2][0];
    out.pos.y = parent.pos.y + p[0] * parent.rot.m[0][1] + p[1] * parent.rot.m[1][1] + p[2] * parent.rot.m[2][1];
    out.pos.z = parent.pos.z + p[0] * parent.rot.m[0][2] + p[1] * parent.rot.m[1][2] + p[2] * parent.rot.m[2][2];
    MatMul(r, parent.rot.m, out.rot.m);
}
double Dist(const WorldTransform& a, const WorldTransform& b) {
    const double dx = a.pos.x - b.pos.x, dy = a.pos.y - b.pos.y, dz = a.pos.z - b.pos.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
double DistXyz(const double (&a)[3], double x, double y, double z) {
    const double dx = a[0] - x, dy = a[1] - y, dz = a[2] - z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
const char* StateName(std::uint8_t s) {
    switch (s) {
        case 0: return "parked";
        case 1: return "getting on";
        case 2: return "riding";
        case 3: return "getting off";
        case 4: return "state 4";
        case 5: return "broken";
        case 6: return "idle timeout";
        default: return "?";
    }
}

// Logs a parent entity that is not a vehicle once per vtable (research: carts, ziplines, seats).
void LogOtherParent(std::uintptr_t p) {
    const std::uintptr_t vt = mem::Q(p);
    for (unsigned i = 0; i < g_logged_parent_n; ++i)
        if (g_logged_parents[i] == vt) return;
    if (g_logged_parent_n >= 8) return;  // enough research lines: never one per send tick
    g_logged_parents[g_logged_parent_n++] = vt;
    const char* t = partner::TypeName(reinterpret_cast<const void*>(p));
    Log("[vehicle] our Sam's parent entity %p is a %s (vtable %p): not one of the three vehicles, not sent", reinterpret_cast<void*>(p), t ? t : "?",
        reinterpret_cast<void*>(vt));
}

void Describe(std::uintptr_t v, const char* what) {
    char path[160] = {};
    std::uint8_t g[16] = {};
    const char* type = nullptr;
    const bool have = realbody::ResourceIdentity(reinterpret_cast<Entity*>(v), path, sizeof(path), g, &type);
    std::uint32_t kind = 0, state = 0;
    Rd32(v + kVehType, kind);
    Rd32(v + kVehRideState, state);
    std::uintptr_t skel = 0;
    std::uint32_t layout = 0;
    std::int32_t count = 0;
    const bool sk = look::PoseSkeleton(v, &skel, &layout, &count);
    char gt[37] = {};
    wire::GearGuidText(g, gt);
    Log("[vehicle] %s %p: %s, kind %u, ride state %u; resource %s '%s' %s (%s); skeleton %s %d joints, layout %08X", what, reinterpret_cast<void*>(v),
        partner::TypeName(reinterpret_cast<const void*>(v)) ? partner::TypeName(reinterpret_cast<const void*>(v)) : "?", kind, state,
        have ? "" : "UNREADABLE", path, gt, type ? type : "?", sk ? "" : "UNREADABLE", count, layout);
    partner::DescribeEntity(reinterpret_cast<Entity*>(v), what);
}

// ---------------------------------------------------------------- receiver helpers
void Push(const wire::VehicleFrame& f, double key) {
    if (g_count && key <= At(g_count - 1).key) {
        ++g_rx_dropped;
        return;
    }
    Rx& slot = g_ring[(g_head + g_count) % kRing];
    slot.key = key;
    slot.f = f;
    if (g_count < kRing) ++g_count;
    else g_head = (g_head + 1) % kRing;
}

// The frame at play time t: a / b bracket it (equal when t is outside the ring or on a frame), f in [0, 1].
bool Bracket(double t, const Rx** a, const Rx** b, double* f) {
    if (!g_count) return false;
    if (t <= At(0).key) return false;  // nothing to play yet
    const Rx& newest = At(g_count - 1);
    if (t >= newest.key) {
        *a = *b = &newest;
        *f = 0;
        return true;
    }
    for (unsigned i = g_count - 1; i > 0; --i) {
        const Rx& hi = At(i);
        const Rx& lo = At(i - 1);
        if (lo.key <= t && t <= hi.key) {
            *a = &lo, *b = &hi;
            *f = hi.key > lo.key ? (t - lo.key) / (hi.key - lo.key) : 1.0;
            return true;
        }
    }
    return false;
}

void Lerp(const wire::VehicleFrame& a, const wire::VehicleFrame& b, double f, WorldTransform& v, float (&seat)[3], float (&seat_r)[3][3],
          wire::GearPose* pose) {
    const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const bool jump = dx * dx + dy * dy + dz * dz > 25.0 * 25.0;  // a teleport: no sliding across the map
    const double t = jump ? 1.0 : f;
    v.pos.x = a.x + dx * t, v.pos.y = a.y + dy * t, v.pos.z = a.z + dz * t;
    wire::MatrixFromQuat(wire::Slerp(a.rot, b.rot, static_cast<float>(t)), v.rot.m);
    for (int i = 0; i < 3; ++i) seat[i] = static_cast<float>(a.seat[i] + (b.seat[i] - a.seat[i]) * t);
    wire::MatrixFromQuat(wire::Slerp(a.seat_rot, b.seat_rot, static_cast<float>(t)), seat_r);
    if (!pose) return;
    const wire::VehicleFrame& src = t >= 0.5 ? b : a;
    pose->n = src.n;
    pose->skeleton = src.layout;
    const bool blend = a.n == b.n && a.layout == b.layout && a.n > 0;
    for (unsigned j = 0; j < src.n; ++j) {
        if (blend) {
            wire::Quat qa = a.q[j], qb = b.q[j];
            if (qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w < 0) qb = wire::Quat{-qb.x, -qb.y, -qb.z, -qb.w};
            const float u = static_cast<float>(t);
            pose->q[j] = wire::Normalize(wire::Quat{qa.x + (qb.x - qa.x) * u, qa.y + (qb.y - qa.y) * u, qa.z + (qb.z - qa.z) * u, qa.w + (qb.w - qa.w) * u});
            for (int k = 0; k < 3; ++k) pose->t[j][k] = a.t[j][k] + (b.t[j][k] - a.t[j][k]) * u;
        } else {
            pose->q[j] = src.q[j];
            for (int k = 0; k < 3; ++k) pose->t[j][k] = src.t[j][k];
        }
    }
}

const char* ValidateCopy(Entity* e, const CopyDef& d) {
    const auto a = reinterpret_cast<std::uintptr_t>(e);
    const std::uintptr_t rtti = mem::RttiOf(a);
    if (!rtti) return "entity type unknown";
    if (mem::DerivesFrom(rtti, "Humanoid") || mem::DerivesFrom(rtti, "DSPlayerEntity") || mem::DerivesFrom(rtti, "VehicleEntity"))
        return "a Humanoid / player / vehicle entity";
    if (ClassOf(a) >= 0) return "a drivable vehicle";
    for (const char* bad : {"DSInteractionComponent", "PhysicsCollisionComponent", "PhysicsRagdollComponent", "DSPlayerControllerComponent",
                            "HumanoidController", "CapsuleComponent", "VehicleMover", "DSBounderComponent"})
        if (partner::FindComponentByName(e, bad)) return bad;
    if (d.welcome) {
        std::uintptr_t comp = 0;
        if (const char* why = holo::ValidateWelcomeHolo(e, &comp)) return why;
    } else {
        for (const char* bad : {"AIIndividualComponent", "AIBodyComponent", "DSAIIndividualComponent", "BasicControlledEntityAIController"})
            if (partner::FindComponentByName(e, bad)) return bad;
    }
    std::uintptr_t skel = 0;
    std::uint32_t layout = 0;
    std::int32_t count = 0;
    if (!look::PoseSkeleton(a, &skel, &layout, &count)) return "no skinned pose";
    if (layout != d.layout || count != d.joints) return "not the vehicle's skeleton";
    return nullptr;
}

void ClearCopy(const char* why, bool count) {
    if (!g_copy.loading && !g_copy.live && !g_copy.sp.sp) {
        g_copy = Copy{};
        return;
    }
    realbody::ClearGearPose(realbody::kVehiclePoseTarget);
    char step[160] = {};
    const int def = g_copy.def;
    holo::SpawnTeardown(g_copy.sp, why, step, sizeof(step));
    if (count) ++g_removes;
    std::snprintf(g_last_remove, sizeof(g_last_remove), "%s", why);
    Log("[vehicle] the partner's vehicle copy '%s' removed (%s): %s", def >= 0 ? kCopies[def].key : "?", why, step);
    g_copy = Copy{};
}

void Begin(int def, const WorldTransform& v, double now) {
    WorldTransform at = v;
    at.pos.z -= 20.0;  // out of sight until it is placed
    std::uint8_t g[16];
    char why[200] = {};
    const CopyDef& d = kCopies[def];
    const unsigned fails = g_copy.def == def ? g_copy.fails : 0;  // the backoff grows while the same copy keeps failing
    g_copy = Copy{};
    g_copy.def = def;
    g_copy.fails = fails;
    if (!bodies::ParseGuid(d.guid, g)) {
        Refuse(def, true, now);
        Log("[vehicle] copy '%s': bad GUID text; refused", d.key);
        return;
    }
    if (realbody::BeginByPathGuid(d.path, g, at, 0x76000000u + static_cast<std::uint32_t>(def), &g_copy.sp, why, sizeof(why))) {
        g_copy.loading = true;
        g_copy.begin_ms = now;
        ++g_spawns;
        Log("[vehicle] the partner's vehicle copy '%s' (%s) loading", d.key, d.path);
    } else {
        g_copy.retry_ms = now + 5000.0 * (1u << (g_copy.fails < 4 ? g_copy.fails : 4));
        ++g_copy.fails;
        Log("[vehicle] copy '%s' not started: %s", d.key, why);
    }
}

void PollCopy(double now) {
    if (!g_copy.loading) return;
    char why[200] = {};
    const CopyDef& d = kCopies[g_copy.def];
    switch (holo::SpawnPoll(g_copy.sp, why, sizeof(why))) {
        case holo::SpawnState::loading:
            if (now - g_copy.begin_ms > 20000) {
                Refuse(g_copy.def, false, now);
                Log("[vehicle] copy '%s' did not load in 20 s: refused for %.0f s", d.key, (g_refused_until[g_copy.def] - now) / 1000);
                ClearCopy("load timeout", false);
            }
            return;
        case holo::SpawnState::cancelled:
        case holo::SpawnState::failed: {
            const int def = g_copy.def;
            const unsigned fails = g_copy.fails + 1;
            Log("[vehicle] copy '%s' did not load: %s", d.key, why);
            ClearCopy("load failed", false);
            g_copy.def = def;
            g_copy.fails = fails;
            g_copy.retry_ms = now + 10000.0 * static_cast<double>(1u << (fails < 4 ? fails - 1 : 3));
            return;
        }
        case holo::SpawnState::realized: {
            g_copy.loading = false;
            const char* bad = g_copy.sp.e ? ValidateCopy(g_copy.sp.e, d) : "no entity";
            if (bad) {
                // no pose yet right at realize may pass later; a wrong class, component or skeleton never does
                const bool forever = std::strcmp(bad, "no skinned pose") != 0;
                Refuse(g_copy.def, forever, now);
                if (forever) Log("[vehicle] copy '%s' refused for this game run: %s", d.key, bad);
                else Log("[vehicle] copy '%s' refused for %.0f s: %s", d.key, (g_refused_until[g_copy.def] - now) / 1000, bad);
                if (g_copy.sp.e) partner::DescribeEntity(g_copy.sp.e, "refused vehicle copy");
                ClearCopy("refused", false);
                return;
            }
            if (!d.welcome) partner::SetVisible(g_copy.sp.e, false);
            partner::WakeIfAsleep(g_copy.sp.e);
            partner::ReadVtable(g_copy.sp.e, &g_copy.vt);
            partner::ReadGuid(g_copy.sp.e, &g_copy.guid);
            g_copy.live = true;
            g_copy.live_ms = now;
            g_copy.fails = 0;
            g_refusals[g_copy.def] = 0;
            Log("[vehicle] the partner's vehicle copy '%s' live after %.0f ms (%s)", d.key, now - g_copy.begin_ms,
                d.welcome ? "hologram" : "solid cutscene body");
            static bool described[kCopyCount] = {};
            if (!described[g_copy.def]) {
                described[g_copy.def] = true;
                partner::DescribeEntity(g_copy.sp.e, d.key);
            }
            return;
        }
    }
}

void LogMeters(double now, bool force) {
    if (!force && now < g_next_meter_ms) return;
    g_next_meter_ms = now + 5000;
    if (g_seat_m.n || g_delay_m.n)
        Log("[vehicle] meter (5 s): partner body vs the copy's seat %.1f cm mean, %.1f cm max over %u ticks; play delay %.0f ms mean, %.0f max; "
            "the sent joints turned up to %.1f deg; the copy's pose vs the closest of the last 6 given %.2f deg / %.1f mm max over %u reads",
            g_seat_m.n ? g_seat_m.sum / g_seat_m.n * 100 : 0.0, g_seat_m.max * 100, g_seat_m.n, g_delay_m.n ? g_delay_m.sum / g_delay_m.n : 0.0,
            g_delay_m.max, static_cast<double>(g_joint_turn), static_cast<double>(g_copy_pose_deg), static_cast<double>(g_copy_pose_m) * 1000, g_copy_pose_reads);
    g_seat_m.Reset();
    g_delay_m.Reset();
    g_win_pose.n = 0;
    g_joint_turn = 0, g_copy_pose_deg = 0, g_copy_pose_m = 0;
    g_copy_pose_reads = 0;
}

}  // namespace

// ---------------------------------------------------------------- API
void Init(const Config& config) noexcept {
    g_mode = config.vehicle_sync;
    if (g_mode < 0 || g_mode > 3) g_mode = 1;
    Log("[vehicle] vehicle sync %s (vehicle_sync=%d)", g_mode == 0 ? "off" : g_mode == 1 ? "on: the roadster's copy (trike and truck copies: vehicle_sync=3)" : g_mode == 2 ? "on, solid copies only (trike, truck)" : "on: roadster, trike and truck copies", g_mode);
}

bool TakeSend(Entity* local, const WorldTransform& local_wt, std::uint32_t sender_ms, std::uint32_t seq, double now, wire::VehicleFrame* out) noexcept {
    if (g_mode == 0 || !local || !out || !Check()) return false;
    const auto sam = reinterpret_cast<std::uintptr_t>(local);
    std::uintptr_t v = 0;
    bool seated = false;
    const std::uintptr_t p = mem::Q(sam + kEntParent);
    if (p) {
        if (ClassOf(p) >= 0) {
            v = p;
        } else {
            const std::uintptr_t pp = mem::Q(p + kEntParent);  // a seat child of the vehicle
            if (ClassOf(pp) >= 0) {
                v = pp;
                static bool once = false;
                if (!once) {
                    once = true;
                    const char* t = partner::TypeName(reinterpret_cast<const void*>(p));
                    Log("[vehicle] our Sam's parent %p (%s) is a child of the vehicle %p", reinterpret_cast<void*>(p), t ? t : "?", reinterpret_cast<void*>(pp));
                }
            } else {
                LogOtherParent(p);
            }
        }
        seated = v != 0;
    }
    if (v && v != g_ours.e) {
        // a new vehicle (or the same one again after a gap)
        g_ours = Ours{};
        g_ours.e = v;
        g_ours.vt = mem::Q(v);
        g_ours.cls = ClassOf(v);
        partner::ReadGuid(reinterpret_cast<Entity*>(v), &g_ours.guid);
        char path[160] = {};
        const char* type = nullptr;
        if (realbody::ResourceIdentity(reinterpret_cast<Entity*>(v), path, sizeof(path), g_ours.res_guid, &type))
            g_ours.res = wire::FindVehicleResource(g_ours.res_guid);
        static int described = 0;
        if (described++ < 4) Describe(v, "our vehicle");
        if (!g_ours.res && ++g_refused_res <= 5) Log("[vehicle] our vehicle's resource '%s' is not a known drivable vehicle: not sent", path);
    }
    if (!v) {
        // Got off: the same vehicle as parked, while it still checks (vtable + per-creation GUID), Sam stays within 30 m
        // and for 60 s at most.
        if (!g_ours.e) return false;
        game::Guid g;
        if (mem::Q(g_ours.e) != g_ours.vt || !partner::ReadGuid(reinterpret_cast<Entity*>(g_ours.e), &g) || g != g_ours.guid) {
            Log("[vehicle] our last vehicle %p is gone: no longer reported", reinterpret_cast<void*>(g_ours.e));
            g_ours = Ours{};
            return false;
        }
        if (g_ours.seated || g_ours.left_ms == 0) {
            g_ours.left_ms = now;
            Log("[vehicle] our Sam got off vehicle %p: reported as parked for up to 60 s within 30 m", reinterpret_cast<void*>(g_ours.e));
        }
        g_ours.seated = false;
        if (now - g_ours.left_ms > 60000 || DistXyz(g_ours.last_pos, local_wt.pos.x, local_wt.pos.y, local_wt.pos.z) > 30.0) {
            Log("[vehicle] our last vehicle %p: no longer reported (%s)", reinterpret_cast<void*>(g_ours.e), now - g_ours.left_ms > 60000 ? "60 s" : "beyond 30 m");
            g_ours = Ours{};
            return false;
        }
        v = g_ours.e;
    } else {
        if (!g_ours.seated && g_ours.left_ms != 0) Log("[vehicle] our Sam is on vehicle %p again", reinterpret_cast<void*>(v));
        g_ours.seated = true;
        g_ours.left_ms = 0;
    }
    if (!g_ours.res) return false;
    std::uint32_t kind = 0, state = 0;
    if (!Rd32(v + kVehType, kind) || kind != kClasses[g_ours.cls].kind || static_cast<std::uint8_t>(g_ours.res->kind) != kind) {
        if (++g_refused_type <= 5) Log("[vehicle] our vehicle's kind %u does not match its class %s / resource: not sent", kind, kClasses[g_ours.cls].name);
        return false;
    }
    if (!Rd32(v + kVehRideState, state) || state > wire::kVehicleStateMax) {
        if (++g_refused_state <= 5) Log("[vehicle] our vehicle's ride state %u is out of range: not sent", state);
        return false;
    }
    WorldTransform vt{};
    if (!game::TryGetTransform(reinterpret_cast<Entity*>(v), &vt)) {
        if (++g_no_transform <= 5) Log("[vehicle] our vehicle's transform is unreadable: not sent");
        return false;
    }
    wire::VehicleFrame& f = *out;
    f = wire::VehicleFrame{};
    f.seq = seq;
    f.sender_ms = sender_ms;
    f.kind = static_cast<std::uint8_t>(kind);
    f.state = static_cast<std::uint8_t>(state);
    std::memcpy(f.guid, g_ours.res_guid, 16);
    f.x = vt.pos.x, f.y = vt.pos.y, f.z = vt.pos.z;
    f.rot = wire::QuatFromMatrix(vt.rot.m);
    if (g_ours.last_ms > 0 && now - g_ours.last_ms > 1.0) {
        const double inst = DistXyz(g_ours.last_pos, vt.pos.x, vt.pos.y, vt.pos.z) * 1000.0 / (now - g_ours.last_ms);
        g_ours.speed = g_ours.speed * 0.7f + static_cast<float>(inst) * 0.3f;
    }
    g_ours.last_ms = now;
    g_ours.last_pos[0] = vt.pos.x, g_ours.last_pos[1] = vt.pos.y, g_ours.last_pos[2] = vt.pos.z;
    f.speed = g_ours.speed < 1000.0f ? g_ours.speed : 999.0f;
    if (seated) {
        float sp[3], sr[3][3];
        LocalOf(vt, local_wt, sp, sr);
        if (std::fabs(sp[0]) < wire::kMaxVehicleSeatM && std::fabs(sp[1]) < wire::kMaxVehicleSeatM && std::fabs(sp[2]) < wire::kMaxVehicleSeatM) {
            f.flags |= wire::kVehSeated;
            f.seat[0] = sp[0], f.seat[1] = sp[1], f.seat[2] = sp[2];
            f.seat_rot = wire::QuatFromMatrix(sr);
        } else {
            static unsigned far_logs = 0;
            if (++far_logs <= 5) Log("[vehicle] our Sam is %.2f / %.2f / %.2f m from his vehicle's origin: not marked seated", sp[0], sp[1], sp[2]);
        }
    }
    static wire::GearPose gp;
    if (look::ReadPose(v, &gp) && gp.n > 0 && gp.n <= wire::kMaxVehicleJoints && (g_ours.res->layout == 0 || gp.skeleton == g_ours.res->layout)) {
        bool ok = true;
        for (unsigned j = 0; j < gp.n && ok; ++j)
            for (float t : gp.t[j]) ok = ok && std::fabs(t) <= wire::kMaxVehicleJointM;
        if (ok) {
            f.flags |= wire::kVehJoints;
            f.layout = gp.skeleton;
            f.n = gp.n;
            for (unsigned j = 0; j < gp.n; ++j) {
                f.q[j] = gp.q[j];
                for (int k = 0; k < 3; ++k) f.t[j][k] = gp.t[j][k];
            }
            ++g_ours.joint_frames;
        }
    } else {
        ++g_pose_unread;
    }
    ++g_ours.sent;
    if (!seated) ++g_ours.parked_sent;
    if (now >= g_next_ours_log_ms) {
        g_next_ours_log_ms = now + 2000;
        Log("[vehicle] ours: %s %s (%u), %s, at (%.2f, %.2f, %.2f) %.1f m/s, seat (%.2f, %.2f, %.2f), %u joints (layout %08X); %u frames sent (%u with joints, %u parked)",
            g_ours.res->key, StateName(f.state), f.state, seated ? "seated" : "parked", f.x, f.y, f.z, static_cast<double>(f.speed), f.seat[0], f.seat[1], f.seat[2],
            f.n, f.layout, g_ours.sent, g_ours.joint_frames, g_ours.parked_sent);
    }
    return true;
}

void SenderTick(Entity* local, double now) noexcept {
    if (g_mode == 0 || g_trace_until <= now || !local || !g_ok) return;
    const std::uintptr_t v = g_ours.e;
    if (!v || ClassOf(v) < 0) return;
    WorldTransform vt{};
    if (!game::TryGetTransform(reinterpret_cast<Entity*>(v), &vt)) return;
    const double qpc = link::NowMsPrecise();
    Log("[vehicle] trace tx t=%.1f pos=(%.3f, %.3f, %.3f) fwd=(%.3f, %.3f)", qpc, vt.pos.x, vt.pos.y, vt.pos.z, static_cast<double>(vt.rot.m[1][0]),
        static_cast<double>(vt.rot.m[1][1]));
}

void OnFrame(const wire::VehicleFrame& f, double key_ms, double now) noexcept {
    if (g_mode == 0) return;
    // After a gap (the partner reported no vehicle for 2 s) the old frames go: the next drive never interpolates from a
    // frame of an earlier one (another place, another kind).
    if (g_count && now - g_last_rx_ms > 2000) g_count = g_head = 0;
    Push(f, key_ms);
    g_last_rx_ms = now;
    ++g_rx_frames;
    static bool first = false;
    if (!first) {
        first = true;
        const wire::VehicleResource* r = wire::FindVehicleResource(f.guid);
        Log("[vehicle] first frame from the partner: %s (kind %u), %s, %u joints", r ? r->key : "?", f.kind, StateName(f.state), f.n);
    }
}

void NewSession() noexcept {
    g_count = g_head = 0;
    g_last_rx_ms = 0;
    g_broken_since = 0;
    RemoveCopy("new link session");
    for (unsigned i = 0; i < kCopyCount; ++i)
        if (g_refused_until[i] < kForever) g_refused_until[i] = 0, g_refusals[i] = 0;  // transient refusals are forgotten
}

void EmergencyStop() noexcept {
    // The tick will not run again: the copy is hidden where it stands (its pose target is already off: realbody's stop).
    if ((g_copy.live || g_copy.loading) && g_copy.sp.e && holo::SpawnAlive(g_copy.sp)) partner::SetVisible(g_copy.sp.e, false);
    g_mode = 0;
}

void RemoveCopy(const char* why) noexcept { ClearCopy(why, true); }

Entity* CopyEntity() noexcept { return g_copy.live && holo::SpawnAlive(g_copy.sp) ? g_copy.sp.e : nullptr; }

void Tick(const TickIn& in, double now) noexcept {
    LogMeters(now, false);
    const bool have_any = g_copy.loading || g_copy.live || g_copy.sp.sp;
    const char* why_none = nullptr;
    if (g_mode == 0) why_none = "vehicle_sync off";
    else if (!in.local_ok) why_none = "our player asleep or gone";
    else if (!in.partner_present) why_none = "no partner body";
    else if (!g_count || now - g_last_rx_ms > 2000) why_none = "no vehicle frame for 2 s";
    else if (!in.have_play) why_none = "no play time";
    const Rx* a = nullptr;
    const Rx* b = nullptr;
    double f = 0;
    if (!why_none && !Bracket(in.play_ms, &a, &b, &f)) why_none = "nothing to play yet";
    if (!why_none && in.play_ms - b->key > 1000) why_none = "the partner stopped reporting a vehicle";
    // Two frames more than 0.5 s apart on the sender's axis are not one motion: the later one stands (its kind, its place).
    if (!why_none && b->key - a->key > 500) a = b, f = 0;
    if (!why_none) {
        if (a->f.state == 5) {
            if (g_broken_since == 0) g_broken_since = now;
            if (now - g_broken_since > 3000) why_none = "broken for 3 s";
        } else {
            g_broken_since = 0;
        }
    }
    int def = -1;
    if (!why_none) {
        def = g_override > 0 ? g_override - 1 : CopyForKind(b->f.kind);
        if (def < 0) why_none = "unknown kind";
        else if (g_override == 0 && g_mode == 1 && def != 0) why_none = "trike / truck copies off (vehicle_sync=1: the roadster only; 3 for all)";
        else if (g_override == 0 && g_mode == 2 && kCopies[def].welcome) why_none = "hologram copies off (vehicle_sync=2)";
        else if (Refused(def, now)) why_none = "its copy was refused";
    }
    if (why_none) {
        if (have_any) RemoveCopy(why_none);
        static const char* gate_logged = nullptr;  // why a reported vehicle shows no copy (once per reason in a row)
        if (def >= 0 && why_none != gate_logged) {
            gate_logged = why_none;
            Log("[vehicle] the partner's %s: no copy (%s)", kCopies[def].key, why_none);
        }
        return;
    }
    // the copy wanted: another one first goes
    if (have_any && g_copy.def != def) RemoveCopy("the partner's vehicle changed");
    WorldTransform v{};
    float seat[3], seat_r[3][3];
    static wire::GearPose pose;
    pose = wire::GearPose{};
    Lerp(a->f, b->f, f, v, seat, seat_r, &pose);
    if (g_off[0] != 0 || g_off[1] != 0 || g_off[2] != 0) {
        const float ident[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        WorldTransform o{};
        Compose(v, g_off, ident, o);
        v = o;
    }
    if (!g_copy.loading && !g_copy.live) {
        if (g_copy.def == def && now < g_copy.retry_ms) return;
        Begin(def, v, now);
        return;
    }
    PollCopy(now);
    if (!g_copy.live) return;
    if (!holo::SpawnAlive(g_copy.sp)) {
        RemoveCopy("destroyed by the game");
        g_copy.retry_ms = now + 10000;
        g_copy.def = def;
        return;
    }
    if (!game::TrySetTransform(g_copy.sp.e, v)) {
        ++g_place_faults;
        Refuse(def, true, now);  // a fault (the crash reporter sees even a caught one): not again this game run
        RemoveCopy("set transform fault");
        return;
    }
    if (!g_copy.placed) {
        g_copy.placed = true;
        Log("[vehicle] copy '%s' placed at (%.2f, %.2f, %.2f)", kCopies[def].key, v.pos.x, v.pos.y, v.pos.z);
    }
    if (!g_copy.shown) {
        char why[200] = {};
        const bool ok = kCopies[def].welcome ? holo::SpawnStartVisible(g_copy.sp.e, why, sizeof(why)) : partner::SetVisible(g_copy.sp.e, true);
        g_copy.shown = ok;
        Log("[vehicle] copy '%s' %s%s%s", kCopies[def].key, ok ? "shown" : "NOT shown", why[0] ? ": " : "", why);
        if (!ok) {
            Refuse(def, false, now);
            RemoveCopy("could not be shown");
            return;
        }
    } else if (!kCopies[def].welcome) {
        std::uint64_t fl = 0;
        if (partner::ReadFlags(g_copy.sp.e, &fl) && (fl & 0x2) == 0) partner::SetVisible(g_copy.sp.e, true);
    }
    // its pose: the partner vehicle's joints when they are on its skeleton
    // The meters (hidden tests and the trace only; never in play): the joints' turn, the copy's read-back pose, the seat.
    const bool meter = automation::Enabled() || g_trace_until > now;
    if (pose.n > 0 && pose.skeleton == kCopies[def].layout && static_cast<std::int32_t>(pose.n) == kCopies[def].joints && now - g_copy.live_ms > 300) {
        // meters: how far the joints turned in this window, and whether the copy shows the pose given last tick
        if (meter && g_win_pose.n != pose.n) g_win_pose = pose;
        for (unsigned j = 0; meter && j < pose.n; ++j) {
            const float t = wire::AngleBetween(pose.q[j], g_win_pose.q[j]);
            if (t > g_joint_turn) g_joint_turn = t;
        }
        // (the copy's update may have taken any of the last few poses given: the closest of them counts)
        static wire::GearPose back;
        if (meter && g_copy.posed && look::ReadPose(reinterpret_cast<std::uintptr_t>(g_copy.sp.e), &back)) {
            float best_d = 1e9f, best_m = 1e9f;
            for (const wire::GearPose& given : g_last_given) {
                float dm = 0, dd = 0;
                if (given.n == back.n && wire::GearPoseDiff(back, given, &dm, &dd) && dd < best_d) best_d = dd, best_m = dm;
            }
            if (best_d < 1e8f) {
                ++g_copy_pose_reads;
                if (best_d > g_copy_pose_deg) g_copy_pose_deg = best_d;
                if (best_m > g_copy_pose_m) g_copy_pose_m = best_m;
            }
        }
        if (meter) g_last_given[g_last_given_i++ % 6] = pose;
        if (realbody::SetGearPose(realbody::kVehiclePoseTarget, &g_copy.sp, &pose)) {
            ++g_pose_sets;
            if (!g_copy.posed) {
                g_copy.posed = true;
                Log("[vehicle] copy '%s' posed from the partner's vehicle joints (%u)", kCopies[def].key, pose.n);
            }
        }
    } else {
        ++g_pose_skips;
    }
    // meters: the partner body against V o seat (seated), and how far behind now the play time is
    if (meter) g_delay_m.Add(now - in.play_ms);
    if (meter && (a->f.flags & wire::kVehSeated) && (b->f.flags & wire::kVehSeated) && in.have_body) {
        WorldTransform s{};
        Compose(v, seat, seat_r, s);
        const double d = Dist(s, in.body);
        g_seat_m.Add(d);
        g_seat_total.Add(d);
    }
    if (g_trace_until > now)
        Log("[vehicle] trace rx play=%.1f now=%.1f placed=(%.3f, %.3f, %.3f) state=%u seated=%d body=(%.3f, %.3f, %.3f)", in.play_ms, now, v.pos.x, v.pos.y,
            v.pos.z, a->f.state, (a->f.flags & wire::kVehSeated) ? 1 : 0, in.body.pos.x, in.body.pos.y, in.body.pos.z);
}

bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    switch (op) {
        case 0: {
            char pc[200] = {};
            realbody::GearPoseCounters(pc, sizeof(pc));
            std::uint64_t tx = 0, rx = 0;
            link::VehicleCounts(&tx, &rx);
            Log("[vehicle] status: mode %d, checks %s; ours %p (%s) %s, %u frames sent (%u with joints, %u parked); refused res/type/state %u/%u/%u, "
                "pose unread %u; link sent %llu, received %llu",
                g_mode, g_why, reinterpret_cast<void*>(g_ours.e), g_ours.res ? g_ours.res->key : "-", g_ours.e ? (g_ours.seated ? "seated" : "parked") : "none",
                g_ours.sent, g_ours.joint_frames, g_ours.parked_sent, g_refused_res, g_refused_type, g_refused_state, g_pose_unread,
                static_cast<unsigned long long>(tx), static_cast<unsigned long long>(rx));
            Log("[vehicle] status: partner frames %u received (%u dropped), ring %u; copy '%s' %s%s%s, %u spawned, %u removed (last: %s), %u place faults, "
                "%u pose sets, %u skipped; pose targets: %s",
                g_rx_frames, g_rx_dropped, g_count, g_copy.def >= 0 ? kCopies[g_copy.def].key : "-",
                g_copy.live ? "live" : (g_copy.loading ? "loading" : "none"), g_copy.shown ? ", shown" : "", g_copy.posed ? ", posed" : "", g_spawns,
                g_removes, g_last_remove, g_place_faults, g_pose_sets, g_pose_skips, pc);
            Log("[vehicle] status: partner body vs the copy's seat over the session: %.1f cm mean, %.1f cm max over %u ticks",
                g_seat_total.n ? g_seat_total.sum / g_seat_total.n * 100 : 0.0, g_seat_total.max * 100, g_seat_total.n);
            std::snprintf(msg, cap, "mode %d; ours %s; copy %s %s; seat error %.1f / %.1f cm", g_mode, g_ours.res ? g_ours.res->key : "-",
                          g_copy.def >= 0 ? kCopies[g_copy.def].key : "-", g_copy.live ? "live" : "none",
                          g_seat_total.n ? g_seat_total.sum / g_seat_total.n * 100 : 0.0, g_seat_total.max * 100);
            return true;
        }
        case 1: {
            const int m = static_cast<int>(v[1]);
            if (m < 0 || m > 3) return std::snprintf(msg, cap, "mode 0 off, 1 roadster, 2 solid only, 3 all"), false;
            g_mode = m;
            if (m == 0) RemoveCopy("vehicle_sync turned off");
            std::snprintf(msg, cap, "vehicle sync mode %d", m);
            return true;
        }
        case 2: {
            const int o = static_cast<int>(v[1]);
            if (o < 0 || o > 3) return std::snprintf(msg, cap, "copy 0 auto, 1 roadster hologram, 2 trike, 3 truck"), false;
            g_override = o;
            std::snprintf(msg, cap, "copy override %s", o ? kCopies[o - 1].key : "off (auto)");
            Log("[vehicle] %s", msg);
            return true;
        }
        case 3:
            g_trace_until = link::NowMsPrecise() + (v[1] > 0 ? v[1] : 5000);
            std::snprintf(msg, cap, "trace for %.0f ms", v[1] > 0 ? v[1] : 5000.0);
            return true;
        case 4:
            g_off[0] = static_cast<float>(v[1] / 100.0), g_off[1] = static_cast<float>(v[2] / 100.0);
            std::snprintf(msg, cap, "copy offset right %.0f cm, forward %.0f cm, up %.0f cm", v[1], v[2], static_cast<double>(g_off[2]) * 100);
            Log("[vehicle] %s", msg);
            return true;
        case 6:
            g_off[2] = static_cast<float>(v[1] / 100.0);
            std::snprintf(msg, cap, "copy offset up %.0f cm", v[1]);
            Log("[vehicle] %s", msg);
            return true;
        case 7:
        case 8: {
            // Hidden tests: the drivable vehicles near Sam (the world's entity list, as the census reads it); 8 also
            // places Sam v[1] m (0 = 1.8) beside the nearest one, on the side he is on, for its Use Vehicle prompt.
            Entity* sam = nullptr;
            WorldTransform swt{};
            if (!game::TryGetLocalPlayer(&sam) || !sam || !game::TryGetTransform(sam, &swt)) return std::snprintf(msg, cap, "no player"), false;
            if (!Check()) return std::snprintf(msg, cap, "checks: %s", g_why), false;
            const std::uintptr_t b = mem::Base();
            if (!mem::CodeMatches(b + 0x357E024, "48 8D 9E 30 04 00 00 48 8B CB") || !mem::CodeMatches(b + 0x357E041, "48 8D 8E 38 04 00 00 48 8B D7 E8"))
                return std::snprintf(msg, cap, "World::AddEntity bytes differ"), false;
            const std::uintptr_t w = mem::Q(b + 0x7BC4E70);
            if (!w || mem::Q(w) != b + 0x3D5D140) return std::snprintf(msg, cap, "no world"), false;
            std::int32_t n = 0;
            if (!mem::Read(w + 0x438, &n, 4) || n <= 0 || n > 200000) return std::snprintf(msg, cap, "world list unreadable"), false;
            const std::uintptr_t data = mem::Q(w + 0x440);
            static std::uintptr_t ents[200000];
            if (!data || !mem::Read(data, ents, static_cast<std::size_t>(n) * 8)) return std::snprintf(msg, cap, "world list unreadable"), false;
            std::uintptr_t best = 0;
            double best_d = 1e9;
            WorldTransform best_wt{};
            unsigned found = 0;
            for (std::int32_t i = 0; i < n; ++i) {
                const int cls = ClassOf(ents[i]);
                if (cls < 0) continue;
                WorldTransform vt{};
                if (!game::TryGetTransform(reinterpret_cast<Entity*>(ents[i]), &vt)) continue;
                const double d = Dist(vt, swt);
                ++found;
                std::uint32_t state = 0;
                Rd32(ents[i] + kVehRideState, state);
                if (found <= 12)
                    Log("[vehicle] near: %s %p at (%.2f, %.2f, %.2f), %.1f m from Sam, forward (%.2f, %.2f, %.2f), ride state %u", kClasses[cls].name,
                        reinterpret_cast<void*>(ents[i]), vt.pos.x, vt.pos.y, vt.pos.z, d, static_cast<double>(vt.rot.m[1][0]),
                        static_cast<double>(vt.rot.m[1][1]), static_cast<double>(vt.rot.m[1][2]), state);
                if (d < best_d) best_d = d, best = ents[i], best_wt = vt;
            }
            if (!best) return std::snprintf(msg, cap, "no drivable vehicle in the world list (%d entities)", n), false;
            if (op == 7) return std::snprintf(msg, cap, "%u vehicle(s); nearest %.1f m", found, best_d), true;
            if (best_d > 40) return std::snprintf(msg, cap, "the nearest vehicle is %.1f m away", best_d), false;
            const double side = v[1] > 0 ? v[1] : 1.8;
            const float* r = best_wt.rot.m[0];
            const double sx = swt.pos.x - best_wt.pos.x, sy = swt.pos.y - best_wt.pos.y;
            const double sgn = sx * r[0] + sy * r[1] >= 0 ? 1.0 : -1.0;
            const double x = best_wt.pos.x + sgn * side * r[0], y = best_wt.pos.y + sgn * side * r[1], z = best_wt.pos.z + 0.1;
            char why[160] = {};
            const bool ok = game::TeleportLocalPlayer(x, y, z, game::TeleportMode::place, why, sizeof(why));
            Log("[vehicle] placing Sam %.1f m %s of the %s at (%.2f, %.2f, %.2f): %s", side, sgn > 0 ? "right" : "left", kClasses[ClassOf(best)].name, x, y,
                z, why);
            std::snprintf(msg, cap, "%s: %s", ok ? "placed" : "not placed", why);
            return ok;
        }
        case 9: {
            // Hidden tests: Sam placed at x = v[1], y = v[2] at his own height (TeleportMode::place; the scenarios' way to
            // stand beside a parked vehicle the world's entity list does not hold).
            Entity* sam = nullptr;
            WorldTransform swt{};
            if (!game::TryGetLocalPlayer(&sam) || !sam || !game::TryGetTransform(sam, &swt)) return std::snprintf(msg, cap, "no player"), false;
            char why[160] = {};
            const bool ok = game::TeleportLocalPlayer(v[1], v[2], swt.pos.z, game::TeleportMode::place, why, sizeof(why));
            std::snprintf(msg, cap, "%s (%.2f, %.2f, %.2f): %s", ok ? "placed at" : "not placed at", v[1], v[2], swt.pos.z, why);
            return ok;
        }
        case 5:
            if (!g_ours.e) return std::snprintf(msg, cap, "no vehicle of ours"), false;
            Describe(g_ours.e, "our vehicle (asked)");
            if (Entity* c = CopyEntity()) partner::DescribeEntity(c, "the partner's vehicle copy");
            std::snprintf(msg, cap, "described");
            return true;
        default:
            std::snprintf(msg, cap, "ops: 0 log, 1 mode, 2 copy override, 3 trace ms, 4 offset right/forward cm, 5 describe, 6 offset up cm");
            return false;
    }
}

}  // namespace st::vsync

namespace st::game {
bool VehicleSyncCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept { return vsync::DebugCommand(op, v, msg, cap); }
}  // namespace st::game
