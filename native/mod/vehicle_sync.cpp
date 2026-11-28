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
#include "nametags.hpp"
#include "partner_gear.hpp"
#include "partner_holo.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"
#include "vehicle_drive.hpp"
#include "vehicle_share.hpp"

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

// The game's registry of every vehicle in the world (the vehicle manager, allocated in 0x142FAA2C0): R = [kRegGlobal],
// R+0 i32 count, R+8 Entity** (an SRW lock at R+0x20 the game takes; we only read, twice, equal reads only). The
// add-to-world handlers of all three classes register (0x142F75070: push, then [e+0x560] |= 2), the remove handlers
// unregister (0x142F76180: clear bit 2, then remove); the game's own nearest-vehicle search (0x142F7DFD0) loops over it.
constexpr std::uintptr_t kRegGlobal = 0x7BFECC0;
constexpr std::uintptr_t kVehFlags = 0x560;  // bit 2: registered
struct Site {
    std::uintptr_t rva;
    const char* sig;
};
constexpr Site kRegSites[] = {
    {0x2F7509C, "48 8B 35 1D 9C C8 04"},             // register: mov rsi, [R]
    {0x2F750A3, "48 8D 5E 20"},                      // lea rbx, [rsi + 0x20] (the lock)
    {0x2F750BD, "48 8B D7 48 8B CE E8 68 EF 98 FE"},  // the push into the array at R + 0
    {0x2F750D6, "83 8F 60 05 00 00 02"},             // or dword [rdi + 0x560], 2
    {0x2F7630B, "83 A3 60 05 00 00 FD"},             // unregister: and dword [rbx + 0x560], ~2
    {0x2F76312, "48 8B 35 A7 89 C8 04"},             // mov rsi, [R]
    {0x2F7E023, "48 63 03 45 33 E4 4C 8B 73 08"},    // the search: count at +0, data at +8
    {0x2F7E076, "8B 87 60 05 00 00"},                // its flag read
};
constexpr unsigned kRegCap = 256;
// The parking manager (hidden tests only: logged by action vehicle_sync 7): [kParkGlobal] + 0xD0, 9 slots of 0x58 bytes,
// the state byte first (2 = the slot whose vehicle the garage menu spawns, 0x142D6ECE0).
constexpr std::uintptr_t kParkGlobal = 0x7BF5798;
constexpr Site kParkSite = {0x2D6ECEA, "48 8B 1D A7 6A E8 04"};

// ---------------------------------------------------------------- the copies (receiver)
struct CopyDef {
    const char* key;
    const char* path;
    const char* guid;
    std::uint32_t layout;
    std::int32_t joints;
    bool welcome;  // a welcome / construction hologram entity (StartVisible; its own checks)
    bool solid;    // draws a drivable vehicle's own art: its collision is turned off on the copy (CollisionOff)
};
// Section 22.11 (after v0.13): the SOLID copies are the default. Their art carries the drivable vehicles' collision
// (part_bkst_main_def 8 and part_cald_main_def_game 18 PolySoup shapes; Sam stood on the trike copy and was stopped inside
// the truck copy, 22.10), so every solid copy has its collision turned off on its own entity before it is shown
// (Entity::SetCollisionEnabled(false), the game's own per-instance switch; nothing shared is written) and checked every
// 250 ms (CollisionWatch). The construction holograms (no collision shapes; the drivable skeletons' layouts) are the
// opt-in vehicle_sync=3, the roadster's copy (no solid roadster copy exists: 22.8, 22.11) and the logged fallback when a
// solid copy cannot be made safe.
constexpr unsigned kCopyCount = 5;
constexpr int kDefRoadsterHolo = 0, kDefTrike = 1, kDefTruck = 2, kDefTrikeHolo = 3, kDefTruckHolo = 4;
constexpr CopyDef kCopies[kCopyCount] = {
    // the roadster: no cutscene body exists; its construction hologram (ForConstruction: no fade near the player),
    // spawned safely in the catalog (docs/character-catalog.md #89), the drivable roadster's 23-joint layout
    {"roadster_hologram", "ds/entities/npc/customize_holo/construction/construction_holo_roadster", "8b3517fe-5706-d73b-b6c8-3427b5af39a6",
     0xECF43324u, 23, true, false},
    // the reverse trike's cutscene body: the drivable trike's art (part_bkst_main_def) and Skeleton (also the Kusanagi's copy)
    {"trike", "ds/entities/cinematics/demo_bkst_main_def_pro_new", "161eefa7-9549-c74f-a8b4-550e9eedfb85", 0x8DABA9B0u, 40, false, true},
    // the truck's cutscene body: the drivable truck's art (part_cald_main_def_game) and Skeleton
    {"truck", "ds/entities/cinematics/demo_cald_main_def_game", "58c639c3-76ef-3446-bfae-fda96943f5d8", 0xB609EC0Du, 47, false, true},
    // the trike's and the truck's construction holograms (customize_holo_*_HoloNpcNotHuman, 14.99 Hz)
    {"trike_hologram", "ds/entities/npc/customize_holo/construction/construction_holo_trike", "4ffc6e8c-e6fe-9a30-aecb-ae8472e1bc8b", 0x8DABA9B0u, 40,
     true, false},
    {"truck_hologram", "ds/entities/npc/customize_holo/construction/construction_holo_truck", "53d0f9dc-e4ab-853a-b8d5-3ba6245a92d9", 0xB609EC0Du, 47,
     true, false},
};
// 0 off; 1 (default) and 2: the solid copies (2 was v0.13's "solid copies" value); 3: all three kinds as construction
// holograms (opt-in; v0.13's default); 4: the roadster's copy only (the v0.12 default)
int g_mode = 1;
bool g_real_fallback = false;  // mode 5 fell back to 1 at run time (vshare not in effect); back to 5 when it is

// ---------------------------------------------------------------- state
bool g_checked = false, g_ok = false;
char g_why[160] = "not checked";
bool g_reg_checked = false, g_reg_ok = false;  // the vehicle registry's bytes (overlap, the parked vehicle's liveness)
bool g_overlap_on = true;                     // session.ini vehicle_overlap_hide
double g_mute_until = 0;                      // hidden tests: our sends held until then (action vehicle_sync 10)

// sender
struct Ours {
    std::uintptr_t e = 0, vt = 0;
    std::uintptr_t sam = 0;  // the local Sam it was ours with (a load makes a new Sam: it is forgotten)
    double broken_ms = 0;    // when it was first seen broken (state 5) with Sam off it
    game::Guid guid;
    int cls = -1;
    const wire::VehicleResource* res = nullptr;
    std::uint8_t res_guid[16] = {};
    bool seated = false;
    double left_ms = 0;  // when Sam got off (0 while seated)
    bool parked_logged = false;
    double last_ms = 0;
    double last_pos[3] = {};
    float speed = 0;
    std::uint32_t sent = 0, joint_frames = 0, parked_sent = 0;
    double last_sent_ms = 0;  // the last send tick that reported it (its look goes out only while it is reported)
    // its battery / defense packs (child entities) in its frame, read while it stands still (section 22.7)
    bool packs_read = false;
    double packs_next_ms = 0;
    std::uint8_t m = 0;
    std::uint8_t pack[wire::kMaxVehiclePacks] = {};
    float pack_t[wire::kMaxVehiclePacks][3] = {};
    wire::Quat pack_q[wire::kMaxVehiclePacks];
    // its wear and lights (section 22.12: STM1 version 3's state block), read every 100 ms
    bool st_read = false;
    double st_next_ms = 0;
    look::VehicleState st;
    std::uint32_t st_hash = 0;
    // the cargo in its carrier (the cargo block): read every 500 ms while it stands still, sent on change and every 1 s
    bool cargo_read = false;
    double cargo_next_ms = 0, cargo_resend_ms = 0;
    std::uint8_t nc = 0, cargo_dropped = 0;
    wire::VehicleCargo cargo[wire::kMaxVehicleCargo];
    std::uint32_t cargo_hash = 0, cargo_sent_hash = 0;
    unsigned cargo_blocks = 0;
};
Ours g_ours;
// A parked vehicle is reported while we stay within this distance of it (section 22.13; no time limit). Beyond it the
// other screen's copy goes (a fast travel, a long walk away), so a vehicle left far behind is not streamed for ever.
constexpr double kParkedReportM = 250.0;
// Hidden tests (action vehicle_sync 24 <n> [model]): n synthetic cargo pieces added to the cargo of the vehicle we
// report (laid out on its bed / rack), to prove the cargo block and the receiver's case props without the game's menu.
unsigned g_test_cargo = 0;
std::uint8_t g_test_cargo_model = 2;
// Looks and packs (section 22.7; session.ini vehicle_look / vehicle_packs)
bool g_look_on = true, g_packs_on = true;
double g_look_next_ms = 0, g_look_resend_ms = 0;
std::uint32_t g_look_sent_hash = 0, g_looks_sent = 0;
double g_last_take_ms = 0;  // the last send tick TakeSend ran (none while our Sam sleeps: loads, menus)
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
    std::uint32_t look_applied = 0;  // the look hash applied (section 22.7); 0 none
    double look_check_ms = 0;
    std::uintptr_t vt = 0;
    game::Guid guid;
    double begin_ms = 0, live_ms = 0;
    double freq_next_ms = 0;
    std::uint8_t freq_orig = 0xFF;  // its own update frequency (BoostEntity)
    unsigned fails = 0;
    double retry_ms = 0;
    // a solid copy: its collision off (CollisionOff at realize), checked every 250 ms (CollisionWatch)
    double coll_next_ms = 0;
    unsigned coll_again = 0;  // times it was found on again and turned off again
    // section 22.12: the partner vehicle's wear and lights as last set on it (DriveState), and the cargo block in play
    bool st_applied = false;
    std::uint16_t st_wear = 0;
    std::uint64_t st_parts[2] = {};
    std::uint8_t st_nl = 0;
    wire::VehicleLight st_light[wire::kMaxVehicleLights];
    double st_full_ms = 0, st_fail_ms = 0;
    std::uint32_t st_look = 0;
    unsigned st_checks = 0;
    std::uint32_t cargo_seq = 0;
    bool cargo_have = false;
    std::uint8_t nc = 0;
    wire::VehicleCargo cargo[wire::kMaxVehicleCargo];
    // section 22.13: the drivable resource whose frames it plays (another model on the same copy, the standard trike and
    // the Kusanagi, respawns it in its own look), and since when it plays a parked vehicle
    const wire::VehicleResource* res = nullptr;
    double parked_since = 0;
    bool parked_logged = false;
};
Copy g_copy;
std::uint32_t g_state_applies = 0, g_state_full = 0, g_state_sets = 0, g_state_failed = 0, g_state_mismatch = 0;
// The case props of the partner vehicle's cargo (section 22.12): one per piece, placed in the copy's frame.
struct CargoCopy {
    holo::Spawn sp;
    const char* path = nullptr;  // the prop it loads (gear::CargoProp), nullptr: none
    const char* key = nullptr;
    std::uint8_t variation = wire::kGearNone;
    bool loading = false, live = false, shown = false, looked = false, checked = false;
    double begin_ms = 0, live_ms = 0, retry_ms = 0;
    WorldTransform last{};
    double coll_next_ms = 0;  // its collision checked again every second (section 22.13; also right after its colour)
    unsigned coll_again = 0;
};
CargoCopy g_cargo[wire::kMaxVehicleCargo];
bool g_cargo_refused = false;  // a cargo prop failed its checks or a set-transform: none for this game run
std::uint32_t g_cargo_spawns = 0, g_cargo_removes = 0, g_cargo_faults = 0, g_cargo_looks = 0;
float g_cargo_err_max = 0;
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
// Overlap with the watcher's own vehicle of the same kind (vehicle_overlap_hide): the copy hidden while one stands in
// the same place, shown again once it has been clear for 0.5 s.
struct OverlapState {
    bool hidden = false;
    double next_ms = 0, clear_since = 0;
    std::uintptr_t with = 0;
    unsigned hides = 0, shows = 0, unreadable = 0;
};
OverlapState g_ovl;
// The partner's vehicle look (STV1 item 255) and the copies of its packs (section 22.7).
struct LookRx {
    bool have = false;
    wire::GearLook l;
    std::uint32_t hash = 0;
} g_lrx;
std::uint32_t g_look_rx = 0, g_look_refused = 0, g_look_applies = 0, g_look_again = 0, g_look_partial = 0, g_look_failed = 0;
struct PackDef {
    const char* key;
    const char* path;
    const char* guid;
};
constexpr PackDef kPacks[wire::kVehiclePackMax] = {
    {"battery_pack", "ds/entities/vehicle/pe_ds_vehicle_battery_pack", "e31fcbb8-1ce3-5549-a7df-c0bc519504a7"},
    {"defense_pack", "ds/entities/vehicle/pe_ds_vehicle_defense_pack", "a697fc82-7564-6448-870e-4804c472104b"},
};
struct PackCopy {
    holo::Spawn sp;
    int def = -1;
    bool loading = false, live = false, shown = false, checked = false, placed = false;
    double begin_ms = 0, live_ms = 0, retry_ms = 0;
    WorldTransform last{};  // where it was put last tick
    double coll_next_ms = 0;  // its collision checked again every second (section 22.13)
    unsigned coll_again = 0;
};
PackCopy g_pack[wire::kMaxVehiclePacks];
bool g_pack_refused[wire::kVehiclePackMax] = {};
std::uint32_t g_pack_spawns = 0, g_pack_removes = 0, g_pack_faults = 0;
float g_pack_err_max = 0;  // hidden tests / trace: a pack copy's read-back place against where it was put last tick (m)
// The copy held where it stands while the partner is silent (link stall, its load or pause): not removed at 2 s, but
// when the partner body goes (10 s) or frames come again.
bool g_holding = false;
std::uint32_t g_holds = 0;
int g_override = 0;  // 0 auto, 1..3 = copy 0..2
float g_off[3] = {};  // copy offset in the vehicle's frame (m): right, forward, up
double g_broken_since = 0;
// Where the partner's vehicle plays this tick (the copy's place, also with no copy), for its sounds (section 22.9):
// set while frames play (not while held or gone), stale otherwise.
WorldTransform g_snd_place{};
double g_snd_ms = -1e9;
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
int g_aim_kind = 0;      // hidden tests (action vehicle_sync 18): the camera aim check is on for this vehicle filter
double g_aim_tol = 25;   // its tolerance (deg; vehicle_sync 18 <kind> [deg])
unsigned g_aims = 0;
std::uint32_t g_moving_ticks = 0;  // ticks while the partner's vehicle moved (the frame-aligned meter's frames)

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

bool CheckRegistry() {
    if (g_reg_checked) return g_reg_ok;
    g_reg_checked = true;
    const std::uintptr_t b = mem::Base();
    if (!b) return false;
    for (const Site& s : kRegSites)
        if (!mem::CodeMatches(b + s.rva, s.sig)) {
            Log("[vehicle] the vehicle registry's bytes differ at ds.exe+0x%llX: overlap hiding and the registry checks are off",
                static_cast<unsigned long long>(s.rva));
            return false;
        }
    if (mem::RipTarget(b + 0x2F7509C, 3, 7) != b + kRegGlobal || mem::RipTarget(b + 0x2F76312, 3, 7) != b + kRegGlobal) {
        Log("[vehicle] the vehicle registry's global is not where expected: overlap hiding and the registry checks are off");
        return false;
    }
    g_reg_ok = true;
    Log("[vehicle] vehicle registry checks ok (ds.exe+0x%llX: count +0, entities +8, registered bit +0x560 & 2)",
        static_cast<unsigned long long>(kRegGlobal));
    return true;
}

// The registered vehicles (fault-free reads, count and array read twice and equal; at most kRegCap). -1: unreadable.
int ReadRegistry(std::uintptr_t (&out)[kRegCap]) {
    if (!CheckRegistry()) return -1;
    const std::uintptr_t r = mem::Q(mem::Base() + kRegGlobal);
    if (!r) return -1;
    std::int32_t n1 = 0, n2 = 0;
    std::uintptr_t d1 = 0, d2 = 0;
    if (!mem::Read(r, &n1, 4) || !mem::Read(r + 8, &d1, 8)) return -1;
    if (n1 < 0 || n1 > static_cast<std::int32_t>(kRegCap)) return -1;
    if (n1 == 0) return 0;
    static std::uintptr_t again[kRegCap];
    if (!d1 || !mem::Read(d1, out, static_cast<std::size_t>(n1) * 8) || !mem::Read(d1, again, static_cast<std::size_t>(n1) * 8)) return -1;
    if (!mem::Read(r, &n2, 4) || !mem::Read(r + 8, &d2, 8) || n1 != n2 || d1 != d2 || std::memcmp(out, again, static_cast<std::size_t>(n1) * 8) != 0)
        return -1;
    return n1;
}

// A vehicle in the registry with its registered bit (a vehicle removed from the world, or freed, is not).
// 1 yes, 0 no, -1 the registry is unreadable.
int Registered(std::uintptr_t e) {
    static std::uintptr_t list[kRegCap];
    const int n = ReadRegistry(list);
    if (n < 0) return -1;
    for (int i = 0; i < n; ++i)
        if (list[i] == e) {
            std::uint32_t fl = 0;
            return mem::Read(e + kVehFlags, &fl, 4) && (fl & 2) ? 1 : 0;
        }
    return 0;
}

// An entity's world transform read fault-free (+0xC8 position doubles, +0xE0 rotation): two equal reads, finite. Never
// the game's getter (which locks the entity's critical section) on an entity that may be gone.
bool ReadXf(std::uintptr_t e, WorldTransform& out) {
    std::uint8_t a[0x3C], b[0x3C];
    if (!e || !mem::Read(e + game::entity_off::kPosition, a, sizeof(a)) || !mem::Read(e + game::entity_off::kPosition, b, sizeof(b)) ||
        std::memcmp(a, b, sizeof(a)) != 0)
        return false;
    std::memcpy(&out.pos, a, 0x18);
    std::memcpy(&out.rot, a + 0x18, 0x24);
    if (!std::isfinite(out.pos.x) || !std::isfinite(out.pos.y) || !std::isfinite(out.pos.z) || std::fabs(out.pos.x) > 1e7 ||
        std::fabs(out.pos.y) > 1e7 || std::fabs(out.pos.z) > 1e7)
        return false;
    for (const auto& row : out.rot.m)
        for (float v : row)
            if (!std::isfinite(v) || std::fabs(v) > 1.5f) return false;
    return true;
}

// ---------------------------------------------------------------- solid copies made safe (section 22.11)
// Entity::SetCollisionEnabled(Entity*, bool) (entity vtable slot +0xF0, the base function in DSControlledEntity's and the
// construction holograms' vtables; checked per call): with false it clears the entity's collision flag (+0x88 bit
// 0x4000) and sends MsgDestroyCollisionVolume to its components, so the model's parts lose their physics instances
// (part +0xD0). The game's own cutscene event DisableEntityCollisionEventInstance (0x1424B7250) does exactly this per
// instance. Nothing shared is written. A part enabled later recreates its physics only while the flag is set
// (0x14238EF60).
constexpr std::uintptr_t kSetColl = 0x234A530;
constexpr std::uintptr_t kSetCollSlot = 0xF0;
constexpr const char* kSetCollSig =
    "40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40 48 8B 81 88 00 00 00 48 8B D9 48 C1 E8 0E 24 01 3A C2 0F 84 ?? ?? ?? ?? 84 D2 74 ?? "
    "80 B9 21 03 00 00 00";
constexpr std::uintptr_t kEntFlags = 0x88;
constexpr std::uint64_t kEntCollision = 0x4000;
constexpr std::uintptr_t kEntModel = 0xB8;  // its Model / SkinnedModel component
// The Model component: +0x48 its entity, +0x58 the part count, +0x60 the parts (0x100 bytes each; +0xD0 the part's
// PhysicsCollisionInstance). Model::OnModelResourceChanged (0x142393380) reads the count and the parts here:
constexpr std::uintptr_t kCompEntity = 0x48, kModelParts = 0x58, kModelPartArr = 0x60, kPartSize = 0x100, kPartPhysics = 0xD0;
constexpr Site kModelSite = {0x23933A6,
                             "83 78 20 00 75 0A 83 7F 20 00 0F 84 F0 00 00 00 48 89 6C 24 50 33 C0 48 89 74 24 58 48 8B CB 4C 89 74 24 20 44 8B 73 58 "
                             "48 89 43 58 48 8B 6B 60 48 89 43 60"};
bool g_coll_checked = false, g_coll_ok = false;
std::uint32_t g_coll_offs = 0, g_coll_again = 0, g_coll_fails = 0;

bool CollCheck() {
    if (g_coll_checked) return g_coll_ok;
    g_coll_checked = true;
    const std::uintptr_t b = mem::Base();
    g_coll_ok = b && mem::CodeMatches(b + kSetColl, kSetCollSig) && mem::CodeMatches(b + kModelSite.rva, kModelSite.sig);
    Log("[vehicle] solid copies: collision switch checks %s (Entity::SetCollisionEnabled ds.exe+0x%llX, the model's parts +0x58 / +0x60)",
        g_coll_ok ? "ok" : "FAILED: the solid copies are off, the holograms stand in", static_cast<unsigned long long>(kSetColl));
    return g_coll_ok;
}

struct CollState {
    bool read = false, on = false;
    int parts = -1, insts = 0;
};
// Fault-free reads: the entity's collision flag and how many of its model's parts carry a physics instance.
CollState ReadColl(std::uintptr_t e) {
    CollState s;
    std::uint64_t fl = 0;
    if (!e || !mem::Read(e + kEntFlags, &fl, sizeof(fl))) return s;
    s.on = (fl & kEntCollision) != 0;
    const std::uintptr_t model = mem::Q(e + kEntModel);
    std::int32_t n = -1;
    if (!model || mem::Q(model + kCompEntity) != e || !mem::Read(model + kModelParts, &n, 4) || n < 0 || n > 512) return s;
    const std::uintptr_t arr = mem::Q(model + kModelPartArr);
    if (n && !arr) return s;
    for (std::int32_t i = 0; i < n; ++i) {
        std::uintptr_t inst = 0;
        if (!mem::Read(arr + kPartSize * static_cast<std::uintptr_t>(i) + kPartPhysics, &inst, sizeof(inst))) return s;
        if (inst) ++s.insts;
    }
    s.parts = n;
    s.read = true;
    return s;
}

bool CallSetColl(std::uintptr_t fn, std::uintptr_t e, bool on) {
    __try {
        reinterpret_cast<void (*)(void*, bool)>(fn)(reinterpret_cast<void*>(e), on);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Collision off on our own copy (game thread). True when it reads off afterwards: the flag clear and no part with a
// physics instance. A fault turns the solid copies off for the session (the holograms stand in).
bool CollisionOff(Entity* ent, const char* what) {
    const auto e = reinterpret_cast<std::uintptr_t>(ent);
    if (!e || !CollCheck()) return false;
    const CollState before = ReadColl(e);
    if (!before.read) {
        Log("[vehicle] %s: its collision state is unreadable", what);
        return false;
    }
    if (!before.on && before.insts == 0) return true;
    const std::uintptr_t b = mem::Base();
    const std::uintptr_t vt = mem::Q(e);
    if (!vt || mem::Q(vt + kSetCollSlot) != b + kSetColl) {
        Log("[vehicle] %s: its class overrides SetCollisionEnabled (vtable %p): collision not switched", what, reinterpret_cast<void*>(vt));
        return false;
    }
    if (!CallSetColl(b + kSetColl, e, false)) {
        g_coll_ok = false;
        ++g_coll_fails;
        Log("[vehicle] %s: SetCollisionEnabled(false) FAULTED: the solid copies are off for this session", what);
        return false;
    }
    ++g_coll_offs;
    const CollState after = ReadColl(e);
    const bool ok = after.read && !after.on && after.insts == 0;
    if (g_coll_offs <= 40 || !ok)
        Log("[vehicle] %s: collision turned off on the copy: flag %d -> %d, physics instances %d -> %d of %d parts%s", what, before.on ? 1 : 0,
            after.on ? 1 : 0, before.insts, after.insts, after.parts, ok ? "" : " - NOT off");
    if (!ok) ++g_coll_fails;
    return ok;
}

// A live copy's collision checked again (packs, cargo props, the gear props: section 22.13). 1 off; 0 it was found on
// again and turned off again (logged); -1 unreadable, or it could not be turned off again.
std::uint32_t g_coll_watch_checks = 0;
int CollisionKeepOff(Entity* ent, const char* what) {
    const auto e = reinterpret_cast<std::uintptr_t>(ent);
    ++g_coll_watch_checks;
    const CollState s = ReadColl(e);
    if (!s.read) {
        Log("[vehicle] %s: its collision state is unreadable", what);
        return -1;
    }
    if (!s.on && s.insts == 0) return 1;
    ++g_coll_again;
    Log("[vehicle] %s: collision found ON again (flag %d, %d physics instances of %d parts): turning it off again", what, s.on ? 1 : 0, s.insts, s.parts);
    return CollisionOff(ent, what) ? 0 : -1;
}

// Where a copy, a pack copy or a cargo prop is spawned: its collision is live until its realize tick turns it off
// (section 22.13), so never near the watcher: 20 m below its place, or, when the watcher stands within 40 m of that
// point, 60 m below the lower of the watcher and its place (under the ground both stand on).
WorldTransform g_watcher{};
bool g_watcher_ok = false;
std::uint32_t g_spawn_deep = 0;
WorldTransform SpawnPlace(const WorldTransform& at) {
    WorldTransform s = at;
    s.pos.z -= 20.0;
    if (g_watcher_ok) {
        const double dx = s.pos.x - g_watcher.pos.x, dy = s.pos.y - g_watcher.pos.y, dz = s.pos.z - g_watcher.pos.z;
        if (dx * dx + dy * dy + dz * dz < 40.0 * 40.0) {
            s.pos.z = (at.pos.z < g_watcher.pos.z ? at.pos.z : g_watcher.pos.z) - 60.0;
            if (++g_spawn_deep <= 5) Log("[vehicle] spawned 60 m below (the watcher stands within 40 m of the usual spawn point)");
        }
    }
    return s;
}

// The copy for the partner's frame (section 22.11): the solid one. The roadster has no solid copy (22.8, 22.11).
// Decision D2-1 ("no hologram vehicles", section 23): in the default modes (1, 2: the fallback of the real shared vehicles)
// no hologram stands in, so the roadster and a solid copy that cannot be made safe show nothing (-1); only the opt-in
// hologram modes 3 and 4 still use the construction holograms.
int CopyFor(const wire::VehicleFrame& f, double now) {
    const wire::VehicleResource* r = wire::FindVehicleResource(f.guid);
    const bool holo = g_mode == 3;
    const bool holo_ok = g_mode == 3 || g_mode == 4;
    const bool solid_ok = !holo && CollCheck();
    auto usable = [&](int d) { return !Refused(d, now); };
    int def = -1;
    const char* why = nullptr;
    switch (f.kind) {
        case 1:
            def = kDefRoadsterHolo;
            why = holo ? "vehicle_sync=3: holograms" : "no solid roadster copy exists (section 22.11)";
            break;
        case 3:
            def = solid_ok && usable(kDefTrike) ? kDefTrike : kDefTrikeHolo;
            if (def == kDefTrikeHolo) why = holo ? "vehicle_sync=3: holograms" : !solid_ok ? "the collision switch did not check out" : "the solid trike was refused";
            break;
        case 2:
            def = solid_ok && usable(kDefTruck) ? kDefTruck : kDefTruckHolo;
            if (def == kDefTruckHolo) why = holo ? "vehicle_sync=3: holograms" : !solid_ok ? "the collision switch did not check out" : "the solid truck was refused";
            break;
        default:
            return -1;
    }
    if (!holo_ok && def >= 0 && kCopies[def].welcome) {
        static const char* none_logged[4] = {};
        const char* nwhy = f.kind == 1 ? "no solid roadster copy exists and holograms are off (D2-1)" : why;
        if (f.kind < 4 && none_logged[f.kind] != nwhy) {
            none_logged[f.kind] = nwhy;
            Log("[vehicle] the partner's %s: no copy (%s)", r ? r->key : "?", nwhy ? nwhy : "?");
        }
        return -1;
    }
    static int logged_def[4] = {-1, -1, -1, -1};
    static const char* logged_why[4] = {};
    if (logged_def[f.kind] != def || logged_why[f.kind] != why) {
        logged_def[f.kind] = def, logged_why[f.kind] = why;
        if (why) Log("[vehicle] the partner's %s: copy '%s' (%s)", r ? r->key : "?", kCopies[def].key, why);
        else Log("[vehicle] the partner's %s: copy '%s'", r ? r->key : "?", kCopies[def].key);
    }
    return def;
}

double Yaw(const WorldTransform& t) { return std::atan2(static_cast<double>(t.rot.m[1][1]), static_cast<double>(t.rot.m[1][0])); }
double YawDiffDeg(const WorldTransform& a, const WorldTransform& b) {
    double d = (Yaw(a) - Yaw(b)) * 57.29577951308232;
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return std::fabs(d);
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

// Research (action vehicle_sync 19, hidden tests): a vehicle's look (variations, shader variables, its resource's
// variation references), its baggage carrier (slots, stacks, pieces with their place in the vehicle's frame) and its
// child entities. Fault-free reads only.
void Census(std::uintptr_t v, const char* tag) {
    Describe(v, tag);
    look::Dump(tag, v, 0);
    look::DumpResourceVariations(tag, v);
    WorldTransform vw{};
    const bool have_vw = ReadXf(v, vw);
    const std::uintptr_t comp = partner::FindComponentByName(reinterpret_cast<Entity*>(v), "DSBaggage2CarrierComponent");
    const std::uintptr_t car = comp && mem::Q(comp + 0x48) == v ? mem::Q(comp + 0x50) : 0;
    std::int32_t slots = -1;
    if (!car || !mem::Read(car + 0x10, &slots, 4) || slots < 0 || slots > 64) {
        Log("[vehicle] census %s %p: baggage carrier component %p, carrier %p, %d slots: not walked", tag, reinterpret_cast<void*>(v),
            reinterpret_cast<void*>(comp), reinterpret_cast<void*>(car), slots);
    } else {
        const std::uintptr_t slot_arr = mem::Q(car + 0x18);
        unsigned walked = 0, pieces = 0;
        for (std::int32_t si = 0; si < slots && walked < 256; ++si) {
            const std::uintptr_t s = mem::Q(slot_arr + 8ull * static_cast<unsigned>(si));
            std::uint8_t type = 0;
            std::int32_t nstack = 0, total = -1;
            if (!s || !mem::Read(s + 1, &type, 1) || !mem::Read(s + 0x18, &nstack, 4) || nstack < 0 || nstack > 64) continue;
            mem::Read(s + 0xC, &total, 4);
            Log("[vehicle] census %s   slot %d type %u: %d stack(s), %d piece(s)", tag, si, type, nstack, total);
            const std::uintptr_t st_arr = mem::Q(s + 0x20);
            for (std::int32_t ti = 0; ti < nstack && walked < 256; ++ti, ++walked) {
                const std::uintptr_t t = mem::Q(st_arr + 8ull * static_cast<unsigned>(ti));
                std::int32_t n = 0;
                if (!t || !mem::Read(t + 0x10, &n, 4) || n < 0 || n > 64) continue;
                const std::uintptr_t items = mem::Q(t + 0x18);
                for (std::int32_t bi = 0; bi < n && walked < 256; ++bi, ++walked) {
                    const std::uintptr_t bag = mem::Q(items + 8ull * static_cast<unsigned>(bi));
                    const char* bt = bag ? partner::TypeName(reinterpret_cast<const void*>(bag)) : nullptr;
                    std::uint8_t model[2] = {};
                    std::int32_t eidx = -2;
                    WorldTransform bw{};
                    if (bag) {
                        mem::Read(bag + 0x20, model, 2);
                        mem::Read(bag + 0xD8, &eidx, 4);
                    }
                    std::uint8_t raw[0x3C] = {};
                    bool placed = false;
                    if (bag && mem::Read(bag + 0x38, raw, sizeof(raw))) {
                        std::memcpy(&bw.pos, raw, 0x18);
                        std::memcpy(&bw.rot, raw + 0x18, 0x24);
                        placed = std::isfinite(bw.pos.x) && std::isfinite(bw.pos.y) && std::isfinite(bw.pos.z);
                    }
                    float lp[3] = {}, lr[3][3] = {};
                    if (placed && have_vw) LocalOf(vw, bw, lp, lr);
                    wire::GearLook gl;
                    const bool lk = bag && look::ReadBaggage(bag, &gl) && gl.nvar == 1;
                    char gt[37] = "-";
                    if (lk) wire::GearGuidText(gl.guid[0], gt);
                    Log("[vehicle] census %s     slot %d stack %d piece %d: %p %s model %u (+0x21 %u) entity index %d; in the vehicle's frame "
                        "(%.3f, %.3f, %.3f) fwd (%.2f, %.2f, %.2f) up (%.2f, %.2f, %.2f)%s; case variation %s",
                        tag, si, ti, bi, reinterpret_cast<void*>(bag), bt ? bt : "?", model[0], model[1], eidx, lp[0], lp[1], lp[2], lr[1][0], lr[1][1],
                        lr[1][2], lr[2][0], lr[2][1], lr[2][2], placed ? "" : " (not placed)", gt);
                    ++pieces;
                }
            }
        }
        Log("[vehicle] census %s %p: carrier %p, %d slots, %u pieces", tag, reinterpret_cast<void*>(v), reinterpret_cast<void*>(car), slots, pieces);
    }
    // child entities (two levels): the hitch carrier, containers, lights ...
    struct N {
        std::uintptr_t e;
        int depth;
    } stack[64];
    unsigned sp = 0, seen = 0;
    for (std::uintptr_t c = mem::Q(v + 0x78), guard = 0; c && sp < 64 && guard < 64; c = mem::Q(c + 0x80), ++guard) stack[sp++] = {c, 1};
    while (sp && seen < 64) {
        const N nd = stack[--sp];
        ++seen;
        char path[160] = {};
        std::uint8_t g[16] = {};
        const char* rt = nullptr;
        realbody::ResourceIdentity(reinterpret_cast<Entity*>(nd.e), path, sizeof(path), g, &rt);
        WorldTransform cw{};
        float lp[3] = {}, lr[3][3] = {};
        const bool cp = ReadXf(nd.e, cw) && have_vw;
        if (cp) LocalOf(vw, cw, lp, lr);
        const char* t = partner::TypeName(reinterpret_cast<const void*>(nd.e));
        Log("[vehicle] census %s   child (depth %d) %p %s '%s' (%s) at (%.3f, %.3f, %.3f) in the vehicle's frame%s", tag, nd.depth,
            reinterpret_cast<void*>(nd.e), t ? t : "?", path, rt ? rt : "-", lp[0], lp[1], lp[2], cp ? "" : " (unreadable)");
        if (nd.depth < 2)
            for (std::uintptr_t c = mem::Q(nd.e + 0x78), guard = 0; c && sp < 64 && guard < 32; c = mem::Q(c + 0x80), ++guard) stack[sp++] = {c, nd.depth + 1};
    }
    if (seen == 0) Log("[vehicle] census %s %p: no child entities", tag, reinterpret_cast<void*>(v));
}

// Sender: the battery / defense packs the game shows on vehicle v (its child entities with one of the two pack
// resources, shown), each in v's frame. Fault-free reads; at most kMaxVehiclePacks.
void ReadPacks(std::uintptr_t v, const WorldTransform& vt) {
    std::uint8_t want[wire::kVehiclePackMax][16];
    for (unsigned k = 0; k < wire::kVehiclePackMax; ++k)
        if (!bodies::ParseGuid(kPacks[k].guid, want[k])) return;
    g_ours.m = 0;
    for (std::uintptr_t c = mem::Q(v + 0x78), guard = 0; c && guard < 32 && g_ours.m < wire::kMaxVehiclePacks; c = mem::Q(c + 0x80), ++guard) {
        char path[160] = {};
        std::uint8_t g[16] = {};
        if (!realbody::ResourceIdentity(reinterpret_cast<Entity*>(c), path, sizeof(path), g, nullptr)) continue;
        int k = -1;
        for (unsigned i = 0; i < wire::kVehiclePackMax; ++i)
            if (std::strcmp(path, kPacks[i].path) == 0) k = static_cast<int>(i);
        if (k < 0) continue;
        if (std::memcmp(g, want[k], 16) != 0) {
            static unsigned logs = 0;
            if (++logs <= 3) Log("[vehicle] our vehicle's %s child has another GUID than expected: not sent", kPacks[k].key);
            continue;
        }
        std::uint64_t fl = 0;
        WorldTransform cw{};
        if (!partner::ReadFlags(reinterpret_cast<Entity*>(c), &fl) || (fl & 0x2) == 0 || !ReadXf(c, cw)) continue;
        float p[3], r[3][3];
        LocalOf(vt, cw, p, r);
        if (std::fabs(p[0]) >= wire::kMaxVehiclePackM || std::fabs(p[1]) >= wire::kMaxVehiclePackM || std::fabs(p[2]) >= wire::kMaxVehiclePackM) continue;
        const unsigned i = g_ours.m++;
        g_ours.pack[i] = static_cast<std::uint8_t>(k + 1);
        for (int a = 0; a < 3; ++a) g_ours.pack_t[i][a] = p[a];
        g_ours.pack_q[i] = wire::QuatFromMatrix(r);
    }
}

// The baggage config's colour variation index of a variation GUID (kGearNone: not in the config), cached.
std::uint8_t ConfigIndexOf(const std::uint8_t (&g)[16]) {
    struct Memo {
        std::uint8_t g[16];
        std::uint8_t idx;
    };
    static Memo memo[16];
    static unsigned memo_n = 0;
    for (unsigned i = 0; i < memo_n; ++i)
        if (std::memcmp(memo[i].g, g, 16) == 0) return memo[i].idx;
    std::uint8_t idx = wire::kGearNone;
    for (unsigned i = 0; i < wire::kVehicleCargoVariations; ++i) {
        wire::GearLook l;
        if (!look::ReadConfigVariation(i, &l)) break;
        if (l.nvar == 1 && std::memcmp(l.guid[0], g, 16) == 0) {
            idx = static_cast<std::uint8_t>(i);
            break;
        }
    }
    Memo& m = memo[memo_n < 16 ? memo_n++ : 15];
    std::memcpy(m.g, g, 16);
    m.idx = idx;
    return idx;
}

// Sender: the cargo in vehicle v's DSBaggage2 carrier (every slot: Default 0, PassengerSeat 69 ...), each drawn piece
// (gear::CargoModelOf: delivery cases) in v's frame from its own world transform (+0x38 position, +0x50 rotation rows),
// its colour variation as the baggage config's index. Fault-free reads; at most kMaxVehicleCargo (the rest counted).
// The carrier walk (also for the real vehicles of section 23.16: which of the partner's pieces our own vehicle carries already).
unsigned WalkCarrier(std::uintptr_t v, const WorldTransform& vt, wire::VehicleCargo* out, unsigned cap, std::uint8_t* dropped, unsigned* seen_out,
                     unsigned* not_drawn_out, unsigned* unplaced_out, bool log_not_drawn) {
    const std::uintptr_t comp = partner::FindComponentByName(reinterpret_cast<Entity*>(v), "DSBaggage2CarrierComponent");
    const std::uintptr_t car = comp && mem::Q(comp + 0x48) == v ? mem::Q(comp + 0x50) : 0;
    std::int32_t slots = -1;
    unsigned nc = 0;
    *dropped = 0;
    unsigned seen = 0, not_drawn = 0, unplaced = 0;
    if (car && mem::Read(car + 0x10, &slots, 4) && slots >= 0 && slots <= 64) {
        const std::uintptr_t slot_arr = mem::Q(car + 0x18);
        unsigned walked = 0;
        for (std::int32_t si = 0; si < slots && walked < 256; ++si) {
            const std::uintptr_t s = mem::Q(slot_arr + 8ull * static_cast<unsigned>(si));
            std::int32_t nstack = 0;
            if (!s || !mem::Read(s + 0x18, &nstack, 4) || nstack < 0 || nstack > 64) continue;
            const std::uintptr_t st_arr = mem::Q(s + 0x20);
            for (std::int32_t ti = 0; ti < nstack && walked < 256; ++ti, ++walked) {
                const std::uintptr_t t = mem::Q(st_arr + 8ull * static_cast<unsigned>(ti));
                std::int32_t n = 0;
                if (!t || !mem::Read(t + 0x10, &n, 4) || n < 0 || n > 64) continue;
                const std::uintptr_t items = mem::Q(t + 0x18);
                for (std::int32_t bi = 0; bi < n && walked < 256; ++bi, ++walked) {
                    const std::uintptr_t bag = mem::Q(items + 8ull * static_cast<unsigned>(bi));
                    if (!bag) continue;
                    ++seen;
                    const std::uint8_t model = gear::CargoModelOf(bag);
                    if (model >= wire::kVehicleCargoModels) {
                        ++not_drawn;
                        static unsigned nd_logs = 0;
                        if (log_not_drawn && ++nd_logs <= 12) {
                            std::uint8_t mb = 0, lf[5] = {};
                            std::uint8_t slot_type = 0;
                            mem::Read(s + 1, &slot_type, 1);
                            mem::Read(bag + 0x20, &mb, 1);
                            const std::uintptr_t li = mem::Q(bag + 0x28);
                            if (li) mem::Read(li + 0x3A, lf, sizeof(lf));
                            const char* bt = partner::TypeName(reinterpret_cast<const void*>(bag));
                            std::uint8_t raw[0x3C] = {};
                            WorldTransform bw{};
                            float p[3] = {}, r[3][3] = {};
                            if (mem::Read(bag + 0x38, raw, sizeof(raw))) {
                                std::memcpy(&bw.pos, raw, 0x18);
                                std::memcpy(&bw.rot, raw + 0x18, 0x24);
                                LocalOf(vt, bw, p, r);
                            }
                            Log("[vehicle] our vehicle's carrier: slot %d (type %u) piece %p %s, model byte %u, attribute %u, case type %u, volume %u, at "
                                "(%.2f, %.2f, %.2f) in the vehicle's frame: not drawn (equipment / tools)",
                                si, slot_type, reinterpret_cast<void*>(bag), bt ? bt : "?", mb, lf[0], lf[1], lf[4], static_cast<double>(p[0]),
                                static_cast<double>(p[1]), static_cast<double>(p[2]));
                        }
                        continue;
                    }
                    std::uint8_t raw[0x3C] = {};
                    WorldTransform bw{};
                    if (!mem::Read(bag + 0x38, raw, sizeof(raw))) {
                        ++unplaced;
                        continue;
                    }
                    std::memcpy(&bw.pos, raw, 0x18);
                    std::memcpy(&bw.rot, raw + 0x18, 0x24);
                    float p[3], r[3][3];
                    LocalOf(vt, bw, p, r);
                    bool ok = std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
                    for (int a = 0; a < 3 && ok; ++a) ok = std::fabs(p[a]) < wire::kMaxVehicleCargoM;
                    for (int k = 0; k < 3 && ok; ++k) {
                        const float len = bw.rot.m[k][0] * bw.rot.m[k][0] + bw.rot.m[k][1] * bw.rot.m[k][1] + bw.rot.m[k][2] * bw.rot.m[k][2];
                        ok = len > 0.9f && len < 1.1f;
                    }
                    if (!ok) {
                        ++unplaced;
                        continue;
                    }
                    if (nc >= cap) {
                        ++*dropped;
                        continue;
                    }
                    wire::VehicleCargo& c = out[nc++];
                    c.model = model;
                    wire::GearLook gl;
                    c.variation = look::ReadBaggage(bag, &gl) && gl.nvar == 1 ? ConfigIndexOf(gl.guid[0]) : wire::kGearNone;
                    for (int a = 0; a < 3; ++a) c.t[a] = p[a];
                    c.q = wire::QuatFromMatrix(r);
                }
            }
        }
    }
    if (seen_out) *seen_out = seen;
    if (not_drawn_out) *not_drawn_out = not_drawn;
    if (unplaced_out) *unplaced_out = unplaced;
    return nc;
}

void ReadCargo(std::uintptr_t v, const WorldTransform& vt) {
    unsigned seen = 0, not_drawn = 0, unplaced = 0;
    g_ours.nc = static_cast<std::uint8_t>(WalkCarrier(v, vt, g_ours.cargo, wire::kMaxVehicleCargo, &g_ours.cargo_dropped, &seen, &not_drawn, &unplaced, true));
    // hidden tests: synthetic pieces on its bed / rack (vehicle_sync 24)
    for (unsigned i = 0; i < g_test_cargo && g_ours.nc < wire::kMaxVehicleCargo; ++i) {
        const int cls = g_ours.cls;
        // on its roof / behind the rider, where a watcher sees them (the truck's bed is closed)
        const float z = cls == 1 ? 2.75f : cls == 2 ? 1.3f : 1.45f, y0 = cls == 1 ? -0.9f : cls == 2 ? -0.95f : -1.3f;
        wire::VehicleCargo& c = g_ours.cargo[g_ours.nc++];
        c.model = g_test_cargo_model;
        wire::GearLook gl;
        c.variation = look::ReadConfigVariation(i % 4, &gl) ? static_cast<std::uint8_t>(i % 4) : wire::kGearNone;
        c.t[0] = -0.45f + 0.45f * static_cast<float>(i % 3);
        c.t[1] = y0 - 0.5f * static_cast<float>((i / 3) % 6);
        c.t[2] = z + 0.45f * static_cast<float>(i / 18);
        c.q = wire::Quat{};
    }
    std::uint32_t h = 2166136261u;
    auto mix = [&](std::uint32_t x) { h = (h ^ x) * 16777619u; };
    mix(g_ours.nc), mix(g_ours.cargo_dropped);
    for (unsigned i = 0; i < g_ours.nc; ++i) {
        const wire::VehicleCargo& c = g_ours.cargo[i];
        mix(c.model), mix(c.variation);
        for (float x : c.t) mix(static_cast<std::uint32_t>(std::lround(x * 100.0f)));
    }
    const bool changed = !g_ours.cargo_read || h != g_ours.cargo_hash;
    g_ours.cargo_hash = h ? h : 1;
    if (changed)
        Log("[vehicle] our vehicle %p carries %u cargo piece(s) drawn (%u in its carrier, %u not drawn: equipment / tools, %u not placed, %u over the cap; "
            "%u synthetic)",
            reinterpret_cast<void*>(v), static_cast<unsigned>(g_ours.nc), seen, not_drawn, unplaced, static_cast<unsigned>(g_ours.cargo_dropped),
            g_test_cargo < wire::kMaxVehicleCargo ? g_test_cargo : wire::kMaxVehicleCargo);
    g_ours.cargo_read = true;
}

// Receiver: a pack copy (the game's own pack entity, by path + GUID) must be a plain Entity with no interaction,
// physics, ragdoll, controller, capsule, vehicle mover, bounder or AI component (its mover, DSVehicleCustomPackMover,
// only sets its own shader variables when added: section 22.7).
const char* ValidatePack(Entity* e) {
    const auto a = reinterpret_cast<std::uintptr_t>(e);
    const std::uintptr_t rtti = mem::RttiOf(a);
    if (!rtti) return "entity type unknown";
    const char* tn = mem::RttiName(rtti);
    if (!tn || std::strcmp(tn, "Entity") != 0) return "not a plain Entity";
    if (ClassOf(a) >= 0) return "a drivable vehicle";
    for (const char* bad : {"DSInteractionComponent", "PhysicsCollisionComponent", "PhysicsRagdollComponent", "DSPlayerControllerComponent",
                            "HumanoidController", "CapsuleComponent", "VehicleMover", "DSBounderComponent", "AIIndividualComponent",
                            "AIBodyComponent", "DSAIIndividualComponent", "BasicControlledEntityAIController", "SoundComponent"})
        if (partner::FindComponentByName(e, bad)) return bad;
    return nullptr;
}

void ClearPack(PackCopy& p, const char* why) {
    if (p.loading || p.live || p.sp.sp) {
        char step[160] = {};
        holo::SpawnTeardown(p.sp, why, step, sizeof(step));
        ++g_pack_removes;
    }
    p = PackCopy{};
}
void ClearPacks(const char* why) {
    unsigned n = 0;
    for (PackCopy& p : g_pack) {
        n += (p.loading || p.live || p.sp.sp) ? 1u : 0u;
        ClearPack(p, why);
    }
    if (n) Log("[vehicle] %u pack copy(ies) removed (%s)", n, why);
}

// The copies of the partner vehicle's packs, placed in the copy's frame every tick (the newer frame's pack places:
// they are fixed on the vehicle), shown with the copy (hidden with it while it overlaps our own vehicle).
void DrivePacks(const WorldTransform& v, const wire::VehicleFrame& f, bool show, double now) {
    const unsigned want = g_packs_on && g_copy.def >= 0 && kCopies[g_copy.def].solid ? f.m : 0;
    unsigned started = 0;
    for (unsigned i = 0; i < wire::kMaxVehiclePacks; ++i) {
        PackCopy& p = g_pack[i];
        const int def = i < want ? static_cast<int>(f.pack[i]) - 1 : -1;
        if (p.def != def) {
            if (p.loading || p.live || p.sp.sp) ClearPack(p, "the partner's packs changed");
            p = PackCopy{};
            p.def = def;
        }
        if (def < 0 || g_pack_refused[def]) continue;
        WorldTransform at{};
        float r[3][3];
        wire::MatrixFromQuat(f.pack_q[i], r);
        Compose(v, f.pack_t[i], r, at);
        if (!p.loading && !p.live) {
            if (now < p.retry_ms || started >= 2) continue;
            std::uint8_t g[16];
            char why[200] = {};
            const WorldTransform below = SpawnPlace(at);
            if (!bodies::ParseGuid(kPacks[def].guid, g)) {
                g_pack_refused[def] = true;
                continue;
            }
            ++started;
            if (realbody::BeginByPathGuid(kPacks[def].path, g, below, 0x77000000u + i, &p.sp, why, sizeof(why))) {
                p.loading = true;
                p.begin_ms = now;
                ++g_pack_spawns;
                if (g_pack_spawns <= 16) Log("[vehicle] pack copy %u '%s' loading", i, kPacks[def].key);
            } else {
                p.retry_ms = now + 10000;
                Log("[vehicle] pack copy %u '%s' not started: %s", i, kPacks[def].key, why);
            }
            continue;
        }
        if (p.loading) {
            char why[200] = {};
            switch (holo::SpawnPoll(p.sp, why, sizeof(why))) {
                case holo::SpawnState::loading:
                    if (now - p.begin_ms > 20000) {
                        Log("[vehicle] pack copy %u '%s' did not load in 20 s", i, kPacks[def].key);
                        ClearPack(p, "load timeout");
                        p.def = def, p.retry_ms = now + 30000;
                    }
                    continue;
                case holo::SpawnState::cancelled:
                case holo::SpawnState::failed:
                    Log("[vehicle] pack copy %u '%s' did not load: %s", i, kPacks[def].key, why);
                    ClearPack(p, "load failed");
                    p.def = def, p.retry_ms = now + 10000;
                    continue;
                case holo::SpawnState::realized: {
                    p.loading = false;
                    const char* bad = p.sp.e ? ValidatePack(p.sp.e) : "no entity";
                    if (bad) {
                        g_pack_refused[def] = true;
                        Log("[vehicle] pack copy '%s' refused for this game run: %s", kPacks[def].key, bad);
                        if (p.sp.e) partner::DescribeEntity(p.sp.e, "refused pack copy");
                        ClearPack(p, "refused");
                        continue;
                    }
                    partner::SetVisible(p.sp.e, false);
                    partner::WakeIfAsleep(p.sp.e);
                    // each pack copy carries one physics instance (run 20261118-194926): off as on the vehicle copies (section 22.11)
                    if (!CollisionOff(p.sp.e, kPacks[def].key)) {
                        g_pack_refused[def] = true;
                        Log("[vehicle] pack copy '%s' refused for this game run: its collision could not be turned off", kPacks[def].key);
                        ClearPack(p, "collision not off");
                        continue;
                    }
                    p.live = true;
                    p.live_ms = now;
                    static bool described[wire::kVehiclePackMax] = {};
                    if (!described[def]) {
                        described[def] = true;
                        partner::DescribeEntity(p.sp.e, kPacks[def].key);
                    }
                    break;
                }
            }
        }
        if (!p.live) continue;
        if (!holo::SpawnAlive(p.sp)) {
            ClearPack(p, "destroyed by the game");
            p.def = def, p.retry_ms = now + 10000;
            continue;
        }
        // its collision stays off (every second, section 22.13): on again more than 3 times, or not off: refused
        if (now >= p.coll_next_ms) {
            p.coll_next_ms = now + 1000;
            const int k = CollisionKeepOff(p.sp.e, kPacks[def].key);
            if (k == 0) ++p.coll_again;
            if (k < 0 || p.coll_again > 3) {
                g_pack_refused[def] = true;
                Log("[vehicle] pack copy '%s' refused for this game run: its collision %s", kPacks[def].key, k < 0 ? "could not be kept off" : "kept coming back");
                ClearPack(p, "collision not off");
                continue;
            }
        }
        // where the game left it since our last placement (its mover must not move it): hidden tests / trace only
        if ((automation::Enabled() || g_trace_until > now) && p.shown && p.placed && now - p.live_ms > 1000) {
            WorldTransform back{};
            if (ReadXf(reinterpret_cast<std::uintptr_t>(p.sp.e), back)) {
                const float d = static_cast<float>(Dist(back, p.last));
                if (d > g_pack_err_max) g_pack_err_max = d;
                if (!p.checked) {
                    p.checked = true;
                    Log("[vehicle] pack copy %u '%s' placed: read back %.3f m from where it was put (the vehicle moved %.2f m/s)", i, kPacks[def].key,
                        static_cast<double>(d), static_cast<double>(f.speed));
                }
            }
        }
        if (!game::TrySetTransform(p.sp.e, at)) {
            ++g_pack_faults;
            g_pack_refused[def] = true;  // a fault: not again this game run
            ClearPack(p, "set transform fault");
            continue;
        }
        p.last = at, p.placed = true;
        if (show != p.shown) {
            partner::SetVisible(p.sp.e, show);
            p.shown = show;
        } else if (show) {
            std::uint64_t fl = 0;
            if (partner::ReadFlags(p.sp.e, &fl) && (fl & 0x2) == 0) partner::SetVisible(p.sp.e, true);
        }
    }
}

// The drivable resource's copy: the copy whose art the resource uses (the look applies only there): the trike's
// (ca03) and the trucks' (ca02, celd) art is their solid copies'; the Kusanagi trike's art is its own, the roadster's
// copy a hologram.
int ArtCopyOf(const wire::VehicleResource* r) {
    if (!r) return -1;
    if (std::strcmp(r->key, "trike") == 0) return kDefTrike;
    if (std::strcmp(r->key, "truck") == 0 || std::strcmp(r->key, "truck_npc") == 0) return kDefTruck;
    return -1;  // the roadster (hologram) and the Kusanagi (the trike copy: another model)
}
std::uint32_t LookKey(const wire::GearLook& l) { return wire::GearLookHash(l) ^ (0x9E3779B1u * (l.layout + 1u)); }

// The partner vehicle's look on the copy: applied when it changed, checked every 2 s (applied again when the copy's
// own components changed it). Only on the solid copy of the same art, only the look of the vehicle being played.
void DriveLook(int def, const wire::VehicleFrame& f, double now) {
    if (!g_look_on || !g_lrx.have || !kCopies[def].solid || !g_copy.shown || now - g_copy.live_ms < 500) return;
    if (g_lrx.l.layout >= wire::kVehicleResourceCount) return;
    const wire::VehicleResource* r = &wire::VehicleResources()[g_lrx.l.layout];
    if (wire::FindVehicleResource(f.guid) != r) return;  // the look of another vehicle (a frame or two at a change)
    if (ArtCopyOf(r) != def) {
        static const wire::VehicleResource* logged = nullptr;
        if (logged != r) {
            logged = r;
            Log("[vehicle] the partner's %s: its look is not applied (copy '%s' has another model)", r->key, kCopies[def].key);
        }
        return;
    }
    const auto copy = reinterpret_cast<std::uintptr_t>(g_copy.sp.e);
    if (g_copy.look_applied == g_lrx.hash) {
        if (now < g_copy.look_check_ms) return;
        g_copy.look_check_ms = now + 2000;
        if (look::SatisfiedVehicle(copy, g_lrx.l)) return;
        ++g_look_again;
    }
    // our own vehicles of the sender's resource: sources of its variations (fault-free reads of the registry)
    static std::uintptr_t list[kRegCap];
    std::uintptr_t own[16];
    unsigned n_own = 0;
    const int n = ReadRegistry(list);
    for (int i = 0; i < n && n_own < 16; ++i) {
        std::uint32_t fl = 0;
        if (ClassOf(list[i]) < 0 || !Rd32(list[i] + kVehFlags, fl) || !(fl & 2)) continue;
        char path[160] = {};
        std::uint8_t g[16] = {};
        if (realbody::ResourceIdentity(reinterpret_cast<Entity*>(list[i]), path, sizeof(path), g, nullptr) && wire::FindVehicleResource(g) == r)
            own[n_own++] = list[i];
    }
    char why[200] = {};
    const look::ApplyResult res = look::ApplyVehicle(copy, g_lrx.l, own, n_own, why, sizeof(why));
    g_copy.look_applied = g_lrx.hash;
    g_copy.look_check_ms = now + 2000;
    g_copy.st_full_ms = 0;  // a variation rebuilds the materials: the wear and lights all set again (DriveState, this tick)
    ++g_look_applies;
    if (res == look::ApplyResult::partial) ++g_look_partial;
    if (res == look::ApplyResult::failed || res == look::ApplyResult::off) ++g_look_failed;
    if (g_look_applies <= 40)
        Log("[vehicle] look of the partner's %s on copy '%s' (%u variation(s), %u colour(s); %u of our own vehicles of that resource as sources): %s%s%s",
            r->key, kCopies[def].key, g_lrx.l.nvar, g_lrx.l.nsv, n_own,
            res == look::ApplyResult::done ? "applied" : res == look::ApplyResult::partial ? "partly applied" : res == look::ApplyResult::off ? "looks OFF" : "FAILED",
            why[0] ? ": " : "", why);
}

// The partner vehicle's wear and lights (STM1 version 3's state block, section 22.12) on the solid copy of the same art:
// what changed since the last frame, and everything every 2 s and after each look apply (a variation rebuilds the
// materials). Wear on every part the sender's bits name, a part no longer named set to 0; a light the sender no longer
// names set to 0. Fails closed: a failed set waits 2 s (logged); a fault turns the looks off (gear_look).
void DriveState(int def, const wire::VehicleFrame& f, double now) {
    if (!g_look_on || !kCopies[def].solid || !g_copy.shown || now - g_copy.live_ms < 500 || !(f.flags & wire::kVehState)) return;
    const wire::VehicleResource* r = wire::FindVehicleResource(f.guid);
    if (ArtCopyOf(r) != def) {
        static const wire::VehicleResource* logged = nullptr;
        if (logged != r) {
            logged = r;
            Log("[vehicle] the partner's %s: its wear and lights are not applied (copy '%s' has another model)", r ? r->key : "?", kCopies[def].key);
        }
        return;
    }
    if (now < g_copy.st_fail_ms) return;
    const bool full = !g_copy.st_applied || now >= g_copy.st_full_ms;  // DriveLook sets st_full_ms 0 after every look apply
    static wire::GearShaderVar vars[wire::kMaxVehicleWearParts + 2 * wire::kMaxVehicleLights];
    unsigned n = 0;
    const bool wear_changed = full || f.wear != g_copy.st_wear || f.wear_parts[0] != g_copy.st_parts[0] || f.wear_parts[1] != g_copy.st_parts[1];
    if (wear_changed) {
        const float w = static_cast<float>(f.wear) / 65535.0f;
        for (unsigned part = 0; part < wire::kMaxVehicleWearParts; ++part) {
            const bool now_on = (f.wear_parts[part / 64] >> (part % 64)) & 1u;
            const bool was_on = g_copy.st_applied && ((g_copy.st_parts[part / 64] >> (part % 64)) & 1u);
            if (!now_on && !was_on) continue;
            wire::GearShaderVar& s = vars[n++];
            s = wire::GearShaderVar{};
            s.hash = wire::kVehWearHash, s.part = static_cast<std::uint16_t>(part), s.count = 1, s.v[0] = now_on ? w : 0.0f;
        }
    }
    auto find = [](const wire::VehicleLight* ls, unsigned ln, const wire::VehicleLight& l) -> const wire::VehicleLight* {
        for (unsigned k = 0; k < ln; ++k)
            if (ls[k].part == l.part && ls[k].kind == l.kind) return &ls[k];
        return nullptr;
    };
    for (unsigned i = 0; i < f.nl; ++i) {
        const wire::VehicleLight& l = f.light[i];
        const wire::VehicleLight* was = g_copy.st_applied ? find(g_copy.st_light, g_copy.st_nl, l) : nullptr;
        if (!full && was && was->v[0] == l.v[0] && was->v[1] == l.v[1] && was->v[2] == l.v[2]) continue;
        wire::GearShaderVar& s = vars[n++];
        s = wire::GearShaderVar{};
        const bool colour = l.kind == static_cast<std::uint8_t>(wire::VehicleLightKind::colour);
        s.hash = colour ? wire::kVehGaugeHash : wire::kVehLightHash, s.part = l.part, s.count = static_cast<std::uint8_t>(colour ? 3 : 1);
        for (int c = 0; c < 3; ++c) s.v[c] = l.v[c];
    }
    for (unsigned i = 0; g_copy.st_applied && i < g_copy.st_nl; ++i) {
        const wire::VehicleLight& l = g_copy.st_light[i];
        if (l.kind != static_cast<std::uint8_t>(wire::VehicleLightKind::intensity) || find(f.light, f.nl, l)) continue;
        wire::GearShaderVar& s = vars[n++];
        s = wire::GearShaderVar{};
        s.hash = wire::kVehLightHash, s.part = l.part, s.count = 1;  // gone on the sender: dark
    }
    if (n == 0) return;
    const auto copy = reinterpret_cast<std::uintptr_t>(g_copy.sp.e);
    char why[200] = {};
    const look::ApplyResult res = look::SetVehicleVars(copy, vars, n, why, sizeof(why));
    ++g_state_applies;  // every apply: a full one (everything) or only what changed
    if (full) ++g_state_full;
    g_state_sets += n;
    if (res == look::ApplyResult::failed || res == look::ApplyResult::off) {
        ++g_state_failed;
        g_copy.st_fail_ms = now + 2000;
        static unsigned fail_logs = 0;
        if (++fail_logs <= 10) Log("[vehicle] wear / lights of the partner's %s on copy '%s': %s%s%s", r->key, kCopies[def].key,
                                   res == look::ApplyResult::off ? "looks OFF" : "FAILED", why[0] ? ": " : "", why);
        return;
    }
    g_copy.st_applied = true;
    g_copy.st_wear = f.wear;
    g_copy.st_parts[0] = f.wear_parts[0], g_copy.st_parts[1] = f.wear_parts[1];
    g_copy.st_nl = f.nl;
    for (unsigned i = 0; i < f.nl; ++i) g_copy.st_light[i] = f.light[i];
    if (full) {
        g_copy.st_full_ms = now + 2000;
        g_copy.st_look = g_copy.look_applied;
    }
    static unsigned logs = 0;
    if (full && ++logs <= 30) {
        unsigned wear_parts = 0;
        for (std::uint64_t w : f.wear_parts)
            for (std::uint64_t b = w; b; b &= b - 1) ++wear_parts;
        Log("[vehicle] wear / lights of the partner's %s on copy '%s': wear %.3f on %u part(s), %u light(s); %u variable(s) set%s%s%s", r->key,
            kCopies[def].key, f.wear / 65535.0, wear_parts, static_cast<unsigned>(f.nl), n, res == look::ApplyResult::partial ? " (partly)" : "",
            why[0] ? ": " : "", why);
    }
    // hidden tests / trace: read back what the copy holds now (every full apply, the first 20 times)
    if (full && (automation::Enabled() || g_trace_until > now) && g_copy.st_checks < 20) {
        ++g_copy.st_checks;
        static wire::GearShaderVar back[256];
        const unsigned nw = look::ReadVars(copy, wire::kVehWearHash, back, 256);
        unsigned wear_ok = 0, wear_bad = 0;
        for (unsigned i = 0; i < nw; ++i) {
            const unsigned part = back[i].part;
            const bool want_on = part < wire::kMaxVehicleWearParts && ((f.wear_parts[part / 64] >> (part % 64)) & 1u);
            const float want = want_on ? static_cast<float>(f.wear) / 65535.0f : 0.0f;
            if (std::fabs(back[i].v[0] - want) <= 1e-3f) ++wear_ok;
            else ++wear_bad;
        }
        // the lamp intensities and the battery gauge's colour (three channels)
        unsigned light_ok = 0, light_bad = 0, colour_ok = 0, colour_bad = 0;
        for (int pass = 0; pass < 2; ++pass) {
            const bool colour = pass == 1;
            const auto kind = static_cast<std::uint8_t>(colour ? wire::VehicleLightKind::colour : wire::VehicleLightKind::intensity);
            const unsigned nlb = look::ReadVars(copy, colour ? wire::kVehGaugeHash : wire::kVehLightHash, back, 256);
            for (unsigned i = 0; i < f.nl; ++i) {
                if (f.light[i].kind != kind) continue;
                bool ok = false;
                for (unsigned k = 0; k < nlb && !ok; ++k) {
                    if (back[k].part != f.light[i].part) continue;
                    ok = true;
                    for (int c = 0; c < (colour ? 3 : 1); ++c)
                        ok = ok && std::fabs(back[k].v[c] - f.light[i].v[c]) <= 1e-3f * (1 + std::fabs(f.light[i].v[c]));
                }
                if (colour) ok ? ++colour_ok : ++colour_bad;
                else ok ? ++light_ok : ++light_bad;
            }
        }
        if (wear_bad || light_bad || colour_bad) ++g_state_mismatch;
        Log("[vehicle] wear / lights read back on copy '%s': wear entries %u (%u as sent, %u not), lights %u of %u as sent, gauge colours %u of %u as sent",
            kCopies[def].key, nw, wear_ok, wear_bad, light_ok, light_ok + light_bad, colour_ok, colour_ok + colour_bad);
    }
}

void ClearCargoCopy(CargoCopy& c, const char* why) {
    if (c.loading || c.live || c.sp.sp) {
        char step[160] = {};
        holo::SpawnTeardown(c.sp, why, step, sizeof(step));
        ++g_cargo_removes;
    }
    c = CargoCopy{};
}
void ClearCargo(const char* why) {
    unsigned n = 0;
    for (CargoCopy& c : g_cargo) {
        n += (c.loading || c.live || c.sp.sp) ? 1u : 0u;
        ClearCargoCopy(c, why);
    }
    if (n) Log("[vehicle] %u cargo prop(s) removed (%s)", n, why);
}
// A cargo prop (the gear's cutscene case, by path + GUID) must not be a Humanoid / player / vehicle and carry none of
// the components the copies may not have.
const char* ValidateCargoProp(Entity* e) {
    const auto a = reinterpret_cast<std::uintptr_t>(e);
    const std::uintptr_t rtti = mem::RttiOf(a);
    if (!rtti) return "entity type unknown";
    if (mem::DerivesFrom(rtti, "Humanoid") || mem::DerivesFrom(rtti, "DSPlayerEntity") || mem::DerivesFrom(rtti, "VehicleEntity") || ClassOf(a) >= 0)
        return "a Humanoid / player / vehicle entity";
    for (const char* bad : {"DSInteractionComponent", "PhysicsCollisionComponent", "PhysicsRagdollComponent", "DSPlayerControllerComponent",
                            "HumanoidController", "CapsuleComponent", "VehicleMover", "DSBounderComponent", "AIIndividualComponent", "AIBodyComponent",
                            "DSAIIndividualComponent", "BasicControlledEntityAIController", "SoundComponent"})
        if (partner::FindComponentByName(e, bad)) return bad;
    return nullptr;
}

// The partner vehicle's cargo (section 22.12): the newest cargo block at or before the frame in play (kept until the
// next one: it comes when it changes and every second), one case prop per piece placed every tick at the copy's place
// times the piece's place in the vehicle's frame, shown with the copy (hidden with it), coloured with the baggage
// config's variation the sender named (gear_look, as the cargo on the partner's back).
void DriveCargo(const WorldTransform& v, const Rx* b, bool show, double now, std::uintptr_t own_vehicle = 0) {
    if (!g_look_on || g_cargo_refused) return;
    // the newest cargo block at or before b
    const unsigned bi = static_cast<unsigned>((static_cast<std::size_t>(b - g_ring) + kRing - g_head) % kRing);
    for (unsigned k = bi + 1; k-- > 0;) {
        const Rx& x = At(k);
        if (!(x.f.flags & wire::kVehCargo)) continue;
        if (!g_copy.cargo_have || x.f.seq != g_copy.cargo_seq) {
            const bool changed = !g_copy.cargo_have || x.f.nc != g_copy.nc ||
                                 std::memcmp(x.f.cargo, g_copy.cargo, sizeof(wire::VehicleCargo) * x.f.nc) != 0;
            g_copy.cargo_have = true;
            g_copy.cargo_seq = x.f.seq;
            g_copy.nc = x.f.nc;
            for (unsigned i = 0; i < x.f.nc; ++i) g_copy.cargo[i] = x.f.cargo[i];
            static unsigned logs = 0;
            if (changed && ++logs <= 30)
                Log("[vehicle] the partner's vehicle carries %u cargo piece(s) (%u left out by the sender)", static_cast<unsigned>(x.f.nc),
                    static_cast<unsigned>(x.f.cargo_dropped));
        }
        break;
    }
    const unsigned want = g_copy.cargo_have ? g_copy.nc : 0;
    // Real shared vehicles (section 23.16, D2-4): the partner's pieces our own world's vehicle carries already (the same model
    // within 0.35 m in the vehicle's frame) get no prop; read again every 500 ms.
    static std::uint32_t own_skip = 0;
    static double own_next_ms = 0;
    static std::uintptr_t own_last = 0;
    if (!own_vehicle) {
        own_skip = 0, own_last = 0;
    } else if (own_vehicle != own_last || now >= own_next_ms) {
        own_last = own_vehicle;
        own_next_ms = now + 500;
        static wire::VehicleCargo mine[wire::kMaxVehicleCargo];
        std::uint8_t dropped = 0;
        const unsigned nm = WalkCarrier(own_vehicle, v, mine, wire::kMaxVehicleCargo, &dropped, nullptr, nullptr, nullptr, false);
        std::uint32_t skip = 0;
        bool used[wire::kMaxVehicleCargo] = {};
        for (unsigned i = 0; i < want && i < 32; ++i)
            for (unsigned k = 0; k < nm; ++k) {
                if (used[k] || mine[k].model != g_copy.cargo[i].model) continue;
                const float dx = mine[k].t[0] - g_copy.cargo[i].t[0], dy = mine[k].t[1] - g_copy.cargo[i].t[1], dz = mine[k].t[2] - g_copy.cargo[i].t[2];
                if (dx * dx + dy * dy + dz * dz > 0.35f * 0.35f) continue;
                used[k] = true;
                skip |= 1u << i;
                break;
            }
        if (skip != own_skip) {
            unsigned ns = 0;
            for (std::uint32_t x = skip; x; x &= x - 1) ++ns;
            Log("[vehicle] the partner's real vehicle: %u of its %u cargo piece(s) are on ours already (no prop), %u shown as props", ns, want, want - ns);
        }
        own_skip = skip;
    }
    unsigned started = 0;
    for (unsigned i = 0; i < wire::kMaxVehicleCargo; ++i) {
        CargoCopy& c = g_cargo[i];
        const char* key = nullptr;
        const char* path = nullptr;
        const char* guid_text = nullptr;
        if (i >= want || (i < 32 && ((own_skip >> i) & 1u)) || !gear::CargoProp(g_copy.cargo[i].model, &key, &path, &guid_text)) path = nullptr;
        const std::uint8_t var = i < want ? g_copy.cargo[i].variation : wire::kGearNone;
        if (c.path != path || (c.live && c.variation != var)) {
            if (c.loading || c.live || c.sp.sp) ClearCargoCopy(c, "the partner's cargo changed");
            c = CargoCopy{};
            c.path = path, c.key = key, c.variation = var;
        }
        if (!path) continue;
        WorldTransform at{};
        float r[3][3];
        wire::MatrixFromQuat(g_copy.cargo[i].q, r);
        Compose(v, g_copy.cargo[i].t, r, at);
        if (!c.loading && !c.live) {
            if (now < c.retry_ms || started >= 2) continue;
            std::uint8_t g[16];
            char why[200] = {};
            const WorldTransform below = SpawnPlace(at);
            if (!bodies::ParseGuid(guid_text, g)) {
                g_cargo_refused = true;
                Log("[vehicle] cargo prop '%s': bad GUID text; cargo props off for this game run", key);
                return;
            }
            ++started;
            if (realbody::BeginByPathGuid(path, g, below, 0x78000000u + i, &c.sp, why, sizeof(why))) {
                c.loading = true;
                c.begin_ms = now;
                ++g_cargo_spawns;
                if (g_cargo_spawns <= 40) Log("[vehicle] cargo prop %u '%s' loading", i, key);
            } else {
                c.retry_ms = now + 10000;
                Log("[vehicle] cargo prop %u '%s' not started: %s", i, key, why);
            }
            continue;
        }
        if (c.loading) {
            char why[200] = {};
            switch (holo::SpawnPoll(c.sp, why, sizeof(why))) {
                case holo::SpawnState::loading:
                    if (now - c.begin_ms > 20000) {
                        Log("[vehicle] cargo prop %u '%s' did not load in 20 s", i, key);
                        ClearCargoCopy(c, "load timeout");
                        c.path = path, c.key = key, c.variation = var, c.retry_ms = now + 30000;
                    }
                    continue;
                case holo::SpawnState::cancelled:
                case holo::SpawnState::failed:
                    Log("[vehicle] cargo prop %u '%s' did not load: %s", i, key, why);
                    ClearCargoCopy(c, "load failed");
                    c.path = path, c.key = key, c.variation = var, c.retry_ms = now + 10000;
                    continue;
                case holo::SpawnState::realized: {
                    c.loading = false;
                    const char* bad = c.sp.e ? ValidateCargoProp(c.sp.e) : "no entity";
                    if (bad) {
                        g_cargo_refused = true;
                        Log("[vehicle] cargo prop '%s' refused: %s; cargo props off for this game run", key, bad);
                        if (c.sp.e) partner::DescribeEntity(c.sp.e, "refused cargo prop");
                        ClearCargo("refused");
                        return;
                    }
                    partner::SetVisible(c.sp.e, false);
                    partner::WakeIfAsleep(c.sp.e);
                    if (!CollisionOff(c.sp.e, key)) {
                        g_cargo_refused = true;
                        Log("[vehicle] cargo prop '%s' refused: its collision could not be turned off; cargo props off for this game run", key);
                        ClearCargo("collision not off");
                        return;
                    }
                    c.live = true;
                    c.live_ms = now;
                    static bool described = false;
                    if (!described) {
                        described = true;
                        partner::DescribeEntity(c.sp.e, "cargo prop");
                    }
                    break;
                }
            }
        }
        if (!c.live) continue;
        if (!holo::SpawnAlive(c.sp)) {
            ClearCargoCopy(c, "destroyed by the game");
            c.path = path, c.key = key, c.variation = var, c.retry_ms = now + 10000;
            continue;
        }
        // its colour (the baggage config's variation, resolved in this game), once
        if (!c.looked && c.variation != wire::kGearNone) {
            c.looked = true;
            wire::GearLook gl;
            if (look::ReadConfigVariation(c.variation, &gl)) {
                gl.item = 0, gl.kind = wire::GearKind::cargo, gl.res = wire::kGearNone, gl.nsv = 0;
                char why[200] = {};
                const look::ApplyResult res = look::Apply(reinterpret_cast<std::uintptr_t>(c.sp.e), gl, why, sizeof(why));
                ++g_cargo_looks;
                if (g_cargo_looks <= 20)
                    Log("[vehicle] cargo prop %u '%s': colour variation %u %s%s%s", i, key, c.variation,
                        res == look::ApplyResult::done ? "applied" : res == look::ApplyResult::partial ? "partly applied" : "NOT applied", why[0] ? ": " : "", why);
            }
            c.coll_next_ms = 0;  // its collision read again right after the colour (it rebuilds the materials)
        }
        // its collision stays off (every second, section 22.13): on again more than 3 times, or not off: cargo props off
        if (now >= c.coll_next_ms) {
            c.coll_next_ms = now + 1000;
            const int k = CollisionKeepOff(c.sp.e, key);
            if (k == 0) ++c.coll_again;
            if (k < 0 || c.coll_again > 3) {
                g_cargo_refused = true;
                Log("[vehicle] cargo prop '%s' refused: its collision %s; cargo props off for this game run", key, k < 0 ? "could not be kept off" : "kept coming back");
                ClearCargo("collision not off");
                return;
            }
        }
        if ((automation::Enabled() || g_trace_until > now) && c.shown && !c.checked && now - c.live_ms > 1000) {
            WorldTransform back{};
            if (ReadXf(reinterpret_cast<std::uintptr_t>(c.sp.e), back)) {
                c.checked = true;
                const float d = static_cast<float>(Dist(back, c.last));
                if (d > g_cargo_err_max) g_cargo_err_max = d;
                Log("[vehicle] cargo prop %u '%s' placed at (%.3f, %.3f, %.3f) in the vehicle's frame: read back %.3f m from where it was put", i, key,
                    static_cast<double>(g_copy.cargo[i].t[0]), static_cast<double>(g_copy.cargo[i].t[1]), static_cast<double>(g_copy.cargo[i].t[2]),
                    static_cast<double>(d));
            }
        }
        if (!game::TrySetTransform(c.sp.e, at)) {
            ++g_cargo_faults;
            g_cargo_refused = true;
            ClearCargo("set transform fault");
            return;
        }
        c.last = at;
        if (show != c.shown) {
            partner::SetVisible(c.sp.e, show);
            c.shown = show;
            static unsigned shown_logs = 0;
            if (show && ++shown_logs <= 4) partner::DescribeEntity(c.sp.e, "cargo prop shown");
        } else if (show) {
            std::uint64_t fl = 0;
            if (partner::ReadFlags(c.sp.e, &fl) && (fl & 0x2) == 0) partner::SetVisible(c.sp.e, true);
        }
    }
}

// ---------------------------------------------------------------- the real shared vehicles (vehicle_sync=5, section 23.16)
// The partner's lamps (STM1 state block) on our real vehicle it drives or stands at: the v0.14 lamp writes, kept until a
// hidden run proves the follower's own lamps (session.ini vehicle_real_native=1 turns them off). Only the lamp intensities and
// the gauge colours: the wear is the same vehicle's in both worlds.
struct RealLights {
    std::uintptr_t e = 0;
    bool applied = false;
    std::uint8_t nl = 0;
    wire::VehicleLight light[wire::kMaxVehicleLights];
    double full_ms = 0, fail_ms = 0;
    unsigned applies = 0;
};
RealLights g_real_lights;
void DriveRealLights(std::uintptr_t e, const wire::VehicleFrame& f, double now) {
    RealLights& r = g_real_lights;
    if (r.e != e) r = RealLights{}, r.e = e;
    if (!g_look_on || !(f.flags & wire::kVehState) || now < r.fail_ms) return;
    const bool full = !r.applied || now >= r.full_ms;
    static wire::GearShaderVar vars[2 * wire::kMaxVehicleLights];
    unsigned n = 0;
    auto find = [](const wire::VehicleLight* ls, unsigned ln, const wire::VehicleLight& l) -> const wire::VehicleLight* {
        for (unsigned k = 0; k < ln; ++k)
            if (ls[k].part == l.part && ls[k].kind == l.kind) return &ls[k];
        return nullptr;
    };
    for (unsigned i = 0; i < f.nl; ++i) {
        const wire::VehicleLight& l = f.light[i];
        const wire::VehicleLight* was = r.applied ? find(r.light, r.nl, l) : nullptr;
        if (!full && was && was->v[0] == l.v[0] && was->v[1] == l.v[1] && was->v[2] == l.v[2]) continue;
        wire::GearShaderVar& sv = vars[n++];
        sv = wire::GearShaderVar{};
        const bool colour = l.kind == static_cast<std::uint8_t>(wire::VehicleLightKind::colour);
        sv.hash = colour ? wire::kVehGaugeHash : wire::kVehLightHash, sv.part = l.part, sv.count = static_cast<std::uint8_t>(colour ? 3 : 1);
        for (int c = 0; c < 3; ++c) sv.v[c] = l.v[c];
    }
    for (unsigned i = 0; r.applied && i < r.nl && n < 2 * wire::kMaxVehicleLights; ++i) {
        const wire::VehicleLight& l = r.light[i];
        if (l.kind != static_cast<std::uint8_t>(wire::VehicleLightKind::intensity) || find(f.light, f.nl, l)) continue;
        wire::GearShaderVar& sv = vars[n++];
        sv = wire::GearShaderVar{};
        sv.hash = wire::kVehLightHash, sv.part = l.part, sv.count = 1;
    }
    if (n == 0) return;
    char why[200] = {};
    const look::ApplyResult res = look::SetVehicleVars(e, vars, n, why, sizeof(why));
    if (res == look::ApplyResult::failed || res == look::ApplyResult::off) {
        r.fail_ms = now + 2000;
        static unsigned fail_logs = 0;
        if (++fail_logs <= 5)
            Log("[vehicle] the partner's lamps on our real vehicle: %s%s%s", res == look::ApplyResult::off ? "looks OFF" : "FAILED", why[0] ? ": " : "", why);
        return;
    }
    r.applied = true;
    r.nl = f.nl;
    for (unsigned i = 0; i < f.nl; ++i) r.light[i] = f.light[i];
    if (full) r.full_ms = now + 2000;
    if (++r.applies <= 3)
        Log("[vehicle] the partner's lamps on our real vehicle %p: %u light(s), %u variable(s) set (vehicle_real_native=0)", reinterpret_cast<void*>(e),
            static_cast<unsigned>(f.nl), n);
}

// Guest only (the host's world is the persisted one): the host's look on our real vehicle when ours differs (a mirror built
// from a record, a paint change on the host). Checked every 2 s, at most 5 applies per vehicle.
void DriveRealLook(std::uintptr_t e, const wire::VehicleFrame& f, double now) {
    static std::uintptr_t last_e = 0;
    static double next_ms = 0;
    static unsigned applies = 0;
    if (e != last_e) last_e = e, next_ms = 0, applies = 0;
    if (!g_look_on || !g_lrx.have || now < next_ms || applies >= 5) return;
    next_ms = now + 2000;
    if (g_lrx.l.layout >= wire::kVehicleResourceCount) return;
    const wire::VehicleResource* r = &wire::VehicleResources()[g_lrx.l.layout];
    if (wire::FindVehicleResource(f.guid) != r) return;
    if (look::SatisfiedVehicle(e, g_lrx.l)) return;
    static std::uintptr_t list[kRegCap];
    std::uintptr_t own[16];
    unsigned n_own = 0;
    const int n = ReadRegistry(list);
    for (int i = 0; i < n && n_own < 16; ++i) {
        std::uint32_t fl = 0;
        if (list[i] == e || ClassOf(list[i]) < 0 || !Rd32(list[i] + kVehFlags, fl) || !(fl & 2)) continue;
        char path[160] = {};
        std::uint8_t g[16] = {};
        if (realbody::ResourceIdentity(reinterpret_cast<Entity*>(list[i]), path, sizeof(path), g, nullptr) && wire::FindVehicleResource(g) == r)
            own[n_own++] = list[i];
    }
    char why[200] = {};
    const look::ApplyResult res = look::ApplyVehicle(e, g_lrx.l, own, n_own, why, sizeof(why));
    ++applies;
    Log("[vehicle] the host's look on our real %s %p (it differed; %u source(s)): %s%s%s", r->key, reinterpret_cast<void*>(e), n_own,
        res == look::ApplyResult::done      ? "applied"
        : res == look::ApplyResult::partial ? "partly applied"
        : res == look::ApplyResult::off     ? "looks OFF"
                                            : "FAILED",
        why[0] ? ": " : "", why);
}

// Mode 5: no copy. The partner's real vehicle in our world (the one we follow while it drives, else the shared vehicle of its
// kind nearest the frames' place) carries the cargo the partner has on it and ours lacks (D2-4: props, collision off), the
// partner's lamps (until vehicle_real_native=1), and on the guest the host's look.
void RealVehicleTick(const Rx* b, double now) {
    std::uintptr_t e = 0;
    WorldTransform vt{};
    bool have = false;
    if (vdrive::PartnerDrivenEntity(&e) && ClassOf(e) == static_cast<int>(b->f.kind) - 1 && ReadXf(e, vt)) {
        have = true;
    } else {
        vshare::Unit u;
        const double p[3] = {b->f.x, b->f.y, b->f.z};
        // the poll is 10 Hz: the unit and its actor are checked again before the look and lamp game calls use the entity
        if (vshare::FindNear(b->f.kind, p, 3.0, &u) && u.entity && vshare::UnitLive(u) && ClassOf(u.entity) == static_cast<int>(b->f.kind) - 1 &&
            ReadXf(u.entity, vt)) {
            e = u.entity;
            have = true;
        }
    }
    static std::uintptr_t logged = 1;
    if (have != (logged != 0) || (have && logged != e)) {
        logged = have ? e : 0;
        if (have)
            Log("[vehicle] the partner's vehicle is our real %s %p (%s)", b->f.kind == 1 ? "roadster" : b->f.kind == 2 ? "truck" : "trike", reinterpret_cast<void*>(e),
                (b->f.flags & wire::kVehSeated) ? "it drives it" : "parked");
        else
            Log("[vehicle] the partner's vehicle is not in our world here (not spawned, or not shared)");
    }
    if (!have) {
        ClearCargo("the partner's vehicle is not here");
        return;
    }
    DriveCargo(vt, b, true, now, e);
    if (!vdrive::NativeFx()) DriveRealLights(e, b->f, now);
    if (vshare::Role() == 2) DriveRealLook(e, b->f, now);
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

// The frame after `r` in the ring (nullptr for the newest).
const Rx* After(const Rx* r) {
    const auto idx = static_cast<unsigned>(r - g_ring);
    if (idx >= kRing) return nullptr;
    const unsigned i = (idx + kRing - g_head) % kRing;
    return i + 1 < g_count ? &At(i + 1) : nullptr;
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

// Two frames that are not one motion: further apart than a vehicle goes (5 m plus 60 m/s over their spacing on the
// sender's axis): a teleport, or another vehicle of the same kind (the frame names no vehicle instance). No sliding.
bool Jump(const Rx& a, const Rx& b) {
    const double dx = b.f.x - a.f.x, dy = b.f.y - a.f.y, dz = b.f.z - a.f.z;
    const double dt = b.key > a.key ? b.key - a.key : 0.0;
    const double lim = 5.0 + 0.06 * dt;
    return dx * dx + dy * dy + dz * dz > lim * lim;
}

void Lerp(const wire::VehicleFrame& a, const wire::VehicleFrame& b, double f, bool jump, WorldTransform& v, float (&seat)[3], float (&seat_r)[3][3],
          wire::GearPose* pose) {
    const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
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
        ClearCargo(why);
        g_copy = Copy{};
        return;
    }
    realbody::ClearGearPose(realbody::kVehiclePoseTarget);
    ClearPacks(why);
    ClearCargo(why);
    char step[160] = {};
    const int def = g_copy.def;
    holo::SpawnTeardown(g_copy.sp, why, step, sizeof(step));
    if (count) ++g_removes;
    std::snprintf(g_last_remove, sizeof(g_last_remove), "%s", why);
    Log("[vehicle] the partner's vehicle copy '%s' removed (%s): %s", def >= 0 ? kCopies[def].key : "?", why, step);
    g_copy = Copy{};
    g_ovl.hidden = false, g_ovl.clear_since = 0, g_ovl.with = 0;
    g_holding = false;
}

void Begin(int def, const WorldTransform& v, double now) {
    const WorldTransform at = SpawnPlace(v);  // out of sight (and away from the watcher) until it is placed
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
            // A solid copy: its collision off before anything else (it stands 20 m below, not shown; section 22.11).
            // Failing: refused for the game run, the hologram stands in (CopyFor).
            if (d.solid) {
                if (!CollisionOff(g_copy.sp.e, d.key)) {
                    Refuse(g_copy.def, true, now);
                    Log("[vehicle] copy '%s' refused for this game run: its collision could not be turned off", d.key);
                    ClearCopy("collision not off", false);
                    return;
                }
                g_copy.coll_next_ms = now;  // checked again on its first placement, before it is shown
            }
            g_copy.live = true;
            g_copy.live_ms = now;
            g_copy.fails = 0;
            g_refusals[g_copy.def] = 0;
            Log("[vehicle] the partner's vehicle copy '%s' live after %.0f ms (%s)", d.key, now - g_copy.begin_ms,
                d.solid ? "solid cutscene body, collision off" : "hologram");
            static bool described[kCopyCount] = {};
            if (!described[g_copy.def]) {
                described[g_copy.def] = true;
                partner::DescribeEntity(g_copy.sp.e, d.key);
            }
            return;
        }
    }
}

// The copy hidden while the watcher's own vehicle of the same kind stands where it is (within 1 m, 1 m up or down and
// 20 deg of yaw: two of the same vehicle in one place, as when both players start beside the same parked roadster), shown
// again once that vehicle has been more than 2 m or 35 deg away for 0.5 s. The partner body stays placed (seated in the
// watcher's own vehicle then). Every 250 ms, fault-free reads of the game's vehicle registry; local vehicles are never
// written. Hiding is SetVisible on our copy, as the emergency stop does.
void DriveOverlap(const WorldTransform& v, std::uint8_t kind, double now) {
    if (!g_overlap_on || !g_copy.live || !g_copy.shown) return;
    if (now < g_ovl.next_ms) return;
    g_ovl.next_ms = now + 250;
    static std::uintptr_t list[kRegCap];
    const int n = ReadRegistry(list);
    if (n < 0) {
        if (++g_ovl.unreadable <= 3) Log("[vehicle] overlap: the vehicle registry is unreadable this pass");
        return;
    }
    double best_d = 1e9, best_dz = 0, best_yaw = 0;
    std::uintptr_t best = 0;
    for (int i = 0; i < n; ++i) {
        const int cls = ClassOf(list[i]);
        if (cls < 0 || kClasses[cls].kind != kind) continue;
        std::uint32_t fl = 0;
        WorldTransform t{};
        if (!mem::Read(list[i] + kVehFlags, &fl, 4) || !(fl & 2) || !ReadXf(list[i], t)) continue;
        const double dx = t.pos.x - v.pos.x, dy = t.pos.y - v.pos.y;
        const double d = std::sqrt(dx * dx + dy * dy);
        if (d < best_d) best_d = d, best_dz = t.pos.z - v.pos.z, best = list[i], best_yaw = YawDiffDeg(t, v);
    }
    const bool is_near = best && best_d < 1.0 && std::fabs(best_dz) < 1.0 && best_yaw < 20.0;
    const bool is_clear = !best || best_d > 2.0 || std::fabs(best_dz) > 1.5 || best_yaw > 35.0;
    const char* key = kCopies[g_copy.def].key;
    if (!g_ovl.hidden) {
        if (!is_near) return;
        partner::SetVisible(g_copy.sp.e, false);
        g_ovl.hidden = true, g_ovl.with = best, g_ovl.clear_since = 0;
        ++g_ovl.hides;
        Log("[vehicle] overlap: copy '%s' hidden: our own %s %p stands in its place (%.2f m, dz %.2f m, yaw %.1f deg)", key,
            kClasses[ClassOf(best)].name, reinterpret_cast<void*>(best), best_d, best_dz, best_yaw);
        return;
    }
    if (!is_clear) {
        g_ovl.clear_since = 0;
        return;
    }
    if (g_ovl.clear_since == 0) g_ovl.clear_since = now;
    if (now - g_ovl.clear_since < 500) return;
    partner::SetVisible(g_copy.sp.e, true);
    std::uint64_t fl = 0;
    const bool vis = partner::ReadFlags(g_copy.sp.e, &fl) && (fl & 0x2) != 0;
    g_ovl.hidden = false, g_ovl.with = 0, g_ovl.clear_since = 0;
    ++g_ovl.shows;
    Log("[vehicle] overlap: copy '%s' shown again (nearest own vehicle of its kind %.2f m, yaw %.1f deg; visible flag %s)", key, best ? best_d : -1.0,
        best_yaw, vis ? "set" : "NOT set");
}

// A solid copy's collision checked every 250 ms (section 22.11): found on again (a game path re-creating it), it is
// turned off again and logged; more than 3 times, or not off after the call, or unreadable: the copy goes and is refused
// for the game run (the fallback stands in). True when the copy was removed.
bool CollisionWatch(int def, double now) {
    if (!kCopies[def].solid || now < g_copy.coll_next_ms) return false;
    g_copy.coll_next_ms = now + 250;
    const CollState s = ReadColl(reinterpret_cast<std::uintptr_t>(g_copy.sp.e));
    if (s.read && !s.on && s.insts == 0) return false;
    const char* why = nullptr;
    if (!s.read) {
        why = "its collision state is unreadable";
    } else {
        ++g_copy.coll_again, ++g_coll_again;
        Log("[vehicle] copy '%s': collision found ON again (flag %d, %d physics instances of %d parts; #%u): turning it off again", kCopies[def].key,
            s.on ? 1 : 0, s.insts, s.parts, g_copy.coll_again);
        if (g_copy.coll_again > 3) why = "its collision kept coming back";
        else if (!CollisionOff(g_copy.sp.e, kCopies[def].key)) why = "its collision could not be turned off again";
    }
    if (!why) return false;
    Refuse(def, true, now);
    Log("[vehicle] copy '%s' refused for this game run: %s", kCopies[def].key, why);
    RemoveCopy(why);
    return true;
}

// Hidden tests (action vehicle_sync 22 / 23): the copy pinned at a world place (the watcher drives into it).
struct Pin {
    bool on = false;
    WorldTransform at{};
} g_pin;

void LogMeters(double now, bool force) {
    if (!force && now < g_next_meter_ms) return;
    g_next_meter_ms = now + 5000;
    realbody::VehicleMeter vm;
    if (realbody::VehiclePoseMeter(&vm) && (automation::Enabled() || g_trace_until > now))
        Log("[vehicle] meter: while the partner's vehicle moved, %u frames (ticks) and %u pose writes on the copy (%.2f per frame), repeats %u, "
            "interpolated in its update %u (held at the newer frame %u); largest joint turn per write %.2f deg mean, %.2f sd, %.2f max",
            g_moving_ticks, vm.writes, g_moving_ticks ? static_cast<double>(vm.writes) / g_moving_ticks : 0.0, vm.repeats, vm.interp, vm.holds,
            static_cast<double>(vm.step_mean_deg), static_cast<double>(vm.step_sd_deg), static_cast<double>(vm.step_max_deg));
    g_moving_ticks = 0;
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
    // vehicle_sync=5 (the real shared vehicles, vehicle_share.cpp) when that module is on; its fallback is 1 (D2-1)
    if (vshare::Active()) g_mode = 5;
    else if (g_mode == 5) g_mode = 1;
    if (g_mode < 0 || g_mode > 5) g_mode = 1;
    if (g_mode == 1 && vshare::OffReason()[0] && config.host_save)
        Log("[vehicle] the real shared vehicles are off (%s): the solid trike / truck copies stand in, no roadster", vshare::OffReason());
    g_overlap_on = config.vehicle_overlap_hide;
    g_look_on = config.vehicle_look;
    g_packs_on = config.vehicle_packs;
    Log("[vehicle] overlap hiding %s (vehicle_overlap_hide=%d); looks %s (vehicle_look=%d); packs %s (vehicle_packs=%d)", g_overlap_on ? "on" : "off",
        g_overlap_on ? 1 : 0, g_look_on ? "on" : "off", g_look_on ? 1 : 0, g_packs_on ? "on" : "off", g_packs_on ? 1 : 0);
    Log("[vehicle] vehicle sync %s (vehicle_sync=%d)",
        g_mode == 0   ? "off"
        : g_mode == 3 ? "on: roadster, trike and truck as construction holograms (opt-in)"
        : g_mode == 4 ? "on: the roadster's copy only"
        : g_mode == 5 ? "real: the vehicles are shared game vehicles (vehicle_share); no copies, STM1 still carries the seat and ride state"
                      : "on: solid trike and truck copies with their collision off on each copy; no roadster (no holograms, D2-1)",
        g_mode);
}

bool TakeSend(Entity* local, const WorldTransform& local_wt, std::uint32_t sender_ms, std::uint32_t seq, double now, wire::VehicleFrame* out) noexcept {
    if (g_mode == 0 || !local || !out || !Check()) return false;
    const auto sam = reinterpret_cast<std::uintptr_t>(local);
    // A load makes a new Sam: a vehicle we had left is forgotten then (it may be gone with the old area); one we still
    // sit on is found again through his parent. A menu (no send tick while our Sam sleeps, the same Sam) keeps a parked
    // vehicle (section 22.13): the registry, vtable, GUID and distance checks below still run on it.
    const double gap = g_last_take_ms > 0 ? now - g_last_take_ms : 0;
    g_last_take_ms = now;
    if (g_ours.e && g_ours.sam != sam) {
        Log("[vehicle] our last vehicle %p forgotten (our Sam is a new entity: a load)", reinterpret_cast<void*>(g_ours.e));
        g_ours = Ours{};
    }
    if (g_ours.e && !g_ours.seated && gap > 1000) {
        static unsigned slept_logs = 0;
        if (++slept_logs <= 5) Log("[vehicle] our parked vehicle %p kept after %.0f ms without a send tick (our Sam slept: a menu)", reinterpret_cast<void*>(g_ours.e), gap);
    }
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
        g_ours.sam = sam;
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
        // Got off: the same vehicle as parked, while it still checks (in the game's vehicle registry, its vtable and
        // per-creation GUID) and Sam stays within kParkedReportM of it, with no time limit (section 22.13: it stands on the
        // other screen as long as on ours; v0.13 dropped it after 60 s or 30 m). Only fault-free reads on it from here: it
        // may have been destroyed or unloaded.
        if (!g_ours.e) return false;
        game::Guid g;
        const int reg = Registered(g_ours.e);
        if (reg == 0 || mem::Q(g_ours.e) != g_ours.vt || !partner::ReadGuid(reinterpret_cast<Entity*>(g_ours.e), &g) || g != g_ours.guid) {
            Log("[vehicle] our last vehicle %p is gone (%s): no longer reported", reinterpret_cast<void*>(g_ours.e),
                reg == 0 ? "not in the vehicle registry" : "its vtable / GUID changed");
            g_ours = Ours{};
            return false;
        }
        if (g_ours.seated || g_ours.left_ms == 0) {
            g_ours.left_ms = now;
            Log("[vehicle] our Sam got off vehicle %p: reported as parked while it is there and we stay within %.0f m", reinterpret_cast<void*>(g_ours.e),
                kParkedReportM);
        }
        g_ours.seated = false;
        if (DistXyz(g_ours.last_pos, local_wt.pos.x, local_wt.pos.y, local_wt.pos.z) > kParkedReportM) {
            Log("[vehicle] our last vehicle %p: no longer reported (we are beyond %.0f m, after %.0f s parked)", reinterpret_cast<void*>(g_ours.e),
                kParkedReportM, (now - g_ours.left_ms) / 1000);
            g_ours = Ours{};
            return false;
        }
        if (!g_ours.parked_logged && now - g_ours.left_ms > 60000) {
            g_ours.parked_logged = true;
            Log("[vehicle] our parked vehicle %p still reported after 60 s", reinterpret_cast<void*>(g_ours.e));
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
    // A broken vehicle Sam is off is reported 3 s more (the other screen drops its copy after 3 s broken), then forgotten.
    if (!seated && state == 5) {
        if (g_ours.broken_ms == 0) g_ours.broken_ms = now;
        if (now - g_ours.broken_ms > 3000) {
            Log("[vehicle] our last vehicle %p is broken: no longer reported", reinterpret_cast<void*>(g_ours.e));
            g_ours = Ours{};
            return false;
        }
    } else {
        g_ours.broken_ms = 0;
    }
    // Seated: the vehicle is Sam's parent this tick (alive): the game's getter, under the entity's lock. Parked: the
    // fault-free read (it may be gone).
    WorldTransform vt{};
    if (seated ? !game::TryGetTransform(reinterpret_cast<Entity*>(v), &vt) : !ReadXf(v, vt)) {
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
    // its packs: read while it stands still (their place on it is fixed; a moving vehicle and its children are not
    // read in the same instant), every 500 ms
    if (g_packs_on && now >= g_ours.packs_next_ms && (!g_ours.packs_read || g_ours.speed < 0.3f)) {
        g_ours.packs_next_ms = now + 500;
        const unsigned before = g_ours.m;
        ReadPacks(v, vt);
        if (!g_ours.packs_read || before != g_ours.m)
            Log("[vehicle] our vehicle %p shows %u pack(s)", reinterpret_cast<void*>(v), static_cast<unsigned>(g_ours.m));
        g_ours.packs_read = true;
    }
    if (g_packs_on && g_ours.m > 0) {
        f.flags |= wire::kVehPacks;
        f.m = g_ours.m;
        for (unsigned i = 0; i < g_ours.m; ++i) {
            f.pack[i] = g_ours.pack[i];
            f.pack_q[i] = g_ours.pack_q[i];
            for (int a = 0; a < 3; ++a) f.pack_t[i][a] = g_ours.pack_t[i][a];
        }
    }
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
    // Version 3 (section 22.12): its wear and lights in every frame (read every 100 ms), its cargo when it changed and
    // every second (read every 500 ms while it stands still: the pieces and the vehicle are not read in the same instant).
    if (g_look_on) {
        if (now >= g_ours.st_next_ms) {
            g_ours.st_next_ms = now + 100;
            look::VehicleState st;
            if (look::ReadVehicleState(v, &st)) {
                std::uint32_t h = 2166136261u;
                auto mix = [&](std::uint32_t x) { h = (h ^ x) * 16777619u; };
                mix(st.wear), mix(static_cast<std::uint32_t>(st.wear_parts[0])), mix(static_cast<std::uint32_t>(st.wear_parts[0] >> 32)),
                    mix(static_cast<std::uint32_t>(st.wear_parts[1])), mix(st.nl);
                for (unsigned i = 0; i < st.nl; ++i) {
                    mix(st.light[i].part | (st.light[i].kind << 8));
                    for (float x : st.light[i].v) mix(wire::FloatToHalf(x));
                }
                static unsigned logs = 0;
                if ((!g_ours.st_read || h != g_ours.st_hash) && ++logs <= 60) {
                    char lights[400] = {};
                    std::size_t used = 0;
                    for (unsigned i = 0; i < st.nl && used + 40 < sizeof(lights); ++i)
                        used += static_cast<std::size_t>(std::snprintf(lights + used, sizeof(lights) - used, " %u%s=%.2f", st.light[i].part,
                                                                       st.light[i].kind == 2 ? "c" : "", static_cast<double>(st.light[i].v[0])));
                    unsigned wear_parts = 0;
                    for (std::uint64_t w : st.wear_parts)
                        for (std::uint64_t b = w; b; b &= b - 1) ++wear_parts;
                    Log("[vehicle] our %s's state: wear %.3f on %u part(s) (spread %.3f), %u light(s):%s; %u entries, %u left out%s", g_ours.res->key,
                        st.wear / 65535.0, wear_parts, static_cast<double>(st.wear_spread), static_cast<unsigned>(st.nl), lights, st.entries, st.left_out,
                        g_ours.st_read ? " (changed)" : "");
                }
                g_ours.st = st;
                g_ours.st_hash = h;
                g_ours.st_read = true;
            }
        }
        if (g_ours.st_read) {
            f.flags |= wire::kVehState;
            f.wear = g_ours.st.wear;
            f.wear_parts[0] = g_ours.st.wear_parts[0], f.wear_parts[1] = g_ours.st.wear_parts[1];
            if (f.wear_parts[0] == 0 && f.wear_parts[1] == 0) f.wear = 0;
            f.nl = g_ours.st.nl;
            for (unsigned i = 0; i < f.nl; ++i) f.light[i] = g_ours.st.light[i];
        }
        if (now >= g_ours.cargo_next_ms && (!g_ours.cargo_read || g_ours.speed < 0.3f)) {
            g_ours.cargo_next_ms = now + 500;
            ReadCargo(v, vt);
        }
        if (g_ours.cargo_read && (g_ours.cargo_hash != g_ours.cargo_sent_hash || now >= g_ours.cargo_resend_ms)) {
            // as many pieces as fit the frame (kMaxVehicleFrameSize); the rest counted as dropped
            f.flags |= wire::kVehCargo;
            const std::size_t used = wire::VehicleFrameBytes(f) + wire::kVehicleCargoHeaderSize;
            const unsigned room = used < wire::kMaxVehicleFrameSize ? static_cast<unsigned>((wire::kMaxVehicleFrameSize - used) / wire::kVehicleCargoSize) : 0;
            f.nc = static_cast<std::uint8_t>(g_ours.nc < room ? g_ours.nc : room);
            const unsigned dropped = g_ours.cargo_dropped + (g_ours.nc - f.nc);
            f.cargo_dropped = static_cast<std::uint8_t>(dropped < 255 ? dropped : 255);
            for (unsigned i = 0; i < f.nc; ++i) f.cargo[i] = g_ours.cargo[i];
            g_ours.cargo_sent_hash = g_ours.cargo_hash;
            g_ours.cargo_resend_ms = now + 1000;
            ++g_ours.cargo_blocks;
        }
        // fail closed: a frame the encoder would refuse goes without the version 3 blocks (logged), never not at all
        static std::uint8_t probe[wire::kMaxVehicleFrameSize];
        if ((f.flags & (wire::kVehState | wire::kVehCargo)) && wire::EncodeVehicle(f, probe, sizeof(probe)) == 0) {
            static unsigned bad = 0;
            if (++bad <= 5) Log("[vehicle] our vehicle's state / cargo blocks do not encode (%u lights, %u pieces): sent without them", f.nl, f.nc);
            f.flags = static_cast<std::uint8_t>(f.flags & ~(wire::kVehState | wire::kVehCargo));
            f.nl = 0, f.wear = 0, f.wear_parts[0] = f.wear_parts[1] = 0, f.nc = 0, f.cargo_dropped = 0;
            g_ours.cargo_sent_hash = 0;
        }
    }
    ++g_ours.sent;
    g_ours.last_sent_ms = now;
    if (!seated) ++g_ours.parked_sent;
    if (now >= g_next_ours_log_ms) {
        g_next_ours_log_ms = now + 2000;
        Log("[vehicle] ours: %s %s (%u), %s, at (%.2f, %.2f, %.2f) %.1f m/s, seat (%.2f, %.2f, %.2f), %u joints (layout %08X); %u frames sent (%u with joints, %u parked)",
            g_ours.res->key, StateName(f.state), f.state, seated ? "seated" : "parked", f.x, f.y, f.z, static_cast<double>(f.speed), f.seat[0], f.seat[1], f.seat[2],
            f.n, f.layout, g_ours.sent, g_ours.joint_frames, g_ours.parked_sent);
    }
    return true;
}

// Hidden tests: whether the game camera looks at the nearest registered vehicle matching the filter (1 roadster, 2 truck,
// 3 trike, 4 the Kusanagi trike; 5 the partner's vehicle copy), horizontally within 25 deg (or the action's own tolerance).
void CheckAim(Entity* local, double now) {
    if (!g_aim_kind || !local) return;
    static double next_log = 0;
    const bool log = now >= next_log;
    if (log) next_log = now + 1000;
    WorldTransform swt{};
    double cf[2] = {};
    if (!game::TryGetTransform(local, &swt) || !nametags::CameraForward(local, cf)) {
        if (log) Log("[vehicle] camera aim: Sam's transform or the camera's direction unreadable");
        return;
    }
    static std::uintptr_t list[kRegCap];
    const int n = g_aim_kind == 5 || g_aim_kind == 6 ? 0 : ReadRegistry(list);
    double best_d = 1e9;
    WorldTransform best{};
    // 5: the partner's vehicle copy (the watcher's view of it after a drive); 6: the real shared vehicle the partner drives
    // here (or drove last): the followed vehicle is not in the registry's list while the NPC controller holds it
    if (g_aim_kind == 5) {
        Entity* c = CopyEntity();
        if (c && ReadXf(reinterpret_cast<std::uintptr_t>(c), best)) best_d = Dist(best, swt);
    }
    if (g_aim_kind == 6) {
        std::uintptr_t e = 0;
        if (vdrive::PartnerVehicleEntity(&e) && ReadXf(e, best)) best_d = Dist(best, swt);
    }
    for (int i = 0; i < n; ++i) {
        std::uint32_t kind = 0, fl = 0;
        if (ClassOf(list[i]) < 0 || !Rd32(list[i] + kVehType, kind) || !Rd32(list[i] + kVehFlags, fl) || !(fl & 2)) continue;
        char path[160] = {};
        std::uint8_t g[16] = {};
        realbody::ResourceIdentity(reinterpret_cast<Entity*>(list[i]), path, sizeof(path), g, nullptr);
        const wire::VehicleResource* r = wire::FindVehicleResource(g);
        const char* key = r ? r->key : "";
        const bool match = (g_aim_kind == 1 && std::strcmp(key, "roadster") == 0) || (g_aim_kind == 2 && kind == 2) ||
                           (g_aim_kind == 3 && std::strcmp(key, "trike") == 0) || (g_aim_kind == 4 && std::strcmp(key, "trike_kusanagi") == 0);
        WorldTransform t{};
        if (!match || !ReadXf(list[i], t)) continue;
        const double d = Dist(t, swt);
        if (d < best_d) best_d = d, best = t;
    }
    if (best_d > 400) {
        if (log) Log("[vehicle] camera aim: no vehicle of kind %d registered within 400 m (%d registered)", g_aim_kind, n);
        return;
    }
    const double dx = best.pos.x - swt.pos.x, dy = best.pos.y - swt.pos.y;
    double diff = (std::atan2(dy, dx) - std::atan2(cf[1], cf[0])) * 57.29577951308232;
    while (diff > 180) diff -= 360;
    while (diff < -180) diff += 360;
    if (std::fabs(diff) > g_aim_tol) {
        if (log) Log("[vehicle] camera aim: %.1f deg off the vehicle %.1f m away (camera forward %.2f, %.2f)", diff, best_d, cf[0], cf[1]);
        return;
    }
    Log("[vehicle] camera aimed #%u at vehicle kind %d %.1f m away (%.1f deg off)", ++g_aims, g_aim_kind, best_d, diff);
    g_aim_kind = 0;
}

void SenderTick(Entity* local, double now) noexcept {
    if (automation::Enabled()) CheckAim(local, now);
    if (g_mode == 0 || g_trace_until <= now || !local || !g_ok) return;
    const std::uintptr_t v = g_ours.e;
    if (!v || ClassOf(v) < 0) return;
    WorldTransform vt{};
    if (!ReadXf(v, vt)) return;
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
    g_lrx = LookRx{};
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
    for (PackCopy& p : g_pack)
        if (p.live && p.sp.e && holo::SpawnAlive(p.sp)) partner::SetVisible(p.sp.e, false);
    for (CargoCopy& c : g_cargo)
        if (c.live && c.sp.e && holo::SpawnAlive(c.sp)) partner::SetVisible(c.sp.e, false);
    g_mode = 0;
}

void RemoveCopy(const char* why) noexcept { ClearCopy(why, true); }

bool CopyCollisionOff(Entity* e, const char* what) noexcept { return CollisionOff(e, what); }
int CopyCollisionKeepOff(Entity* e, const char* what) noexcept { return CollisionKeepOff(e, what); }

bool SendMuted(double now) noexcept { return now < g_mute_until; }

bool TakeSendLook(double now, wire::GearLook* out) noexcept {
    if (g_mode == 0 || !g_look_on || !out || !g_ours.e || !g_ours.res || now - g_ours.last_sent_ms > 200 || now < g_look_next_ms) return false;
    g_look_next_ms = now + 500;
    static wire::GearLook l;
    l = wire::GearLook{};
    if (!look::ReadVehicle(g_ours.e, &l)) return false;
    l.item = wire::kLookVehicleItem;
    l.kind = wire::GearKind::entity;
    l.res = wire::kGearNone;
    l.layout = static_cast<std::uint16_t>(g_ours.res - wire::VehicleResources());
    const std::uint32_t h = LookKey(l) ^ static_cast<std::uint32_t>(g_ours.e >> 4);
    if (h == g_look_sent_hash && now < g_look_resend_ms) return false;  // unchanged: again every 2 s (a lost frame)
    if (h != g_look_sent_hash)
        Log("[vehicle] our %s's look: %u variation(s), %u colour index(es)%s", g_ours.res->key, l.nvar, l.nsv, g_looks_sent ? " (changed)" : "");
    g_look_sent_hash = h;
    g_look_resend_ms = now + 2000;
    ++g_looks_sent;
    *out = l;
    return true;
}

void MarkLookUnsent() noexcept { g_look_sent_hash = 0, g_look_next_ms = 0; }

void OnLook(const wire::GearLook& l) noexcept {
    if (g_mode == 0 || !g_look_on || l.item != wire::kLookVehicleItem || l.layout >= wire::kVehicleResourceCount) return;
    // Only what the sender reads for a vehicle (look::ReadVehicle): no ArtParts bits, and the colour index variable
    // 0x3A3714ED only (review 2026-11-18: any shader variable and part bits were taken before; they stay on our copy,
    // but nothing else is expected).
    bool ok = l.parts_apply == 0 && l.parts_visible == 0;
    for (unsigned i = 0; ok && i < l.nsv; ++i) ok = l.sv[i].hash == 0x3A3714EDu;
    if (!ok) {
        static unsigned bad_logs = 0;
        ++g_look_refused;
        if (++bad_logs <= 5) Log("[vehicle] the partner's vehicle look refused: ArtParts bits or a shader variable other than the colour index");
        return;
    }
    const std::uint32_t h = LookKey(l);
    ++g_look_rx;
    if (g_lrx.have && h == g_lrx.hash) return;
    g_lrx.l = l;
    g_lrx.hash = h ? h : 1;
    g_lrx.have = true;
    static unsigned logs = 0;
    if (++logs <= 20)
        Log("[vehicle] the partner's %s's look: %u variation(s), %u colour index(es)", wire::VehicleResources()[l.layout].key, l.nvar, l.nsv);
}

Entity* CopyEntity() noexcept { return g_copy.live && holo::SpawnAlive(g_copy.sp) ? g_copy.sp.e : nullptr; }
bool CopyTucked() noexcept { return g_copy.live && g_ovl.hidden; }

int VehicleClass(std::uintptr_t e) noexcept { return g_ok && e ? ClassOf(e) : -1; }
int VehicleRegistered(std::uintptr_t e) noexcept { return e ? Registered(e) : -1; }

std::uintptr_t ReportedVehicle(double now, bool* seated) noexcept {
    if (g_mode == 0 || !g_ours.e || !g_ours.res || g_ours.last_sent_ms <= 0 || now - g_ours.last_sent_ms > 250) return 0;
    if (seated) *seated = g_ours.seated;
    return g_ours.e;
}

bool ReportedVehicleLocal(std::uintptr_t e, const double (&p)[3], float (&out)[3]) noexcept {
    if (!e || e != g_ours.e) return false;
    WorldTransform vt{}, pt{};
    if (!ReadXf(e, vt)) return false;
    pt.pos.x = p[0], pt.pos.y = p[1], pt.pos.z = p[2];
    pt.rot = vt.rot;
    float r[3][3];
    LocalOf(vt, pt, out, r);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

bool PartnerSeatAt(double play_ms, float (&seat)[3], float (&seat_r)[3][3], std::uint8_t* kind) noexcept {
    if (g_mode == 0) return false;
    const Rx* a = nullptr;
    const Rx* b = nullptr;
    double f = 0;
    if (!Bracket(play_ms, &a, &b, &f) || play_ms - b->key > 500) return false;
    if (!(a->f.flags & wire::kVehSeated) || !(b->f.flags & wire::kVehSeated)) return false;
    if (b->key - a->key > 500) a = b, f = 0;
    WorldTransform v{};
    Lerp(a->f, b->f, f, Jump(*a, *b), v, seat, seat_r, nullptr);
    if (kind) *kind = b->f.kind;
    return true;
}

bool MirrorVehicleSounds() noexcept { return !(g_mode == 5 && vdrive::NativeFx()); }

bool PartnerVehicleWorld(double now, const float (&local)[3], double (&out)[3]) noexcept {
    if (g_mode == 0 || now - g_snd_ms > 150) return false;
    const WorldTransform& v = g_snd_place;
    for (int i = 0; i < 3; ++i)
        out[i] = (i == 0 ? v.pos.x : i == 1 ? v.pos.y : v.pos.z) + local[0] * v.rot.m[0][i] + local[1] * v.rot.m[1][i] + local[2] * v.rot.m[2][i];
    return true;
}

void Tick(const TickIn& in, double now) noexcept {
    LogMeters(now, false);
    // the watcher's place (fault-free reads): the copies are spawned away from it (SpawnPlace)
    g_watcher_ok = in.local_ok && in.local && ReadXf(reinterpret_cast<std::uintptr_t>(in.local), g_watcher);
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
    // The partner is silent (no pose either: a link stall, its load or its pause menu), or its frames have just come
    // back after a gap (the ring starts over a little ahead of the play time): the copy stays where it stands until the
    // frames play again or the partner body goes (10 s of silence), instead of going at 2 s under a body still there.
    const bool silent_reason = why_none && std::strcmp(why_none, "vehicle_sync off") != 0 && std::strcmp(why_none, "our player asleep or gone") != 0 &&
                               std::strcmp(why_none, "no partner body") != 0;
    if (silent_reason && g_copy.live &&
        (in.pose_age_ms > 500 || (std::strcmp(why_none, "nothing to play yet") == 0 && g_count && At(0).key - in.play_ms < 1500))) {
        if (!g_holding) {
            g_holding = true;
            ++g_holds;
            Log("[vehicle] the partner's vehicle copy '%s' held where it stands (%s; the partner's pose is %.0f ms old)", kCopies[g_copy.def].key,
                why_none, in.pose_age_ms > 1e8 ? -1.0 : in.pose_age_ms);
        }
        return;
    }
    if (g_holding && !why_none) {
        g_holding = false;
        Log("[vehicle] the partner's vehicle copy '%s' plays again (the partner's pose %.0f ms old)", g_copy.def >= 0 ? kCopies[g_copy.def].key : "?",
            in.pose_age_ms);
    }
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
    // the place the partner's vehicle sounds follow (partner_sound, slot 7): the frames' place, with or without a copy
    if (!why_none) {
        float s[3], sr[3][3];
        Lerp(a->f, b->f, f, Jump(*a, *b), g_snd_place, s, sr, nullptr);
        g_snd_ms = now;
    }
    int def = -1;
    // The real sharing stopped at run time (a fault, the co-op session's stop) or the host's game does not share real vehicles:
    // the v0.14 solid trike / truck copies stand in (vehicle_sync=1, no roadster: D2-1); back to real when it is in effect again.
    if (g_mode == 5 && !vshare::RealInEffect()) {
        g_mode = 1;
        g_real_fallback = true;
        Log("[vehicle] the real shared vehicles are not in effect (%s): the solid trike / truck copies stand in, no roadster",
            vshare::Active() ? "the host's game shares none now" : vshare::OffReason());
    } else if (g_real_fallback && g_mode == 1 && vshare::RealInEffect()) {
        g_mode = 5;
        g_real_fallback = false;
        Log("[vehicle] the real shared vehicles are in effect again: no copies");
    }
    if (!why_none && g_mode == 5 && g_override == 0) {
        if (have_any) RemoveCopy("real shared vehicles (vehicle_sync=5)");
        RealVehicleTick(b, now);
        return;
    }
    if (g_mode == 5 && why_none) ClearCargo(why_none);
    if (!why_none) {
        def = why_none ? -1 : g_override > 0 ? g_override - 1 : CopyFor(b->f, now);
        if (why_none) {
        } else if (def < 0) {
            why_none = b->f.kind >= 1 && b->f.kind <= 3 ? "no copy for this vehicle (no holograms)" : "unknown kind";
        } else if (g_override == 0 && g_mode == 4 && b->f.kind != 1) {
            why_none = "trike / truck copies off (vehicle_sync=4: the roadster only)";
        } else if (Refused(def, now)) {
            why_none = "its copy was refused";
        }
    }
    if (why_none) {
        g_holding = false;
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
    // The same copy for another model (a standard trike, then the Kusanagi within the parked report: both the trike copy,
    // section 22.13): the copy goes and comes back in its own look, so no paint, wear or lamp of the earlier vehicle stays
    // on it (DriveLook / DriveState apply nothing for the Kusanagi).
    const wire::VehicleResource* fres = wire::FindVehicleResource(b->f.guid);
    if ((g_copy.loading || g_copy.live) && g_copy.def == def && g_copy.res && fres && fres != g_copy.res && ArtCopyOf(fres) != ArtCopyOf(g_copy.res)) {
        Log("[vehicle] the partner's vehicle changed from %s to %s (another model on copy '%s'): the copy comes back in its own look", g_copy.res->key,
            fres->key, kCopies[def].key);
        RemoveCopy("the partner's vehicle changed model");
    }
    WorldTransform v{};
    float seat[3], seat_r[3][3];
    static wire::GearPose pose;
    pose = wire::GearPose{};
    const bool jump = Jump(*a, *b);
    if (jump && a != b) {
        static unsigned jump_logs = 0;
        if (++jump_logs <= 10)
            Log("[vehicle] the partner's vehicle jumped %.1f m between two frames %.0f ms apart: not slid (a teleport or another vehicle)",
                std::sqrt((b->f.x - a->f.x) * (b->f.x - a->f.x) + (b->f.y - a->f.y) * (b->f.y - a->f.y) + (b->f.z - a->f.z) * (b->f.z - a->f.z)),
                b->key - a->key);
    }
    Lerp(a->f, b->f, f, jump, v, seat, seat_r, &pose);
    if (g_off[0] != 0 || g_off[1] != 0 || g_off[2] != 0) {
        const float ident[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        WorldTransform o{};
        Compose(v, g_off, ident, o);
        v = o;
    }
    if (g_pin.on) v = g_pin.at;  // hidden tests: pinned where the watcher drives into it
    if (!g_copy.loading && !g_copy.live) {
        if (g_copy.def == def && now < g_copy.retry_ms) return;
        Begin(def, v, now);
        g_copy.res = fres;
        return;
    }
    if (!g_copy.res) g_copy.res = fres;
    // a parked vehicle stays as long as the driver's game reports it (section 22.13: no 60 s limit any more)
    if (b->f.flags & wire::kVehSeated) {
        g_copy.parked_since = 0;
    } else if (g_copy.live) {
        if (g_copy.parked_since == 0) g_copy.parked_since = now;
        if (!g_copy.parked_logged && now - g_copy.parked_since > 60000) {
            g_copy.parked_logged = true;
            Log("[vehicle] the partner's parked %s still shown after 60 s (copy '%s')", fres ? fres->key : "?", kCopies[def].key);
        }
    }
    PollCopy(now);
    if (!g_copy.live) return;
    if (!holo::SpawnAlive(g_copy.sp)) {
        RemoveCopy("destroyed by the game");
        g_copy.retry_ms = now + 10000;
        g_copy.def = def;
        return;
    }
    // While it overlaps the watcher's own vehicle of its kind the copy is also put 20 m below (where it was spawned):
    // SetVisible(false) does not stop the hologram from drawing (DSWelcomeHoloComponent / ShaderAnimComponent: its grey
    // and green patches showed through the watcher's own roadster, run 20261118-160727). DriveOverlap still compares
    // the frames' place (v), not the copy's.
    WorldTransform at = v;
    if (g_ovl.hidden) at.pos.z -= 20.0;
    if (!game::TrySetTransform(g_copy.sp.e, at)) {
        ++g_place_faults;
        Refuse(def, true, now);  // a fault (the crash reporter sees even a caught one): not again this game run
        RemoveCopy("set transform fault");
        return;
    }
    if (!g_copy.placed) {
        g_copy.placed = true;
        Log("[vehicle] copy '%s' placed at (%.2f, %.2f, %.2f)", kCopies[def].key, v.pos.x, v.pos.y, v.pos.z);
    }
    // a solid copy's collision stays off (checked every 250 ms; before it is first shown too)
    if (CollisionWatch(def, now)) return;
    // Its model updated every frame (the hologram asks for 14.99 Hz, the cutscene bodies for 59.94 Hz), so its wheels and
    // steering move every frame: smooth bit kSmoothBoost, checked every 500 ms (the mask may change at run time; back at
    // its own rate without it).
    if (now >= g_copy.freq_next_ms) {
        g_copy.freq_next_ms = now + 500;
        realbody::BoostEntity(g_copy.sp.e, "vehicle copy", &g_copy.freq_orig);
    }
    // the frame-aligned meter counts only while the partner's vehicle moves (wheels turning), and only in hidden tests
    // or while the trace runs (never in play: one acos per joint per update at 240 Hz; review 2026-11-18)
    realbody::VehicleMeterGate(b->f.speed > 1.0f && (automation::Enabled() || g_trace_until > now));
    if (b->f.speed > 1.0f) ++g_moving_ticks;
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
    } else if (!kCopies[def].welcome && !g_ovl.hidden) {
        std::uint64_t fl = 0;
        if (partner::ReadFlags(g_copy.sp.e, &fl) && (fl & 0x2) == 0) partner::SetVisible(g_copy.sp.e, true);
    }
    DriveOverlap(v, b->f.kind, now);
    DrivePacks(v, b->f, g_copy.shown && !g_ovl.hidden, now);
    DriveLook(def, b->f, now);
    DriveState(def, b->f, now);
    DriveCargo(v, b, g_copy.shown && !g_ovl.hidden, now);
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
        // Interpolated in the copy's own model update between the bracketing frames (smooth bit kSmoothDetourInterp)
        // when both carry joints on its skeleton and are one motion (no teleport between them); else this tick's pose.
        static wire::GearPose pa, pb, pc;
        bool ok = false;
        if (a != b && !jump && realbody::PoseInterpOn() && a->f.n == pose.n && b->f.n == pose.n && a->f.layout == pose.skeleton &&
            b->f.layout == pose.skeleton && b->key > a->key) {
            pa = wire::GearPose{}, pb = wire::GearPose{};
            pa.n = pb.n = pose.n;
            pa.skeleton = pb.skeleton = pose.skeleton;
            for (unsigned j = 0; j < pose.n; ++j) {
                pa.q[j] = a->f.q[j], pb.q[j] = b->f.q[j];
                for (int k = 0; k < 3; ++k) pa.t[j][k] = a->f.t[j][k], pb.t[j][k] = b->f.t[j][k];
            }
            realbody::GearPoseInterp ip;
            ip.b = &pb, ip.key_a = a->key, ip.key_b = b->key, ip.play_ms = in.play_ms;
            // the frame after b, when it is the same motion (the copy's update runs later in the frame than this tick)
            if (const Rx* c = After(b)) {
                if (c->f.n == pose.n && c->f.layout == pose.skeleton && c->key > b->key && c->key - b->key <= 500 && !Jump(*b, *c)) {
                    pc = wire::GearPose{};
                    pc.n = pose.n, pc.skeleton = pose.skeleton;
                    for (unsigned j = 0; j < pose.n; ++j) {
                        pc.q[j] = c->f.q[j];
                        for (int k = 0; k < 3; ++k) pc.t[j][k] = c->f.t[j][k];
                    }
                    ip.c = &pc, ip.key_c = c->key;
                }
            }
            ok = realbody::SetGearPose(realbody::kVehiclePoseTarget, &g_copy.sp, &pa, &ip);
        } else {
            ok = realbody::SetGearPose(realbody::kVehiclePoseTarget, &g_copy.sp, &pose);
        }
        if (ok) {
            ++g_pose_sets;
            if (!g_copy.posed) {
                g_copy.posed = true;
                Log("[vehicle] copy '%s' posed from the partner's vehicle joints (%u)%s", kCopies[def].key, pose.n,
                    realbody::PoseInterpOn() ? ", interpolated in its own update" : "");
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
            Log("[vehicle] status: partner body vs the copy's seat over the session: %.1f cm mean, %.1f cm max over %u ticks; overlap hiding %s "
                "(%u hides, %u shows%s); held while the partner was silent %u time(s)",
                g_seat_total.n ? g_seat_total.sum / g_seat_total.n * 100 : 0.0, g_seat_total.max * 100, g_seat_total.n, g_overlap_on ? "on" : "off",
                g_ovl.hides, g_ovl.shows, g_ovl.hidden ? ", hidden now" : "", g_holds);
            {
                char lc[400] = {};
                look::Counters(lc, sizeof(lc));
                unsigned live = 0;
                for (const PackCopy& p : g_pack) live += p.live ? 1u : 0u;
                Log("[vehicle] status: looks %s: %u sent, %u received (%u refused), %u applied (%u partly, %u again, %u failed); packs %s: ours %u, %u pack copies "
                    "spawned, %u removed, %u live, %u place faults, read back %.3f m max, refused battery %d defense %d; [look] %s",
                    g_look_on ? "on" : "off", g_looks_sent, g_look_rx, g_look_refused, g_look_applies, g_look_partial, g_look_again, g_look_failed, g_packs_on ? "on" : "off",
                    static_cast<unsigned>(g_ours.m), g_pack_spawns, g_pack_removes, live, g_pack_faults, static_cast<double>(g_pack_err_max),
                    g_pack_refused[0] ? 1 : 0, g_pack_refused[1] ? 1 : 0, lc);
            }
            {
                unsigned cl = 0;
                for (const CargoCopy& c : g_cargo) cl += c.live ? 1u : 0u;
                Log("[vehicle] status: wear / lights: ours %s (wear %.3f, %u lights), %u applies on the copy (%u full, %u of changes only; %u variables "
                    "set, %u failed, %u read-back mismatches); cargo: ours %u piece(s) (%u blocks sent), the partner's %u, %u props live, %u spawned, %u "
                    "removed, %u colours, %u place faults, read back %.3f m max%s",
                    g_ours.st_read ? "read" : "-", g_ours.st.wear / 65535.0, static_cast<unsigned>(g_ours.st.nl), g_state_applies, g_state_full,
                    g_state_applies - g_state_full, g_state_sets, g_state_failed, g_state_mismatch, static_cast<unsigned>(g_ours.nc), g_ours.cargo_blocks, g_copy.cargo_have ? static_cast<unsigned>(g_copy.nc) : 0u, cl,
                    g_cargo_spawns, g_cargo_removes, g_cargo_looks, g_cargo_faults, static_cast<double>(g_cargo_err_max),
                    g_cargo_refused ? "; cargo props REFUSED" : "");
            }
            Log("[vehicle] status: solid copies: collision switch %s, %u turned off, %u found on again, %u failures, %u checks of packs / cargo / gear "
                "props, %u spawned 60 m below (the watcher near); refused at some point:%s%s%s%s%s",
                g_coll_ok ? "ok" : (g_coll_checked ? "FAILED" : "not checked"), g_coll_offs, g_coll_again, g_coll_fails, g_coll_watch_checks, g_spawn_deep,
                g_refused_until[0] > 0 ? " roadster_hologram" : "", g_refused_until[1] > 0 ? " trike" : "", g_refused_until[2] > 0 ? " truck" : "",
                g_refused_until[3] > 0 ? " trike_hologram" : "", g_refused_until[4] > 0 ? " truck_hologram" : "");
            std::snprintf(msg, cap, "mode %d; ours %s; copy %s %s; seat error %.1f / %.1f cm", g_mode, g_ours.res ? g_ours.res->key : "-",
                          g_copy.def >= 0 ? kCopies[g_copy.def].key : "-", g_copy.live ? "live" : "none",
                          g_seat_total.n ? g_seat_total.sum / g_seat_total.n * 100 : 0.0, g_seat_total.max * 100);
            return true;
        }
        case 1: {
            const int m = static_cast<int>(v[1]);
            if (m < 0 || m > 4) return std::snprintf(msg, cap, "mode 0 off, 1 / 2 solid, 3 holograms, 4 roadster only"), false;
            const bool holo_change = (g_mode == 3) != (m == 3);
            g_mode = m;
            g_real_fallback = false;
            if (m == 0) RemoveCopy("vehicle_sync turned off");
            else if (holo_change) RemoveCopy("vehicle_sync switched between solid copies and holograms");
            std::snprintf(msg, cap, "vehicle sync mode %d", m);
            return true;
        }
        case 2: {
            const int o = static_cast<int>(v[1]);
            if (o < 0 || o > static_cast<int>(kCopyCount))
                return std::snprintf(msg, cap, "copy 0 auto, 1 roadster hologram, 2 trike, 3 truck, 4 trike hologram, 5 truck hologram"),
                       false;
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
        case 8:
        case 12: {
            // Hidden tests: the drivable vehicles in the game's vehicle registry (7 logs them and the parking slots). 8 also
            // places Sam v[1] m (0 = 1.8) beside the nearest one, on the side he is on, facing it, for its Use Vehicle
            // prompt; 12 places him v[1] m (0 = 12) back along the game camera's forward from a point 8 m ahead of it, 4 m to
            // the right (a watching point: the camera keeps its direction, so the point is ahead of Sam, beside him), snapped
            // to the ground. v[2] picks the vehicle: 0 any (within 40 m), 1 roadster, 2 truck, 3 trike, 4 the Kusanagi trike (within
            // 400 m).
            Entity* sam = nullptr;
            WorldTransform swt{};
            if (!game::TryGetLocalPlayer(&sam) || !sam || !game::TryGetTransform(sam, &swt)) return std::snprintf(msg, cap, "no player"), false;
            if (!Check()) return std::snprintf(msg, cap, "checks: %s", g_why), false;
            static std::uintptr_t list[kRegCap];
            const int n = ReadRegistry(list);
            if (n < 0) return std::snprintf(msg, cap, "the vehicle registry is unreadable"), false;
            std::uintptr_t best = 0;
            double best_d = 1e9;
            WorldTransform best_wt{};
            unsigned found = 0;
            for (int i = 0; i < n; ++i) {
                const int cls = ClassOf(list[i]);
                WorldTransform vt{};
                std::uint32_t state = 0, kind = 0, fl = 0;
                Rd32(list[i] + kVehRideState, state);
                Rd32(list[i] + kVehType, kind);
                Rd32(list[i] + kVehFlags, fl);
                char path[160] = {};
                std::uint8_t g[16] = {};
                const char* type = nullptr;
                realbody::ResourceIdentity(reinterpret_cast<Entity*>(list[i]), path, sizeof(path), g, &type);
                const wire::VehicleResource* r = wire::FindVehicleResource(g);
                if (!ReadXf(list[i], vt)) {
                    Log("[vehicle] registry %d: %p %s, transform unreadable", i, reinterpret_cast<void*>(list[i]), cls >= 0 ? kClasses[cls].name : "?");
                    continue;
                }
                const double d = Dist(vt, swt);
                Log("[vehicle] registry %d: %s %p '%s' (%s) kind %u, ride state %u, flags %X, at (%.2f, %.2f, %.2f), %.1f m from Sam, forward (%.2f, %.2f)", i,
                    cls >= 0 ? kClasses[cls].name : "?", reinterpret_cast<void*>(list[i]), path, r ? r->key : "not on the allow-list", kind, state, fl,
                    vt.pos.x, vt.pos.y, vt.pos.z, d, static_cast<double>(vt.rot.m[1][0]), static_cast<double>(vt.rot.m[1][1]));
                if (cls < 0) continue;
                ++found;
                const int want = static_cast<int>(v[2]);
                if (op != 7 && want != 0) {
                    const char* key = r ? r->key : "";
                    const bool match = (want == 1 && std::strcmp(key, "roadster") == 0) || (want == 2 && kind == 2) ||
                                       (want == 3 && std::strcmp(key, "trike") == 0) || (want == 4 && std::strcmp(key, "trike_kusanagi") == 0);
                    if (!match || (fl & 2) == 0) continue;
                }
                if (d < best_d) best_d = d, best = list[i], best_wt = vt;
            }
            if (op == 7) {
                // the parking manager's slots (the garage menu spawns the one in state 2), raw: research only
                const std::uintptr_t b = mem::Base();
                if (mem::CodeMatches(b + kParkSite.rva, kParkSite.sig) && mem::RipTarget(b + kParkSite.rva, 3, 7) == b + kParkGlobal) {
                    const std::uintptr_t pm = mem::Q(b + kParkGlobal);
                    for (unsigned s = 0; pm && s < 9; ++s) {
                        std::uint8_t raw[0x58] = {};
                        if (!mem::Read(pm + 0xD0 + s * 0x58, raw, sizeof(raw))) continue;
                        char hex[0x58 * 3 + 1] = {};
                        for (unsigned k = 0; k < 0x58; ++k) std::snprintf(hex + k * 3, 4, "%02X ", raw[k]);
                        Log("[vehicle] parking slot %u (state %u): %s", s, raw[0], hex);
                    }
                    if (!pm) Log("[vehicle] parking manager: none");
                } else {
                    Log("[vehicle] parking manager bytes differ: not read");
                }
                return std::snprintf(msg, cap, "%d registered, %u drivable; nearest %.1f m", n, found, best ? best_d : -1.0), true;
            }
            if (!best) return std::snprintf(msg, cap, "no such drivable vehicle registered (%d entries)", n), false;
            if (best_d > (v[2] != 0 ? 400.0 : 40.0)) return std::snprintf(msg, cap, "the nearest vehicle is %.1f m away", best_d), false;
            if (op == 12) {
                const double dist = v[1] > 0 ? v[1] : 12.0;
                double cf[2] = {};
                if (!nametags::CameraForward(sam, cf)) return std::snprintf(msg, cap, "the camera's direction is unreadable"), false;
                const double fx = cf[0], fy = cf[1];
                const double tx = best_wt.pos.x + 8.0 * best_wt.rot.m[1][0], ty = best_wt.pos.y + 8.0 * best_wt.rot.m[1][1];
                const double x = tx - dist * fx + 4.0 * fy, y = ty - dist * fy - 4.0 * fx, z = best_wt.pos.z + 2.0;
                char why[160] = {};
                const bool ok = game::TeleportLocalPlayerFacing(x, y, z, fx, fy, game::TeleportMode::snap_to_ground, why, sizeof(why));
                Log("[vehicle] watching point: Sam %.1f m back along the camera from a point 8 m ahead of the %s %p, 4 m to its right, at (%.2f, %.2f) "
                    "snapped to the ground: %s", dist,
                    kClasses[ClassOf(best)].name, reinterpret_cast<void*>(best), x, y, why);
                std::snprintf(msg, cap, "%s: %s", ok ? "placed" : "not placed", why);
                return ok;
            }
            const double side = v[1] > 0 ? v[1] : 1.8;
            const float* r = best_wt.rot.m[0];
            const double sx = swt.pos.x - best_wt.pos.x, sy = swt.pos.y - best_wt.pos.y;
            const double sgn = sx * r[0] + sy * r[1] >= 0 ? 1.0 : -1.0;
            const double x = best_wt.pos.x + sgn * side * r[0], y = best_wt.pos.y + sgn * side * r[1], z = best_wt.pos.z + (side > 3 ? 2.0 : 0.1);
            char why[160] = {};
            // facing the vehicle (its Use Vehicle prompt); further away than 3 m (a watching point) snapped to the ground
            const bool ok = game::TeleportLocalPlayerFacing(x, y, z, best_wt.pos.x - x, best_wt.pos.y - y,
                                                            side > 3 ? game::TeleportMode::snap_to_ground : game::TeleportMode::place, why, sizeof(why));
            Log("[vehicle] placing Sam %.1f m %s of the %s at (%.2f, %.2f, %.2f): %s", side, sgn > 0 ? "right" : "left", kClasses[ClassOf(best)].name, x, y,
                z, why);
            std::snprintf(msg, cap, "%s: %s", ok ? "placed" : "not placed", why);
            return ok;
        }
        case 10: {
            // Hidden tests: our sends held for v[1] ms (pose, joints, vehicle, gear ...), the link itself stays up: the
            // other side sees a silent partner, as in a link stall, a load or a pause.
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            const double ms = v[1] > 0 ? (v[1] < 60000 ? v[1] : 60000) : 0;
            g_mute_until = link::NowMsPrecise() + ms;
            Log("[vehicle] test: our sends held for %.0f ms", ms);
            return std::snprintf(msg, cap, "sends held for %.0f ms", ms), true;
        }
        case 13:
            LogMeters(link::NowMsPrecise(), true);  // the meters now (a segment boundary in a hidden test)
            return std::snprintf(msg, cap, "meters logged"), true;
        case 18:
            // Hidden tests: from now on every tick checks whether the game camera looks at the nearest registered vehicle of
            // kind v[1] (as in 8), and logs "camera aimed #n" once it does within 25 deg (a scenario turns the camera
            // with short right-stick pulses until then: `until ... camera aimed #n => pad none 200 rx=32767`).
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            g_aim_kind = static_cast<int>(v[1]);
            g_aim_tol = v[2] >= 1 && v[2] <= 25 ? v[2] : 25;   // a finer aim (the walk-in tests: Sam walks where the camera looks)
            return std::snprintf(msg, cap, "aiming the camera at vehicle kind %d (within %.0f deg)", g_aim_kind, g_aim_tol), true;
        case 16:
        case 17: {
            // Hidden tests (the solid copies' collision): 16 places Sam v[1] m (0 = 3) before the live copy (moved v[2] m
            // along world x: in echo -4 is our own vehicle, the control) along the game camera's forward, facing it, so a
            // forward walk goes through it; 17 logs where Sam is against that point (along the line and beside it).
            Entity* sam = nullptr;
            WorldTransform swt{};
            if (!game::TryGetLocalPlayer(&sam) || !sam || !game::TryGetTransform(sam, &swt)) return std::snprintf(msg, cap, "no player"), false;
            Entity* c = CopyEntity();
            WorldTransform cw{};
            if (!c || !ReadXf(reinterpret_cast<std::uintptr_t>(c), cw)) return std::snprintf(msg, cap, "no live copy"), false;
            static double line[2] = {1, 0}, shift = 0;
            if (op == 16) shift = v[2];
            cw.pos.x += shift;
            if (op == 16) {
                double cf[2] = {};
                if (!nametags::CameraForward(sam, cf)) return std::snprintf(msg, cap, "the camera's direction is unreadable"), false;
                line[0] = cf[0], line[1] = cf[1];
                const double d = v[1] > 0 ? v[1] : 3.0;
                const double x = cw.pos.x - d * cf[0], y = cw.pos.y - d * cf[1], z = cw.pos.z + 0.3;
                char why[160] = {};
                const bool ok = game::TeleportLocalPlayerFacing(x, y, z, cf[0], cf[1], game::TeleportMode::place, why, sizeof(why));
                Log("[vehicle] collision test: Sam placed %.1f m before the copy '%s' at (%.2f, %.2f, %.2f), the copy at (%.2f, %.2f, %.2f): %s", d,
                    g_copy.def >= 0 ? kCopies[g_copy.def].key : "?", x, y, z, cw.pos.x, cw.pos.y, cw.pos.z, why);
                return std::snprintf(msg, cap, "%s: %s", ok ? "placed" : "not placed", why), ok;
            }
            const double dx = swt.pos.x - cw.pos.x, dy = swt.pos.y - cw.pos.y;
            const double along = dx * line[0] + dy * line[1], beside = -dx * line[1] + dy * line[0];
            Log("[vehicle] collision test: Sam at (%.2f, %.2f, %.2f): %.2f m along the line past the copy's origin (negative: before it), %.2f m beside "
                "it, dz %.2f; the copy at (%.2f, %.2f, %.2f)",
                swt.pos.x, swt.pos.y, swt.pos.z, along, beside, swt.pos.z - cw.pos.z, cw.pos.x, cw.pos.y, cw.pos.z);
            return std::snprintf(msg, cap, "Sam %.2f m along, %.2f m beside the copy", along, beside), true;
        }
        case 11:
            g_overlap_on = v[1] != 0;
            if (!g_overlap_on && g_ovl.hidden && g_copy.live && g_copy.sp.e) {
                partner::SetVisible(g_copy.sp.e, true);
                g_ovl.hidden = false;
            }
            std::snprintf(msg, cap, "overlap hiding %s (%u hides, %u shows so far)", g_overlap_on ? "on" : "off", g_ovl.hides, g_ovl.shows);
            Log("[vehicle] %s", msg);
            return true;
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
        case 24: {
            // Hidden tests: v[1] synthetic cargo pieces (0..32) of EDSBaggage2Model v[2] (0 = 2, a medium case) added to the
            // cargo of the vehicle we report, laid out on its bed / rack (the cargo block and the receiver's props without
            // the game's cargo menu).
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            const int n = static_cast<int>(v[1]);
            const int model = v[2] > 0 ? static_cast<int>(v[2]) : 2;
            if (n < 0 || n > static_cast<int>(wire::kMaxVehicleCargo) || model >= wire::kVehicleCargoModels)
                return std::snprintf(msg, cap, "0..32 pieces, model 1..30"), false;
            g_test_cargo = static_cast<unsigned>(n);
            g_test_cargo_model = static_cast<std::uint8_t>(model);
            g_ours.cargo_read = false, g_ours.cargo_next_ms = 0;
            std::snprintf(msg, cap, "%d synthetic cargo piece(s) of model %d on our vehicle", n, model);
            Log("[vehicle] test: %s", msg);
            return true;
        }
        case 14:
        case 15: {
            // 14 <0/1>: the partner vehicle's look off / on (off: the copy goes and comes back in its own look), 15 <0/1> packs
            const bool on = v[1] != 0;
            if (op == 14) {
                g_look_on = on;
                if (!on) {
                    g_lrx = LookRx{};
                    RemoveCopy("vehicle_look turned off");
                }
                MarkLookUnsent();
            } else {
                g_packs_on = on;
                if (!on) ClearPacks("vehicle_packs turned off");
                g_ours.packs_read = false, g_ours.m = 0;
            }
            std::snprintf(msg, cap, "%s %s", op == 14 ? "looks" : "packs", on ? "on" : "off");
            Log("[vehicle] %s", msg);
            return true;
        }
        case 19: {
            // Hidden tests / research: the census (look, carrier, children) of every registered vehicle matching v[1] (0 all,
            // 1 roadster, 2 truck, 3 trike, 4 the Kusanagi trike) within 400 m of Sam, and of the live copy.
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            if (!Check()) return std::snprintf(msg, cap, "checks: %s", g_why), false;
            Entity* sam = nullptr;
            WorldTransform swt{};
            if (!game::TryGetLocalPlayer(&sam) || !sam || !game::TryGetTransform(sam, &swt)) return std::snprintf(msg, cap, "no player"), false;
            static std::uintptr_t list[kRegCap];
            const int n = ReadRegistry(list);
            if (n < 0) return std::snprintf(msg, cap, "the vehicle registry is unreadable"), false;
            unsigned done = 0;
            const int want = static_cast<int>(v[1]);
            for (int i = 0; i < n; ++i) {
                const int cls = ClassOf(list[i]);
                std::uint32_t kind = 0, fl = 0;
                WorldTransform vt{};
                if (cls < 0 || !Rd32(list[i] + kVehType, kind) || !Rd32(list[i] + kVehFlags, fl) || !(fl & 2) || !ReadXf(list[i], vt) || Dist(vt, swt) > 400)
                    continue;
                char path[160] = {};
                std::uint8_t g[16] = {};
                realbody::ResourceIdentity(reinterpret_cast<Entity*>(list[i]), path, sizeof(path), g, nullptr);
                const wire::VehicleResource* r = wire::FindVehicleResource(g);
                const char* key = r ? r->key : "";
                if (want != 0 && !((want == 1 && std::strcmp(key, "roadster") == 0) || (want == 2 && kind == 2) || (want == 3 && std::strcmp(key, "trike") == 0) ||
                                   (want == 4 && std::strcmp(key, "trike_kusanagi") == 0)))
                    continue;
                char tag[64];
                std::snprintf(tag, sizeof(tag), "registry %d (%s, %.0f m)", i, key[0] ? key : "?", Dist(vt, swt));
                Census(list[i], tag);
                ++done;
            }
            if (Entity* c = CopyEntity()) {
                look::Dump("the partner's vehicle copy", reinterpret_cast<std::uintptr_t>(c), 0);
                look::DumpResourceVariations("the partner's vehicle copy", reinterpret_cast<std::uintptr_t>(c));
            }
            return std::snprintf(msg, cap, "census of %u vehicle(s)", done), true;
        }
        case 21: {
            // 21: the live copy's collision state (and our own vehicle's, as a control), logged
            const CollState ours = ReadColl(g_ours.e);
            Entity* c = CopyEntity();
            const CollState cs = ReadColl(reinterpret_cast<std::uintptr_t>(c));
            Log("[vehicle] collision state: copy '%s' %s: flag %d, %d physics instances of %d parts%s; our own vehicle %p: flag %d, %d instances of %d parts; "
                "collision offs %u, found on again %u, failures %u",
                g_copy.def >= 0 ? kCopies[g_copy.def].key : "-", c ? "live" : "none", cs.on ? 1 : 0, cs.insts, cs.parts, cs.read ? "" : " (unread)",
                reinterpret_cast<void*>(g_ours.e), ours.on ? 1 : 0, ours.insts, ours.parts, g_coll_offs, g_coll_again, g_coll_fails);
            return std::snprintf(msg, cap, "copy '%s' collision %d, %d instances", g_copy.def >= 0 ? kCopies[g_copy.def].key : "-", cs.on ? 1 : 0, cs.insts), true;
        }
        case 22:
        case 23: {
            // Hidden tests: 22 <forward m> [right m] pins the copy at our own vehicle's place plus that offset in its frame
            // (the watcher then drives into it); 23 unpins it.
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            if (op == 23) {
                g_pin.on = false;
                Log("[vehicle] pin: the copy follows its frames again");
                return std::snprintf(msg, cap, "unpinned"), true;
            }
            WorldTransform vt{};
            if (!g_ours.e || !ReadXf(g_ours.e, vt)) return std::snprintf(msg, cap, "no vehicle of ours"), false;
            const float off[3] = {static_cast<float>(v[2]), static_cast<float>(v[1]), 0.0f};
            const float ident[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            Compose(vt, off, ident, g_pin.at);
            g_pin.on = true;
            Log("[vehicle] pin: the copy pinned %.1f m ahead, %.1f m right of our vehicle %p at (%.2f, %.2f, %.2f), forward (%.2f, %.2f)", v[1], v[2],
                reinterpret_cast<void*>(g_ours.e), g_pin.at.pos.x, g_pin.at.pos.y, g_pin.at.pos.z, static_cast<double>(g_pin.at.rot.m[1][0]),
                static_cast<double>(g_pin.at.rot.m[1][1]));
            return std::snprintf(msg, cap, "pinned at (%.2f, %.2f, %.2f)", g_pin.at.pos.x, g_pin.at.pos.y, g_pin.at.pos.z), true;
        }
        case 5:
            if (!g_ours.e) return std::snprintf(msg, cap, "no vehicle of ours"), false;
            Describe(g_ours.e, "our vehicle (asked)");
            if (Entity* c = CopyEntity()) partner::DescribeEntity(c, "the partner's vehicle copy");
            std::snprintf(msg, cap, "described");
            return true;
        default:
            std::snprintf(msg, cap, "ops: 0 log, 1 mode, 2 copy override, 3 trace ms, 4 offset right/forward cm, 5 describe, 6 offset up cm, 7 list, "
                          "8 place beside, 9 place at x y, 10 hold sends ms, 11 overlap hiding 0/1");
            return false;
    }
}

}  // namespace st::vsync

namespace st::game {
bool VehicleSyncCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept { return vsync::DebugCommand(op, v, msg, cap); }
}  // namespace st::game
