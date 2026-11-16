// Stranded Together — winhttp.dll proxy entry.
//
// ds.exe imports winhttp.dll statically; this DLL sits next to ds.exe, forwards
// every winhttp export to System32 (winhttp_forwards.inc) and bootstraps the
// mod on its own thread (never under the loader lock).
#include <windows.h>

#include <atomic>
#include <string>

#include "automation.hpp"
#include "config.hpp"
#include "debug_actions.hpp"
#include "game_api.hpp"
#include "link.hpp"
#include "log.hpp"
#include "MinHook.h"
#include "overlay/overlay.hpp"
#include "player_sync.hpp"
#include "steam_owner.hpp"
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

void HookedRunCallbacks() {
    g_original();
    if (!g_sync_enabled.load(std::memory_order_acquire)) return;
    if (!GuardedTick()) {
        g_sync_enabled.store(false);
        if (!g_faulted.exchange(true))
            st::Log("[mod] EXCEPTION in the per-frame sync: co-op disabled for this run (the game keeps running)");
    }
}

DWORD WINAPI Bootstrap(LPVOID) {
    const std::wstring game_dir = ExeDir();
    const std::wstring mod_dir = game_dir + L"\\stranded_together";
    st::LogInit(mod_dir);
    st::Log("[mod] Stranded Together %s loaded into ds.exe (pid %lu)", ST_BUILD_ID, GetCurrentProcessId());

    // Hidden test runs (tools/testenv): automation=1 is read before LoadConfig
    // consumes a one-shot session.ini.
    st::automation::Init(mod_dir);
    const st::Config cfg = st::LoadConfig(mod_dir);
    if (!cfg.present || cfg.role == st::Role::none) {
        st::Log("[mod] no co-op session requested: the game runs unmodified");
        return 0;
    }
    st::debug::SetRoleLabel(st::RoleName(cfg.role));

    // In-game debug menu (Direct3D 12 overlay). Installed before anything that
    // can fail below, so the menu's Status tab can show why co-op is off.
    if (cfg.debug_menu) {
        st::overlay::Options ov;
        ov.toggle_vk = cfg.debug_menu_vk;
        ov.mod_dir = mod_dir;
        ov.build = ST_BUILD_ID;
        ov.automation = st::automation::Enabled() && st::automation::IsolateRequested();
        // ds.exe exits(0) when d3d12.dll is already loaded before its own device
        // creation (check at 0x1419A6CE9), and the overlay loads d3d12.dll. So the
        // overlay always waits until the game has loaded it itself (every mode, not
        // only hidden runs: v0.2.0 quit on normal launches because of this).
        const bool overlay_ok = st::automation::WaitForGameD3D12(120000);
        if (!overlay_ok || !st::overlay::Install(ov)) st::Log("[mod] debug menu unavailable this run (see [overlay] above)");
    } else {
        st::Log("[mod] debug menu off (debug_menu=0)");
    }
    st::automation::Start(mod_dir, ST_BUILD_ID);  // no-op unless automation=1

    if (!st::game::Resolve()) {
        st::Log("[mod] this ds.exe build is not supported (functions not found): co-op disabled");
        st::debug::SetModState("ds.exe build not supported: game functions not found");
        return 0;
    }

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        st::Log("[mod] MinHook init failed (%s)", MH_StatusToString(init));
        st::debug::SetModState("MinHook init failed");
        return 0;
    }
    void* target = reinterpret_cast<void*>(st::game::Get().SteamRunCallbacksWrapper);
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&HookedRunCallbacks), reinterpret_cast<void**>(&g_original));
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        st::Log("[mod] could not hook the frame pump (%s): co-op disabled", MH_StatusToString(s));
        st::debug::SetModState("frame pump hook failed: no tick, actions never run");
        return 0;
    }
    st::Log("[mod] frame pump hooked");

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
