// Stranded Together — winhttp.dll proxy entry.
//
// ds.exe imports winhttp.dll statically; this DLL sits next to ds.exe, forwards
// every winhttp export to System32 (winhttp_forwards.inc) and bootstraps the
// mod on its own thread (never under the loader lock).
#include <windows.h>

#include <atomic>
#include <string>

#include "audio_probe.hpp"
#include "automation.hpp"
#include "config.hpp"
#include "coop_session.hpp"
#include "debug_actions.hpp"
#include "self_voice.hpp"
#include "partner_sound.hpp"
#include "partner_voice.hpp"

#include "game_api.hpp"
#include "link.hpp"
#include "log.hpp"
#include "MinHook.h"
#include "real_body.hpp"
#include "overlay/overlay.hpp"
#include "partner_collision.hpp"
#include "partner_gear.hpp"
#include "overlay/overlay_input.hpp"
#include "player_sync.hpp"
#include "sandbox.hpp"
#include "steam_owner.hpp"
#include "vehicle_sync.hpp"
#include "winhttp_forwards.inc"

#ifndef ST_BUILD_ID
#define ST_BUILD_ID "dev"
#endif

namespace {

using RunCallbacksWrapperFn = void (*)();
RunCallbacksWrapperFn g_original = nullptr;
std::atomic<bool> g_sync_enabled{false};
std::atomic<bool> g_faulted{false};

std::wstring ExeDir() {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring p(path, n);
    const auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

bool HostIsDeathStranding() {
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"ds.exe") == 0;
}

// SEH boundary around all mod work done on the game thread: a bug in the mod
// must cost the co-op session, not the player's game.
bool GuardedTick() noexcept {
    __try {
        st::sync::Tick();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// After a fault the tick never runs again: nothing it maintains may stay half done (the self
// view's hide reason on Sam, the bodies published to the animate detour).
// Each stop on its own: one that faults must not keep the others from running. The capsule first
// (a frozen capsule would be an invisible wall for the rest of the session).
bool GuardedStop(void (*stop)() noexcept) noexcept {
    __try {
        stop();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The host-held save session (its gate, the guest's waiting screen and input block live on only through the tick).
void CoopStopAfterFault() noexcept { st::coop::EmergencyStop("the mod's per-frame tick faulted (see the log); restart both games to play co-op"); }
bool GuardedEmergencyStop() noexcept {
    const bool coll = GuardedStop(&st::collision::EmergencyStop);
    const bool body = GuardedStop(&st::realbody::EmergencyStop);
    const bool gear = GuardedStop(&st::gear::EmergencyStop);
    const bool veh = GuardedStop(&st::vsync::EmergencyStop);
    const bool coop = GuardedStop(&CoopStopAfterFault);
    return coll && body && gear && veh && coop;
}

// frame_cap=<fps> (session.ini; the launcher's local two-instance test and hidden two-game
// runs): the game thread waits here once per presented frame, so two games on one PC do not
// each render as fast as the GPU allows (a hidden game's Present is never vsynced).
std::atomic<unsigned> g_frame_cap{0};

void FrameCap() noexcept {
    const unsigned cap = g_frame_cap.load(std::memory_order_relaxed);
    if (!cap) return;
    static unsigned long last_presents = ~0ul;
    static LONGLONG next = 0, freq = 0;
    static HANDLE timer = nullptr;
    static bool timer_tried = false;
    // once per frame: the frame pump may run more than once per presented frame
    const unsigned long presents = st::overlay::Installed() ? st::overlay::GetStats().presents : 0;
    if (presents != 0 && presents == last_presents) return;
    last_presents = presents;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    if (!timer_tried) {
        timer_tried = true;
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    const LONGLONG period = freq / cap;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (next == 0 || now.QuadPart - next > period * 4) next = now.QuadPart;  // start / after a long frame
    if (now.QuadPart < next) {
        const LONGLONG wait_100ns = (next - now.QuadPart) * 10000000 / freq;
        LARGE_INTEGER due;
        due.QuadPart = -wait_100ns;
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 1000);
        else Sleep(static_cast<DWORD>(wait_100ns / 10000));
    }
    next += period;
}

}  // namespace

void st::SetRuntimeFrameCap(unsigned fps) noexcept { g_frame_cap.store(fps == 0 ? 0u : fps < 15 ? 15u : fps > 240 ? 240u : fps); }
unsigned st::RuntimeFrameCap() noexcept { return g_frame_cap.load(std::memory_order_relaxed); }

namespace {

void HookedRunCallbacks() {
    g_original();
    FrameCap();
    if (!g_sync_enabled.load(std::memory_order_acquire)) return;
    if (!GuardedTick()) {
        g_sync_enabled.store(false);
        if (!g_faulted.exchange(true)) {
            st::Log("[mod] EXCEPTION in the per-frame sync: co-op disabled for this run (the game keeps running)");
            if (!GuardedEmergencyStop()) st::Log("[mod] the emergency stop faulted too");
        }
    }
}

std::wstring EnvW(const wchar_t* name) {
    wchar_t buf[512] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return n > 0 && n < ARRAYSIZE(buf) ? std::wstring(buf, n) : std::wstring();
}

DWORD WINAPI Bootstrap(LPVOID) {
    const std::wstring game_dir = ExeDir();
    // A second game on this PC (launcher local test, hidden two-instance runs) has its own mod
    // folder below stranded_together (ST_INSTANCE_DIR, sandbox.hpp): the one DllMain used too.
    bool instance = false;
    const std::wstring instance_env = EnvW(st::sandbox::kInstanceEnv);
    const std::wstring mod_dir = st::sandbox::ModDirFor(game_dir, instance_env, &instance);
    if (instance) CreateDirectoryW(mod_dir.c_str(), nullptr);
    st::LogInit(mod_dir);
    st::Log("[mod] Stranded Together %s loaded into ds.exe (pid %lu)", ST_BUILD_ID, GetCurrentProcessId());
    if (instance) st::Log("[mod] instance folder %ls (ST_INSTANCE_DIR)", instance_env.c_str());
    else if (!instance_env.empty()) st::Log("[mod] ST_INSTANCE_DIR '%ls' is not a valid folder name: ignored", instance_env.c_str());

    // Hidden test runs (tools/testenv): automation=1 is read before LoadConfig
    // consumes a one-shot session.ini.
    st::automation::Init(mod_dir);
    const st::Config cfg = st::LoadConfig(mod_dir);
    if (!cfg.present || cfg.role == st::Role::none) {
        st::Log("[mod] no co-op session requested: the game runs unmodified");
        return 0;
    }
    st::debug::SetRoleLabel(st::RoleName(cfg.role));
    // The host-held save session (host_save=1, docs/coop-session.md): its byte checks and hooks go in now, before the
    // overlay waits for the game's d3d12 below, so the guest's title menu is gated before it can ever be shown.
    st::coop::Install(cfg);

    // Local two-instance test: which input this game takes and that it keeps running unfocused
    // (overlay_input.hpp "local test"). The overlay installs these hooks, so it is always on then.
    const st::sandbox::LocalInput local = st::sandbox::ParseLocalInput(cfg.local_input);
    if (local != st::sandbox::LocalInput::off) {
        st::overlay::input::SetLocalMode(local == st::sandbox::LocalInput::guest      ? st::overlay::input::LocalMode::guest
                                         : local == st::sandbox::LocalInput::host_pad ? st::overlay::input::LocalMode::host_pad
                                                                                      : st::overlay::input::LocalMode::host);
        st::Log("[mod] local two-instance test: input %s", st::sandbox::LocalInputName(local));
    }

    if (!cfg.local_window.empty()) {
        st::overlay::input::SetWindowSlot(cfg.local_window == "left" ? st::overlay::input::WindowSlot::left : st::overlay::input::WindowSlot::right);
        st::Log("[mod] local test: the game window goes to the %s half of the monitor", cfg.local_window.c_str());
    }

    // In-game debug menu (Direct3D 12 overlay). Installed before anything that
    // can fail below, so the menu's Status tab can show why co-op is off. Name tags draw through
    // it too (docs/real-partner-body.md section 10): with debug_menu=0 it is installed for them
    // alone, without the menu (no toggle key, no window hooks).
    // The host-held save session's screens (the guest's waiting screen, the notices) need the overlay with its input
    // hooks: on for that session even with debug_menu=0.
    const bool coop_screens = st::coop::Active();
    const bool menu = cfg.debug_menu || local != st::sandbox::LocalInput::off || !cfg.local_window.empty() || coop_screens;
    if (coop_screens && !cfg.debug_menu) st::Log("[mod] host_save: the overlay is on for the co-op screens (debug_menu=0 asked for none)");
    const bool tags = cfg.name_tags && (cfg.role == st::Role::host || cfg.role == st::Role::guest || cfg.role == st::Role::echo);
    if (menu || tags) {
        st::overlay::Options ov;
        ov.menu = menu;
        ov.game_font = cfg.name_tag_game_font;
        ov.startup_hint = menu;
        ov.toggle_vk = cfg.debug_menu_vk;
        ov.mod_dir = mod_dir;
        ov.build = ST_BUILD_ID;
        ov.automation = st::automation::Enabled() && st::automation::IsolateRequested();
        // ds.exe exits(0) when d3d12.dll is already loaded before its own device
        // creation (check at 0x1419A6CE9), and the overlay loads d3d12.dll. So the
        // overlay always waits until the game has loaded it itself (every mode, not
        // only hidden runs: v0.2.0 quit on normal launches because of this).
        const bool overlay_ok = st::automation::WaitForGameD3D12(
            120000, local != st::sandbox::LocalInput::off || !cfg.local_window.empty() || st::automation::Enabled());
        if (!overlay_ok || !st::overlay::Install(ov)) st::Log("[mod] debug menu unavailable this run (see [overlay] above)");
        else if (!menu) st::Log("[mod] overlay installed for the name tags only (debug_menu=0)");
    } else {
        st::Log("[mod] debug menu off (debug_menu=0)");
    }
    st::automation::Start(mod_dir, ST_BUILD_ID);  // no-op unless automation=1
    if (st::sandbox::MutexRenameInstalled())
        st::Log("[mod] second instance: mutex=renamed (%lu call(s) of ds.exe's single-instance check went to %s%s)",
                st::sandbox::MutexRenames(), st::sandbox::kAppMutex, st::sandbox::kMutexSuffix);

    // From here on an early return means the per-frame tick never runs: the co-op save session (installed above) is
    // stopped too, or the guest would sit behind a waiting screen and a gate nothing ever lifts.
    if (!st::game::Resolve()) {
        st::Log("[mod] this ds.exe build is not supported (functions not found): co-op disabled");
        st::debug::SetModState("ds.exe build not supported: game functions not found");
        st::coop::EmergencyStop("this game build is not supported (game functions not found)");
        return 0;
    }

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        st::Log("[mod] MinHook init failed (%s)", MH_StatusToString(init));
        st::debug::SetModState("MinHook init failed");
        st::coop::EmergencyStop("MinHook init failed");
        return 0;
    }
    // Real partner body (docs/real-partner-body.md): the pose hook goes in at start-up, never
    // mid-game (enabling a hook suspends every thread while job threads may be inside it).
    if (cfg.partner_body == st::PartnerBody::real || cfg.pose_hook) st::realbody::InstallHook();
    // Audio probe (docs/audio-research.md section 9, Phase 0): logging only, at start-up like the pose hook.
    if (cfg.audio_probe) st::audioprobe::Install();
    // Self voice (docs/audio-research.md section 9, Phase 1): Sam's voice muted for a player who is not Sam. After the
    // probe (which then owns the talk-handler hook and asks self_voice first); nothing is hooked for self_voice=sam.
    // Partner voice (section 15, Phase 2; voice_sync=1): the same talk hook copies the local player's talks, and the
    // subtitle hook drops the partner's subtitles (voice_subtitles=0).
    st::selfvoice::Install(cfg.self_voice, cfg.role == st::Role::host, cfg.my_character, cfg.voice_sync);
    st::pvoice::Install(cfg);
    // Partner sounds (section 16, Phase 3; sound_sync=1): the Wwise event mirror, after the probe (which then owns the
    // Wwise and play-path hooks and forwards to it); nothing is hooked for sound_sync=0.
    st::psound::Install(cfg);
    // A muted talk runs and only its voice post is silenced when a PostEvent detour calls selfvoice::PrePost (partner_sound
    // installs one for it even with sound_sync=0); otherwise the talk is dropped (docs/audio-research.md section 17).
    st::selfvoice::PostHookReady(st::psound::PostHooked());

    void* target = reinterpret_cast<void*>(st::game::Get().SteamRunCallbacksWrapper);
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&HookedRunCallbacks), reinterpret_cast<void**>(&g_original));
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        st::Log("[mod] could not hook the frame pump (%s): co-op disabled", MH_StatusToString(s));
        st::debug::SetModState("frame pump hook failed: no tick, actions never run");
        st::coop::EmergencyStop("the frame pump could not be hooked (no per-frame tick)");
        return 0;
    }
    st::Log("[mod] frame pump hooked");
    if (cfg.frame_cap) {
        g_frame_cap.store(cfg.frame_cap);
        st::Log("[mod] frame_cap=%u: the game thread waits so this game shows at most %u frames per second", cfg.frame_cap, cfg.frame_cap);
    }

    const bool net = cfg.role == st::Role::host || cfg.role == st::Role::guest;
    if (net) {
        st::steam::WantSessionRequests(cfg.role == st::Role::host && cfg.transport == st::Transport::steam);
        st::steam::SetShutdownHandler([] {
            st::link::Shutdown();
            Sleep(100);  // the transport sends CLOSE, closes the session and parks
        });
        if (!st::link::Start(cfg)) st::Log("[mod] link did not start; see the reason above");
    }
    st::sync::Init(cfg, mod_dir);
    g_sync_enabled.store(true, std::memory_order_release);
    st::debug::SetModState("ready");
    st::Log("[mod] ready (%s). Hotkeys in game: F6 = log positions, F7 = respawn partner%s", st::RoleName(cfg.role),
            cfg.debug_menu && st::overlay::Installed() ? ", debug menu key (default F9)" : "");
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // Only ds.exe gets the mod; anything else that happens to load this
        // winhttp.dll (it forwards everything) just gets plain WinHTTP.
        if (HostIsDeathStranding()) {
            // Hidden test runs (session.ini automation=1): the user's saves are redirected
            // to a scratch copy before ds.exe's own code runs; when that cannot be done the
            // process ends here (st::automation::EarlyInit). No-op otherwise.
            st::automation::EarlyInit();
            HANDLE t = CreateThread(nullptr, 0, &Bootstrap, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
