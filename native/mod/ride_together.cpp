// Riding together (design M3). See ride_together.hpp, ride_wire.hpp and docs/real-partner-body.md section 24.
#include "ride_together.hpp"

#include <windows.h>
#include <xinput.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "automation.hpp"
#include "coop_session.hpp"
#include "game_actions.hpp"
#include "link.hpp"
#include "MinHook.h"
#include "log.hpp"
#include "overlay/overlay_input.hpp"
#include "partner_source.hpp"
#include "pose_wire.hpp"
#include "real_body.hpp"
#include "ride_camera.hpp"
#include "ride_wire.hpp"
#include "save_guard.hpp"
#include "vehicle_drive.hpp"
#include "vehicle_share.hpp"
#include "vehicle_wire.hpp"

namespace st::ride {
namespace {
namespace mem = partner::mem;
using game::WorldTransform;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut)
constexpr std::uintptr_t kEntityTeleport = 0x14235A010;  // Entity teleport (entity, WorldTransform*, 0, 0): PlaceOn mode 0's call
constexpr std::uintptr_t kCapsuleOff = 0x14243A740;      // the capsule's disable: ++[c+0x58], then its switch (c, 0)
constexpr std::uintptr_t kCapsuleOn = 0x14243A9E0;       // the capsule's enable: --[c+0x58], on again when it reaches 0
constexpr std::uintptr_t kCounterInc = 0x14240D540;      // inc dword [rcx]; ret
constexpr std::uintptr_t kCounterDec = 0x142417680;      // dec dword [rcx]; ret
constexpr std::uintptr_t kCapsuleSwitch = 0x14243AA30;   // the switch: [c+0x60] the physics object, [c+0x5C] on, [c+0x48] entity
constexpr std::uintptr_t kCapsuleVt = 0x143D1BD08;       // the player capsule component's vtable (Sam +0x5A0 refers to it + 0x20)
constexpr std::uintptr_t kSamCapsuleRef = 0x5A0;
constexpr std::uintptr_t kCapCounter = 0x58, kCapOn = 0x5C, kCapEntity = 0x48;
constexpr std::uintptr_t kEParent = 0x70;
constexpr std::uintptr_t kVehVt[4] = {0, 0x143E274D8, 0x143E26790, 0x143E25DB8};  // roadster, truck, trike entities (vehicle_drive)
constexpr std::uintptr_t kVRideState = 0x4CC, kVFlags560 = 0x560, kVUnit = 0x8A8, kUEntRef = 0x68;

struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
};
// clang-format off
constexpr Site kSites[] = {
    {kEntityTeleport, "48 8B C4 48 89 58 18 48 89 70 20 55 57 41 56 48 8D A8 48 FF FF FF 48 81", "Entity teleport"},
    {kCapsuleOff,     "40 53 48 83 EC 20 48 8B D9 48 83 C1 58 E8 EE 2D FD FF 33 D2 48 8B CB 48 83 C4 20 5B E9", "capsule disable (+0x58 counter, switch off)"},
    {kCapsuleOn,      "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 83 C1 58 E8 8A CC FD FF 48 8B 43 48 48 8B 90 88 00 00 00 48 C1 EA 0E F6 C2 01 74 0F 83 7B 58 00",
                      "capsule enable (+0x58 counter, the entity's collision flag)"},
    {kCounterInc,     "FF 01 C3", "the capsule counter increment"},
    {kCounterDec,     "FF 09 C3", "the capsule counter decrement"},
    {kCapsuleSwitch,  "40 57 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58 44 0F B6 C2 48 8B F9 48 8B 51 60 48 85 D2 0F 84 ?? ?? ?? ?? 0F B6 41 5C",
                      "the capsule switch (+0x60 physics, +0x5C on)"},
};
// clang-format on

using FnTeleport = void (*)(std::uintptr_t, const WorldTransform*, std::uint8_t, std::uint8_t);
using FnVoid1 = void (*)(std::uintptr_t);

// ---------------------------------------------------------------- state
bool g_checks_ok = false;
bool g_cfg_on = true;
bool g_test_hold_tp = false;           // ride_test 26: no seat teleport while the vehicle stands (hidden tests)
int g_pose_mode = 1;                   // session.ini ride_pose (ride_test 22): 1 the seated idle (source 3), 0 v0.20's driver copy
std::atomic<bool> g_on{false};
std::atomic<bool> g_stop{false};       // the emergency stop: AfterStop restores once
char g_off[200] = "not installed";
int g_role = 0;                        // 1 host, 2 guest (vehicle_share's)
double g_now = 0;
double g_dt_ms = 16.7;                 // the tick interval (EMA)
double g_last_tick = 0;
std::uint32_t g_gen = 0;
TickIn g_in;                           // this tick's input (the handler reads it)
CrashHitFn g_crash_hit = nullptr;
bool g_notice_done = false;

// seats: the table, or a tuning override per kind (ride_test 3)
bool g_seat_override[4] = {};
float g_seat_ov[4][3] = {};
bool SeatOffset(std::uint8_t kind, float (&off)[3]) {
    if (kind >= 1 && kind <= 3 && g_seat_override[kind]) {
        std::memcpy(off, g_seat_ov[kind], sizeof(off));
        return true;
    }
    return rwire::PassengerSeat(kind, off);
}

// The passenger's side (our Sam carried)
enum class PState : std::uint8_t { idle, requesting, riding };
struct Pass {
    PState st = PState::idle;
    bool solo = false;                 // ride_test 1: the seat check, no partner
    std::uint32_t ride_id = 0;
    vwire::VKey key;
    std::uint8_t kind = 0;
    std::uintptr_t entity = 0, unit = 0;
    std::uintptr_t sam = 0, cap = 0;
    bool cap_off = false;
    std::int32_t cap_counter0 = 0;
    std::uint32_t world = 0;
    double req_ms = 0, since = 0;
    double not_driven_since = 0, gone_since = 0, asleep_since = 0;
    double stall_since = 0;  // no pose from the driver over kLinkSilentMs since (0: not); stalls ridden out
    unsigned stalls = 0;
    bool driver_ended = false;
    std::uint8_t driver_reason = 0;
    float health0 = -1;
    double next_health_ms = 0;
    bool have_last = false;
    WorldTransform last_target{};
    WorldTransform last_xf{};
    bool have_tp_xf = false;           // the vehicle's transform at our newest teleport of Sam (where he was left)
    WorldTransform tp_xf{};
    bool tp_fault = false;             // the entity teleport faulted this ride: never again for the exit
    double next_seq_ms = 0;            // the sequence answer (Taken), every 250 ms
    float last_vel[3] = {};
    double last_speed = 0;
    // meters (one line a second)
    double err_sum = 0, err_max = 0, err_veh_sum = 0, err_veh_max = 0;
    unsigned err_n = 0, teleports = 0, tp_faults = 0;
    double ride_err_sum = 0, ride_err_max = 0;  // the whole ride (the summary line)
    unsigned ride_err_n = 0, ride_err_over = 0; // ... and the ticks over 0.25 m (a follower teleport moved the vehicle)
    double log_ms = 0;
    rwire::FlipDetector flip;
    rwire::CrashDetector crash;
    rwire::CrashConfirm crash2;        // v0.21: a confirmed crash only
    unsigned cab_crashes = 0, crash_rejects = 0;
    double crash_dv = 0;
    double test_crash_dv = 0;
    bool test_taken = false;
    double exit_wait_until = 0;        // a hold to get off came while too fast: off once it slows down (until then)
    bool native = false;               // asked through the game's own "Use Vehicle" (kReqNative)
    bool resume = false;               // asked for the seat back after a save (kReqResume)
    bool off_hint_done = false;        // carried: the ImGui get-off hint (the game showed no RideOff prompt)
    double last_exit_ms = -1e18;       // the last get-off press (a native press and our own read of it count once)
    // the seated idle's calm test (realbody::SetRideSeat): the vehicle's yaw rate, low-passed
    bool have_yaw = false;
    double yaw = 0, yaw_ms = 0, yaw_rate = 0, calm_since = 0;
    double pose_log_ms = 0;
    std::uint32_t pose_latches = 0;
};
Pass g_p;
rwire::HoldDetector g_hold;            // board (ImGui fallback) / get off (ImGui fallback)
rwire::PressDetector g_press;          // get off (native prompt): a press
double g_np_last_press_ms = -1e18;     // the game's TryStart repeats while the button is held: one press per gap
unsigned g_np_seen_presses = 0;
double g_cooldown_until = 0;
double g_prompt_ms = 0;
// Boarding waits while our Sam was just hurt (a vehicle hit, a fall: he lies on the ground and gets up): no ride prompt
// beside the partner's vehicle that just knocked him down (seen in run 20261119-164745, H1b).
constexpr double kHurtBoardBlockMs = 5000.0;
float g_board_health = -1;
double g_board_health_ms = 0, g_hurt_until = 0;
unsigned g_boards = 0, g_exits = 0, g_denied = 0, g_requests = 0;

// The driver's side (the partner carried in our vehicle)
struct Drv {
    bool accepted = false;
    std::uint32_t ride_id = 0;
    vwire::VKey key;
    std::uintptr_t entity = 0;
    double accept_ms = 0, last_bit_ms = -1e18;
    bool placed_logged = false;
    double err_sum = 0, err_max = 0;
    unsigned err_n = 0;
    double log_ms = 0;
};
Drv g_d;
unsigned g_accepts = 0, g_denies = 0, g_ends_rx = 0, g_ends_tx = 0, g_bad_rx = 0;

// v0.21 (play test 3: "the passenger flew out of the car"): a save puts the carried Sam off (the save guard, 24.12: no save
// captures him at the seat); once it is written his game asks for the seat back (kReqResume, the same ride id) and the
// driver's game gives it for the ride it ended for that save. ride_test 29 0 turns it off (hidden tests: v0.20's exit).
bool g_resume_enabled = true;
struct Resume {
    bool on = false;
    std::uint32_t ride_id = 0;
    vwire::VKey key;
    double since = 0, saved_ms = 0;
};
Resume g_resume;                       // the passenger's side
struct DrvResume {
    std::uint32_t ride_id = 0;
    std::uint32_t vehicle_id = 0;
    double until = 0;
};
DrvResume g_dres;                      // the driver's side: the ride it ended for a save
unsigned g_resumes = 0;

// A pending ground placement after the exit: PlaceOn refused for its rate limit (retried for 2 s after the entity
// teleport), or not possible yet (our Sam asleep in a menu, the game writing a save: retried for up to 2 minutes). Dropped when the
// game takes Sam meanwhile or he was moved over 4 m from where he was left.
constexpr double kPlaceRetryMs = 2000.0, kPlaceWaitMs = 120000.0, kPlaceMovedM = 4.0;
struct PendingPlace {
    bool on = false;
    double until = 0, next_ms = 0;
    double pos[3] = {};
    float facing[2] = {};
    bool have_from = false;
    double from[3] = {};               // our Sam's place when it was armed
    bool at_spot = false;              // our Sam already stands at the spot (the entity teleport while he slept, for a save,
                                       // at a ground-safe height): the save guard need not wait for this PlaceOn
};
PendingPlace g_place;

// The save guard's view of the ride (save_guard.hpp): our Sam carried; his exit placement still waiting for PlaceOn; the
// vehicle and its kind (the guard checks that he is out of its box before it lets a held capture run).
void PublishSaveState() noexcept {
    const bool riding = g_p.st == PState::riding;
    saveguard::SetRideState(riding, g_place.on && !g_place.at_spot, riding ? g_p.entity : 0, riding ? g_p.kind : 0, riding ? g_p.last_speed : 0.0);
}

// hidden tests (ride_test 5): our driven vehicle rolled over about its forward axis, 4 deg a tick around a point 1 m above
// its origin, by the game's own deferred vehicle teleport (a "flip" whose frames show no velocity jump)
unsigned g_roll_left = 0;

// seat measure
std::uintptr_t g_meas_e = 0, g_meas_unit = 0;
std::uint8_t g_meas_kind = 0;

// ---------------------------------------------------------------- the game's own "Use Vehicle" prompt (native)
// Play test 2 (v0.19.0): the ImGui "Hold [X] / [F]" prompt showed but never boarded (our button reads missed the user's
// pad and keys), and the user wants the game's own "Use Vehicle", which the game crosses out on the partner's vehicle.
// The prompt is EDSUIActionLocalizedId 5 (RideOn; 6 RideOff), requested by Sam's ride action (vtable 0x143D82CF8) from
// its icon provider vt+0xF0 for action slots 3 and 6 (the two control schemes); request+0x09 is the cross-out, +0x98 the
// target vehicle's game-actor id (vehicle +0x314). A press of an enabled slot reaches the action's TryStart (vt+0x08), which
// writes that id to action+0x1E0 and returns 1; the game then gets on (SetDriver, which our follower's seat refuses).
// Two detours, both reached only through the ride vtable (the vptr is checked first):
//   provider post-hook  for the partner-driven vehicle while boarding is allowed: the cross-out lifted (+0x09 = 0, +0x06 = 1,
//                       reason 0, a real distance key) and the action's disabled latch (+0x13A bit 0x20) as before the call;
//                       carried: the RideOn the game offers on the vehicle we ride becomes an enabled RideOff;
//   TryStart post-hook  a start on the partner's vehicle (or the one we ride) is undone (action+0x1E0 / +0x10A as before)
//                       and returns 0, so the game never starts its own get-on there; the press goes to the tick as ours
//                       (REQUEST, or getting off). Any other vehicle keeps the game's own get-on.
// The press is the game's own input (keyboard F, any pad the game reads), whatever the device. docs section 24.3.
constexpr std::uintptr_t kRideVt = 0x143D82CF8;        // Sam's ride action
constexpr std::uintptr_t kRideIcon = 0x14279D870;      // its icon provider (vt+0xF0): (action, slot, request*)
constexpr std::uintptr_t kRideTryStart = 0x1427D5A80;  // its TryStart (vt+0x08): bool (action)
constexpr std::uintptr_t kRideSlotQuery = 0x1427A4950; // its slot query (vt+0xA8): jmp 0x1427A4970
constexpr std::uintptr_t kActionQuery = 0x1427A4970;   // the action context's slot query (X, action, kind, out)
constexpr std::uintptr_t kVtIcon = kRideVt + 0xF0, kVtTry = kRideVt + 0x08, kVtQuery = kRideVt + 0xA8;
constexpr std::uintptr_t kAFlags = 0x13A, kATarget = 0x1E0, kAStart = 0x10A;
constexpr std::uint32_t kNoActorId = 0x1FFFFF;
constexpr std::int32_t kIconRideOn = 5, kIconRideOff = 6;
// clang-format off
constexpr Site kNativeSites[] = {
    {kRideIcon,      "40 53 56 57 41 56 41 57 B8 B0 C9 04 00 E8", "the ride action's icon provider"},
    {0x14279E693,    "C6 46 09 01 C7 06 05 00 00 00 40 38 7E 09 74 06", "the provider's cross-out (request +0x09) and RideOn (+0x00 = 5)"},
    {0x14279E6EC,    "89 86 88 00 00 00 89 9E 98 00 00 00 C7 46 20 74 55 96 03 44 89 66 24", "the provider's target id (+0x98) and reason (+0x24)"},
    {0x14279E9A5,    "41 80 8E 3A 01 00 00 20", "the provider's disabled latch (action +0x13A bit 0x20)"},
    {kRideTryStart,  "40 53 48 81 EC 90 00 00 00 48 8B 05 A0 B5 78 02 48 33 C4 48 89 84 24 80 00 00 00", "the ride action's TryStart"},
    {0x1427D5B97,    "89 83 E0 01 00 00 0F B6 44 24 24 88 83 0A 01 00 00 B0 01 C6 83 0B 01 00 00 00", "TryStart's target write (action +0x1E0, +0x10A, +0x10B)"},
    {kRideSlotQuery, "4D 8B C8 44 8B C2 48 8B D1 48 8B 49 60 E9 0E 00 00 00", "the ride action's slot query (vt+0xA8)"},
    {kActionQuery,   "4D 63 D0 4D 69 C2 E0 03 00 00 49 39 94 08 38 53 00 00", "the action slot query (owner, generation, disabled, pressed)"},
};
// The game's own pad state (device-neutral: XInput, a PlayStation pad through scePad, Steam Input, HID): the input manager
// 0x147BE6330 has 7 player slots of 0xF0 bytes (+0xE4 1 = read, +0x108 player id, +0x30 its controller (+0x28 device), +0x40
// the 0xB0-byte pad state); both converters write X / Square to +0x13 and B / Circle to +0x11 (0xFF while held). For getting
// off a carried ride and the ImGui fallback (the native prompt reads the game's input itself).
constexpr std::uintptr_t kInputMgr = 0x147BE6330, kInSlotSize = 0xF0, kInSlotPad = 0x40, kInSlotState = 0xE4, kInSlotPlayer = 0x108;
constexpr std::uintptr_t kPadX = 0x13, kPadB = 0x11;
constexpr std::uintptr_t kInputMgrLoad = 0x1418E9E60;  // lea rcx, [kInputMgr] (byte-checked: its RIP target is kInputMgr)
constexpr Site kPadSites[] = {
    {0x1419B3090, "40 53 56 57 41 56 48 83 EC 48", "the game's pad reader"},
    {0x1419B21CC, "B8 00 40 00 00", "XInput X (0x4000) to the pad state"},
    {0x1419B21EC, "41 88 49 13", "XInput X to pad state +0x13"},
    {0x1419B21C0, "41 88 41 11", "XInput B to pad state +0x11"},
    {0x1419B8850, "41 88 4E 13", "PlayStation Square to pad state +0x13"},
    {0x1419B882E, "41 88 46 11", "PlayStation Circle to pad state +0x11"},
};
// clang-format on

using FnIcon = void (*)(std::uintptr_t, int, std::uint8_t*);
using FnTry = bool (*)(std::uintptr_t);
FnIcon o_icon = nullptr;
FnTry o_try = nullptr;
std::uintptr_t g_ride_vt = 0;              // A(kRideVt)
bool g_pad_ok = false;                     // kPadSites checked
char g_np_off[160] = "not installed";
std::atomic<bool> g_np_hooked{false};      // both detours on
std::atomic<bool> g_np_enabled{true};      // ride_test 12 (hidden tests: the ImGui fallback)
std::atomic<std::uint32_t> g_np_seen{0};   // the partner-driven vehicle's actor id here (0 none)
std::atomic<std::uint32_t> g_np_lift{0};   // ... while boarding it is allowed: its prompt is lifted
std::atomic<std::uint32_t> g_np_ride{0};   // carried: the vehicle we ride (its RideOn becomes RideOff)
std::atomic<float> g_np_dist{2.0f};        // our Sam to that vehicle (the request's distance key when the game wrote 1e6)
std::atomic<unsigned> g_np_icon_calls{0}, g_np_lifts{0}, g_np_offs{0}, g_np_refused{0};
std::atomic<unsigned> g_np_presses{0};     // TryStart starts we took (board or get off)
std::atomic<std::uint32_t> g_np_press_id{0};
// What the provider made of the partner's vehicle, per slot (3, 6): valid 31 | lifted 30 | off 29 | disabled 28 |
// reason (4 bits) 20 | icon 0..7 (the tick logs every change).
std::atomic<std::uint32_t> g_np_obs[2]{};

bool NativeOn() noexcept { return g_np_hooked.load(std::memory_order_relaxed) && g_np_enabled.load(std::memory_order_relaxed); }
// The detours act only while riding together runs: after an emergency stop (or with it off) the game's prompt and its
// get-on pass unchanged, whatever ids were left behind.
bool NativeActs() noexcept {
    return g_np_enabled.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed) && !g_stop.load(std::memory_order_relaxed);
}

void H_Icon(std::uintptr_t a, int slot, std::uint8_t* req) {
    const std::uint8_t latch = *reinterpret_cast<std::uint8_t*>(a + kAFlags) & 0x20;
    o_icon(a, slot, req);
    if ((slot != 3 && slot != 6) || !req || *reinterpret_cast<std::uint64_t*>(a) != g_ride_vt || !NativeActs()) return;
    g_np_icon_calls.fetch_add(1, std::memory_order_relaxed);
    const std::int32_t icon = *reinterpret_cast<std::int32_t*>(req);
    const std::uint32_t id = *reinterpret_cast<std::uint32_t*>(req + 0x98);
    if (icon != kIconRideOn || id == 0 || id == kNoActorId) return;
    const std::uint32_t seen = g_np_seen.load(std::memory_order_relaxed), lift = g_np_lift.load(std::memory_order_relaxed),
                        ride = g_np_ride.load(std::memory_order_relaxed);
    if (id != seen && id != ride) return;  // another vehicle: the game's own prompt as it is
    const std::uint8_t dis0 = req[0x09];
    const std::int32_t reason0 = *reinterpret_cast<std::int32_t*>(req + 0x24);
    bool lifted = false, off = false;
    if (ride && id == ride) {
        *reinterpret_cast<std::int32_t*>(req) = kIconRideOff;  // carried: getting off, enabled
        off = true;
        g_np_offs.fetch_add(1, std::memory_order_relaxed);
    } else if (lift && id == lift) {
        lifted = true;
        g_np_lifts.fetch_add(1, std::memory_order_relaxed);
    }
    if (lifted || off) {
        req[0x09] = 0;
        req[0x06] = 1;
        *reinterpret_cast<std::int32_t*>(req + 0x24) = 0;
        if (!(*reinterpret_cast<float*>(req + 0x50) < 1e5f)) *reinterpret_cast<float*>(req + 0x50) = off ? 0.1f : g_np_dist.load(std::memory_order_relaxed);
        std::uint8_t& f = *reinterpret_cast<std::uint8_t*>(a + kAFlags);
        f = static_cast<std::uint8_t>((f & ~0x20) | latch);
    }
    const std::uint32_t obs = 0x80000000u | (lifted ? 0x40000000u : 0) | (off ? 0x20000000u : 0) | (dis0 ? 0x10000000u : 0) |
                              (static_cast<std::uint32_t>(reason0 & 0xF) << 20) | static_cast<std::uint32_t>(icon & 0xFF);
    g_np_obs[slot == 3 ? 0 : 1].store(obs, std::memory_order_relaxed);
}

bool H_TryStart(std::uintptr_t a) {
    const std::uint32_t t0 = *reinterpret_cast<std::uint32_t*>(a + kATarget);
    const std::uint16_t b0 = *reinterpret_cast<std::uint16_t*>(a + kAStart);
    const bool r = o_try(a);
    if (!r || *reinterpret_cast<std::uint64_t*>(a) != g_ride_vt || !NativeActs()) return r;
    const std::uint32_t id = *reinterpret_cast<std::uint32_t*>(a + kATarget);
    if (id == 0 || id == kNoActorId) return r;
    const std::uint32_t seen = g_np_seen.load(std::memory_order_relaxed), lift = g_np_lift.load(std::memory_order_relaxed),
                        ride = g_np_ride.load(std::memory_order_relaxed);
    if (id != seen && id != ride && id != lift) return r;  // another vehicle: the game's own get-on
    // the partner's vehicle (or the one we ride): never the game's get-on (our follower holds its seat); every start is
    // passed on (the tick counts one press per press: TryStart repeats while the button is held)
    *reinterpret_cast<std::uint32_t*>(a + kATarget) = t0;
    *reinterpret_cast<std::uint16_t*>(a + kAStart) = b0;
    if (id != lift && id != ride) g_np_refused.fetch_add(1, std::memory_order_relaxed);
    g_np_press_id.store(id, std::memory_order_relaxed);
    g_np_presses.fetch_add(1, std::memory_order_release);
    return false;
}

// Bootstrap thread (riding together ON): the checks and both detours; any failure leaves the ImGui hold prompt.
void InstallNative() {
    g_ride_vt = A(kRideVt);
    unsigned bad = 0;
    const char* first = nullptr;
    for (const Site& s : kNativeSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            ++bad;
            if (!first) first = s.name;
            Log("[ride] native prompt: byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
        }
    const bool vt_ok = mem::Q(A(kVtIcon)) == A(kRideIcon) && mem::Q(A(kVtTry)) == A(kRideTryStart) && mem::Q(A(kVtQuery)) == A(kRideSlotQuery);
    if (!vt_ok) {
        ++bad;
        if (!first) first = "the ride action's vtable (provider / TryStart / query slots)";
        Log("[ride] native prompt: byte check FAILED: the ride action's vtable 0x%llX", static_cast<unsigned long long>(kRideVt));
    }
    const unsigned n = static_cast<unsigned>(sizeof(kNativeSites) / sizeof(kNativeSites[0])) + 1;
    if (bad) {
        std::snprintf(g_np_off, sizeof(g_np_off), "this game build differs (%s)", first);
        Log("[ride] native Use Vehicle prompt OFF: %s (%u of %u checks failed): the ImGui hold prompt boards instead", g_np_off, bad, n);
        return;
    }
    void* ti = reinterpret_cast<void*>(A(kRideIcon));
    void* tt = reinterpret_cast<void*>(A(kRideTryStart));
    MH_STATUS s = MH_CreateHook(ti, reinterpret_cast<void*>(&H_Icon), reinterpret_cast<void**>(&o_icon));
    if (s == MH_OK) {
        s = MH_CreateHook(tt, reinterpret_cast<void*>(&H_TryStart), reinterpret_cast<void**>(&o_try));
        if (s != MH_OK) MH_RemoveHook(ti);
    }
    // TryStart first: the prompt is never lifted without the press being ours
    if (s == MH_OK) s = MH_EnableHook(tt);
    if (s == MH_OK) {
        s = MH_EnableHook(ti);
        if (s != MH_OK) MH_DisableHook(tt);
    }
    if (s != MH_OK) {
        std::snprintf(g_np_off, sizeof(g_np_off), "the hooks failed (%s)", MH_StatusToString(s));
        Log("[ride] native Use Vehicle prompt OFF: %s: the ImGui hold prompt boards instead", g_np_off);
        return;
    }
    g_np_off[0] = 0;
    g_np_hooked.store(true);
    Log("[ride] native Use Vehicle prompt ON: %u checks; the game's own prompt on the partner's vehicle (lifted while boarding is allowed), "
        "its press asks to ride; carried, the prompt gets off",
        n);
}

// The game's pad state: that button (pad state offset) is down for the local player's slot; `device` its controller's
// device (1 PlayStation, 2 Steam Input, 3 XInput, 4 HID).
bool GamePadDown(std::uintptr_t off, int* device) noexcept {
    if (!g_pad_ok) return false;
    for (int i = 0; i < 7; ++i) {
        const std::uintptr_t slot = A(kInputMgr) + static_cast<std::uintptr_t>(i) * kInSlotSize;
        std::int32_t state = 0, player = -1;
        std::uint8_t b = 0;
        if (!mem::Rd(slot + kInSlotState, state) || state != 1 || !mem::Rd(slot + kInSlotPlayer, player) || player != 0) continue;
        if (!mem::Rd(slot + kInSlotPad + off, b) || !b) continue;
        if (device) {
            std::uintptr_t ctl = 0;
            std::int32_t dev = -1;
            if (mem::Rd(slot + 0x30, ctl) && ctl) mem::Rd(ctl + 0x28, dev);
            *device = dev;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- v0.21: the landing shield (play test 3's ejections)
// While carried, Sam's capsule is off, so his HumanoidMover never touches ground and keeps the place he boarded as its fall
// reference (mover +0x220..+0x230). Its landing check 0x14360C0B0 (every mover update, from 0x143608A6A) takes his vertical
// speed from his entity's own position change (|z - prev z| / dt: our teleport to the seat every tick counts) and, when
// that speed drops by more than a threshold since the last frame (mover +0x274), deals ImpactDamage(falling height = the
// reference's z - his z). Down a slope that height grows (play test 3: 22.9 m -> 520 damage, the "health drop" exit; 5.7 m
// at a crash exit's placement -> 50). The shield skips that check for our carried Sam (and for 1.5 s after any exit, so the
// placement's drop lands on a fresh reference), doing what the game itself does at 0x143605C52 (the reference = his place
// now) and clearing the last vertical speed. Real falls count again once he stands. Reads and plain writes only, no game
// call: safe on whatever thread runs the mover update.
constexpr std::uintptr_t kLanding = 0x14360C0B0;
constexpr double kShieldAfterMs = 1500.0;
// clang-format off
constexpr Site kLandSites[] = {
    {kLanding,    "40 55 56 48 8D AC 24 D8 FE FF FF 48 81 EC 28 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 D0 00 00 00 48 8B F1 48 8B 49 48 E8",
                  "the mover's landing check (mover +0x48 its entity)"},
    {0x14360C108, "C5 FB 10 80 D8 00 00 00 C5 FB 5C 88 18 01 00 00", "its vertical speed: entity +0xD8 - +0x118"},
    {0x14360C179, "C5 FA 5E B8 00 02 00 00", "... over the entity's step +0x200"},
    {0x14360C2BB, "C5 FB 10 86 30 02 00 00 C5 FB 10 96 28 02 00 00", "its falling height: mover +0x230 / +0x228"},
    {0x14360C55D, "C5 FA 11 BE 74 02 00 00", "its last vertical speed: mover +0x274"},
    {0x143605C52, "C5 F8 10 87 C8 00 00 00 C5 F8 11 83 20 02 00 00 C5 FB 10 8F D8 00 00 00 C5 FB 11 8B 30 02 00 00",
                  "the game's own fall-reference reset (mover +0x220..+0x230 = entity +0xC8..+0xD8)"},
    {0x143608A6A, "E8 41 36 00 00", "the mover update's call of the landing check"},
    // the game's own reset of the falling state (0x14360944F): +0x140 vertical speed 0, +0x214 / +0x218 (time in the air, no
    // contact) 0, +0xE4 0, then the reference at the place (+0x220..+0x230)
    {0x14360944F, "48 8B 03 48 8B CB 44 89 AB 40 01 00 00 4C 89 AB 14 02 00 00 44 89 AB E4 00 00 00 C5 F8 10 06",
                  "the game's falling-state reset (mover +0x140, +0x214, +0xE4)"},
};
// clang-format on
std::atomic<std::uintptr_t> g_diag_sam{0};         // the local Sam (diagnostics: does his mover run its landing check)
std::atomic<std::uint32_t> g_land_calls_sam{0};
using FnLanding = void (*)(std::uintptr_t);
FnLanding o_landing = nullptr;
std::atomic<bool> g_shield_hooked{false};
char g_shield_off[160] = "not installed";
std::atomic<std::uintptr_t> g_shield_sam{0};       // our Sam while carried, and for kShieldAfterMs after an exit
std::atomic<std::uint64_t> g_shield_until{0};      // GetTickCount64 end; 0 = no end (carried)
std::atomic<bool> g_shield_enabled{true};          // ride_test 16 0/1 (hidden tests: v0.20's behaviour)
std::atomic<std::uint32_t> g_shield_skips{0};
std::atomic<std::uint32_t> g_shield_drop_bits{0}, g_shield_h_bits{0};  // maxima since the last log (float bits)
void MaxBits(std::atomic<std::uint32_t>& a, float v) noexcept {
    if (!(v > 0) || !std::isfinite(v)) return;
    std::uint32_t nb;
    std::memcpy(&nb, &v, 4);
    std::uint32_t cur = a.load(std::memory_order_relaxed);
    for (;;) {
        float c;
        std::memcpy(&c, &cur, 4);
        if (c >= v || a.compare_exchange_weak(cur, nb, std::memory_order_relaxed)) return;
    }
}
float TakeBits(std::atomic<std::uint32_t>& a) noexcept {
    const std::uint32_t b = a.exchange(0, std::memory_order_relaxed);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
void H_Landing(std::uintptr_t mover) {
    const std::uintptr_t sam = g_shield_sam.load(std::memory_order_acquire);
    {
        const std::uintptr_t ds = g_diag_sam.load(std::memory_order_relaxed);
        if (ds && *reinterpret_cast<const volatile std::uintptr_t*>(mover + 0x48) == ds) g_land_calls_sam.fetch_add(1, std::memory_order_relaxed);
    }
    // the game's own first read (mov rcx, [rcx+0x48]): the mover is the game's live object
    if (sam && g_shield_enabled.load(std::memory_order_relaxed) && *reinterpret_cast<const volatile std::uintptr_t*>(mover + 0x48) == sam) {
        const std::uint64_t until = g_shield_until.load(std::memory_order_relaxed);
        if (until == 0 || GetTickCount64() < until) {
            const double z = *reinterpret_cast<const volatile double*>(sam + 0xD8);
            const double pz = *reinterpret_cast<const volatile double*>(sam + 0x118);
            const float dt = *reinterpret_cast<const volatile float*>(sam + 0x200);
            const float last = *reinterpret_cast<const volatile float*>(mover + 0x274);
            if (dt > 1e-4f) MaxBits(g_shield_drop_bits, last - static_cast<float>(std::fabs(z - pz)) / dt);
            MaxBits(g_shield_h_bits, static_cast<float>(*reinterpret_cast<const volatile double*>(mover + 0x230) - z));
            // the game's own reset (0x143605C52): the fall reference is where he is now; the last vertical speed 0
            for (int i = 0; i < 3; ++i)
                *reinterpret_cast<volatile double*>(mover + 0x220 + 8 * i) = *reinterpret_cast<const volatile double*>(sam + 0xC8 + 8 * i);
            *reinterpret_cast<volatile float*>(mover + 0x274) = 0.0f;
            g_shield_skips.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    o_landing(mover);
}
bool ShieldActiveFor(std::uintptr_t entity) noexcept {
    if (!entity || entity != g_shield_sam.load(std::memory_order_acquire) || !g_shield_enabled.load(std::memory_order_relaxed)) return false;
    const std::uint64_t until = g_shield_until.load(std::memory_order_relaxed);
    return until == 0 || GetTickCount64() < until;
}
// ---------------------------------------------------------------- v0.21: the health guard (the root of play test 3's ejections)
// Hidden run 20261120-170125 (a write watchpoint on the carried Sam's health): 57 s down play test 3's road, where the
// elevated road ends in a small drop by the big hologram, the game set his health from 1000 to 0 in one write, through
// its health setter 0x1427400F0 (from 0x1427023C0, the damage path of the message handler 0x14271C150, not the MsgDamage
// handler vhit hooks): with his capsule off his own movement state never lands, and the drop counts as a fatal fall
// (the user's play test: 520 damage at the same place, at 1.7 m/s). The setter itself also asks for the PlayerDied game
// state (bit 42, 0x142DCE3D0) at 0. While the shield covers our Sam (carried, and 1.5 s after the ride), the setter keeps
// his health when it would go down: the game's own "no decrease" path (its entity flag +0x5C8 bit 23 returns the same
// way). Logged with the amount and the caller. (The object's +0x98 is not the Sam entity; it is matched by its address.)
constexpr std::uintptr_t kSetHealth = 0x1427400F0;
constexpr Site kHealthSites[] = {
    {kSetHealth,  "40 53 48 83 EC 30 48 8B 81 98 00 00 00 48 8B D9 8B 88 C8 05 00 00 0F BA E1 19", "the health setter (obj +0x98 owner, +0xC0 health)"},
    {0x14274019B, "C5 FA 11 83 C0 00 00 00", "its health write (+0xC0)"},
    {0x1427401EF, "48 BA 00 00 00 00 00 04 00 00 E8", "its PlayerDied request (game state bit 42)"},
    {0x142702432, "E8 B9 DC 03 00", "the damage path's call of the setter"},
};
using FnSetHealth = void (*)(std::uintptr_t, float, int, int);
FnSetHealth o_sethealth = nullptr;
std::atomic<bool> g_health_hooked{false};
std::atomic<std::uintptr_t> g_shield_hobj{0};      // our Sam's health object (Sam +0x54C0 -> +0x2E0), set with the shield
std::atomic<std::uint32_t> g_health_blocked{0};
// our own hit (AllowOwnHit): until when, and how much health it may still take (float bits)
std::atomic<std::uint64_t> g_own_hit_until{0};
std::atomic<std::uint32_t> g_own_hit_left{0};
std::atomic<std::uint32_t> g_health_own{0};
unsigned g_health_own_logged = 0;  // game thread
struct HealthBlock {
    float from = 0, to = 0;
    std::uint64_t caller = 0;
};
HealthBlock g_hb[8];
std::atomic<unsigned> g_hb_n{0}, g_hb_logged{0};
void H_SetHealth(std::uintptr_t obj, float value, int mode, int flag) {
    if (obj && obj == g_shield_hobj.load(std::memory_order_acquire)) {
        const float cur = *reinterpret_cast<const volatile float*>(obj + 0xC0);
        // !(value >= cur): also a NaN or an infinite "kill" (the clamps in the setter make those 0)
        if (ShieldActiveFor(g_shield_sam.load(std::memory_order_relaxed)) && !(value >= cur)) {
            // our own vehicle hit (D3-3) that partner_tackle's damage hook let through: its amount, once, never a kill
            if (std::isfinite(value) && value > 0.0f && GetTickCount64() < g_own_hit_until.load(std::memory_order_acquire)) {
                const std::uint32_t lb = g_own_hit_left.load(std::memory_order_relaxed);
                float left;
                std::memcpy(&left, &lb, 4);
                const float take = cur - value;
                if (take <= left) {
                    left -= take;
                    std::uint32_t nb;
                    std::memcpy(&nb, &left, 4);
                    g_own_hit_left.store(nb, std::memory_order_relaxed);
                    g_health_own.fetch_add(1, std::memory_order_relaxed);
                    o_sethealth(obj, value, mode, flag);
                    return;
                }
            }
            g_health_blocked.fetch_add(1, std::memory_order_relaxed);
            const unsigned i = g_hb_n.fetch_add(1, std::memory_order_relaxed);
            if (i < 8) g_hb[i].from = cur, g_hb[i].to = value, g_hb[i].caller = reinterpret_cast<std::uint64_t>(_ReturnAddress());
            return;
        }
    }
    o_sethealth(obj, value, mode, flag);
}
void AllowOwnHitImpl(float amount) noexcept {
    if (!(amount > 0.0f) || !std::isfinite(amount)) return;
    const float budget = 2.0f * amount + 1.0f;
    std::uint32_t b;
    std::memcpy(&b, &budget, 4);
    g_own_hit_left.store(b, std::memory_order_relaxed);
    g_own_hit_until.store(GetTickCount64() + 500, std::memory_order_release);
}
void InstallHealthGuard() {
    for (const Site& s : kHealthSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            Log("[ride] byte check FAILED: %s at 0x%llX: the health guard is OFF (a carried Sam can be hurt or killed by a drop)", s.name,
                static_cast<unsigned long long>(s.va));
            return;
        }
    if (mem::RipTarget(A(0x142702432), 1, 5) != A(kSetHealth)) {
        Log("[ride] byte check FAILED: the damage path's call does not reach 0x1427400F0: the health guard is OFF");
        return;
    }
    void* t = reinterpret_cast<void*>(A(kSetHealth));
    MH_STATUS s = MH_CreateHook(t, reinterpret_cast<void*>(&H_SetHealth), reinterpret_cast<void**>(&o_sethealth));
    if (s == MH_OK) s = MH_EnableHook(t);
    if (s != MH_OK) {
        Log("[ride] the health guard is OFF: the hook failed (%s)", MH_StatusToString(s));
        return;
    }
    g_health_hooked.store(true);
    Log("[ride] health guard ON: %u byte checks; a carried Sam (and 1.5 s after the ride) loses no health and is never killed",
        static_cast<unsigned>(sizeof(kHealthSites) / sizeof(kHealthSites[0])) + 1);
}
void HealthGuardTick() {
    const unsigned n = (std::min)(8u, g_hb_n.load());
    for (unsigned i = g_hb_logged.load(); i < n; ++i)
        Log("[ride] health guard: our Sam's health kept at %.1f (the game set %.1f, called from ds.exe+0x%llX)", g_hb[i].from, g_hb[i].to,
            static_cast<unsigned long long>(g_hb[i].caller - mem::Base()));
    g_hb_logged.store(n);
    const unsigned own = g_health_own.load(std::memory_order_relaxed);
    if (own != g_health_own_logged) {
        const std::uintptr_t hobj = g_shield_hobj.load(std::memory_order_relaxed);
        float h = -1;
        if (hobj) mem::Rd(hobj + 0xC0, h);
        Log("[ride] health guard: our own vehicle hit (D3-3) let through (%u write(s) so far): our Sam's health now %.1f", own, h);
        g_health_own_logged = own;
    }
}
// ---------------------------------------------------------------- v0.22: the boots guard (the passenger's boots wore as the vehicle moved)
// The user's v0.21 play test: riding as the passenger, the footwear lost durability as the vehicle moved. DS applies all
// boots wear in one per-frame tick, 0x142753FF0(status, dt) (status = Sam +0x54E0, the boots at status +0x320; its only
// caller is the status update at 0x142732761): walking wear (dt * DecSpeedIn{Walk,Run,Dash} while the on-foot state's
// flag is set) plus the event wear queued in status +0x358 (+0x35C / +0x360 the foot-injury values) by 0x14271DA10(status,
// type, scale): 0 jump, 1 tumble, 2 landed, 3 sliding (every 0.1 s, scale = min(speed / 5 m/s, 1)), 4 slipping. The speed
// those read is the game's own, from Sam's position change (0x14282A870), so the carry's teleport to the seat every tick
// reads as Sam moving at the vehicle's speed. A vanilla driver is not in the on-foot state: no wear. While the shield
// covers our Sam (carried, and 1.5 s after any exit) the guard skips the tick and empties the queue (the lives stay as they
// are, no foot-injury counters, no "serious injury" from the teleport's speed, no BootsInfo notice) and drops the events;
// walking wears the boots again as soon as the window ends. Docs: docs/real-partner-body.md 24.17.
constexpr std::uintptr_t kShoesTick = 0x142753FF0;
constexpr std::uintptr_t kShoesEvent = 0x14271DA10;
// clang-format off
constexpr Site kBootsSites[] = {
    {kShoesTick,  "40 55 53 56 57 41 54 41 55 41 57 48 8D 6C 24 D9 48 81 EC F0 00 00 00", "the boots tick (status, dt)"},
    {0x14275401E, "48 8B 81 B0 00 00 00 48 8D B1 20 03 00 00", "its boots at status +0x320"},
    {0x142754067, "48 8B 8F 88 00 00 00 0F B7 D8 F6 81 BC 54 00 00 02", "its owner Sam at status +0x88"},
    {0x1427540FA, "F6 81 CC 05 00 00 40", "its on-foot wear flag (+0x5CC bit 0x40)"},
    {0x1427541F2, "C4 C1 2A 59 87 4C 06 00 00", "its walking wear (dt * DecSpeedInWalk)"},
    {0x142754D08, "48 C7 46 38 00 00 00 00 44 89 66 40", "its queue cleared (status +0x358..+0x363)"},
    {0x142732759, "C4 C1 78 28 C9 48 8B CB E8 8A 18 02 00", "the status update's call of the tick"},
    {kShoesEvent, "48 83 EC 58 4C 8B 81 B8 00 00 00 C5 F8 29 74 24 40", "the wear event adder (status, type, scale)"},
    {0x14271DB85, "C5 FA 58 89 58 03 00 00 C4 C1 52 59 C0 C5 FA 11 89 58 03 00 00", "its add to the queue (status +0x358)"},
    {0x142851428, "E8 E3 C5 EC FF", "the on-foot update's sliding wear call"},
    {0x1426E74B7, "48 8B 88 E0 54 00 00", "SetShoesLife: status = Sam +0x54E0"},
};
// clang-format on
using FnShoesTick = void (*)(std::uintptr_t, float);
using FnShoesEvent = void (*)(std::uintptr_t, std::uint8_t, float);
FnShoesTick o_shoes_tick = nullptr;
FnShoesEvent o_shoes_event = nullptr;
std::atomic<bool> g_boots_hooked{false}, g_boots_ev_hooked{false};
std::atomic<bool> g_boots_enabled{true};  // ride_test 35 0/1 (hidden tests: v0.21's behaviour)
std::atomic<std::uint32_t> g_boots_skips{0};
std::atomic<std::uint32_t> g_boots_ev[5] = {}, g_boots_ev_blocked[5] = {};  // our Sam's wear events by type (all / dropped)
std::atomic<std::uint64_t> g_boots_ev_from[5] = {};                            // the newest caller per type (diagnostics)
bool BootsGuarded(std::uintptr_t owner) noexcept {
    if (!owner || owner != g_shield_sam.load(std::memory_order_acquire) || !g_boots_enabled.load(std::memory_order_relaxed)) return false;
    const std::uint64_t until = g_shield_until.load(std::memory_order_relaxed);
    return until == 0 || GetTickCount64() < until;
}
void H_ShoesTick(std::uintptr_t status, float dt) {
    // the game's own read (0x142754067): the status' owner Sam
    if (BootsGuarded(*reinterpret_cast<const volatile std::uintptr_t*>(status + 0x88))) {
        *reinterpret_cast<volatile std::uint64_t*>(status + 0x358) = 0;  // what the tick itself clears at its end (0x142754D08)
        *reinterpret_cast<volatile std::uint32_t*>(status + 0x360) = 0;
        g_boots_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    o_shoes_tick(status, dt);
}
void H_ShoesEvent(std::uintptr_t status, std::uint8_t type, float scale) {
    const std::uintptr_t owner = *reinterpret_cast<const volatile std::uintptr_t*>(status + 0x88);
    const unsigned t = type < 5 ? type : 4;
    if (owner && owner == g_diag_sam.load(std::memory_order_relaxed)) {
        g_boots_ev[t].fetch_add(1, std::memory_order_relaxed);
        g_boots_ev_from[t].store(reinterpret_cast<std::uint64_t>(_ReturnAddress()), std::memory_order_relaxed);
    }
    if (g_boots_hooked.load(std::memory_order_relaxed) && BootsGuarded(owner)) {
        g_boots_ev_blocked[t].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    o_shoes_event(status, type, scale);
}
void InstallBootsGuard() {
    for (const Site& s : kBootsSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            Log("[ride] byte check FAILED: %s at 0x%llX: the boots guard is OFF (a passenger's boots can wear as the vehicle moves)", s.name,
                static_cast<unsigned long long>(s.va));
            return;
        }
    if (mem::RipTarget(A(0x142732761), 1, 5) != A(kShoesTick) || mem::RipTarget(A(0x142851428), 1, 5) != A(kShoesEvent)) {
        Log("[ride] byte check FAILED: the calls at 0x142732761 / 0x142851428 do not reach the boots tick / the wear event adder: the boots guard is OFF");
        return;
    }
    void* t = reinterpret_cast<void*>(A(kShoesTick));
    MH_STATUS s = MH_CreateHook(t, reinterpret_cast<void*>(&H_ShoesTick), reinterpret_cast<void**>(&o_shoes_tick));
    if (s == MH_OK) s = MH_EnableHook(t);
    if (s != MH_OK) {
        Log("[ride] the boots guard is OFF: the hook failed (%s)", MH_StatusToString(s));
        return;
    }
    g_boots_hooked.store(true);
    // the events: counted (the meter names the source) and dropped while guarded; the tick's hook alone keeps the lives
    void* e = reinterpret_cast<void*>(A(kShoesEvent));
    MH_STATUS se = MH_CreateHook(e, reinterpret_cast<void*>(&H_ShoesEvent), reinterpret_cast<void**>(&o_shoes_event));
    if (se == MH_OK) se = MH_EnableHook(e);
    g_boots_ev_hooked.store(se == MH_OK);
    Log("[ride] boots guard ON: %u byte checks; a carried Sam (and 1.5 s after the ride) wears no boots%s%s",
        static_cast<unsigned>(sizeof(kBootsSites) / sizeof(kBootsSites[0])) + 2, se == MH_OK ? "" : " (the event counter is off: ",
        se == MH_OK ? "" : MH_StatusToString(se));
}
// The boots as the game keeps them (reads only): the meter, the ride's before / after.
struct BootsNow {
    bool ok = false;
    std::uint8_t kind = 0;
    std::uint32_t state = 0;
    float main = 0, layer = 0, bare = 0, pending = 0, item = -1, speed = 0, st140 = 0, st144 = 0;
    int flag = -1, level = -1;
    float Life() const noexcept { return main + layer; }
};
bool ReadBootsOf(std::uintptr_t sam, BootsNow& b) noexcept {
    b = BootsNow{};
    const std::uintptr_t st = sam ? mem::Q(sam + 0x54E0) : 0;
    if (!st || mem::Q(st + 0x88) != sam) return false;
    const std::uintptr_t sh = st + 0x320;
    if (!mem::Rd(sh + 0x08, b.kind) || !mem::Rd(sh + 0x0C, b.main) || !mem::Rd(sh + 0x1C, b.layer) || !mem::Rd(sh + 0x24, b.bare) ||
        !mem::Rd(sh + 0x34, b.state) || !mem::Rd(sh + 0x38, b.pending))
        return false;
    if (const std::uintptr_t item = mem::Q(sh + 0x48)) mem::Rd(item + 0x34, b.item);
    const std::uintptr_t bo = mem::Q(st + 0xB0);
    const std::uintptr_t y = bo ? mem::Q(bo + 0x428) : 0;
    std::uint32_t f = 0;
    std::int32_t lv = 0;
    if (y && mem::Rd(y + 0x5CC, f)) b.flag = (f & 0x40) ? 1 : 0;
    if (y && mem::Rd(y + 0x354, lv)) b.level = lv;
    if (const std::uintptr_t p = mem::Q(st + 0x98)) mem::Rd(p + 0xA0C, b.speed);
    mem::Rd(st + 0x140, b.st140), mem::Rd(st + 0x144, b.st144);  // the stamina tick's (0x142750490) first fields: logged, not interpreted
    b.ok = true;
    return true;
}
void EventCounts(char* out, std::size_t cap, std::atomic<std::uint32_t> (&a)[5]) noexcept {
    std::snprintf(out, cap, "%u/%u/%u/%u/%u", a[0].load(), a[1].load(), a[2].load(), a[3].load(), a[4].load());
}
// the ride's before / after (game thread)
struct BootsRide {
    bool on = false, due = false;
    BootsNow at_board;
    double end_ms = 0;
    std::uint32_t skips0 = 0, ev0[5] = {}, blk0[5] = {};
    char kind[16] = {};
};
BootsRide g_boots_ride;
bool g_boots_meter = false;  // ride_test 34 1 (game thread)
double g_boots_meter_ms = 0;
float g_boots_meter_last = -1;
unsigned g_boots_marks = 0;
BootsNow g_boots_mark;
double g_boots_mark_ms = 0;
void BootsRideStart(std::uintptr_t sam, const char* kind) noexcept {
    // the seat back after a save (Resume) before the summary: one ride, measured from the first boarding
    if (g_boots_ride.due) {
        g_boots_ride.due = false;
        return;
    }
    g_boots_ride = BootsRide{};
    g_boots_ride.on = ReadBootsOf(sam, g_boots_ride.at_board);
    g_boots_ride.skips0 = g_boots_skips.load();
    for (int i = 0; i < 5; ++i) g_boots_ride.ev0[i] = g_boots_ev[i].load(), g_boots_ride.blk0[i] = g_boots_ev_blocked[i].load();
    std::snprintf(g_boots_ride.kind, sizeof(g_boots_ride.kind), "%s", kind);
}
void BootsRideEnd(double now) noexcept {
    if (g_boots_ride.on) g_boots_ride.due = true, g_boots_ride.end_ms = now + kShieldAfterMs + 500;
}
void BootsRideTick(std::uintptr_t sam, double now) noexcept {
    if (!g_boots_ride.due || now < g_boots_ride.end_ms || g_resume.on || (g_p.st == PState::requesting && g_p.resume)) return;  // a seat back after a save continues the ride
    g_boots_ride.due = false;
    g_boots_ride.on = false;
    BootsNow b;
    if (!ReadBootsOf(sam, b)) {
        Log("[ride] boots over the ride on the %s: unreadable now", g_boots_ride.kind);
        return;
    }
    std::uint32_t ev[5], blk[5];
    for (int i = 0; i < 5; ++i) ev[i] = g_boots_ev[i].load() - g_boots_ride.ev0[i], blk[i] = g_boots_ev_blocked[i].load() - g_boots_ride.blk0[i];
    const float d = b.Life() - g_boots_ride.at_board.Life();
    Log("[ride] boots over the ride on the %s (and 1.5 s after): life %.3f -> %.3f, %s; guard %s, %u wear ticks skipped; wear events j/t/l/s/p "
        "%u/%u/%u/%u/%u, dropped %u/%u/%u/%u/%u",
        g_boots_ride.kind, g_boots_ride.at_board.Life(), b.Life(), std::fabs(d) < 0.001f ? "unchanged" : "LOST",
        !g_boots_hooked.load() ? "not installed" : g_boots_enabled.load() ? "on" : "OFF (ride_test 35 0)", g_boots_skips.load() - g_boots_ride.skips0, ev[0], ev[1],
        ev[2], ev[3], ev[4], blk[0], blk[1], blk[2], blk[3], blk[4]);
    char from[200] = {};
    int n = 0;
    static const char* const kNames[5] = {"jump", "tumble", "landed", "sliding", "slipping"};
    for (int i = 0; i < 5; ++i) {
        const std::uint64_t r = g_boots_ev_from[i].load();
        if (ev[i] && r && n >= 0 && n < static_cast<int>(sizeof(from)))
            n += std::snprintf(from + n, sizeof(from) - n, "%s%s from ds.exe+0x%llX", n ? ", " : "", kNames[i], static_cast<unsigned long long>(r - mem::Base()));
    }
    if (from[0]) Log("[ride] boots over the ride: the newest wear event callers: %s", from);
}
void BootsMeterLine(std::uintptr_t sam, const char* what) noexcept {
    BootsNow b;
    if (!ReadBootsOf(sam, b)) {
        Log("[ride] boots %s: unreadable (no Sam or status)", what);
        return;
    }
    char ev[64], blk[64];
    EventCounts(ev, sizeof(ev), g_boots_ev);
    EventCounts(blk, sizeof(blk), g_boots_ev_blocked);
    Log("[ride] boots %s: life %.3f (main %.3f layer %.3f bare %.3f, kind %u state %u, item %.3f), %+.3f in 1 s; queued %.3f; on-foot flag %d, move "
        "level %d, speed %.2f m/s; status +0x140 %.2f +0x144 %.2f; skipped %u; events j/t/l/s/p %s, dropped %s",
        what, b.Life(), b.main, b.layer, b.bare, b.kind, b.state, b.item, g_boots_meter_last < 0 ? 0.0 : static_cast<double>(b.Life() - g_boots_meter_last),
        b.pending, b.flag, b.level, b.speed, b.st140, b.st144, g_boots_skips.load(), ev, blk);
    g_boots_meter_last = b.Life();
}
void BootsMeterTick(std::uintptr_t sam, double now) noexcept {
    BootsRideTick(sam, now);
    if (!g_boots_meter || now - g_boots_meter_ms < 1000) return;
    g_boots_meter_ms = now;
    BootsMeterLine(sam, "meter");
}
// ride_test 34 2: a mark; logs the life and its change since the last mark (a scenario's control steps)
bool BootsMark(std::uintptr_t sam, double now, char* msg, std::size_t cap) noexcept {
    BootsNow b;
    if (!ReadBootsOf(sam, b)) return std::snprintf(msg, cap, "boots unreadable"), false;
    ++g_boots_marks;
    const bool first = g_boots_marks == 1 || !g_boots_mark.ok;
    const float d = first ? 0.0f : b.Life() - g_boots_mark.Life();
    Log("[ride] boots mark #%u: life %.3f (%s %+.3f since mark #%u, %.1f s); main %.3f layer %.3f, kind %u; carried %s", g_boots_marks, b.Life(),
        first ? "first" : std::fabs(d) < 0.001f ? "unchanged" : d < 0 ? "dropped" : "rose", d, g_boots_marks - 1, first ? 0.0 : (now - g_boots_mark_ms) / 1000.0,
        b.main, b.layer, b.kind, BootsGuarded(sam) ? "or guarded" : "no");
    g_boots_mark = b;
    g_boots_mark_ms = now;
    return std::snprintf(msg, cap, "boots mark #%u: life %.3f (%+.3f)", g_boots_marks, b.Life(), d), true;
}
void InstallShield() {
    InstallHealthGuard();
    InstallBootsGuard();
    for (const Site& s : kLandSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            std::snprintf(g_shield_off, sizeof(g_shield_off), "this game build differs (%s)", s.name);
            Log("[ride] byte check FAILED: %s at 0x%llX: the landing shield is OFF (a carried Sam can take fall damage on a slope)", s.name,
                static_cast<unsigned long long>(s.va));
            return;
        }
    // the mover update's call really reaches the landing check
    if (mem::RipTarget(A(0x143608A6A), 1, 5) != A(kLanding)) {
        std::snprintf(g_shield_off, sizeof(g_shield_off), "this game build differs (the landing check's call)");
        Log("[ride] byte check FAILED: the call at 0x143608A6A does not reach 0x14360C0B0: the landing shield is OFF");
        return;
    }
    void* t = reinterpret_cast<void*>(A(kLanding));
    MH_STATUS s = MH_CreateHook(t, reinterpret_cast<void*>(&H_Landing), reinterpret_cast<void**>(&o_landing));
    if (s == MH_OK) s = MH_EnableHook(t);
    if (s != MH_OK) {
        std::snprintf(g_shield_off, sizeof(g_shield_off), "the hook failed (%s)", MH_StatusToString(s));
        Log("[ride] the landing shield is OFF: %s", g_shield_off);
        return;
    }
    g_shield_off[0] = 0;
    g_shield_hooked.store(true);
    Log("[ride] landing shield ON: %u byte checks; a carried Sam (and 1.5 s after any exit) never takes the game's landing damage",
        static_cast<unsigned>(sizeof(kLandSites) / sizeof(kLandSites[0])) + 1);
}
// Our Sam is shielded from now on (carried: no end).
void ShieldOn(std::uintptr_t sam) noexcept {
    {
        const std::uintptr_t pc = mem::Q(sam + 0x54C0);
        const std::uintptr_t hobj = pc ? mem::Q(pc + 0x2E0) : 0;
        float hv = 0;
        g_shield_hobj.store(hobj && mem::Rd(hobj + 0xC0, hv) && std::isfinite(hv) ? hobj : 0, std::memory_order_release);
    }
    g_shield_until.store(0, std::memory_order_relaxed);
    g_shield_sam.store(sam, std::memory_order_release);
    g_shield_skips.store(0, std::memory_order_relaxed);
    TakeBits(g_shield_drop_bits), TakeBits(g_shield_h_bits);
}
// ... until kShieldAfterMs from now (an exit, the stop).
void ShieldAfter() noexcept {
    if (g_shield_sam.load(std::memory_order_relaxed)) g_shield_until.store(GetTickCount64() + static_cast<std::uint64_t>(kShieldAfterMs), std::memory_order_relaxed);
}
// ---------------------------------------------------------------- hidden-test diagnostics: who writes our Sam's health
// ride_test 18 1 arms a write watchpoint (debug register 0, every thread of the game) on his health (Sam +0x54C0 -> +0x2E0
// -> +0xC0) while he is carried; each write records the writing instruction and the code addresses on its stack (logged by
// the tick). Hidden runs only; disarmed 5 s after the ride.
struct WpHit {
    std::uint64_t rip = 0;
    std::uint64_t ret[8] = {};
    float value = 0;
};
WpHit g_wp_hits[16];
std::atomic<unsigned> g_wp_n{0}, g_wp_logged{0};
std::atomic<std::uintptr_t> g_wp_addr{0};
bool g_wp_enabled = false, g_wp_armed = false;
double g_wp_disarm_ms = 0;
PVOID g_wp_veh = nullptr;
LONG CALLBACK WpHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    const unsigned i = g_wp_n.fetch_add(1);
    if (i < 16) {
        WpHit& h = g_wp_hits[i];
        h.rip = c->Rip;
        const std::uintptr_t a = g_wp_addr.load();
        if (a) h.value = *reinterpret_cast<const volatile float*>(a);
        const std::uint64_t* sp = reinterpret_cast<const std::uint64_t*>(c->Rsp);
        unsigned k = 0;
        for (int j = 0; j < 160 && k < 8; ++j) {
            const std::uint64_t q = sp[j];
            if (mem::InText(q)) h.ret[k++] = q;
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
DWORD WINAPI WpSetAll(LPVOID p) {
    const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(p);
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    unsigned n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                if (addr) {
                    c.Dr0 = addr;
                    c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18);  // L0, write, 4 bytes
                } else {
                    c.Dr0 = 0;
                    c.Dr7 &= ~0xF0003ull;
                }
                if (SetThreadContext(t, &c)) ++n;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return n;
}
void WpArm(std::uintptr_t addr) {
    if (!g_wp_veh) g_wp_veh = AddVectoredExceptionHandler(1, &WpHandler);
    g_wp_addr.store(addr);
    HANDLE th = CreateThread(nullptr, 0, &WpSetAll, reinterpret_cast<LPVOID>(addr), 0, nullptr);
    DWORD n = 0;
    if (th) {
        WaitForSingleObject(th, 2000);
        GetExitCodeThread(th, &n);
        CloseHandle(th);
    }
    g_wp_armed = addr != 0;
    Log("[ride] test: health watchpoint %s at %p on %lu threads", addr ? "armed" : "disarmed", reinterpret_cast<void*>(addr), static_cast<unsigned long>(n));
}
void WpTick(double now) {
    const unsigned n = (std::min)(16u, g_wp_n.load());
    for (unsigned i = g_wp_logged.load(); i < n; ++i) {
        const WpHit& h = g_wp_hits[i];
        char st[400] = {};
        std::size_t k = 0;
        for (std::uint64_t r : h.ret)
            if (r) k += std::snprintf(st + k, sizeof(st) - k, " +0x%llX", static_cast<unsigned long long>(r - mem::Base()));
        Log("[ride] test: health write #%u by ds.exe+0x%llX (health now %.1f); code on its stack:%s", i + 1, static_cast<unsigned long long>(h.rip - mem::Base()),
            h.value, st);
    }
    g_wp_logged.store(n);
    if (g_wp_armed && g_wp_disarm_ms > 0 && now > g_wp_disarm_ms) {
        g_wp_disarm_ms = 0;
        WpArm(0);
    }
}

// v0.21: the crash rule. g_crash_legacy (ride_test 17 0): v0.20's one-frame rule, the exit and the hit for every kind.
std::atomic<bool> g_crash_legacy{false};
// The vehicle our Sam last rode, and when he got off (partner_tackle: no hit from it right after).
std::uintptr_t g_last_ride_entity = 0;
double g_last_ride_end_ms = -1e18;
constexpr double kAfterRideNoHitMs = 3000.0;

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
int KindOfEntity(std::uintptr_t e) noexcept {
    if (!e) return 0;
    const std::uintptr_t vt = mem::Q(e);
    for (int k = 1; k <= 3; ++k)
        if (vt == A(kVehVt[k])) return k;
    return 0;
}
bool VehicleLive(std::uintptr_t e, std::uintptr_t unit, std::uint8_t kind) noexcept {
    return e && unit && KindOfEntity(e) == kind && mem::Q(e + kVUnit) == unit && mem::Q(unit + kUEntRef) == e + 0x20;
}
const float (&Rows(const WorldTransform& t))[9] { return *reinterpret_cast<const float(*)[9]>(&t.rot); }
void PosOf(const WorldTransform& t, double (&p)[3]) noexcept { p[0] = t.pos.x, p[1] = t.pos.y, p[2] = t.pos.z; }
double Dist2D(const double (&a)[3], const double (&b)[3]) noexcept { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])); }
double Dist3(const WorldTransform& a, const WorldTransform& b) noexcept {
    const double x = a.pos.x - b.pos.x, y = a.pos.y - b.pos.y, z = a.pos.z - b.pos.z;
    return std::sqrt(x * x + y * y + z * z);
}

// The seat's world transform on a vehicle at `xf`, moved `lead_s` along its velocity: the vehicle's rotation, the seat's place.
WorldTransform SeatXf(const WorldTransform& xf, const float (&vel)[3], double lead_s, const float (&seat)[3]) noexcept {
    double p[3];
    PosOf(xf, p);
    for (int i = 0; i < 3; ++i) p[i] += static_cast<double>(vel[i]) * lead_s;
    double w[3];
    rwire::ToWorld(p, Rows(xf), seat, w);
    WorldTransform out = xf;
    out.pos.x = w[0], out.pos.y = w[1], out.pos.z = w[2];
    return out;
}

// The local Sam's capsule component (Sam +0x5A0 refers to it + 0x20; its vtable and its entity checked), 0 when not.
std::uintptr_t SamCapsule(std::uintptr_t sam) noexcept {
    const std::uintptr_t ref = sam ? mem::Q(sam + kSamCapsuleRef) : 0;
    if (!ref) return 0;
    const std::uintptr_t c = ref - 0x20;
    return mem::Q(c) == A(kCapsuleVt) && mem::Q(c + kCapEntity) == sam ? c : 0;
}
std::int32_t CapCounter(std::uintptr_t c) noexcept {
    std::int32_t n = -999;
    if (c) mem::Rd(c + kCapCounter, n);
    return n;
}
int CapOnByte(std::uintptr_t c) noexcept {
    std::uint8_t b = 0xFF;
    if (c) mem::Rd(c + kCapOn, b);
    return b;
}

// ---------------------------------------------------------------- SEH calls (POD locals only)
bool CallTeleport(std::uintptr_t sam, const WorldTransform* wt) noexcept {
    __try {
        reinterpret_cast<FnTeleport>(A(kEntityTeleport))(sam, wt, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallCapsule(std::uintptr_t fn, std::uintptr_t cap) noexcept {
    __try {
        reinterpret_cast<FnVoid1>(fn)(cap);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- messages
std::uint32_t OurWorld() noexcept {
    std::uint32_t c = 0;
    vshare::WorldReadyNow(&c);
    return c;
}
void Send(rwire::MsgType type, std::uint32_t ride_id, std::uint32_t vehicle_id, std::uint8_t kind, std::uint8_t reason) {
    rwire::Msg m;
    m.type = type;
    m.seat = rwire::SeatOf(kind);
    m.reason = reason;
    m.from = g_role == 1 ? 0 : 1;
    m.ride_id = ride_id;
    m.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(g_now));
    m.vehicle_id = vehicle_id;
    m.kind = kind;
    m.world = OurWorld();
    std::uint8_t buf[rwire::kMessageSize];
    const std::size_t n = rwire::EncodeMsg(m, buf, sizeof(buf));
    const bool ok = n && vshare::SendEvent(buf, n);
    if (type == rwire::MsgType::end && ok) ++g_ends_tx;
    const char* rs = type == rwire::MsgType::deny ? rwire::DenyName(reason) : type == rwire::MsgType::end ? rwire::EndName(reason) : "";
    Log("[ride] %s: %s %s ride #%08X %s 0x%06X%s%s%s", g_role == 1 ? "host" : "guest", ok ? "sent" : "could not send", rwire::MsgTypeName(static_cast<std::uint8_t>(type)),
        ride_id, vwire::KindName(kind), vehicle_id, rs[0] ? " (" : "", rs, rs[0] ? ")" : "");
}

void Notice(const char* text, bool warn, double ms) { coop::SetVehicleNotice(text, warn, g_now + ms); }
void PeerLabel(char* out, std::size_t cap) {
    char name[96] = {};
    std::uint32_t gen = 0;
    bool from_host = false;
    if (link::PeerName(name, sizeof(name), &gen, &from_host) && name[0]) std::snprintf(out, cap, "%s", name);
    else std::snprintf(out, cap, "%s", g_role == 1 ? "your partner" : "the host");
}

// ---------------------------------------------------------------- the passenger: carry
void PoseLog(bool now);
int CarryPoseSource() { return g_pose_mode ? 3 : g_p.solo ? 2 : 1; }
void ApplyCarryState(bool on) {
    overlay::input::SetRideCarry(on);
    realbody::SetRideSeat(on ? g_p.kind : 0, 0);
    realbody::SetRidePose(on ? CarryPoseSource() : 0);
    vdrive::SetCarriedUnit(on ? g_p.unit : 0);
    if (!on) ridecam::Stop("the ride ended");
}

// Turns the capsule back on exactly once (it is a counter), and only on the Sam we turned it off on.
void CapsuleBack(const char* why) {
    if (!g_p.cap_off) return;
    g_p.cap_off = false;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    const std::uintptr_t cap = sam == g_p.sam ? SamCapsule(sam) : 0;
    if (!cap || cap != g_p.cap) {
        Log("[ride] capsule: not turned back on (%s): our Sam is another entity now (a load: the game made a new one)", why);
        return;
    }
    const std::int32_t before = CapCounter(cap);
    const bool ok = CallCapsule(A(kCapsuleOn), cap);
    Log("[ride] capsule back ON (%s): counter %d -> %d, on byte %d%s", why, before, CapCounter(cap), CapOnByte(cap), ok ? "" : " (FAULT in the enable)");
}

void ArmPlace(const double (&pos)[3], const float (&facing)[2], const double* from, double ms) {
    g_place = PendingPlace{};
    g_place.on = true;
    g_place.until = g_now + ms;
    g_place.next_ms = g_now + 100;
    std::memcpy(g_place.pos, pos, sizeof(g_place.pos));
    std::memcpy(g_place.facing, facing, sizeof(g_place.facing));
    if (from) {
        g_place.have_from = true;
        std::memcpy(g_place.from, from, sizeof(g_place.from));
    }
}

// Puts our Sam beside the vehicle: the game's PlaceOn (mode 1: its ground probe) at the exit spot, facing the vehicle.
// game::PlacementGate first, so the mod never warps Sam where the game's own guards would refuse:
//   the game has Sam (a load, a stopping game event, the post-load setup, a parent, Sam+0x678, a snap or pushed transform,
//     a sequence): no placement (the game places him; the capsule comes back on where he is);
//   Sam asleep (a menu), a save being captured / written, the world waiting for a scene: the placement waits (up to 2
//     minutes, while he stays where he was left; for a save the guard holds, a sleeping Sam is put at the spot first, at a
//     ground-safe height, only when the game's other checks (gate 2: a sequence, a stopping game state ...) pass too);
//   PlaceOn refused for its rate limit only, or the teleport off (its byte checks) with Sam awake: the entity teleport
//     there (the carry's own call; never after it faulted this ride) and PlaceOn again for 2 s.
// The spot is beside the vehicle where our Sam is: its transform now, or at our newest teleport when he was left behind (a
// menu while it drove on).
void PlaceBeside(bool forced, bool allow_raw) {
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    WorldTransform sam_xf{};
    const bool have_sam = sam && sam == g_p.sam && ReadXf(sam, sam_xf);
    WorldTransform xf{};
    bool have = VehicleLive(g_p.entity, g_p.unit, g_p.kind) && ReadXf(g_p.entity, xf);
    float seat[3] = {};
    SeatOffset(g_p.kind, seat);
    const float zero[3] = {};
    if (have && have_sam && g_p.have_tp_xf && Dist3(SeatXf(xf, zero, 0, seat), sam_xf) > 3.0) {
        Log("[ride] our Sam is %.1f m from the seat (left behind): put beside where the %s was at our last teleport", Dist3(SeatXf(xf, zero, 0, seat), sam_xf),
            vwire::KindName(g_p.kind));
        xf = g_p.tp_xf;
    } else if (!have) {
        if (g_p.have_tp_xf) xf = g_p.tp_xf;
        else if (g_p.have_last) xf = g_p.last_xf;
        else return;
    }
    double p[3];
    PosOf(xf, p);
    const rwire::ExitSpot s = rwire::ExitPlacement(g_p.kind, p, Rows(xf), g_p.last_vel, forced);
    const double spot[3] = {s.pos[0], s.pos[1], s.pos[2] + 0.5};
    double from[3] = {};
    if (have_sam) PosOf(sam_xf, from);
    char why[160] = {};
    const int gate = game::PlacementGate(why, sizeof(why));
    if (gate == 2) {
        Log("[ride] not put beside the %s: the game has our Sam (%s); the game places him", vwire::KindName(g_p.kind), why);
        return;
    }
    if (gate == 1) {
        // A save waits for our Sam (the save guard holds its capture) while he sleeps in a menu (the pause menu's Save):
        // PlaceOn refuses a sleeping Sam, so the carry's own entity teleport puts him at the spot now (the save then records
        // him there), and PlaceOn follows once he is awake. Never while the game captures or writes a save, and never where
        // the game has him (the same checks as an awake placement: a sequence, a stopping game state, a snap ...). There is
        // no ground probe: the spot is lifted to the vehicle's own ground plane there (a slope rising beside it) plus
        // kSaveSpotLiftM, so the save never records him inside the ground (he drops onto it).
        bool for_save = saveguard::WantsOff() && !saveguard::InProgress() && !g_in.local_active && have_sam && allow_raw;
        char why2[160] = {};
        if (for_save) {
            const int awake_gate = game::PlacementGate(why2, sizeof(why2), true);
            if (awake_gate != 0) {
                for_save = false;
                Log("[ride] a save waits and our Sam sleeps (%s), but %s (gate %d): no entity teleport; the placement waits", why, why2, awake_gate);
            }
        }
        bool raw = false;
        double save_spot[3] = {spot[0], spot[1], spot[2]};
        if (for_save) {
            save_spot[2] = rwire::SaveSpotHeight(p, Rows(xf), s.pos);
            WorldTransform t{};
            t.pos.x = save_spot[0], t.pos.y = save_spot[1], t.pos.z = save_spot[2];
            t.rot.m[0][0] = s.facing[1], t.rot.m[0][1] = -s.facing[0], t.rot.m[0][2] = 0;
            t.rot.m[1][0] = s.facing[0], t.rot.m[1][1] = s.facing[1], t.rot.m[1][2] = 0;
            t.rot.m[2][0] = 0, t.rot.m[2][1] = 0, t.rot.m[2][2] = 1;
            raw = CallTeleport(g_p.sam, &t);
            Log("[ride] a save waits and our Sam sleeps (%s): entity teleport beside the %s %s at (%.2f, %.2f, %.2f) (ground-safe: %.2f m over the exit "
                "spot's height)",
                why, vwire::KindName(g_p.kind), raw ? "done" : "FAULTED", save_spot[0], save_spot[1], save_spot[2], save_spot[2] - s.pos[2]);
        }
        ArmPlace(spot, s.facing, raw ? &save_spot[0] : (have_sam ? &from[0] : nullptr), kPlaceWaitMs);
        g_place.at_spot = raw;
        Log("[ride] put beside the %s once possible (%s): waiting up to %.0f s at (%.2f, %.2f, %.2f)", vwire::KindName(g_p.kind), why, kPlaceWaitMs / 1000, spot[0],
            spot[1], spot[2]);
        return;
    }
    if (gate == 0 &&
        game::TeleportLocalPlayerFacing(spot[0], spot[1], spot[2], s.facing[0], s.facing[1], game::TeleportMode::place, why, sizeof(why))) {
        Log("[ride] put beside the %s on its %s side at (%.2f, %.2f, %.2f): %s", vwire::KindName(g_p.kind), s.side > 0 ? "right" : "left", spot[0], spot[1], spot[2],
            why);
        return;
    }
    // only the rate limit refused (gate 0), or the teleport is off (gate 3)
    if (!allow_raw || !g_in.local_active || !have_sam) {
        Log("[ride] PlaceOn refused (%s): no entity teleport (%s); %s", why, !allow_raw ? "it faulted this ride" : !have_sam ? "our Sam is unreadable" : "our Sam is asleep",
            gate == 0 ? "PlaceOn again for 2 s" : "our Sam stays where he is");
        if (gate == 0) ArmPlace(spot, s.facing, have_sam ? &from[0] : nullptr, kPlaceRetryMs);
        return;
    }
    WorldTransform t{};
    t.pos.x = spot[0], t.pos.y = spot[1], t.pos.z = spot[2];
    t.rot.m[0][0] = s.facing[1], t.rot.m[0][1] = -s.facing[0], t.rot.m[0][2] = 0;
    t.rot.m[1][0] = s.facing[0], t.rot.m[1][1] = s.facing[1], t.rot.m[1][2] = 0;
    t.rot.m[2][0] = 0, t.rot.m[2][1] = 0, t.rot.m[2][2] = 1;
    const bool ok = CallTeleport(g_p.sam, &t);
    Log("[ride] PlaceOn refused (%s): entity teleport beside the %s %s%s", why, vwire::KindName(g_p.kind), ok ? "done" : "FAULTED",
        gate == 0 ? "; PlaceOn again for 2 s" : "");
    if (gate == 0) ArmPlace(spot, s.facing, ok ? &spot[0] : (have_sam ? &from[0] : nullptr), kPlaceRetryMs);
}

void PendingPlaceTick() {
    if (!g_place.on) return;
    if (g_now > g_place.until) {
        g_place.on = false;
        Log("[ride] the exit's placement never went through: our Sam stays where he is");
        return;
    }
    if (g_now < g_place.next_ms) return;
    g_place.next_ms = g_now + 100;
    char why[160] = {};
    const int gate = game::PlacementGate(why, sizeof(why));
    if (gate == 1) return;  // still asleep or saving
    if (gate != 0) {
        g_place.on = false;
        Log("[ride] the exit's placement dropped: %s", why);
        return;
    }
    if (g_place.have_from) {
        WorldTransform x{};
        const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
        if (!sam || !ReadXf(sam, x)) return;
        const double dx = x.pos.x - g_place.from[0], dy = x.pos.y - g_place.from[1], dz = x.pos.z - g_place.from[2];
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > kPlaceMovedM) {
            g_place.on = false;
            Log("[ride] the exit's placement dropped: our Sam was moved %.1f m since (the game placed him)", d);
            return;
        }
    }
    if (game::TeleportLocalPlayerFacing(g_place.pos[0], g_place.pos[1], g_place.pos[2], g_place.facing[0], g_place.facing[1], game::TeleportMode::place, why,
                                        sizeof(why))) {
        g_place.on = false;
        Log("[ride] the exit's PlaceOn went through: %s", why);
    }
}
bool PendingPlaceGuarded() noexcept {
    __try {
        PendingPlaceTick();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_place.on = false;
        return false;
    }
}

void EndCarry(rwire::End reason, bool place, bool tell_driver) {
    if (g_p.st != PState::riding) return;
    const bool forced = reason != rwire::End::choice && reason != rwire::End::test;
    const double rode = (g_now - g_p.since) / 1000.0;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    const bool same_sam = sam && sam == g_p.sam;
    PoseLog(true);
    ApplyCarryState(false);
    // the landing shield stays 1.5 s more: the placement's drop and the first ground contact land on a fresh fall reference
    ShieldAfter();
    BootsRideEnd(g_now);
    if (g_wp_armed) g_wp_disarm_ms = g_now + 5000;
    g_last_ride_entity = g_p.entity;
    g_last_ride_end_ms = g_now;
    if (place && same_sam) PlaceBeside(forced, !g_p.tp_fault);
    CapsuleBack(rwire::EndName(static_cast<std::uint8_t>(reason)));
    ++g_exits;
    Log("[ride] %s: our Sam off the %s 0x%06X (#%u, %s%s) after %.1f s; Sam vs the seat over the ride mean %.3f m, max %.3f m, %u of %u ticks over "
        "0.25 m; %u teleports%s",
        g_role == 1 ? "host" : g_role == 2 ? "guest" : "solo", vwire::KindName(g_p.kind), g_p.key.id, g_exits, rwire::EndName(static_cast<std::uint8_t>(reason)),
        forced ? ", forced" : "", rode, g_p.ride_err_n ? g_p.ride_err_sum / g_p.ride_err_n : 0.0, g_p.ride_err_max, g_p.ride_err_over, g_p.ride_err_n,
        g_p.teleports, g_p.solo ? " (solo seat test)" : "");
    const bool jumped_fast = reason == rwire::End::jumped && g_p.last_speed > rwire::kCrashDvMps;
    if (reason == rwire::End::crash && !g_crash_legacy.load()) {
        // v0.21: the ridden vehicle never hurts its passenger (no D3-3 hit); the pillion thrown off a confirmed crash lands
        // under the landing shield
        Log("[ride] crash exit (dv %.1f m/s at %.1f m/s): put off, no hit (the ridden vehicle never hurts its passenger)", g_p.crash_dv, g_p.last_speed);
    } else if ((reason == rwire::End::crash || jumped_fast) && same_sam) {
        // D3-3: the passenger takes the game's own hit and knock-down (M4's handler); also when he jumps off above 8 m/s
        const double dv = jumped_fast ? g_p.last_speed : g_p.crash_dv;
        bool hit = false;
        if (g_crash_hit && VehicleLive(g_p.entity, g_p.unit, g_p.kind)) hit = g_crash_hit(g_p.entity, dv, g_p.last_speed);
        Log("[ride] %s exit (dv %.1f m/s at %.1f m/s): %s", jumped_fast ? "jump-off" : "crash", dv, g_p.last_speed,
            !g_crash_hit ? "no hit handler (M4 partner_tackle not installed): put off only" : hit ? "the game's hit applied" : "the hit handler refused");
    }
    if (tell_driver && !g_p.solo) Send(rwire::MsgType::end, g_p.ride_id, g_p.key.id, g_p.kind, static_cast<std::uint8_t>(reason));
    const bool resume = reason == rwire::End::saving && g_resume_enabled && !g_p.solo && same_sam && tell_driver && !g_stop.load();
    if (resume) {
        g_resume = Resume{};
        g_resume.on = true;
        g_resume.ride_id = g_p.ride_id;
        g_resume.key = g_p.key;
        g_resume.since = g_now;
        Log("[ride] once the save is written our Sam asks for the %s's seat back (ride #%08X, within %.0f s)", vwire::KindName(g_p.kind), g_p.ride_id,
            rwire::kResumeWindowMs / 1000);
    }
    char text[160];
    if (resume) {
        Notice("The game saves: back on the seat in a moment.", false, 3000);
    } else if (forced && reason != rwire::End::stopped) {
        std::snprintf(text, sizeof(text), "Off the %s: %s.", vwire::KindName(g_p.kind), rwire::EndName(static_cast<std::uint8_t>(reason)));
        Notice(text, true, 4000);
    } else {
        Notice("", false, 0);
    }
    g_p = Pass{};
    g_hold.Reset();
    g_press.Reset();
    g_np_ride.store(0, std::memory_order_relaxed);
    g_cooldown_until = g_now + 1500;
    PublishSaveState();
}

bool StartCarry(const vwire::VKey& key, std::uint8_t kind, std::uintptr_t entity, std::uintptr_t unit, std::uint32_t ride_id, bool solo) {
    if (g_stop.load()) {
        Log("[ride] cannot carry our Sam: riding together stopped");
        return false;
    }
    if (!saveguard::ReadsOk()) {
        Log("[ride] cannot carry our Sam: the save checks are off (this game build differs): a save could record him at the seat");
        return false;
    }
    if (saveguard::Busy() && !saveguard::TestAllowsCarry()) {
        Log("[ride] cannot carry our Sam: the game has a save pending or is saving");
        return false;
    }
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    const std::uintptr_t cap = SamCapsule(sam);
    if (!sam || !cap) {
        Log("[ride] cannot carry our Sam: %s", !sam ? "no local Sam" : "his capsule component does not check (Sam +0x5A0)");
        return false;
    }
    float seat[3];
    if (!SeatOffset(kind, seat)) return false;
    g_p = Pass{};
    g_p.st = PState::riding;
    g_p.solo = solo;
    g_p.ride_id = ride_id;
    g_p.key = key;
    g_p.kind = kind;
    g_p.entity = entity;
    g_p.unit = unit;
    g_p.sam = sam;
    g_p.cap = cap;
    g_p.since = g_now;
    g_p.world = OurWorld();
    g_p.log_ms = g_now;
    g_p.cap_counter0 = CapCounter(cap);
    ShieldOn(sam);  // before the capsule goes off: no landing damage (and no other damage, vhit's guard) while carried
    BootsRideStart(sam, vwire::KindName(kind));
    if (g_health_hooked.load() && !g_shield_hobj.load()) Log("[ride] health guard: our Sam's health object is not readable (Sam +0x54C0 -> +0x2E0): not guarded");
    if (g_wp_enabled && automation::Enabled()) {
        const std::uintptr_t pc = mem::Q(sam + 0x54C0);
        const std::uintptr_t hobj = pc ? mem::Q(pc + 0x2E0) : 0;
        if (hobj) WpArm(hobj + 0xC0);
        g_wp_disarm_ms = 0;
    }
    const bool ok = CallCapsule(A(kCapsuleOff), cap);
    g_p.cap_off = ok;
    if (!ok) {
        Log("[ride] FAULT in the capsule disable: not carried");
        ShieldAfter();
        g_p = Pass{};
        return false;
    }
    ApplyCarryState(true);
    PublishSaveState();
    g_hold.Reset();  // the hold that boarded must be released before one gets off
    g_press.Reset(); // ... and the press (native prompt)
    g_np_last_press_ms = g_now;  // a native press still held from boarding never gets off
    {
        std::uint32_t aid = 0;
        if (!solo && mem::Rd(entity + 0x314, aid) && aid != 0 && aid != kNoActorId) g_np_ride.store(aid, std::memory_order_relaxed);
        g_np_lift.store(0, std::memory_order_relaxed);
        g_np_offs.store(0, std::memory_order_relaxed);
    }
    ++g_boards;
    Log("[ride] %s: our Sam carried on the %s 0x%06X (%s, ride #%08X, #%u): capsule off (counter %d -> %d, on byte %d), seat (%.2f, %.2f, %.2f), "
        "input blocked, pose %s",
        g_role == 1 ? "host" : g_role == 2 ? "guest" : "solo", vwire::KindName(kind), key.id, rwire::SeatName(static_cast<std::uint8_t>(rwire::SeatOf(kind))), ride_id,
        g_boards, g_p.cap_counter0, CapCounter(cap), CapOnByte(cap), seat[0], seat[1], seat[2],
        g_pose_mode ? "seated idle" : solo ? "the kept driver pose (solo test)" : "the driver's");
    char label[96], text[200];
    PeerLabel(label, sizeof(label));
    if (solo) std::snprintf(text, sizeof(text), "Seat test: carried on the %s.", vwire::KindName(kind));
    else if (NativeOn()) std::snprintf(text, sizeof(text), "Riding with %s. Press [X] / [F] to get off.", label);
    else std::snprintf(text, sizeof(text), "Riding with %s. Hold [X] / [F] to get off.", label);
    Notice(text, false, 5000);
    return true;
}

// The game takes our Sam. Two kinds:
//   game  the game moves or holds him itself (a parent, a load, a cutscene / game-over / fast-travel state current or
//         requested, Sam+0x678 (its warp guard), a pushed transform, a sequence binding): no placement of ours;
//   ours  nothing of the game's places him (a health drop: a MULE spear, a BT, a fall; asleep for a minute; the world
//         waiting for a scene, load mask 0x100): he is put beside the vehicle (PlaceBeside waits while he sleeps).
// A save is not "taken": the save guard's own exit (End::saving, CarryTick) comes before the save captures him.
// Every tick (the sequence answer every 250 ms; the health every 100 ms). A BT / tar grab has no trigger of its own (the
// game's "Hugged" state is not researched): it ends the ride by its health drop or its game state.
constexpr std::uint64_t kTakenBits = (1ull << 3) | (1ull << 6) | (1ull << 8) | (1ull << 9) | (1ull << 10) | (1ull << 11) | (1ull << 12) | (1ull << 13) |
                                     (1ull << 14) | (1ull << 15) | (1ull << 17) | (1ull << 18) | (1ull << 19) | (1ull << 20) | (1ull << 22) | (1ull << 23) |
                                     (1ull << 24) | (1ull << 25) | (1ull << 29) | (1ull << 30);
enum class TakenBy : std::uint8_t { none, game, ours };
TakenBy Taken(const char** why) {
    const std::uintptr_t sam = g_p.sam;
    if (mem::Q(sam + kEParent) != 0) return *why = "Sam has a parent entity", TakenBy::game;
    const bool seq = g_now >= g_p.next_seq_ms;
    if (seq) g_p.next_seq_ms = g_now + 250;
    bool ours = false;
    if (const char* t = game::CarriedSamTaken(kTakenBits, seq, &ours)) return *why = t, ours ? TakenBy::ours : TakenBy::game;
    if (g_p.asleep_since > 0 && g_now - g_p.asleep_since > 60000) return *why = "asleep for a minute", TakenBy::ours;
    if (g_now >= g_p.next_health_ms) {
        g_p.next_health_ms = g_now + 100;
        game::Diagnostics d;
        game::ReadDiagnostics(&d);
        if (d.health_valid) {
            if (g_p.health0 < 0 || d.health > g_p.health0) g_p.health0 = d.health;
            else if (d.health < g_p.health0 - 1.0f) return *why = "a health drop", TakenBy::ours;
        }
    }
    if (g_p.test_taken) return *why = "test", TakenBy::game;
    return TakenBy::none;
}

// Our own read of the ride buttons (getting off while carried, the ImGui fallback's hold): any XInput pad (0..3, before our
// carry mask), the game's own pad state (a pad the game reads without XInput: the PlayStation pad, Steam Input, HID; the
// carry mask zeroes an XInput pad there, so both are read), and the ride key at either hook code. In the launcher's Local
// test the host game is keyboard + mouse only (the pads are the guest's): no pad source there. `how` names the source.
// (v0.19: XInput index 0 and the key at HC_ACTION only: neither saw the user's DualSense nor, with ds.exe's hook above
// ours, his F; play test 2.)
bool PadSourcesAllowed() { return overlay::input::GetLocalMode() != overlay::input::LocalMode::host; }
bool RideButtonDown(bool exit, const char** how) {
    static char buf[48];
    if (PadSourcesAllowed()) {
        int idx = -1;
        const std::uint16_t want = exit ? (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_B) : XINPUT_GAMEPAD_X;
        if (overlay::input::RidePadButtons(500, &idx) & want) {
            std::snprintf(buf, sizeof(buf), "XInput pad #%d", idx);
            return *how = buf, true;
        }
        int dev = -1;
        if (GamePadDown(kPadX, &dev) || (exit && GamePadDown(kPadB, &dev))) {
            std::snprintf(buf, sizeof(buf), "the game's pad state (device %d)", dev);
            return *how = buf, true;
        }
    }
    if (overlay::input::RideKeyDown()) return *how = "key F", true;
    return *how = "", false;
}
bool ExitButtonDown(const char** how) { return RideButtonDown(true, how); }
bool BoardButtonDown(const char** how) { return RideButtonDown(false, how); }

// The game's TryStart starts on the partner's vehicle since the last tick (native prompt), drained every tick whatever the
// state (a start while asking or riding never counts later): one press per press (TryStart repeats while it is held).
std::uint32_t g_np_tick_press = 0;     // this tick: the vehicle id of a new press, 0 none
void PollNativePress() {
    g_np_tick_press = 0;
    const unsigned n = g_np_presses.load(std::memory_order_acquire);
    if (n == g_np_seen_presses) return;
    g_np_seen_presses = n;
    const bool fresh = rwire::NativePressIsNew(g_np_last_press_ms, g_now);
    g_np_last_press_ms = g_now;
    if (fresh) g_np_tick_press = g_np_press_id.load(std::memory_order_relaxed);
}
bool TakeNativePress(std::uint32_t want_id) {
    if (!want_id || g_np_tick_press != want_id) return false;
    g_np_tick_press = 0;
    return true;
}

// The seated idle's state (source 3): a new base at once, the arms every 10 s while carried, and at the end.
void PoseLog(bool now) {
    if (realbody::RidePose() != 3) return;
    realbody::RidePoseStats s;
    const bool line = now || g_now - g_p.pose_log_ms >= 10000;
    realbody::RidePoseStatus(&s, line);
    if (s.latches != g_p.pose_latches) {
        g_p.pose_latches = s.latches;
        char from[96];
        if (s.latch_from == 2) std::snprintf(from, sizeof(from), "our own Sam's calm driving at %.1f m/s", s.latch_speed);
        else if (s.latch_from == 3) std::snprintf(from, sizeof(from), "the kept snapshot (seat_measure)");
        else if (s.latch_final) std::snprintf(from, sizeof(from), "the driver's frame, driving calmly at %.1f m/s", s.latch_speed);
        else std::snprintf(from, sizeof(from), "the driver's frame as the ride started");
        Log("[ride] seated idle: the %s's base %s (%s)", vwire::KindName(static_cast<std::uint8_t>(s.latch_kind)), s.latch_final ? "latched" : "held for now", from);
    }
    if (!line) return;
    g_p.pose_log_ms = g_now;
    Log("[ride] seated idle: arms %s (convention %d, gate error %.1e / %.1e), %u solves, %u refused; hands vs their targets mean %.2f cm max %.2f cm over "
        "%u updates; rest along %.2f up %.2f in %.2f twist %.0f deg",
        !s.ik_on ? (s.ik_off_by_error ? "OFF (the hands missed their targets: off for the session)" : "off (ride_test 23 0 0)")
        : s.ik_state == 2 ? "OFF (the joint convention did not check)"
        : s.ik_state < 0  ? "not measured yet"
                          : "ON",
        s.ik_state, s.gate_err[0], s.gate_err[1], s.solves, s.fails, s.err_mean_cm, s.err_max_cm, s.err_n, s.along, s.up, s.in, s.twist_deg);
}

void CarryTick() {
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    if (!sam || sam != g_p.sam) {
        g_p.cap_off = false;  // that Sam is gone: his capsule with him
        EndCarry(rwire::End::world, false, true);
        return;
    }
    // the vehicle
    const bool live = VehicleLive(g_p.entity, g_p.unit, g_p.kind);
    WorldTransform xf{};
    const bool have_xf = live && ReadXf(g_p.entity, xf);
    if (have_xf) g_p.gone_since = 0;
    else if (g_p.gone_since == 0) g_p.gone_since = g_now;
    // its velocity: measured by the drive module (the partner's follower), else from our own reads
    vdrive::DrivenView view;
    const bool driven = !g_p.solo && vdrive::PartnerDriving(&view) && view.key.id == g_p.key.id;
    float vel[3] = {};
    if (driven && view.meas) std::memcpy(vel, view.vel, sizeof(vel));
    else if (have_xf && g_p.have_last && g_dt_ms > 1) {
        vel[0] = static_cast<float>((xf.pos.x - g_p.last_xf.pos.x) * 1000.0 / g_dt_ms);
        vel[1] = static_cast<float>((xf.pos.y - g_p.last_xf.pos.y) * 1000.0 / g_dt_ms);
        vel[2] = static_cast<float>((xf.pos.z - g_p.last_xf.pos.z) * 1000.0 / g_dt_ms);
        for (float& c : vel)
            if (!std::isfinite(c) || std::fabs(c) > 100.0f) c = 0;
    }
    const double speed = std::sqrt(static_cast<double>(vel[0]) * vel[0] + static_cast<double>(vel[1]) * vel[1] + static_cast<double>(vel[2]) * vel[2]);
    if (!g_p.solo) {
        if (driven) g_p.not_driven_since = 0;
        else if (g_p.not_driven_since == 0) g_p.not_driven_since = g_now;
    }
    // asleep (a menu): held, not teleported; the input block lifted meanwhile (the game's menu or notice needs the pad,
    // the mouse and the keys), back once he is awake
    if (!g_in.local_active) {
        if (g_p.asleep_since == 0) g_p.asleep_since = g_now;
    } else {
        g_p.asleep_since = 0;
    }
    overlay::input::SetRideCarry(g_in.local_active);
    // the forced exits
    rwire::ForcedIn f;
    f.stopped = g_stop.load();
    std::uint32_t counter = 0;
    const bool ready = vshare::WorldReadyNow(&counter);
    f.world_changed = !g_p.solo && (!ready || counter != g_p.world);
    const char* taken_why = nullptr;
    const TakenBy taken = Taken(&taken_why);
    f.taken = taken != TakenBy::none;
    f.link_silent_ms = g_p.solo ? 0 : g_now - g_in.partner_rx_ms;
    if (f.link_silent_ms > rwire::kLinkSilentMs) {
        // v0.21: a stall is ridden out on the seat (up to kLinkSilentCarriedMs); a link that says it is gone ends it now
        f.link_lost = !link::GetInfo().connected;
        if (g_p.stall_since == 0) {
            g_p.stall_since = g_now;
            Log("[ride] no pose from the driver for %.0f ms: our Sam stays on the seat (link %s; the ride ends after %.1f s without one)", f.link_silent_ms,
                f.link_lost ? "lost" : "still connected", rwire::kLinkSilentCarriedMs / 1000);
        }
    } else if (g_p.stall_since != 0) {
        Log("[ride] the driver's poses are back after %.0f ms: the ride goes on", g_now - g_p.stall_since);
        g_p.stall_since = 0;
        ++g_p.stalls;
    }
    f.not_driven_ms = g_p.not_driven_since > 0 ? g_now - g_p.not_driven_since : 0;
    f.vehicle_live = g_p.gone_since == 0 || g_now - g_p.gone_since < 300;
    std::int32_t rs = 0;
    std::uint32_t f560 = 0;
    f.broken = (live && mem::Rd(g_p.entity + kVRideState, rs) && (rs == 5 || rs == 6)) || (live && mem::Rd(g_p.entity + kVFlags560, f560) && (f560 & 0x2000)) ||
               (driven && view.have_frame && view.frame_broken);
    // our follower's own teleport (vdrive, at most every 2 s, only far off: 8 m while we ride it) is never a crash or a flip
    WorldTransform tp_xf{};
    double tp_age = 1e9;
    const bool own_tp = !g_p.solo && vdrive::RecentTeleport(g_p.entity, 500, &tp_xf, &tp_age);
    f.flipped = have_xf && g_p.flip.Update(own_tp ? 1.0 : xf.rot.m[2][2], g_now);
    if (driven && g_in.have_play) {
        float fv[3];
        double key = 0;
        float kmh = -1;
        if (vdrive::FrameAtPlay(g_p.key.id, g_in.play_ms, fv, &key, &kmh)) {
            if (g_crash_legacy.load()) {
                const double dv = g_p.crash.Update(fv, key);
                if (dv > 0) g_p.crash_dv = dv;
            } else {
                // v0.21: only a crash the driver's frames confirm (its speed changed and stayed changed, the game's speed field
                // dropped); the cab (truck, roadster) keeps its passenger, as it keeps its driver; the trike's pillion is thrown off
                const double dv = g_p.crash2.Update(fv, kmh, key);
                const double rej = g_p.crash2.TakeRejected();
                if (rej > 0) {
                    ++g_p.crash_rejects;
                    Log("[ride] crash candidate (one frame, dv %.1f m/s at %.1f m/s): not confirmed by the driver's frames: no exit", rej, speed);
                }
                if (dv > 0) {
                    if (g_p.kind == 3) {
                        g_p.crash_dv = dv;
                    } else {
                        ++g_p.cab_crashes;
                        Log("[ride] the driver's %s crashed (dv %.1f m/s at %.1f m/s, confirmed): our Sam stays in the cab", vwire::KindName(g_p.kind), dv, speed);
                    }
                }
            }
        }
    }
    if (g_p.test_crash_dv > 0) g_p.crash_dv = g_p.test_crash_dv, g_p.test_crash_dv = 0;
    f.crash_dv = g_p.crash_dv;
    f.speed = speed;
    f.driver_ended = g_p.driver_ended;
    // a save: the save guard holds a pending one until our Sam is off (WantsOff); a save pending or written without the
    // hold (the guard off, its cap, the post-load save) ends the ride too: the last resort, the placement waits for the write
    f.saving = saveguard::WantsOff() || (saveguard::Busy() && !saveguard::TestAllowsCarry());
    const rwire::End forced = rwire::ForcedExit(f);
    if (forced != rwire::End::count) {
        if (forced == rwire::End::taken) Log("[ride] the game took our Sam: %s", taken_why ? taken_why : "?");
        if (forced == rwire::End::saving)
            Log("[ride] the game %s: our Sam gets off first (%s)", saveguard::InProgress() ? "is saving" : "is about to save",
                saveguard::WantsOff() ? "the save guard holds the save until he is placed" : "the save was not held: the last-resort exit");
        if (forced == rwire::End::driver_ended) Log("[ride] the driver's game ended the ride (%s)", rwire::EndName(g_p.driver_reason));
        if (forced == rwire::End::crash) g_p.last_speed = (std::max)(g_p.last_speed, speed);
        // no placement when the game has Sam (Taken's "game" kind) or the world changed; the driver's END needs no answer
        const bool place = forced == rwire::End::taken ? taken == TakenBy::ours : forced != rwire::End::world && forced != rwire::End::stopped;
        const bool tell = forced != rwire::End::driver_ended;
        EndCarry(forced == rwire::End::driver_ended && g_p.driver_reason == static_cast<std::uint8_t>(rwire::End::driver_off) ? rwire::End::driver_off : forced, place, tell);
        return;
    }
    // getting off by choice (native prompt: a press of X / F, or the game's own RideOff press; the ImGui fallback: a hold):
    // while it is too fast it waits up to 3 s for it to slow down; a second press within that wait gets off at any speed
    // (jumped: put off away from the vehicle's path, above 8 m/s the D3-3 hit), so a driver who keeps going never holds the
    // passenger (only while awake: a menu's buttons are the menu's)
    const char* exit_how = "";
    const bool exit_down = g_in.local_active && ExitButtonDown(&exit_how);
    bool exit_now = false;
    if (NativeOn()) {
        const bool ours = g_press.Update(exit_down);
        const bool game = TakeNativePress(g_np_ride.load(std::memory_order_relaxed)) && g_in.local_active;
        if ((ours || game) && g_now - g_p.last_exit_ms >= rwire::kNativePressGapMs) {
            exit_now = true;
            g_p.last_exit_ms = g_now;
            Log("[ride] get off: pressed (%s)", game ? "the game's RideOff prompt" : exit_how);
        }
        if (!g_p.off_hint_done && g_now - g_p.since > 2500) {
            // the game's ride action offers no prompt while its Sam is carried (its provider is not asked: runs
            // 20261120-102245): the boarding notice says how to get off
            g_p.off_hint_done = true;
            Log("[ride] carried: %s", g_np_offs.load(std::memory_order_relaxed) ? "the game's own RideOff prompt is shown"
                                                                                : "the game offers no RideOff prompt here (the notice says X / F)");
        }
    } else {
        exit_now = g_hold.Update(exit_down, g_now);
        if (exit_now) Log("[ride] get off: held (%s)", exit_how);
    }
    if (exit_now) {
        if (g_p.exit_wait_until > g_now && speed >= rwire::kExitMaxSpeed) {
            Log("[ride] get off: a second %s while the %s moves at %.1f m/s: jumping off", NativeOn() ? "press" : "hold", vwire::KindName(g_p.kind), speed);
            g_p.last_speed = (std::max)(g_p.last_speed, speed);
            EndCarry(rwire::End::jumped, true, true);
            return;
        }
        g_p.exit_wait_until = g_now + 3000;
        if (speed >= rwire::kExitMaxSpeed) {
            Log("[ride] get off: the %s moves at %.1f m/s; off as soon as it is under %.0f m/s (3 s), or at a second %s", vwire::KindName(g_p.kind), speed,
                rwire::kExitMaxSpeed, NativeOn() ? "press" : "hold");
            Notice(NativeOn() ? "Too fast to get off: slow down, or press again to jump off." : "Too fast to get off: slow down, or hold again to jump off.", true,
                   3000);
        }
    }
    if (g_p.exit_wait_until > g_now && speed < rwire::kExitMaxSpeed) {
        EndCarry(rwire::End::choice, true, true);
        return;
    }
    if (!have_xf) return;
    // the meters: where Sam is now against where the seat is now (the vehicle moved by its physics since we placed him)
    float seat[3];
    SeatOffset(g_p.kind, seat);
    const float zero[3] = {};
    const WorldTransform seat_now = SeatXf(xf, zero, 0, seat);
    WorldTransform sam_xf{};
    const bool sam_xf_ok = ReadXf(sam, sam_xf);
    if (g_p.have_last && g_in.local_active && sam_xf_ok) {
        const double e = Dist3(sam_xf, seat_now), ev = Dist3(sam_xf, g_p.last_target);
        g_p.err_sum += e, g_p.err_n++;
        if (e > g_p.err_max) g_p.err_max = e;
        g_p.ride_err_sum += e, g_p.ride_err_n++;
        if (e > g_p.ride_err_max) g_p.ride_err_max = e;
        if (e > 0.25) ++g_p.ride_err_over;
        g_p.err_veh_sum += ev;
        if (ev > g_p.err_veh_max) g_p.err_veh_max = ev;
    }
    std::memcpy(g_p.last_vel, vel, sizeof(vel));
    g_p.last_speed = speed;
    g_p.last_xf = xf;
    // the seated idle's calm test: 3 - 30 m/s, the yaw rate (low-passed 0.25 s) under 3 deg/s, for 0.5 s; never across our
    // follower's own teleport
    {
        const double yaw = std::atan2(xf.rot.m[1][1], xf.rot.m[1][0]);
        if (g_p.have_yaw && g_now > g_p.yaw_ms) {
            const double dt = (g_now - g_p.yaw_ms) / 1000.0;
            const double rate = std::fabs(vwire::WrapPi(yaw - g_p.yaw)) / dt * 57.29577951308232;
            const double k = 1.0 - std::exp(-dt / 0.25);
            g_p.yaw_rate += ((own_tp ? 99.0 : (std::min)(rate, 99.0)) - g_p.yaw_rate) * k;
        }
        g_p.have_yaw = true, g_p.yaw = yaw, g_p.yaw_ms = g_now;
        const bool calm_now = speed >= 3.0 && speed <= 30.0 && g_p.yaw_rate < 3.0 && !own_tp;
        if (!calm_now) g_p.calm_since = 0;
        else if (g_p.calm_since == 0) g_p.calm_since = g_now;
        realbody::SetRideSeat(g_p.kind, g_p.calm_since > 0 && g_now - g_p.calm_since >= 500 ? speed : 0.0);
    }
    // the carry: the seat one frame ahead along the vehicle's velocity (it moves by its own physics later in this frame); our
    // follower's own teleport requested and not applied yet: the seat where it goes (v0.20 left Sam 1.7 m off the seat for
    // a frame: "phasing through the car")
    if (g_in.local_active) {
        const double lead = (std::min)(0.05, g_dt_ms / 1000.0);
        const bool to_tp = own_tp && tp_age < 200 && Dist3(xf, tp_xf) > 0.5;
        if (to_tp) Log("[ride] our %s's follower teleport (%.0f ms ago, %.1f m): Sam put on the seat where it goes", vwire::KindName(g_p.kind), tp_age, Dist3(xf, tp_xf));
        const WorldTransform target = to_tp ? SeatXf(tp_xf, zero, 0.0, seat) : SeatXf(xf, vel, speed > 0.3 ? lead : 0.0, seat);
        {
            // the camera: the game's own ride camera on the vehicle (its position, one frame ahead as the seat)
            const WorldTransform& vb = to_tp ? tp_xf : xf;
            const double ahead = to_tp || speed <= 0.3 ? 0.0 : lead;
            const double shift[3] = {vel[0] * ahead, vel[1] * ahead, vel[2] * ahead};
            ridecam::Feed(reinterpret_cast<game::Entity*>(sam), g_p.entity, g_p.kind, vb, shift);
            ridecam::Follow(reinterpret_cast<game::Entity*>(sam), vb, speed);
        }
        if (g_test_hold_tp && speed < 0.3 && g_p.have_last) {
            // hidden tests (ride_test 26 1): not teleported while the vehicle stands (the camera's reaction to the teleport)
        } else if (CallTeleport(sam, &target)) {
            ++g_p.teleports;
            g_p.last_target = target;
            g_p.have_last = true;
            g_p.tp_xf = xf;
            g_p.have_tp_xf = true;
        } else if (++g_p.tp_faults >= 1) {
            // put beside the vehicle with the game's PlaceOn only (the faulting call never again)
            Log("[ride] FAULT in the entity teleport: the ride ends");
            g_p.tp_fault = true;
            EndCarry(rwire::End::stopped, true, true);
            return;
        }
    }
    if (g_now - g_p.log_ms >= 1000) {
        Log("[ride] carried on the %s 0x%06X: %.1f m/s; Sam vs the seat now mean %.3f m max %.3f m (vs our placement mean %.3f max %.3f) over %u ticks; "
            "capsule counter %d on %d; pose %d",
            vwire::KindName(g_p.kind), g_p.key.id, speed, g_p.err_n ? g_p.err_sum / g_p.err_n : 0.0, g_p.err_max, g_p.err_n ? g_p.err_veh_sum / g_p.err_n : 0.0,
            g_p.err_veh_max, g_p.err_n, CapCounter(g_p.cap), CapOnByte(g_p.cap), realbody::RidePose());
        if (g_shield_hooked.load()) {
            const unsigned n = g_shield_skips.exchange(0, std::memory_order_relaxed);
            const float drop = TakeBits(g_shield_drop_bits), h = TakeBits(g_shield_h_bits);
            const unsigned lc = g_land_calls_sam.exchange(0, std::memory_order_relaxed);
            Log("[ride] carried: the game's landing check skipped %u times%s (it would have seen a vertical speed drop up to %.1f m/s, a falling height "
                "up to %.1f m; his mover ran it %u times); health kept %u times so far",
                n, g_shield_enabled.load() ? "" : " (shield OFF: ride_test 16 0)", drop, h, lc, g_health_blocked.load());
        }
        static double boots_log_ms = 0;
        if (g_boots_hooked.load() && g_now - boots_log_ms >= 10000) {
            boots_log_ms = g_now;
            BootsNow b;
            const bool rd = ReadBootsOf(g_p.sam, b);
            std::uint32_t blk[5];
            for (int i = 0; i < 5; ++i) blk[i] = g_boots_ev_blocked[i].load() - g_boots_ride.blk0[i];
            Log("[ride] carried: boots guard %s: %u wear ticks skipped, wear events dropped j/t/l/s/p %u/%u/%u/%u/%u; boots life %.3f (at boarding %.3f)",
                g_boots_enabled.load() ? "on" : "OFF (ride_test 35 0)", g_boots_skips.load() - g_boots_ride.skips0, blk[0], blk[1], blk[2], blk[3], blk[4],
                rd ? b.Life() : -1.0f, g_boots_ride.on ? g_boots_ride.at_board.Life() : -1.0f);
        }
        g_p.log_ms = g_now;
        g_p.err_sum = g_p.err_max = g_p.err_veh_sum = g_p.err_veh_max = 0;
        g_p.err_n = 0;
    }
    PoseLog(false);
}

// ---------------------------------------------------------------- the passenger: boarding
bool OnFoot() {
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    return sam && g_in.local_ok && g_in.local_active && mem::Q(sam + kEParent) == 0;
}

// Our Sam's health dropped within kHurtBoardBlockMs (read every 100 ms while boarding is looked at).
bool RecentlyHurt() {
    if (g_now - g_board_health_ms >= 100) {
        g_board_health_ms = g_now;
        game::Diagnostics d;
        game::ReadDiagnostics(&d);
        if (d.health_valid && d.health > 0) {  // 0 while a world loads (run 20261119-171723: "1000 -> 0" at the load)
            if (g_board_health >= 0 && d.health < g_board_health - 1.0f) {
                if (g_now >= g_hurt_until) Log("[ride] our Sam was hurt (health %.1f -> %.1f): no boarding for %.0f s", g_board_health, d.health, kHurtBoardBlockMs / 1000);
                g_hurt_until = g_now + kHurtBoardBlockMs;
            }
            g_board_health = d.health;
        } else {
            g_board_health = -1;
        }
    }
    return g_now < g_hurt_until;
}

// What boarding the partner-driven vehicle looks like now.
struct BoardView {
    rwire::Block block = rwire::Block::no_vehicle;
    vdrive::DrivenView v;
    double dist = 1e9;                 // our Sam to its passenger door (horizontal)
    double origin_dist = 1e9;          // ... to its origin (horizontal)
    double dist3 = 1e9;                // ... to its origin (3D: the native prompt's distance key)
    bool native = false;
};
BoardView ViewBoard(bool native) {
    BoardView b;
    rwire::BoardIn in;
    in.feature_on = g_on.load();
    in.link_up = g_now - g_in.partner_rx_ms < rwire::kLinkSilentMs && vshare::PeerInOurWorld();
    in.partner_drives = vdrive::PartnerDriving(&b.v);
    in.kind = b.v.key.kind;
    in.local_on_foot = OnFoot() && !RecentlyHurt();
    in.busy = g_p.st != PState::idle || g_now < g_cooldown_until;
    in.saving = !saveguard::ReadsOk() || (saveguard::Busy() && !saveguard::TestAllowsCarry());
    WorldTransform xf{}, sam{};
    float door[3];
    if (in.partner_drives && ReadXf(b.v.entity, xf) && g_in.local_ok && ReadXf(reinterpret_cast<std::uintptr_t>(g_in.local), sam) &&
        rwire::DoorPoint(b.v.key.kind, door)) {
        double p[3], w[3], s[3];
        PosOf(xf, p);
        rwire::ToWorld(p, Rows(xf), door, w);
        PosOf(sam, s);
        b.dist = Dist2D(w, s);
        b.origin_dist = Dist2D(p, s);
        b.dist3 = Dist3(xf, sam);
    }
    in.door_dist_m = b.dist;
    // the native 7 m rule only for a board asked through the game's own prompt (kReqNative): a debug / test board
    // (kReqTest) is judged by the door rule here, as the driver and the ACCEPT check judge it
    in.native = b.native = native && NativeOn();
    in.origin_dist_m = b.origin_dist;
    in.vehicle_speed = b.v.meas ? b.v.speed_mps : 0;
    b.block = rwire::BoardCheck(in);
    return b;
}

void Request(const BoardView& b, std::uint8_t reason, const char* how) {
    g_p = Pass{};
    g_p.st = PState::requesting;
    g_p.ride_id = (static_cast<std::uint32_t>(GetTickCount64()) * 2654435761u) | 1u;
    g_p.key = b.v.key;
    g_p.kind = b.v.key.kind;
    g_p.entity = b.v.entity;
    g_p.unit = b.v.unit;
    g_p.req_ms = g_now;
    g_p.native = reason == rwire::kReqNative;
    ++g_requests;
    Log("[ride] %s: asking to ride the %s 0x%06X (%s, %.2f m from its door, %.2f m from its origin, ride #%08X)", g_role == 1 ? "host" : "guest",
        vwire::KindName(g_p.kind), g_p.key.id, reason == rwire::kReqTest ? "test" : how, b.dist, b.origin_dist, g_p.ride_id);
    Send(rwire::MsgType::request, g_p.ride_id, g_p.key.id, g_p.kind, reason);
    Notice("Asking to ride...", false, 3000);
}

// The native prompt's view of the partner's vehicle (the hooks read it on whatever thread the game runs them): its actor
// id, whether boarding it is allowed now (the prompt is lifted), our distance to it. And the provider's observations of it
// logged on every change (which of the game's reasons crosses it out; whether the lift took).
std::uint32_t ActorIdOf(std::uintptr_t entity) {
    std::uint32_t id = 0;
    if (!entity || !mem::Rd(entity + 0x314, id) || id == kNoActorId) return 0;
    return id;
}
void NativeObsLog() {
    static std::uint32_t last[2] = {};
    static unsigned lines = 0;
    for (int i = 0; i < 2; ++i) {
        const std::uint32_t o = g_np_obs[i].load(std::memory_order_relaxed);
        if (o == last[i]) continue;
        last[i] = o;
        if (!o || ++lines > 80) continue;
        Log("[ride] native prompt (slot %d): %s on the partner's vehicle 0x%06X; the game %s (reason %u)%s", i == 0 ? 3 : 6,
            (o & 0x20000000u) ? "RideOff" : "Use Vehicle", g_np_seen.load(std::memory_order_relaxed), (o & 0x10000000u) ? "crossed it out" : "offered it",
            (o >> 20) & 0xF,
            (o & 0x40000000u) ? ": lifted (available)" : (o & 0x20000000u) ? ": carried, offered as get off" : ": left as the game has it");
    }
}

// After a save's exit: once the save is written (and any placement done), our Sam on foot, the partner still driving that
// vehicle: ask for the seat back (once; a deny or no answer leaves him on foot, as before).
void ResumeTick() {
    if (!g_resume.on || g_p.st != PState::idle) return;
    const char* why = nullptr;
    if (g_now - g_resume.since > rwire::kResumeWindowMs) why = "not in time (the save or the placement took too long)";
    else if (!g_on.load() || g_stop.load()) why = "riding together is off";
    if (!why) {
        if (saveguard::Busy() || g_place.on) return;  // the save is not written yet, or the placement waits
        if (g_resume.saved_ms == 0) g_resume.saved_ms = g_now;
        vdrive::DrivenView v;
        const bool link_up = g_now - g_in.partner_rx_ms < rwire::kLinkSilentMs && vshare::PeerInOurWorld();
        if (!link_up) {
            why = "the link is down";
        } else if (!vdrive::PartnerDriving(&v) || v.key.id != g_resume.key.id) {
            why = "the partner no longer drives it";
        } else if (!OnFoot()) {
            return;  // the game holds him this frame: wait (the window caps it)
        } else {
            g_p = Pass{};
            g_p.st = PState::requesting;
            g_p.ride_id = g_resume.ride_id;
            g_p.key = v.key;
            g_p.kind = v.key.kind;
            g_p.entity = v.entity;
            g_p.unit = v.unit;
            g_p.req_ms = g_now;
            g_p.resume = true;
            ++g_requests;
            Log("[ride] %s: asking for the %s 0x%06X's seat back after the save (ride #%08X, %.0f ms after the exit, %.1f m/s)", g_role == 1 ? "host" : "guest",
                vwire::KindName(g_p.kind), g_p.key.id, g_p.ride_id, g_now - g_resume.since, v.meas ? v.speed_mps : 0.0);
            Send(rwire::MsgType::request, g_p.ride_id, g_p.key.id, g_p.kind, rwire::kReqResume);
            g_resume.on = false;
            return;
        }
    }
    Log("[ride] no seat back after the save (ride #%08X): %s", g_resume.ride_id, why);
    g_resume.on = false;
}

void PassengerTick() {
    NativeObsLog();
    PollNativePress();
    ResumeTick();
    if (g_p.st == PState::riding) {
        g_np_seen.store(g_np_ride.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_np_lift.store(0, std::memory_order_relaxed);
        CarryTick();
        return;
    }
    g_np_ride.store(0, std::memory_order_relaxed);
    if (g_p.st == PState::requesting) {
        // the prompt stays available while the driver answers (its presses are drained, not counted)
        g_np_lift.store(NativeOn() ? g_np_seen.load(std::memory_order_relaxed) : 0, std::memory_order_relaxed);

        if (g_now - g_p.req_ms > rwire::kRequestTimeoutMs) {
            Log("[ride] ride #%08X: no answer in %.0f s", g_p.ride_id, rwire::kRequestTimeoutMs / 1000);
            Notice("No answer from the driver.", true, 3000);
            g_p = Pass{};
            g_cooldown_until = g_now + 1500;
        }
        return;
    }
    if (!g_on.load()) {
        g_np_seen.store(0, std::memory_order_relaxed);
        g_np_lift.store(0, std::memory_order_relaxed);
        return;
    }
    const BoardView b = ViewBoard(NativeOn());
    const std::uint32_t aid = b.v.entity && b.block != rwire::Block::no_vehicle && b.block != rwire::Block::no_link ? ActorIdOf(b.v.entity) : 0;
    g_np_seen.store(aid, std::memory_order_relaxed);
    g_np_dist.store(static_cast<float>((std::min)(b.dist3, 50.0)), std::memory_order_relaxed);
    const bool allowed = b.block == rwire::Block::ok && aid != 0;
    g_np_lift.store(allowed && NativeOn() ? aid : 0, std::memory_order_relaxed);
    if (NativeOn()) {
        // the game's own "Use Vehicle": lifted while allowed (the game's cross-out otherwise); its press asks to ride
        static rwire::Block last_block = rwire::Block::count;
        if (b.block != last_block && aid) {
            Log("[ride] native prompt: the partner's %s 0x%06X: boarding %s (%.2f m from its origin, %.2f m from its door)", vwire::KindName(b.v.key.kind), aid,
                b.block == rwire::Block::ok ? "allowed: the game's prompt is lifted" : rwire::BlockName(b.block), b.origin_dist, b.dist);
        }
        last_block = aid ? b.block : rwire::Block::count;
        if (TakeNativePress(aid)) {
            if (allowed) {
                Request(b, rwire::kReqNative, "the game's Use Vehicle");
            } else {
                Log("[ride] native press on the partner's vehicle refused: %s", rwire::BlockName(b.block));
            }
        }
        return;
    }
    const char* how = "";
    const bool down = BoardButtonDown(&how);
    if (b.block != rwire::Block::ok) {
        g_hold.Update(down, g_now);  // keeps track of releases
        return;
    }
    char label[96], text[200];
    PeerLabel(label, sizeof(label));
    const double held = g_hold.HeldMs(g_now);
    if (down && held > 0) std::snprintf(text, sizeof(text), "Hold [X] / [F] to ride with %s... %d%%", label, static_cast<int>((std::min)(100.0, held * 100.0 / rwire::kHoldMs)));
    else std::snprintf(text, sizeof(text), "Hold [X] / [F] to ride with %s", label);
    if (g_now - g_prompt_ms > 100) {
        if (g_prompt_ms == 0 || g_now - g_prompt_ms > 2000) {
            // the prompt opens: what our button reads see now (play test 2: the prompt showed and nothing counted)
            unsigned ages[4];
            const int pads = overlay::input::RidePadAges(ages);
            int dev = -1;
            const bool gp = GamePadDown(kPadX, &dev);
            Log("[ride] prompt: the %s %.2f m from its door; XInput pads read %d (ages %u/%u/%u/%u ms), game pad X %d (device %d, checks %s), key F %d, "
                "pad sources %s",
                vwire::KindName(b.v.key.kind), b.dist, pads, ages[0], ages[1], ages[2], ages[3], gp ? 1 : 0, dev, g_pad_ok ? "ok" : "off",
                overlay::input::RideKeyDown() ? 1 : 0, PadSourcesAllowed() ? "on" : "off (Local test host: keyboard only)");
        }
        Notice(text, false, 400);
        g_prompt_ms = g_now;
    }
    if (g_hold.Update(down, g_now)) Request(b, rwire::kReqHold, how);
}

// ---------------------------------------------------------------- the driver
void DriverEnd(rwire::End reason, bool tell) {
    if (!g_d.accepted) return;
    if (reason == rwire::End::saving) g_dres = DrvResume{g_d.ride_id, g_d.key.id, g_now + rwire::kResumeWindowMs + 2000};
    Log("[ride] %s: the partner's ride #%08X in our %s 0x%06X is over (%s) after %.1f s; body vs seat mean %.3f m", g_role == 1 ? "host" : "guest", g_d.ride_id,
        vwire::KindName(g_d.key.kind), g_d.key.id, rwire::EndName(static_cast<std::uint8_t>(reason)), (g_now - g_d.accept_ms) / 1000.0,
        g_d.err_n ? g_d.err_sum / g_d.err_n : 0.0);
    if (tell) Send(rwire::MsgType::end, g_d.ride_id, g_d.key.id, g_d.key.kind, static_cast<std::uint8_t>(reason));
    g_d = Drv{};
}

void DriverTick() {
    if (!g_d.accepted) return;
    vdrive::DrivenView v;
    if (!vdrive::LocalDriving(&v) || v.key.id != g_d.key.id) {
        DriverEnd(rwire::End::driver_off, true);
        return;
    }
    g_d.entity = v.entity;
    if ((g_in.partner_flags & wire::kPosePassenger) && g_now - g_in.partner_rx_ms < 500) g_d.last_bit_ms = g_in.partner_rx_ms;
    if (rwire::DriverTimedOut(true, g_now - g_d.accept_ms, g_now - g_d.last_bit_ms)) {
        DriverEnd(rwire::End::timeout, true);
        return;
    }
    if (g_now - g_d.log_ms >= 1000 && g_d.err_n) {
        Log("[ride] %s: the partner rides our %s 0x%06X: body vs the seat mean %.3f m max %.3f m over %u ticks; passenger bit %.0f ms ago", g_role == 1 ? "host" : "guest",
            vwire::KindName(g_d.key.kind), g_d.key.id, g_d.err_sum / g_d.err_n, g_d.err_max, g_d.err_n, g_now - g_d.last_bit_ms);
        g_d.log_ms = g_now;
        g_d.err_sum = g_d.err_max = 0;
        g_d.err_n = 0;
    }
}

// ---------------------------------------------------------------- the trike driver's back (v0.23)
// The user: "hide the driver's back cargo on the trike when he has a passenger behind him". The pillion passenger sits
// where the driver's backpack and its stacked cargo are (run 20261121-103032: inside them on both screens). While the
// partner rides our trike (accepted, its body seated: DriverPlacesBody, as PassengerTarget), our Sam's inventory carries
// the game's own HideBackpack and HideSpareShoes: the cutscene event DSPlayerHideBaggageEventInstance's apply (0x14293A380)
// writes inventory +0x4C7C bits 0 and 1, and the inventory's per-frame update (0x1427EEC30) then hides the backpack entity
// (Entity_SetVisible; what hangs on it with it), every carried piece (the carrier's hide reason 0, 0x142CB27A0) and the spare
// boots hanging behind him (inventory +0x4D90: run 20261121-133429 E3a showed them left in the air with bit 0 alone), and
// shows them again (with the carrier's refresh) once the bits clear, as the event's end does. Runtime state only (no
// serializer reads the byte), set and cleared on the game thread, never a game call. Only the bits we set are cleared (a
// bit the game's own event holds is left to it), and only on the same Sam's same inventory
// (a new Sam / world: forgotten, the new inventory starts clear). The passenger's game sees it through our gear frame
// (partner_gear: a hidden backpack, what hangs on it and hidden cargo are not sent) and hides the partner's backpack props
// itself meanwhile (PillionNow). The truck and the roadster are unchanged (their passenger sits beside the driver).
constexpr std::uintptr_t kPlayerVt = 0x143D7D0D8;  // DSPlayerEntity
constexpr std::uintptr_t kSamInv = 0x54C8, kInvOwner = 0x48, kInvHide = 0x4C7C;
constexpr std::uint8_t kHideBits = 0x03;  // bit 0 HideBackpack, bit 1 HideSpareShoes
// clang-format off
constexpr Site kPackSites[] = {
    {0x14293A446, "41 80 A3 7C 4C 00 00 FE", "the HideBaggage event's HideBackpack (inventory +0x4C7C bit 0)"},
    {0x1427F04E8, "F6 C2 02 40 0F B6 C7", "the inventory update's HideSpareShoes test (bit 1)"},
    {0x1427F04AA, "0F B6 96 7C 4C 00 00 F6 C2 01 74 32", "the inventory update's HideBackpack test"},
    {0x1427F0F06, "F6 86 7E 4C 00 00 04 48 8B 86 B8 4C 00 00", "the inventory update's backpack visibility (+0x4C7E, +0x4CB8)"},
    {0x1427F2A9A, "0F B6 54 24 31 45 33 C0 4C 8B E0 48 89 45 D0 48 8B 48 50 E8 EE FC 4B 00", "the inventory update's cargo hide (reason 0)"},
    {0x142CB27A0, "8B 81 8C 00 00 00 4C 8B C9 85 C0", "the carrier's hide-reason mask (+0x8C)"},
    {0x14271FC22, "48 89 83 C8 54 00 00", "Sam's inventory component (+0x54C8)"},
};
// clang-format on
bool g_pack_cfg = true;                // session.ini ride_hide_pack
bool g_pack_ok = false;                // its checks passed (and the flag on)
int g_pack_test = -1;                  // ride_test 36 <1|0|-1>: hidden tests force it on / off; -1 follows the ride
struct PackHide {
    std::uintptr_t sam = 0, inv = 0;   // held on this Sam's inventory
    bool held = false;                 // we set bits and have not cleared them
    std::uint8_t ours = 0;             // ... these (of kHideBits)
    bool foreign_logged = false;       // the game's own event held it when we wanted it (not ours: never cleared by us)
    double since = 0;
    unsigned reasserts = 0;            // the game cleared it while we held it (an event's end): set again
};
PackHide g_pack;
unsigned g_pack_hides = 0, g_pack_releases = 0;
std::uintptr_t g_inv_sam = 0, g_inv_ok = 0;  // the last inventory checked (type name read once per Sam / inventory)

// Our Sam's inventory component, checked (DSPlayerEntity, the component's owner, its RTTI type); 0 when it does not.
std::uintptr_t SamInventory(std::uintptr_t sam) {
    if (!sam || mem::Q(sam) != A(kPlayerVt)) return 0;
    const std::uintptr_t inv = mem::Q(sam + kSamInv);
    if (!inv || mem::Q(inv + kInvOwner) != sam) return 0;
    if (sam == g_inv_sam && inv == g_inv_ok) return inv;
    const char* t = mem::RttiName(mem::RttiOf(inv));
    if (!t || std::strcmp(t, "DSPlayerInventoryComponent") != 0) return 0;
    g_inv_sam = sam, g_inv_ok = inv;
    return inv;
}
// The byte, OR / AND `mask` (game thread; the game writes this byte only there). False on a fault.
bool PackBit(std::uintptr_t inv, int op, std::uint8_t* before, std::uint8_t mask = kHideBits) noexcept {
    __try {
        volatile std::uint8_t* b = reinterpret_cast<volatile std::uint8_t*>(inv + kInvHide);
        const std::uint8_t v = *b;
        if (before) *before = v;
        if (op > 0) *b = static_cast<std::uint8_t>(v | mask);
        else if (op < 0) *b = static_cast<std::uint8_t>(v & ~mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Gives back what we hold (the ride ended, the mod stops): clears bit 0 on the same Sam's same inventory.
void PackRelease(std::uintptr_t sam, const char* why) {
    if (!g_pack.held) return;
    const std::uintptr_t inv = SamInventory(sam);
    if (sam != g_pack.sam || inv != g_pack.inv || !inv) {
        Log("[ride] trike pillion: our Sam or his inventory changed (%s): the hide is forgotten (the new inventory starts clear)", why);
    } else {
        std::uint8_t b = 0;
        const bool ok = PackBit(inv, -1, &b, g_pack.ours);
        ++g_pack_releases;
        Log("[ride] trike pillion: the driver's backpack and cargo shown again (%s) after %.1f s%s (inventory +0x4C7C 0x%02X -> 0x%02X%s; #%u)", why,
            (g_now - g_pack.since) / 1000.0, g_pack.reasserts ? ", the game cleared it meanwhile and it was set again" : "", b,
            static_cast<unsigned>(b & ~g_pack.ours), ok ? "" : ", FAULTED", g_pack_releases);
    }
    g_pack = PackHide{};
}
// Game thread, every tick (Tick, after the driver's side): hidden while the partner rides our trike's pillion.
void PackTick(const TickIn& in) {
    if (!g_pack_ok) return;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(in.local);
    if (!sam || !in.local_ok) return;  // a load / no Sam this tick: the next tick decides (a new Sam forgets)
    const bool ride = !g_stop.load() && g_on.load() && g_d.accepted && g_d.key.kind == 3 &&
                      rwire::DriverPlacesBody(true, g_now - g_d.accept_ms, g_now - g_d.last_bit_ms);
    const bool want = !g_stop.load() && (g_pack_test >= 0 ? g_pack_test == 1 : ride);
    if (g_pack.held && sam != g_pack.sam) {
        PackRelease(sam, "a new Sam");
        if (!want) return;
    }
    if (!want) {
        if (g_pack.held) PackRelease(sam, g_pack_test == 0 ? "test" : g_d.accepted ? "the passenger's body is not seated" : "the ride is over");
        g_pack.foreign_logged = false;
        return;
    }
    const std::uintptr_t inv = SamInventory(sam);
    if (!inv) return;
    if (g_pack.held && inv != g_pack.inv) PackRelease(sam, "a new inventory");
    std::uint8_t b = 0;
    if (!PackBit(inv, 0, &b)) return;
    const std::uint8_t missing = static_cast<std::uint8_t>(kHideBits & ~b);
    if (!missing) {
        if (!g_pack.held && !g_pack.foreign_logged) {
            g_pack.foreign_logged = true;
            Log("[ride] trike pillion: the game itself hides the backpack now (+0x4C7C 0x%02X, a scripted event): left to it", b);
        }
        return;
    }
    if (!PackBit(inv, 1, nullptr, missing)) {
        Log("[ride] trike pillion: FAULT setting the inventory's HideBackpack: off for this session");
        g_pack_ok = false;
        g_pack = PackHide{};
        return;
    }
    if (g_pack.held) {
        g_pack.ours = static_cast<std::uint8_t>(g_pack.ours | missing);
        if (++g_pack.reasserts <= 5) Log("[ride] trike pillion: the game cleared the backpack hide (0x%02X) while the passenger rides: set again", missing);
        return;
    }
    g_pack.held = true;
    g_pack.ours = missing;
    g_pack.sam = sam, g_pack.inv = inv, g_pack.since = g_now, g_pack.reasserts = 0;
    ++g_pack_hides;
    Log("[ride] trike pillion%s: the driver's backpack and cargo hidden (the game's HideBackpack + HideSpareShoes, inventory %p +0x4C7C 0x%02X -> "
        "0x%02X; #%u)",
        g_pack_test >= 0 ? " (test)" : "", reinterpret_cast<void*>(inv), b, static_cast<unsigned>(b | missing), g_pack_hides);
}
bool PackTickGuarded(const TickIn& in) noexcept {
    __try {
        PackTick(in);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool PackReleaseGuarded(std::uintptr_t sam, const char* why) noexcept {
    __try {
        PackRelease(sam, why);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void InstallPack(const Config& config) {
    g_pack_cfg = config.ride_hide_pack;
    unsigned bad = 0;
    const char* first = nullptr;
    for (const Site& s : kPackSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            ++bad;
            if (!first) first = s.name;
            Log("[ride] trike pillion: byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
        }
    const unsigned n = static_cast<unsigned>(sizeof(kPackSites) / sizeof(kPackSites[0]));
    if (bad) {
        Log("[ride] trike pillion: the driver's backpack stays shown with a passenger: this game build differs (%s; %u of %u checks failed)", first, bad, n);
        return;
    }
    if (!g_pack_cfg) {
        Log("[ride] trike pillion: the driver's backpack stays shown with a passenger (session.ini ride_hide_pack=0; %u byte checks ok)", n);
        return;
    }
    g_pack_ok = true;
    Log("[ride] trike pillion: the driver's backpack and cargo are hidden while the partner rides behind him (%u byte checks ok)", n);
}

void OnRequest(const rwire::Msg& m) {
    rwire::DriverIn in;
    in.feature_on = g_on.load();
    in.same_world = m.world == OurWorld() && vshare::PeerInOurWorld();
    vdrive::DrivenView v;
    in.we_drive_it = vdrive::LocalDriving(&v) && v.key.id == m.vehicle_id && v.key.kind == m.kind;
    in.native = m.reason == rwire::kReqNative;
    in.seat_taken = g_d.accepted && g_d.ride_id != m.ride_id;
    in.speed = v.meas ? v.speed_mps : 0;
    WorldTransform xf{};
    float door[3];
    if (in.we_drive_it && g_in.have_partner && ReadXf(v.entity, xf) && rwire::DoorPoint(m.kind, door)) {
        double p[3], w[3], s[3];
        PosOf(xf, p);
        rwire::ToWorld(p, Rows(xf), door, w);
        PosOf(g_in.partner, s);
        in.have_partner = true;
        in.partner_dist_m = Dist2D(w, s);
        in.partner_origin_dist_m = Dist2D(p, s);
    }
    rwire::Deny why = rwire::Deny::count;
    if (g_d.accepted && g_d.ride_id == m.ride_id) {
        Send(rwire::MsgType::accept, m.ride_id, m.vehicle_id, m.kind, 0);  // a repeat: the same answer
        return;
    }
    // the seat back after a save: the ride we ended for that save, in time, still driving that vehicle, the seat free
    const bool resume = m.reason == rwire::kReqResume;
    if (resume) {
        const bool ok = in.feature_on && in.same_world && in.we_drive_it && !g_d.accepted && m.ride_id == g_dres.ride_id && m.vehicle_id == g_dres.vehicle_id &&
                        g_now < g_dres.until;
        g_dres = DrvResume{};
        if (!ok) {
            ++g_denies;
            why = !in.we_drive_it ? rwire::Deny::not_driving : g_d.accepted ? rwire::Deny::seat_taken : !in.same_world ? rwire::Deny::world : rwire::Deny::off;
            Log("[ride] %s: the partner asks for our %s 0x%06X's seat back after a save (ride #%08X): DENIED, %s", g_role == 1 ? "host" : "guest",
                vwire::KindName(m.kind), m.vehicle_id, m.ride_id,
                !in.feature_on    ? "riding together is off"
                : !in.same_world  ? "another world"
                : !in.we_drive_it ? "we no longer drive it"
                : g_d.accepted    ? "the seat is taken"
                                  : "not the ride we ended for a save, or too late");
            Send(rwire::MsgType::deny, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(why));
            return;
        }
    } else if (!rwire::DriverAccepts(in, &why)) {
        ++g_denies;
        Log("[ride] %s: the partner asks to ride our %s 0x%06X (ride #%08X%s): DENIED, %s (speed %.1f m/s, partner %.2f m from the door, %.2f m from the origin)",
            g_role == 1 ? "host" : "guest", vwire::KindName(m.kind), m.vehicle_id, m.ride_id, in.native ? ", the game's Use Vehicle" : "",
            rwire::DenyName(static_cast<std::uint8_t>(why)), in.speed, in.partner_dist_m, in.partner_origin_dist_m);
        Send(rwire::MsgType::deny, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(why));
        return;
    }
    g_d = Drv{};
    g_d.accepted = true;
    g_d.ride_id = m.ride_id;
    g_d.key = v.key;
    g_d.entity = v.entity;
    g_d.accept_ms = g_now;
    g_d.log_ms = g_now;
    ++g_accepts;
    Log("[ride] %s: the partner asks to ride our %s 0x%06X (ride #%08X%s): ACCEPTED (#%u; partner %.2f m from the door, %.2f m from the origin, %.1f m/s)",
        g_role == 1 ? "host" : "guest", vwire::KindName(m.kind), m.vehicle_id, m.ride_id,
        in.native ? ", the game's Use Vehicle" : resume ? ", the seat back after a save" : "", g_accepts, in.partner_dist_m,
        in.partner_origin_dist_m, in.speed);
    Send(rwire::MsgType::accept, m.ride_id, m.vehicle_id, m.kind, 0);
    char label[96], text[200];
    PeerLabel(label, sizeof(label));
    std::snprintf(text, sizeof(text), "%s rides with you.", label);
    Notice(text, false, 4000);
}

void OnMsg(std::uint8_t /*type*/, const std::uint8_t* msg, std::size_t size, std::uint32_t /*generation*/, double now) {
    g_now = now;
    rwire::Msg m;
    if (!rwire::DecodeMsg(msg, size, m) || m.from == (g_role == 1 ? 0 : 1)) {
        if (++g_bad_rx <= 10) Log("[ride] dropped a ride message (%zu bytes): not valid or not from the partner", size);
        return;
    }
    // riding together stopped here (a fault, the test's emergency stop): nothing starts any more; an ACCEPT (an answer to a
    // request sent before the stop) is ended at once, a REQUEST denied
    if (g_stop.load() || !g_on.load()) {
        if (++g_bad_rx <= 10)
            Log("[ride] %s for ride #%08X after riding together stopped: %s", rwire::MsgTypeName(static_cast<std::uint8_t>(m.type)), m.ride_id,
                m.type == rwire::MsgType::accept ? "ended at once" : m.type == rwire::MsgType::request ? "denied" : "dropped");
        if (m.type == rwire::MsgType::accept) Send(rwire::MsgType::end, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(rwire::End::stopped));
        else if (m.type == rwire::MsgType::request) Send(rwire::MsgType::deny, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(rwire::Deny::off));
        return;
    }
    // another world's REQUEST / ACCEPT never starts a ride (a REQUEST is denied, an ACCEPT ended); a DENY or END is the
    // answer to a ride of ours and carries the sender's world, which differs exactly then: it passes (it acts only on its
    // own ride id)
    if (m.world != OurWorld() && (m.type == rwire::MsgType::request || m.type == rwire::MsgType::accept)) {
        if (++g_bad_rx <= 10) Log("[ride] dropped a %s for ride #%08X: another world (%u, ours %u)", rwire::MsgTypeName(static_cast<std::uint8_t>(m.type)), m.ride_id,
                                  m.world, OurWorld());
        if (m.type == rwire::MsgType::request) {
            Send(rwire::MsgType::deny, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(rwire::Deny::world));
        } else {
            if (g_p.st == PState::requesting && g_p.ride_id == m.ride_id) {
                g_p = Pass{};
                g_cooldown_until = g_now + 1500;
            }
            Send(rwire::MsgType::end, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(rwire::End::world));
        }
        return;
    }
    switch (m.type) {
        case rwire::MsgType::request: OnRequest(m); break;
        case rwire::MsgType::accept: {
            if (g_p.st != PState::requesting || g_p.ride_id != m.ride_id || g_p.key.id != m.vehicle_id) {
                Log("[ride] an ACCEPT for ride #%08X we are not asking for: ended at once", m.ride_id);
                Send(rwire::MsgType::end, m.ride_id, m.vehicle_id, m.kind, static_cast<std::uint8_t>(rwire::End::timeout));
                break;
            }
            const BoardView b = ViewBoard(g_p.native);
            const bool still = b.v.key.id == g_p.key.id && OnFoot() &&
                               (g_p.resume || b.dist <= rwire::kBoardRadiusM + 1.5 || (g_p.native && b.origin_dist <= rwire::kNativeReachM + 1.5));
            const bool was_resume = g_p.resume;

            Log("[ride] the driver accepted ride #%08X after %.0f ms (%.2f m from the door)", m.ride_id, g_now - g_p.req_ms, b.dist);
            const vwire::VKey key = g_p.key;
            const std::uint32_t id = g_p.ride_id;
            g_p = Pass{};
            if (!still || !StartCarry(key, key.kind, b.v.entity, b.v.unit, id, false)) {
                Log("[ride] ride #%08X not started: %s", id, still ? "the carry did not start" : "we are no longer at the vehicle");
                Send(rwire::MsgType::end, id, key.id, key.kind, static_cast<std::uint8_t>(rwire::End::timeout));
                g_cooldown_until = g_now + 1500;
            } else if (was_resume) {
                ++g_resumes;
                Log("[ride] %s: our Sam back on the %s's seat after the save (ride #%08X, #%u)", g_role == 1 ? "host" : "guest", vwire::KindName(key.kind), id,
                    g_resumes);
            }
            break;
        }
        case rwire::MsgType::deny: {
            if (g_p.st != PState::requesting || g_p.ride_id != m.ride_id) break;
            ++g_denied;
            Log("[ride] the driver denied ride #%08X: %s", m.ride_id, rwire::DenyName(m.reason));
            char text[160];
            std::snprintf(text, sizeof(text), "Cannot ride: %s.", rwire::DenyName(m.reason));
            Notice(text, true, 3000);
            g_p = Pass{};
            g_cooldown_until = g_now + 1500;
            break;
        }
        case rwire::MsgType::end: {
            ++g_ends_rx;
            if (g_d.accepted && g_d.ride_id == m.ride_id) {
                Log("[ride] the partner ended ride #%08X: %s", m.ride_id, rwire::EndName(m.reason));
                DriverEnd(static_cast<rwire::End>(m.reason), false);
            }
            if (g_p.st == PState::riding && g_p.ride_id == m.ride_id) {
                g_p.driver_ended = true;
                g_p.driver_reason = m.reason;
            }
            if (g_p.st == PState::requesting && g_p.ride_id == m.ride_id) g_p = Pass{};
            break;
        }
    }
}

void RollTick() {
    if (!g_roll_left) return;
    vdrive::DrivenView d;
    WorldTransform xf{};
    if (!vdrive::LocalDriving(&d) || !ReadXf(d.entity, xf)) {
        g_roll_left = 0;
        Log("[ride] test: roll stopped (our Sam no longer drives)");
        return;
    }
    const double a = 4.0 * 3.141592653589793 / 180.0, c = std::cos(a), sn = std::sin(a);
    // the pivot 1 m above the origin along up; rotate right and up about forward
    double piv[3];
    for (int i = 0; i < 3; ++i) piv[i] = (i == 0 ? xf.pos.x : i == 1 ? xf.pos.y : xf.pos.z) + xf.rot.m[2][i];
    WorldTransform t = xf;
    for (int i = 0; i < 3; ++i) {
        t.rot.m[0][i] = static_cast<float>(c * xf.rot.m[0][i] + sn * xf.rot.m[2][i]);
        t.rot.m[2][i] = static_cast<float>(-sn * xf.rot.m[0][i] + c * xf.rot.m[2][i]);
    }
    t.pos.x = piv[0] - t.rot.m[2][0], t.pos.y = piv[1] - t.rot.m[2][1], t.pos.z = piv[2] - t.rot.m[2][2];
    vshare::RequestTeleport(d.entity, t);
    if (--g_roll_left == 0) Log("[ride] test: roll done (up z %.2f)", t.rot.m[2][2]);
}

// Every tick: our own Sam's calm driving (the seated idle's base from our own pose), the hands' travel (both seats: the
// passenger's hands still, the driver's on the wheel) and the camera meter.
struct OwnDrive {
    std::uint32_t id = 0;
    bool have_yaw = false;
    double yaw = 0, yaw_ms = 0, yaw_rate = 0, calm_since = 0;
};
OwnDrive g_own;
double g_hand_log_ms = 0;
void MeterTick() {
    const bool carried = g_p.st == PState::riding;
    vdrive::DrivenView v;
    const bool driving = !carried && vdrive::LocalDriving(&v);
    WorldTransform xf{};
    if (driving && ReadXf(v.entity, xf)) {
        if (g_own.id != v.key.id) g_own = OwnDrive{}, g_own.id = v.key.id;
        const double yaw = std::atan2(xf.rot.m[1][1], xf.rot.m[1][0]);
        if (g_own.have_yaw && g_now > g_own.yaw_ms) {
            const double dt = (g_now - g_own.yaw_ms) / 1000.0;
            const double rate = std::fabs(vwire::WrapPi(yaw - g_own.yaw)) / dt * 57.29577951308232;
            g_own.yaw_rate += ((std::min)(rate, 99.0) - g_own.yaw_rate) * (1.0 - std::exp(-dt / 0.25));
        }
        g_own.have_yaw = true, g_own.yaw = yaw, g_own.yaw_ms = g_now;
        const double speed = v.speed_mps;
        if (!(speed >= 3.0 && speed <= 30.0 && g_own.yaw_rate < 3.0)) g_own.calm_since = 0;
        else if (g_own.calm_since == 0) g_own.calm_since = g_now;
        realbody::SetOwnDriving(v.key.kind, g_own.calm_since > 0 && g_now - g_own.calm_since >= 500 ? speed : 0.0);
    } else {
        if (g_own.id) realbody::SetOwnDriving(0, 0);
        g_own = OwnDrive{};
    }
    const bool meter = carried || driving;
    realbody::SetHandMeter(meter);
    if (!meter) {
        g_hand_log_ms = g_now;
    } else if (g_now - g_hand_log_ms >= 10000) {
        g_hand_log_ms = g_now;
        float r = 0, l = 0;
        unsigned n = 0;
        if (realbody::TakeHandTravel(&r, &l, &n))
            Log("[ride] hands (%s, %s): travel in Sam's model space over 10 s right %.1f cm left %.1f cm (%u updates)", carried ? "passenger" : "driver",
                vwire::KindName(carried ? g_p.kind : v.key.kind), r, l, n);
    }
    if (carried) ridecam::Meter(g_in.local, g_p.entity, g_p.kind, true, g_now);
    else if (driving) ridecam::Meter(g_in.local, v.entity, v.key.kind, false, g_now);
    else ridecam::Meter(nullptr, 0, 0, false, g_now);
}

void TickBody(const TickIn& in, double now) {
    g_in = in;
    g_diag_sam.store(reinterpret_cast<std::uintptr_t>(in.local), std::memory_order_relaxed);
    if (g_wp_armed || g_wp_n.load()) WpTick(now);
    if (g_hb_n.load() != g_hb_logged.load() || g_health_own.load(std::memory_order_relaxed) != g_health_own_logged) HealthGuardTick();
    if (g_last_tick > 0 && now > g_last_tick) g_dt_ms = g_dt_ms * 0.9 + (std::min)(100.0, now - g_last_tick) * 0.1;
    g_last_tick = now;
    g_now = now;
    BootsMeterTick(reinterpret_cast<std::uintptr_t>(in.local), now);
    PendingPlaceTick();
    const std::uint32_t gen = link::Generation();
    if (gen != g_gen) {
        if (g_gen != 0 && (g_p.st == PState::riding || g_d.accepted)) Log("[ride] a new link session: the ride ends");
        g_gen = gen;
        if (!g_p.solo) EndCarry(rwire::End::link, true, false);
        if (g_p.st == PState::requesting) g_p = Pass{};
        DriverEnd(rwire::End::driver_ended, false);
    }
    PassengerTick();
    DriverTick();
    RollTick();
    MeterTick();
}
bool TickGuarded(const TickIn& in, double now) noexcept {
    __try {
        TickBody(in, now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RestoreAfterStop() {
    if (g_p.st == PState::requesting) {
        // a request in flight: asked no more (a late ACCEPT is ended by OnMsg)
        Log("[ride] the mod stopped while asking to ride (#%08X): the request is withdrawn", g_p.ride_id);
        Send(rwire::MsgType::end, g_p.ride_id, g_p.key.id, g_p.kind, static_cast<std::uint8_t>(rwire::End::stopped));
        g_p = Pass{};
        return;
    }
    if (g_p.st != PState::riding) return;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
    if (sam && sam == g_p.sam) PlaceBeside(true, !g_p.tp_fault);
    CapsuleBack("the mod stopped");
    Log("[ride] the mod stopped while our Sam was carried: input and pose back, capsule %s", sam == g_p.sam ? "back on, put beside the vehicle" : "with the old Sam");
    g_p = Pass{};
    PublishSaveState();
}
bool RestoreGuarded() noexcept {
    __try {
        RestoreAfterStop();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

// ---------------------------------------------------------------- public
void Install(const Config& config) noexcept {
    g_cfg_on = config.ride_together;
    g_pose_mode = config.ride_pose;
    ridecam::Install(config);
    InstallPack(config);
    unsigned bad = 0;
    for (const Site& s : kSites) {
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            ++bad;
            Log("[ride] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            if (!g_off[0] || std::strcmp(g_off, "not installed") == 0) std::snprintf(g_off, sizeof(g_off), "this game build differs (%s)", s.name);
        }
    }
    const bool vt_ok = mem::InImage(A(kCapsuleVt)) && mem::InText(mem::Q(A(kCapsuleVt)));
    if (!vt_ok) {
        ++bad;
        Log("[ride] byte check FAILED: the capsule vtable 0x%llX", static_cast<unsigned long long>(kCapsuleVt));
        std::snprintf(g_off, sizeof(g_off), "this game build differs (the capsule vtable)");
    }
    g_checks_ok = bad == 0;
    const unsigned n = static_cast<unsigned>(sizeof(kSites) / sizeof(kSites[0])) + 1;
    if (!g_checks_ok) {
        Log("[ride] riding together OFF: %s (%u of %u checks failed)", g_off, bad, n);
        return;
    }
    if (!g_cfg_on) {
        std::snprintf(g_off, sizeof(g_off), "session.ini ride_together=0");
        Log("[ride] riding together OFF: %s (%u byte checks ok)", g_off, n);
        return;
    }
    if (!vshare::Active() || !vdrive::Active()) {
        std::snprintf(g_off, sizeof(g_off), "%s", !vshare::Active() ? "the real shared vehicles are off" : "remote driving is off");
        Log("[ride] riding together OFF: %s (%u byte checks ok; the solo seat test works)", g_off, n);
        return;
    }
    g_role = vshare::Role();
    if (!vshare::RegisterHandler(rwire::kSvmType, &OnMsg)) {
        std::snprintf(g_off, sizeof(g_off), "channel 3 type %u is taken", static_cast<unsigned>(rwire::kSvmType));
        Log("[ride] riding together OFF: %s", g_off);
        return;
    }
    g_off[0] = 0;
    // the game's pad state (our own reads: getting off, the ImGui fallback); off alone on a mismatch
    unsigned pad_bad = 0;
    for (const Site& s : kPadSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            ++pad_bad;
            Log("[ride] byte check FAILED: %s at 0x%llX: the game's pad state is not read (XInput and the F key only)", s.name, static_cast<unsigned long long>(s.va));
        }
    // the input manager global itself: an instruction that loads it must point at kInputMgr (the code alone could match
    // on a build whose .data moved)
    if (!mem::CodeMatches(A(kInputMgrLoad), "48 8D 0D ?? ?? ?? ??") || mem::RipTarget(A(kInputMgrLoad), 3, 7) != A(kInputMgr)) {
        ++pad_bad;
        Log("[ride] byte check FAILED: the input manager global 0x%llX (lea at 0x%llX): the game's pad state is not read (XInput and the F key "
            "only)",
            static_cast<unsigned long long>(kInputMgr), static_cast<unsigned long long>(kInputMgrLoad));
    }
    g_pad_ok = pad_bad == 0;
    InstallNative();
    InstallShield();
    g_on.store(true);
    if (g_np_hooked.load())
        Log("[ride] riding together ON (%s): %u byte checks; the game's own Use Vehicle on the partner's vehicle boards (a press), the same "
            "button gets off; the game's pad state %s; seats roadster / truck / trike pillion as in ride_wire.cpp",
            g_role == 1 ? "host" : "guest", n, g_pad_ok ? "read" : "OFF (checks)");
    else
        Log("[ride] riding together ON (%s): %u byte checks; hold X (pad) / F (keyboard) 0.6 s beside the partner's vehicle (the native prompt is "
            "off: %s); the game's pad state %s; seats roadster / truck / trike pillion as in ride_wire.cpp",
            g_role == 1 ? "host" : "guest", n, g_np_off, g_pad_ok ? "read" : "OFF (checks)");
}

bool Active() noexcept { return g_on.load(); }
const char* OffReason() noexcept { return g_on.load() ? "" : g_off; }

// Logs every time our Sam sits in a vehicle (any kind; hidden tests wait on it, with the shared vehicles off too).
void SeatWatch(const TickIn& in) noexcept {
    static std::uintptr_t last = 0;
    static unsigned n = 0;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(in.local);
    const std::uintptr_t p = sam && in.local_ok ? mem::Q(sam + kEParent) : 0;
    const std::uintptr_t v = p && KindOfEntity(p) ? p : 0;
    if (v == last) return;
    last = v;
    if (v) Log("[ride] our Sam is seated in the %s (#%u)", vwire::KindName(static_cast<std::uint8_t>(KindOfEntity(v))), ++n);
    else Log("[ride] our Sam is out of the vehicle");
}

static void TickInner(const TickIn& in, double now) noexcept;
void Tick(const TickIn& in, double now) noexcept {
    TickInner(in, now);
    PublishSaveState();
    // the trike driver's back (v0.23): after the driver's side decided this tick
    if (g_pack_ok && !g_stop.load() && !PackTickGuarded(in)) {
        Log("[ride] trike pillion: FAULT in its tick: off for this session");
        g_pack_ok = false;
    }
}
static void TickInner(const TickIn& in, double now) noexcept {
    if (!g_checks_ok) return;
    SeatWatch(in);
    if (g_stop.load()) {
        g_in = in;
        g_now = now;
        AfterStop();
        // a placement that waits (Sam asleep, a save being written) or retries PlaceOn goes on; nothing else
        if (g_place.on && !PendingPlaceGuarded()) Log("[ride] FAULT in the exit's placement after the stop: dropped");
        return;
    }
    if (!g_on.load() && g_p.st != PState::riding && !g_d.accepted) {
        g_in = in;
        g_now = now;
        if (!g_notice_done && g_cfg_on && g_off[0] && coop::Active() && vdrive::Active()) {
            g_notice_done = true;
            char text[220];
            std::snprintf(text, sizeof(text), "Riding together off: %s.", g_off);
            coop::SetVehicleNotice(text, true, now + 60000);
        }
        return;
    }
    static bool shield_notice = false;
    if (!shield_notice && g_on.load() && (!g_shield_hooked.load() || !g_health_hooked.load()) && coop::Active()) {
        shield_notice = true;
        char text[240];
        if (!g_health_hooked.load())
            std::snprintf(text, sizeof(text), "Riding together: the health guard is off (this game build differs): a passenger can be hurt and put off.");
        else
            std::snprintf(text, sizeof(text), "Riding together: the landing shield is off (%s): a passenger can take fall damage getting off.", g_shield_off);
        coop::SetVehicleNotice(text, true, now + 15000);
    }
    static bool np_notice = false;
    if (!np_notice && g_on.load() && !g_np_hooked.load() && coop::Active()) {
        np_notice = true;
        char text[240];
        std::snprintf(text, sizeof(text), "The game's own Use Vehicle is off for riding together (%s): hold X / F beside the partner's vehicle.", g_np_off);
        coop::SetVehicleNotice(text, true, now + 15000);
    }
    if (!TickGuarded(in, now)) {
        Log("[ride] FAULT in the riding tick: stopped (input and pose back, capsule back on)");
        EmergencyStop();
        AfterStop();
    }
}

void NewSession() noexcept {
    // the tick sees the generation change and ends the ride there (game thread)
}

void EmergencyStop() noexcept {
    if (!g_stop.load()) std::snprintf(g_off, sizeof(g_off), "stopped");
    g_on.store(false);
    g_stop.store(true);
    // The native prompt's detours stay installed but act on nothing any more (they also pass everything once g_stop is
    // set): the stopped tick never runs PassengerTick, the only code that rewrites these ids.
    g_np_enabled.store(false);
    g_np_seen.store(0);
    g_np_lift.store(0);
    g_np_ride.store(0);
    overlay::input::SetRideCarry(false);
    realbody::SetRidePose(0);
    realbody::SetHandMeter(false);
    vdrive::SetCarriedUnit(0);
    ridecam::Stop("the mod stopped");
    ShieldAfter();  // the restore puts him beside the vehicle: that drop lands under the shield too
}

void AfterStop() noexcept {
    static std::atomic<bool> done{false};
    if (!g_stop.load() || done.exchange(true)) return;
    if (!RestoreGuarded()) Log("[ride] FAULT while restoring after the stop (the capsule may stay off until a load)");
    if (!PackReleaseGuarded(reinterpret_cast<std::uintptr_t>(g_in.local), "the mod stopped")) Log("[ride] FAULT while showing the driver's backpack again");
    g_d = Drv{};
}

std::uint32_t PoseFlags() noexcept {
    return g_p.st == PState::riding && !g_p.solo ? (wire::kPosePassenger | wire::kPoseInVehicle) : 0u;
}
bool Carried() noexcept { return g_p.st == PState::riding; }
Pillion PillionNow() noexcept {
    if (g_stop.load()) return Pillion::none;
    if (g_p.st == PState::riding && !g_p.solo && g_p.kind == 3) return Pillion::we_ride;
    if (g_d.accepted && g_d.key.kind == 3 && rwire::DriverPlacesBody(true, g_now - g_d.accept_ms, g_now - g_d.last_bit_ms)) return Pillion::we_drive;
    return Pillion::none;
}
bool PackHidden() noexcept { return g_pack.held; }
bool ShieldCovers(std::uintptr_t entity) noexcept { return ShieldActiveFor(entity); }
void AllowOwnHit(float amount) noexcept { AllowOwnHitImpl(amount); }
bool JustRode(std::uintptr_t vehicle_entity) noexcept {
    return vehicle_entity && vehicle_entity == g_last_ride_entity && g_now - g_last_ride_end_ms < kAfterRideNoHitMs;
}

bool PassengerTarget(game::WorldTransform* target) noexcept {
    if (!target || !g_d.accepted || g_stop.load()) return false;
    if (!rwire::DriverPlacesBody(true, g_now - g_d.accept_ms, g_now - g_d.last_bit_ms)) return false;
    vdrive::DrivenView v;
    if (!vdrive::LocalDriving(&v) || v.key.id != g_d.key.id) return false;
    WorldTransform xf{};
    float seat[3];
    if (!ReadXf(v.entity, xf) || !SeatOffset(v.key.kind, seat)) return false;
    const double lead = (std::min)(0.05, g_dt_ms / 1000.0);
    const float zero[3] = {};
    // the meter: where the real body is now (placed last tick, the vehicle moved by its physics since) against the seat now
    WorldTransform body{};
    const std::uintptr_t be = reinterpret_cast<std::uintptr_t>(realbody::LiveEntity());
    if (be && g_d.placed_logged && ReadXf(be, body)) {
        const double e = Dist3(SeatXf(xf, zero, 0, seat), body);
        if (e < 5.0) {
            g_d.err_sum += e, g_d.err_n++;
            if (e > g_d.err_max) g_d.err_max = e;
        }
    }
    *target = SeatXf(xf, v.meas ? v.vel : zero, v.meas && v.speed_mps > 0.3 ? lead : 0.0, seat);
    if (!g_d.placed_logged) {
        g_d.placed_logged = true;
        Log("[ride] the partner body sits in our %s 0x%06X at the passenger seat (%.2f, %.2f, %.2f)", vwire::KindName(v.key.kind), v.key.id, seat[0], seat[1], seat[2]);
    }
    return true;
}

std::uintptr_t RideVehicle() noexcept {
    if (g_d.accepted) return g_d.entity;
    if (g_p.st == PState::riding) return g_p.entity;
    return 0;
}

void SetCrashHitHandler(CrashHitFn fn) noexcept { g_crash_hit = fn; }

// ---------------------------------------------------------------- debug actions
bool StateCommand(char* msg, std::size_t cap) noexcept {
    std::uint32_t w = 0, ns = 0, rs = 0, fl = 0;
    realbody::RidePoseCounters(&w, &ns, &rs, &fl);
    unsigned long pads = 0, keys = 0, raw = 0;
    overlay::input::RideCounters(&pads, &keys, &raw);
    float s1[3] = {}, s2[3] = {}, s3[3] = {};
    SeatOffset(1, s1), SeatOffset(2, s2), SeatOffset(3, s3);
    char full[1200];
    std::snprintf(full, sizeof(full),
                  "%s%s; passenger %s%s %s 0x%06X ride #%08X; driver %s ride #%08X %s 0x%06X; boards %u exits %u requests %u denied %u; accepts %u denies %u "
                  "ends rx %u tx %u bad %u; pose writes %u nosrc %u restores %u; input pads %lu keys %lu raw %lu; capsule counter %d on %d; seats "
                  "roadster (%.2f, %.2f, %.2f) truck (%.2f, %.2f, %.2f) trike (%.2f, %.2f, %.2f)",
                  g_on.load() ? "ON" : "OFF: ", g_on.load() ? "" : g_off, g_p.st == PState::riding ? "riding" : g_p.st == PState::requesting ? "requesting" : "idle",
                  g_p.solo ? " (solo test)" : "", vwire::KindName(g_p.kind), g_p.key.id, g_p.ride_id, g_d.accepted ? "carrying" : "idle", g_d.ride_id,
                  vwire::KindName(g_d.key.kind), g_d.key.id, g_boards, g_exits, g_requests, g_denied, g_accepts, g_denies, g_ends_rx, g_ends_tx, g_bad_rx, w, ns, rs,
                  pads, keys, raw, CapCounter(SamCapsule(reinterpret_cast<std::uintptr_t>(g_in.local))),
                  CapOnByte(SamCapsule(reinterpret_cast<std::uintptr_t>(g_in.local))), s1[0], s1[1], s1[2], s2[0], s2[1], s2[2], s3[0], s3[1], s3[2]);
    Log("[ride] state: %s", full);
    Log("[ride] state: native prompt %s%s%s; partner vehicle 0x%06X lifted 0x%06X riding 0x%06X; provider passes %u lifts %u get-off %u; presses %u "
        "(refused %u); game pad state %s; obs slot 3 0x%08X slot 6 0x%08X",
        NativeOn() ? "ON" : "OFF", NativeOn() ? "" : ": ", NativeOn() ? "" : (g_np_hooked.load() ? "ride_test 12 0" : g_np_off), g_np_seen.load(), g_np_lift.load(),
        g_np_ride.load(), g_np_icon_calls.load(), g_np_lifts.load(), g_np_offs.load(), g_np_presses.load(), g_np_refused.load(), g_pad_ok ? "read" : "off",
        g_np_obs[0].load(), g_np_obs[1].load());
    {
        std::uint8_t b = 0;
        const std::uintptr_t inv = SamInventory(reinterpret_cast<std::uintptr_t>(g_in.local));
        const bool have = inv && PackBit(inv, 0, &b);
        Log("[ride] state: trike pillion hide %s, held %d (%u hide(s), %u release(s)), test %d, inventory +0x4C7C %s0x%02X", g_pack_ok ? "on" : "OFF",
            g_pack.held ? 1 : 0, g_pack_hides, g_pack_releases, g_pack_test, have ? "" : "unread ", b);
    }
    std::snprintf(msg, cap, "%s", full);
    return true;
}

bool BoardCommand(char* msg, std::size_t cap) noexcept {
    if (!g_on.load()) return std::snprintf(msg, cap, "riding together is off: %s", g_off), false;
    const BoardView b = ViewBoard(false);
    if (b.block != rwire::Block::ok) return std::snprintf(msg, cap, "cannot board: %s (door %.2f m)", rwire::BlockName(b.block), b.dist), false;
    Request(b, rwire::kReqTest, "test");
    return std::snprintf(msg, cap, "asked to ride the %s 0x%06X (ride #%08X)", vwire::KindName(g_p.kind), g_p.key.id, g_p.ride_id), true;
}

bool ExitCommand(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (g_p.st != PState::riding) return std::snprintf(msg, cap, "not riding"), false;
    if (g_p.last_speed >= rwire::kExitMaxSpeed && v[0] == 0) return std::snprintf(msg, cap, "too fast (%.1f m/s)", g_p.last_speed), false;
    EndCarry(v[0] != 0 ? rwire::End::test : rwire::End::choice, true, true);
    return std::snprintf(msg, cap, "off"), true;
}

bool SeatMeasureCommand(const double (&v)[3], char* msg, std::size_t cap) noexcept {
    (void)v;
    game::Entity* local = nullptr;
    if (!game::TryGetLocalPlayer(&local) || !local) return std::snprintf(msg, cap, "no local Sam"), false;
    const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(local);
    std::uintptr_t e = mem::Q(sam + kEParent);
    const bool seated = e && KindOfEntity(e) != 0;
    if (!seated) {
        e = g_meas_e;
        if (!e || KindOfEntity(e) != g_meas_kind || mem::Q(e + kVUnit) != g_meas_unit)
            return std::snprintf(msg, cap, "our Sam drives nothing and no vehicle was measured before"), false;
    }
    const auto kind = static_cast<std::uint8_t>(KindOfEntity(e));
    WorldTransform vx{}, sx{};
    if (!ReadXf(e, vx) || !ReadXf(sam, sx)) return std::snprintf(msg, cap, "transforms unreadable"), false;
    double vp[3], sp[3], root[3];
    PosOf(vx, vp), PosOf(sx, sp);
    rwire::ToLocal(vp, Rows(vx), sp, root);
    // Sam's facing in the vehicle's frame
    const double fx = sx.rot.m[1][0] * vx.rot.m[0][0] + sx.rot.m[1][1] * vx.rot.m[0][1] + sx.rot.m[1][2] * vx.rot.m[0][2];
    const double fy = sx.rot.m[1][0] * vx.rot.m[1][0] + sx.rot.m[1][1] * vx.rot.m[1][1] + sx.rot.m[1][2] * vx.rot.m[1][2];
    const double yaw = std::atan2(fx, fy) * 57.29577951308232;
    double pel[3] = {}, pel_l[3] = {};
    const bool have_pel = realbody::JointWorld(local, 1, pel);
    if (have_pel) rwire::ToLocal(vp, Rows(vx), pel, pel_l);
    // the vehicle's joint 1 (the helpers' parent) and the game's seat helpers through it (static data of the vehicles'
    // SkeletonHelpers: the position of each helper in joint 1's frame)
    float m[4][4] = {};
    unsigned joints = 0;
    char jname[96] = {};
    const bool have_j = realbody::ReadJointModelAny(reinterpret_cast<game::Entity*>(e), 1, &joints, m, jname, sizeof(jname));
    struct H {
        const char* name;
        float p[3];
    };
    H hs[2] = {};
    unsigned nh = 0;
    if (kind == 1) hs[0] = {"HLP_Driver", {-0.3133f, -0.6599f, 0.5601f}}, hs[1] = {"HLP_Passenger", {0.3133f, -0.6599f, 0.5601f}}, nh = 2;
    else if (kind == 2) hs[0] = {"HLP_Driver", {-0.4726f, 0.7203f, 1.7929f}}, hs[1] = {"HLP_Passenger", {0.4726f, 0.7203f, 1.7929f}}, nh = 2;
    else if (kind == 3) hs[0] = {"HLP_C_Driver", {0.0f, -0.4979f, 0.3360f}}, nh = 1;
    double hl[2][3] = {};
    char hbuf[400] = {};
    std::size_t hn = 0;
    for (unsigned i = 0; i < nh && have_j; ++i) {
        for (int k = 0; k < 3; ++k) hl[i][k] = m[3][k] + hs[i].p[0] * m[0][k] + hs[i].p[1] * m[1][k] + hs[i].p[2] * m[2][k];
        hn += std::snprintf(hbuf + hn, sizeof(hbuf) - hn, "%s%s (%.3f, %.3f, %.3f)", i ? ", " : "", hs[i].name, hl[i][0], hl[i][1], hl[i][2]);
    }
    float seat[3] = {};
    SeatOffset(kind, seat);
    char derived[120] = "-";
    if (nh == 2 && have_j && seated)
        std::snprintf(derived, sizeof(derived), "(%.3f, %.3f, %.3f)", root[0] + hl[1][0] - hl[0][0], root[1] + hl[1][1] - hl[0][1], root[2] + hl[1][2] - hl[0][2]);
    g_meas_e = e, g_meas_unit = mem::Q(e + kVUnit), g_meas_kind = kind;
    const bool snap = seated && realbody::SnapshotRidePose(kind);
    char full[1200];
    std::snprintf(full, sizeof(full),
                  "%s %s: Sam root (%.3f, %.3f, %.3f) yaw %.1f deg, pelvis %s(%.3f, %.3f, %.3f); joint 1 '%s' of %u at (%.3f, %.3f, %.3f) axes x (%.2f, %.2f, %.2f) "
                  "y (%.2f, %.2f, %.2f); helpers: %s; passenger from the helpers %s; seat in use (%.2f, %.2f, %.2f); pose kept %s",
                  vwire::KindName(kind), seated ? "seated" : "on foot (the last vehicle)", root[0], root[1], root[2], yaw, have_pel ? "" : "unread ", pel_l[0],
                  pel_l[1], pel_l[2], jname, joints, m[3][0], m[3][1], m[3][2], m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], hbuf[0] ? hbuf : "-", derived,
                  seat[0], seat[1], seat[2], snap ? "yes" : "no");
    Log("[ride] seat measure: %s", full);
    std::snprintf(msg, cap, "%s", full);
    return true;
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    switch (op) {
        case 0: return StateCommand(msg, cap);
        case 1: {
            // solo seat check: our Sam carried on the measured vehicle with the kept driver pose
            if (!g_checks_ok) return std::snprintf(msg, cap, "byte checks failed: %s", g_off), false;
            if (g_p.st != PState::idle) return std::snprintf(msg, cap, "busy"), false;
            if (!g_meas_e || KindOfEntity(g_meas_e) != g_meas_kind || mem::Q(g_meas_e + kVUnit) != g_meas_unit)
                return std::snprintf(msg, cap, "measure a vehicle first (seat_measure while driving)"), false;
            if (!OnFoot()) return std::snprintf(msg, cap, "our Sam is not on foot"), false;
            vwire::VKey key;
            std::uint32_t id = 0;
            mem::Rd(g_meas_unit + 0x08, id);
            key.id = id, key.kind = g_meas_kind;
            if (!StartCarry(key, g_meas_kind, g_meas_e, g_meas_unit, 0x5E57u, true)) return std::snprintf(msg, cap, "the carry did not start"), false;
            return std::snprintf(msg, cap, "carried on the %s (solo seat test)", vwire::KindName(g_meas_kind)), true;
        }
        case 2:
            if (g_p.st != PState::riding) return std::snprintf(msg, cap, "not riding"), false;
            EndCarry(rwire::End::test, true, true);
            return std::snprintf(msg, cap, "off"), true;
        case 4:
            if (g_p.st != PState::riding) return std::snprintf(msg, cap, "not riding"), false;
            g_p.test_crash_dv = v[1] > 0 ? v[1] : 12.0;
            return std::snprintf(msg, cap, "a crash of %.1f m/s next tick", g_p.test_crash_dv), true;
        case 5:
        case 6: {
            vdrive::DrivenView d;
            if (!vdrive::LocalDriving(&d)) return std::snprintf(msg, cap, "our Sam drives nothing shared"), false;
            WorldTransform xf{};
            if (!ReadXf(d.entity, xf)) return std::snprintf(msg, cap, "unreadable"), false;
            if (op == 5) {
                if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
                g_roll_left = 45;  // 180 deg over 45 ticks (~1.5 s): no velocity jump over 8 m/s in the frames
                Log("[ride] test: rolling our %s 0x%06X over (45 x 4 deg)", vwire::KindName(d.key.kind), d.key.id);
                return std::snprintf(msg, cap, "rolling over"), true;
            }
            const bool ok = vshare::RequestTeleport(d.entity, xf);
            Log("[ride] test: stopped dead our %s 0x%06X (RequestTeleport %s, %.1f m/s)", vwire::KindName(d.key.kind), d.key.id, ok ? "requested" : "FAILED",
                d.speed_mps);
            return std::snprintf(msg, cap, "stop: %s", ok ? "requested" : "failed"), ok;
        }
        case 7:
            if (g_p.st != PState::riding) return std::snprintf(msg, cap, "not riding"), false;
            g_p.test_taken = true;
            return std::snprintf(msg, cap, "taken next tick"), true;
        case 8: {
            // hidden tests: our Sam at the passenger door point (v[1] m: 0 or 1 = the point, more = further out) of the vehicle the partner drives here, facing it
            vdrive::DrivenView d;
            if (!vdrive::PartnerDriving(&d)) return std::snprintf(msg, cap, "the partner drives nothing we follow"), false;
            WorldTransform xf{};
            float door[3];
            if (!ReadXf(d.entity, xf) || !rwire::DoorPoint(d.key.kind, door)) return std::snprintf(msg, cap, "unreadable"), false;
            door[0] += static_cast<float>((v[1] > 0 ? v[1] : 1.0) - 1.0);  // 1 = the door point itself
            double p[3], w[3];
            PosOf(xf, p);
            rwire::ToWorld(p, Rows(xf), door, w);
            const double fx = -xf.rot.m[0][0], fy = -xf.rot.m[0][1];
            char why[160] = {};
            const bool ok = game::TeleportLocalPlayerFacing(w[0], w[1], w[2] + 0.5, fx, fy, game::TeleportMode::place, why, sizeof(why));
            return std::snprintf(msg, cap, "at the %s's passenger door: %s", vwire::KindName(d.key.kind), why), ok;
        }
        case 9:
            // hidden tests: the module's own emergency stop (what a fault of the per-frame tick does): the input block and the
            // pose override go now; the next tick puts our Sam beside the vehicle and turns his capsule back on. Riding stays off.
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            Log("[ride] test: emergency stop");
            EmergencyStop();
            return std::snprintf(msg, cap, "emergency stop: restored on the next tick"), true;
        case 10: {
            // hidden tests: ask to ride (as "Ride board") and stop the module in the same call, so the driver's ACCEPT
            // always arrives after the stop (the late ACCEPT of a fault between the request and the answer)
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            if (!g_on.load()) return std::snprintf(msg, cap, "riding together is off: %s", g_off), false;
            const BoardView b = ViewBoard(false);
            if (b.block != rwire::Block::ok) return std::snprintf(msg, cap, "cannot board: %s (door %.2f m)", rwire::BlockName(b.block), b.dist), false;
            Request(b, rwire::kReqTest, "test");
            Log("[ride] test: emergency stop right after the request");
            EmergencyStop();
            return std::snprintf(msg, cap, "asked to ride (ride #%08X), then the emergency stop", g_p.ride_id), true;
        }
        case 12:
            // hidden tests: the game's own Use Vehicle off (0: the ImGui hold prompt, as when the native checks fail) or back on (1)
            if (!g_np_hooked.load()) return std::snprintf(msg, cap, "the native prompt is not installed: %s", g_np_off), false;
            g_np_enabled.store(v[1] != 0);
            g_np_lift.store(0);
            Log("[ride] test: the native Use Vehicle prompt %s", v[1] != 0 ? "ON" : "OFF (the ImGui hold prompt boards)");
            return std::snprintf(msg, cap, "native prompt %s", v[1] != 0 ? "on" : "off"), true;
        case 11: {

            // hidden tests: the boarding check now, logged (the action succeeds whatever it says: a scenario waits on the line)
            const BoardView b = ViewBoard(NativeOn());
            Log("[ride] test: boarding now: %s (door %.2f m)", rwire::BlockName(b.block), b.dist);
            return std::snprintf(msg, cap, "boarding now: %s (door %.2f m)", rwire::BlockName(b.block), b.dist), true;
        }
        case 31:
        case 32:
        case 33: {
            const int k = g_meas_kind ? g_meas_kind : (g_p.kind ? g_p.kind : 2);
            if (!g_seat_override[k]) {
                rwire::PassengerSeat(static_cast<std::uint8_t>(k), g_seat_ov[k]);
                g_seat_override[k] = true;
            }
            g_seat_ov[k][op - 31] = static_cast<float>(v[1]);
            Log("[ride] test: the %s's passenger seat now (%.2f, %.2f, %.2f)", vwire::KindName(static_cast<std::uint8_t>(k)), g_seat_ov[k][0], g_seat_ov[k][1],
                g_seat_ov[k][2]);
            return std::snprintf(msg, cap, "%s seat (%.2f, %.2f, %.2f)", vwire::KindName(static_cast<std::uint8_t>(k)), g_seat_ov[k][0], g_seat_ov[k][1], g_seat_ov[k][2]),
                   true;
        }
        case 30:
            for (bool& b : g_seat_override) b = false;
            return std::snprintf(msg, cap, "seat overrides off"), true;
        case 15: {
            // the landing shield and our Sam's health (logged)
            game::Diagnostics d;
            game::ReadDiagnostics(&d);
            const std::uint64_t until = g_shield_until.load();
            const std::uint64_t now64 = GetTickCount64();
            Log("[ride] landing shield: %s%s, %s; %u skipped since the last line; our Sam's health %.1f; crash rule %s; cab crashes kept %u, candidates "
                "rejected %u",
                g_shield_hooked.load() ? "installed" : "NOT installed: ", g_shield_hooked.load() ? "" : g_shield_off,
                !g_shield_enabled.load() ? "off (ride_test 16 0)"
                : !g_shield_sam.load()   ? "idle"
                : until == 0             ? "on (carried)"
                : now64 < until          ? "on (after an exit)"
                                         : "idle",
                g_shield_skips.load(), d.health_valid ? d.health : -1.0f, g_crash_legacy.load() ? "v0.20 (one frame)" : "v0.21 (confirmed)", g_p.cab_crashes,
                g_p.crash_rejects);
            return std::snprintf(msg, cap, "shield %s, health %.1f", g_shield_hooked.load() ? "installed" : "off", d.health_valid ? d.health : -1.0f), true;
        }
        case 16:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            g_shield_enabled.store(v[1] != 0);
            Log("[ride] test: the landing shield %s", v[1] != 0 ? "ON" : "OFF (v0.20: the game's landing check runs on a carried Sam)");
            return std::snprintf(msg, cap, "landing shield %s", v[1] != 0 ? "on" : "off"), true;
        case 34: {
            // the boots: 34 1 / 0 the meter (a line a second), 34 2 a mark (the life and its change since the last mark)
            const std::uintptr_t sam = reinterpret_cast<std::uintptr_t>(g_in.local);
            if (static_cast<int>(v[1]) == 2) return BootsMark(sam, g_now, msg, cap);
            g_boots_meter = v[1] != 0;
            g_boots_meter_last = -1;
            g_boots_meter_ms = 0;
            Log("[ride] test: the boots meter %s (boots guard %s)", g_boots_meter ? "ON" : "off",
                !g_boots_hooked.load() ? "not installed" : g_boots_enabled.load() ? "on" : "OFF");
            return std::snprintf(msg, cap, "boots meter %s", g_boots_meter ? "on" : "off"), true;
        }
        case 36: {
            // v0.23: the trike driver's back hide forced on (1) / off (0) / back to following the ride (-1), hidden tests
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            if (!g_pack_ok) return std::snprintf(msg, cap, "the trike pillion hide is off (see the log)"), false;
            const int want = v[1] > 0.5 ? 1 : v[1] < -0.5 ? -1 : 0;
            g_pack_test = want;
            std::uint8_t b = 0;
            const std::uintptr_t inv = SamInventory(reinterpret_cast<std::uintptr_t>(g_in.local));
            const bool have = inv && PackBit(inv, 0, &b);
            Log("[ride] test: the trike pillion hide %s (held %d, inventory +0x4C7C %s0x%02X)", want == 1 ? "forced ON" : want == 0 ? "forced off" : "follows the ride",
                g_pack.held ? 1 : 0, have ? "" : "unread ", b);
            return std::snprintf(msg, cap, "pillion hide %s", want == 1 ? "on" : want == 0 ? "off" : "auto"), true;
        }
        case 35:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            if (!g_boots_hooked.load()) return std::snprintf(msg, cap, "the boots guard is not installed"), false;
            g_boots_enabled.store(v[1] != 0);
            Log("[ride] test: the boots guard %s", v[1] != 0 ? "ON" : "OFF (v0.21: a carried Sam's boots wear)");
            return std::snprintf(msg, cap, "boots guard %s", v[1] != 0 ? "on" : "off"), true;
        case 18:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            g_wp_enabled = v[1] != 0;
            return std::snprintf(msg, cap, "health watchpoint on carry %s", g_wp_enabled ? "on" : "off"), true;
        case 20:
        case 21:
        case 25:
        case 27:
        case 28: return ridecam::Command(op, v, msg, cap);
        case 22: {
            // the passenger's pose: 0 v0.20's copy of the driver's live pose, 1 the seated idle (now, if riding)
            g_pose_mode = v[1] != 0 ? 1 : 0;
            if (g_p.st == PState::riding) realbody::SetRidePose(CarryPoseSource());
            Log("[ride] test: the passenger's pose %s", g_pose_mode ? "the seated idle" : "v0.20 (the driver's live pose)");
            return std::snprintf(msg, cap, "passenger pose %s", g_pose_mode ? "seated idle" : "the driver's (v0.20)"), true;
        }
        case 23:
            // the seated idle's tuning: 23 <what> <value> (0 arms 0/1, 1 along, 2 up m, 3 in m, 4 twist deg)
            realbody::RidePoseTune(static_cast<int>(v[1]), v[2]);
            PoseLog(true);
            return std::snprintf(msg, cap, "pose tune %d = %.3f", static_cast<int>(v[1]), v[2]), true;
        case 26:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            g_test_hold_tp = v[1] != 0;
            Log("[ride] test: the seat teleport %s while the vehicle stands", g_test_hold_tp ? "HELD" : "on");
            return std::snprintf(msg, cap, "teleport hold %d", g_test_hold_tp ? 1 : 0), true;
        case 24:
            PoseLog(true);
            return std::snprintf(msg, cap, "pose state logged"), true;
        case 29:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            g_resume_enabled = v[1] != 0;
            Log("[ride] test: the seat back after a save %s", g_resume_enabled ? "ON" : "OFF (v0.20: the save's exit leaves our Sam on foot)");
            return std::snprintf(msg, cap, "seat back after a save %s", g_resume_enabled ? "on" : "off"), true;
        case 17:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden tests only"), false;
            g_crash_legacy.store(v[1] == 0);
            Log("[ride] test: the crash rule %s", v[1] == 0 ? "v0.20 (one frame over 8 m/s: exit and hit, every kind)" : "v0.21 (confirmed; the cab keeps its passenger)");
            return std::snprintf(msg, cap, "crash rule %s", v[1] == 0 ? "v0.20" : "v0.21"), true;
        default: return std::snprintf(msg, cap, "unknown op %d", op), false;
    }
}

}  // namespace st::ride
