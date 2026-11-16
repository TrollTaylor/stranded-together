#include "steam_owner.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "MinHook.h"
#include "log.hpp"
#include "stcoop/steam_net_abi.hpp"
#include "stcoop/steam_net_callbacks.hpp"

namespace st::steam {
namespace {

namespace sn = stcoop::steam_net;

using GetHSteamUserFn = std::int32_t (*)();
using FindOrCreateUserInterfaceFn = void* (*)(std::int32_t user, const char* version);
using GetSteamIdFn = std::uint64_t (*)(void* user);
using BLoggedOnFn = bool (*)(void* user);
using GetPersonaNameFn = const char* (*)(void* friends);
using SetRichPresenceFn = bool (*)(void* friends, const char* key, const char* value);
using RegisterCallbackFn = void (*)(void* callback, int id);

struct Exports {
    GetHSteamUserFn get_user = nullptr;
    FindOrCreateUserInterfaceFn find_or_create = nullptr;
    GetSteamIdFn get_steam_id = nullptr;
    BLoggedOnFn logged_on = nullptr;
    GetPersonaNameFn persona = nullptr;
    SetRichPresenceFn rich_presence = nullptr;
    RegisterCallbackFn register_callback = nullptr;
};

Exports g_x;
void* g_user = nullptr;
void* g_friends = nullptr;
stcoop::lab::SteamNetApi g_api;  // published once g_phase == ready
std::atomic<int> g_phase{static_cast<int>(Phase::waiting_for_steam)};
const char* g_reason = "";
std::uint64_t g_self = 0;
char g_persona[64] = {};
volatile long g_halt = 0;
unsigned g_attempts = 0;
bool g_want_requests = false;
bool g_logged_offline = false;
void (*g_shutdown_handler)() = nullptr;
using ShutdownFn = void (*)();
ShutdownFn g_real_shutdown = nullptr;
bool g_shutdown_hooked = false;

void HookedShutdown() {
    if (g_shutdown_handler) {
        Log("[steam] game is shutting Steam down: closing the link first");
        g_shutdown_handler();
    }
    g_real_shutdown();
}

void HookShutdown(HMODULE mod) {
    if (g_shutdown_hooked || !g_shutdown_handler) return;
    g_shutdown_hooked = true;
    void* target = reinterpret_cast<void*>(GetProcAddress(mod, "SteamAPI_Shutdown"));
    if (!target) return;
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookedShutdown), reinterpret_cast<void**>(&g_real_shutdown));
    if (st == MH_OK) st = MH_EnableHook(target);
    Log("[steam] SteamAPI_Shutdown hook: %s", MH_StatusToString(st));
}
ULONGLONG g_first_try_ms = 0;

// ds.exe uses these exact versions itself (strings in ds.exe).
constexpr char kUserVersion[] = "SteamUser021";
constexpr char kFriendsVersion[] = "SteamFriends017";

void OnFault(void*, const char* where) noexcept {
    InterlockedExchange(&g_halt, 1);
    Log("[steam] fault inside steamclient at %s: all further Steam calls disabled", where ? where : "?");
}

void Fail(const char* reason) {
    g_reason = reason;
    g_phase.store(static_cast<int>(Phase::failed), std::memory_order_release);
    Log("[steam] unavailable: %s", reason);
}

// SEH thunks (no C++ objects with destructors in these frames).
bool TryGetUser(std::int32_t& out) noexcept {
    __try {
        out = g_x.get_user();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool TryFind(std::int32_t user, const char* version, void*& out) noexcept {
    __try {
        out = g_x.find_or_create(user, version);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool TryIdentity(std::uint64_t& id, bool& logged_on, const char*& persona) noexcept {
    __try {
        logged_on = g_x.logged_on ? g_x.logged_on(g_user) : true;
        id = g_x.get_steam_id(g_user);
        persona = (g_x.persona && g_friends) ? g_x.persona(g_friends) : nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ResolveExports(HMODULE mod) {
    g_x.get_user = reinterpret_cast<GetHSteamUserFn>(GetProcAddress(mod, "SteamAPI_GetHSteamUser"));
    g_x.find_or_create =
        reinterpret_cast<FindOrCreateUserInterfaceFn>(GetProcAddress(mod, "SteamInternal_FindOrCreateUserInterface"));
    g_x.get_steam_id = reinterpret_cast<GetSteamIdFn>(GetProcAddress(mod, "SteamAPI_ISteamUser_GetSteamID"));
    g_x.logged_on = reinterpret_cast<BLoggedOnFn>(GetProcAddress(mod, "SteamAPI_ISteamUser_BLoggedOn"));
    g_x.persona = reinterpret_cast<GetPersonaNameFn>(GetProcAddress(mod, "SteamAPI_ISteamFriends_GetPersonaName"));
    g_x.rich_presence = reinterpret_cast<SetRichPresenceFn>(GetProcAddress(mod, "SteamAPI_ISteamFriends_SetRichPresence"));
    g_x.register_callback = reinterpret_cast<RegisterCallbackFn>(GetProcAddress(mod, "SteamAPI_RegisterCallback"));
    return g_x.get_user && g_x.find_or_create && g_x.get_steam_id;
}

bool TryRegister(void* object, int id) noexcept {
    __try {
        g_x.register_callback(object, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void WantSessionRequests(bool want) noexcept { g_want_requests = want; }
void SetShutdownHandler(void (*fn)()) noexcept { g_shutdown_handler = fn; }

void Pump() noexcept {
    if (g_phase.load(std::memory_order_acquire) != static_cast<int>(Phase::waiting_for_steam)) return;
    const ULONGLONG now = GetTickCount64();
    if (!g_first_try_ms) g_first_try_ms = now;
    // Throttle to ~4 tries per second; give up after 3 minutes.
    if ((g_attempts++ & 15) != 0) return;
    if (now - g_first_try_ms > 180000) return Fail("steam_api never became ready (is Steam running and logged in?)");

    HMODULE mod = GetModuleHandleW(L"steam_api64.dll");
    if (!mod) return;  // ds.exe delay-loads it on first use
    if (!g_x.get_user && !ResolveExports(mod)) return Fail("steam_api64.dll is missing required exports");
    HookShutdown(mod);

    std::int32_t user = 0;
    if (!TryGetUser(user)) return Fail("SteamAPI_GetHSteamUser faulted");
    if (user == 0) return;  // SteamAPI_Init has not run yet

    void* user_if = nullptr;
    void* friends_if = nullptr;
    void* messages = nullptr;
    void* sockets = nullptr;
    void* utils = nullptr;
    if (!TryFind(user, kUserVersion, user_if) || !TryFind(user, kFriendsVersion, friends_if) ||
        !TryFind(user, sn::kMessagesVersion, messages) || !TryFind(user, sn::kSocketsVersion, sockets) ||
        !TryFind(0, sn::kUtilsVersion, utils)) {
        return Fail("SteamInternal_FindOrCreateUserInterface faulted");
    }
    if (!user_if) return Fail("no ISteamUser");
    if (!messages) return Fail("steamclient did not provide SteamNetworkingMessages002 (update Steam)");
    g_user = user_if;
    g_friends = friends_if;

    bool logged_on = false;
    const char* persona = nullptr;
    std::uint64_t self = 0;
    if (!TryIdentity(self, logged_on, persona)) return Fail("ISteamUser identity calls faulted");
    if (!logged_on || !stcoop::steam::is_individual_steamid(self)) {
        if (!g_logged_offline) {
            g_logged_offline = true;
            Log("[steam] not logged on yet (id=%llu); waiting (is Steam offline?)", static_cast<unsigned long long>(self));
        }
        return;
    }
    g_self = self;
    if (persona) {
        std::strncpy(g_persona, persona, sizeof(g_persona) - 1);
    }

    g_api.messages = static_cast<sn::ISteamNetworkingMessages002Abi*>(messages);
    g_api.sockets = static_cast<sn::ISteamNetworkingSockets012Abi*>(sockets);
    g_api.utils = static_cast<sn::ISteamNetworkingUtils004Abi*>(utils);
    g_api.on_fault = &OnFault;
    g_api.halt = &g_halt;
    // Function-pointer session-request dispatch needs Sockets012 RunCallbacks and
    // Utils004 SetConfigValue; we did not run DDT's RTTI forwarder gates, so mark
    // it checked only when both interfaces exist (the transport still SEH-guards).
    g_api.dispatch_refused = (sockets && utils) ? nullptr : "interfaces_missing";
    if (g_want_requests && g_x.register_callback) {
        const bool ok = TryRegister(stcoop::steam_cb::SessionRequestObject(), sn::kCallbackSessionRequest);
        Log("[steam] SessionRequest (1251) callback %s", ok ? "registered" : "registration FAULTED");
    }
    g_phase.store(static_cast<int>(Phase::ready), std::memory_order_release);
    Log("[steam] ready: self=%llu persona='%s' messages=%p sockets=%p utils=%p", static_cast<unsigned long long>(g_self),
        g_persona, messages, sockets, utils);
}

Phase GetPhase() noexcept { return static_cast<Phase>(g_phase.load(std::memory_order_acquire)); }
const char* FailReason() noexcept { return g_reason; }
std::uint64_t SelfId() noexcept { return GetPhase() == Phase::ready ? g_self : 0; }
const char* PersonaName() noexcept { return GetPhase() == Phase::ready ? g_persona : ""; }

int Resolve(void*, stcoop::lab::SteamNetApi& out, const char*& reason) noexcept {
    switch (GetPhase()) {
        case Phase::ready:
            out = g_api;
            return 1;
        case Phase::failed:
            reason = g_reason;
            return -1;
        default:
            return 0;
    }
}

void SetRichPresence(const char* key, const char* value) noexcept {
    if (GetPhase() != Phase::ready || !g_friends || !g_x.rich_presence || g_halt) return;
    __try {
        g_x.rich_presence(g_friends, key, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_halt, 1);
    }
}

}  // namespace st::steam
