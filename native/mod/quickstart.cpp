// Hidden runs' quickstart. See quickstart.hpp and docs/hidden-testing.md "start".
#include "quickstart.hpp"

#include <windows.h>

#include <cstdarg>

#include "MinHook.h"
#include "automation.hpp"
#include "config.hpp"
#include "debug_actions.hpp"
#include "game_actions.hpp"
#include "game_api.hpp"
#include "link.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "title_wait.hpp"

namespace st::quickstart {
namespace {
namespace mem = partner::mem;

constexpr std::uintptr_t kImage = 0x140000000ull;  // ds.exe VAs at its preferred base
std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- game addresses (ds.exe DC; coop_session.cpp / story_state.cpp)
// Application.ContinueLastSave: cmp [session], 0; jne ret; mov r8b, 0xFF; lea rcx, [app]; mov dl, 0xF; jmp title load. Its full
// bytes include both RIP displacements and the jump's (to 0x142624A70), so they prove the globals and the callee.
constexpr std::uintptr_t kContinueLast = 0x142642A00;
constexpr const char* kContinueSig = "48 83 3D 10 39 5A 05 00 75 11 41 B0 FF 48 8D 0D 1C 39 5A 05 B2 0F E9 55 20 FE FF C3";
// The title load (app, u8 type, s8 index) -> u8 (0 refused). A host-held save host's coop hook sits on its first bytes (MinHook's
// jump): the entry is 40 56 (push rsi) or E9 (the hook), the rest from +6 unchanged.
constexpr std::uintptr_t kTitleLoad = 0x142624A70;
constexpr const char* kTitleLoadTail = "48 81 EC 90 00 00 00 48 8B 05";
constexpr std::uintptr_t kSessionGlobal = 0x147BE6318;  // the game session: 0 at the title
constexpr std::uintptr_t kAppGlobal = 0x147BE6330;      // the Application object

constexpr double kTitleSettleMs = 3000;     // the title page up this long before the call (the guest's proven rule)
constexpr double kRetryMs = 3000;           // a refused call (0) is tried again after this, kMaxCalls in all
constexpr int kMaxCalls = 3;
constexpr double kNoTitleMs = 120000;       // no title page this long after the start: give up (one line)
constexpr double kWorldTimeoutMs = 180000;  // no world this long after the call: say so (one line)
constexpr double kStillMs = 1500;           // "in the world": playable and still this long

enum class Phase : std::uint8_t { off, title, world, done };
Phase g_phase = Phase::off;
Spec g_spec;
bool g_guest_wait = false;   // a host-held save guest: no call, the host's save loads it
double g_start_ms = -1;      // the first tick (link::NowMsPrecise)
double g_call_ms = -1;       // the newest call
int g_calls = 0;
double g_still_since = -1;
std::uintptr_t g_still_entity = 0;
double g_still_pos[3] = {};
bool g_told_no_title = false, g_told_no_world = false;
char g_spec_text[48] = {};

double SinceProcessStart() noexcept {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return -1;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto t = [](const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return static_cast<double>(t(now) - t(c)) / 1e7;
}

int CallContinue() noexcept {  // the u8 the title load returns, -1 fault
    __try {
        return reinterpret_cast<std::uint8_t (*)()>(A(kContinueLast))();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int CallLoad(int type, int index) noexcept {
    __try {
        return reinterpret_cast<std::uint8_t (*)(std::uintptr_t, std::uint8_t, std::uint8_t)>(A(kTitleLoad))(
            A(kAppGlobal), static_cast<std::uint8_t>(type), static_cast<std::uint8_t>(index));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

const char* CheckSites() noexcept {
    if (!mem::CodeMatches(A(kContinueLast), kContinueSig)) return "ContinueLastSave";
    if (mem::RipTarget(A(kContinueLast), 3, 8) != A(kSessionGlobal)) return "ContinueLastSave (the session global)";
    if (mem::RipTarget(A(kContinueLast + 0xD), 3, 7) != A(kAppGlobal)) return "ContinueLastSave (the app global)";
    if (mem::RipTarget(A(kContinueLast + 0x16), 1, 5) != A(kTitleLoad)) return "ContinueLastSave (its jump to the title load)";
    std::uint8_t first = 0;
    if (!mem::Rd(A(kTitleLoad), first) || (first != 0x40 && first != 0xE9) || !mem::CodeMatches(A(kTitleLoad + 6), kTitleLoadTail))
        return "the title load";
    return nullptr;
}

// The local Sam playable (the placement gate open: a player, awake, nothing loading, no stopping event, no setup phase) and
// still for kStillMs. Game thread.
char g_why[120] = "no local player";
// The same test as the scenario's waitworld (automation.cpp DoWaitWorld): the tick's published snapshot (debug::Snapshot, its
// game diagnostics) says the local Sam is there and awake, nothing loads, no stopping event or setup phase, the teleport gate open.
bool InWorld(double now) noexcept {
    const debug::Snapshot s = debug::GetSnapshot();
    const game::Diagnostics& d = s.diag;
    const char* block = nullptr;
    if (!s.local_valid) block = "no local player";
    else if (!s.local_awake) block = "Sam is asleep (loading, menu or cutscene)";
    else if (!d.module_valid || d.load_mask != 0) block = "the game is loading";
    else if (!d.state_valid || (d.state_flags & game::kStoppingGameEventMask)) {
        static char b[96];
        std::snprintf(b, sizeof(b), "a menu, cutscene or scripted scene (state valid %d, flags 0x%llX)", d.state_valid ? 1 : 0,
                      static_cast<unsigned long long>(d.state_flags));
        block = b;
    }
    else if (d.state_phase == 2) block = "the post-load setup phase";
    else if (!d.teleport_ready) block = d.teleport_block[0] ? d.teleport_block : "the placement gate is closed";
    const double dx = s.local_pos[0] - g_still_pos[0], dy = s.local_pos[1] - g_still_pos[1], dz = s.local_pos[2] - g_still_pos[2];
    if (block || s.local_entity != g_still_entity || dx * dx + dy * dy + dz * dz > 0.25) {
        if (block) std::snprintf(g_why, sizeof(g_why), "%s", block);
        g_still_since = block ? -1 : now;
        g_still_entity = s.local_entity;
        g_still_pos[0] = s.local_pos[0], g_still_pos[1] = s.local_pos[1], g_still_pos[2] = s.local_pos[2];
        return false;
    }
    if (g_still_since < 0) g_still_since = now;  // playable again in the same place: the window starts now
    return now - g_still_since >= kStillMs;
}

}  // namespace

void Install(const Config& config) noexcept {
    const char* spec = automation::QuickstartRequested();
    if (!spec || !spec[0]) return;
    std::snprintf(g_spec_text, sizeof(g_spec_text), "%s", spec);
    wchar_t env[16] = {};
    const bool supervised = GetEnvironmentVariableW(automation::saves::kSupervisedEnv, env, 16) > 0;
    if (!automation::Enabled() || !supervised || !automation::SavesRedirected()) {
        Log("[quick] quickstart '%s' REFUSED: not a supervised hidden run with its saves redirected", spec);
        return;
    }
    if (!ParseSpec(spec, &g_spec) || g_spec.mode == Spec::none) {
        Log("[quick] quickstart '%s' not understood (continue | load <type> <index> | story <n>): off", spec);
        return;
    }
    g_guest_wait = config.role == Role::guest && config.host_save;
    if (g_guest_wait) {
        g_phase = Phase::world;
        Log("[quick] quickstart '%s': this game is the host-held save guest: no title call (the host's save loads it); "
            "'[quick] in the world' once it plays",
            spec);
        return;
    }
    if (const char* bad = CheckSites()) {
        Log("[quick] quickstart OFF: this game build differs (%s byte check failed): nothing is called; the scenario's waitworld fails", bad);
        return;
    }
    char why[160] = {};
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        Log("[quick] quickstart OFF: MinHook init failed (%s)", MH_StatusToString(init));
        return;
    }
    if (!title_wait::Installed() && !title_wait::InstallObserver(true, why, sizeof(why))) {
        Log("[quick] quickstart OFF: the title page watch could not go in (%s)", why);
        return;
    }
    g_phase = Phase::title;
    if (g_spec.mode == Spec::continue_last)
        Log("[quick] quickstart 'continue': byte checks ok; Application.ContinueLastSave (0x142642A00 -> the title load 0x142624A70, 0xF, -1) "
            "is called once the title page has been up %.0f s",
            kTitleSettleMs / 1000.0);
    else
        Log("[quick] quickstart '%s': byte checks ok; the title load 0x142624A70(app, %d, %d) is called once the title page has been up %.0f s", spec,
            g_spec.type, g_spec.index, kTitleSettleMs / 1000.0);
}

void Tick(double) noexcept {
    if (g_phase == Phase::off || g_phase == Phase::done) return;
    const double now = link::NowMsPrecise();
    if (g_start_ms < 0) g_start_ms = now;
    if (g_phase == Phase::title) {
        if (g_calls >= kMaxCalls) return;
        if (g_call_ms >= 0 && now - g_call_ms < kRetryMs) return;
        const double up = title_wait::TitlePageUpMs(now);
        if (up < kTitleSettleMs || mem::Q(A(kSessionGlobal)) != 0) {
            if (!g_told_no_title && g_calls == 0 && now - g_start_ms > kNoTitleMs) {
                g_told_no_title = true;
                Log("[quick] no title page for %.0f s after the mod's first tick: nothing called", kNoTitleMs / 1000.0);
            }
            return;
        }
        ++g_calls;
        g_call_ms = now;
        const int r = g_spec.mode == Spec::continue_last ? CallContinue() : CallLoad(g_spec.type, g_spec.index);
        if (g_spec.mode == Spec::continue_last)
            Log("[quick] title Continue called (#%d, the title page up %.1f s, %.1f s after ds.exe started) -> %d", g_calls, up / 1000.0, SinceProcessStart(), r);
        else
            Log("[quick] title load (%d, %d) called (#%d, the title page up %.1f s, %.1f s after ds.exe started) -> %d", g_spec.type, g_spec.index, g_calls,
                up / 1000.0, SinceProcessStart(), r);
        if (r < 0) {
            g_phase = Phase::done;
            Log("[quick] the title call FAULTED: quickstart off");
        } else if (r > 0) {
            g_phase = Phase::world;
        } else if (g_calls >= kMaxCalls) {
            Log("[quick] the game refused the title call %d times: quickstart gave up", g_calls);
        }
        return;
    }
    // Phase::world
    if (InWorld(now)) {
        g_phase = Phase::done;
        Log("[quick] in the world after %.1f s from the call (%.1f s after ds.exe started)%s", g_call_ms >= 0 ? (now - g_call_ms) / 1000.0 : 0.0,
            SinceProcessStart(), g_guest_wait ? " (the host-held save guest)" : "");
        return;
    }
    const double from = g_call_ms >= 0 ? g_call_ms : g_start_ms;
    static double next_note = 30000;
    if (now - from > next_note && next_note <= kWorldTimeoutMs) {
        Log("[quick] not in the world yet %.0f s after %s: %s", next_note / 1000.0, g_call_ms >= 0 ? "the call" : "the start", g_why);
        next_note += 30000;
    }
    if (!g_told_no_world && now - from > kWorldTimeoutMs) {
        g_told_no_world = true;
        Log("[quick] not in the world %.0f s after %s (%s): the quickstart stops watching", kWorldTimeoutMs / 1000.0, g_call_ms >= 0 ? "the call" : "the start", g_why);
        g_phase = Phase::done;
    }
}

}  // namespace st::quickstart
