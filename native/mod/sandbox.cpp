// Second-instance mutex rename (sandbox.hpp). Installed from DllMain by
// st::automation::EarlyInit, only with second_instance=1 and an active save redirect.
#include "sandbox.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>

#include "MinHook.h"

namespace st::sandbox {
namespace {

using CreateMutexA_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
using CreateMutexW_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
using CreateMutexExA_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCSTR, DWORD, DWORD);
using CreateMutexExW_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

CreateMutexA_t o_CreateMutexA;
CreateMutexW_t o_CreateMutexW;
CreateMutexExA_t o_CreateMutexExA;
CreateMutexExW_t o_CreateMutexExW;
std::atomic<unsigned long> g_renames{0};
std::atomic<bool> g_installed{false};

// Plain buffers (no allocation on the game's static-initializer path when the name is not ours).
HANDLE WINAPI H_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name) {
    if (name && std::strstr(name, kAppMutex)) {
        try {
            const std::string r = RenamedMutex(name);
            if (!r.empty()) {
                g_renames.fetch_add(1);
                return o_CreateMutexA(sa, owner, r.c_str());
            }
        } catch (...) {
        }
    }
    return o_CreateMutexA(sa, owner, name);
}

HANDLE WINAPI H_CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name) {
    if (name && wcsstr(name, L"DeathStrandingDCAppMutex")) {
        try {
            const std::wstring r = RenamedMutexW(name);
            if (!r.empty()) {
                g_renames.fetch_add(1);
                return o_CreateMutexW(sa, owner, r.c_str());
            }
        } catch (...) {
        }
    }
    return o_CreateMutexW(sa, owner, name);
}

HANDLE WINAPI H_CreateMutexExA(LPSECURITY_ATTRIBUTES sa, LPCSTR name, DWORD flags, DWORD access) {
    if (name && std::strstr(name, kAppMutex)) {
        try {
            const std::string r = RenamedMutex(name);
            if (!r.empty()) {
                g_renames.fetch_add(1);
                return o_CreateMutexExA(sa, r.c_str(), flags, access);
            }
        } catch (...) {
        }
    }
    return o_CreateMutexExA(sa, name, flags, access);
}

HANDLE WINAPI H_CreateMutexExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access) {
    if (name && wcsstr(name, L"DeathStrandingDCAppMutex")) {
        try {
            const std::wstring r = RenamedMutexW(name);
            if (!r.empty()) {
                g_renames.fetch_add(1);
                return o_CreateMutexExW(sa, r.c_str(), flags, access);
            }
        } catch (...) {
        }
    }
    return o_CreateMutexExW(sa, name, flags, access);
}

void* Resolve(const char* name) noexcept {
    for (const wchar_t* dll : {L"kernelbase.dll", L"kernel32.dll"}) {
        HMODULE m = GetModuleHandleW(dll);
        void* p = m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
        if (p) return p;
    }
    return nullptr;
}

}  // namespace

bool InstallMutexRename(char* why, std::size_t cap) noexcept {
    struct Spec {
        const char* name;
        void* detour;
        void** original;
        bool required;
    };
    const Spec specs[] = {
        {"CreateMutexA", reinterpret_cast<void*>(&H_CreateMutexA), reinterpret_cast<void**>(&o_CreateMutexA), true},  // ds.exe's import
        {"CreateMutexW", reinterpret_cast<void*>(&H_CreateMutexW), reinterpret_cast<void**>(&o_CreateMutexW), false},
        {"CreateMutexExA", reinterpret_cast<void*>(&H_CreateMutexExA), reinterpret_cast<void**>(&o_CreateMutexExA), false},
        {"CreateMutexExW", reinterpret_cast<void*>(&H_CreateMutexExW), reinterpret_cast<void**>(&o_CreateMutexExW), false},
    };
    for (const Spec& s : specs) {
        void* target = Resolve(s.name);
        MH_STATUS st = target ? MH_CreateHook(target, s.detour, s.original) : MH_ERROR_FUNCTION_NOT_FOUND;
        if (st == MH_OK) st = MH_EnableHook(target);
        if (st != MH_OK && s.required) {
            std::snprintf(why, cap, "second instance: hook %s failed (%s)", s.name, MH_StatusToString(st));
            return false;
        }
    }
    // Self-test through the hook: our CreateMutexA of the game's name must create the renamed
    // mutex (so the game's own call later never sees the first instance's mutex).
    const std::string renamed = RenamedMutex(kAppMutex);
    const unsigned long before = g_renames.load();
    HANDLE probe = CreateMutexA(nullptr, FALSE, kAppMutex);
    const DWORD err = GetLastError();
    bool ok = probe != nullptr && g_renames.load() == before + 1;
    if (probe) {
        HANDLE same = OpenMutexA(SYNCHRONIZE, FALSE, renamed.c_str());
        ok = ok && same != nullptr;
        if (same) CloseHandle(same);
        CloseHandle(probe);
    }
    g_renames.store(before);  // the probe does not count
    if (!ok) {
        std::snprintf(why, cap, "second instance: the mutex rename self-test failed (error %lu)", err);
        return false;
    }
    g_installed.store(true);
    return true;
}

unsigned long MutexRenames() noexcept { return g_renames.load(); }
bool MutexRenameInstalled() noexcept { return g_installed.load(); }

}  // namespace st::sandbox
