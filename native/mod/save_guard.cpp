// The save guard (docs/real-partner-body.md section 24.12). See save_guard.hpp.
#include "save_guard.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include "automation.hpp"
#include "coop_session.hpp"
#include "MinHook.h"
#include "log.hpp"
#include "partner_source.hpp"
#include "ride_wire.hpp"

namespace st::saveguard {
namespace {
namespace mem = partner::mem;

constexpr std::uintptr_t kImage = 0x140000000ull;
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut)
constexpr std::uintptr_t kGameModuleGlobal = 0x147BE6318;  // GameModule*; +0x2A0 the SaveGameSystem S
constexpr std::uintptr_t kRequestsGlobal = 0x147C0D6B0;    // the save-request system W; [W+8] the pending-save holder M
constexpr std::uintptr_t kProcessor = 0x14341C350;         // void(M, float dt): the holder's per-frame processor (hooked: logs, tests)
constexpr std::uintptr_t kProcessorThunk = 0x14341C100;    // mov rcx,[rcx+8]; jmp kProcessor
constexpr std::uintptr_t kSaveTick = 0x143632D60;          // SaveGameSystem per frame: the thunk on [W], then the executor
constexpr std::uintptr_t kNothingPending = 0x14341C432;    // cmp byte [M+0xC8], 0xFF
constexpr std::uintptr_t kIssue = 0x14341C9AD;             // the holder's fields to the enqueue 0x143630440, M+0x118 = 1
constexpr std::uintptr_t kIsSaving = 0x1436323B0;          // S: the queue head (S+0x88 count, [[S+0x90]]+0xC state) in 1..4
constexpr std::uintptr_t kPendingOrSaving = 0x143409C30;   // W: [M+0xC8] != 0xFF or kIsSaving(GM+0x2A0)
constexpr std::uintptr_t kKick = 0x14362DB80;              // S: submits the prepared capture job S+0x98 (hooked: the hold)
constexpr std::uintptr_t kKickSlot = 0x143F199F0;          // the save system's vtable entry that holds kKick (render begin)
constexpr std::uintptr_t kExecJob = 0x1436338C6;           // the executor's state 1: S+0xA0 = task, S+0xAC = 0, task state 2
constexpr std::uintptr_t kResultSite = 0x143633A39;        // the executor: M+0x10C / +0x10D, then S+0x12F (1 ok)
constexpr std::uintptr_t kResultWrite = 0x14341D160;       // M+0x10C = type, M+0x10D = index of the finished save
constexpr std::uintptr_t kCreateSave = 0x14365C560;        // DSCreateSaveGame(u8 type, u16, u16, u16, u8 reuse_last_pos, void*)
constexpr std::uintptr_t kCreateSaveBody = 0x14365C770;    // its body: GameModule check, the request (id 0x01D0D8B5), the post
constexpr std::uintptr_t kReasons = 0x1433FEA20;           // void(M unused, u32* mask, u8, u8): why a save is refused now (queries)
constexpr std::uintptr_t kPostReasons = 0x143411745;       // the post 0x143411590: the call to kReasons, then M+0x114 (autosave cooldown)
constexpr std::uintptr_t kPostCooldown = 0x143411785;
constexpr std::uintptr_t kCanSave = 0x143634FB0;           // DSCanSaveGame: GameModule+0x328 (the save queue count) == 0

constexpr std::uintptr_t kGmSave = 0x2A0;
constexpr std::uintptr_t kSQueueCount = 0x88, kSQueue = 0x90, kSJob = 0x98, kSTask = 0xA0, kSInFlight = 0xA8, kSResult = 0x12F, kTaskState = 0x0C;
constexpr std::uintptr_t kTaskType = 0x10, kTaskIndex = 0x11, kTaskKind = 0x48;
constexpr std::uint8_t kPostLoadKind = 10;  // the post-load quick save's task kind (the executor's +0x48 == 10: its load flow)
constexpr std::uintptr_t kMReason = 0xC0, kMId = 0xC4, kMType = 0xC8, kMIndex = 0xC9, kMKind = 0xE1, kMIssued = 0x118;
constexpr std::uintptr_t kMLastType = 0x10C, kMLastIndex = 0x10D, kMCooldown = 0x114, kMFlags = 0xC2;
// The processor drops (clears, not delays) a request when the reason query 0x1433FEA20 returns any of these when it looks
// (a superset over every type: 0x1B1FBFFC for auto / manual, 0x4000, 0x10000, bits 0/1, 0x10 / 0x10000000): a hidden
// test that keeps a forced save pending releases it only while none is set (the guard never delays the game's own saves)
constexpr std::uint32_t kProcessorDropMask = 0x1B1FFFFF;
constexpr std::uintptr_t kGmSaveQueue = 0x328;
constexpr std::uint32_t kTypeMask = 0x1B3FBFFC;  // the post refuses an auto / manual save with any of these reasons
constexpr std::uint32_t kPostLoadId = 0x43005559;  // the post-load quick save (0x14263FA90: +0xC0 0x0F, kind 10)
constexpr std::uint32_t kCreateSaveId = 0x01D0D8B5;
constexpr ULONGLONG kRideRecentMs = 10000;  // the vehicle box check: within this long after our Sam was last carried

// kHook: the hold and its reads (every site the hold, Busy, InProgress or WantsOff rely on); kReads: the reads alone (a
// failed reads site turns the hold off too: no hold without them)
enum : std::uint32_t { kHook = 1, kReads = 2, kResult = 4, kForce = 8, kDiag = 16 };
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    std::uint32_t features;
};
// clang-format off
constexpr Site kSites[] = {
    {kProcessor,      "40 57 48 81 EC C0 00 00 00 48 8B 05 D0 4C B4 01 48 33 C4 48 89 84 24 90 00 00 00 C6 81 18 01 00 00 00 48 8B F9 48 8B 05 9C 9F 7C 04 C5 F8 29 B4",
                      "the save processor (M+0x118, dt in xmm1)", kHook},
    {kProcessorThunk, "48 8B 49 08 E9 47 02 00 00", "the processor thunk ([W+8])", kHook | kReads},
    {kSaveTick,       "40 53 48 83 EC 20 48 83 3D AA 35 5B 04 00 48 8B D9 74 1E 48 8B 0D 36 A9 5D 04 48 85 C9 74 05 E8 7C 93 DE FF 48 8B CB 48 83 C4 20 5B E9 AF 08 00 00 48 83 C4 20 5B C3",
                      "the save system's per-frame call (GameModule, W)", kHook | kReads},
    {kNothingPending, "80 BF C8 00 00 00 FF", "the processor's nothing-pending test (M+0xC8)", kHook | kReads},
    {kIssue,          "80 BF C0 00 00 00 0A 44 8B 8F CC 00 00 00 44 0F B6 87 C9 00 00 00 0F 95 C0 0F B6 97 C8 00 00 00 88 87 E2 00 00 00 0F B6 87 E1 00 00 00 48 89 7C 24 40 88 44 24 38 0F B6 87 E0 00 00 00 88 44 24 30 48 8B 87 D8 00 00 00 48 89 44 24 28 0F B6 87 D0 00 00 00 C6 87 18 01 00 00 01 48 8B 0D 09 99 7C 04 48 81 C1 A0 02 00 00 88 44 24 20 E8 21 3A 21 00",
                      "the processor's issue (the holder's fields, M+0x118 = 1, the enqueue)", kHook | kReads},
    {kIsSaving,       "83 B9 88 00 00 00 00 74 1D 48 8B 81 90 00 00 00 48 8B 08 8B 51 0C 8D 42 FF 83 F8 02 76 05 83 FA 04 75 03 B0 01 C3 32 C0 C3",
                      "the save system's is-saving query (S+0x88, S+0x90, head+0xC)", kHook | kReads},
    {kPendingOrSaving,"40 53 48 83 EC 20 48 8B D9 48 8B 0D D8 C6 7D 04 48 81 C1 A0 02 00 00 E8 64 87 22 00 84 C0 75 15 48 8B 43 08 80 B8 C8 00 00 00 FF 75 08 32 C0 48 83 C4 20 5B C3",
                      "the pending-or-saving query (GameModule+0x2A0, [W+8]+0xC8)", kHook | kReads},
    {kKick,           "48 89 5C 24 10 57 48 83 EC 30 40 32 FF 48 8B D9 48 83 B9 98 00 00 00 00 0F 84 97 00 00 00 48 8B 81 A0 00 00 00 48 85 C0 74 66 40 38 78 6D 74 60 4C 8B 15 F9 FA 5D 04 4D 85 D2 74 51 80 78 48 05 0F B6 48 10 44 0F B6 40 6F 41 0F 94 C3 80 F9 04 74 09 80 F9 01 74 04 33 D2 EB 02 B2 01 44 0F B6 48 6E 49 8B CA 44 88 44 24 20 45 0F B6 C3 E8 AD 46 DD FF 0F B6 F8 84 C0 74 16 FF 83 AC 00 00 00 83 BB AC 00 00 00 5A 0F 8E 9D 00 00 00 40 32 FF B8 01 00 00 00 F0 0F C1 83 A8 00 00 00 48 8B 93 98 00 00 00 48 8D 0D 55 DB 14 02 E8 10 85 3D FE 40 84 FF 75 75",
                      "the capture job's kick (S+0x98 job, S+0xA0 task, its own wait S+0xAC < 91, S+0xA8 in flight)", kHook | kReads},
    {kExecJob,        "48 8B 45 E7 48 89 87 A0 00 00 00 B8 FF FF FF FF 44 89 BF AC 00 00 00 C7 46 0C 02 00 00 00",
                      "the executor's capture job (S+0xA0 task, S+0xAC, task state 2)", kHook | kReads},
    {kResultSite,     "E8 22 97 DE FF 80 7E 48 0A 75 2D 80 BF 2F 01 00 00 01", "the executor's result (S+0x12F; task kind +0x48 == 10)", kResult | kHook},
    {kResultWrite,    "48 8B 41 08 88 90 0C 01 00 00 44 88 80 0D 01 00 00 C3", "the finished save's slot (M+0x10C / +0x10D)", kResult},
    {kCreateSave,     "48 83 EC 58 48 8D 44 24 40 48 C7 44 24 40 00 00 00 00 48 89 44 24 38 41 BA 40 00 00 00 48 8B 84 24 88 00 00 00 48 89 44 24 30 0F B6 84 24 80 00 00 00 88 44 24 28 66 44 89 54 24 20 48 C7 44 24 48 00 00 00 00 E8 C6 01 00 00 48 83 C4 58 C3",
                      "DSCreateSaveGame (the forced save, hidden tests)", kForce},
    {kCreateSaveBody, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC 20 01 00 00 48 8B 05 9E 48",
                      "DSCreateSaveGame's body", kForce},
    {kReasons,        "40 53 48 83 EC 20 48 89 6C 24 30 48 8B DA 48 89 74 24 40 41 0F B6 E9 C7 02 00 00 00 00 41 0F B6",
                      "the save refusal reasons (diagnostics, hidden tests)", kDiag},
    {kPostReasons,    "E8 D6 D2 FE FF", "the post's reasons call", kDiag},
    {kPostCooldown,   "C5 FA 10 86 14 01 00 00", "the post's autosave cooldown (M+0x114)", kDiag},
    {kCanSave,        "48 8B 05 61 13 5B 04 48 85 C0 74 0C 83 B8 28 03 00 00 00 75 03 B0 01 C3 32 C0 C3", "DSCanSaveGame (GameModule+0x328)", kDiag},
};
// clang-format on

using FnProcessor = void (*)(std::uintptr_t, float);
using FnKick = void (*)(std::uintptr_t);
using FnCreateSave = void (*)(std::uint8_t, std::uint16_t, std::uint16_t, std::uint16_t, std::uint8_t, void*);
using FnReasons = void (*)(std::uintptr_t, std::uint32_t*, std::uint8_t, std::uint8_t);
FnProcessor o_processor = nullptr;
FnKick o_kick = nullptr;

// ---------------------------------------------------------------- state
std::uint32_t g_features = 0;
std::atomic<bool> g_hooked{false};
std::atomic<bool> g_stopped{false};  // EmergencyStop: everything passes through, the ride's state is no longer read
std::atomic<bool> g_hold_on{true};  // Command 4 (hidden tests)
std::atomic<ULONGLONG> g_test_until{0};  // Command 6 / 7 (hidden tests): the next forced save stays pending (until then)
std::atomic<bool> g_test_board{false};   // Command 6: boarding allowed meanwhile (7: refused, as for any pending save)
std::atomic<bool> g_test_asleep{false};  // Command 6 [s] 1: kept pending until our Sam is carried AND asleep (a menu)
std::atomic<bool> g_test_fast{false};    // Command 6 [s] 2: kept pending until our Sam is carried AND the vehicle drives fast
std::atomic<bool> g_test_holding{false}; // the processor keeps a test save pending now (Command 6 / 7)
std::atomic<bool> g_forced_posted{false};// Command 1 posted a forced save the processor has not seen yet
constexpr double kTestFastMps = 8.0;
std::atomic<bool> g_awake{true};         // our Sam is awake (the tick's view; asleep in a menu)
char g_off[200] = "not installed";
bool g_notice_done = false;

// published by the ride (game thread), read by the detours
std::atomic<bool> g_carried{false}, g_placing{false};
std::atomic<std::uintptr_t> g_vehicle{0};
std::atomic<std::uint8_t> g_kind{0};
std::atomic<ULONGLONG> g_carried_tick{0};  // GetTickCount64 when our Sam was last seen carried
std::atomic<double> g_speed{0.0};  // the vehicle's speed while our Sam is carried (m/s)
std::atomic<std::uintptr_t> g_sam{0};
std::atomic<bool> g_want_off{false};
std::atomic<DWORD> g_tick_tid{0}, g_proc_tid{0}, g_kick_tid{0};

// the processor detour's own (the game thread)
bool g_have_req = false;
std::uint64_t g_req_key = 0;
unsigned g_req_n = 0;
unsigned g_req_frames = 0;   // processor calls with this request pending
bool g_req_waiting = false;  // kept pending: a test, or a capture the guard holds is queued (no second task)
ULONGLONG g_req_t0 = 0;      // GetTickCount64
bool g_watch_issue = false;  // log the processor's outcome for this request after a call
unsigned g_req_forced = 0;   // this request is the Command 1 forced save number n (0: the game's own)
bool g_req_post_load = false;
bool g_req_tested = false;   // the test kept it pending (its issue line says so)
bool g_req_capwait = false;  // it waited for a held capture (its issue line says so)

// the kick detour's own (its thread): the capture now prepared (S+0x98 job, S+0xA0 task)
std::uintptr_t g_cap_job = 0, g_cap_task = 0;
bool g_cap_post_load = false;
bool g_cap_holding = false;
bool g_cap_done = false;          // released, capped or run: never held again
bool g_cap_started_logged = false;
bool g_cap_wait_logged = false;
ULONGLONG g_cap_t0 = 0;
unsigned g_cap_frames = 0;
unsigned g_settled = 1000000;     // kick frames in a row with our Sam neither carried, placing nor in the vehicle's box
std::atomic<bool> g_cap_hold_now{false};       // a capture is held now (the processor keeps a second request meanwhile)
std::atomic<ULONGLONG> g_cap_hold_t0{0};       // since then (GetTickCount64)
std::atomic<std::uintptr_t> g_done_task{0};    // the task whose capture is no longer held (InProgress / WantsOff)

// how a save ran: the logs' words (the issue, capture and finish lines carry them; the hidden scenarios match them)
enum : int { kModeNone = 0, kModeAtOnce, kModeHeld, kModeCapped, kModeHoldOff, kModePostLoad, kModeTest, kModeQueued };
const char* ModeName(int m) noexcept {
    switch (m) {
        case kModeAtOnce: return "at once";
        case kModeHeld: return "after the hold";
        case kModeCapped: return "after the cap";
        case kModeHoldOff: return "the hold was off";
        case kModePostLoad: return "the post-load save";
        case kModeTest: return "after the test wait";
        case kModeQueued: return "after the held capture";
        default: return "?";
    }
}
// " (forced save F2)" or ""
void ForcedTag(unsigned n, char (&out)[32]) noexcept {
    if (n) std::snprintf(out, sizeof(out), " (forced save F%u)", n);
    else out[0] = 0;
}
std::atomic<unsigned> g_forced_n{0};  // Command 1's accepted forced saves
// the newest issued request (the processor detour), for the capture lines (the kick)
std::atomic<unsigned> g_issue_req{0}, g_issue_forced{0};
std::atomic<bool> g_issue_post_load{false};
// the newest capture (the kick detour), for the finish line (the tick)
std::atomic<unsigned> g_cap_req{0}, g_cap_forced{0};
std::atomic<int> g_cap_mode{0};

// counters (any thread reads)
std::atomic<unsigned> g_n_pending{0}, g_n_at_once{0}, g_n_held{0}, g_n_released{0}, g_n_capped{0}, g_n_issued{0}, g_n_dropped{0},
    g_n_gone{0}, g_n_ok{0}, g_n_failed{0}, g_n_queued_wait{0}, g_n_unplaced{0};

// the newest release: our Sam's place then, the vehicle's
struct Released {
    bool have = false;
    unsigned req = 0;
    double sam[3] = {};
    bool have_vehicle = false;
    double veh[3] = {};
    float rows[9] = {};
};
Released g_released;  // written by the kick detour, read by the commands (one writer)
// where our Sam stood when his newest carry started (the game thread): Command 2 tells a save of the placed spot from one of
// the boarding spot
bool g_have_board = false;
double g_board_pos[3] = {};

// the tick's save-system watch
int g_last_state = 0;   // the queue head's state, 0 none
bool g_was_busy = false;
double g_now = 0;
unsigned g_last_finished_type = 0xFF;
int g_last_finished_index = -1;
std::wstring g_newest_rel;
int g_newest_type = -1, g_newest_index = -1;

// ---------------------------------------------------------------- reads (fault-free: mem::Rd is ReadProcessMemory)
std::uintptr_t Holder() noexcept {
    const std::uintptr_t w = mem::Q(A(kRequestsGlobal));
    return w ? mem::Q(w + 8) : 0;
}
std::uintptr_t SaveSystem() noexcept {
    const std::uintptr_t gm = mem::Q(A(kGameModuleGlobal));
    return gm ? gm + kGmSave : 0;
}
struct Pending {
    bool on = false;
    std::uint8_t type = 0xFF, reason = 0, kind = 0;
    std::int8_t index = 0;
    std::uint32_t id = 0;
};
Pending ReadPending(std::uintptr_t m) noexcept {
    Pending p;
    std::uint8_t t = 0xFF;
    if (!m || !mem::Rd(m + kMType, t) || t == 0xFF) return p;
    p.on = true;
    p.type = t;
    mem::Rd(m + kMIndex, p.index);
    mem::Rd(m + kMReason, p.reason);
    mem::Rd(m + kMKind, p.kind);
    mem::Rd(m + kMId, p.id);
    return p;
}
bool PostLoadRequest(const Pending& p) noexcept { return p.on && (p.id == kPostLoadId || p.reason == 0x0F); }
std::int32_t QueueCount(std::uintptr_t s) noexcept {
    std::int32_t n = 0;
    return s && mem::Rd(s + kSQueueCount, n) ? n : 0;
}
std::uintptr_t HeadTask(std::uintptr_t s) noexcept {
    if (QueueCount(s) <= 0) return 0;
    const std::uintptr_t arr = mem::Q(s + kSQueue);
    return arr ? mem::Q(arr) : 0;
}
// The queue head's state (1 queued, 2 capture prepared / capturing, 3 captured, 4 writing, 5 cancelled, 6.. loads), 0 when
// the queue is empty.
int HeadState(std::uintptr_t s) noexcept {
    const std::uintptr_t head = HeadTask(s);
    std::int32_t st = 0;
    return head && mem::Rd(head + kTaskState, st) ? st : 0;
}
std::int32_t InFlight(std::uintptr_t s) noexcept {
    std::int32_t n = 0;
    return s && mem::Rd(s + kSInFlight, n) ? n : 0;
}
bool InProgressRead() noexcept {
    const std::uintptr_t s = SaveSystem();
    if (!s) return false;
    const int st = HeadState(s);
    return (st >= 1 && st <= 4) || mem::Q(s + kSJob) != 0;
}
bool HoldLive() noexcept { return g_hooked.load() && !g_stopped.load() && g_hold_on.load(); }
// The guard holds, or will hold, the capture of the save now pending, queued or prepared: none of it has run yet (no job in
// flight, the head not captured), it is not the post-load save and its capture was not released or capped already.
bool HoldCovers() noexcept {
    if (!HoldLive()) return false;
    const std::uintptr_t s = SaveSystem();
    if (!s || InFlight(s) != 0) return false;
    const int st = HeadState(s);
    if (st >= 3) return false;
    std::uintptr_t task = 0;
    if (mem::Q(s + kSJob)) task = mem::Q(s + kSTask);
    else if (st == 1) task = HeadTask(s);
    if (task) {
        if (task == g_done_task.load()) return false;
        std::uint8_t kind = 0;
        return mem::Rd(task + kTaskKind, kind) && kind != kPostLoadKind;
    }
    if (st != 0) return false;
    // nothing queued: a request in the holder is issued at once, and its capture held
    const Pending p = ReadPending(Holder());
    return p.on && !PostLoadRequest(p) && !g_test_holding.load();
}
const char* TypeName(std::uint8_t t) noexcept { return t == 1 ? "manual" : t == 2 ? "quick" : t == 4 ? "auto" : "?"; }
const char* StateName(int s) noexcept {
    switch (s) {
        case 0: return "none";
        case 1: return "queued";
        case 2: return "capture prepared";
        case 3: return "captured";
        case 4: return "writing";
        case 5: return "cancelled by the game";
        default: return s >= 6 && s <= 0x11 ? "a load" : "?";
    }
}

bool ReadPos(std::uintptr_t e, double (&p)[3]) noexcept {
    double b[3];
    if (!e || !mem::Read(e + 0xC8, b, sizeof(b))) return false;
    for (double v : b)
        if (!std::isfinite(v) || std::fabs(v) > 1e7) return false;
    std::memcpy(p, b, sizeof(p));
    return true;
}
bool ReadRows(std::uintptr_t e, float (&r)[9]) noexcept {
    float b[9];
    if (!e || !mem::Read(e + 0xE0, b, sizeof(b))) return false;
    for (float v : b)
        if (!std::isfinite(v) || std::fabs(v) > 1.5f) return false;
    std::memcpy(r, b, sizeof(r));
    return true;
}
// Our Sam's place in the vehicle's frame (right, forward, up).
void LocalIn(const double (&veh)[3], const float (&rows)[9], const double (&p)[3], double (&out)[3]) noexcept {
    const double d[3] = {p[0] - veh[0], p[1] - veh[1], p[2] - veh[2]};
    for (int i = 0; i < 3; ++i) out[i] = d[0] * rows[i * 3] + d[1] * rows[i * 3 + 1] + d[2] * rows[i * 3 + 2];
}
// "(x, y, z), in the truck's frame (r, f, u)" for the logs.
void DescribeSam(char* out, std::size_t cap, std::uintptr_t vehicle) noexcept {
    double p[3];
    if (!ReadPos(g_sam.load(), p)) {
        std::snprintf(out, cap, "unreadable");
        return;
    }
    double v[3];
    float r[9];
    if (vehicle && ReadPos(vehicle, v) && ReadRows(vehicle, r)) {
        double l[3];
        LocalIn(v, r, p, l);
        std::snprintf(out, cap, "at (%.2f, %.2f, %.2f), in the vehicle's frame (%.2f, %.2f, %.2f) m, %.2f m from its origin across", p[0], p[1], p[2], l[0], l[1],
                      l[2], std::sqrt(l[0] * l[0] + l[1] * l[1]));
    } else {
        std::snprintf(out, cap, "at (%.2f, %.2f, %.2f)", p[0], p[1], p[2]);
    }
}
// Our Sam's entity inside the box of the vehicle he rode last (just after a ride): 1 inside, 0 outside or not after a ride,
// -1 unknown (the vehicle or Sam unreadable). A Sam the game seats (a parent entity: the game's own driver or passenger) is
// the game's: 0.
int SamInVehicle() noexcept {
    const ULONGLONG t = g_carried_tick.load();
    if (!t || GetTickCount64() - t > kRideRecentMs) return 0;
    const std::uintptr_t sam = g_sam.load(), veh = g_vehicle.load();
    double p[3], v[3];
    float r[9];
    if (!ReadPos(sam, p) || !ReadPos(veh, v) || !ReadRows(veh, r)) return -1;
    if (mem::Q(sam + 0x70) != 0) return 0;
    return rwire::InVehicleBox(g_kind.load(), v, r, p) ? 1 : 0;
}

bool CallReasons(std::uintptr_t m, std::uint32_t* mask, std::uint8_t a = 0, std::uint8_t b = 0) noexcept;
// The reasons the processor itself would see for the request in `m` now (its own arguments: M+0xC2 bit 5, kind 5);
// false when they cannot be read (the diagnostics' byte checks failed, or the call faulted).
bool ProcessorReasons(std::uintptr_t m, const Pending& p, std::uint32_t* mask) noexcept {
    if (!(g_features & kDiag)) return false;
    std::uint8_t flags = 0;
    mem::Rd(m + kMFlags, flags);
    return CallReasons(m, mask, static_cast<std::uint8_t>((flags >> 5) & 1), static_cast<std::uint8_t>(p.kind == 5));
}
bool g_test_reason_logged = false;

// ---------------------------------------------------------------- the processor detour (the game thread): logs, tests
// The game's processor runs every frame, unchanged: a request is issued into the save queue as the game decides (the
// guard holds the capture, never the request: a poster may clear a request that is not queued by its next step, as the
// autosave task 0x143419760 does). Two waits only: a hidden test's forced save (Command 6 / 7), and a second request while
// a capture the guard holds is still prepared in the queue (the executor would give that one the capture job and orphan
// the held task; the game's own kick wait has the same window, one frame long without the hold).
// true: call the game's processor now; false: keep the request pending (return without calling it).
bool Decide(std::uintptr_t m) noexcept {
    const DWORD tid = GetCurrentThreadId();
    if (g_proc_tid.load() == 0) {
        g_proc_tid.store(tid);
        Log("[save] the game's save processor runs on thread %lu (the mod's tick: %lu)%s", tid, g_tick_tid.load(),
            g_tick_tid.load() && g_tick_tid.load() != tid ? ": NOT the same thread" : "");
    }
    const bool stopped = g_stopped.load();
    const bool carried = !stopped && g_carried.load(), placing = !stopped && g_placing.load();
    const Pending p = ReadPending(m);
    const ULONGLONG now = GetTickCount64();
    if (!p.on) {
        if (g_req_waiting) {
            ++g_n_gone;
            Log("[save] #%u: the waiting request is gone (the game cleared its holder: a load, a reset or a newer request of its own)", g_req_n);
        }
        g_req_waiting = false;
        g_test_holding.store(false);
        g_have_req = false;
        return true;
    }
    const std::uint64_t key = (static_cast<std::uint64_t>(p.id) << 32) | (static_cast<std::uint64_t>(p.type) << 24) |
                              (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p.index)) << 16) | (static_cast<std::uint64_t>(p.reason) << 8) | p.kind;
    if (!g_have_req || key != g_req_key) {
        if (g_req_waiting) Log("[save] #%u: the waiting request was replaced by another one (the game's own)", g_req_n);
        g_have_req = true;
        g_req_key = key;
        g_req_waiting = false;
        g_req_tested = false;
        g_req_capwait = false;
        g_test_reason_logged = false;
        g_test_holding.store(false);
        g_watch_issue = false;
        g_req_t0 = now;
        g_req_frames = 0;
        g_req_post_load = PostLoadRequest(p);
        g_req_forced = p.id == kCreateSaveId && g_forced_posted.exchange(false) ? g_forced_n.load() : 0;
        ++g_req_n;
        ++g_n_pending;
        char tag[32];
        ForcedTag(g_req_forced, tag);
        Log("[save] #%u pending%s: %s save, index %d, reason 0x%02X, kind %u, id 0x%08X; our Sam %s", g_req_n, tag, TypeName(p.type), p.index, p.reason, p.kind,
            p.id, carried ? "CARRIED (riding as a passenger)" : placing ? "being placed beside the vehicle" : "not carried");
    }
    ++g_req_frames;
    // hidden tests (Command 6 / 7, Command 1's own forced save only): the game refuses to post a save while our Sam is
    // carried (its reasons, Command 5), so the test posts one on foot and this keeps it pending until he boards; from then
    // on the processor issues it and the guard's capture hold takes over
    if (const ULONGLONG until = g_test_until.load(); until && g_req_forced && !stopped) {
        std::uint32_t why = 0;
        const bool ready = carried && (!g_test_asleep.load() || !g_awake.load()) && (!g_test_fast.load() || g_speed.load() >= kTestFastMps);
        if (ready && now < until && ProcessorReasons(m, p, &why) && (why & kProcessorDropMask)) {
            // the processor would drop it now (its player-state reasons, e.g. 0x400 while carried): the test's own delay
            // must not lose its save; kept pending until they clear (up to the test's deadline)
            if (!g_test_reason_logged) {
                g_test_reason_logged = true;
                Log("[save] #%u test: our Sam is carried, but the game's processor would drop the save now (reasons 0x%08X): kept pending until they clear",
                    g_req_n, why);
            }
            return false;
        }
        if (ready) {
            g_test_until.store(0);
            Log("[save] #%u test: our Sam is carried%s now (the vehicle at %.1f m/s); the guard's own rules from here", g_req_n,
                g_awake.load() ? "" : " and asleep (a menu)", g_speed.load());
        } else if (now < until) {
            if (!g_test_holding.load()) {
                g_test_holding.store(true);
                g_req_waiting = true;
                g_req_tested = true;
                Log("[save] #%u test: kept pending %s (up to %.0f s)", g_req_n,
                    !g_test_board.load()   ? "(no boarding meanwhile)"
                    : g_test_asleep.load() ? "until our Sam boards and sleeps (a menu)"
                    : g_test_fast.load()   ? "until our Sam is carried at 8 m/s or more"
                                           : "until our Sam boards",
                    static_cast<double>(until - now) / 1000);
            }
            return false;
        } else {
            g_test_until.store(0);
            Log("[save] #%u test: the test's wait is over (%s): the save goes to the game's processor", g_req_n,
                g_test_reason_logged ? "the game's drop reasons never cleared" : "nobody boarded");
        }
        g_test_holding.store(false);
        g_test_reason_logged = false;
        g_req_waiting = false;
    }
    // a capture the guard holds is prepared in the queue: no second task until it is submitted (at most the hold's cap)
    // (only while that hold is live: its job still there and within the cap and a second, so a kick that stops being called
    // (a world teardown) never keeps a request waiting)
    if (!stopped && g_cap_hold_now.load() && now - g_cap_hold_t0.load() < static_cast<ULONGLONG>(rwire::kSaveHoldCapMs) + 1000 &&
        mem::Q(SaveSystem() + kSJob) != 0) {
        if (!g_req_waiting) {
            g_req_waiting = true;
            g_req_capwait = true;
            ++g_n_queued_wait;
            Log("[save] #%u waits: the capture the guard holds is still prepared in the save queue (no second task meanwhile)", g_req_n);
        }
        return false;
    }
    if (!g_watch_issue) {
        g_watch_issue = true;
        if (g_req_waiting) Log("[save] #%u: the processor looks at it again now", g_req_n);
    }
    g_req_waiting = false;
    return true;
}

void AfterRun(std::uintptr_t m) noexcept {
    if (!g_watch_issue) return;
    std::uint8_t issued = 0;
    mem::Rd(m + kMIssued, issued);
    const Pending p = ReadPending(m);
    if (issued == 1) {
        ++g_n_issued;
        char where[200];
        DescribeSam(where, sizeof(where), g_vehicle.load());
        char tag[32];
        ForcedTag(g_req_forced, tag);
        const std::uintptr_t s = SaveSystem();
        const int mode = g_req_tested ? kModeTest : g_req_capwait ? kModeQueued : g_req_post_load ? kModePostLoad : kModeAtOnce;
        Log("[save] #%u%s issued to the save system by the game (%s, processor frame %u, %.0f ms after it was posted; the save queue now holds %d); our Sam %s, %s",
            g_req_n, tag, ModeName(mode), g_req_frames, static_cast<double>(GetTickCount64() - g_req_t0), QueueCount(s),
            g_carried.load() ? "CARRIED" : "not carried", where);
        g_issue_post_load.store(g_req_post_load);
        g_issue_forced.store(g_req_forced);
        g_issue_req.store(g_req_n);
        g_watch_issue = false;
        g_have_req = false;
    } else if (!p.on) {
        ++g_n_dropped;
        Log("[save] #%u not issued: the game's processor dropped it (its own rules)", g_req_n);
        g_watch_issue = false;
        g_have_req = false;
    }
    // else: the game's own wait (its processor keeps the request): looked at again next frame
}

void H_Processor(std::uintptr_t m, float dt) {
    if (!Decide(m)) return;
    o_processor(m, dt);
    AfterRun(m);
}

// ---------------------------------------------------------------- the kick detour: the hold (the save system's capture)
// The save system's executor turns the queued save into the capture job (S+0x98; the task S+0xA0 in state 2) and this kick
// submits it once per frame; the job reads our Sam's entity transform. The kick's own wait (the game's refusal reasons,
// up to 91 frames) returns before submitting, as this detour does: the save stays queued with its job prepared, exactly as
// the game made them, and the game's kick submits it on a later frame. Nothing waits on the job while it is not submitted
// (OnEndUpdateRendering joins S+0xA8, 0 then).
// true: call the game's kick now; false: hold.
bool KickDecide(std::uintptr_t s) noexcept {
    const DWORD tid = GetCurrentThreadId();
    if (g_kick_tid.load() == 0) {
        g_kick_tid.store(tid);
        Log("[save] the save system's capture kick runs on thread %lu (the mod's tick: %lu, the processor: %lu)", tid, g_tick_tid.load(), g_proc_tid.load());
    }
    const bool stopped = g_stopped.load();
    const bool carried = !stopped && g_carried.load(), placing = !stopped && g_placing.load();
    const std::uintptr_t job = mem::Q(s + kSJob);
    if (!job) {
        if (g_cap_holding) {
            ++g_n_gone;
            Log("[save] #%u: the held capture is gone (the game removed its job: a load or a reset)", g_cap_req.load());
        }
        g_cap_holding = false;
        g_cap_hold_now.store(false);
        g_cap_job = 0, g_cap_task = 0;
        if (InFlight(s) == 0 && HeadState(s) == 0) g_done_task.store(0);
        g_want_off.store(false);
        // settled frames count on: the ride's state, also between saves
        const int in_box = carried || placing ? 0 : SamInVehicle();
        if (carried || placing || in_box == 1) g_settled = 0;
        else if (g_settled < 1000000) ++g_settled;
        return true;
    }
    const std::uintptr_t task = mem::Q(s + kSTask);
    const ULONGLONG now = GetTickCount64();
    if (job != g_cap_job || task != g_cap_task) {
        if (g_cap_holding) Log("[save] #%u: the held capture was replaced by another job (the game's own)", g_cap_req.load());
        g_cap_job = job, g_cap_task = task;
        g_cap_holding = false;
        g_cap_hold_now.store(false);
        g_cap_done = false;
        g_cap_started_logged = false;
        g_cap_wait_logged = false;
        if (g_done_task.load() == task) g_done_task.store(0);
        std::uint8_t kind = 0, type = 0xFF;
        std::int8_t index = 0;
        if (task) mem::Rd(task + kTaskKind, kind), mem::Rd(task + kTaskType, type), mem::Rd(task + kTaskIndex, index);
        g_cap_post_load = kind == kPostLoadKind || g_issue_post_load.load();
        g_cap_forced.store(g_issue_forced.load());
        g_cap_req.store(g_issue_req.load());
        g_cap_mode.store(kModeNone);
        char tag[32];
        ForcedTag(g_cap_forced.load(), tag);
        Log("[save] #%u%s capture prepared by the save system (%s save, index %d, kind %u); our Sam %s", g_cap_req.load(), tag, TypeName(type), index, kind,
            carried ? "CARRIED" : placing ? "being placed" : "not carried");
    }
    const int in_box = carried || placing ? 0 : SamInVehicle();
    if (carried || placing || in_box == 1) g_settled = 0;
    else if (g_settled < 1000000) ++g_settled;
    rwire::SaveHoldIn in;
    in.pending = true;
    in.post_load = g_cap_post_load;
    in.done = g_cap_done;
    in.carried = carried;
    in.placing = placing;
    in.in_vehicle = in_box == 1;
    in.settled_frames = g_settled;
    in.held_ms = g_cap_holding ? static_cast<double>(now - g_cap_t0) : -1.0;
    const rwire::SaveStep step = HoldLive() ? rwire::SaveGate(in) : rwire::SaveStep::run;
    if (step == rwire::SaveStep::hold) {
        if (!g_cap_holding) {
            g_cap_holding = true;
            g_cap_t0 = now;
            g_cap_hold_t0.store(now);
            g_cap_hold_now.store(true);
            g_cap_frames = 0;
            ++g_n_held;
            Log("[save] #%u HELD: our Sam is %s: the ride ends and he is put beside the vehicle first (the save stays queued, its capture prepared and not "
                "run)",
                g_cap_req.load(), carried ? "carried" : placing ? "being placed" : in.in_vehicle ? "still in the vehicle's box" : "just off the ride");
        }
        ++g_cap_frames;
        if (carried) g_want_off.store(true);
        return false;
    }
    char where[200];
    if (g_cap_holding) {
        DescribeSam(where, sizeof(where), g_vehicle.load());
        if (step == rwire::SaveStep::run_capped) {
            ++g_n_capped;
            if (in.in_vehicle) ++g_n_unplaced;
            g_cap_mode.store(kModeCapped);
            Log("[save] #%u CAP: held %u frames (%.0f ms) and our Sam is still %s: the save runs unchanged now; Sam %s", g_cap_req.load(), g_cap_frames,
                static_cast<double>(now - g_cap_t0), carried ? "carried" : placing ? "being placed" : in.in_vehicle ? "in the vehicle's box (NOT placed)" : "settling",
                where);
        } else {
            ++g_n_released;
            g_cap_mode.store(kModeHeld);
            Log("[save] #%u released after %u frames (%.0f ms): our Sam is off the ride and placed (not carried, no placement waiting, %s, %u frames settled); "
                "Sam %s",
                g_cap_req.load(), g_cap_frames, static_cast<double>(now - g_cap_t0),
                in_box == 0 ? "outside the vehicle's box" : "his place against the vehicle unknown (it is unreadable)", g_settled, where);
            Released r;
            r.have = true;
            r.req = g_cap_req.load();
            const std::uintptr_t veh = g_vehicle.load();
            if (ReadPos(g_sam.load(), r.sam)) {
                r.have_vehicle = ReadPos(veh, r.veh) && ReadRows(veh, r.rows);
                g_released = r;
            }
        }
    } else if (!g_cap_done) {
        // the first frame of a capture that was never held
        ++g_n_at_once;
        g_cap_mode.store(g_cap_post_load ? kModePostLoad : !HoldLive() ? kModeHoldOff : kModeAtOnce);
        Log("[save] #%u capture runs at once: %s", g_cap_req.load(),
            g_cap_post_load ? "the post-load save (never held)" : stopped ? "the guard is stopped" : !g_hold_on.load() ? "the hold is off (test)" : "our Sam is not carried (no delay)");
    }
    g_cap_done = true;
    g_done_task.store(task);
    g_cap_holding = false;
    g_cap_hold_now.store(false);
    g_cap_frames = 0;
    g_want_off.store(false);
    return true;
}

void KickAfter(std::uintptr_t s) noexcept {
    if (!g_cap_job) return;
    if (mem::Q(s + kSJob) == 0) {
        if (!g_cap_started_logged) {
            g_cap_started_logged = true;
            char where[200];
            DescribeSam(where, sizeof(where), g_vehicle.load());
            char tag[32];
            ForcedTag(g_cap_forced.load(), tag);
            Log("[save] #%u%s capture started (%s): our Sam %s, %s", g_cap_req.load(), tag, ModeName(g_cap_mode.load()), g_carried.load() ? "CARRIED" : "not carried",
                where);
        }
    } else if (!g_cap_wait_logged) {
        g_cap_wait_logged = true;
        Log("[save] #%u: the game's kick waits by itself (its refusal reasons; up to 91 frames, then the capture job decides)", g_cap_req.load());
    }
}

void H_Kick(std::uintptr_t s) {
    if (!KickDecide(s)) return;
    o_kick(s);
    KickAfter(s);
}

// ---------------------------------------------------------------- diagnostics (hidden tests): why the game refuses a save now
bool CallReasons(std::uintptr_t m, std::uint32_t* mask, std::uint8_t a, std::uint8_t b) noexcept {
    __try {
        reinterpret_cast<FnReasons>(A(kReasons))(m, mask, a, b);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// One line: the reason bits the post 0x143411590 tests for an auto / manual save, its autosave cooldown and the save queue.
void LogWhy(const char* when, char* out, std::size_t cap) noexcept {
    std::uint32_t mask = 0;
    const std::uintptr_t m = Holder();
    float cooldown = -1;
    std::int32_t queue = -1;
    const std::uintptr_t gm = mem::Q(A(kGameModuleGlobal));
    if (m) mem::Rd(m + kMCooldown, cooldown);
    if (gm) mem::Rd(gm + kGmSaveQueue, queue);
    const bool ok = m && CallReasons(m, &mask);
    // the bits from the local player's state (0x14283EC70: player(0)): 0x10 0x200 0x400 0x1000 0x2000 0x1000000 0x10000000
    const std::uint32_t player = mask & 0x11003610u;
    std::snprintf(out, cap,
                  "%s: refusal reasons 0x%08X%s (for auto / manual 0x%08X%s; from the player's state 0x%X), autosave cooldown %.2f s, save queue %d; "
                  "our Sam %s",
                  when, mask, ok ? "" : " (the call FAULTED)", mask & kTypeMask, (mask & kTypeMask) ? ": REFUSED" : ": allowed", player, cooldown, queue,
                  g_carried.load() ? "carried" : "not carried");
    Log("[save] why %s", out);
}

// ---------------------------------------------------------------- the forced save (hidden tests)
bool CallCreateSave(std::uint8_t type) noexcept {
    __try {
        reinterpret_cast<FnCreateSave>(A(kCreateSave))(type, 0, 0, 0, 0, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The newest checkpoint.dat under the run's scratch save folder (hidden runs): its path below the root and its slot.
bool NewestSave(std::wstring* rel, int* type, int* index, double* age_s) {
    const wchar_t* root = automation::SavesScratchRoot();
    if (!root || !root[0]) return false;
    FILETIME best{};
    bool found = false;
    struct Dir {
        std::wstring path;
        int depth;
    };
    Dir stack[64];
    int n = 0;
    stack[n++] = {root, 0};
    while (n > 0) {
        const Dir d = stack[--n];
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d.path + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (std::wcscmp(fd.cFileName, L".") == 0 || std::wcscmp(fd.cFileName, L"..") == 0) continue;
            const std::wstring p = d.path + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (d.depth < 4 && n < 64) stack[n++] = {p, d.depth + 1};
            } else if (_wcsicmp(fd.cFileName, L"checkpoint.dat") == 0 && (!found || CompareFileTime(&fd.ftLastWriteTime, &best) > 0)) {
                best = fd.ftLastWriteTime;
                found = true;
                *rel = p.substr(std::wcslen(root));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!found) return false;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG a = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const ULONGLONG b = (static_cast<ULONGLONG>(best.dwHighDateTime) << 32) | best.dwLowDateTime;
    *age_s = a > b ? static_cast<double>(a - b) / 1e7 : 0.0;
    // the slot folder: ...\autosave12\checkpoint.dat
    *type = -1, *index = -1;
    const std::size_t slash = rel->find_last_of(L'\\');
    const std::size_t prev = slash == std::wstring::npos || slash == 0 ? std::wstring::npos : rel->find_last_of(L'\\', slash - 1);
    if (slash != std::wstring::npos && prev != std::wstring::npos) {
        const std::wstring folder = rel->substr(prev + 1, slash - prev - 1);
        struct K {
            const wchar_t* prefix;
            int type;
        };
        const K ks[] = {{L"autosave", 4}, {L"manualsave", 1}, {L"quicksave", 2}};
        for (const K& k : ks) {
            const std::size_t len = std::wcslen(k.prefix);
            if (folder.size() > len && _wcsnicmp(folder.c_str(), k.prefix, len) == 0) {
                *type = k.type;
                *index = _wtoi(folder.c_str() + len);
            }
        }
    }
    return true;
}

void Narrow(const std::wstring& w, char* out, std::size_t cap) {
    std::size_t i = 0;
    for (; i + 1 < cap && i < w.size(); ++i) out[i] = w[i] < 0x80 ? static_cast<char>(w[i]) : '?';
    if (cap) out[i] = 0;
}

// ---------------------------------------------------------------- the tick's watch of the save system
void Watch() {
    if (!(g_features & kReads)) return;
    const std::uintptr_t s = SaveSystem();
    const int st = s ? HeadState(s) : 0;
    const bool busy = st >= 1 && st <= 4;  // a save's states only (the job pointer may serve a load too)
    const int shown = st >= 1 && st <= 5 ? st : 0;
    if (shown != g_last_state) {
        char where[200];
        DescribeSam(where, sizeof(where), g_vehicle.load());
        if (shown) Log("[save] the save system: %s (state %d); our Sam %s, %s", StateName(shown), shown, g_carried.load() ? "CARRIED" : "not carried", where);
        g_last_state = shown;
    }
    if (g_was_busy && !busy) {
        std::uint8_t res = 0, lt = 0xFF;
        std::int8_t li = -1;
        if (g_features & kResult) {
            mem::Rd(s + kSResult, res);
            const std::uintptr_t m = Holder();
            if (m) mem::Rd(m + kMLastType, lt), mem::Rd(m + kMLastIndex, li);
        }
        const bool ok = res == 1;
        if (ok) ++g_n_ok;
        else ++g_n_failed;
        g_last_finished_type = lt;
        g_last_finished_index = li;
        char rel[260] = "-";
        std::wstring r;
        int t = -1, i = -1;
        double age = 0;
        if (NewestSave(&r, &t, &i, &age)) {
            Narrow(r, rel, sizeof(rel));
            g_newest_rel = r, g_newest_type = t, g_newest_index = i;
        }
        char tag[32];
        ForcedTag(g_cap_forced.load(), tag);
        Log("[save] finished %s: #%u%s (%s; the save system's result %u, slot type %u index %d); the newest save file in this run's scratch folder: %s (%s %d, "
            "written %.1f s ago)",
            (g_features & kResult) ? (ok ? "ok" : "NOT ok") : "(result unchecked)", g_cap_req.load(), tag, ModeName(g_cap_mode.load()), res, lt, li, rel,
            t == 4 ? "autosave" : t == 1 ? "manualsave" : t == 2 ? "quicksave" : "slot", i, age);
    }
    g_was_busy = busy;
}

bool WatchGuarded() noexcept {
    __try {
        Watch();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

// ---------------------------------------------------------------- public
void Install(const Config& config) noexcept {
    (void)config;
    std::uint32_t features = kHook | kReads | kResult | kForce | kDiag;
    unsigned bad = 0;
    for (const Site& s : kSites) {
        if (mem::CodeMatches(A(s.va), s.sig)) continue;
        ++bad;
        features &= ~s.features;
        Log("[save] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
        if ((s.features & (kHook | kReads)) && std::strcmp(g_off, "not installed") == 0) std::snprintf(g_off, sizeof(g_off), "this game build differs (%s)", s.name);
    }
    // structural: the save system's render-begin slot holds the kick (the detour is on the function; this proves it is the
    // one the save system calls)
    if (mem::Q(A(kKickSlot)) != A(kKick)) {
        ++bad;
        features &= ~(kHook | kReads);
        Log("[save] byte check FAILED: the save system's vtable slot 0x%llX does not hold the kick", static_cast<unsigned long long>(kKickSlot));
        if (std::strcmp(g_off, "not installed") == 0) std::snprintf(g_off, sizeof(g_off), "this game build differs (the save system's kick slot)");
    }
    // no hold without its reads (Busy, InProgress, WantsOff), and no reads without the layout the hold checks
    if (!(features & kReads)) features &= ~kHook;
    g_features = features;
    const unsigned n = static_cast<unsigned>(sizeof(kSites) / sizeof(kSites[0])) + 1;
    if (!(features & kHook)) {
        Log("[save] save guard OFF: %s (%u of %u checks failed); the ride's own exit while the game saves stays (reads %s)%s", g_off, bad, n,
            (features & kReads) ? "ok" : "OFF", (features & kReads) ? "" : "; boarding as a passenger is refused (no save check)");
        return;
    }
    void* proc = reinterpret_cast<void*>(A(kProcessor));
    void* kick = reinterpret_cast<void*>(A(kKick));
    MH_STATUS s = MH_CreateHook(proc, reinterpret_cast<void*>(&H_Processor), reinterpret_cast<void**>(&o_processor));
    if (s == MH_OK) {
        s = MH_CreateHook(kick, reinterpret_cast<void*>(&H_Kick), reinterpret_cast<void**>(&o_kick));
        if (s != MH_OK) MH_RemoveHook(proc);
    }
    if (s == MH_OK) {
        s = MH_EnableHook(proc);
        if (s == MH_OK) {
            s = MH_EnableHook(kick);
            if (s != MH_OK) MH_DisableHook(proc);
        }
        if (s != MH_OK) MH_RemoveHook(kick), MH_RemoveHook(proc);
    }
    if (s != MH_OK) {
        std::snprintf(g_off, sizeof(g_off), "the save hooks failed (%s)", MH_StatusToString(s));
        Log("[save] save guard OFF: %s", g_off);
        return;
    }
    g_off[0] = 0;
    g_hooked.store(true);
    Log("[save] save guard ON: the save system's capture kick 0x%llX and the save processor 0x%llX hooked; %u of %u byte checks ok (reads %s, result %s, "
        "forced save %s); a capture waits up to %.0f s while our Sam is carried",
        static_cast<unsigned long long>(kKick), static_cast<unsigned long long>(kProcessor), n - bad, n, (features & kReads) ? "on" : "OFF",
        (features & kResult) ? "on" : "off", (features & kForce) ? "on" : "off", rwire::kSaveHoldCapMs / 1000);
}

bool Active() noexcept { return g_hooked.load() && !g_stopped.load(); }
const char* OffReason() noexcept { return g_stopped.load() ? "stopped (the mod's emergency stop)" : g_hooked.load() ? "" : g_off; }

void Tick(game::Entity* local, bool awake, double now_ms) noexcept {
    g_now = now_ms;
    g_awake.store(awake);
    if (g_tick_tid.load() == 0) g_tick_tid.store(GetCurrentThreadId());
    g_sam.store(reinterpret_cast<std::uintptr_t>(local));
    if (!g_hooked.load() && !g_notice_done && std::strcmp(g_off, "not installed") != 0 && coop::Active()) {
        g_notice_done = true;
        char text[260];
        std::snprintf(text, sizeof(text), "Save guard off: %s. %s", g_off,
                      (g_features & kReads) ? "A save while riding as a passenger may record Sam at the seat." : "Riding as a passenger is refused.");
        coop::SetVehicleNotice(text, true, now_ms + 60000);
    }
    if (!WatchGuarded()) Log("[save] FAULT in the save watch (logging only)");
}

bool ReadsOk() noexcept { return (g_features & kReads) != 0; }
bool Busy() noexcept {
    if (!(g_features & kReads)) return false;
    const std::uintptr_t m = Holder();
    std::uint8_t t = 0xFF;
    if (m && mem::Rd(m + kMType, t) && t != 0xFF) return true;
    return InProgressRead();
}
bool InProgress() noexcept { return (g_features & kReads) && InProgressRead() && !HoldCovers(); }
bool WantsOff() noexcept {
    if (g_stopped.load()) return false;
    return g_want_off.load() || (g_carried.load() && HoldCovers());
}

void SetRideState(bool carried, bool placing, std::uintptr_t vehicle_entity, std::uint8_t kind, double speed_mps) noexcept {
    if (g_stopped.load()) return;
    g_placing.store(placing);
    g_speed.store(carried ? speed_mps : 0.0);
    if (carried && !g_carried.exchange(true)) g_have_board = ReadPos(g_sam.load(), g_board_pos);
    if (!carried) g_carried.store(false);
    if (carried) g_carried_tick.store(GetTickCount64());
    if (vehicle_entity) {
        g_vehicle.store(vehicle_entity);
        g_kind.store(kind);
    }
}

void EmergencyStop() noexcept {
    if (g_stopped.exchange(true)) return;
    g_carried.store(false);
    g_placing.store(false);
    g_want_off.store(false);
    g_test_until.store(0);
    g_test_holding.store(false);
    Log("[save] save guard stopped (the mod's emergency stop): every save and capture runs unchanged from now on");
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    switch (op) {
        case 0: {
            const std::uintptr_t m = Holder();
            const Pending p = ReadPending(m);
            const std::uintptr_t s = SaveSystem();
            char full[800];
            std::snprintf(full, sizeof(full),
                          "%s%s; hold %s; pending %s (type %u index %d id 0x%08X); save system %s; requests %u: at once %u, held %u, released %u, capped %u, "
                          "issued %u, dropped by the game %u, gone %u; finished ok %u, not ok %u; waited for a held capture %u, capped unplaced %u; threads: "
                          "processor %lu, kick %lu, tick %lu; our Sam %s; want off %d",
                          g_stopped.load() ? "STOPPED" : g_hooked.load() ? "ON" : "OFF: ", g_stopped.load() || g_hooked.load() ? "" : g_off,
                          g_hold_on.load() ? "on" : "OFF (test)", p.on ? "yes" : "no", p.type, p.index, p.id, StateName(s ? HeadState(s) : 0), g_n_pending.load(),
                          g_n_at_once.load(), g_n_held.load(), g_n_released.load(), g_n_capped.load(), g_n_issued.load(), g_n_dropped.load(), g_n_gone.load(),
                          g_n_ok.load(), g_n_failed.load(), g_n_queued_wait.load(), g_n_unplaced.load(), g_proc_tid.load(), g_kick_tid.load(), g_tick_tid.load(),
                          g_carried.load() ? "carried" : "not carried", WantsOff() ? 1 : 0);
            Log("[save] state: %s", full);
            std::snprintf(msg, cap, "%s", full);
            return true;
        }
        case 1: {
            if (!automation::Enabled() || !automation::SavesRedirected()) return std::snprintf(msg, cap, "hidden runs only (the save redirect active)"), false;
            if (!(g_features & kForce) || !(g_features & kReads)) return std::snprintf(msg, cap, "the forced save's byte checks failed"), false;
            if (!mem::Q(A(kGameModuleGlobal)) || !Holder()) return std::snprintf(msg, cap, "not in a world (no GameModule / save holder)"), false;
            if (Busy()) {
                Log("[save] test: no forced save: a save is pending or in progress already; our Sam %s", g_carried.load() ? "carried" : "not carried");
                // v[2] = 1: a refusal is an answer (as the game's own below)
                return std::snprintf(msg, cap, "a save is pending or in progress already"), v[2] != 0;
            }
            const int t = v[1] > 0 ? static_cast<int>(v[1]) : 4;
            if (t != 1 && t != 4) return std::snprintf(msg, cap, "type 4 (auto) or 1 (manual) only"), false;
            const bool ok = CallCreateSave(static_cast<std::uint8_t>(t));
            const Pending p = ReadPending(Holder());
            const bool accepted = p.on && p.id == kCreateSaveId;
            if (accepted) ++g_forced_n, g_forced_posted.store(true);
            Log("[save] test: DSCreateSaveGame(type %d) %s: %s F%u (holder type %u, id 0x%08X); our Sam %s", t, ok ? "called" : "FAULTED",
                accepted ? "ACCEPTED, pending as forced save" : p.on ? "another request is pending, not" : "REFUSED by the game (nothing pending), not", accepted ? g_forced_n.load() : 0u,
                p.type, p.id, g_carried.load() ? "carried" : "not carried");
            if (ok && !accepted && (g_features & kDiag)) {
                char why[300];
                LogWhy("the forced save was refused", why, sizeof(why));
            }
            // v[2] = 2: a second DSCreateSaveGame at once, while the first is still pending in the holder (two saves in a
            // row: what the game's post does with it; the guard sees one request per holder content)
            if (ok && accepted && v[2] == 2) {
                const bool ok2 = CallCreateSave(static_cast<std::uint8_t>(t));
                const Pending p2 = ReadPending(Holder());
                Log("[save] test: a second DSCreateSaveGame(type %d) while F%u is pending: %s; the holder now type %u, id 0x%08X, reason 0x%02X (%s)", t,
                    g_forced_n.load(), ok2 ? "called" : "FAULTED", p2.type, p2.id, p2.reason,
                    !p2.on ? "EMPTY" : (p2.type == p.type && p2.id == p.id && p2.reason == p.reason) ? "the same request" : "a different request");
            }
            std::snprintf(msg, cap, "forced %s save: %s", TypeName(static_cast<std::uint8_t>(t)), !ok ? "FAULT" : accepted ? "pending" : "refused");
            // v[2] = 1 (or 2): a refusal is an answer too (a scenario's `until` asks again until the game takes one)
            return ok && (accepted || v[2] != 0);
        }
        case 2: {
            double p[3];
            if (!ReadPos(g_sam.load(), p)) return std::snprintf(msg, cap, "our Sam is unreadable"), false;
            const std::uintptr_t sam = g_sam.load();
            const std::uintptr_t parent = mem::Q(sam + 0x70);
            char full[600];
            int n = std::snprintf(full, sizeof(full), "our Sam at (%.2f, %.2f, %.2f), %s, %s", p[0], p[1], p[2], parent ? "with a parent entity (a vehicle?)" : "no parent",
                                  g_carried.load() ? "carried" : "not carried");
            const Released r = g_released;
            if (r.have && n > 0 && static_cast<std::size_t>(n) < sizeof(full)) {
                const double dx = p[0] - r.sam[0], dy = p[1] - r.sam[1], dz = p[2] - r.sam[2];
                n += std::snprintf(full + n, sizeof(full) - n, "; the held save #%u was released with Sam at (%.2f, %.2f, %.2f): now %.2f m from there (dz %+.2f)", r.req,
                                   r.sam[0], r.sam[1], r.sam[2], std::sqrt(dx * dx + dy * dy + dz * dz), dz);
                if (r.have_vehicle && n > 0 && static_cast<std::size_t>(n) < sizeof(full)) {
                    double l[3];
                    LocalIn(r.veh, r.rows, p, l);
                    n += std::snprintf(full + n, sizeof(full) - n, "; in the frame the vehicle had then (%.2f, %.2f, %.2f) m", l[0], l[1], l[2]);
                }
            }
            if (g_have_board && n > 0 && static_cast<std::size_t>(n) < sizeof(full)) {
                const double bx = p[0] - g_board_pos[0], by = p[1] - g_board_pos[1], bz = p[2] - g_board_pos[2];
                std::snprintf(full + n, sizeof(full) - n, "; %.2f m from where he boarded last", std::sqrt(bx * bx + by * by + bz * bz));
            }
            // v[1] = n > 0: "where (check n)", so a scenario's waitlog (it searches the whole log) can name this one call
            if (v[1] > 0) Log("[save] where (check %d): %s", static_cast<int>(v[1]), full);
            else Log("[save] where: %s", full);
            std::snprintf(msg, cap, "%s", full);
            return true;
        }
        case 3: {
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            std::wstring rel;
            int t = -1, i = -1;
            double age = 0;
            if (!NewestSave(&rel, &t, &i, &age) || t < 0 || i < 0) return std::snprintf(msg, cap, "no slot save in this run's scratch folder"), false;
            char r8[260];
            Narrow(rel, r8, sizeof(r8));
            Log("[save] test: loading the newest save of this run: %s (type %d index %d, written %.1f s ago)", r8, t, i, age);
            const double cv[3] = {5, static_cast<double>(t), static_cast<double>(i)};
            return coop::Command(5, cv, msg, cap);
        }
        case 4:
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            g_hold_on.store(v[1] != 0);
            Log("[save] test: the hold %s", v[1] != 0 ? "ON" : "OFF (the ride's own exit alone)");
            return std::snprintf(msg, cap, "hold %s", v[1] != 0 ? "on" : "off"), true;
        case 5: {
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only (it calls the game's reason query)"), false;
            if (!(g_features & kDiag)) return std::snprintf(msg, cap, "the diagnostics' byte checks failed"), false;
            char why[300];
            LogWhy("now", why, sizeof(why));
            std::snprintf(msg, cap, "%s", why);
            return true;
        }
        case 6:
        case 7: {
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            const double s = v[1] > 0 ? v[1] : 15.0;
            g_test_board.store(op == 6);
            g_test_asleep.store(op == 6 && v[2] == 1);
            g_test_fast.store(op == 6 && v[2] == 2);
            g_test_until.store(GetTickCount64() + static_cast<ULONGLONG>(s * 1000));
            Log("[save] test: the next forced save stays pending %s (up to %.0f s)",
                op == 7 ? "(boarding refused meanwhile)" : v[2] == 1 ? "until our Sam boards and sleeps" : v[2] == 2 ? "until our Sam is carried at 8 m/s or more" : "until our Sam boards",
                s);
            return std::snprintf(msg, cap, "the next forced save stays pending (%.0f s)", s), true;
        }
        default: return std::snprintf(msg, cap, "unknown op %d", op), false;
    }
}

bool TestAllowsCarry() noexcept {
    const ULONGLONG until = g_test_until.load();
    if (!until || !g_test_board.load()) return false;
    // expired (the forced save was refused, never posted or cleared by the game): off for good, the real rules again
    if (GetTickCount64() >= until + 1000) {
        ULONGLONG expected = until;
        if (g_test_until.compare_exchange_strong(expected, 0)) Log("[save] test: the boarding allowance expired (no forced save was kept pending until a boarding)");
        return false;
    }
    return true;
}

}  // namespace st::saveguard
