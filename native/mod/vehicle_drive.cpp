// Shared real vehicles, stage 2: remote driving, ownership and state (design M2.6 - M2.9). See vehicle_drive.hpp and
// docs/real-partner-body.md sections 23.11 - 23.17.
#include "vehicle_drive.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include "automation.hpp"
#include "coop_session.hpp"
#include "game_actions.hpp"
#include "link.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "vehicle_share.hpp"
#include "vehicle_sync.hpp"

namespace st::vdrive {
namespace {
namespace mem = partner::mem;
using game::WorldTransform;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut)
constexpr std::uintptr_t kCtlCtor = 0x142F51C60;     // VehicleDriveCtl ctor (ctl)
constexpr std::uintptr_t kCtlAttach = 0x142F5A590;   // Attach (ctl, vehicle, u32* driver id) -> bool
constexpr std::uintptr_t kCtlEngine = 0x142F5A310;   // SetEngine (ctl, bool)
constexpr std::uintptr_t kCtlDetach = 0x142F5A3D0;   // Detach (ctl)
constexpr std::uintptr_t kCtlDtor = 0x142F521F0;     // dtor (ctl)
constexpr std::uintptr_t kInReset = 0x142F54440;     // the input object's vt+0x08 (Reset + route follower, every update)
constexpr std::uintptr_t kInVt = 0x143E229B8;        // the input object's vtable
constexpr std::uintptr_t kSetDriver = 0x142F7C4C0;   // vehicle vt+0x110 (speed car, motorbike)
constexpr std::uintptr_t kSetDriverTruck = 0x142F7C310;
constexpr std::uintptr_t kVehVt[4] = {0, 0x143E274D8, 0x143E26790, 0x143E25DB8};  // roadster, truck, trike entities
constexpr std::uintptr_t kRideOff = 0x1426E7100;     // RideOffVehicleForcibly: [[0x147BE7980]+0x206D1] |= 8
constexpr std::uintptr_t kRideOffGlobal = 0x147BE7980;
constexpr std::uintptr_t kTouch = 0x142F52740;       // RecentVehicles::Touch (R+0x58, unit, 0)
constexpr std::uintptr_t kRegGlobal = 0x147BFECC0;   // R (vehicle manager; vehicle_share checks its RIP)
constexpr std::uintptr_t kNetCorrect = 0x1423B80E0;  // the remote-vehicle net correction
constexpr std::uintptr_t kFirstInst = 0x14234DCD0;   // entity -> first physics instance (entity, int* idx)
constexpr std::uintptr_t kMatInverse = 0x141940080;  // Mat44 inverse (in, out)
constexpr std::uintptr_t kWtToMat = 0x1419C8510;     // WorldTransform -> Mat44 (our MatFromWt replicates it: checked)
constexpr std::uintptr_t kMatToWt = 0x1419C7000;     // Mat44 -> WorldTransform (out, in)
constexpr std::uintptr_t kMatMul = 0x1419333D0;      // Mat44 multiply (a, out, b)
constexpr std::uintptr_t kDriveTo = 0x141C43E10;     // physics instance drive-to-transform (inst, WorldTransform*, t, 0)
constexpr std::uintptr_t kCompLookup = 0x1422B2A60;  // entity component lookup (entity+0x90, key) -> component or 0
constexpr std::uintptr_t kDriveCompKey = 0x144B35D30; // the key Attach looks up (the vehicle's AI drive component)
constexpr std::uintptr_t kHandleAddRef = 0x14191C790; // vehicle handle addref (handle*)
constexpr std::uintptr_t kHandleRelease = 0x14191C810; // vehicle handle release (handle*)
constexpr std::uintptr_t kSetController = 0x142F80310; // SetController (vehicle, input object)

// vehicle entity fields
constexpr std::uintptr_t kEParent = 0x70;            // Entity: parent (Sam's vehicle while riding)
constexpr std::uintptr_t kEPos = 0xC8;
constexpr std::uintptr_t kEParts = 0xB8;             // the physics part table (+0x58 count, +0x60 parts of 0x100)
constexpr std::uintptr_t kEDt = 0x200;               // f32 the entity's step (the remote step passes 3x it)
constexpr std::uintptr_t kVHavok = 0x330;            // [veh+0x330]+0xF0 = speed km/h
constexpr std::uintptr_t kVRideState = 0x4CC;
constexpr std::uintptr_t kVInput = 0x528;            // the input object the vehicle reads
constexpr std::uintptr_t kVFlags560 = 0x560;         // 0x2000 broken
constexpr std::uintptr_t kVReq564 = 0x564;           // request bits (bit 21 explode)
constexpr std::uintptr_t kVBattery = 0x5A4;          // f32 battery (the state machine: <= 0 parked -> 6)
constexpr std::uintptr_t kVHasDriver = 0x63C;
constexpr std::uintptr_t kVLife = 0x6E8, kVLifeMax = 0x6EC;
constexpr std::uintptr_t kVUnit = 0x8A8;
constexpr std::uintptr_t kVDriver = 0x8B0;           // the driver's game-actor id (0x1FFFFF: NPC / ours)
constexpr std::uintptr_t kVLatch = 0xBD8;            // 0x100000: exploded (latched by the state machine)
constexpr std::uint32_t kNoActor = 0x1FFFFF;
constexpr std::uintptr_t kUEntRef = 0x68;            // unit: entity + 0x20

enum Group : unsigned {
    kGCtl = 1u,      // the NPC controller recipe, the input vtable, the seat (lock) (required)
    kGRideOff = 2u,  // the forced ride-off (required: the host wins every tie)
    kGState = 4u,    // life, broken, explode, speed reads (optional: no state copy)
    kGBattery = 8u,  // the battery field (optional)
    kGTouch = 16u,   // the recent-vehicles touch (optional, host)
    kGNative = 32u,  // the native net correction (optional: then the nudge / teleports)
    kGNudge = 64u,   // the clamped velocity nudge (optional)
    kGLite = 128u,   // Attach without its route part, for vehicles without the AI drive component (optional: trikes and
                     // roadsters are then not followed)
    kGNpc = 256u,    // the NPC-driven bits SetDriver sets for the NPC controller and clears on its own get-off (optional: then
                     // left set after a detach, as before)
};
constexpr unsigned kRequired = kGCtl | kGRideOff;
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    {kCtlCtor,    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83", "VehicleDriveCtl ctor", kGCtl},
    {kCtlAttach,  "40 53 55 56 57 41 54 41 57 48 81 EC 38 0C 00 00 48 8B 05 89 6A 00 02 48", "VehicleDriveCtl Attach", kGCtl},
    {kCtlEngine,  "40 53 48 83 EC 40 8B 41 18 0F B6 DA 84 D2 74 10 83 F8 02 0F 85 95 00 00", "VehicleDriveCtl SetEngine", kGCtl},
    {kCtlDetach,  "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 48 8B 01 48", "VehicleDriveCtl Detach", kGCtl},
    {kCtlDtor,    "48 89 5C 24 08 57 48 83 EC 40 33 FF 48 8B D9 48 8B 09 48 85 C9 48 8D 41", "VehicleDriveCtl dtor", kGCtl},
    {kInReset,    "48 89 5C 24 10 57 48 83 EC 40 C5 FA 11 89 00 01 00 00 33 FF C7 41 14 00", "input vt+0x08 reset", kGCtl},
    {0x142F8D920, "40 53 48 83 EC 70 C5 F8 29 74 24 60 48 8B 05 FD", "the vehicle update that resets the inputs", kGCtl},
    {kSetDriver,  "40 53 57 48 83 EC 48 48 89 74 24 70 48 8D 99 88 02 00 00 4C 89 74 24 78", "SetDriver", kGCtl},
    {kSetDriverTruck, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC 40 01 00 00", "truck SetDriver", kGCtl},
    {0x142F7C50F, "83 BF 3C 06 00 00 00 0F 85 CE 02 00 00 83 BF CC 04 00 00 05 0F 84 C1 02 00 00", "SetDriver refuses a held seat (+0x63C) or state 5", kGCtl},
    {0x142F80310, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 99 88 02 00 00 48 8B", "SetController", kGCtl},
    {0x142F6F4A0, "48 83 EC 28 83 B9 3C 06 00 00 00 74 29 48 8B 91 28 05 00 00 48 85 D2 74", "is driven: +0x63C / +0x528", kGCtl},
    {0x142F9BC20, "40 53 48 83 EC 30 83 B9 CC 04 00 00 05 48 8B D9 C5 F8 29 74 24 20 C5 C8", "ride-state machine (+0x4CC)", kGCtl},
    {0x142F9BD76, "F7 83 B0 08 00 00 00 E0 1F 00", "ride state: driver id +0x8B0", kGCtl},
    {kRideOff,    "48 8B 05 ?? ?? ?? ?? 80 88 D1 06 02 00 08 C3", "RideOffVehicleForcibly", kGRideOff},
    {0x142F9BC40, "C5 F8 2F B1 E8 06 00 00", "ride state: life +0x6E8", kGState},
    {0x142F9BC54, "F7 81 60 05 00 00 00 20 00 00", "ride state: broken +0x560 & 0x2000", kGState},
    {0x142F9BC70, "0F BA E0 15 73 62 80 89 DD 0B 00 00 80 0F BA F0 15 81 89 D8 0B 00 00 00 00 10 00", "ride state: explode +0x564 bit 21, latch +0xBD8", kGState},
    {0x142F8AC5D, "48 8B 83 30 03 00 00 C5 FA 10 80 F0 00 00 00", "speed [+0x330]+0xF0", kGState},
    {0x142F9BD50, "C5 F8 2F B3 A4 05 00 00", "ride state: battery +0x5A4", kGBattery},
    {kTouch,      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 41 0F B6 F0 48 8B", "RecentVehicles Touch", kGTouch},
    {kNetCorrect, "40 55 53 56 57 41 56 48 8D 6C 24 F9 48 81 EC 00 01 00 00 48 8B 05 36 8F", "net correction", kGNative},
    {kFirstInst,  "48 8B 89 B8 00 00 00 48 85 C9 0F 85 80 25 04 00 33 C0 C3", "first physics instance", kGNative},
    {kMatInverse, "48 8B C4 48 83 EC 68 C5 F8 10 69 20 C5 F8 10 51 10", "Mat44 inverse", kGNative},
    {kWtToMat,    "8B 42 18 C5 FB 10 02 C5 FB 10 4A 08 C5 FB 10 52 10 89 01 8B 42 24 89 41 10 8B 42 30 89 41 20", "WorldTransform to Mat44 (the layout we build)", kGNative},
    {kDriveTo,    "48 83 EC 38 C5 F8 57 C0 C5 F8 2E D0 C5 F8 28 DA 7A 02 74 4A 80 B9 DB 00", "drive-to-transform", kGNudge},
    {kMatMul,     "4C 8B DC 48 83 EC 78 C4 C1 78 10 28", "Mat44 multiply", kGNudge},
    {kMatToWt,    "48 83 EC 28 C5 F8 10 52 30 C5 E8 C6 C2 55 C5 EA 5A DA 48 8D 41 18 4C 8B C9", "Mat44 to WorldTransform", kGNudge},
    {kFirstInst,  "48 8B 89 B8 00 00 00 48 85 C9", "first physics instance", kGNudge},
    // Attach reads the vehicle's AI drive component and calls into it unchecked: a vehicle without one (the trike, run
    // 20261119-111839: a fault inside Attach, then the game's crash reporter for 44 s) must never reach it. The precheck
    // repeats the lookup (required: without it no vehicle is attached).
    {kCompLookup, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 48 8B 59 18 4C 8B DA 48 63", "component lookup", kGCtl},
    {0x142F5A678, "48 8D 8D 90 00 00 00 48 8D 15 AA B6 BD 01 E8 D5 83 35 FF", "Attach: the drive component (entity+0x90, key 0x144B35D30)", kGCtl},
    {0x14242D660, "48 83 79 78 00 74 05 48 8D 41 50 C3 80 B9 98 00 00 00 00 74 08 48 8B 81", "the drive component's route source (+0x78 / +0x98 / +0x88)", kGCtl},
    // the lite Attach replays Attach's own writes before its route part
    {kHandleAddRef, "40 57 48 83 EC 20 48 8B F9 48 8B 09 48 85 C9 74 58 33 C0 48 89 5C 24 30", "vehicle handle addref", kGLite},
    {kHandleRelease, "40 57 48 83 EC 20 48 8B F9 48 8B 09 48 85 C9 0F 84 8D 00 00 00 33 C0 48", "vehicle handle release", kGLite},
    {0x142F5A624, "48 39 3E 74 13 48 8B CE E8 DF 21 9C FE 48 8B CE 48 89 3E E8 54 21 9C FE", "Attach: the vehicle handle at ctl+0", kGLite},
    {0x142F5A65A, "48 8D 56 30 48 85 C0 48 8D 48 E0 49 0F 44 CC E8 A2 5C 02 00", "Attach: SetController(vehicle, ctl+0x30)", kGLite},
    {0x142F5A68B, "48 8D BD 18 03 00 00 48 89 86 00 01 00 00", "Attach: ctl+0x100 = the drive component", kGLite},
    {0x142F5A6B2, "8B 9D 14 03 00 00 48 85 FF 74 09 48 8B CF FF 15 6A FE BD 00 33 D2 89 5E 1C", "Attach: ctl+0x1C = vehicle+0x314", kGLite},
    {0x142F5A785, "C7 46 18 02 00 00 00 41 8B 07 89 46 20", "Attach: state 2, the driver id at ctl+0x20", kGLite},
    // the vehicle feeds the input object only while its byte +0x38 (ctl+0x68, 1 from the ctor) is 0
    {0x142F8DA01, "83 B9 3C 06 00 00 00 74 79 48 8B 91 28 05 00 00 48 85 D2 74 6D 83 F8 02 75 68 80 7A 38 00 75 4A 48 8B 02 48 8B CA FF 50 08",
     "the vehicle update: inputs read while input+0x38 == 0", kGLite},
    // SetDriver for an NPC driver sets [+0x898]+0x48 |= 2 and +0x408 |= 0x10; its own get-off path clears both (and +0x408's
    // player bit 2). The follower's Detach does not run that path: the mod clears the two NPC bits it caused, as the game would.
    {0x142F7C74A, "48 8B 87 98 08 00 00 83 48 48 02", "SetDriver (NPC): [+0x898]+0x48 |= 2", kGNpc},
    {0x142F7C774, "80 8F 08 04 00 00 10", "SetDriver (NPC): +0x408 |= 0x10", kGNpc},
    {0x142F7C88F, "48 8B 87 98 08 00 00 83 60 48 FD 80 A7 08 04 00 00 ED", "SetDriver (get off): clears [+0x898]+0x48 bit 2 and +0x408 bits 0x12", kGNpc},
};
// clang-format on

using FnCtor = void* (*)(void*);
using FnAttach = std::uint8_t (*)(void*, std::uintptr_t, std::uint32_t*);
using FnEngine = void (*)(void*, std::uint8_t);
using FnVoid1 = void (*)(void*);
using FnVoid0 = void (*)();
using FnTouch = void (*)(std::uintptr_t, std::uintptr_t, std::uint8_t);
using FnInReset = void (*)(std::uintptr_t, float);
using FnFirstInst = std::uintptr_t (*)(std::uintptr_t, int*);
using FnMatInv = void* (*)(const void*, void*);
using FnCorrect = void (*)(std::uintptr_t, const void*, std::uintptr_t, const void*, const void*, float, float, float, float, float, float, char);
using FnMatMul = void* (*)(const void*, void*, const void*);
using FnMatToWt = void* (*)(void*, const void*);
using FnDriveTo = void (*)(std::uintptr_t, const void*, float, char);
using FnActivate = void (*)(std::uintptr_t);
using FnLookup = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t);
using FnSetDriverV = std::uint8_t (*)(std::uintptr_t, std::uint32_t*, std::uint8_t, std::uint8_t);
using FnSetCtl = void (*)(std::uintptr_t, std::uintptr_t);

// ---------------------------------------------------------------- state
std::atomic<bool> g_on{false};
char g_off[200] = "not installed";
unsigned g_groups_ok = 0;
bool g_native_fx = false;      // session.ini vehicle_real_native
bool g_native_on = true;       // debug: native correction allowed
bool g_parked_native = false;  // debug (vehicle_drive 2 x 1): the native correction on parked vehicles too (it did not close
                               // 0.5 -> 1.27 m on a truck summoned into a trike: run 20261119-105428)
bool g_corr_on = true;         // debug: corrections allowed (off: the open-loop replay)
double g_ignore_m = 0.1;       // debug: the ignore band (parked)
double g_ignore_drive_m = 0.3; // debug: the ignore band while the partner drives (the input replay alone stays within ~0.1 m)
bool g_stopped = false;
bool g_detach_after_stop = false;  // the emergency stop left followers attached: they let go on the next game-thread pass
bool g_notice_done = false;
int g_role = 0;                // 1 host, 2 guest
double g_now = 0;
std::uint32_t g_gen = 0;
std::uint32_t g_counter = 0;
bool g_ready = false;
double g_ready_since = 0;
double g_last_tick = 0;
bool g_battery_ok = false;     // the battery probe passed
unsigned g_battery_reads = 0, g_battery_bad = 0;

// ---------------------------------------------------------------- the controller block (the mod's memory)
// 0x2000 bytes from VirtualAlloc: the game's VehicleDriveCtl at +0 (about 0xA50 bytes), our input buffer at +0x1800. The input
// object (ctl+0x30) gets our copy of vtable 0x143E229B8 whose slot +0x08 (InUpdate) calls the game's reset and then copies the
// received inputs over +0x08..+0x37. InUpdate runs wherever the vehicle updates: it reads the buffer through a sequence lock.
constexpr std::size_t kBlock = 0x2000, kInBufOff = 0x1800, kInputOff = 0x30;
constexpr std::uint32_t kInMagic = 0x31445453u;  // "STD1"
struct InBuf {
    std::uint32_t magic;
    volatile LONG seq;
    volatile LONG on;
    volatile LONG calls;
    alignas(8) std::uint8_t data[vwire::kDriveInputBytes];
};
static_assert(kInBufOff + sizeof(InBuf) < kBlock, "input buffer inside the block");
std::uintptr_t g_vt_copy[24] = {};
bool g_vt_ready = false;
volatile LONG g_in_calls_total = 0;

InBuf* BufOf(std::uintptr_t block) noexcept { return reinterpret_cast<InBuf*>(block + kInBufOff); }

void InUpdate(std::uintptr_t in, float dt) {
    reinterpret_cast<FnInReset>(A(kInReset))(in, dt);
    InBuf* b = BufOf(in - kInputOff);
    if (b->magic != kInMagic) return;
    InterlockedIncrement(&b->calls);
    InterlockedIncrement(&g_in_calls_total);
    if (!b->on) return;
    alignas(8) std::uint8_t tmp[vwire::kDriveInputBytes];
    for (int k = 0; k < 4; ++k) {
        const LONG s1 = b->seq;
        if (s1 & 1) {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        std::memcpy(tmp, b->data, sizeof(tmp));
        MemoryBarrier();
        if (b->seq == s1) {
            std::memcpy(reinterpret_cast<void*>(in + 8), tmp, sizeof(tmp));
            return;
        }
    }
}

void WriteInputs(std::uintptr_t block, const std::uint8_t (&in)[vwire::kDriveInputBytes]) noexcept {
    InBuf* b = BufOf(block);
    InterlockedIncrement(&b->seq);
    MemoryBarrier();
    std::memcpy(b->data, in, sizeof(in));
    MemoryBarrier();
    InterlockedIncrement(&b->seq);
    b->on = 1;
}

// The inputs of an idle driver: no throttle, no steering, the defaults, hold on.
void IdleInputs(std::uint8_t (&in)[vwire::kDriveInputBytes]) noexcept {
    std::memset(in, 0, sizeof(in));
    const float one = 1.0f;
    for (int off : {0x14, 0x18, 0x1C, 0x2C}) std::memcpy(in + off - 8, &one, 4);
    in[0x34 - 8] = 1;
}

// ---------------------------------------------------------------- SEH helpers (POD locals only)
int CallCtorAttach(std::uintptr_t block, std::uintptr_t veh) noexcept {
    __try {
        reinterpret_cast<FnCtor>(A(kCtlCtor))(reinterpret_cast<void*>(block));
        std::uint32_t id = kNoActor;
        const std::uint8_t ok = reinterpret_cast<FnAttach>(A(kCtlAttach))(reinterpret_cast<void*>(block), veh, &id);
        if (!ok) return 0;
        reinterpret_cast<FnEngine>(A(kCtlEngine))(reinterpret_cast<void*>(block), 1);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// Attach without its route part (the vehicle has no AI drive component): the writes Attach makes before it (checked bytes,
// kGLite), then SetEngine. The input object's reset returns early with ctl+0x100 (input+0xD0) == 0: nothing reads the route.
int CallCtorAttachLite(std::uintptr_t block, std::uintptr_t veh) noexcept {
    __try {
        reinterpret_cast<FnCtor>(A(kCtlCtor))(reinterpret_cast<void*>(block));
        std::uint32_t id = kNoActor;
        const auto set_driver = reinterpret_cast<FnSetDriverV>(*reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(veh) + 0x110));
        if (!set_driver(veh, &id, 1, 0)) return 0;
        auto* handle = reinterpret_cast<std::uintptr_t*>(block);
        if (*handle != veh + 0x20) {
            reinterpret_cast<FnVoid1>(A(kHandleRelease))(handle);
            *handle = veh + 0x20;
            reinterpret_cast<FnVoid1>(A(kHandleAddRef))(handle);
        }
        reinterpret_cast<FnSetCtl>(A(kSetController))(veh, block + kInputOff);
        *reinterpret_cast<std::uintptr_t*>(block + 0x100) = 0;
        *reinterpret_cast<std::uint32_t*>(block + 0x1C) = *reinterpret_cast<const std::uint32_t*>(veh + 0x314);
        *reinterpret_cast<std::uint32_t*>(block + 0x18) = 2;
        *reinterpret_cast<std::uint32_t*>(block + 0x20) = id;
        // input+0x38: the ctor sets it 1 and the vehicle then never reads the inputs (0x142F8DA1B); after the full Attach it
        // is 0 (the truck's follower got its updates; which call clears it was not traced), the lite-attached trike got none
        // (run 20261119-113138)
        *reinterpret_cast<std::uint8_t*>(block + kInputOff + 0x38) = 0;
        reinterpret_cast<FnEngine>(A(kCtlEngine))(reinterpret_cast<void*>(block), 1);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// The vehicle's AI drive component (what Attach looks up), 0 when it has none; 1 when the lookup faulted.
std::uintptr_t CallLookup(std::uintptr_t veh) noexcept {
    __try {
        return reinterpret_cast<FnLookup>(A(kCompLookup))(veh + 0x90, A(kDriveCompKey));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}
bool CallDetachDtor(std::uintptr_t block) noexcept {
    __try {
        reinterpret_cast<FnEngine>(A(kCtlEngine))(reinterpret_cast<void*>(block), 0);
        reinterpret_cast<FnVoid1>(A(kCtlDetach))(reinterpret_cast<void*>(block));
        reinterpret_cast<FnVoid1>(A(kCtlDtor))(reinterpret_cast<void*>(block));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallDtorOnly(std::uintptr_t block) noexcept {
    __try {
        reinterpret_cast<FnVoid1>(A(kCtlDtor))(reinterpret_cast<void*>(block));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallRideOff() noexcept {
    __try {
        reinterpret_cast<FnVoid0>(A(kRideOff))();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallTouch(std::uintptr_t list, std::uintptr_t unit) noexcept {
    __try {
        reinterpret_cast<FnTouch>(A(kTouch))(list, unit, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::uintptr_t CallFirstInst(std::uintptr_t e, int* idx) noexcept {
    __try {
        return reinterpret_cast<FnFirstInst>(A(kFirstInst))(e, idx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
bool CallInverse(const float* in, float* out) noexcept {
    __try {
        reinterpret_cast<FnMatInv>(A(kMatInverse))(in, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallCorrect(std::uintptr_t e, const float* mat, std::uintptr_t inst, const float* frame, const float* inv, float ign, float imp, float tele, float cos_ign,
                 float cos_imp, float t) noexcept {
    __try {
        reinterpret_cast<FnCorrect>(A(kNetCorrect))(e, mat, inst, frame, inv, ign, imp, tele, cos_ign, cos_imp, t, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallDriveTo(std::uintptr_t inst, const float* target_mat, const float* frame, float t) noexcept {
    __try {
        alignas(16) float body[16];
        alignas(16) std::uint8_t wt[0x40];
        const std::uintptr_t act = mem::Q(mem::Q(inst) + 0xE0);
        if (!act || !mem::InText(act)) return false;
        reinterpret_cast<FnActivate>(act)(inst);
        reinterpret_cast<FnMatMul>(A(kMatMul))(target_mat, body, frame);
        reinterpret_cast<FnMatToWt>(A(kMatToWt))(wt, body);
        reinterpret_cast<FnDriveTo>(A(kDriveTo))(inst, wt, t, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool OrBits(std::uintptr_t a, std::uint32_t bits) noexcept {
    __try {
        _InterlockedOr(reinterpret_cast<volatile long*>(a), static_cast<long>(bits));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool AndBits32(std::uintptr_t a, std::uint32_t mask) noexcept {
    __try {
        _InterlockedAnd(reinterpret_cast<volatile long*>(a), static_cast<long>(mask));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool AndBits8(std::uintptr_t a, std::uint8_t mask) noexcept {
    __try {
        _InterlockedAnd8(reinterpret_cast<volatile char*>(a), static_cast<char>(mask));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool WriteF32(std::uintptr_t a, float v) noexcept {
    __try {
        *reinterpret_cast<volatile float*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SetVtable(std::uintptr_t obj, std::uintptr_t vt) noexcept {
    __try {
        *reinterpret_cast<volatile std::uintptr_t*>(obj) = vt;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- reads
bool ReadXf(std::uintptr_t e, WorldTransform& out) noexcept {
    std::uint8_t b1[0x3C], b2[0x3C];
    if (!e || !mem::Read(e + kEPos, b1, sizeof(b1)) || !mem::Read(e + kEPos, b2, sizeof(b2)) || std::memcmp(b1, b2, sizeof(b1)) != 0) return false;
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
// The entity is still that unit's live vehicle (the poll is 10 Hz: re-checked before every use).
bool EntityLive(std::uintptr_t unit, std::uintptr_t e, std::uint8_t kind) noexcept {
    return unit && e && KindOfEntity(e) == kind && mem::Q(e + kVUnit) == unit && mem::Q(unit + kUEntRef) == e + 0x20;
}
double Dist3(const WorldTransform& a, const WorldTransform& b) noexcept {
    const double x = a.pos.x - b.pos.x, y = a.pos.y - b.pos.y, z = a.pos.z - b.pos.z;
    return std::sqrt(x * x + y * y + z * z);
}
double Dist3(const WorldTransform& a, const double (&b)[3]) noexcept {
    const double x = a.pos.x - b[0], y = a.pos.y - b[1], z = a.pos.z - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
const float (&Rows(const WorldTransform& t))[9] { return *reinterpret_cast<const float(*)[9]>(&t.rot); }
WorldTransform XfOfFrame(const vwire::DriveFrame& f) noexcept {
    WorldTransform t{};
    t.pos.x = f.pos[0], t.pos.y = f.pos[1], t.pos.z = f.pos[2];
    float r[9];
    vwire::RowsFromQuat(f.quat, r);
    std::memcpy(&t.rot, r, sizeof(r));
    return t;
}
// Mat44 as 0x1419C8510 builds it from a WorldTransform: the three rotation rows as they are (out[4] = rot+0x0C, out[8] =
// rot+0x18: not transposed) into the first three rows (w 0), the position as floats in the fourth (w 1). A transposed copy
// drove every corrected vehicle toward the inverse yaw (twice the heading off: 69 deg on the summoned trike and truck, run
// 20261119-104937; the 2..5 m "branch did not move it" of run 20261119-103440).
void MatFromWt(const WorldTransform& t, float (&m)[16]) noexcept {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) m[r * 4 + c] = t.rot.m[r][c];
        m[r * 4 + 3] = 0;
    }
    m[12] = static_cast<float>(t.pos.x), m[13] = static_cast<float>(t.pos.y), m[14] = static_cast<float>(t.pos.z), m[15] = 1.0f;
}

// The interpolated transform between two frames.
WorldTransform Lerp(const vwire::DriveFrame& a, const vwire::DriveFrame& b, double f) noexcept {
    WorldTransform t{};
    t.pos.x = a.pos[0] + (b.pos[0] - a.pos[0]) * f;
    t.pos.y = a.pos[1] + (b.pos[1] - a.pos[1]) * f;
    t.pos.z = a.pos[2] + (b.pos[2] - a.pos[2]) * f;
    float qa[4], qb[4];
    std::memcpy(qa, a.quat, sizeof(qa));
    std::memcpy(qb, b.quat, sizeof(qb));
    if (qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3] < 0)
        for (float& v : qb) v = -v;
    const float u = static_cast<float>(f);
    const float q[4] = {qa[0] + (qb[0] - qa[0]) * u, qa[1] + (qb[1] - qa[1]) * u, qa[2] + (qb[2] - qa[2]) * u, qa[3] + (qb[3] - qa[3]) * u};
    float r[9];
    vwire::RowsFromQuat(q, r);
    std::memcpy(&t.rot, r, sizeof(r));
    return t;
}

// ---------------------------------------------------------------- per vehicle
struct Frm {
    double key = 0;
    vwire::DriveFrame f;
};
struct Veh {
    vwire::VKey key;
    vwire::OwnState own;               // as this game knows it (the host decides)
    // the partner's frames (it owns it)
    std::deque<Frm> track;             // DRIVE frames, ascending keys
    double track_rx_ms = -1e18;
    bool have_park = false;
    Frm park;
    double park_rx_ms = -1e18;
    double last_rx_ms = -1e18;         // any frame accepted
    double release_detach_ms = 0;      // the partner got off (RELEASE): detach after the last frames play
    std::uint32_t last_seq = 0;
    // the follower controller
    std::uintptr_t block = 0, ctl_entity = 0, ctl_unit = 0;
    double attach_ms = 0;
    unsigned attach_fails = 0;
    bool lite_logged = false;
    double attach_retry_ms = 0;
    LONG calls_at_log = 0;
    // corrections
    double next_corr_ms = 0, next_nudge_ms = 0, last_tp_ms = -1e18, last_interp_ms = -1e18, blocked_log_ms = -1e18;
    bool native_bad = false;
    double imp_since_ms = 0, imp_err0 = 0;
    unsigned n_none = 0, n_imp = 0, n_interp = 0, n_tp = 0, n_blocked = 0, n_nudge = 0, n_native_fail = 0;
    double err_sum = 0, err_max = 0, ang_max = 0;
    unsigned err_n = 0;
    double win_sum = 0, win_max = 0, win_ang = 0;
    unsigned win_n = 0;
    double win_log_ms = 0;
    // state copy (follower)
    bool applied_broken = false, applied_exploded = false;
    double state_next_ms = 0;
    unsigned life_writes = 0, battery_writes = 0;
    // owner side
    double park_sent_ms = -1e18;
    WorldTransform park_sent{};
    bool park_sent_have = false;
    std::uint32_t seq = 0;
    bool meas_have = false;
    WorldTransform meas_last{};
    double meas_ms = 0;
    float vel[3] = {}, avel[3] = {};
    double speed_mps = 0;
    vwire::RestDetector rest;
    bool owner_broken_sent = false, owner_exploded_sent = false;
    // local driving and ownership
    double host_drove_ms = -1e18;      // host: its Sam last held it
    double claim_ms = -1e18;
    unsigned claims = 0;
    double force_off_ms = -1e18;
    unsigned force_offs = 0;
    bool released_got_off = false;     // we owned it and got off: at rest it is the host's (guest), RELEASE at rest
    double release_rest_ms = -1e18;
    unsigned release_rest_sent = 0;
    double last_guest_frame_ms = -1e18;
    double push_claim_ms = -1e18;
    double place_note_ms = -1e18;
    double final_place_until = 0;      // host: the guest's RELEASE at rest named where it parked: placed there once (until then)
    std::uint32_t frames_rx = 0, frames_refused = 0;
    // review fixes
    std::uint8_t pre_npc408 = 0, pre_npc48 = 0;  // the NPC bits before our attach (cleared after a detach only when they were clear)
    double last_detach_ms = -1e18;     // our follower let go (a coasting follower is not a push)
    double away_since_ms = 0;          // host: the guest owns it parked and its Sam is away from it since
    bool far_place = false;            // host: its actor is not spawned here; the guest has it at far_xf (placed there on reappear)
    WorldTransform far_xf{};
    unsigned tp_fails = 0;             // parked teleports onto the same place that did not hold (the backoff)
    bool tp_have = false;
    double tp_target[3] = {};
    double held_since_ms = 0;          // in the ignore band since (10 s there: the teleports held)
    double reach_log_ms = -1e18;
};
std::map<std::uint32_t, Veh> g_veh;
Veh& V(const vwire::VKey& k) {
    Veh& v = g_veh[k.id];
    if (!vwire::KeyValid(v.key)) v.key = k;
    return v;
}
Veh* FindV(std::uint32_t id) {
    auto it = g_veh.find(id);
    return it == g_veh.end() ? nullptr : &it->second;
}

bool WeOwn(const Veh& v) noexcept { return (g_role == 1 && v.own.owner == vwire::kOwnerHost) || (g_role == 2 && v.own.owner == vwire::kOwnerGuest); }
bool PartnerOwns(const Veh& v) noexcept { return !WeOwn(v); }

// the local driving
struct LocalDrive {
    bool on = false;
    vwire::VKey key;
    std::uintptr_t entity = 0, unit = 0;
    double since = 0;
};
LocalDrive g_drive;

// the units of this tick (host id -> unit)
std::vector<vshare::Unit> g_units;
const vshare::Unit* UnitFor(std::uint32_t host_id) noexcept {
    for (const vshare::Unit& u : g_units)
        if (u.key.id == host_id) return &u;
    return nullptr;
}

// counters
std::uint64_t g_tx_drive = 0, g_tx_park = 0, g_rx_drive = 0, g_rx_park = 0, g_rx_refused = 0, g_attaches = 0, g_detaches = 0, g_attach_fail = 0;
std::uint64_t g_own_tx = 0, g_own_rx = 0, g_force_offs = 0, g_claims = 0, g_grants = 0, g_denies = 0;

// ---------------------------------------------------------------- channel 3 (VOWN)
void SendOwn(vwire::OwnType type, const vwire::VKey& key, std::uint8_t owner, std::uint8_t reason, std::uint32_t epoch, const WorldTransform* xf,
             const float* vel) {
    vwire::Own m;
    m.type = type;
    m.key = key;
    m.owner = owner;
    m.reason = reason;
    m.epoch = epoch;
    m.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(g_now));
    if (xf) {
        m.has_xf = true;
        m.pos[0] = xf->pos.x, m.pos[1] = xf->pos.y, m.pos[2] = xf->pos.z;
        vwire::QuatFromRows(Rows(*xf), m.quat);
        if (vel) std::memcpy(m.vel, vel, sizeof(m.vel));
        for (float& f : m.vel)
            if (!std::isfinite(f) || std::fabs(f) > 199.0f) f = 0;
    }
    m.world = g_counter;  // our world: the partner drops a message of another one
    std::uint8_t buf[vwire::kHeaderSize + vwire::kOwnBodySize];
    const std::size_t n = vwire::EncodeOwn(m, buf, sizeof(buf));
    if (n && vshare::SendEvent(buf, n)) {
        ++g_own_tx;
        Log("[vdrive] %s: %s %s 0x%06X (owner %s, reason %u, epoch %u)", g_role == 1 ? "host" : "guest", vwire::OwnTypeName(static_cast<std::uint8_t>(type)),
            vwire::KindName(key.kind), key.id, owner == vwire::kOwnerHost ? "host" : "guest", reason, epoch);
    } else {
        Log("[vdrive] %s: could not queue %s for 0x%06X", g_role == 1 ? "host" : "guest", vwire::OwnTypeName(static_cast<std::uint8_t>(type)), key.id);
    }
}

void ForceOff(Veh& v, const char* why) {
    if (!g_drive.on || g_drive.key.id != v.key.id) return;
    if (g_now - v.force_off_ms < 1000 || v.force_offs >= 6) return;
    v.force_off_ms = g_now;
    ++v.force_offs;
    const bool ok = mem::Q(A(kRideOffGlobal)) != 0 && CallRideOff();
    ++g_force_offs;
    Log("[vdrive] guest: our Sam put off %s 0x%06X (%s): RideOffVehicleForcibly %s (try %u)", vwire::KindName(v.key.kind), v.key.id, why, ok ? "called" : "FAULTED",
        v.force_offs);
}

vwire::Footprint FootprintAt(const WorldTransform& xf, std::uint8_t kind) noexcept {
    vwire::Footprint fp;
    fp.c[0] = xf.pos.x, fp.c[1] = xf.pos.y, fp.c[2] = xf.pos.z;
    std::memcpy(fp.rot, &xf.rot, sizeof(fp.rot));
    vwire::KindHalfExtents(kind, fp.half);
    return fp;
}
WorldTransform XfOfOwn(const vwire::Own& m) noexcept {
    WorldTransform t{};
    t.pos.x = m.pos[0], t.pos.y = m.pos[1], t.pos.z = m.pos[2];
    float r[9];
    vwire::RowsFromQuat(m.quat, r);
    std::memcpy(&t.rot, r, sizeof(r));
    return t;
}
// Host: where our unit of this vehicle is (its actor's place, else its own; the guest's place while it is not spawned here).
bool HostPlace(const Veh* v, std::uint32_t id, WorldTransform* out) noexcept {
    if (v && v->far_place) {
        *out = v->far_xf;
        return true;
    }
    vshare::Unit u;
    if (!vshare::FindByHostId(id, &u) || !u.has_xf) return false;
    *out = u.xf;
    return true;
}

// Host (review fix): a CLAIM is granted only when the partner body is at the vehicle (get on: within 3 m of its footprint;
// push: 6 m). Otherwise DENY (not there); a vehicle the guest owned comes back to the host. True: decide it as before.
bool HostClaimNear(const vwire::Own& m, double now) {
    Veh* pv = FindV(m.key.id);
    vshare::Unit hu;
    const bool have_unit = vshare::FindByKey(m.key, &hu) && hu.key == m.key;
    WorldTransform place{};
    const bool have_place = have_unit && HostPlace(pv, m.key.id, &place);
    double pp[3] = {};
    const bool have_p = vshare::PartnerPlace(pp);
    if (have_place && vwire::ClaimNear(FootprintAt(place, m.key.kind), have_p, pp, m.reason)) return true;
    ++g_claims;
    ++g_denies;
    vwire::OwnState s = pv ? pv->own : vwire::OwnState{};
    if (s.owner != vwire::kOwnerHost) {
        s.owner = vwire::kOwnerHost;
        ++s.epoch;
    }
    if (pv) {
        pv->own = s;
        pv->release_detach_ms = now;
        pv->track.clear();
    }
    const double d = have_place && have_p ? Dist3(place, pp) : -1;
    Log("[vdrive] host: the guest claims %s 0x%06X (%s, epoch %u): DENIED (%s; the partner body %.1f m from its centre)", vwire::KindName(m.key.kind),
        m.key.id, m.reason == vwire::kClaimPush ? "push" : "get on", m.epoch,
        !have_unit ? "no such vehicle here" : !have_place ? "no place of it here" : !have_p ? "no partner body here" : "its Sam is not at it", d);
    SendOwn(vwire::OwnType::deny, m.key, vwire::kOwnerHost, vwire::kDenyNotThere, s.epoch, nullptr, nullptr);
    return false;
}

// Host: the guest has our vehicle at `xf` while its actor is not spawned here (it drove it out of our streaming range):
// remembered (and the snapshot carries it) until the actor is back, then it is placed there. Only within reach of the
// partner body (a bad place never moves a host vehicle across the map).
void NoteFarPlace(Veh& v, const WorldTransform& xf, const char* why) {
    double pp[3] = {};
    const bool have_p = vshare::PartnerPlace(pp);
    const double fp[3] = {xf.pos.x, xf.pos.y, xf.pos.z};
    const double none[3] = {};
    if (!vwire::FrameInReach(fp, false, none, have_p, pp)) return;
    if (!v.far_place)
        Log("[vdrive] host: %s 0x%06X is not spawned here; the guest has it at (%.1f, %.1f, %.1f) (%s): placed there when it is back", vwire::KindName(v.key.kind),
            v.key.id, xf.pos.x, xf.pos.y, xf.pos.z, why);
    v.far_place = true;
    v.far_xf = xf;
    vshare::SetPlaceOverride(v.key.id, xf);
}
void ClearFarPlace(Veh& v) {
    if (!v.far_place) return;
    v.far_place = false;
    vshare::ClearPlaceOverride(v.key.id);
}

void OnOwn(std::uint8_t type, const std::uint8_t* msg, std::size_t size, std::uint32_t generation, double now) {
    (void)type;
    (void)generation;
    vwire::Own m;
    if (!g_on.load() || !vwire::DecodeOwn(msg, size, m)) {
        Log("[vdrive] dropped a VOWN message (%zu bytes): not valid", size);
        return;
    }
    ++g_own_rx;
    // another world's message (the partner still in the old world after a reload, or already in a newer one): dropped
    if (g_role == 2) vshare::NotePeerWorld(m.world);
    if (m.world != g_counter || (g_role == 1 && !vshare::PeerInOurWorld())) {
        Log("[vdrive] %s: dropped a %s for 0x%06X from world #%u (ours #%u%s)", g_role == 1 ? "host" : "guest",
            vwire::OwnTypeName(static_cast<std::uint8_t>(m.type)), m.key.id, m.world, g_counter,
            g_role == 1 && m.world == g_counter ? ": the guest has not asked for this world's vehicles yet" : "");
        return;
    }
    if (g_role == 1 && m.type == vwire::OwnType::claim && !HostClaimNear(m, now)) return;
    if (g_role == 1 && m.type == vwire::OwnType::release && !FindV(m.key.id)) {
        Log("[vdrive] host: dropped a RELEASE of 0x%06X: the guest never owned it", m.key.id);
        return;
    }
    Veh& v = V(m.key);
    if (g_role == 1) {
        switch (m.type) {
            case vwire::OwnType::claim: {
                ++g_claims;
                const bool host_on = g_drive.on && g_drive.key.id == m.key.id;
                const double ago = v.host_drove_ms > -1e17 ? now - v.host_drove_ms : -1;
                const vwire::OwnStep st = vwire::HostOnClaim(v.own, m.reason, host_on, host_on ? 0 : ago);
                Log("[vdrive] host: the guest claims %s 0x%06X (%s, epoch %u): %s", vwire::KindName(m.key.kind), m.key.id,
                    m.reason == vwire::kClaimPush ? "push" : "get on", m.epoch, st.type == vwire::OwnType::grant ? "granted" : "DENIED (the host wins)");
                v.own = st.next;
                if (st.next.owner == vwire::kOwnerGuest) {
                    v.last_guest_frame_ms = now;  // its frames start now
                    vshare::NotePartnerDrove(v.key, now);
                }
                if (st.type == vwire::OwnType::grant) ++g_grants;
                else ++g_denies;
                if (st.send) SendOwn(st.type, v.key, st.next.owner, st.reason, st.next.epoch, nullptr, nullptr);
                return;
            }
            case vwire::OwnType::release: {
                // at rest, but its DRIVE frames still come: the guest got on again within the round trip; no handback
                if (m.reason == vwire::kReleaseAtRest && v.own.owner == vwire::kOwnerGuest && now - v.track_rx_ms < 500) {
                    Log("[vdrive] host: the guest's RELEASE at rest of %s 0x%06X ignored: it drives it again", vwire::KindName(m.key.kind), m.key.id);
                    return;
                }
                // its actor is not spawned here: the guest's place of it is kept for when it is back
                if (m.has_xf && v.own.owner == vwire::kOwnerGuest && m.epoch == v.own.epoch) {
                    const vshare::Unit* u = UnitFor(m.key.id);
                    if (u && !(u->entity && EntityLive(u->unit, u->entity, v.key.kind))) NoteFarPlace(v, XfOfOwn(m), "its RELEASE");
                }
                const vwire::OwnStep st = vwire::HostOnRelease(v.own, m.reason, m.epoch);
                Log("[vdrive] host: the guest releases %s 0x%06X (%s, epoch %u)%s", vwire::KindName(m.key.kind), m.key.id,
                    m.reason == vwire::kReleaseAtRest ? "at rest" : "got off", m.epoch, st.send ? ": the host's again" : "");
                if (m.reason == vwire::kReleaseGotOff && v.own.owner == vwire::kOwnerGuest) v.release_detach_ms = now + 400;
                if (st.send) {
                    v.own = st.next;
                    v.release_detach_ms = now;  // the follower lets go now
                    // its final place is the newest one: the park target until our own frames take over
                    if (m.has_xf) {
                        vwire::DriveFrame f;
                        f.type = vwire::DriveType::park;
                        f.key = m.key;
                        std::memcpy(f.pos, m.pos, sizeof(f.pos));
                        std::memcpy(f.quat, m.quat, sizeof(f.quat));
                        v.park.f = f;
                        v.park.key = 0;
                        v.have_park = true;
                        v.park_rx_ms = now;
                        v.track.clear();
                        v.final_place_until = now + 3000;
                    }
                    SendOwn(st.type, v.key, st.next.owner, st.reason, st.next.epoch, nullptr, nullptr);
                }
                return;
            }
            default:
                break;
        }
    } else if (g_role == 2) {
        switch (m.type) {
            case vwire::OwnType::grant:
            case vwire::OwnType::deny: {
                bool changed = false;
                const vwire::OwnState was = v.own;
                v.own = vwire::GuestOnAnswer(v.own, m, &changed);
                if (m.type == vwire::OwnType::grant) ++g_grants;
                else ++g_denies;
                Log("[vdrive] guest: %s for %s 0x%06X: owner %s, epoch %u (was %s / %u)%s", vwire::OwnTypeName(static_cast<std::uint8_t>(m.type)),
                    vwire::KindName(m.key.kind), m.key.id, v.own.owner == vwire::kOwnerHost ? "host" : "guest", v.own.epoch,
                    was.owner == vwire::kOwnerHost ? "host" : "guest", was.epoch, changed ? "" : " (no change)");
                const bool on_it = g_drive.on && g_drive.key.id == m.key.id;
                if (v.own.owner == vwire::kOwnerHost) {
                    v.released_got_off = false;
                    v.release_rest_sent = 0;
                    if (on_it && m.type == vwire::OwnType::grant && m.reason == vwire::kGrantHandback) {
                        // the handback crossed our getting on again (the RELEASE at rest was on its way): claimed again, not put off
                        v.force_offs = 0;
                        v.claims = 0;
                        v.claim_ms = -1e18;
                        Log("[vdrive] guest: the host's handback of %s 0x%06X crossed our Sam getting on again: claimed again", vwire::KindName(m.key.kind),
                            m.key.id);
                    } else if (on_it) {
                        ForceOff(v, m.type == vwire::OwnType::deny ? (m.reason == vwire::kDenyNotThere ? "the host does not see our Sam at it"
                                                                                                   : "the host's Sam was first")
                                                                   : "the host took it");
                    }
                } else {
                    v.claims = 0;
                    v.track.clear();  // ours now: no follower
                    if (!on_it) {
                        // ours while our Sam is not on it (a push, or the answer came after we got off): back to the host at rest
                        v.released_got_off = true;
                        v.release_rest_sent = 0;
                        v.rest.Reset();
                        Log("[vdrive] guest: %s 0x%06X is ours while our Sam is not on it: the host's again once it rests", vwire::KindName(m.key.kind),
                            m.key.id);
                    }
                }
                return;
            }
            case vwire::OwnType::release:
                // the host's Sam got off: its last frames play, then the follower lets go
                if (m.owner == vwire::kOwnerHost) v.release_detach_ms = now + 400;
                Log("[vdrive] guest: the host released %s 0x%06X (%s)", vwire::KindName(m.key.kind), m.key.id,
                    m.reason == vwire::kReleaseAtRest ? "at rest" : "got off");
                return;
            default:
                break;
        }
    }
    Log("[vdrive] dropped a %s for 0x%06X: not valid for this side", vwire::OwnTypeName(static_cast<std::uint8_t>(m.type)), m.key.id);
}

// ---------------------------------------------------------------- vehicle_share's queries
bool GuestOwnsQuery(const vwire::VKey& k) {
    const Veh* v = FindV(k.id);
    return v && v->own.owner == vwire::kOwnerGuest;
}
bool FollowedQuery(std::uint32_t host_id) {
    const Veh* v = FindV(host_id);
    if (!v) return false;
    if (v->block) return true;
    if (g_now - v->last_rx_ms < 5000) return true;
    if (g_role == 2 && v->own.owner == vwire::kOwnerGuest) return true;
    return g_drive.on && g_drive.key.id == host_id;
}

// ---------------------------------------------------------------- the follower controller
// The NPC-driven bits our attach made SetDriver set (+0x408 & 0x10, [+0x898]+0x48 & 2), cleared on a vehicle nobody drives
// now, as the game's own get-off path does; only bits that were clear before our attach (`pre_*` 0) or, on an actor that
// spawned again after ours went (`any`), whenever set. Returns the bits cleared.
unsigned ClearNpcBits(std::uintptr_t e, bool any, std::uint8_t pre408, std::uint8_t pre48) noexcept {
    if (!(g_groups_ok & kGNpc) || !e) return 0;
    std::int32_t has = 0;
    std::uint32_t drv = 0;
    if (!mem::Rd(e + kVHasDriver, has) || has != 0 || !mem::Rd(e + kVDriver, drv) || drv != kNoActor) return 0;
    unsigned cleared = 0;
    std::uint8_t b408 = 0;
    if (mem::Rd(e + 0x408, b408) && (b408 & 0x10) && (any || !pre408) && AndBits8(e + 0x408, static_cast<std::uint8_t>(~0x10u))) cleared |= 1;
    const std::uintptr_t s = mem::Q(e + 0x898);
    std::uint32_t f48 = 0;
    if (s && mem::Rd(s + 0x48, f48) && (f48 & 2) && (any || !pre48) && AndBits32(s + 0x48, ~2u)) cleared |= 2;
    return cleared;
}

// Units whose actor went while our follower held them: the host's Touch (the recent player vehicles) and the NPC bits are
// done when the unit is seen again with its id (review fix).
struct AfterGone {
    std::uintptr_t unit = 0;
    std::uint32_t id = 0;
    std::uint8_t kind = 0;
    double t = 0;
};
std::vector<AfterGone> g_after_gone;

void Detach(Veh& v, const char* why) {
    if (!v.block) return;
    InBuf* b = BufOf(v.block);
    b->on = 0;
    v.last_detach_ms = g_now;
    const std::uintptr_t in = v.block + kInputOff;
    if (mem::Q(in) == reinterpret_cast<std::uintptr_t>(&g_vt_copy[1])) SetVtable(in, A(kInVt));
    const LONG calls = b->calls;
    // The game's Detach / dtor only on the vehicle still alive (a world being torn down, an entity streamed out): otherwise the
    // block stays allocated with its inputs off (never freed under a reference we cannot see).
    if (!EntityLive(v.ctl_unit, v.ctl_entity, v.key.kind)) {
        Log("[vdrive] follower of %s 0x%06X: its vehicle is gone (%s): the controller block is left as it is", vwire::KindName(v.key.kind), v.key.id, why);
        ++g_detaches;
        if (v.ctl_unit && g_after_gone.size() < 64) {
            AfterGone a;
            a.unit = v.ctl_unit, a.id = v.key.id, a.kind = v.key.kind, a.t = g_now;
            g_after_gone.push_back(a);
        }
        v.block = 0;
        v.ctl_entity = 0;
        v.ctl_unit = 0;
        v.applied_broken = v.applied_exploded = false;
        return;
    }
    const bool ok = CallDetachDtor(v.block);
    const unsigned npc = ok ? ClearNpcBits(v.ctl_entity, false, v.pre_npc408, v.pre_npc48) : 0;
    {
        // what the game's Detach left (the first few detaches: evidence for the NPC-bit clearing)
        static unsigned shown = 0;
        std::uint8_t b408 = 0;
        std::uint32_t f48 = 0, drv = 0;
        std::int32_t has = 0;
        const std::uintptr_t s = mem::Q(v.ctl_entity + 0x898);
        const bool f48_ok = s && mem::Rd(s + 0x48, f48);
        if (ok && shown < 4 && mem::Rd(v.ctl_entity + 0x408, b408) && mem::Rd(v.ctl_entity + kVHasDriver, has) && mem::Rd(v.ctl_entity + kVDriver, drv)) {
            ++shown;
            Log("[vdrive] %s 0x%06X after the detach: +0x408 0x%02X (before our attach 0x%02X), [+0x898]+0x48 %s0x%X (before 0x%X), seat %d, driver 0x%X; NPC bits "
                "cleared now: %u",
                vwire::KindName(v.key.kind), v.key.id, b408, v.pre_npc408, f48_ok ? "" : "unreadable ", f48 & 2, v.pre_npc48, has, drv, npc);
        }
    }
    if (npc) Log("[vdrive] %s 0x%06X: the NPC-driven bits our attach set are cleared (%s%s)", vwire::KindName(v.key.kind), v.key.id, (npc & 1) ? "+0x408 0x10" : "",
                 (npc & 2) ? ((npc & 1) ? ", [+0x898]+0x48 2" : "[+0x898]+0x48 2") : "");
    // host: the NPC attach took the vehicle off the recent player vehicles; the game puts a player's back with Touch
    bool touched = false;
    if (ok && g_role == 1 && (g_groups_ok & kGTouch) && v.ctl_unit) {
        const std::uintptr_t r = mem::Q(A(kRegGlobal));
        std::uint32_t id = 0;
        if (r && mem::Rd(v.ctl_unit + 8, id) && id == v.key.id) touched = CallTouch(r + 0x58, v.ctl_unit);
    }
    ++g_detaches;
    // "#n" counts per game (a scenario's waitlog matches the whole log: the n-th one is unique)
    Log("[vdrive] follower of %s 0x%06X detached #%llu (%s) after %.1f s, %ld input updates%s%s", vwire::KindName(v.key.kind), v.key.id,
        static_cast<unsigned long long>(g_detaches), why, (g_now - v.attach_ms) / 1000.0, static_cast<long>(calls), ok ? "" : " (FAULT in detach)",
        touched ? "; put back on the recent vehicles (Touch)" : "");
    if (ok) VirtualFree(reinterpret_cast<void*>(v.block), 0, MEM_RELEASE);  // a fault leaves the block (it may still be referenced)
    v.block = 0;
    v.ctl_entity = 0;
    v.ctl_unit = 0;
    v.applied_broken = v.applied_exploded = false;
}

void DetachAll(const char* why) {
    for (auto& kv : g_veh) Detach(kv.second, why);
}

bool Attach(Veh& v, const vshare::Unit& u) {
    if (!g_vt_ready) return false;
    const std::uintptr_t e = u.entity;
    if (!EntityLive(u.unit, e, v.key.kind)) return false;
    const std::uintptr_t sd = mem::Q(mem::Q(e) + 0x110);
    if (sd != A(kSetDriver) && sd != A(kSetDriverTruck)) {
        Log("[vdrive] follower of 0x%06X: the vehicle's SetDriver slot differs: not attached", v.key.id);
        return false;
    }
    std::int32_t has = 0, st = 0;
    if (!mem::Rd(e + kVHasDriver, has) || !mem::Rd(e + kVRideState, st) || has != 0 || st == 5) return false;  // somebody holds it, or broken
    // Attach calls into the vehicle's AI drive component unchecked (0x14242D660, then [+0x30]): only a vehicle whose component
    // holds a route source takes the full Attach; one without (trikes, the roadster) the lite one, or none.
    const std::uintptr_t comp = CallLookup(e);
    if (comp == 1) {
        Log("[vdrive] follower of 0x%06X: the drive component lookup faulted: not attached", v.key.id);
        return false;
    }
    bool full = false;
    if (comp) {
        std::uintptr_t r78 = 0, r88 = 0, q30 = 0;
        std::uint8_t b98 = 0;
        if (!mem::Rd(comp + 0x78, r78) || !mem::Rd(comp + 0x98, b98) || !mem::Rd(comp + 0x88, r88)) return false;
        const std::uintptr_t src = r78 ? comp + 0x50 : b98 ? r88 : 0;
        full = src && mem::Rd(src + 0x30, q30);
    }
    if (!full && !(g_groups_ok & kGLite)) {
        if (!v.lite_logged)
            Log("[vdrive] follower of %s 0x%06X: the vehicle has no AI drive component and the lite attach is off (byte checks): not followed",
                vwire::KindName(v.key.kind), v.key.id);
        v.lite_logged = true;
        return false;
    }
    // the NPC bits before our attach (only those it sets are cleared after the detach)
    {
        std::uint8_t b408 = 0;
        std::uint32_t f48 = 0;
        const std::uintptr_t s = mem::Q(e + 0x898);
        v.pre_npc408 = mem::Rd(e + 0x408, b408) ? static_cast<std::uint8_t>(b408 & 0x10) : 0x10;
        v.pre_npc48 = s && mem::Rd(s + 0x48, f48) ? static_cast<std::uint8_t>(f48 & 2) : 2;  // unreadable: never cleared
    }
    auto* m = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kBlock, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!m) return false;
    const auto block = reinterpret_cast<std::uintptr_t>(m);
    InBuf* b = BufOf(block);
    b->magic = kInMagic;
    b->on = 0;
    const int r = full ? CallCtorAttach(block, e) : CallCtorAttachLite(block, e);
    if (r != 1) {
        ++g_attach_fail;
        if (r == 0) {
            CallDtorOnly(block);
            VirtualFree(m, 0, MEM_RELEASE);
        }
        Log("[vdrive] follower of %s 0x%06X: %s", vwire::KindName(v.key.kind), v.key.id, r == 0 ? "Attach refused" : "FAULT in ctor / Attach (block kept)");
        return false;
    }
    const std::uintptr_t in = block + kInputOff;
    const bool vt_ok = mem::Q(in) == A(kInVt) && SetVtable(in, reinterpret_cast<std::uintptr_t>(&g_vt_copy[1]));
    std::uint8_t idle[vwire::kDriveInputBytes];
    IdleInputs(idle);
    WriteInputs(block, idle);
    const bool seat_ok = mem::Q(e + kVInput) == in;
    std::uint32_t drv = 0;
    mem::Rd(e + kVDriver, drv);
    v.block = block;
    v.ctl_entity = e;
    v.ctl_unit = u.unit;
    v.attach_ms = g_now;
    ++g_attaches;
    Log("[vdrive] follower of %s 0x%06X attached #%llu: the game's NPC controller%s (driver 0x%X, [+0x528] %s ctl+0x30, input vtable %s)",
        vwire::KindName(v.key.kind), v.key.id, static_cast<unsigned long long>(g_attaches), full ? "" : " without its route part (no AI drive component)", drv,
        seat_ok ? "==" : "!=",
        vt_ok ? "ours (the inputs replay)" : "UNEXPECTED (detached)");
    if (!vt_ok || !seat_ok) {
        Detach(v, "the input object is not as expected");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- corrections
bool PartFrames(std::uintptr_t e, int idx, float (&frame)[16], float (&inv)[16]) noexcept {
    static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(frame, ident, sizeof(frame));
    std::memcpy(inv, ident, sizeof(inv));
    const std::uintptr_t tbl = mem::Q(e + kEParts);
    std::int32_t n = 0;
    if (!tbl || idx < 0 || !mem::Rd(tbl + 0x58, n) || idx >= n || n > 64) return true;
    const std::uintptr_t part = mem::Q(tbl + 0x60) + static_cast<std::uintptr_t>(idx) * 0x100;
    std::uint32_t fl = 0;
    if (!mem::Rd(part + 0xEC, fl)) return false;
    if ((fl >> 10) & 1u) return true;  // identity, as the game's remote step
    if (!mem::Read(part + 0x10, frame, sizeof(frame))) return false;
    for (float x : frame)
        if (!std::isfinite(x) || std::fabs(x) > 1e7f) return false;
    return CallInverse(frame, inv);
}

float StepTime(std::uintptr_t e) noexcept {
    float dt = 0;
    if (!mem::Rd(e + kEDt, dt) || !std::isfinite(dt) || dt <= 0.0005f || dt > 0.2f) dt = 1.0f / 30.0f;
    return 3.0f * dt;
}

bool InstOk(std::uintptr_t inst) noexcept { return inst && mem::InImage(mem::Q(inst)) && mem::InText(mem::Q(mem::Q(inst) + 0xE0)); }

bool NativeCorrect(Veh& v, std::uintptr_t e, const WorldTransform& target, const vwire::BandLimits& lim) {
    int idx = -1;
    const std::uintptr_t inst = CallFirstInst(e, &idx);
    if (!InstOk(inst)) return false;
    alignas(16) float frame[16], inv[16], mat[16];
    if (!PartFrames(e, idx, frame, inv)) return false;
    MatFromWt(target, mat);
    const float cos_ign = static_cast<float>(std::cos(lim.ignore_deg * 0.017453292519943295));
    const float cos_imp = static_cast<float>(std::cos(lim.impulse_deg * 0.017453292519943295));
    const bool ok = CallCorrect(e, mat, inst, frame, inv, static_cast<float>(lim.ignore_m), static_cast<float>(lim.impulse_m),
                                static_cast<float>(lim.interp_m), cos_ign, cos_imp, StepTime(e));
    if (!ok) {
        ++v.n_native_fail;
        v.native_bad = true;
        Log("[vdrive] FAULT in the native net correction on 0x%06X: the nudge / teleports from now on for it", v.key.id);
    }
    return ok;
}

// The clamped velocity nudge through the Havok instance: the game's drive-to-transform (0x141C43E10, the call the native
// correction's impulse branch makes) toward the place the vehicle should reach in one step, `t`: its owner's velocity (`tvel`,
// the frames' measured one; 0 when parked) plus what closes the error in 0.5 s, at most 2 m/s more; the rotation turned by the
// owner's yaw rate plus at most 1 rad/s toward the target. The game sets the velocity that reaches it, so the follower keeps
// moving with its own wheels in between (one call per 100 ms while it is driven).
void RotateYaw(WorldTransform& t, double yaw) noexcept {
    const double c = std::cos(yaw), s = std::sin(yaw);
    for (auto& row : t.rot.m) {
        const double x = row[0], y = row[1];
        row[0] = static_cast<float>(c * x - s * y);
        row[1] = static_cast<float>(s * x + c * y);
    }
}
bool Nudge(Veh& v, std::uintptr_t e, const WorldTransform& cur, const WorldTransform& target, const float (&tvel)[3], float yaw_rate) {
    if (!(g_groups_ok & kGNudge)) return false;
    int idx = -1;
    const std::uintptr_t inst = CallFirstInst(e, &idx);
    if (!InstOk(inst)) return false;
    alignas(16) float frame[16], inv[16], mat[16];
    if (!(g_groups_ok & kGNative)) {
        static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(frame, ident, sizeof(frame));
    } else if (!PartFrames(e, idx, frame, inv)) {
        return false;
    }
    const float t = StepTime(e);
    const double err[3] = {target.pos.x - cur.pos.x, target.pos.y - cur.pos.y, target.pos.z - cur.pos.z};
    float want[3];
    vwire::NudgeVelocity(err, tvel, 0.5, 2.0, want);
    WorldTransform to = cur;
    to.pos.x += want[0] * t, to.pos.y += want[1] * t, to.pos.z += want[2] * t;
    // the rotation: the target's, carried on by the owner's yaw rate over the step, reached at most 1 rad/s faster
    WorldTransform rot_target = target;
    if (std::isfinite(yaw_rate) && std::fabs(yaw_rate) < 20.0f) RotateYaw(rot_target, static_cast<double>(yaw_rate) * t);
    const double ang = vwire::AngleBetweenRows(Rows(cur), Rows(rot_target)) * 0.017453292519943295;
    const double allowed = (std::fabs(static_cast<double>(yaw_rate)) + 1.0) * t;
    const double ra = ang > 1e-6 ? (std::min)(1.0, allowed / ang) : 1.0;
    float qa[4], qb[4];
    vwire::QuatFromRows(Rows(cur), qa);
    vwire::QuatFromRows(Rows(rot_target), qb);
    if (qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3] < 0)
        for (float& x : qb) x = -x;
    const float q[4] = {static_cast<float>(qa[0] + (qb[0] - qa[0]) * ra), static_cast<float>(qa[1] + (qb[1] - qa[1]) * ra),
                        static_cast<float>(qa[2] + (qb[2] - qa[2]) * ra), static_cast<float>(qa[3] + (qb[3] - qa[3]) * ra)};
    float r[9];
    vwire::RowsFromQuat(q, r);
    std::memcpy(&to.rot, r, sizeof(r));
    MatFromWt(to, mat);
    const bool ok = CallDriveTo(inst, mat, frame, t);
    if (ok) ++v.n_nudge;
    return ok;
}

std::atomic<std::uintptr_t> g_carried_unit{0};  // ride_together: the unit our Sam rides on as a passenger (0 none)
bool Clear(std::uintptr_t unit, std::uint8_t kind, const WorldTransform& target, const game::Entity* local) noexcept {
    vwire::Footprint fp;
    fp.c[0] = target.pos.x, fp.c[1] = target.pos.y, fp.c[2] = target.pos.z;
    std::memcpy(fp.rot, &target.rot, sizeof(fp.rot));
    vwire::KindHalfExtents(kind, fp.half);
    static vwire::Footprint others[256];
    static std::uint32_t ids[256];
    std::size_t n = vshare::Footprints(unit, others, 256, ids);
    // A partner-owned vehicle that stands where its owner has it (within 0.5 m of its newest PARK frame) is left out: the
    // owner's world holds both at those places, so the target is no worse there than in that world (two summoned vehicles
    // side by side otherwise blocked each other's teleport for good: run 20261119-105928).
    std::size_t kept = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const Veh* o = ids[i] ? FindV(ids[i]) : nullptr;
        bool skip = false;
        if (o && o->have_park && PartnerOwns(*o) && g_now - o->park_rx_ms < 5000 && (o->track.empty() || o->track.back().key <= o->park.key)) {
            const double dx = others[i].c[0] - o->park.f.pos[0], dy = others[i].c[1] - o->park.f.pos[1], dz = others[i].c[2] - o->park.f.pos[2];
            skip = dx * dx + dy * dy + dz * dz < 0.25;
        }
        if (!skip) others[kept++] = others[i];
    }
    n = kept;
    WorldTransform sam{};
    // our Sam carried as the passenger of this very vehicle (ride_together) moves with it: he never blocks its teleport
    const bool carried_here = unit && g_carried_unit.load(std::memory_order_relaxed) == unit;
    const bool have_sam = !carried_here && local && ReadXf(reinterpret_cast<std::uintptr_t>(local), sam);
    const double sp[3] = {sam.pos.x, sam.pos.y, sam.pos.z};
    return vwire::TeleportClear(fp, have_sam, sp, others, n);
}

game::Entity* g_local = nullptr;

// One correction of our vehicle toward where its owner has it (`target`: the frames at the play time). Driven (the partner
// drives it, our NPC controller replays the inputs): the bands start at g_ignore_drive_m (the replay alone stays within about
// 0.1 m: proof drive), the impulse band nudges with the owner's velocity (at most every 100 ms). Parked: the game's own net
// correction (its velocity drive, then its interpolated teleport), every 100 ms; the nudge when it is off or misbehaves.
// Over 5 m (and 2..5 m without the native one): RequestTeleport, only where the vehicle fits.
void Correct(Veh& v, const vshare::Unit& u, const WorldTransform& target, bool driving, const float (&tvel)[3], float yaw_rate) {
    if (!g_corr_on) return;
    const std::uintptr_t e = u.entity;
    if (!EntityLive(u.unit, e, v.key.kind)) return;
    WorldTransform cur{};
    if (!ReadXf(e, cur)) return;
    const double err = Dist3(cur, target);
    const double ang = vwire::AngleBetweenRows(Rows(cur), Rows(target));
    vwire::BandLimits lim;
    lim.ignore_m = driving ? g_ignore_drive_m : g_ignore_m;
    const vwire::Band band = vwire::ChooseBand(err, ang, lim);
    // meters
    v.err_sum += err, ++v.err_n;
    v.err_max = (std::max)(v.err_max, err);
    v.ang_max = (std::max)(v.ang_max, ang);
    v.win_sum += err, ++v.win_n;
    v.win_max = (std::max)(v.win_max, err);
    v.win_ang = (std::max)(v.win_ang, ang);
    if (g_now - v.win_log_ms >= 1000) {
        if (v.win_n && (driving || v.win_max > 0.1)) {
            std::int32_t rs = -1;
            float ax = 0;
            mem::Rd(e + kVRideState, rs);
            const std::uintptr_t inp = mem::Q(e + kVInput);
            if (inp) mem::Rd(inp + 8, ax);
            const double ts = std::sqrt(static_cast<double>(tvel[0]) * tvel[0] + static_cast<double>(tvel[1]) * tvel[1] + static_cast<double>(tvel[2]) * tvel[2]);
            Log("[vdrive] %s 0x%06X %s: placement error mean %.2f max %.2f m, angle max %.1f deg (1 s; now target-ours %.2f %.2f %.2f; speed ours %.1f owner "
                "%.1f m/s, ride state %d, throttle %.2f); corrections none %u impulse %u interp %u teleport %u blocked %u nudge %u%s",
                vwire::KindName(v.key.kind), v.key.id, driving ? "followed" : "parked", v.win_sum / v.win_n, v.win_max, v.win_ang, target.pos.x - cur.pos.x,
                target.pos.y - cur.pos.y, target.pos.z - cur.pos.z, v.speed_mps, ts, rs, static_cast<double>(ax), v.n_none, v.n_imp, v.n_interp, v.n_tp,
                v.n_blocked, v.n_nudge, v.native_bad ? " (native off for it)" : "");
        }
        v.win_sum = v.win_max = v.win_ang = 0, v.win_n = 0, v.win_log_ms = g_now;
    }
    if (band != vwire::Band::impulse) v.imp_since_ms = 0;
    // parked teleports that do not hold (the vehicle rests against something only the owner's world lacks, or the other way
    // round): after two onto the same place the gap doubles up to 60 s; 10 s in the ignore band, or a new place, start over
    if (band == vwire::Band::none) {
        if (v.held_since_ms == 0) v.held_since_ms = g_now;
        else if (g_now - v.held_since_ms > 10000) v.tp_fails = 0;
    } else {
        v.held_since_ms = 0;
    }
    const double tpos[3] = {target.pos.x, target.pos.y, target.pos.z};
    const bool same_place = v.tp_have && vwire::Dist(tpos, v.tp_target) < 0.1;
    if (v.tp_have && !same_place) v.tp_fails = 0;
    const double parked_gap = vwire::ParkedTeleportGapMs(v.tp_fails);
    auto note_parked_tp = [&]() {
        if (driving) return;
        if (same_place) {
            ++v.tp_fails;
            if (v.tp_fails == 2)
                Log("[vdrive] %s 0x%06X: its parked teleports onto the owner's place do not hold (the worlds differ there): fewer of them now",
                    vwire::KindName(v.key.kind), v.key.id);
        }
        v.tp_have = true;
        std::memcpy(v.tp_target, tpos, sizeof(tpos));
    };
    const bool native = g_native_on && (g_groups_ok & kGNative) && !v.native_bad;
    // Parked (nobody drives it in either world): the game's vehicle teleport onto the owner's place instead of the impulse
    // band. The native correction and then the nudge did not close the error of a parked mirror (run 20261119-105428: a truck
    // summoned into a trike went from 0.5 to 1.27 m, pushed up onto the trike, 69 native calls and 54 nudges) while
    // RequestTeleport put a mirror there and it stayed (run 20261119-104720: 0.01 m after). Small errors (under 0.25 m and 10 deg) wait 1.5 s first so a vehicle still
    // settling is not moved twice; at most one teleport every 2 s, only where it fits.
    // The same while the partner sits in it but neither its vehicle nor ours moves (the nudge left a stopped truck 0.48 m off
    // for 12 s, 150 calls: run 20261119-110534).
    const double tspeed = std::sqrt(static_cast<double>(tvel[0]) * tvel[0] + static_cast<double>(tvel[1]) * tvel[1] + static_cast<double>(tvel[2]) * tvel[2]);
    const bool settled = driving && tspeed < 0.3 && std::fabs(yaw_rate) < 0.1f && v.meas_have && g_now - v.meas_ms < 500 && v.speed_mps < 0.3;
    if (band == vwire::Band::impulse && (!driving || settled) && !g_parked_native) {
        ++v.n_imp;
        if (v.imp_since_ms == 0) v.imp_since_ms = g_now;
        const bool big = err >= 0.25 || ang >= 10.0;
        if (!big && g_now - v.imp_since_ms < 1500) return;
        if (g_now - v.last_tp_ms < (driving ? 2000.0 : parked_gap)) return;
        if (!Clear(u.unit, v.key.kind, target, g_local)) {
            ++v.n_blocked;
            if (g_now - v.blocked_log_ms > 3000) {
                v.blocked_log_ms = g_now;
                Log("[vdrive] %s 0x%06X is %.2f m off (parked) but its place is taken (the local Sam or another vehicle): waiting", vwire::KindName(v.key.kind),
                    v.key.id, err);
            }
            return;
        }
        v.last_tp_ms = v.last_interp_ms = g_now;
        note_parked_tp();
        if (vshare::RequestTeleport(e, target)) {
            ++v.n_tp;
            Log("[vdrive] %s 0x%06X %.2f m off (%.0f deg), %s: RequestTeleport onto the owner's place", vwire::KindName(v.key.kind), v.key.id, err, ang,
                driving ? "stopped with its driver" : "parked");
        }
        return;
    }
    switch (band) {
        case vwire::Band::none:
            ++v.n_none;
            return;
        case vwire::Band::impulse: {
            if (driving) {
                if (g_now < v.next_nudge_ms) return;
                v.next_nudge_ms = g_now + 100;
                ++v.n_imp;
                Nudge(v, e, cur, target, tvel, yaw_rate);
                return;
            }
            ++v.n_imp;
            // misbehaving: 3 s in this band without getting under 70 % of where it started
            if (v.imp_since_ms == 0) {
                v.imp_since_ms = g_now, v.imp_err0 = err;
            } else if (native && g_now - v.imp_since_ms > 3000) {
                if (err > 0.7 * v.imp_err0 && err > 0.5) {
                    v.native_bad = true;
                    Log("[vdrive] the native correction did not close %.2f m on 0x%06X in 3 s (still %.2f m): the clamped nudge for it", v.imp_err0, v.key.id, err);
                }
                v.imp_since_ms = g_now, v.imp_err0 = err;
            }
            const float zero[3] = {0, 0, 0};
            if (native) NativeCorrect(v, e, target, lim);
            else Nudge(v, e, cur, target, zero, 0.0f);
            return;
        }
        case vwire::Band::interp:
        case vwire::Band::teleport: {
            const bool interp = band == vwire::Band::interp;
            const double gap = driving ? (interp ? 500.0 : 1000.0) : (std::max)(interp ? 500.0 : 1000.0, v.tp_fails >= 2 ? parked_gap : 0.0);
            if (g_now - (interp ? v.last_interp_ms : v.last_tp_ms) < gap) return;
            if (!Clear(u.unit, v.key.kind, target, g_local)) {
                ++v.n_blocked;
                if (g_now - v.blocked_log_ms > 3000) {
                    v.blocked_log_ms = g_now;
                    Log("[vdrive] %s 0x%06X is %.1f m off but its place is taken (the local Sam or another vehicle): waiting", vwire::KindName(v.key.kind), v.key.id, err);
                }
                return;
            }
            // The native correction's 2..5 m branch (0x1422DFF80 + 0x1423964D0) set the entity's transform but the vehicle's
            // physics kept its own (47 calls, 0.7 m / 131 deg unchanged: run 20261119-103440): the game's vehicle teleport
            // instead, which moves a dynamic vehicle (proof drive2).
            v.last_tp_ms = v.last_interp_ms = g_now;
            note_parked_tp();
            if (vshare::RequestTeleport(e, target)) {
                if (interp) ++v.n_interp;
                else ++v.n_tp;
                Log("[vdrive] %s 0x%06X %.1f m off (%.0f deg): RequestTeleport onto the owner's place", vwire::KindName(v.key.kind), v.key.id, err, ang);
            }
            return;
        }
    }
}

// ---------------------------------------------------------------- state copy (follower)
void ApplyState(Veh& v, std::uintptr_t unit_ptr, std::uintptr_t e, const vwire::DriveFrame& f) {
    if (!(g_groups_ok & kGState) || g_now < v.state_next_ms) return;
    // the poll is 10 Hz: the entity is checked again right before the writes (it may have streamed out or been removed)
    if (!EntityLive(unit_ptr, e, v.key.kind)) return;
    v.state_next_ms = g_now + 100;
    float life = 0, max = 0;
    if (mem::Rd(e + kVLife, life) && mem::Rd(e + kVLifeMax, max) && std::isfinite(life) && std::isfinite(max) && max > 0 && max < 1e6 && f.life_max > 0) {
        float want = f.life;
        if (want > max) want = max;
        if (std::fabs(want - life) > 0.5f) {
            // the unit's health (+0x150, what the save keeps) follows when it holds the same value as the entity's: the
            // game set the entity's back from it after a write of the entity's alone (run 20261119-103440)
            float unit_life = 0;
            const std::uintptr_t unit = mem::Q(e + kVUnit);
            if (unit && mem::Rd(unit + 0x150, unit_life) && std::isfinite(unit_life) && std::fabs(unit_life - life) <= 1.0f) WriteF32(unit + 0x150, want);
            if (WriteF32(e + kVLife, want)) {
                ++v.life_writes;
                if (v.life_writes <= 5 || (want <= 0 && life > 0))
                    Log("[vdrive] %s 0x%06X: life %.1f -> %.1f (the owner's)%s", vwire::KindName(v.key.kind), v.key.id, life, want,
                        want <= 0 ? ": destroyed by the owner's game" : "");
            }
        }
    }
    if (g_battery_ok && f.Has(vwire::kDrvBattery)) {
        float bat = 0;
        const float tol = (std::max)(0.005f, 0.01f * std::fabs(f.battery));
        if (mem::Rd(e + kVBattery, bat) && std::isfinite(bat) && std::fabs(bat - f.battery) > tol && WriteF32(e + kVBattery, f.battery)) {
            ++v.battery_writes;
            if (v.battery_writes <= 3) Log("[vdrive] %s 0x%06X: battery %.1f -> %.1f (the owner's)", vwire::KindName(v.key.kind), v.key.id, bat, f.battery);
        }
    }
    std::uint32_t f560 = 0, latch = 0;
    if (f.Has(vwire::kDrvBroken) && !v.applied_broken && mem::Rd(e + kVFlags560, f560) && !(f560 & 0x2000)) {
        v.applied_broken = true;
        const bool ok = OrBits(e + kVFlags560, 0x2000);
        Log("[vdrive] %s 0x%06X: broken on the owner's side: +0x560 |= 0x2000 %s", vwire::KindName(v.key.kind), v.key.id, ok ? "set" : "FAULTED");
    }
    if (f.Has(vwire::kDrvExploded) && !v.applied_exploded && mem::Rd(e + kVLatch, latch) && !(latch & 0x100000)) {
        v.applied_exploded = true;
        const bool ok = OrBits(e + kVReq564, 0x200000);
        Log("[vdrive] %s 0x%06X: exploded on the owner's side: +0x564 |= 0x200000 (the game's own explosion) %s", vwire::KindName(v.key.kind), v.key.id,
            ok ? "requested" : "FAULTED");
    }
}

// ---------------------------------------------------------------- owner side reads
void Measure(Veh& v, const WorldTransform& xf) {
    if (v.meas_have && g_now - v.meas_ms >= 20) {
        const double dt = (g_now - v.meas_ms) / 1000.0;
        v.vel[0] = static_cast<float>((xf.pos.x - v.meas_last.pos.x) / dt);
        v.vel[1] = static_cast<float>((xf.pos.y - v.meas_last.pos.y) / dt);
        v.vel[2] = static_cast<float>((xf.pos.z - v.meas_last.pos.z) / dt);
        const double yaw1 = std::atan2(v.meas_last.rot.m[1][1], v.meas_last.rot.m[1][0]), yaw2 = std::atan2(xf.rot.m[1][1], xf.rot.m[1][0]);
        double dy = yaw2 - yaw1;
        while (dy > 3.141592653589793) dy -= 6.283185307179586;
        while (dy < -3.141592653589793) dy += 6.283185307179586;
        v.avel[0] = v.avel[1] = 0;
        v.avel[2] = static_cast<float>(dy / dt);
        for (float& c : v.vel)
            if (!std::isfinite(c) || std::fabs(c) > 199.0f) c = 0;
        if (!std::isfinite(v.avel[2]) || std::fabs(v.avel[2]) > 49.0f) v.avel[2] = 0;
        v.speed_mps = std::sqrt(static_cast<double>(v.vel[0]) * v.vel[0] + static_cast<double>(v.vel[1]) * v.vel[1] + static_cast<double>(v.vel[2]) * v.vel[2]);
        v.meas_last = xf;
        v.meas_ms = g_now;
        v.rest.Update(v.speed_mps, g_now);
    } else if (!v.meas_have) {
        v.meas_have = true;
        v.meas_last = xf;
        v.meas_ms = g_now;
    }
}

bool FillFrame(Veh& v, std::uintptr_t e, const WorldTransform& xf, vwire::DriveType type, std::uint32_t sender_ms, vwire::DriveFrame* out) {
    vwire::DriveFrame f;
    f.type = type;
    f.key = v.key;
    f.epoch = v.own.epoch;
    f.seq = ++v.seq;
    f.sender_ms = sender_ms;
    f.pos[0] = xf.pos.x, f.pos[1] = xf.pos.y, f.pos[2] = xf.pos.z;
    vwire::QuatFromRows(Rows(xf), f.quat);
    std::memcpy(f.vel, v.vel, sizeof(f.vel));
    std::memcpy(f.avel, v.avel, sizeof(f.avel));
    std::int32_t st = 0;
    if (mem::Rd(e + kVRideState, st) && st >= 0 && st <= 15) f.ride_state = static_cast<std::uint8_t>(st);
    if (type == vwire::DriveType::drive) f.flags |= vwire::kDrvSeated;
    if (st == 2 || st == 4) f.flags |= vwire::kDrvEngine;
    if (v.rest.AtRest()) f.flags |= vwire::kDrvAtRest;
    if (g_groups_ok & kGState) {
        float life = 0, max = 0, kmh = 0;
        if (mem::Rd(e + kVLife, life) && mem::Rd(e + kVLifeMax, max) && std::isfinite(life) && std::isfinite(max) && max > 0 && max <= 1e6f) {
            f.life_max = max;
            f.life = life < 0 ? 0 : life > max ? max : life;
        }
        const std::uintptr_t hv = mem::Q(e + kVHavok);
        if (hv && mem::Rd(hv + 0xF0, kmh) && std::isfinite(kmh) && std::fabs(kmh) < 999.0f) f.speed_kmh = kmh;
        std::uint32_t f560 = 0, latch = 0;
        if (mem::Rd(e + kVFlags560, f560) && (f560 & 0x2000)) f.flags |= vwire::kDrvBroken;
        if (mem::Rd(e + kVLatch, latch) && (latch & 0x100000)) f.flags |= vwire::kDrvExploded;
    }
    if (g_battery_ok) {
        float bat = 0;
        if (mem::Rd(e + kVBattery, bat) && std::isfinite(bat) && bat >= 0 && bat < 1e6f) {
            f.battery = bat;
            f.flags |= vwire::kDrvBattery;
        }
    }
    if (type == vwire::DriveType::drive) {
        // the 48 bytes the game feeds the vehicle: [vehicle+0x528] + 0x08, read twice and equal
        const std::uintptr_t in = mem::Q(e + kVInput);
        static bool vt_logged[4] = {};
        if (in && v.key.kind < 4 && !vt_logged[v.key.kind]) {
            vt_logged[v.key.kind] = true;
            const std::uintptr_t ivt = mem::Q(in);
            Log("[vdrive] our %s's input object: vtable ds.exe+0x%llX (the follower's is ds.exe+0x%llX)", vwire::KindName(v.key.kind),
                static_cast<unsigned long long>(ivt - mem::Base()), static_cast<unsigned long long>(A(kInVt) - mem::Base()));
        }
        std::uint8_t a[vwire::kDriveInputBytes], b[vwire::kDriveInputBytes];
        if (!in || !mem::Read(in + 8, a, sizeof(a)) || !mem::Read(in + 8, b, sizeof(b)) || std::memcmp(a, b, sizeof(a)) != 0) return false;
        a[47] = 0;  // padding
        for (int i = 44; i < 47; ++i) a[i] = a[i] ? 1 : 0;
        // an axis beyond the wire's +-4 (none seen on the truck: run 20261119-103440) is clamped, a NaN zeroed, and logged once
        bool clamped = false;
        for (int i = 0; i < 11; ++i) {
            float x;
            std::memcpy(&x, a + 4 * i, 4);
            const float c = !std::isfinite(x) ? 0.0f : x > 4.0f ? 4.0f : x < -4.0f ? -4.0f : x;
            if (c != x || !std::isfinite(x)) clamped = true, std::memcpy(a + 4 * i, &c, 4);
        }
        static unsigned clamp_logs = 0;
        if (clamped && ++clamp_logs <= 5) Log("[vdrive] our %s's input axes went beyond +-4 or NaN: clamped for the frame", vwire::KindName(v.key.kind));
        if (!vwire::InputsValid(a)) return false;
        std::memcpy(f.input, a, sizeof(a));
    }
    *out = f;
    return true;
}

// ---------------------------------------------------------------- the tick parts
void ResetAll(const char* why) {
    DetachAll(why);
    for (auto& kv : g_veh) ClearFarPlace(kv.second);
    g_veh.clear();
    g_drive = LocalDrive{};
}

// Our Sam on a shared vehicle (its parent, the seat held by a player driver, not our follower).
void DetectLocalDrive(const TickIn& in) {
    LocalDrive now{};
    if (in.local_ok && in.local) {
        const std::uintptr_t e = mem::Q(reinterpret_cast<std::uintptr_t>(in.local) + kEParent);
        std::int32_t has = 0;
        std::uint32_t drv = 0;
        if (e && KindOfEntity(e) && mem::Rd(e + kVHasDriver, has) && has != 0 && mem::Rd(e + kVDriver, drv) && drv != kNoActor) {
            for (const vshare::Unit& u : g_units)
                if (u.entity == e) {
                    now.on = true;
                    now.key = u.key;
                    now.entity = e;
                    now.unit = u.unit;
                    break;
                }
        }
    }
    if (now.on && g_drive.on && now.key.id == g_drive.key.id) {
        g_drive.entity = now.entity, g_drive.unit = now.unit;
        return;
    }
    // stopped driving the earlier one
    if (g_drive.on) {
        Veh& v = V(g_drive.key);
        WorldTransform xf{};
        const bool have = ReadXf(g_drive.entity, xf) && EntityLive(g_drive.unit, g_drive.entity, v.key.kind);
        static unsigned offs = 0;
        Log("[vdrive] %s: our Sam got off %s 0x%06X (#%u) after %.1f s%s", g_role == 1 ? "host" : "guest", vwire::KindName(v.key.kind), v.key.id, ++offs,
            (g_now - g_drive.since) / 1000.0, WeOwn(v) ? "" : " (never ours)");
        if (WeOwn(v)) {
            SendOwn(vwire::OwnType::release, v.key, g_role == 1 ? vwire::kOwnerHost : vwire::kOwnerGuest, vwire::kReleaseGotOff, v.own.epoch, have ? &xf : nullptr,
                    v.vel);
            if (g_role == 2) {
                v.released_got_off = true;
                v.release_rest_sent = 0;
                v.rest.Reset();
            }
        }
        g_drive = LocalDrive{};
    }
    if (now.on) {
        g_drive = now;
        g_drive.since = g_now;
        Veh& v = V(now.key);
        v.force_offs = 0;
        v.claims = 0;
        static unsigned drives = 0;
        Log("[vdrive] %s: our Sam drives %s 0x%06X (owner %s, epoch %u) #%u", g_role == 1 ? "host" : "guest", vwire::KindName(v.key.kind), v.key.id,
            v.own.owner == vwire::kOwnerHost ? "host" : "guest", v.own.epoch, ++drives);
        if (v.block) Detach(v, "our Sam took the seat");
        if (g_role == 1) {
            v.host_drove_ms = g_now;
            const vwire::OwnStep st = vwire::HostOnHostTakes(v.own);
            if (st.send) {
                v.own = st.next;
                v.track.clear();
                SendOwn(st.type, v.key, st.next.owner, st.reason, st.next.epoch, nullptr, nullptr);
            }
        }
    }
}

void OwnershipTick(const TickIn& in) {
    (void)in;
    for (auto& kv : g_veh) {
        Veh& v = kv.second;
        if (g_role == 2) {
            // our Sam drives one that is not ours yet: CLAIM (again every 1.5 s, at most 5 times)
            if (g_drive.on && g_drive.key.id == v.key.id && v.own.owner != vwire::kOwnerGuest && v.force_offs == 0) {
                if (g_now - v.claim_ms > 1500 && v.claims < 5) {
                    v.claim_ms = g_now;
                    ++v.claims;
                    SendOwn(vwire::OwnType::claim, v.key, vwire::kOwnerGuest, vwire::kClaimDrive, v.own.epoch, nullptr, nullptr);
                }
            }
            // a host's DENY / GRANT(host) arrived while seated: off (retried by ForceOff's own pacing)
            if (g_drive.on && g_drive.key.id == v.key.id && v.force_offs > 0 && v.own.owner == vwire::kOwnerHost) ForceOff(v, "still on the host's vehicle");
            // ours, got off: at rest it goes back to the host
            if (v.own.owner == vwire::kOwnerGuest && v.released_got_off && !(g_drive.on && g_drive.key.id == v.key.id) && v.rest.AtRest() &&
                g_now - v.release_rest_ms > 3000 && v.release_rest_sent < 5) {
                const vshare::Unit* u = UnitFor(v.key.id);
                WorldTransform xf{};
                const bool have = u && EntityLive(u->unit, u->entity, v.key.kind) && ReadXf(u->entity, xf);
                v.release_rest_ms = g_now;
                ++v.release_rest_sent;
                SendOwn(vwire::OwnType::release, v.key, vwire::kOwnerGuest, vwire::kReleaseAtRest, v.own.epoch, have ? &xf : nullptr, v.vel);
            }
        } else if (g_role == 1) {
            // the guest owner went silent: back to the host
            if (v.own.owner == vwire::kOwnerGuest) {
                const vwire::OwnStep st = vwire::HostOnSilence(v.own, g_now - v.last_guest_frame_ms);
                if (st.send) {
                    Log("[vdrive] host: no frame from the guest for %s 0x%06X in %.0f s: the host's again", vwire::KindName(v.key.kind), v.key.id,
                        (g_now - v.last_guest_frame_ms) / 1000.0);
                    v.own = st.next;
                    v.release_detach_ms = g_now;
                    SendOwn(st.type, v.key, st.next.owner, st.reason, st.next.epoch, nullptr, nullptr);
                    continue;
                }
                // review fix: the guest owns it parked (no DRIVE frame) and its Sam stays away from it: back to the host
                WorldTransform place{};
                double pp[3] = {};
                const bool away = g_now - v.track_rx_ms > vwire::kAwayIdleMs && HostPlace(&v, v.key.id, &place) && vshare::PartnerPlace(pp) &&
                                  Dist3(place, pp) > vwire::kAwayM;
                if (!away) {
                    v.away_since_ms = 0;
                } else if (v.away_since_ms == 0) {
                    v.away_since_ms = g_now;
                } else if (g_now - v.away_since_ms > vwire::kAwayMs) {
                    const vwire::OwnStep take = vwire::HostOnHostTakes(v.own);
                    v.away_since_ms = 0;
                    if (take.send) {
                        Log("[vdrive] host: the guest owns %s 0x%06X parked and its Sam is %.0f m away from it: the host's again", vwire::KindName(v.key.kind),
                            v.key.id, Dist3(place, pp));
                        v.own = take.next;
                        v.release_detach_ms = g_now;
                        SendOwn(take.type, v.key, take.next.owner, vwire::kGrantTimeout, take.next.epoch, nullptr, nullptr);
                    }
                }
            } else {
                v.away_since_ms = 0;
            }
        }
    }
}

// The non-owner pushes a parked vehicle (it moves here while the owner's frames say it stands): the guest claims it, the host
// takes it.
void PushTick(const TickIn& in) {
    if (!in.local_ok || !in.local) return;
    WorldTransform sam{};
    if (!ReadXf(reinterpret_cast<std::uintptr_t>(in.local), sam)) return;
    for (auto& kv : g_veh) {
        Veh& v = kv.second;
        if (!PartnerOwns(v) || !v.have_park || v.block || g_now - v.park_rx_ms > 3000 || g_now - v.push_claim_ms < 2000) continue;
        if (g_drive.on && g_drive.key.id == v.key.id) continue;
        if (!v.track.empty() && v.track.back().key > v.park.key) continue;  // the partner drives it
        // our own contact only: not a follower still coasting after it let go, nor the partner's driving that just ended
        if (g_now - v.last_detach_ms < 2000 || g_now - v.track_rx_ms < 2500) continue;
        const vshare::Unit* u = UnitFor(v.key.id);
        WorldTransform xf{};
        if (!u || !EntityLive(u->unit, u->entity, v.key.kind) || !ReadXf(u->entity, xf)) continue;
        if (Dist3(xf, sam) > 6.0) continue;
        const WorldTransform target = XfOfFrame(v.park.f);
        if (Dist3(xf, target) <= 0.1 || v.speed_mps < 0.3) continue;  // moved, and moving now (a floating mirror at rest is not a push)
        v.push_claim_ms = g_now;
        if (g_role == 2) {
            SendOwn(vwire::OwnType::claim, v.key, vwire::kOwnerGuest, vwire::kClaimPush, v.own.epoch, nullptr, nullptr);
        } else {
            const vwire::OwnStep st = vwire::HostOnHostTakes(v.own);
            if (st.send) {
                Log("[vdrive] host: our Sam pushes the guest's %s 0x%06X: the host's now", vwire::KindName(v.key.kind), v.key.id);
                v.own = st.next;
                v.track.clear();
                v.have_park = false;
                SendOwn(st.type, v.key, st.next.owner, st.reason, st.next.epoch, nullptr, nullptr);
            }
        }
    }
}

// The frame pair at play time in a track (a / b bracket it; false when nothing plays yet).
bool Bracket(const std::deque<Frm>& t, double play, const Frm** a, const Frm** b, double* f) {
    if (t.empty() || play < t.front().key) return false;
    if (play >= t.back().key) {
        *a = *b = &t.back();
        *f = 0;
        return true;
    }
    for (std::size_t i = t.size() - 1; i > 0; --i) {
        const Frm& hi = t[i];
        const Frm& lo = t[i - 1];
        if (lo.key <= play && play <= hi.key) {
            *a = &lo, *b = &hi;
            *f = hi.key > lo.key ? (play - lo.key) / (hi.key - lo.key) : 1.0;
            return true;
        }
    }
    return false;
}

void FollowTick(const TickIn& in) {
    for (auto& kv : g_veh) {
        Veh& v = kv.second;
        const vshare::Unit* u = UnitFor(v.key.id);
        const bool partner_owner = PartnerOwns(v);
        const bool local_on_it = g_drive.on && g_drive.key.id == v.key.id;
        // drop played frames (keep 2 s behind the play time)
        if (in.have_play)
            while (v.track.size() > 2 && v.track[1].key < in.play_ms - 2000) v.track.pop_front();
        const Frm* a = nullptr;
        const Frm* b = nullptr;
        double f = 0;
        const bool have = in.have_play && partner_owner && Bracket(v.track, in.play_ms, &a, &b, &f);
        const bool stale = have && in.play_ms - v.track.back().key > 300;
        const bool ended = have && (in.play_ms - v.track.back().key > 1500 || (v.release_detach_ms > 0 && g_now >= v.release_detach_ms) ||
                                    (v.have_park && v.park.key > v.track.back().key && in.play_ms >= v.park.key));
        const bool want = have && !ended && !local_on_it && u && u->entity && (v.block || !stale);
        if (v.block && (!want || !u || u->unit != v.ctl_unit || u->entity != v.ctl_entity || !EntityLive(v.ctl_unit, v.ctl_entity, v.key.kind))) {
            Detach(v, !want ? (local_on_it ? "our Sam on it" : ended ? "the partner stopped driving it" : "no frames") : "the vehicle changed or went");
        }
        // (release_detach_ms stays set until the partner drives again: OnFrame clears it with the next DRIVE frame)
        if (ended && !v.block && !v.track.empty() && in.play_ms - v.track.back().key > 3000) v.track.clear();
        if (want && !v.block && g_now >= v.attach_retry_ms) {
            if (!Attach(v, *u)) {
                ++v.attach_fails;
                v.attach_retry_ms = g_now + (std::min)(10000.0, 500.0 * v.attach_fails);
            } else {
                v.attach_fails = 0;
                // the first placement: a vehicle far off is put there before it drives
                v.last_tp_ms = -1e18;
            }
        }
        if (!v.block || !have) continue;
        // two frames more than 0.5 s apart are not one motion (a gap in the partner's frames): the later one stands
        if (b->key - a->key > 500) a = b, f = 0;
        // inputs at the play time (the frame at or before it); stale frames: an idle driver
        std::uint8_t inputs[vwire::kDriveInputBytes];
        if (stale) IdleInputs(inputs);
        else std::memcpy(inputs, a->f.input, sizeof(inputs));
        WriteInputs(v.block, inputs);
        if (!stale) {
            const WorldTransform target = Lerp(a->f, b->f, f);
            float tvel[3];
            for (int k = 0; k < 3; ++k) tvel[k] = static_cast<float>(a->f.vel[k] + (b->f.vel[k] - a->f.vel[k]) * f);
            const float yaw_rate = static_cast<float>(a->f.avel[2] + (b->f.avel[2] - a->f.avel[2]) * f);
            Correct(v, *u, target, true, tvel, yaw_rate);
            ApplyState(v, u->unit, u->entity, a->f);
        }
    }
}

// Host: the guest parked it and handed it back (RELEASE at rest with its final place). Our follower stopped up to ~0.3 m
// from there (the driven ignore band; 0.21 m in run 20261119-120115), and as the owner again we would pull the guest's
// truck to our place. Ours goes where the guest parked it instead: one RequestTeleport (0.1 .. 5 m, nobody on it, where it
// fits), retried for 3 s while blocked.
void FinalPlaceTick() {
    if (g_role != 1) return;
    for (auto& kv : g_veh) {
        Veh& v = kv.second;
        if (v.final_place_until <= 0) continue;
        if (g_now > v.final_place_until || !WeOwn(v) || (g_drive.on && g_drive.key.id == v.key.id)) {
            v.final_place_until = 0;
            continue;
        }
        const vshare::Unit* u = UnitFor(v.key.id);
        if (!u || !u->entity || u->rider != vwire::kNoRider || !EntityLive(u->unit, u->entity, v.key.kind)) continue;
        WorldTransform cur{};
        if (!ReadXf(u->entity, cur)) continue;
        const WorldTransform target = XfOfFrame(v.park.f);
        const double err = Dist3(cur, target);
        if (err < 0.1 || err > 5.0) {
            v.final_place_until = 0;
            continue;
        }
        if (!Clear(u->unit, v.key.kind, target, g_local)) continue;
        v.final_place_until = 0;
        if (vshare::RequestTeleport(u->entity, target))
            Log("[vdrive] host: %s 0x%06X placed where the guest parked it (%.2f m)", vwire::KindName(v.key.kind), v.key.id, err);
    }
}

void ParkedTick() {
    for (auto& kv : g_veh) {
        Veh& v = kv.second;
        if (!PartnerOwns(v) || !v.have_park || v.block || g_now - v.park_rx_ms > 5000) continue;
        if (g_drive.on && g_drive.key.id == v.key.id) continue;
        if (!v.track.empty() && v.track.back().key > v.park.key) continue;  // the partner drives it (its frames play)
        if (g_now < v.next_corr_ms) continue;
        v.next_corr_ms = g_now + 100;
        const vshare::Unit* u = UnitFor(v.key.id);
        if (!u || !u->entity || u->rider != vwire::kNoRider) continue;  // not spawned here, or somebody rides it here
        // the unit list is the 10 Hz poll's: the entity checked again now, before the correction and the state writes
        if (!EntityLive(u->unit, u->entity, v.key.kind)) continue;
        const float zero[3] = {0, 0, 0};
        Correct(v, *u, XfOfFrame(v.park.f), false, zero, 0.0f);
        ApplyState(v, u->unit, u->entity, v.park.f);
    }
}

// Host: a vehicle the guest left out of our streaming range (NoteFarPlace) is put where the guest has it once its actor is
// back here and nobody else places it (our Sam not on it, no follower, the guest's frames not moving it).
void FarPlaceTick() {
    if (g_role != 1) return;
    for (auto& kv : g_veh) {
        Veh& v = kv.second;
        if (!v.far_place) continue;
        if ((g_drive.on && g_drive.key.id == v.key.id) || v.block) {
            ClearFarPlace(v);
            continue;
        }
        const vshare::Unit* u = UnitFor(v.key.id);
        if (!u || !u->entity || !EntityLive(u->unit, u->entity, v.key.kind)) continue;
        if (u->rider != vwire::kNoRider || (PartnerOwns(v) && g_now - v.park_rx_ms < 5000)) {
            ClearFarPlace(v);  // ridden, or the guest's PARK frames place it (ParkedTick)
            continue;
        }
        WorldTransform cur{};
        if (!ReadXf(u->entity, cur)) continue;
        const double err = Dist3(cur, v.far_xf);
        if (err < 0.1) {
            ClearFarPlace(v);
            continue;
        }
        if (!Clear(u->unit, v.key.kind, v.far_xf, g_local)) {
            if (g_now - v.blocked_log_ms > 5000) {
                v.blocked_log_ms = g_now;
                Log("[vdrive] host: %s 0x%06X is back here %.1f m from where the guest left it; that place is taken: waiting", vwire::KindName(v.key.kind),
                    v.key.id, err);
            }
            continue;
        }
        const bool ok = vshare::RequestTeleport(u->entity, v.far_xf);
        Log("[vdrive] host: %s 0x%06X is back here: placed where the guest left it (%.1f m) %s", vwire::KindName(v.key.kind), v.key.id, err,
            ok ? "" : "FAILED");
        ClearFarPlace(v);
    }
}

// Units whose actor went under our follower: when the unit is seen again with its id, the host puts it back on the recent
// player vehicles (Touch, what the live Detach does) and, once its actor is spawned again, the NPC bits are cleared.
void AfterGoneTick() {
    for (auto it = g_after_gone.begin(); it != g_after_gone.end();) {
        const vshare::Unit* u = nullptr;
        for (const vshare::Unit& x : g_units)
            if (x.unit == it->unit && x.key.id == it->id) u = &x;
        const Veh* v = FindV(it->id);
        if (g_now - it->t > 600000 || (v && v->block)) {
            it = g_after_gone.erase(it);
            continue;
        }
        if (!u || !u->entity || !EntityLive(u->unit, u->entity, it->kind)) {
            ++it;
            continue;
        }
        bool touched = false;
        if (g_role == 1 && (g_groups_ok & kGTouch)) {
            const std::uintptr_t r = mem::Q(A(kRegGlobal));
            std::uint32_t id = 0;
            if (r && mem::Rd(u->unit + 8, id) && id == u->local_id) touched = CallTouch(r + 0x58, u->unit);
        }
        const unsigned npc = ClearNpcBits(u->entity, true, 0, 0);
        Log("[vdrive] %s 0x%06X is spawned here again after our follower lost it%s%s", vwire::KindName(it->kind), it->id,
            touched ? ": put back on the recent vehicles (Touch)" : "", npc ? "; its NPC-driven bits cleared" : "");
        it = g_after_gone.erase(it);
    }
}

// Velocities of what we drive and own (for the frames and the rest detector), the guest's own place for the binding.
void MeasureTick(const TickIn& in) {
    WorldTransform sam{};
    const bool have_sam = in.local_ok && in.local && ReadXf(reinterpret_cast<std::uintptr_t>(in.local), sam);
    for (const vshare::Unit& u : g_units) {
        if (!u.entity) continue;
        Veh* v = FindV(u.key.id);
        const bool driving = g_drive.on && g_drive.key.id == u.key.id;
        const bool close_by = (have_sam && u.has_xf && Dist3(u.xf, sam) < 300) || (in.have_partner && u.has_xf && Dist3(u.xf, in.partner) < 300);
        if (!driving && !close_by && !(v && v->have_park)) continue;
        Veh& vv = v ? *v : V(u.key);
        if (!EntityLive(u.unit, u.entity, vv.key.kind)) continue;
        WorldTransform xf{};
        if (!ReadXf(u.entity, xf)) continue;
        Measure(vv, xf);
        if (g_role == 2 && WeOwn(vv) && g_now - vv.place_note_ms > 1000) {
            vv.place_note_ms = g_now;
            vshare::NoteHostPlace(vv.key.id, xf);  // the host follows us: the table entry moves with it
        }
    }
}

void TickBody(const TickIn& in, double now) {
    g_now = now;
    g_local = in.local;
    const double since_last = now - g_last_tick;
    g_last_tick = now;
    (void)since_last;
    // the link session and the world
    const std::uint32_t gen = link::Generation();
    if (gen != g_gen) {
        g_gen = gen;
        if (!g_veh.empty()) Log("[vdrive] a new link session: every vehicle is the host's again");
        ResetAll("a new link session");
    }
    std::uint32_t counter = 0;
    const bool ready = vshare::WorldReadyNow(&counter);
    if (ready != g_ready || counter != g_counter) {
        if (!g_veh.empty() || g_drive.on) Log("[vdrive] world %s (#%u): followers detached, the ownership starts over", ready ? "ready" : "not ready", counter);
        ResetAll("the world changed");
        g_after_gone.clear();  // the old world's units
        g_ready = ready;
        g_counter = counter;
        g_ready_since = now;
    }
    if (!ready) return;
    // this tick's units
    static vshare::Unit buf[256];
    const std::size_t n = vshare::SharedUnits(buf, 256);
    if (n == 0) return;  // no poll this tick (or no vehicle at all): nothing is decided on it
    g_units.assign(buf, buf + n);
    // a vehicle that left our world: its follower goes
    for (auto& kv : g_veh)
        if (kv.second.block && !UnitFor(kv.first)) Detach(kv.second, "the vehicle is gone here");
    DetectLocalDrive(in);
    MeasureTick(in);
    OwnershipTick(in);
    PushTick(in);
    FollowTick(in);
    ParkedTick();
    FinalPlaceTick();
    FarPlaceTick();
    AfterGoneTick();
}

bool TickGuarded(const TickIn& in, double now) noexcept {
    __try {
        TickBody(in, now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ProbeBattery() {
    // the battery field (+0x5A4) against the unit's 0..1 battery (+0x154): plausible on the first vehicles seen
    if (!(g_groups_ok & kGBattery) || g_battery_ok || g_battery_bad > 3) return;
    for (const vshare::Unit& u : g_units) {
        if (!u.entity) continue;
        float bat = 0, unit_bat = 0;
        if (!mem::Rd(u.entity + kVBattery, bat) || !mem::Rd(u.unit + 0x154, unit_bat)) continue;
        // +0x5A4 is the field the ride-state machine tests (<= 0: dead battery); it counts as the battery only when it agrees
        // with the unit's persisted 0..1 battery (run 20261119-103440: 1.00 against 0.663 / 0.000: not copied)
        const bool ok = std::isfinite(bat) && std::isfinite(unit_bat) && unit_bat >= 0 && unit_bat <= 1.0001f && std::fabs(bat - unit_bat) <= 0.02f;
        ++g_battery_reads;
        Log("[vdrive] battery probe: %s 0x%06X entity+0x5A4 = %.2f, unit+0x154 = %.3f: %s", vwire::KindName(u.key.kind), u.key.id, bat, unit_bat,
            ok ? "plausible" : "NOT plausible");
        if (!ok) ++g_battery_bad;
        else if (g_battery_reads - g_battery_bad >= 2) {
            g_battery_ok = true;
            Log("[vdrive] battery probe passed: the owner's battery (+0x5A4) is copied");
        }
        if (g_battery_bad > 3) Log("[vdrive] battery probe: +0x5A4 does not follow the unit's battery: the battery is not copied");
        if (g_battery_ok || g_battery_bad > 3) return;
        if (g_battery_reads >= 6) return;
    }
}

}  // namespace

// ---------------------------------------------------------------- public
void Install(const Config& config) noexcept {
    g_native_fx = config.vehicle_real_native;
    if (!vshare::Active()) {
        std::snprintf(g_off, sizeof(g_off), "the real shared vehicles are off");
        return;
    }
    g_role = vshare::Role();
    unsigned bad = 0;
    const char* first_bad = nullptr;
    unsigned checked = 0;
    for (const Site& s : kSites) {
        ++checked;
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            bad |= s.group;
            Log("[vdrive] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            if (!first_bad && (s.group & kRequired)) first_bad = s.name;
        }
    }
    auto structural = [&](bool ok, const char* what, unsigned group) {
        ++checked;
        if (ok) return;
        bad |= group;
        Log("[vdrive] byte check FAILED: %s", what);
        if (!first_bad && (group & kRequired)) first_bad = what;
    };
    const std::uintptr_t vt = A(kInVt);
    structural(mem::Q(vt) == A(0x142F52570) && mem::Q(vt + 8) == A(kInReset) && mem::Q(vt + 0x10) == A(0x142F5A950) && mem::Q(vt + 0x18) == A(0x142F6F470) &&
                   mem::Q(vt + 0x20) == A(0x142F6A990),
               "the input object's vtable 0x143E229B8", kGCtl);
    structural(mem::Q(A(kVehVt[1]) + 0x110) == A(kSetDriver) && mem::Q(A(kVehVt[3]) + 0x110) == A(kSetDriver) &&
                   mem::Q(A(kVehVt[2]) + 0x110) == A(kSetDriverTruck),
               "the vehicles' SetDriver slot (+0x110)", kGCtl);
    structural(mem::RipTarget(A(kRideOff), 3, 7) == A(kRideOffGlobal), "RideOffVehicleForcibly's global", kGRideOff);
    g_groups_ok = ~bad & 511u;
    if (bad & kRequired) {
        std::snprintf(g_off, sizeof(g_off), "this game build differs (%s)", first_bad ? first_bad : "byte check");
        Log("[vdrive] remote driving OFF: %s; the vehicles stay shared, a partner's driving does not move them here", g_off);
        return;
    }
    // our copy of the input vtable: [0] = the RTTI slot (vt - 8), [1..] = the slots; slot +0x08 is ours
    if (!mem::Read(vt - 8, g_vt_copy, sizeof(g_vt_copy))) {
        std::snprintf(g_off, sizeof(g_off), "the input vtable is not readable");
        Log("[vdrive] remote driving OFF: %s", g_off);
        return;
    }
    g_vt_copy[2] = reinterpret_cast<std::uintptr_t>(&InUpdate);
    g_vt_ready = true;
    for (std::uint8_t t = 16; t <= 19; ++t) vshare::RegisterHandler(t, &OnOwn);
    vshare::SetGuestOwnsQuery(&GuestOwnsQuery);
    vshare::SetFollowedQuery(&FollowedQuery);
    g_off[0] = 0;
    g_on.store(true);
    Log("[vdrive] remote driving ON (%s): %u byte checks; state copy %s, battery %s, native correction %s, nudge %s, recent-vehicle touch %s, "
        "lite attach (no AI drive component: trikes, roadster) %s, NPC bits cleared after a detach %s; vehicle_real_native=%d (%s)",
        g_role == 1 ? "host" : "guest", checked, (g_groups_ok & kGState) ? "on" : "OFF", (g_groups_ok & kGBattery) ? "probed at run time" : "OFF",
        (g_groups_ok & kGNative) ? "on" : "OFF", (g_groups_ok & kGNudge) ? "on" : "OFF", (g_groups_ok & kGTouch) ? "on" : "OFF",
        (g_groups_ok & kGLite) ? "on" : "OFF", (g_groups_ok & kGNpc) ? "on" : "OFF", g_native_fx ? 1 : 0,
        g_native_fx ? "the follower's own lamps and sounds only" : "the v0.14 lamp writes and the STA1 vehicle sounds stay on real vehicles");
}

bool Active() noexcept { return g_on.load() && vshare::Active(); }
const char* OffReason() noexcept { return Active() ? "" : g_off; }
bool NativeFx() noexcept { return g_native_fx; }

unsigned TakeSend(game::Entity* local, std::uint32_t sender_ms, double now, std::uint8_t (*out)[vwire::kMaxDriveFrameSize], std::size_t* sizes,
                  unsigned max) noexcept {
    if (!Active() || !g_ready || !out || !sizes || !max || !link::GetInfo().connected) return 0;
    (void)local;
    unsigned n = 0;
    __try {
        // the vehicle our Sam drives (ours: the host's always, the guest's once granted)
        if (g_drive.on) {
            Veh* v = FindV(g_drive.key.id);
            WorldTransform xf{};
            vwire::DriveFrame f;
            if (v && WeOwn(*v) && EntityLive(g_drive.unit, g_drive.entity, v->key.kind) && ReadXf(g_drive.entity, xf) &&
                FillFrame(*v, g_drive.entity, xf, vwire::DriveType::drive, sender_ms, &f)) {
                const std::size_t s = vwire::EncodeDrive(f, out[n], vwire::kMaxDriveFrameSize);
                if (s) sizes[n++] = s, ++g_tx_drive;
            }
        }
        // parked vehicles we own: 1 Hz, at once on a change (5 cm / 2 deg, at most 10 Hz each); the changed ones first
        for (int pass = 0; pass < 2 && n < max; ++pass) {
            for (const vshare::Unit& u : g_units) {
                if (n >= max) break;
                Veh* v = FindV(u.key.id);
                if (!v || !WeOwn(*v) || !u.entity || v->block || (g_drive.on && g_drive.key.id == u.key.id)) continue;
                if (!v->meas_have || now - v->meas_ms > 1000) continue;  // not measured: not near either player
                const WorldTransform& xf = v->meas_last;
                const bool changed = !v->park_sent_have || Dist3(xf, v->park_sent) > 0.05 ||
                                     vwire::AngleBetweenRows(Rows(xf), Rows(v->park_sent)) > 2.0 || (v->speed_mps > 0.05 && !v->rest.AtRest());
                const bool due = now - v->park_sent_ms >= 1000;
                if (pass == 0 ? !(changed && now - v->park_sent_ms >= 100) : !due) continue;
                if (!EntityLive(u.unit, u.entity, v->key.kind)) continue;
                vwire::DriveFrame f;
                if (!FillFrame(*v, u.entity, xf, vwire::DriveType::park, sender_ms, &f)) continue;
                const std::size_t s = vwire::EncodeDrive(f, out[n], vwire::kMaxDriveFrameSize);
                if (!s) continue;
                sizes[n++] = s;
                ++g_tx_park;
                v->park_sent_ms = now;
                v->park_sent = xf;
                v->park_sent_have = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[vdrive] FAULT reading our vehicles for the frames: remote driving stopped");
        EmergencyStop();
        return 0;
    }
    return n;
}

void OnFrame(const vwire::DriveFrame& f, double key, double now) noexcept {
    if (!Active() || !g_ready) return;
    // the partner in another world (still in the old one after a reload): its frames are not for our vehicles
    if (!vshare::PeerInOurWorld()) {
        ++g_rx_refused;
        return;
    }
    if (g_role == 1 && !FindV(f.key.id)) {
        ++g_rx_refused;  // the guest owns nothing it was not granted
        return;
    }
    Veh& v = V(f.key);
    if (v.key.kind != f.key.kind) {
        ++g_rx_refused;
        return;
    }
    bool accept = false;
    if (g_role == 1) {
        accept = vwire::HostAcceptsGuestFrame(v.own, f.epoch);
        if (accept) {
            // review fix: within reach of our unit or of the partner body (one bad frame never moves a host vehicle far)
            WorldTransform place{};
            const bool have_place = HostPlace(&v, v.key.id, &place);
            const double up[3] = {place.pos.x, place.pos.y, place.pos.z};
            double pp[3] = {};
            const bool have_p = vshare::PartnerPlace(pp);
            if (!vwire::FrameInReach(f.pos, have_place, up, have_p, pp)) {
                ++v.frames_refused;
                ++g_rx_refused;
                if (now - v.reach_log_ms > 5000) {
                    v.reach_log_ms = now;
                    Log("[vdrive] host: the guest's frame for %s 0x%06X at (%.1f, %.1f, %.1f) refused: more than %.0f m from our unit and from the partner body",
                        vwire::KindName(v.key.kind), v.key.id, f.pos[0], f.pos[1], f.pos[2], vwire::kFrameReachM);
                }
                return;
            }
            v.last_guest_frame_ms = now;
            if (f.type == vwire::DriveType::drive) vshare::NotePartnerDrove(v.key, now);
            // its actor not spawned here: the guest's place of it is kept (snapshot, and placed there when it is back)
            const vshare::Unit* u = UnitFor(v.key.id);
            if (u && !u->entity && (f.type == vwire::DriveType::park || now - v.park_rx_ms > 1000)) NoteFarPlace(v, XfOfFrame(f), "its frames");
        }
    } else {
        bool changed = false;
        v.own = vwire::GuestOnHostFrame(v.own, f.epoch, &changed);
        if (changed) {
            Log("[vdrive] guest: the host's frames for %s 0x%06X carry epoch %u: the host owns it", vwire::KindName(f.key.kind), f.key.id, f.epoch);
            v.released_got_off = false;
            if (g_drive.on && g_drive.key.id == f.key.id) ForceOff(v, "the host owns it");
        }
        accept = v.own.owner == vwire::kOwnerHost;
    }
    if (!accept) {
        ++v.frames_refused;
        ++g_rx_refused;
        return;
    }
    ++v.frames_rx;
    v.last_rx_ms = now;
    if (f.type == vwire::DriveType::drive) {
        ++g_rx_drive;
        if (!v.track.empty() && key <= v.track.back().key) return;  // old or repeated
        Frm fr;
        fr.key = key;
        fr.f = f;
        v.track.push_back(fr);
        while (v.track.size() > 120) v.track.pop_front();
        v.track_rx_ms = now;
        v.release_detach_ms = 0;  // it drives (again)
    } else {
        ++g_rx_park;
        if (v.have_park && v.park.key > 0 && key <= v.park.key) return;
        v.park.key = key;
        v.park.f = f;
        v.have_park = true;
        v.park_rx_ms = now;
    }
    if (g_role == 2 && now - v.place_note_ms > 250) {
        v.place_note_ms = now;
        vshare::NoteHostPlace(f.key.id, XfOfFrame(f));
    }
}

void Tick(const TickIn& in, double now) noexcept {
    if (!g_on.load()) {
        AfterStop();  // the followers the emergency stop left attached let go (once)
        if (!g_notice_done && vshare::Active() && g_off[0] && coop::Active()) {
            g_notice_done = true;
            char text[220];
            std::snprintf(text, sizeof(text), "Shared vehicles: remote driving off: %s.", g_off);
            coop::SetVehicleNotice(text, true, now + 60000);
        }
        return;
    }
    if (!vshare::Active()) {
        ResetAll("vehicle sharing stopped");
        return;
    }
    if (!TickGuarded(in, now)) {
        Log("[vdrive] FAULT in the remote driving tick: stopped");
        EmergencyStop();
        return;
    }
    if (g_battery_reads < 6 && !g_battery_ok) ProbeBattery();
}

void NewSession() noexcept {
    if (!g_on.load()) return;
    // handled on the next tick (the generation check), on the game thread
}

void EmergencyStop() noexcept {
    if (!g_on.exchange(false)) return;
    g_stopped = true;
    std::snprintf(g_off, sizeof(g_off), "stopped after a fault");
    for (auto& kv : g_veh)
        if (kv.second.block) {
            BufOf(kv.second.block)->on = 0;  // the inputs stop now; the controllers let go on the next game-thread pass (AfterStop)
            g_detach_after_stop = true;
        }
}

namespace {
void DetachAfterStopBody() {
    unsigned n = 0;
    for (auto& kv : g_veh)
        if (kv.second.block) {
            Detach(kv.second, "stopped after a fault");
            ++n;
        }
    for (auto& kv : g_veh) ClearFarPlace(kv.second);
    g_drive = LocalDrive{};
    Log("[vdrive] after the stop: %u follower(s) let go (the vehicles are free to board, the host's back on its recent vehicles)", n);
}
bool DetachAfterStopGuarded() noexcept {
    __try {
        DetachAfterStopBody();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

void AfterStop() noexcept {
    if (!g_detach_after_stop || g_on.load()) return;
    g_detach_after_stop = false;  // once, even if it faults
    if (!DetachAfterStopGuarded()) Log("[vdrive] FAULT while the followers let go after the stop: the rest stay attached");
}

bool SeatTarget(double play_ms, game::WorldTransform* target) noexcept {
    if (!Active() || !target) return false;
    for (auto& kv : g_veh) {
        Veh& v = kv.second;
        if (!v.block || !v.ctl_entity) continue;
        float seat[3], seat_r[3][3];
        std::uint8_t kind = 0;
        if (!vsync::PartnerSeatAt(play_ms, seat, seat_r, &kind) || kind != v.key.kind) return false;
        WorldTransform vx{};
        if (!EntityLive(v.ctl_unit, v.ctl_entity, v.key.kind) || !ReadXf(v.ctl_entity, vx)) return false;
        // one frame ahead along its velocity (the vehicle moves later in this frame than the body is placed)
        const double ahead = (std::min)(0.05, (std::max)(0.0, (g_now - v.meas_ms) / 1000.0) + 1.0 / 60.0);
        if (v.meas_have && g_now - v.meas_ms < 100 && v.speed_mps > 0.5 && v.speed_mps < 80) {
            vx.pos.x += v.vel[0] * ahead, vx.pos.y += v.vel[1] * ahead, vx.pos.z += v.vel[2] * ahead;
        }
        WorldTransform out = vx;
        for (int i = 0; i < 3; ++i) {
            const double o = static_cast<double>(seat[0]) * vx.rot.m[0][i] + static_cast<double>(seat[1]) * vx.rot.m[1][i] + static_cast<double>(seat[2]) * vx.rot.m[2][i];
            (i == 0 ? out.pos.x : i == 1 ? out.pos.y : out.pos.z) += o;
        }
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                out.rot.m[r][c] = seat_r[r][0] * vx.rot.m[0][c] + seat_r[r][1] * vx.rot.m[1][c] + seat_r[r][2] * vx.rot.m[2][c];
        *target = out;
        return true;
    }
    return false;
}

bool PartnerVehicleEntity(std::uintptr_t* entity) noexcept {
    if (!Active() || !entity) return false;
    std::uintptr_t e = 0;
    for (auto& kv : g_veh)
        if (kv.second.block && EntityLive(kv.second.ctl_unit, kv.second.ctl_entity, kv.second.key.kind)) e = kv.second.ctl_entity;
    if (!e) {
        double newest = -1e18;
        for (auto& kv : g_veh) {
            const Veh& x = kv.second;
            if (x.track_rx_ms < newest || x.track_rx_ms < -1e17) continue;  // the partner's DRIVE frames: it drove it (it may be ours again)
            const vshare::Unit* u = UnitFor(kv.first);
            if (u && u->entity) newest = x.track_rx_ms, e = u->entity;
        }
    }
    *entity = e;
    return e != 0;
}

namespace {
void FillView(const Veh& v, std::uintptr_t entity, std::uintptr_t unit, DrivenView* out) noexcept {
    DrivenView d;
    d.key = v.key;
    d.entity = entity;
    d.unit = unit;
    d.meas = v.meas_have && g_now - v.meas_ms < 200;
    if (d.meas) {
        std::memcpy(d.vel, v.vel, sizeof(d.vel));
        d.speed_mps = v.speed_mps;
    }
    if (!v.track.empty()) {
        const Frm& f = v.track.back();
        d.have_frame = true;
        d.frame_key = f.key;
        std::memcpy(d.frame_vel, f.f.vel, sizeof(d.frame_vel));
        d.frame_seated = f.f.Has(vwire::kDrvSeated);
        d.frame_broken = f.f.Has(vwire::kDrvBroken) || f.f.Has(vwire::kDrvExploded);
        d.frame_ride_state = f.f.ride_state;
        d.frame_rx_age_ms = g_now - v.track_rx_ms;
    }
    *out = d;
}
}  // namespace

void SetCarriedUnit(std::uintptr_t unit) noexcept { g_carried_unit.store(unit, std::memory_order_relaxed); }

bool LocalDriving(DrivenView* out) noexcept {
    if (!Active() || !g_drive.on) return false;
    const Veh* v = FindV(g_drive.key.id);
    if (!v || !EntityLive(g_drive.unit, g_drive.entity, g_drive.key.kind)) return false;
    if (out) FillView(*v, g_drive.entity, g_drive.unit, out);
    return true;
}

bool PartnerDriving(DrivenView* out) noexcept {
    if (!Active()) return false;
    for (auto& kv : g_veh) {
        const Veh& v = kv.second;
        if (!v.block || !v.ctl_entity || !EntityLive(v.ctl_unit, v.ctl_entity, v.key.kind)) continue;
        if (out) FillView(v, v.ctl_entity, v.ctl_unit, out);
        return true;
    }
    return false;
}

bool FrameAtPlay(std::uint32_t host_id, double play_ms, float (&vel)[3], double* key_ms) noexcept {
    const Veh* v = FindV(host_id);
    if (!v || v->track.empty()) return false;
    const Frm* a = nullptr;
    const Frm* b = nullptr;
    double f = 0;
    if (!Bracket(v->track, play_ms, &a, &b, &f) || !a) return false;
    std::memcpy(vel, a->f.vel, sizeof(vel));
    if (key_ms) *key_ms = a->key;
    return true;
}

bool PartnerDrivenEntity(std::uintptr_t* entity) noexcept {
    if (!Active()) return false;
    for (auto& kv : g_veh)
        if (kv.second.block && kv.second.ctl_entity && EntityLive(kv.second.ctl_unit, kv.second.ctl_entity, kv.second.key.kind)) {
            if (entity) *entity = kv.second.ctl_entity;
            return true;
        }
    return false;
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    switch (op) {
        case 0: {
            std::uint64_t tx = 0, rx = 0, dropped = 0;
            link::DriveCounts(&tx, &rx, &dropped);
            std::snprintf(msg, cap,
                          "%s %s%s; vehicles %zu; drive frames sent %llu, park %llu; received drive %llu park %llu refused %llu (link %llu/%llu, dropped %llu); "
                          "followers attached %llu detached %llu failed %llu; VOWN sent %llu received %llu (claims %llu grants %llu denies %llu forced off %llu); "
                          "input updates %ld; battery %s",
                          g_role == 1 ? "host" : g_role == 2 ? "guest" : "none", Active() ? "ON" : "OFF: ", Active() ? "" : g_off, g_veh.size(),
                          static_cast<unsigned long long>(g_tx_drive), static_cast<unsigned long long>(g_tx_park), static_cast<unsigned long long>(g_rx_drive),
                          static_cast<unsigned long long>(g_rx_park), static_cast<unsigned long long>(g_rx_refused), static_cast<unsigned long long>(tx),
                          static_cast<unsigned long long>(rx), static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(g_attaches),
                          static_cast<unsigned long long>(g_detaches), static_cast<unsigned long long>(g_attach_fail), static_cast<unsigned long long>(g_own_tx),
                          static_cast<unsigned long long>(g_own_rx), static_cast<unsigned long long>(g_claims), static_cast<unsigned long long>(g_grants),
                          static_cast<unsigned long long>(g_denies), static_cast<unsigned long long>(g_force_offs), static_cast<long>(g_in_calls_total),
                          g_battery_ok ? "copied" : "not copied");
            Log("[vdrive] status: %s", msg);
            return true;
        }
        case 1: {
            unsigned lines = 0;
            for (auto& kv : g_veh) {
                const Veh& x = kv.second;
                if (!x.block && x.frames_rx == 0 && x.own.epoch == 0 && !(g_drive.on && g_drive.key.id == kv.first)) continue;
                ++lines;
                Log("[vdrive] vehicle %s 0x%06X: owner %s epoch %u%s%s; frames rx %u refused %u, track %zu, park %s; error mean %.2f max %.2f m, angle max %.1f; "
                    "corrections none %u impulse %u interp %u teleport %u blocked %u nudge %u native fails %u; life writes %u battery writes %u",
                    vwire::KindName(x.key.kind), kv.first, x.own.owner == vwire::kOwnerHost ? "host" : "guest", x.own.epoch, x.block ? ", FOLLOWER attached" : "",
                    g_drive.on && g_drive.key.id == kv.first ? ", our Sam drives it" : "", x.frames_rx, x.frames_refused, x.track.size(), x.have_park ? "yes" : "no",
                    x.err_n ? x.err_sum / x.err_n : 0.0, x.err_max, x.ang_max, x.n_none, x.n_imp, x.n_interp, x.n_tp, x.n_blocked, x.n_nudge, x.n_native_fail,
                    x.life_writes, x.battery_writes);
            }
            return std::snprintf(msg, cap, "%u vehicle(s) with drive state logged", lines), true;
        }
        case 2:
            g_native_on = v[1] != 0;
            g_parked_native = v[2] != 0;
            return std::snprintf(msg, cap, "native net correction %s; parked vehicles: %s", g_native_on ? "on" : "off (nudge / teleports)",
                                 g_parked_native ? "the impulse band (native / nudge)" : "teleports"), true;
        case 3:
            g_ignore_m = v[1] > 0 && v[1] < 2 ? v[1] : 0.1;
            g_ignore_drive_m = v[2] > 0 && v[2] < 2 ? v[2] : 0.3;
            return std::snprintf(msg, cap, "ignore band %.2f m parked, %.2f m driven", g_ignore_m, g_ignore_drive_m), true;
        case 4:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            g_corr_on = v[1] != 0;
            return std::snprintf(msg, cap, "corrections %s", g_corr_on ? "on" : "OFF (open-loop replay)"), true;
        case 5:
            g_native_fx = v[1] != 0;
            return std::snprintf(msg, cap, "native fx %s", g_native_fx ? "on (no lamp writes / STA1 vehicle sounds on real vehicles)" : "off"), true;
        case 6: {
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            const int kind = static_cast<int>(v[1]);
            const double side = v[2] != 0 ? v[2] : 0.6;
            WorldTransform sam{};
            if (!g_local || !ReadXf(reinterpret_cast<std::uintptr_t>(g_local), sam)) return std::snprintf(msg, cap, "no local player"), false;
            const vshare::Unit* best = nullptr;
            double bd = 30;
            for (const vshare::Unit& u : g_units) {
                if (!u.entity || !u.has_xf || (kind && u.key.kind != kind)) continue;
                const double d = Dist3(u.xf, sam);
                if (d < bd) bd = d, best = &u;
            }
            if (!best) return std::snprintf(msg, cap, "no vehicle within 30 m"), false;
            WorldTransform to{};
            if (!ReadXf(best->entity, to)) return std::snprintf(msg, cap, "unreadable"), false;
            to.pos.x += to.rot.m[0][0] * side, to.pos.y += to.rot.m[0][1] * side;
            const bool ok = vshare::RequestTeleport(best->entity, to);
            return std::snprintf(msg, cap, "test push: %s 0x%06X moved %.2f m sideways %s", vwire::KindName(best->key.kind), best->key.id, side, ok ? "requested" : "FAILED"),
                   ok;
        }
        case 7: {
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            std::uintptr_t e = g_drive.on ? g_drive.entity : 0;
            if (!e) {
                WorldTransform sam{};
                double bd = 30;
                if (g_local && ReadXf(reinterpret_cast<std::uintptr_t>(g_local), sam))
                    for (const vshare::Unit& u : g_units)
                        if (u.entity && u.has_xf && Dist3(u.xf, sam) < bd) bd = Dist3(u.xf, sam), e = u.entity;
            }
            if (!e) return std::snprintf(msg, cap, "no vehicle"), false;
            float max = 0;
            mem::Rd(e + kVLifeMax, max);
            const float life = static_cast<float>(v[1] < 0 ? 0 : v[1]);
            const bool ok = std::isfinite(max) && max > 0 && WriteF32(e + kVLife, life > max ? max : life);
            return std::snprintf(msg, cap, "test: life %.1f of %.1f %s", life, max, ok ? "set" : "FAILED"), ok;
        }
        case 8:
            return std::snprintf(msg, cap, "battery probe: %s (%u reads, implausible %u)", g_battery_ok ? "passed" : "not passed", g_battery_reads, g_battery_bad), true;
        case 9: {
            // hidden tests: our Sam placed v[1] m (0 = 9) to the left of the vehicle the partner drives here (else the one it owns
            // that moved last), facing it, snapped to the ground: a watcher near the real vehicle (then vehicle_sync 18 aims the
            // camera at the nearest of its kind)
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            std::uintptr_t e = 0;
            PartnerVehicleEntity(&e);
            WorldTransform vx{};
            if (!e || !ReadXf(e, vx)) return std::snprintf(msg, cap, "the partner drives no vehicle here"), false;
            const double side = v[1] > 0 ? v[1] : 9.0;
            const double x = vx.pos.x - side * vx.rot.m[0][0], y = vx.pos.y - side * vx.rot.m[0][1];
            char why[160] = {};
            // v[2] != 0: placed at the vehicle's height without the ground snap (indoors: the snap put the watcher on the hill above
            // the underground garage, runs 20261119-112511 / -113900)
            const bool indoor = v[2] != 0;
            const bool ok = game::TeleportLocalPlayerFacing(x, y, vx.pos.z + (indoor ? 0.3 : 2.0), vx.pos.x - x, vx.pos.y - y,
                                                            indoor ? game::TeleportMode::place : game::TeleportMode::snap_to_ground, why, sizeof(why));
            return std::snprintf(msg, cap, "our Sam %.1f m left of the partner's vehicle at (%.2f, %.2f): %s", side, vx.pos.x, vx.pos.y, why), ok;
        }
    }
    return std::snprintf(msg, cap, "vehicle_drive: 0 status, 1 vehicles, 2 native 0/1, 3 ignore m, 4 corrections 0/1, 5 native fx 0/1, 6 kind [m] test push, 7 life test, 8 battery"),
           false;
}

}  // namespace st::vdrive

namespace st::game {
bool VehicleDriveCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept { return vdrive::Command(op, v, msg, cap); }
}  // namespace st::game
