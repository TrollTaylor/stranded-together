// The host-held save session, game side. See coop_session.hpp and docs/coop-session.md.
#include "coop_session.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <new>
#include <string>
#include <vector>

#include "MinHook.h"
#include "automation.hpp"
#include "coop_rules.hpp"
#include "debug_actions.hpp"
#include "game_actions.hpp"
#include "game_api.hpp"
#include "link.hpp"
#include "log.hpp"
#include "overlay/overlay.hpp"
#include "overlay/overlay_input.hpp"
#include "partner_source.hpp"
#include "save_xfer.hpp"
#include "steam_owner.hpp"
#include "title_wait.hpp"

namespace st::coop {
namespace {
namespace mem = partner::mem;

constexpr std::uintptr_t kImage = 0x140000000ull;  // the addresses below are ds.exe VAs at its preferred base
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe, Director's Cut)
constexpr std::uintptr_t kTitleFlow = 0x14327B1A0;     // DSUITitleMenuFunction OnStartGameFlowImpl(this): dispatch on this+0x130
constexpr std::uintptr_t kLoadDecide = 0x14324F4B0;    // the load page's decision (this)
constexpr std::uintptr_t kTitleLoad = 0x142624A70;     // load at the title (app, u8 type, s8 index): Continue = (0xF, -1)
constexpr std::uintptr_t kConvLoad = 0x142624C80;      // load the converted original-game save (app): quicksave newest
constexpr std::uintptr_t kNewWorld = 0x1426393E0;      // a new world (app, world): New Game
constexpr std::uintptr_t kWorldLoadReq = 0x14362D750;  // in-world load request (session+0x2A0, type, index, u8, reason, u32, u8)
constexpr std::uintptr_t kSlotName = 0x143635630;      // slot name from the slot cache (out string*, type, index, ...)
constexpr std::uintptr_t kStrRelease = 0x14190AD20;    // Decima string release (string*)
constexpr std::uintptr_t kSlotRefresh = 0x141A00DA0;   // rescan every registered slot ("Update cached slot info")
constexpr std::uintptr_t kHasSlot = 0x1426376E0;       // the slot exists in the cache (app, type, index)
constexpr std::uintptr_t kPauseLoad = 0x142636990;     // the pause menu's Load Game (app, type, index)
constexpr std::uintptr_t kQuitGame = 0x142642B20;      // GameModule.QuitGame: 0x142635B50([session], 1, 0)
constexpr std::uintptr_t kContinueLast = 0x142642A00;  // Application.ContinueLastSave: names the session and app globals
constexpr std::uintptr_t kBuildString = 0x141A13610;   // the game's build string (a Decima string: char* at +0)
constexpr std::uintptr_t kSaveVersion = 0x143635620;   // SaveGame version (mov al, 0x43)
constexpr std::uintptr_t kSaveFlags = 0x1436355A0;     // save flags (u8): called with 0
constexpr std::uintptr_t kRetryGuard = 0x14362B4A0;    // (session+0x2A0): a checkpoint exists (hidden tests: Command 7)
constexpr std::uintptr_t kSessionGlobal = 0x147BE6318; // the game session: 0 at the title
constexpr std::uintptr_t kAppGlobal = 0x147BE6330;     // the Application object

// What each group of addresses turns on. A failed group turns its feature off for the run (one line in the log and on
// the overlay); the session needs kCommon plus kHostDetect (host) or kGate + kRefresh (guest).
enum Group : unsigned {
    kCommon = 1u,       // the session / app globals and the version functions
    kHostDetect = 2u,   // host: which save the host starts
    kGate = 4u,         // guest: the hard gate on the title menu
    kRefresh = 8u,      // guest: the slot cache refresh (the received save becomes visible)
    kLoadCall = 16u,    // guest: the direct load calls (else the hidden-run virtual-pad fallback)
    kQuit = 32u,        // guest: QuitGame (follow the host to the title)
    kTestRetry = 64u,   // hidden tests only: the checkpoint retry request (Command 7)
};
struct Site {
    std::uintptr_t va;
    const char* sig;
    const char* name;
    unsigned groups;
};
// clang-format off
constexpr Site kSites[] = {
    {kContinueLast, "48 83 3D 10 39 5A 05 00 75 11 41 B0 FF 48 8D 0D 1C 39 5A 05 B2 0F E9 55 20 FE FF C3", "ContinueLastSave (session and app globals)", kCommon},
    {kBuildString,  "40 53 48 83 EC 50 65 48 8B 04 25 58 00 00 00 8B 0D", "game build string", kCommon},
    {kSaveVersion,  "B0 43 C3", "SaveGame version", kCommon},
    {kSaveFlags,    "48 89 5C 24 08 57 48 83 EC 20 0F B6 D9 33 FF E8", "save flags", kCommon},
    {kTitleLoad,    "40 56 41 54 41 57 48 81 EC 90 00 00 00 48 8B 05", "title load", kHostDetect | kLoadCall},
    {kConvLoad,     "40 56 48 81 EC 80 00 00 00 48 8B 05 A0 C3 93 02", "converted load", kHostDetect | kGate},
    {kNewWorld,     "48 89 5C 24 18 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 83 EC 30 80 B9", "new world", kHostDetect | kGate},
    {kWorldLoadReq, "48 89 5C 24 20 88 54 24 10 55 57 41 54 41 55 41 56 48 83 EC 30", "in-world load request", kHostDetect},
    {kSlotName,     "40 55 53 56 57 41 55 41 56 41 57 48 8D 6C 24 F9 48 81 EC A0 00 00 00", "slot name resolver", kHostDetect},
    {kStrRelease,   "40 53 48 83 EC 20 48 8B 19 48 8D 05", "string release", kHostDetect},
    {kTitleFlow,    "40 53 48 83 EC 20 48 8B D9 E8 F2 73 D9 FF BA 12 75 ED 63", "title menu start flow", kGate},
    {kLoadDecide,   "40 53 48 83 EC 20 48 8B D9 48 8B 89 B8 00 00 00", "load page decision", kGate},
    {kSlotRefresh,  "4C 8B DC 55 49 8D AB 28 FE FF FF 48 81 EC D0 02 00 00", "slot cache refresh", kRefresh},
    {kHasSlot,      "48 83 EC 68 48 8B 05 45 99 92 02 48 33 C4", "slot exists", kLoadCall},
    {kPauseLoad,    "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 40 41 0F B6 F0 0F B6 EA", "pause-menu load", kLoadCall},
    {kQuitGame,     "48 8B 0D F1 37 5A 05 45 33 C0 41 8D 50 01 E9 1D 30 FF FF", "QuitGame", kQuit},
    {kRetryGuard,   "48 8B 81 C8 00 00 00 48 85 C0 74 19 80 78 24 00 75 10 66 83", "checkpoint exists (tests)", kTestRetry},
};
// clang-format on

constexpr double kLinkLossMs = 60000;       // D1-5: a lost link this long sends the guest back to the title
constexpr double kHostTitleMs = 3000;       // the host at the title this long: the guest follows (D1-4)
constexpr double kQuitWaitMs = 30000;       // QuitGame must reach the title within this
constexpr double kLoadTimeoutMs = 150000;   // the guest's load must reach a player within this
constexpr double kTeleportOfferMs = 90000;  // D1-7: the one-time "Teleport to host" offer
constexpr double kNoReplyMs = 15000;        // connected this long without the partner's session messages: say so
constexpr unsigned kSlotBase = 5;           // the guest's slots: manualsave5 / manualsave6 (never an autosave)

// ---------------------------------------------------------------- function types
using FnVoid1 = void (*)(std::uintptr_t);
using FnTitleLoad = std::uint8_t (*)(std::uintptr_t, std::uint8_t, std::uint8_t);
using FnConvLoad = std::uint64_t (*)(std::uintptr_t);
using FnNewWorld = void (*)(std::uintptr_t, std::uintptr_t);
using FnWorldReq = std::uint64_t (*)(std::uintptr_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using FnResolve = void* (*)(void*, std::uint8_t, std::uint8_t, std::uint8_t*, std::uint8_t*, std::uint16_t*, std::uint64_t*);
using FnHasSlot = std::uint8_t (*)(std::uintptr_t, std::uint8_t, std::uint8_t);
using FnPauseLoad = void (*)(std::uintptr_t, std::uint8_t, std::uint8_t);
using FnRefresh = void (*)(std::uintptr_t);
using FnQuit = void (*)();
using FnBuild = std::uintptr_t (*)();
using FnU8 = std::uint8_t (*)();
using FnFlags = std::uint32_t (*)(std::uint8_t);

FnVoid1 o_flow = nullptr, o_decide = nullptr;
FnTitleLoad o_tload = nullptr;
FnConvLoad o_conv = nullptr;
FnNewWorld o_newworld = nullptr;
FnWorldReq o_wreq = nullptr;

// ---------------------------------------------------------------- state
Config g_cfg;
int g_role = 0;  // 1 host, 2 guest
std::atomic<bool> g_active{false};
char g_off[220] = {};          // why the session is off ("" = on)
// host_save=0 for a host or a guest (the old mode: each player on their own save): nothing of the session is installed,
// only the notice that its features are off (NoticeTick; docs/coop-session.md "When the features are off").
int g_plain = 0;               // 1 host, 2 guest; 0 = a session (or not a host / guest)
struct PlainNotice {
    double next_ms = 0, world_since = 0, away_since = 0;
    bool shown = false, published = false;
    char text[200] = {};
} g_pn;
bool g_load_call = false;      // guest: the direct load calls passed their byte check
bool g_quit_ok = false;        // guest: QuitGame passed its byte check
bool g_test_retry_ok = false;  // hidden tests: the retry request's guard passed its byte check
std::atomic<bool> g_allow{false};                // the mod's own world start is running (game thread)
std::atomic<long long> g_allow_until_ms{0};      // the hidden-run fallback: the gate open until then
std::atomic<unsigned> g_blocked{0};
std::atomic<bool> g_tick_player{false};          // a local player exists (last tick)

SRWLOCK g_mx = SRWLOCK_INIT;  // everything below shared by the game thread (hooks, Tick) and the worker
stx::GameVersion g_version;   // this game's (game thread, first tick)
bool g_version_ready = false;
std::string g_version_note;

std::uint32_t g_account = 0;   // the account the game runs as (steam::QuerySelfId, game thread): its save folder's name

// Host
struct HostEvent {
    int type = 0, index = 0;
    char slot[stx::kSlotNameChars + 1] = {};
    stx::Reason reason = stx::Reason::title_continue;
    bool approx = false;   // the newest autosave stands in (index -2)
    bool newest = false;   // the newest save of any kind (late join, "offer now")
    std::uint32_t load_counter = 0;
    double t = 0;
    std::uint64_t ft = 0;  // the wall clock (FILETIME) at detection: compared with the slots' write times
};
// The host's current world: the slot it was loaded from and when (title Continue / Load, in-world Load, New Game). "The
// newest save" (Retry, late join) is only ever a slot written since that load, else this slot itself
// (rules::NewestCandidates): never an older world's newer-looking autosave.
char g_world_slot[stx::kSlotNameChars + 1] = {};
std::uint64_t g_world_ft = 0;
std::deque<HostEvent> g_events;
std::uint32_t g_load_counter = 0;
bool g_new_game = false;          // New Game: the first save is offered when it appears
bool g_new_game_baseline = false; // the worker takes the slots' write times first
char g_test_forget[stx::kSlotNameChars + 1] = {};  // hidden tests (Command 9): a slot dropped from the New Game baseline
double g_last_detect_ms = -1e18;
int g_last_detect_type = -100, g_last_detect_index = -100;
bool g_player_since_detect = true;
std::uintptr_t g_detect_player = 0;
game::Guid g_detect_guid;
stx::HostPhase g_host_phase = stx::HostPhase::title;
bool g_host_load_pending = false;
double g_host_load_ms = 0;

// Guest (game thread <-> worker)
struct PendingLoad {
    bool ready = false;
    int slot = -1;
    stx::Offer offer;
    std::string title;   // the save's [Slot] Title
};
PendingLoad g_pending;
int g_load_result = -1;          // game thread -> worker: a stx::ResultCode
bool g_guest_world = false;      // the guest's game is in a world (session and player)
// The shared vehicles' notice (vehicle_share.cpp: "Summoning...", why the real vehicles are off): shown in the banner
// while no session notice is.
char g_vnotice[200] = {};
bool g_vnotice_warn = false;
double g_vnotice_until = 0;
bool g_guest_title = true;       // the guest's game session is 0
// Why the guest's title is not shown in the game's own menu (title_wait.cpp; "" = it is): the full-screen panel then.
char g_title_off[160] = {};

// What the screens show (worker -> game thread)
struct Ui {
    bool connected = false, ever_connected = false;
    double connected_since = 0;
    char peer_name[64] = {};
    // guest
    bool have_host_state = false;
    stx::State host_state;
    double last_host_msg_ms = 0;
    bool receiving = false;
    std::uint32_t have = 0, size = 0;
    char offer_slot[stx::kSlotNameChars + 1] = {};
    stx::Reason offer_reason = stx::Reason::title_continue;
    bool version_mismatch = false;
    char host_build[stx::kBuildChars + 1] = {};
    char refused[160] = {};
    // host
    bool have_gstate = false;
    stx::GState gstate;
    double last_gstate_ms = 0;
    bool have_guest_version = false;
    char guest_build[stx::kBuildChars + 1] = {};
    bool guest_version_mismatch = false;
    stx::Sender::Phase sender_phase = stx::Sender::Phase::idle;
    std::uint32_t sent = 0, offer_size = 0;
    std::uint32_t partner_loaded_counter = 0;
    double partner_loaded_ms = 0;
    char offer_problem[160] = {};   // why the last load could not be offered ("" = it was)
};
Ui g_ui;

// Worker-only
stx::Sender g_sender;
stx::SandboxPartStore* g_store = nullptr;
stx::Receiver* g_rx = nullptr;
stx::SandboxTarget g_target;
std::uint32_t g_gen = 0;
HANDLE g_worker = nullptr;

struct Lock {
    Lock() noexcept { AcquireSRWLockExclusive(&g_mx); }
    ~Lock() { ReleaseSRWLockExclusive(&g_mx); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

const char* ReasonText(stx::Reason r) noexcept {
    switch (r) {
        case stx::Reason::title_continue: return "title Continue";
        case stx::Reason::title_load: return "title Load";
        case stx::Reason::new_game_first_save: return "the first save of a New Game";
        case stx::Reason::in_world_load: return "in-world Load";
        case stx::Reason::retry: return "Retry";
        case stx::Reason::late_join: return "late join";
    }
    return "?";
}

std::string Hex8(const stx::Sha256& s) { return stx::ShaHex(s).substr(0, 16); }

// ---------------------------------------------------------------- byte checks
unsigned CheckSites() noexcept {
    unsigned failed = 0;
    for (const Site& s : kSites)
        if (!mem::CodeMatches(A(s.va), s.sig)) {
            Log("[coop] byte check FAILED: %s at 0x%llX", s.name, static_cast<unsigned long long>(s.va));
            failed |= s.groups;
        }
    // The globals as the game's own code names them (RIP-relative operands).
    if (!(failed & kCommon) && (mem::RipTarget(A(kContinueLast), 3, 8) != A(kSessionGlobal) ||
                                mem::RipTarget(A(kContinueLast + 0xD), 3, 7) != A(kAppGlobal))) {
        Log("[coop] byte check FAILED: ContinueLastSave does not name the session 0x%llX and app 0x%llX globals",
            static_cast<unsigned long long>(kSessionGlobal), static_cast<unsigned long long>(kAppGlobal));
        failed |= kCommon;
    }
    if (!(failed & kQuit) && mem::RipTarget(A(kQuitGame), 3, 7) != A(kSessionGlobal)) {
        Log("[coop] byte check FAILED: QuitGame does not name the session global");
        failed |= kQuit;
    }
    return failed;
}

std::uintptr_t Session() noexcept { return mem::Q(A(kSessionGlobal)); }

// ---------------------------------------------------------------- game calls (game thread, SEH; no C++ objects here)
bool CallResolve(std::uint8_t type, std::uint8_t index, char* out, std::size_t cap) noexcept {
    out[0] = 0;
    __try {
        std::uintptr_t str = 0;
        std::uint8_t a = 0, b = 0;
        std::uint16_t id = 0;
        std::uint64_t c = 0;
        reinterpret_cast<FnResolve>(A(kSlotName))(&str, type, index, &a, &b, &id, &c);
        if (str) {
            const char* p = reinterpret_cast<const char*>(str);
            std::size_t i = 0;
            for (; i + 1 < cap && p[i]; ++i) out[i] = p[i];
            out[i] = 0;
        }
        reinterpret_cast<FnVoid1>(A(kStrRelease))(reinterpret_cast<std::uintptr_t>(&str));
        return out[0] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

bool CallVersion(char* build, std::size_t cap, std::uint32_t* version, std::uint32_t* flags) noexcept {
    build[0] = 0;
    __try {
        const std::uintptr_t s = reinterpret_cast<FnBuild>(A(kBuildString))();
        const char* p = s ? *reinterpret_cast<const char* const*>(s) : nullptr;
        if (p) {
            std::size_t i = 0;
            for (; i + 1 < cap && p[i]; ++i) build[i] = p[i];
            build[i] = 0;
        }
        *version = reinterpret_cast<FnU8>(A(kSaveVersion))();
        *flags = reinterpret_cast<FnFlags>(A(kSaveFlags))(0);
        return build[0] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int CallRefreshAndHas(int slot) noexcept {  // 1 exists, 0 missing, -1 fault
    __try {
        reinterpret_cast<FnRefresh>(A(kSlotRefresh))(0);
        if (!g_load_call) return 1;  // fallback: no "exists" call
        return reinterpret_cast<FnHasSlot>(A(kHasSlot))(A(kAppGlobal), 1, static_cast<std::uint8_t>(slot)) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int CallTitleLoad(int slot) noexcept {  // the call's u8 result, -1 fault
    __try {
        g_allow.store(true);
        const std::uint8_t r = o_tload ? o_tload(A(kAppGlobal), 1, static_cast<std::uint8_t>(slot)) : 0;
        g_allow.store(false);
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_allow.store(false);
        return -1;
    }
}

bool CallPauseLoad(int slot) noexcept {
    __try {
        reinterpret_cast<FnPauseLoad>(A(kPauseLoad))(A(kAppGlobal), 1, static_cast<std::uint8_t>(slot));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallQuit() noexcept {
    __try {
        reinterpret_cast<FnQuit>(A(kQuitGame))();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Hidden tests only (Command 5): the pause menu's Load of (type, index) on this game.
bool CallPauseLoadAny(int type, int index) noexcept {
    __try {
        reinterpret_cast<FnPauseLoad>(A(kPauseLoad))(A(kAppGlobal), static_cast<std::uint8_t>(type), static_cast<std::uint8_t>(index));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Hidden tests only (Command 7): the checkpoint retry as the Game Over screen's caller 0x142DCAC60 makes it:
// 0x14362D750(session+0x2A0, 4, -2, 2, 5, -1, 1), only while 0x14362B4A0(session+0x2A0) says a checkpoint exists. Through
// the hook, so the host's detection sees it as it would see a real Retry. Mode 0: only with a checkpoint; 1: only ask;
// 2: request it even without one. Every other load request clears the checkpoint record (memset of session+0x2A0+0xC8),
// so a freshly loaded world has none until play passes a checkpoint (runs 20261119-072858 and -073105: absent for 3 minutes).
// With (index -2, reason 5) and no checkpoint, 0x14362D750 checks 0x14362B4A0 itself and returns without acting or
// clearing anything: mode 2 only lets the host's detection see a Retry. 1 requested, 2 requested without a checkpoint,
// 0 no checkpoint, -1 fault.
int CallRetry(int mode) noexcept {
    __try {
        const std::uintptr_t session = *reinterpret_cast<const std::uintptr_t*>(A(kSessionGlobal));
        if (!session) return 0;
        const bool have = reinterpret_cast<std::uint8_t (*)(std::uintptr_t)>(A(kRetryGuard))(session + 0x2A0) != 0;
        if (mode == 1) return have ? 1 : 0;
        if (!have && mode != 2) return 0;
        reinterpret_cast<FnWorldReq>(A(kWorldLoadReq))(session + 0x2A0, 4, 0xFE, 2, 5, 0xFFFFFFFFull, 1);
        return have ? 1 : 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ---------------------------------------------------------------- host detection (hooks, game thread)
std::uint64_t NowFt() noexcept {
    FILETIME f{};
    GetSystemTimeAsFileTime(&f);
    return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

// A load the host's game starts: named (the slot cache's resolver) and its detection noted for the follow-up rule, before
// the game's own function runs (a follow-up request may come from inside it).
HostEvent PrepareHostLoad(int type, int index, stx::Reason reason, bool approx) noexcept {
    HostEvent ev;
    ev.type = type;
    ev.index = index;
    ev.reason = reason;
    ev.approx = approx;
    ev.t = link::NowMsPrecise();
    ev.ft = NowFt();
    if (index >= -1) CallResolve(static_cast<std::uint8_t>(type), static_cast<std::uint8_t>(index), ev.slot, sizeof(ev.slot));
    game::Entity* pl = nullptr;
    game::TryGetLocalPlayer(&pl);
    game::Guid pg;
    if (pl && !game::TryReadGuid(pl, &pg)) pl = nullptr;
    Lock l;
    g_last_detect_ms = ev.t;
    g_last_detect_type = type;
    g_last_detect_index = index;
    g_player_since_detect = false;
    g_detect_player = reinterpret_cast<std::uintptr_t>(pl);
    g_detect_guid = pl ? pg : game::Guid{};
    return ev;
}

// The load is on its way: it gets the next load counter and is offered (worker).
void CommitHostLoad(HostEvent ev, const char* where, std::uint64_t extra_reason) noexcept {
    {
        Lock l;
        ev.load_counter = ++g_load_counter;
        g_events.push_back(ev);
        g_new_game = false;
        g_host_load_pending = true;
        g_host_load_ms = ev.t;
        if (ev.reason == stx::Reason::title_continue || ev.reason == stx::Reason::title_load || ev.reason == stx::Reason::in_world_load) {
            std::memcpy(g_world_slot, ev.slot, sizeof(g_world_slot));   // a Retry stays in the same world
            g_world_ft = ev.ft;
        }
    }
    char rs[40] = "";
    if (extra_reason != ~0ull) std::snprintf(rs, sizeof(rs), ", reason %llu", static_cast<unsigned long long>(extra_reason));
    Log("[coop] host: %s (type 0x%X, index %d%s) -> %s (load #%u)", where, ev.type & 0xFF, ev.index, rs,
        ev.slot[0] ? ev.slot : ev.approx ? "the newest autosave of this world (it stands in for the in-memory checkpoint)" : "slot unknown",
        ev.load_counter);
}

void HostLoadSeen(int type, int index, stx::Reason reason, bool approx, const char* where, std::uint64_t extra_reason) noexcept {
    CommitHostLoad(PrepareHostLoad(type, index, reason, approx), where, extra_reason);
}

void GateBlocked(const char* what) noexcept {
    const unsigned n = g_blocked.fetch_add(1) + 1;
    if (n <= 20 || n % 50 == 0) Log("[coop] gate: BLOCKED %s #%u (the guest waits for the host's save)", what, n);
}
bool GateOpen() noexcept { return g_allow.load() || g_allow_until_ms.load() > static_cast<long long>(link::NowMsPrecise()); }
bool Guarding() noexcept { return g_role == 2 && g_active.load() && !GateOpen(); }

void H_flow(std::uintptr_t self) {
    if (Guarding()) {
        std::uint8_t sel = 0xEE;
        mem::Rd(self + 0x130, sel);
        char what[96];
        std::snprintf(what, sizeof(what), "the title menu's start (choice +0x130 = %u: %s)", sel,
                      sel == 0 ? "Continue" : sel == 1 ? "New Game" : sel == 3 ? "converted save" : "?");
        GateBlocked(what);
        return;
    }
    o_flow(self);
}

void H_decide(std::uintptr_t self) {
    if (Guarding()) {
        GateBlocked("the load page's decision");
        return;
    }
    o_decide(self);
}

std::uint8_t H_tload(std::uintptr_t app, std::uint8_t type, std::uint8_t index) {
    if (Guarding()) {
        char what[80];
        std::snprintf(what, sizeof(what), "a title load (type 0x%X, index %d)", type, static_cast<std::int8_t>(index));
        GateBlocked(what);
        return 0;
    }
    const bool host = g_role == 1 && g_active.load();
    const bool cont = type == 0xF && static_cast<std::int8_t>(index) == -1;
    HostEvent ev;
    if (host) ev = PrepareHostLoad(type, static_cast<std::int8_t>(index), cont ? stx::Reason::title_continue : stx::Reason::title_load, false);
    const std::uint8_t r = o_tload(app, type, index);
    if (host) {
        // Offered only when the game took the load (0 = refused: the host stays at its title, so must the partner).
        if (r) CommitHostLoad(ev, cont ? "title Continue" : "title Load", ~0ull);
        Log("[coop] host: the title load returned %u%s", r, r ? "" : ": the game refused it, nothing is offered");
    }
    return r;
}

std::uint64_t H_conv(std::uintptr_t app) {
    if (Guarding()) {
        GateBlocked("the converted-save load");
        return 0;
    }
    if (g_role == 1 && g_active.load()) HostLoadSeen(2, -1, stx::Reason::title_load, false, "converted save (quicksave, newest)", ~0ull);
    return o_conv(app);
}

void H_newworld(std::uintptr_t app, std::uintptr_t world) {
    if (Guarding()) {
        GateBlocked("New Game (a new world)");
        return;
    }
    if (g_role == 1 && g_active.load()) {
        const double now = link::NowMsPrecise();
        {
            Lock l;
            g_new_game = true;
            g_new_game_baseline = true;
            g_host_load_pending = true;
            g_host_load_ms = now;
            g_last_detect_ms = now;
            g_last_detect_type = -1;
            g_last_detect_index = -1;
            g_player_since_detect = false;
            g_detect_player = 0;
            g_world_slot[0] = 0;   // a new world: no slot yet (its first save names it)
            g_world_ft = NowFt();
        }
        Log("[coop] host: New Game: the partner joins at your first save (the slots are watched read only, 1 Hz)");
    }
    o_newworld(app, world);
}

std::uint64_t H_wreq(std::uintptr_t s, std::uint64_t type, std::uint64_t index, std::uint64_t a, std::uint64_t reason, std::uint64_t c, std::uint64_t d) {
    if (g_role == 1 && g_active.load()) {
        const int t = static_cast<int>(type & 0xFF);
        const int i = static_cast<std::int8_t>(index & 0xFF);
        const double now = link::NowMsPrecise();
        bool follow_up = false;
        std::uint32_t counter = 0;
        {
            Lock l;
            follow_up = rules::IsFollowUp(Session() == 0, now, g_last_detect_ms, g_player_since_detect, t, i, g_last_detect_type, g_last_detect_index);
            counter = g_load_counter;
        }
        if (follow_up)
            Log("[coop] host: in-world load request (type 0x%X, index %d, reason %llu): the game's own follow-up of load #%u, not a new load", t, i,
                static_cast<unsigned long long>(reason & 0xFFFFFFFF), counter);
        else if (i == -2)
            HostLoadSeen(t, -2, stx::Reason::retry, true, "Retry (the in-memory checkpoint)", reason & 0xFFFFFFFF);
        else
            HostLoadSeen(t, i, stx::Reason::in_world_load, false, "in-world Load", reason & 0xFFFFFFFF);
    }
    return o_wreq(s, type, index, a, reason, c, d);
}

bool HookOne(std::uintptr_t va, void* detour, void** orig, const char* name) noexcept {
    void* target = reinterpret_cast<void*>(A(va));
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) Log("[coop] hook %s (0x%llX) failed: %s", name, static_cast<unsigned long long>(va), MH_StatusToString(s));
    return s == MH_OK;
}

// ---------------------------------------------------------------- files (worker thread)
std::wstring EnvW(const wchar_t* name) {
    wchar_t buf[600] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, 600);
    return n > 0 && n < 600 ? std::wstring(buf, n) : std::wstring();
}

std::wstring Widen(const char* s) {
    std::wstring w;
    for (; s && *s; ++s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
    return w;
}

bool IsDigits(const wchar_t* s) {
    if (!s || !*s) return false;
    for (; *s; ++s)
        if (*s < L'0' || *s > L'9') return false;
    return true;
}

std::uint32_t Account() {
    Lock l;
    return g_account;
}

// The digit (account) folders below `root`.
std::vector<std::wstring> DigitDirs(const std::wstring& root) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsDigits(fd.cFileName))
            out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// The host's account folder as the game spells it (%USERPROFILE%\AppData\Local\KojimaProductions\DeathStrandingDC\<id>; a
// hidden run's redirect maps that spelling to its scratch copy). The account the game runs as (Steam, asked on the game
// thread); not known yet (Steam not ready): the one account folder there is. Never a guess between several accounts:
// "" and *why then (another account's save never leaves the PC, and the partner never gets another world).
std::wstring HostAccountDir(std::string* why) {
    const std::wstring root = EnvW(L"USERPROFILE") + L"\\AppData\\Local\\KojimaProductions\\DeathStrandingDC";
    if (const std::uint32_t id = Account()) {
        const std::wstring dir = root + L"\\" + std::to_wstring(id);
        const DWORD a = GetFileAttributesW(dir.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) return dir;
        if (why) *why = "your account's save folder does not exist (no save yet?)";
        return std::wstring();
    }
    const std::vector<std::wstring> dirs = DigitDirs(root);
    if (dirs.size() == 1) return root + L"\\" + dirs[0];
    if (why) {
        if (dirs.empty()) *why = "no account folder below the save folder";
        else
            *why = std::to_string(dirs.size()) + " Steam accounts have Death Stranding saves on this PC and Steam has not said which one this game "
                                                 "runs as yet: nothing is sent rather than another account's save";
    }
    return std::wstring();
}

bool TempExists(const std::wstring& account, const std::wstring& slot) {   // the game is writing the slot
    return GetFileAttributesW((account + L"\\" + slot + L"\\checkpoint.dattemp").c_str()) != INVALID_FILE_ATTRIBUTES;
}

// A slot's checkpoint.dat write time (GetFileAttributesEx opens nothing: the game's temp + rename is never in the way).
bool SlotTime(const std::wstring& account, const std::wstring& slot, std::uint64_t* t) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW((account + L"\\" + slot + L"\\checkpoint.dat").c_str(), GetFileExInfoStandard, &a)) return false;
    *t = (static_cast<std::uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return true;
}

struct SlotInfo {
    std::string name;
    std::uint64_t time = 0;
};
// Every slot folder with a checkpoint.dat; autosave_only: autosaveN only.
std::vector<SlotInfo> ListSlots(const std::wstring& account, bool autosave_only) {
    std::vector<SlotInfo> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((account + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        char name[64] = {};
        std::size_t i = 0;
        bool ascii = true;
        for (; fd.cFileName[i] && i + 1 < sizeof(name); ++i) {
            if (fd.cFileName[i] > 127) ascii = false;
            name[i] = static_cast<char>(fd.cFileName[i]);
        }
        if (!ascii || !stx::IsSlotName(name)) continue;
        if (autosave_only && std::strncmp(name, "autosave", 8) != 0) continue;
        SlotInfo s;
        s.name = name;
        if (SlotTime(account, fd.cFileName, &s.time)) out.push_back(s);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

FILETIME NowFileTime() {
    FILETIME f{};
    GetSystemTimeAsFileTime(&f);
    return f;
}
std::uint64_t Ft(const FILETIME& f) { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

// Read a host slot (read only) once its write has settled: equal SHA-256 over two reads, then validated. Each read
// opens, reads and closes at once (save_xfer.hpp: an open handle makes the game's rename of checkpoint.dattemp fail),
// never while a checkpoint.dattemp is there.
bool ReadHostSlot(const std::wstring& account, const std::string& slot, std::vector<std::uint8_t>& out, stx::CheckpointInfo* info, std::string* why) {
    const std::wstring wslot = Widen(slot.c_str());
    std::uint64_t t0 = 0;
    for (int i = 0; i < 40; ++i) {  // up to 10 s for a write in progress to settle (2 s without a change, no .dattemp)
        if (!SlotTime(account, wslot, &t0)) return *why = "no checkpoint.dat in " + slot, false;
        if (Ft(NowFileTime()) - t0 >= 20000000ull && !TempExists(account, wslot)) break;
        Sleep(250);
    }
    if (TempExists(account, wslot)) return *why = "the game is still writing " + slot + " (checkpoint.dattemp)", false;
    const ULONGLONG start = GetTickCount64();
    const stx::ReadStatus rs = stx::ReadSaveSnapshot(account + L"\\" + wslot + L"\\checkpoint.dat", out, 400, 3);
    if (rs != stx::ReadStatus::ok) return *why = std::string("read ") + stx::ReadStatusName(rs), false;
    std::uint64_t t1 = 0;
    if (!SlotTime(account, wslot, &t1) || t1 != t0) return *why = "the game wrote the slot again while it was read", false;
    const stx::CheckpointError ce = stx::ValidateCheckpoint(out.data(), out.size(), info);
    if (ce != stx::CheckpointError::ok) return *why = std::string("not a valid checkpoint.dat: ") + stx::CheckpointErrorName(ce), false;
    Log("[coop] host: read %s read only: %zu bytes, sha256 %s, two equal reads in %.2f s ('%s')", slot.c_str(), out.size(),
        Hex8(stx::Sha256Of(out.data(), out.size())).c_str(), static_cast<double>(GetTickCount64() - start) / 1000.0, info ? info->title.c_str() : "");
    return true;
}

// ---------------------------------------------------------------- the worker: host
struct HostWorker {
    double last_state_ms = 0;
    stx::State last_state;
    bool baseline_taken = false;
    std::vector<SlotInfo> baseline;
    std::wstring baseline_account;   // the folder the baseline was taken from ("" = none known then)
    double next_poll_ms = 0;
    double offer_ms = 0;
    std::uint64_t chunks0 = 0, bytes0 = 0, resumes0 = 0, restarts0 = 0;   // the sender's counters when the offer was made
    double last_late_ms = -1e18;
    std::uint64_t retransmits0 = 0;
    stx::Sender::Phase last_phase = stx::Sender::Phase::idle;
    double gen_ms = 0;
    bool told_no_reply = false;
};
HostWorker g_hw;

void SetOfferProblem(const char* text) {
    Lock l;
    std::snprintf(g_ui.offer_problem, sizeof(g_ui.offer_problem), "%s", text);
}

void HostOffer(const HostEvent& ev, double now) {
    std::string account_why;
    const std::wstring account = HostAccountDir(&account_why);
    if (account.empty()) {
        Log("[coop] host: load #%u: nothing offered: %s", ev.load_counter, account_why.c_str());
        SetOfferProblem(account_why.c_str());
        return;
    }
    // The slot the game named; or, for "the newest" (Retry, late join, offer now), the newest slots of the host's current
    // world (written since its load; else the slot it was loaded from): one that cannot be read or validated is skipped
    // for the next newest (at most 3 tried).
    std::vector<std::string> candidates;
    if (ev.approx || ev.newest || !ev.slot[0]) {
        std::vector<rules::SlotTime> slots;
        for (const SlotInfo& s : ListSlots(account, false)) slots.push_back(rules::SlotTime{s.name, s.time});
        std::string world_slot;
        std::uint64_t world_ft = 0;
        {
            Lock l;
            world_slot = g_world_slot;
            world_ft = g_world_ft;
        }
        if (!ev.approx && !ev.newest) {
            // A load whose slot the resolver could not name (a title Continue loads the newest save): the newest of all.
            world_slot.clear();
            world_ft = 0;
        }
        bool own = false;
        candidates = rules::NewestCandidates(slots, ev.approx, world_ft, world_slot, &own);
        if (candidates.empty()) {
            Log("[coop] host: load #%u: no %s of the current world on disk (none written since its load%s%s): nothing offered", ev.load_counter,
                ev.approx ? "autosave" : "save", world_slot.empty() ? "" : ", and not ", world_slot.c_str());
            SetOfferProblem("no save of your current world on disk yet");
            return;
        }
        Log("[coop] host: load #%u: %s %s", ev.load_counter,
            own ? "nothing written since this world's load: its own slot," : ev.approx ? "the newest autosave of this world is" : "the newest save of this world is",
            candidates[0].c_str());
    } else {
        candidates.push_back(ev.slot);
    }
    std::string slot;
    std::vector<std::uint8_t> bytes;
    stx::CheckpointInfo info;
    for (const std::string& c : candidates) {
        std::string why;
        if (ReadHostSlot(account, c, bytes, &info, &why)) {
            slot = c;
            break;
        }
        Log("[coop] host: load #%u: %s could not be offered: %s%s", ev.load_counter, c.c_str(), why.c_str(),
            &c != &candidates.back() ? " (trying the next newest)" : "");
    }
    if (slot.empty()) {
        SetOfferProblem("your save could not be read (see the log)");
        return;
    }
    SetOfferProblem("");
    if (!ev.slot[0] && !ev.approx && !ev.newest) {
        Lock l;
        if (g_world_ft == ev.ft) std::snprintf(g_world_slot, sizeof(g_world_slot), "%s", slot.c_str());   // this world's slot, named now
    }
    stx::Offer o;
    o.load_counter = ev.load_counter;
    o.slot_type = static_cast<std::uint8_t>(ev.type);
    o.slot_index = static_cast<std::int8_t>(ev.index);
    stx::SetSlotName(o, slot.c_str());
    o.approx = ev.approx;
    o.reason = ev.reason;
    {
        Lock l;
        o.version = g_version;
    }
    if (!g_sender.Offer(o, bytes.data(), bytes.size())) {
        Log("[coop] host: load #%u: the transfer refused the offer of %s (an older load counter than #%u?)", ev.load_counter, slot.c_str(),
            g_sender.load_counter());
        return;
    }
    g_hw.offer_ms = now;
    g_hw.retransmits0 = link::GetInfo().rel_retransmits;
    g_hw.chunks0 = g_sender.chunks_sent;
    g_hw.bytes0 = g_sender.bytes_sent;
    g_hw.resumes0 = g_sender.resumes;
    g_hw.restarts0 = g_sender.restarts;
    Log("[coop] host: offer #%u (xfer %u): %s for %s, %u bytes, sha256 %s%s", ev.load_counter, g_sender.xfer_id(), slot.c_str(),
        ReasonText(ev.reason), g_sender.offer().size, Hex8(g_sender.offer().sha).c_str(), ev.approx ? ", approx (stands in for the checkpoint)" : "");
}

void HostOnMessage(const stx::Message& m, double now) {
    switch (m.h.kind) {
        case stx::Kind::want:
        case stx::Kind::result:
        case stx::Kind::abort: {
            g_sender.OnMessage(m);
            if (m.h.kind == stx::Kind::want)
                Log("[coop] host: the partner wants xfer %u from byte %u%s", m.h.xfer_id, m.want.next_offset,
                    m.want.next_offset ? " (resume)" : "");
            if (m.h.kind == stx::Kind::result && m.h.xfer_id == g_sender.xfer_id()) {
                const auto li = link::GetInfo();
                Log("[coop] host: the partner's RESULT for load #%u: %s (%s); %llu chunks, %llu bytes sent, %.1f s since the offer, resumes %llu, "
                    "restarts %llu, link retransmits %llu",
                    g_sender.load_counter(), m.result.ok ? "ok" : "FAILED", stx::ResultCodeName(m.result.code),
                    static_cast<unsigned long long>(g_sender.chunks_sent - g_hw.chunks0), static_cast<unsigned long long>(g_sender.bytes_sent - g_hw.bytes0),
                    (now - g_hw.offer_ms) / 1000.0, static_cast<unsigned long long>(g_sender.resumes - g_hw.resumes0),
                    static_cast<unsigned long long>(g_sender.restarts - g_hw.restarts0), static_cast<unsigned long long>(li.rel_retransmits - g_hw.retransmits0));
                if (m.result.ok) {
                    Lock l;
                    g_ui.partner_loaded_counter = g_sender.load_counter();
                    g_ui.partner_loaded_ms = now;
                }
            }
            if (m.h.kind == stx::Kind::abort) Log("[coop] host: the partner aborted xfer %u: %s", m.h.xfer_id, stx::AbortCodeName(m.abort.code));
            return;
        }
        case stx::Kind::gstate: {
            Lock l;
            const bool changed = !g_ui.have_gstate || g_ui.gstate.phase != m.gstate.phase || g_ui.gstate.loaded_load_counter != m.gstate.loaded_load_counter;
            g_ui.have_gstate = true;
            g_ui.gstate = m.gstate;
            g_ui.last_gstate_ms = now;
            if (changed)
                Log("[coop] host: the partner is %s (loaded #%u%s)", m.gstate.phase == stx::GuestPhase::waiting     ? "waiting"
                                                                     : m.gstate.phase == stx::GuestPhase::receiving ? "receiving"
                                                                     : m.gstate.phase == stx::GuestPhase::loading   ? "loading"
                                                                     : m.gstate.phase == stx::GuestPhase::world     ? "in the world"
                                                                                                                    : "REFUSED",
                    m.gstate.loaded_load_counter, m.gstate.refuse_reason ? (std::string(", reason ") + stx::ResultCodeName(static_cast<stx::ResultCode>(m.gstate.refuse_reason))).c_str() : "");
            return;
        }
        case stx::Kind::state: {  // the guest's own game version (its STATE carries only that)
            Lock l;
            const bool first = !g_ui.have_guest_version || std::strcmp(g_ui.guest_build, m.state.version.build) != 0;
            g_ui.have_guest_version = true;
            std::snprintf(g_ui.guest_build, sizeof(g_ui.guest_build), "%s", m.state.version.build);
            g_ui.guest_version_mismatch = !stx::SameVersion(m.state.version, g_version);
            if (first)
                Log("[coop] host: the partner's game: build '%s', save version %u, flags 0x%X: %s", m.state.version.build,
                    m.state.version.savegame_version, m.state.version.save_flags,
                    g_ui.guest_version_mismatch ? "DIFFERS from ours: the session is refused (update both games)" : "same as ours");
            return;
        }
        default: return;
    }
}

void HostWork(double now) {
    // Loads the hooks saw, oldest first.
    for (;;) {
        HostEvent ev;
        {
            Lock l;
            if (g_events.empty()) break;
            ev = g_events.front();
            g_events.pop_front();
        }
        HostOffer(ev, now);
    }
    bool new_game = false, take_baseline = false;
    stx::HostPhase phase = stx::HostPhase::title;
    {
        Lock l;
        new_game = g_new_game;
        take_baseline = g_new_game_baseline;
        g_new_game_baseline = false;
        phase = g_host_phase;
    }
    // New Game: the first save of the new world (D1-3), polled read only at 1 Hz.
    if (take_baseline) {
        std::string why;
        const std::wstring account = HostAccountDir(&why);
        g_hw.baseline = account.empty() ? std::vector<SlotInfo>() : ListSlots(account, false);
        g_hw.baseline_account = account;
        g_hw.baseline_taken = true;
        if (account.empty()) Log("[coop] host: New Game: the first save cannot be found: %s", why.c_str());
        else Log("[coop] host: New Game: %zu existing slot(s) noted; waiting for the first new save", g_hw.baseline.size());
    }
    {
        char forget[sizeof(g_test_forget)] = {};
        {
            Lock l;
            std::memcpy(forget, g_test_forget, sizeof(forget));
            g_test_forget[0] = 0;
        }
        if (forget[0] && g_hw.baseline_taken) {
            std::size_t before = g_hw.baseline.size();
            for (std::size_t i = 0; i < g_hw.baseline.size();)
                if (g_hw.baseline[i].name == forget) g_hw.baseline.erase(g_hw.baseline.begin() + static_cast<std::ptrdiff_t>(i));
                else ++i;
            Log("[coop] test: %s %s the New Game baseline: the 1 Hz poll takes it for the first new save", forget,
                before != g_hw.baseline.size() ? "dropped from" : "was not in");
        }
    }
    if (new_game && g_hw.baseline_taken && now >= g_hw.next_poll_ms) {
        g_hw.next_poll_ms = now + 1000;
        const std::wstring account = HostAccountDir(nullptr);
        if (account != g_hw.baseline_account) {
            // The account became known only now (Steam was not ready at New Game): the baseline is taken again from its
            // folder, so no old slot is ever taken for the first save (a New Game's intro lasts minutes before it).
            g_hw.baseline = account.empty() ? std::vector<SlotInfo>() : ListSlots(account, false);
            g_hw.baseline_account = account;
            Log("[coop] host: New Game: the account folder is known now: %zu existing slot(s) noted; waiting for the first new save",
                g_hw.baseline.size());
        }
        const std::vector<SlotInfo> list = account.empty() ? std::vector<SlotInfo>() : ListSlots(account, false);
        const SlotInfo* best = nullptr;
        for (const SlotInfo& s : list) {
            bool old = false;
            for (const SlotInfo& b : g_hw.baseline)
                if (b.name == s.name && b.time >= s.time) old = true;
            if (!old && (!best || s.time > best->time)) best = &s;
        }
        if (best && Ft(NowFileTime()) - best->time >= 20000000ull) {
            HostEvent ev;
            ev.type = std::strncmp(best->name.c_str(), "autosave", 8) == 0 ? 4 : std::strncmp(best->name.c_str(), "quicksave", 9) == 0 ? 2 : 1;
            ev.index = std::atoi(best->name.c_str() + (ev.type == 4 ? 8 : ev.type == 2 ? 9 : 10));
            std::snprintf(ev.slot, sizeof(ev.slot), "%s", best->name.c_str());
            ev.reason = stx::Reason::new_game_first_save;
            {
                Lock l;
                ev.load_counter = ++g_load_counter;
                g_new_game = false;
                std::memcpy(g_world_slot, ev.slot, sizeof(g_world_slot));   // the new world's own slot from now on
            }
            Log("[coop] host: New Game: the first save is %s (load #%u)", ev.slot, ev.load_counter);
            HostOffer(ev, now);
        }
    }
    // A partner without the current world (late join, back at its title after a lost link, a failed load): the newest
    // save, once per kReofferMs (D1-7).
    Ui ui;
    {
        Lock l;
        ui = g_ui;
    }
    rules::LateJoinIn lj;
    lj.connected = ui.connected;
    lj.have_gstate = ui.have_gstate;
    lj.gstate_age_ms = now - ui.last_gstate_ms;
    lj.host_phase = phase;
    lj.new_game_pending = new_game;
    lj.sender_busy = g_sender.busy();
    lj.sender_done = g_sender.phase() == stx::Sender::Phase::done;
    lj.current_counter = g_sender.load_counter();
    lj.current_sha = g_sender.offer().sha;
    lj.version_mismatch = ui.guest_version_mismatch;
    lj.gstate = ui.gstate;
    lj.since_last_offer_ms = now - g_hw.last_late_ms;
    const rules::LateJoin late = rules::WantLateJoin(lj);
    {
        const bool in_older_world = late == rules::LateJoin::older_world;
        if (late != rules::LateJoin::none) {
            g_hw.last_late_ms = now;
            HostEvent ev;
            ev.type = 4;
            ev.index = -1;
            ev.newest = true;
            ev.reason = stx::Reason::late_join;
            {
                Lock l;
                ev.load_counter = ++g_load_counter;
            }
            Log("[coop] host: the partner %s: offering the newest save (late join, load #%u)",
                in_older_world ? "is in an older world" : "is waiting while you are in the world", ev.load_counter);
            HostOffer(ev, now);
        }
    }
    // STATE on change and at 1 Hz.
    stx::State st;
    st.phase = phase;
    st.load_counter = g_sender.phase() == stx::Sender::Phase::idle ? 0 : g_sender.load_counter();
    st.current_save_sha = g_sender.offer().sha;
    st.offer_xfer_id = g_sender.busy() ? g_sender.xfer_id() : 0;
    {
        Lock l;
        st.version = g_version;
    }
    st.new_game_pending = new_game;
    const bool changed = st.phase != g_hw.last_state.phase || st.load_counter != g_hw.last_state.load_counter ||
                         st.offer_xfer_id != g_hw.last_state.offer_xfer_id || st.new_game_pending != g_hw.last_state.new_game_pending;
    if (ui.connected && (changed || now - g_hw.last_state_ms >= 1000)) {
        std::uint8_t buf[stx::kHeaderSize + stx::kStateBody];
        const std::size_t n = stx::EncodeState(st, buf, sizeof(buf));
        if (n && link::SendSave(buf, n)) {
            g_hw.last_state = st;
            g_hw.last_state_ms = now;
        }
    }
    link::SetSaveBurst(phase != stx::HostPhase::world);
    // The sender's output (flow control: the channel's window).
    std::uint8_t buf[stx::kMaxMessage];
    std::size_t n = 0;
    while (link::SaveQueued() < 32 && (n = g_sender.Build(buf, sizeof(buf))) != 0 && link::SendSave(buf, n)) g_sender.Advance();
    if (g_sender.phase() != g_hw.last_phase) {
        if (g_sender.phase() == stx::Sender::Phase::ended)
            Log("[coop] host: xfer %u sent completely (%llu chunks, %llu bytes for this offer) in %.1f s; waiting for the partner's RESULT",
                g_sender.xfer_id(), static_cast<unsigned long long>(g_sender.chunks_sent - g_hw.chunks0),
                static_cast<unsigned long long>(g_sender.bytes_sent - g_hw.bytes0), (now - g_hw.offer_ms) / 1000.0);
        g_hw.last_phase = g_sender.phase();
    }
    {
        Lock l;
        g_ui.sender_phase = g_sender.phase();
        g_ui.sent = g_sender.send_offset();
        g_ui.offer_size = g_sender.offer().size;
    }
    if (ui.connected && !ui.have_gstate && !ui.have_guest_version && now - g_hw.gen_ms > kNoReplyMs && !g_hw.told_no_reply) {
        g_hw.told_no_reply = true;
        Log("[coop] host: connected for %.0f s without a word from the partner's save session: is 'Host holds the save' off on their side?",
            (now - g_hw.gen_ms) / 1000.0);
    }
}

// ---------------------------------------------------------------- the worker: guest
struct GuestWorker {
    double last_gstate_ms = 0, last_vstate_ms = 0;
    stx::GState last_gstate;
    stx::Receiver::Phase last_phase = stx::Receiver::Phase::waiting;
    double recv_start_ms = 0;
    int next_slot = static_cast<int>(kSlotBase);
    bool local_set = false;
    bool told_mismatch = false;
    std::uint32_t logged_offer = 0;
    std::uint32_t logged_wait = 0;
};
GuestWorker g_gw;

// The account folder the guest's game reads (below its redirected folder): the account the game runs as; not known yet,
// the one digit folder the launcher seeded there; several (never left by the launcher): 0, nothing is written.
std::uint32_t GuestAccount() {
    if (const std::uint32_t id = Account()) return id;
    const std::vector<std::wstring> dirs = DigitDirs(g_target.scratch_root + L"\\DeathStrandingDC");
    return dirs.size() == 1 ? static_cast<std::uint32_t>(std::wcstoul(dirs[0].c_str(), nullptr, 10)) : 0;
}

void GuestOnMessage(const stx::Message& m, double now) {
    if (m.h.kind == stx::Kind::gstate || m.h.kind == stx::Kind::want || m.h.kind == stx::Kind::result) return;  // ours, never the host's
    const bool was_mismatch = g_rx->version_mismatch();
    g_rx->OnMessage(m);
    if (m.h.kind == stx::Kind::offer && m.h.xfer_id != g_gw.logged_offer) {
        g_gw.logged_offer = m.h.xfer_id;
        Log("[coop] guest: offer #%u (xfer %u) from the host: %s for %s, %u bytes, sha256 %s%s -> %s", m.offer.load_counter, m.h.xfer_id,
            m.offer.slot_name, ReasonText(m.offer.reason), m.offer.size, Hex8(m.offer.sha).c_str(), m.offer.approx ? ", approx" : "",
            stx::Receiver::PhaseName(g_rx->phase()));
    }
    if (!was_mismatch && g_rx->version_mismatch()) {
        const stx::GameVersion& hv = m.h.kind == stx::Kind::offer ? m.offer.version : m.state.version;
        Lock l;
        Log("[coop] guest: Game versions differ: host '%s' (save version %u, flags 0x%X) vs ours '%s' (%u, 0x%X): refused, nothing is written",
            hv.build, hv.savegame_version, hv.save_flags, g_version.build, g_version.savegame_version, g_version.save_flags);
        g_ui.version_mismatch = true;
        std::snprintf(g_ui.host_build, sizeof(g_ui.host_build), "%s", hv.build);
    }
    if (m.h.kind == stx::Kind::state) {
        Lock l;
        const bool changed = !g_ui.have_host_state || g_ui.host_state.phase != m.state.phase || g_ui.host_state.load_counter != m.state.load_counter ||
                             g_ui.host_state.new_game_pending != m.state.new_game_pending;
        g_ui.have_host_state = true;
        g_ui.host_state = m.state;
        g_ui.last_host_msg_ms = now;
        if (!g_ui.version_mismatch) std::snprintf(g_ui.host_build, sizeof(g_ui.host_build), "%s", m.state.version.build);
        if (changed)
            Log("[coop] guest: the host is %s (load #%u%s)", m.state.phase == stx::HostPhase::title     ? "at the title"
                                                             : m.state.phase == stx::HostPhase::loading ? "loading"
                                                             : m.state.phase == stx::HostPhase::world   ? "in the world"
                                                                                                        : "quitting",
                m.state.load_counter, m.state.new_game_pending ? ", New Game: you join at the host's first save" : "");
    } else {
        Lock l;
        g_ui.last_host_msg_ms = now;
    }
    if (m.h.kind == stx::Kind::abort) Log("[coop] guest: the host aborted xfer %u: %s", m.h.xfer_id, stx::AbortCodeName(m.abort.code));
}

void GuestWork(double now) {
    stx::Receiver& rx = *g_rx;
    const stx::Receiver::Phase ph = rx.phase();
    if (ph != g_gw.last_phase) {
        if (ph == stx::Receiver::Phase::receiving) g_gw.recv_start_ms = now;
        if (ph == stx::Receiver::Phase::complete)
            Log("[coop] guest: received %u bytes in %.1f s (%.0f KB/s), sha256 equal, checkpoint.dat valid; dropped chunks %llu, resumes %llu, sha "
                "retries %llu",
                rx.offer().size, (now - g_gw.recv_start_ms) / 1000.0, rx.offer().size / 1024.0 / ((now - g_gw.recv_start_ms) / 1000.0 + 1e-6),
                static_cast<unsigned long long>(rx.dropped_chunks), static_cast<unsigned long long>(rx.resumes),
                static_cast<unsigned long long>(rx.sha_retries));
        if (ph == stx::Receiver::Phase::failed) Log("[coop] guest: the transfer FAILED: %s (waiting for the next offer)", stx::ResultCodeName(rx.last_error()));
        g_gw.last_phase = ph;
    }
    // A complete save: written below the redirected folder only, then loaded by the game thread.
    bool busy = false;
    {
        Lock l;
        busy = g_pending.ready;
    }
    if (ph == stx::Receiver::Phase::complete && (busy || rx.loading())) {
        // One load at a time (Receiver::TakeComplete): the newer save waits for the running load's result.
        if (g_gw.logged_wait != rx.xfer_id()) {
            g_gw.logged_wait = rx.xfer_id();
            Log("[coop] guest: the host's save #%u is here; it is loaded once the running load of #%u has its result", rx.offer().load_counter,
                g_pending.offer.load_counter);
        }
    } else if (ph == stx::Receiver::Phase::complete) {
        std::vector<std::uint8_t> bytes;
        stx::Offer offer;
        if (rx.TakeComplete(bytes, offer)) {
            stx::CheckpointInfo info;
            stx::ValidateCheckpoint(bytes.data(), bytes.size(), &info);
            const int slot = g_gw.next_slot;
            g_gw.next_slot = rules::NextGuestSlot(slot);
            const std::uint32_t account = GuestAccount();
            std::string why;
            const std::wstring rel = stx::ManualSlotRel(account, slot);
            if (rel.empty() || !stx::WriteGuestSaveFile(g_target, rel, bytes.data(), bytes.size(), &why)) {
                Log("[coop] guest: the host's save could NOT be written as manualsave%d (account %u): %s", slot, account,
                    rel.empty() ? "the game's account is not known (Steam not ready) and the co-op folder does not hold exactly one account folder"
                                : why.c_str());
                rx.LoadDone(stx::ResultCode::disk);
            } else {
                Log("[coop] guest: wrote the host's save #%u as manualsave%d (%zu bytes, read back sha256 equal) below the co-op folder only",
                    offer.load_counter, slot, bytes.size());
                Lock l;
                g_pending.ready = true;
                g_pending.slot = slot;
                g_pending.offer = offer;
                g_pending.title = info.title;
            }
        }
    }
    int result = -1;
    {
        Lock l;
        result = g_load_result;
        g_load_result = -1;
    }
    if (result >= 0) rx.LoadDone(static_cast<stx::ResultCode>(result));
    // GSTATE on change and at 1 Hz (the phase as this game really is: back at its title = waiting); our version every 5 s.
    stx::GState g = rx.Status();
    bool world = false, title = true, connected = false;
    {
        Lock l;
        world = g_guest_world;
        title = g_guest_title;
        connected = g_ui.connected;
        g_ui.receiving = ph == stx::Receiver::Phase::receiving;
        g_ui.have = rx.have_bytes();
        g_ui.size = rx.offer().size;
        std::snprintf(g_ui.offer_slot, sizeof(g_ui.offer_slot), "%s", rx.offer().slot_name);
        g_ui.offer_reason = rx.offer().reason;
    }
    g.phase = rules::GuestPhaseOf(g.phase, title, world);
    if (connected) {
        const bool changed = g.phase != g_gw.last_gstate.phase || g.loaded_load_counter != g_gw.last_gstate.loaded_load_counter;
        if (changed || now - g_gw.last_gstate_ms >= 1000) {
            std::uint8_t buf[stx::kHeaderSize + stx::kGStateBody];
            const std::size_t n = stx::EncodeGState(g, buf, sizeof(buf));
            if (n && link::SendSave(buf, n)) {
                g_gw.last_gstate = g;
                g_gw.last_gstate_ms = now;
            }
        }
        if (now - g_gw.last_vstate_ms >= 5000) {
            stx::State vs;
            {
                Lock l;
                vs.version = g_version;
            }
            std::uint8_t buf[stx::kHeaderSize + stx::kStateBody];
            const std::size_t n = stx::EncodeState(vs, buf, sizeof(buf));
            if (n && link::SendSave(buf, n)) g_gw.last_vstate_ms = now;
        }
    }
    std::uint8_t buf[stx::kMaxMessage];
    std::size_t n = 0;
    while (link::SaveQueued() < 32 && (n = rx.Build(buf, sizeof(buf))) != 0 && link::SendSave(buf, n)) rx.Advance();
}

// ---------------------------------------------------------------- the worker loop
// A new link session, or the same one back after an outage (the link keeps its session, and its reliable channels
// deliver what was queued, when the peer answers again within its handshake: run 20261119-080005 saw no new generation).
void OnLinkSession(double now) {
    if (g_role == 1) {
        stx::HostPhase phase;
        {
            Lock l;
            phase = g_host_phase;
        }
        const stx::Reason r = g_sender.offer().reason;
        if (rules::StaleOnReconnect(phase, g_sender.phase() == stx::Sender::Phase::offered, r)) {
            // Never answered, and the host has played on since: a late join gets the newest save instead (D1-7).
            Log("[coop] host: the unanswered offer #%u (%s) is stale now that you are in the world: the newest save is offered instead",
                g_sender.load_counter(), ReasonText(r));
            g_sender.Cancel(stx::AbortCode::superseded);
        } else if (g_sender.busy()) {
            Log("[coop] host: offer #%u announced again (the partner resumes it)", g_sender.load_counter());
            g_sender.Reannounce();
        }
        g_hw.last_state_ms = 0;
        g_hw.last_state = stx::State{};
        g_hw.last_state.phase = stx::HostPhase::quitting;  // forces a STATE now
        g_hw.gen_ms = now;
        g_hw.told_no_reply = false;
        g_hw.last_late_ms = -1e18;
        Lock l;
        g_ui.have_gstate = false;
        g_ui.have_guest_version = false;
    } else {
        g_gw.last_gstate_ms = 0;
        g_gw.last_vstate_ms = 0;
        g_gw.last_gstate.phase = stx::GuestPhase::refused;  // forces a GSTATE now
    }
}

void OnNewGeneration(std::uint32_t gen, double now) {
    Log("[coop] new link session #%u (%s)", gen, g_role == 1 ? "host" : "guest");
    if (g_role == 2 && g_rx) {
        // The host may be a restarted game whose load counters and transfers start over: the running transfer is
        // forgotten (its bytes stay for a resume of the same file), a running load and the loaded save are kept.
        const stx::Receiver::Phase before = g_rx->phase();
        g_rx->NewSession();
        if (before != g_rx->phase())
            Log("[coop] guest: new link session: the transfer state starts over (%s -> %s)", stx::Receiver::PhaseName(before),
                stx::Receiver::PhaseName(g_rx->phase()));
    }
    g_gen = gen;
    OnLinkSession(now);
}

DWORD WINAPI Worker(LPVOID) {
    for (;;) {
        const double now = link::NowMsPrecise();
        bool ready = false;
        {
            Lock l;
            ready = g_version_ready;
        }
        if (!ready || !g_active.load()) {   // not started yet, or stopped (EmergencyStop): nothing read, written or sent
            Sleep(50);
            continue;
        }
        if (g_role == 2 && !g_gw.local_set) {
            Lock l;
            g_rx->SetLocalVersion(g_version);
            g_gw.local_set = true;
        }
        const link::Info li = link::GetInfo();
        char peer_name[64] = {};
        link::PeerName(peer_name, sizeof(peer_name), nullptr, nullptr);
        {
            Lock l;
            if (li.connected && !g_ui.connected) g_ui.connected_since = now;
            g_ui.connected = li.connected;
            g_ui.ever_connected = g_ui.ever_connected || li.connected;
            std::snprintf(g_ui.peer_name, sizeof(g_ui.peer_name), "%s", peer_name);
        }
        const std::uint32_t gen = link::Generation();
        static bool was_connected = false;
        if (gen != g_gen && li.connected) OnNewGeneration(gen, now);
        else if (li.connected && !was_connected && g_gen != 0) {
            Log("[coop] the link is back (link session #%u kept): handled as a new session (%s)", g_gen, g_role == 1 ? "host" : "guest");
            OnLinkSession(now);
        }
        was_connected = li.connected;
        std::uint8_t buf[stx::kMaxMessage];
        std::size_t size = 0;
        std::uint32_t mgen = 0;
        static unsigned bad = 0;
        while (link::PopSave(buf, sizeof(buf), size, mgen)) {
            if (mgen != g_gen) OnNewGeneration(mgen, now);
            stx::Message m;
            if (!stx::Decode(buf, size, m)) {
                if (++bad <= 5) Log("[coop] a save-channel message did not decode (%zu bytes): dropped", size);
                continue;
            }
            if (g_role == 1) HostOnMessage(m, now);
            else GuestOnMessage(m, now);
        }
        if (g_role == 1) HostWork(now);
        else GuestWork(now);
        Sleep(10);
    }
}

// ---------------------------------------------------------------- game thread
struct TickState {
    bool first = true;
    double first_tick_ms = 0;
    double title_since = 0;        // the session global 0 since then (0 = in a session)
    bool told_title_wait = false;
    double next_account_ms = 0;
    double next_view_ms = 0;
    std::uintptr_t prev_session = 0;
    // guest load
    enum class Load { idle, refresh, call, wait_world } load = Load::idle;
    PendingLoad job;
    double load_t = 0;
    bool load_at_title = false;
    std::uintptr_t start_player = 0;
    game::Guid start_guid;
    bool saw_gap = false;
    bool title_accepted = false;   // the title load returned 1: the game's own loading screen is up
    std::uint32_t loaded_counter = 0;
    // guest events
    double host_title_since = 0;
    double quit_at = 0;
    bool quit_unusable = false;
    const char* quit_why = "";
    double lost_since = 0;
    bool was_connected = false;
    double tp_offer_until = 0;
    bool tp_offered = false;
    double fallback_until = 0;
    // The local player entity is re-created a few times while a world loads (run 20261119-065746: 0 -> A -> 0 -> B -> 0 -> C
    // within ~5 s): a player counts only once it has been the same entity for kStableMs.
    std::uintptr_t seen_player = 0;
    game::Guid seen_guid;
    double seen_since = 0;
    bool hide_panel = false;       // hidden tests (Command 8): the panel is not drawn (the input block and the gate stay)
    bool lift_block = false;       // hidden tests (Command 8 2): the input block lifted too (the gate alone holds the title)
    bool title_test_off = false;   // hidden tests (Command 8 3): the game's own title menu back as well (title_wait off)
    double native_since = 0;       // since when the relabelled title page has been on screen (title_wait::Showing; 0 = not now)
    bool told_native_title = false;
    bool told_no_page = false;
    // The native title's run-time fallback: since when the title page is up but our label is not in it (title_wait::
    // LabelMissing; 0 = not now). After kNativeFallbackMs (or 45 s without any title page) the panel comes back, without
    // the input block, with the reason, until the relabel shows again.
    double label_missing_since = 0;
    bool native_fallback = false;  // the panel is up as the fallback (logged on each change)
};
TickState g_ts;
constexpr double kNativeFallbackMs = 2000;
constexpr double kNoTitlePageMs = 45000;

void PostLoadResult(stx::ResultCode code) {
    Lock l;
    g_load_result = static_cast<int>(code);
}

void FirstTick() {
    char build[64] = {};
    std::uint32_t version = 0, flags = 0;
    stx::GameVersion v;
    const bool ok = CallVersion(build, sizeof(build), &version, &flags);
    // "dsq 179/4027081 ..." -> the first two words, at most 24 characters.
    std::string b = build;
    std::size_t sp = b.find(' ');
    if (sp != std::string::npos) sp = b.find(' ', sp + 1);
    if (sp != std::string::npos) b.resize(sp);
    if (b.size() > stx::kBuildChars) b.resize(stx::kBuildChars);
    std::string note;
    if (!ok || !stx::SetBuild(v, b.c_str())) {
        stx::SetBuild(v, "unknown");
        note = "the game's build string could not be read";
    }
    v.savegame_version = version;
    v.save_flags = flags;
    if (!g_cfg.coop_test_build.empty()) {
        if (automation::Enabled() && stx::SetBuild(v, g_cfg.coop_test_build.c_str())) note = "coop_test_build: a FAKE build for the version test";
        else note = "coop_test_build ignored (hidden runs only)";
    }
    Lock l;
    g_version = v;
    g_version_ready = true;
    Log("[coop] this game: build '%s', save version %u, save flags 0x%X%s%s", v.build, v.savegame_version, v.save_flags, note.empty() ? "" : "; ",
        note.c_str());
}

void GuestLoadTick(double now, std::uintptr_t session, game::Entity* player, const game::Guid& guid, bool stable) {
    TickState& t = g_ts;
    switch (t.load) {
        case TickState::Load::idle: {
            if (t.quit_at) return;  // QuitGame is on its way to the title: the save is loaded there
            {
                Lock l;
                if (!g_pending.ready) return;
            }
            // At the title: only once its menu is up (rules::GuestTitleLoadReady: the relabelled title page on screen for
            // a moment, or, without it, the time rule past the boot screens / the teardown after QuitGame). Until then
            // the save waits; nothing of the game is called.
            if (session == 0 && !rules::GuestTitleLoadReady(now, t.first_tick_ms, true, t.title_since, t.native_since)) {
                if (!t.told_title_wait) {
                    t.told_title_wait = true;
                    Log("[coop] guest: the host's save is here; the title load waits until the title menu has been up for a while "
                        "(the game's own title page %.0f s on screen, or %.0f s after the start and %.0f s at the title)",
                        rules::kNativeTitleSettleMs / 1000.0, rules::kTitleBootMs / 1000.0, rules::kTitleSettleMs / 1000.0);
                }
                return;
            }
            if (session == 0)
                Log("[coop] guest: the title is ready for the load (%s)",
                    rules::NativeTitleReady(now, true, t.native_since) ? "the game's own title page is up" : "the time rule");
            t.told_title_wait = false;
            Lock l;
            t.job = g_pending;
            t.load = TickState::Load::refresh;
            t.load_t = now;
            return;
        }
        case TickState::Load::refresh: {
            const int has = CallRefreshAndHas(t.job.slot);
            Log("[coop] guest: slot cache refreshed (0x141A00DA0); manualsave%d %s", t.job.slot,
                has > 0 ? "exists" : has == 0 ? "is MISSING (the game's cache filter refused it)" : "check FAULTED");
            if (has <= 0) {
                PostLoadResult(stx::ResultCode::load_refused);
                Lock l;
                g_pending.ready = false;
                t.load = TickState::Load::idle;
                return;
            }
            t.load = TickState::Load::call;
            t.load_t = now + 200;  // the next tick after 200 ms (as the proof run)
            return;
        }
        case TickState::Load::call: {
            if (now < t.load_t) return;
            if (session == 0 && !rules::GuestTitleLoadReady(now, t.first_tick_ms, true, t.title_since, t.native_since)) return;   // left a world meanwhile
            t.load_at_title = session == 0;
            t.start_player = reinterpret_cast<std::uintptr_t>(player);
            t.start_guid = guid;
            t.saw_gap = player == nullptr;
            bool ok = false;
            if (!g_load_call) {
                // Fallback (a load call failed its byte check): hidden runs only, a virtual pad A on CONTINUE with the gate open
                // (the session's slots were wiped at its start, so CONTINUE is the save just written).
                if (t.load_at_title && automation::Enabled()) {
                    g_allow_until_ms.store(static_cast<long long>(now + 20000));
                    overlay::input::PadState p;
                    p.connected = true;
                    p.buttons = 0x1000;  // XINPUT_GAMEPAD_A
                    overlay::input::SetVirtualPad(p);
                    t.fallback_until = now + 200;
                    ok = true;
                    Log("[coop] guest: FALLBACK: the direct load calls are unavailable; virtual pad A on CONTINUE (gate open 20 s)");
                } else {
                    Log("[coop] guest: the direct load calls are unavailable and this is %s: the save cannot be loaded",
                        t.load_at_title ? "not a hidden run" : "the world");
                }
            } else if (t.load_at_title) {
                const int r = CallTitleLoad(t.job.slot);
                Log("[coop] guest: title load of manualsave%d (0x142624A70, gate opened for this call) -> %d", t.job.slot, r);
                ok = r == 1;
                if (ok) t.title_accepted = true;
            } else {
                ok = CallPauseLoad(t.job.slot);
                Log("[coop] guest: in-world load of manualsave%d (0x142636990) %s", t.job.slot, ok ? "requested" : "FAULTED");
            }
            {
                Lock l;
                g_pending.ready = false;
            }
            if (!ok) {
                PostLoadResult(stx::ResultCode::load_refused);
                t.load = TickState::Load::idle;
                return;
            }
            t.load = TickState::Load::wait_world;
            t.load_t = now;
            return;
        }
        case TickState::Load::wait_world: {
            if (t.fallback_until && now >= t.fallback_until) {
                overlay::input::SetVirtualPad(overlay::input::PadState{true});
                t.fallback_until = 0;
            }
            const bool differs = player && (reinterpret_cast<std::uintptr_t>(player) != t.start_player || guid != t.start_guid);
            if (!player || differs) t.saw_gap = true;
            if (stable && t.saw_gap && differs) {
                Log("[coop] guest: in the world after %.1f s: the host's save #%u (%s, '%s') is loaded", (now - t.load_t) / 1000.0, t.job.offer.load_counter,
                    t.job.offer.slot_name, t.job.title.c_str());
                PostLoadResult(stx::ResultCode::ok);
                t.loaded_counter = t.job.offer.load_counter;
                t.title_accepted = false;
                if (t.job.offer.reason == stx::Reason::late_join && !t.tp_offered) {
                    t.tp_offered = true;
                    t.tp_offer_until = now + kTeleportOfferMs;
                    Log("[coop] guest: you joined while the host was playing: 'Teleport to host' is offered once (F9 menu) for %.0f s",
                        kTeleportOfferMs / 1000.0);
                }
                t.load = TickState::Load::idle;
                return;
            }
            if (now - t.load_t > kLoadTimeoutMs) {
                Log("[coop] guest: the load of manualsave%d did not reach the world within %.0f s: FAILED", t.job.slot, kLoadTimeoutMs / 1000.0);
                PostLoadResult(stx::ResultCode::load_failed);
                t.title_accepted = false;
                t.load = TickState::Load::idle;
            }
            return;
        }
    }
}

// The guest's game back to its title (the host is there, or the link stayed lost): QuitGame, watched.
void GuestQuitToTitle(double now, const char* why) {
    TickState& t = g_ts;
    if (t.quit_at || t.quit_unusable) return;
    if (!g_quit_ok) {
        t.quit_unusable = true;
        Log("[coop] guest: %s, but QuitGame failed its byte check: staying in the world", why);
        return;
    }
    Log("[coop] guest: %s: QuitGame (0x142642B20) to go back to the title", why);
    t.quit_why = why;
    if (!CallQuit()) {
        t.quit_unusable = true;
        Log("[coop] guest: QuitGame FAULTED: staying in the world");
        return;
    }
    t.quit_at = now;
}

void BuildView(double now, bool at_title, bool in_world) {
    overlay::CoopView v;
    v.active = true;
    Ui ui;
    {
        Lock l;
        ui = g_ui;
    }
    const char* host = ui.peer_name[0] ? ui.peer_name : "the host";
    const char* Host = ui.peer_name[0] ? ui.peer_name : "The host";  // at the start of a sentence
    const char* partner = ui.peer_name[0] ? ui.peer_name : "your partner";
    const char* Partner = ui.peer_name[0] ? ui.peer_name : "Your partner";
    const link::Info li = link::GetInfo();
    if (!g_off[0] && g_role == 2) {
        TickState& t = g_ts;
        const bool loading = t.load != TickState::Load::idle;
        // The game's own title menu carries the state (title_wait.cpp): NEW GAME's label, everything but EXIT GAME hidden.
        // Then there is no full-screen panel and no input block at all: the guest sees the game's own screens (the boot
        // logos, or nothing but black and the game's spinner while they are skipped), then its own title with the label;
        // the gate (Guarding) holds every start without the overlay. The panel is only the fallback when the native title
        // is off (its byte checks, or the hidden tests' Command 8 3 / coop_test_title_off).
        const bool native_title = title_wait::Installed() && !t.title_test_off;
        // Run-time fallback of the native title: the page is up but the label never got into it (a fault in the data
        // source write, the NEW GAME key not found, another data source), or no title page at all after 45 s. The panel
        // explains the wait again (no input block: the game's own screen stays usable); hidden as soon as the label shows.
        const char* native_fail = nullptr;
        if (native_title && at_title && !t.title_accepted) {
            if (t.label_missing_since && now - t.label_missing_since > kNativeFallbackMs)
                native_fail = title_wait::Faults() ? "its label write faulted" : "the label is not in the menu";
            else if (!title_wait::PageSeen() && t.first_tick_ms && now - t.first_tick_ms > kNoTitlePageMs)
                native_fail = "no title page yet";
        }
        if ((native_fail != nullptr) != t.native_fallback) {
            t.native_fallback = native_fail != nullptr;
            Log("[title] %s%s", t.native_fallback ? "the game's own title does not show the session: the waiting panel is back (no input block): "
                                                  : "the game's own title shows the session again: the waiting panel is hidden",
                native_fail ? native_fail : "");
        }
        v.block_input = at_title && !t.title_accepted && !t.lift_block && !native_title;
        v.waiting = at_title && !t.title_accepted && !t.hide_panel && (!native_title || native_fail);
        {
            rules::TitleLabelIn tl;
            tl.host = rules::SafeHostName(ui.peer_name);
            tl.connected = ui.connected;
            tl.ever_connected = ui.ever_connected;
            tl.version_mismatch = ui.version_mismatch;
            bool pending = false;   // received and written, our load call not started yet: "Loading" already
            {
                Lock l;
                pending = g_pending.ready;
            }
            tl.loading = loading || t.title_accepted || pending;
            tl.receiving = ui.receiving;
            tl.have = ui.have;
            tl.size = ui.size;
            tl.no_reply = ui.connected && !ui.have_host_state && now - ui.connected_since > kNoReplyMs;
            tl.have_host_state = ui.have_host_state;
            tl.host_phase = ui.host_state.phase;
            tl.new_game_pending = ui.have_host_state && ui.host_state.new_game_pending;
            // Kept through our own title load until the world is there: the title page fades out under it (never a flash
            // of the game's own items).
            // (A version mismatch: the label says "Game versions differ: update both games"; both builds are in the log.)
            title_wait::Set((at_title || (t.title_accepted && !in_world)) && !t.title_test_off, rules::TitleLabel(tl).c_str());
        }
        std::snprintf(v.title, sizeof(v.title), "Waiting for %s to start the game", host);
        if (!ui.connected) std::snprintf(v.lines[0], sizeof(v.lines[0]), "Link: %s", ui.ever_connected ? "reconnecting..." : "connecting...");
        else std::snprintf(v.lines[0], sizeof(v.lines[0]), "Link: connected (%u ms)", li.rtt_ms);
        if (ui.version_mismatch) {
            std::snprintf(v.lines[1], sizeof(v.lines[1]), "Game versions differ: %s vs %s. Update both games.", ui.host_build, g_version.build);
        } else if (ui.have_host_state) {
            const stx::State& s = ui.host_state;
            if (s.new_game_pending) std::snprintf(v.lines[1], sizeof(v.lines[1]), "%s started a new game. You join at their first save.", Host);
            else if (s.phase == stx::HostPhase::title) std::snprintf(v.lines[1], sizeof(v.lines[1]), "%s is at the title screen.", Host);
            else if (s.phase == stx::HostPhase::loading)
                std::snprintf(v.lines[1], sizeof(v.lines[1]), "%s is loading %s.", Host, ui.offer_slot[0] ? ui.offer_slot : "a save");
            else std::snprintf(v.lines[1], sizeof(v.lines[1]), "%s is in the game.", Host);
        } else if (ui.connected && now - ui.connected_since > kNoReplyMs) {
            std::snprintf(v.lines[1], sizeof(v.lines[1]), "No word from %s's save session: is 'Host holds the save' on for the host?", host);
        }
        if (ui.receiving && ui.size) {
            v.progress = static_cast<float>(ui.have) / static_cast<float>(ui.size);
            std::snprintf(v.progress_text, sizeof(v.progress_text), "Receiving %s's save: %.1f / %.1f MB", host, ui.have / 1048576.0, ui.size / 1048576.0);
        } else if (loading) {
            v.progress = 1.0f;
            std::snprintf(v.progress_text, sizeof(v.progress_text), "Loading %s's save...", host);
        }
        std::snprintf(v.lines[2], sizeof(v.lines[2]), "You play a copy of %s's world. Your own saves are not used.", host);
        if (g_title_off[0]) std::snprintf(v.lines[3], sizeof(v.lines[3]), "(The game's own title menu is not used: %s)", g_title_off);
        else if (native_fail) std::snprintf(v.lines[3], sizeof(v.lines[3]), "(The game's own title menu does not show the session: %s)", native_fail);
        v.leave = true;
        // In the world: notices.
        if (in_world || !at_title) {
            if (ui.version_mismatch) {
                std::snprintf(v.banner, sizeof(v.banner), "Game versions differ: %s vs %s. Update both games.", ui.host_build, g_version.build);
                v.banner_warn = true;
            } else if (t.lost_since) {
                const double left = (kLinkLossMs - (now - t.lost_since)) / 1000.0;
                std::snprintf(v.banner, sizeof(v.banner), "Connection to %s lost. Reconnecting... (back to the waiting screen in %.0f s)", host,
                              left > 0 ? left : 0.0);
                v.banner_warn = true;
            } else if (ui.have_host_state && ui.host_state.phase == stx::HostPhase::title && in_world) {
                std::snprintf(v.banner, sizeof(v.banner), "%s is at the title screen.", Host);
                v.banner_warn = true;
            } else if (ui.receiving && ui.size) {
                std::snprintf(v.banner, sizeof(v.banner), "%s loaded another save: receiving it (%.0f%%)", Host, 100.0 * ui.have / ui.size);
            } else if (t.tp_offer_until > now) {
                std::snprintf(v.banner, sizeof(v.banner), "You joined while %s was playing. Teleport to them: F9 menu > Teleport to host (once)", host);
                v.teleport_offer = true;
            }
        }
        std::snprintf(v.status, sizeof(v.status), "guest; %s; host %s; loaded #%u", ui.connected ? "linked" : "not linked",
                      !ui.have_host_state                                 ? "?"
                      : ui.host_state.phase == stx::HostPhase::title     ? "at the title"
                      : ui.host_state.phase == stx::HostPhase::loading   ? "loading"
                                                                         : "in the game",
                      t.loaded_counter);
    } else if (!g_off[0]) {
        title_wait::Set(false, "");
        // Host notices.
        bool new_game = false;
        stx::HostPhase host_phase = stx::HostPhase::title;
        std::uint32_t counter = 0;
        {
            Lock l;
            new_game = g_new_game;
            host_phase = g_host_phase;
            counter = g_load_counter;
        }
        if (ui.connected) {
            if (ui.guest_version_mismatch) {
                std::snprintf(v.banner, sizeof(v.banner), "Game versions differ: %s vs %s. Update both games.", g_version.build, ui.guest_build);
                v.banner_warn = true;
            } else if (ui.offer_problem[0]) {
                std::snprintf(v.banner, sizeof(v.banner), "Your save could not be sent to %s: %s", partner, ui.offer_problem);
                v.banner_warn = true;
            } else if (ui.sender_phase == stx::Sender::Phase::streaming && ui.offer_size) {
                std::snprintf(v.banner, sizeof(v.banner), "Sending your save to %s: %.0f%%", partner, 100.0 * ui.sent / ui.offer_size);
            } else if (ui.sender_phase == stx::Sender::Phase::offered || ui.sender_phase == stx::Sender::Phase::ended) {
                std::snprintf(v.banner, sizeof(v.banner), "Partner joining: sending your save");
            } else if (ui.have_gstate && ui.gstate.phase == stx::GuestPhase::loading) {
                std::snprintf(v.banner, sizeof(v.banner), "Partner joining: %s is loading your save", partner);
            } else if (new_game) {
                std::snprintf(v.banner, sizeof(v.banner), "Your partner joins at your first save");
            } else if (ui.partner_loaded_ms && now - ui.partner_loaded_ms < 8000) {
                std::snprintf(v.banner, sizeof(v.banner), "%s is in your world", Partner);
            } else if (ui.connected_since && now - ui.connected_since < 10000 && host_phase == stx::HostPhase::title) {
                std::snprintf(v.banner, sizeof(v.banner), "%s connected: they join when you start the game", Partner);
            }
        }
        std::snprintf(v.status, sizeof(v.status), "host; %s; offer %s #%u; partner %s", ui.connected ? "linked" : "not linked",
                      stx::Sender::PhaseName(ui.sender_phase), counter,
                      !ui.have_gstate                                  ? "?"
                      : ui.gstate.phase == stx::GuestPhase::waiting    ? "waiting"
                      : ui.gstate.phase == stx::GuestPhase::receiving  ? "receiving"
                      : ui.gstate.phase == stx::GuestPhase::loading    ? "loading"
                      : ui.gstate.phase == stx::GuestPhase::world      ? "in the world"
                                                                       : "refused");
    }
    {
        Lock l;
        if (!v.banner[0] && g_vnotice[0] && now < g_vnotice_until) {
            std::snprintf(v.banner, sizeof(v.banner), "%s", g_vnotice);
            v.banner_warn = g_vnotice_warn;
        }
    }
    if (g_off[0]) {
        title_wait::Set(false, "");
        // The reason on screen for the first minute (then in the menu header only).
        static double first = 0;
        if (!first) first = now;
        std::snprintf(v.status, sizeof(v.status), "OFF: %s", g_off);
        if (now - first < 60000) std::snprintf(v.banner, sizeof(v.banner), "Co-op save session off: %s", g_off);
        v.banner_warn = true;
    }
    overlay::SetCoopView(v);
}

}  // namespace

// ---------------------------------------------------------------- public
void Install(const Config& config) noexcept {
    g_cfg = config;
    if (!config.host_save) {
        if (config.role == Role::host || config.role == Role::guest) {
            g_plain = config.role == Role::host ? 1 : 2;
            Log("[coop] host_save=0 (%s): the host-held save session is OFF, and with it the guest's waiting screen, the shared vehicles, "
                "riding together and vehicle hits; each player plays their own save (an overlay notice says so)",
                g_plain == 1 ? "host" : "guest");
        }
        return;
    }
    if (config.role != Role::host && config.role != Role::guest) {
        Log("[coop] host_save=1 ignored: role %s (only host and guest)", RoleName(config.role));
        return;
    }
    g_role = config.role == Role::host ? 1 : 2;
    if (g_role == 2 && !automation::SavesRedirected()) {
        // DllMain refuses this already (sandbox::Mode::refuse_host_save); a session.ini that DllMain read differently
        // (it never should: both strip a UTF-8 BOM) must still never leave the guest's game on its own saves.
        Log("[coop] host_save=1 guest WITHOUT the save redirect: ending ds.exe before it can touch your saves");
        automation::RefuseUnredirected("role=guest with host_save=1 but the save redirect is not active (coop::Install)");
        return;
    }
    const unsigned failed = CheckSites();
    // The guest's direct load calls are required outside hidden runs: without them the session could never load the
    // host's save (the virtual-pad fallback is for hidden runs only), so it is off with its reason instead.
    const unsigned need = kCommon | (g_role == 1 ? kHostDetect : (kGate | kRefresh | (automation::Enabled() ? 0u : kLoadCall)));
    const unsigned total = static_cast<unsigned>(sizeof(kSites) / sizeof(kSites[0]));
    if (failed & need) {
        std::snprintf(g_off, sizeof(g_off), "this game build differs (%s byte check failed)",
                      (failed & kCommon)        ? "session"
                      : (failed & kHostDetect)  ? "host detection"
                      : (failed & kGate)        ? "title gate"
                      : (failed & kRefresh)     ? "slot refresh"
                                                : "save load call");
        Log("[coop] host-held save session OFF (%s): %s", g_role == 1 ? "host" : "guest", g_off);
        overlay::CoopView v;
        v.active = true;
        std::snprintf(v.status, sizeof(v.status), "OFF: %s", g_off);
        std::snprintf(v.banner, sizeof(v.banner), "Co-op save session off: %s", g_off);
        v.banner_warn = true;
        overlay::SetCoopView(v);
        return;
    }
    g_load_call = !(failed & kLoadCall);
    g_quit_ok = !(failed & kQuit);
    g_test_retry_ok = !(failed & kTestRetry);
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        std::snprintf(g_off, sizeof(g_off), "MinHook init failed (%s)", MH_StatusToString(init));
        Log("[coop] host-held save session OFF: %s", g_off);
        return;
    }
    bool hooks = true;
    if (g_role == 1) {
        hooks = HookOne(kTitleLoad, reinterpret_cast<void*>(&H_tload), reinterpret_cast<void**>(&o_tload), "title load") &&
                HookOne(kConvLoad, reinterpret_cast<void*>(&H_conv), reinterpret_cast<void**>(&o_conv), "converted load") &&
                HookOne(kNewWorld, reinterpret_cast<void*>(&H_newworld), reinterpret_cast<void**>(&o_newworld), "new world") &&
                HookOne(kWorldLoadReq, reinterpret_cast<void*>(&H_wreq), reinterpret_cast<void**>(&o_wreq), "in-world load request");
    } else {
        hooks = HookOne(kTitleFlow, reinterpret_cast<void*>(&H_flow), reinterpret_cast<void**>(&o_flow), "title menu start flow") &&
                HookOne(kLoadDecide, reinterpret_cast<void*>(&H_decide), reinterpret_cast<void**>(&o_decide), "load page decision") &&
                HookOne(kConvLoad, reinterpret_cast<void*>(&H_conv), reinterpret_cast<void**>(&o_conv), "converted load") &&
                HookOne(kNewWorld, reinterpret_cast<void*>(&H_newworld), reinterpret_cast<void**>(&o_newworld), "new world");
        // The title load is the gate's second layer and the guest's own load call (through its trampoline).
        if (hooks && g_load_call)
            hooks = HookOne(kTitleLoad, reinterpret_cast<void*>(&H_tload), reinterpret_cast<void**>(&o_tload), "title load");
    }
    if (!hooks) {
        std::snprintf(g_off, sizeof(g_off), "a hook could not be installed");
        Log("[coop] host-held save session OFF: %s", g_off);
        return;
    }
    // The guest's title in the game's own menu (its byte checks are its own: a mismatch leaves the full-screen panel).
    // Not with the hidden-run virtual-pad fallback: that one presses CONTINUE on the game's menu.
    if (g_role == 2) {
        if (!g_load_call) std::snprintf(g_title_off, sizeof(g_title_off), "the direct load calls are unavailable");
        else if (config.coop_test_title_off && automation::Enabled()) {
            std::snprintf(g_title_off, sizeof(g_title_off), "coop_test_title_off=1 (hidden test of the fallback)");
            Log("[title] the guest's title in the game's own menu is OFF (%s): the full-screen waiting panel is used", g_title_off);
        } else if (!title_wait::Install(g_title_off, sizeof(g_title_off), config.guest_skip_logos) && !g_title_off[0])
            std::snprintf(g_title_off, sizeof(g_title_off), "not installed");
        if (config.coop_test_title_off && !automation::Enabled()) Log("[title] coop_test_title_off ignored (hidden runs only)");
    }
    if (g_role == 2) {
        const wchar_t* root = automation::SavesScratchRoot();
        if (!root) {
            std::snprintf(g_off, sizeof(g_off), "the save redirect has no folder (nothing is ever written)");
            Log("[coop] host-held save session OFF: %s", g_off);
            return;
        }
        g_target.scratch_root = root;
        g_target.redirected = true;
        const std::wstring profile = EnvW(L"USERPROFILE"), local = EnvW(L"LOCALAPPDATA");
        if (!profile.empty()) g_target.real_roots.push_back(profile + L"\\AppData\\Local\\KojimaProductions");
        if (!local.empty()) g_target.real_roots.push_back(local + L"\\KojimaProductions");
        const std::string problem = stx::SandboxTargetProblem(g_target);
        if (!problem.empty()) {
            std::snprintf(g_off, sizeof(g_off), "the co-op folder is not usable: %s", problem.c_str());
            Log("[coop] host-held save session OFF: %s", g_off);
            return;
        }
        g_store = new (std::nothrow) stx::SandboxPartStore(g_target);
        g_rx = g_store ? new (std::nothrow) stx::Receiver(*g_store) : nullptr;
        if (!g_rx) {
            std::snprintf(g_off, sizeof(g_off), "out of memory");
            return;
        }
    }
    g_active.store(true);
    g_worker = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    Log("[coop] host-held save session ON (%s): %u byte checks, %s; hooks in; %s", g_role == 1 ? "host: your own saves, offered read only" : "guest: the title is gated",
        total, failed ? "some optional checks failed (see above)" : "all ok",
        g_role == 1 ? "the partner gets the save you start"
                    : g_load_call ? (g_quit_ok ? "loads by the game's own calls; follows the host to the title"
                                               : "loads by the game's own calls; QuitGame unavailable: stays in the world when the host quits")
                                  : "the direct load calls are unavailable: hidden-run virtual-pad fallback only");
    if (g_role == 2 && g_title_off[0]) {
        // The fallback only (the game's own title menu is not used): the full-screen panel from the start, the reason in it.
        overlay::CoopView v;
        v.active = true;
        v.waiting = true;
        v.block_input = true;
        v.leave = true;
        std::snprintf(v.title, sizeof(v.title), "Waiting for the host to start the game");
        std::snprintf(v.lines[0], sizeof(v.lines[0]), "Link: connecting...");
        std::snprintf(v.lines[2], sizeof(v.lines[2]), "You play a copy of the host's world. Your own saves are not used.");
        std::snprintf(v.lines[3], sizeof(v.lines[3]), "(The game's own title menu is not used: %s)", g_title_off);
        std::snprintf(v.status, sizeof(v.status), "guest; starting");
        overlay::SetCoopView(v);
    } else if (g_role == 2) {
        // The game's own screens from the start: no panel, no input block; the menu header's status only.
        Log("[coop] guest: no waiting panel: the game's own title menu shows the session (\"Waiting for <host> to start the game\", "
            "EXIT GAME); the gate holds every start");
        overlay::CoopView v;
        v.active = true;
        std::snprintf(v.status, sizeof(v.status), "guest; starting");
        overlay::SetCoopView(v);
    }
}

bool Active() noexcept { return g_active.load(); }

const char* OffReason() noexcept {
    if (g_plain) return "host_save=0";
    return g_active.load() ? "" : (g_off[0] ? g_off : "not installed");
}

bool PlainNoticeWanted() noexcept { return g_plain != 0; }

void NoticeTick(const char* features_off, bool local_in_world, double now) noexcept {
    if (!g_plain || now < g_pn.next_ms) return;
    g_pn.next_ms = now + 500;
    // Shown at the title, while loading and for the first minute of each stay in the world; the menu header keeps it.
    if (local_in_world) {
        g_pn.away_since = 0;
        if (!g_pn.world_since) g_pn.world_since = now;
    } else {
        if (!g_pn.away_since) g_pn.away_since = now;
        if (now - g_pn.away_since > 3000) g_pn.world_since = 0;
    }
    const bool show = !g_pn.world_since || now - g_pn.world_since < 60000;
    const char* text = features_off && features_off[0] ? features_off : "host_save=0";
    if (g_pn.published && show == g_pn.shown && std::strcmp(text, g_pn.text) == 0) return;
    g_pn.published = true;
    g_pn.shown = show;
    std::snprintf(g_pn.text, sizeof(g_pn.text), "%s", text);
    overlay::CoopView v;
    v.active = true;   // the menu header's status line
    std::snprintf(v.status, sizeof(v.status), "OFF: %s", text);
    if (show) {
        std::snprintf(v.banner, sizeof(v.banner), "Co-op features OFF: %s", text);
        v.banner_warn = true;
    }
    overlay::SetCoopView(v);
    Log("[coop] notice %s: co-op features OFF: %s", show ? "shown" : "hidden (the menu header keeps it)", text);
}

int SessionRole() noexcept { return g_active.load() ? g_role : 0; }

bool WorldReady(std::uint32_t* load_counter) noexcept {
    if (!g_active.load() || g_role == 0) return false;
    Lock l;
    if (g_role == 1) {
        if (g_host_phase != stx::HostPhase::world) return false;
        if (load_counter) *load_counter = g_load_counter;
        return true;
    }
    if (!g_guest_world || g_ts.load != TickState::Load::idle || g_ts.loaded_counter == 0) return false;
    if (load_counter) *load_counter = g_ts.loaded_counter;
    return true;
}

void SetVehicleNotice(const char* text, bool warn, double until_ms) noexcept {
    Lock l;
    std::snprintf(g_vnotice, sizeof(g_vnotice), "%s", text ? text : "");
    g_vnotice_warn = warn;
    g_vnotice_until = until_ms;
}

void EmergencyStop(const char* reason) noexcept {
    if (g_role == 0 || !g_active.exchange(false)) return;   // never on, or stopped already
    g_allow.store(false);
    g_allow_until_ms.store(0);
    std::snprintf(g_off, sizeof(g_off), "stopped: %s", reason ? reason : "?");
    Log("[coop] host-held save session STOPPED (%s): %s; the gate is open, the waiting screen and its input block are gone",
        g_role == 1 ? "host" : "guest", reason ? reason : "?");
    if (g_ts.fallback_until) overlay::input::SetVirtualPad(overlay::input::PadState{true});   // the hidden-run fallback's A released
    title_wait::Set(false, "");   // the game's own title menu back at its next update
    overlay::CoopView v;
    v.active = true;
    v.waiting = false;
    v.block_input = false;
    std::snprintf(v.status, sizeof(v.status), "OFF: %s", g_off);
    std::snprintf(v.banner, sizeof(v.banner), "Co-op save session stopped: %s", reason ? reason : "?");
    v.banner_warn = true;
    overlay::SetCoopView(v);
}

void Tick(double now) noexcept {
    if (g_role == 0) return;
    if (!g_active.load()) {
        if (g_off[0] && now >= g_ts.next_view_ms) {
            g_ts.next_view_ms = now + 1000;
            BuildView(now, true, false);
        }
        return;
    }
    TickState& t = g_ts;
    if (t.first) {
        t.first = false;
        t.first_tick_ms = now;
        FirstTick();
    }
    const std::uintptr_t session = Session();
    if (session) t.title_since = 0;
    else if (!t.title_since) t.title_since = now;
    // The account the game runs as (its save folder's name), from the game's own steam_api, until known.
    if (now >= t.next_account_ms) {
        bool known = false;
        {
            Lock l;
            known = g_account != 0;
        }
        if (!known) {
            t.next_account_ms = now + 2000;
            const std::uint64_t id = steam::QuerySelfId();
            if (id & 0xFFFFFFFFull) {
                Lock l;
                g_account = static_cast<std::uint32_t>(id & 0xFFFFFFFFull);
            }
            if (id) Log("[coop] the game runs as Steam account %u: its save folder is DeathStrandingDC\\%u", static_cast<unsigned>(id & 0xFFFFFFFFull),
                        static_cast<unsigned>(id & 0xFFFFFFFFull));
        } else {
            t.next_account_ms = 1e300;
        }
    }
    game::Entity* player = nullptr;
    game::TryGetLocalPlayer(&player);
    game::Guid guid;
    if (player && !game::TryReadGuid(player, &guid)) player = nullptr;
    if (reinterpret_cast<std::uintptr_t>(player) != t.seen_player || (player && guid != t.seen_guid)) {
        t.seen_player = reinterpret_cast<std::uintptr_t>(player);
        t.seen_guid = player ? guid : game::Guid{};
        t.seen_since = now;
    }
    const bool stable = player != nullptr && now - t.seen_since >= rules::kStableMs;
    g_tick_player.store(stable);
    const bool in_world = session != 0 && stable;
    const bool at_title = session == 0;
    bool connected = false;
    stx::State hs;
    bool have_hs = false;
    {
        Lock l;
        connected = g_ui.connected;
        hs = g_ui.host_state;
        have_hs = g_ui.have_host_state;
        if (g_role == 1) {
            if (stable && !g_player_since_detect && (reinterpret_cast<std::uintptr_t>(player) != g_detect_player || guid != g_detect_guid)) {
                g_player_since_detect = true;
                if (g_host_load_pending) Log("[coop] host: in the world (load #%u) after %.1f s", g_load_counter, (now - g_host_load_ms) / 1000.0);
                g_host_load_pending = false;
            }
            if (g_host_load_pending && now - g_host_load_ms > 180000) g_host_load_pending = false;
            const stx::HostPhase before = g_host_phase;
            g_host_phase = rules::HostPhaseOf(session != 0, stable, g_host_load_pending);
            if (before != g_host_phase && g_host_phase == stx::HostPhase::title && t.prev_session) {
                g_new_game = false;
                Log("[coop] host: back at the title (the game session ended)");
            }
        } else {
            g_guest_world = in_world;
            g_guest_title = at_title;
        }
    }
    if (g_role == 2) {
        // The relabelled title page on screen (the native title's "title menu is up" signal for the load rule).
        if (at_title && title_wait::Showing(now)) {
            if (!t.native_since) {
                t.native_since = now;
                if (!t.told_native_title) {
                    t.told_native_title = true;
                    Log("[coop] guest: the game's own title page shows the session %.1f s after the first tick", (now - t.first_tick_ms) / 1000.0);
                }
            }
        } else {
            t.native_since = 0;
        }
        if (at_title && title_wait::LabelMissing(now)) {
            if (!t.label_missing_since) t.label_missing_since = now;
        } else {
            t.label_missing_since = 0;
        }
        if (at_title && title_wait::Installed() && !title_wait::PageSeen() && !t.told_no_page && now - t.first_tick_ms > kNoTitlePageMs) {
            t.told_no_page = true;
            Log("[title] no title page 45 s after the start (the game still in its boot screens, or a notice before them?): the waiting "
                "panel explains the wait (no input block); the gate holds; the load waits for the title (the time rule is the backstop)");
        }
        GuestLoadTick(now, session, player, guid, stable);
        // The host is at the title: follow it there (D1-4), once the guest's own world is up.
        if (have_hs && connected && hs.phase == stx::HostPhase::title && in_world && t.load == TickState::Load::idle) {
            if (!t.host_title_since) t.host_title_since = now;
            if (now - t.host_title_since >= kHostTitleMs) GuestQuitToTitle(now, "the host went back to the title");
        } else {
            t.host_title_since = 0;
        }
        // A lost link (D1-5): the banner, then back to the title after 60 s; the host's state decides on reconnect.
        if (!connected && t.was_connected && session && !t.lost_since) {
            t.lost_since = now;
            Log("[coop] guest: the link to the host is lost: reconnecting (back to the waiting screen after %.0f s)", kLinkLossMs / 1000.0);
        }
        if (connected && t.lost_since) {
            Log("[coop] guest: the link is back after %.1f s: the host compares (load counter, save) and offers again if they differ",
                (now - t.lost_since) / 1000.0);
            t.lost_since = 0;
        }
        if (t.lost_since && now - t.lost_since >= kLinkLossMs && session) {
            GuestQuitToTitle(now, "the link stayed lost for 60 s");
            t.lost_since = 0;
        }
        if (!session) t.lost_since = 0;
        t.was_connected = connected;
        if (t.quit_at) {
            if (!session) {
                Log("[coop] guest: back at the title %.1f s after QuitGame (%s): the waiting screen again", (now - t.quit_at) / 1000.0, t.quit_why);
                t.quit_at = 0;
            } else if (now - t.quit_at > kQuitWaitMs) {
                Log("[coop] guest: QuitGame did not reach the title within %.0f s: staying in the world (a notice says where the host is)",
                    kQuitWaitMs / 1000.0);
                t.quit_at = 0;
                t.quit_unusable = true;
            }
        }
        if (t.prev_session && !session) t.title_accepted = false;
        const unsigned req = overlay::TakeCoopRequests();
        if (req & overlay::kCoopLeave) Log("[coop] guest: Leave pressed on the waiting screen: the game closes (the overlay posted WM_CLOSE)");
        if ((req & overlay::kCoopTeleport) && t.tp_offer_until > now) {
            t.tp_offer_until = 0;
            const std::uint32_t serial = debug::Queue(debug::Action::teleport_to_partner);
            Log("[coop] guest: Teleport to host accepted (debug action #%u: Teleport to partner)", serial);
        }
        if (t.tp_offer_until && now > t.tp_offer_until) {
            t.tp_offer_until = 0;
            Log("[coop] guest: the teleport offer ended unused");
        }
    }
    t.prev_session = session;
    if (now >= t.next_view_ms) {
        t.next_view_ms = now + 100;
        BuildView(now, at_title, in_world);
    }
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (g_role == 0) return std::snprintf(msg, cap, "no host-held save session (host_save=0 or role %s)", RoleName(g_cfg.role)), false;
    if (!g_active.load()) return std::snprintf(msg, cap, "the host-held save session is OFF: %s", g_off), false;
    const double now = link::NowMsPrecise();
    switch (op) {
        case 0: {
            Ui ui;
            {
                Lock l;
                ui = g_ui;
            }
            if (g_role == 1)
                std::snprintf(msg, cap, "host: phase %d, load #%u, offer %s (%u/%u), partner %s phase %d loaded #%u, version '%s'%s, gate n/a",
                              static_cast<int>(g_host_phase), g_load_counter, stx::Sender::PhaseName(ui.sender_phase), ui.sent, ui.offer_size,
                              ui.connected ? "linked" : "not linked", ui.have_gstate ? static_cast<int>(ui.gstate.phase) : -1,
                              ui.have_gstate ? ui.gstate.loaded_load_counter : 0, g_version.build,
                              ui.guest_version_mismatch ? " (the partner's DIFFERS)" : "");
            else
                std::snprintf(msg, cap, "guest: host %s phase %d load #%u, receiving %u/%u, loaded #%u, gate blocks %u, version '%s'%s",
                              ui.connected ? "linked" : "not linked", ui.have_host_state ? static_cast<int>(ui.host_state.phase) : -1,
                              ui.host_state.load_counter, ui.have, ui.size, g_ts.loaded_counter, g_blocked.load(), g_version.build,
                              ui.version_mismatch ? " (the host's DIFFERS)" : "");
            Log("[coop] status: %s", msg);
            if (g_role == 2) {
                char tw[220];
                title_wait::Describe(tw, sizeof(tw));
                Log("[coop] status: %s%s%s", tw, g_title_off[0] ? "; off: " : "", g_title_off);
            }
            return true;
        }
        case 1: {
            if (!automation::Enabled()) return std::snprintf(msg, cap, "the test link blackout is for hidden runs only"), false;
            const double ms = v[1] < 0 ? 0 : v[1] > 600000 ? 600000 : v[1];
            link::TestBlackout(static_cast<std::uint32_t>(ms));
            std::snprintf(msg, cap, "test blackout: every received datagram is dropped for %.0f ms (dropped so far %llu)", ms,
                          static_cast<unsigned long long>(link::TestBlackoutDropped()));
            Log("[coop] %s", msg);
            return true;
        }
        case 2: {
            if (g_role != 1) return std::snprintf(msg, cap, "host only"), false;
            HostEvent ev;
            ev.type = 4;
            ev.index = -1;
            ev.newest = true;
            ev.reason = stx::Reason::late_join;
            ev.t = now;
            Lock l;
            ev.load_counter = ++g_load_counter;
            g_events.push_back(ev);
            std::snprintf(msg, cap, "the newest save is offered as load #%u", ev.load_counter);
            return true;
        }
        case 3: {
            if (g_role != 2) return std::snprintf(msg, cap, "guest only"), false;
            if (g_ts.tp_offer_until <= now) return std::snprintf(msg, cap, "no teleport offer now"), false;
            g_ts.tp_offer_until = 0;
            const std::uint32_t serial = debug::Queue(debug::Action::teleport_to_partner);
            std::snprintf(msg, cap, "Teleport to host accepted (debug action #%u: Teleport to partner)", serial);
            Log("[coop] guest: %s", msg);
            return true;
        }
        case 4: {
            if (g_role != 1) return std::snprintf(msg, cap, "host only"), false;
            const std::wstring account = HostAccountDir(nullptr);
            const std::vector<SlotInfo> list = account.empty() ? std::vector<SlotInfo>() : ListSlots(account, false);
            std::string all;
            for (const SlotInfo& s : list) all += s.name + " ";
            Log("[coop] host: %zu slot(s): %s", list.size(), all.c_str());
            std::snprintf(msg, cap, "%zu slot(s) logged", list.size());
            return true;
        }
        case 5: {  // hidden tests: the pause menu's Load of (type v1, index v2) on the host (the proof's in-world load)
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            if (g_role != 1 || !g_load_call || !Session()) return std::snprintf(msg, cap, "host in the world only (and the load call byte-checked)"), false;
            const int type = static_cast<int>(v[1]), index = static_cast<int>(v[2]);
            const bool ok = CallPauseLoadAny(type, index);
            std::snprintf(msg, cap, "test: pause-menu Load (0x142636990) of type %d index %d %s", type, index, ok ? "requested" : "FAULTED");
            Log("[coop] %s", msg);
            return ok;
        }
        case 6: {  // hidden tests: QuitGame on this game (the host back to its title)
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            if (!g_quit_ok || !Session()) return std::snprintf(msg, cap, "in the world only (and QuitGame byte-checked)"), false;
            const bool ok = CallQuit();
            std::snprintf(msg, cap, "test: QuitGame (0x142642B20) %s", ok ? "called" : "FAULTED");
            Log("[coop] %s", msg);
            return ok;
        }
        case 7: {  // hidden tests: the checkpoint retry request (as the Game Over screen's Retry)
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            if (g_role != 1 || !g_test_retry_ok) return std::snprintf(msg, cap, "host only (and the guard byte-checked)"), false;
            // 7: retry with a checkpoint; 7 1: only ask whether one exists (always ok, for polling); 7 2: request it even
            // without one (the game ignores it then; the host's detection still sees a Retry)
            const int mode = v[1] == 1 ? 1 : v[1] == 2 ? 2 : 0;
            const int r = CallRetry(mode);
            if (mode == 1)
                std::snprintf(msg, cap, "test: checkpoint %s (0x14362B4A0)", r > 0 ? "PRESENT" : r == 0 ? "absent" : "check FAULTED");
            else
                std::snprintf(msg, cap, "test: checkpoint retry (0x14362D750 with index -2, reason 5) %s",
                              r == 1   ? "requested"
                              : r == 2 ? "requested WITHOUT a checkpoint (the game ignores it; only the detection is tested)"
                              : r == 0 ? "not requested: no checkpoint"
                                       : "FAULTED");
            Log("[coop] %s", msg);
            return mode == 1 ? r >= 0 : r > 0;
        }
        case 9: {  // hidden tests: a New Game's first save stood in for by an existing slot (type v1, index v2): it is dropped
                   // from the baseline, so the read-only poll offers it (the intro outlasts a hidden run; nothing is written)
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            if (g_role != 1) return std::snprintf(msg, cap, "host only"), false;
            const int type = static_cast<int>(v[1]), index = static_cast<int>(v[2]);
            const char* base = type == 4 ? "autosave" : type == 2 ? "quicksave" : type == 1 ? "manualsave" : nullptr;
            if (!base || index < 0 || index > 99) return std::snprintf(msg, cap, "type 4/2/1 and index 0..99"), false;
            Lock l;
            if (!g_new_game) return std::snprintf(msg, cap, "no New Game pending"), false;
            std::snprintf(g_test_forget, sizeof(g_test_forget), "%s%d", base, index);
            std::snprintf(msg, cap, "test: %s stands in for the New Game's first save (dropped from the baseline)", g_test_forget);
            return true;
        }
        case 8: {  // hidden tests: the waiting panel not drawn (1; 2: the input block lifted too; 3: and the game's own title menu
                   // back, title_wait off: the gate alone holds the title) / everything as it was (0)
            if (!automation::Enabled()) return std::snprintf(msg, cap, "hidden runs only"), false;
            if (g_role != 2) return std::snprintf(msg, cap, "guest only"), false;
            g_ts.hide_panel = v[1] != 0;
            g_ts.lift_block = v[1] >= 2;
            g_ts.title_test_off = v[1] >= 3;
            g_ts.next_view_ms = 0;
            if (title_wait::Installed() && !g_ts.title_test_off)   // the native title: there is no panel to hide or draw
                std::snprintf(msg, cap, "test: the native waiting title is on (no panel, no input block; the gate stays)");
            else
                std::snprintf(msg, cap, "test: the waiting panel is %s (%s)", g_ts.hide_panel ? "hidden" : "drawn",
                              g_ts.title_test_off ? "the input block lifted and the game's own title menu back: the gate alone holds the title"
                              : g_ts.lift_block   ? "the input block lifted too: the gate alone holds the title"
                                                  : "the input block and the gate stay");
            Log("[coop] %s", msg);
            return true;
        }
        default: break;
    }
    return std::snprintf(msg, cap, "coop_session: 0 log, 1 ms blackout, 2 offer the newest save (host), 3 teleport (guest), 4 slots (host), 5 t i load, "
                                   "6 quit, 7 [1|2] retry [query|forced], 8 0/1/2/3 panel, 9 t i New Game first save (5-9 and 1: hidden tests)"),
           false;
}

}  // namespace st::coop

namespace st::game {
bool CoopSessionCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept { return coop::Command(op, v, msg, cap); }
}  // namespace st::game
