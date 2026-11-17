// File-based automation channel for hidden test runs. See automation.hpp and
// docs/hidden-testing.md. Runs on its own thread; never touches game memory, except the
// foreign-saves bypass (automation.hpp "foreign saves": 3 bytes, DllMain, only with
// automation_foreign_saves=1 in a supervised, redirected run, byte-checked first).
#include "automation.hpp"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "MinHook.h"
#include "debug_actions.hpp"
#include "log.hpp"
#include "overlay/overlay.hpp"
#include "overlay/overlay_input.hpp"
#include "overlay/overlay_renderer.hpp"
#include "sandbox.hpp"

namespace st::automation {
namespace {

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_mute{true};
std::atomic<bool> g_isolate{true};
std::atomic<bool> g_started{false};
std::wstring g_in, g_out;
std::string g_build;
std::atomic<unsigned long> g_commands{0}, g_failures{0};

// ------------------------------------------------------------------ files
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool WriteAtomic(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void Append(const std::wstring& path, const std::string& line) {
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
    CloseHandle(f);
}

bool ReadText(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < (1 << 20);
    if (ok) {
        out->assign(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD got = 0;
        ok = size.QuadPart == 0 || (ReadFile(f, out->data(), static_cast<DWORD>(size.QuadPart), &got, nullptr) && got == size.QuadPart);
    }
    CloseHandle(f);
    return ok;
}

double NowMs() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return q;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// ------------------------------------------------------------------ save redirect
// Installed from DllMain (EarlyInit) before ds.exe's own code runs; see automation.hpp
// "save redirect". Globals here are plain arrays and atomics: no destructors, so the
// hooks stay valid while the process exits (other threads may still open files after
// this DLL's CRT has torn down). Hooks never fault: paths are read with wcsnlen/strnlen
// bounds, allocation failures fail the call (a save path is never passed through
// unmapped).
constexpr std::size_t kPathCap = 1024;
constexpr int kMaxRoots = 8;
wchar_t g_roots[kMaxRoots][kPathCap];
wchar_t g_root_scratch[kMaxRoots][kPathCap];  // where root i is mapped to
std::atomic<int> g_nroots{0};
int g_nsave_roots = 0;          // the first roots are the save tree (probe self-test); the rest single files
wchar_t g_scratch[kPathCap];    // the save tree's scratch copy
wchar_t g_mod_dir[kPathCap];    // this instance's mod folder (<game>\stranded_together[\<ST_INSTANCE_DIR>])
wchar_t g_base_mod_dir[kPathCap];  // <game>\stranded_together
wchar_t g_auto_dir[kPathCap];
wchar_t g_status_dir[kPathCap];    // where the active / refused files go (automation: g_auto_dir, sandbox: g_mod_dir)
// automation (hidden runs) or sandbox (launcher test saves): set once in DllMain.
std::atomic<int> g_mode{0};  // 0 none, 1 automation, 2 sandbox
const char* Tag() noexcept { return g_mode.load(std::memory_order_relaxed) == 2 ? "[sandbox]" : "[auto]"; }
std::atomic<bool> g_rd_active{false};
std::atomic<bool> g_log_ready{false};
std::atomic<unsigned long> g_rd_calls{0}, g_rd_failures{0};
std::atomic<int> g_rd_hooks{0};
char g_rd_missing[256];  // optional hooks that could not be installed

struct Seen {
    wchar_t text[260];
    char op[16];
    bool logged;
};
constexpr int kMaxSeen = 96, kMaxOutside = 48;
Seen g_seen[kMaxSeen];
int g_nseen = 0;
bool g_seen_full = false;
Seen g_outside[kMaxOutside];
int g_noutside = 0;
SRWLOCK g_seen_lock = SRWLOCK_INIT;

void Utf8(const wchar_t* w, char* out, int cap) noexcept {
    out[0] = 0;
    if (!w) return;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, nullptr, nullptr);
    if (n <= 0) out[0] = 0;
    out[cap - 1] = 0;
}

void WcsCopy(wchar_t* dst, std::size_t cap, const wchar_t* src) noexcept {
    std::size_t i = 0;
    for (; src && src[i] && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = 0;
}

// One log line per new entry; entries noted before the log was open are logged by
// FlushSeen() from Init.
void FlushSeen() noexcept {
    if (!g_log_ready.load(std::memory_order_acquire)) return;
    for (;;) {
        wchar_t text[260];
        char op[16];
        bool outside = false, full = false, found = false;
        AcquireSRWLockExclusive(&g_seen_lock);
        for (int i = 0; i < g_nseen && !found; ++i)
            if (!g_seen[i].logged) {
                g_seen[i].logged = found = true;
                WcsCopy(text, 260, g_seen[i].text);
                std::memcpy(op, g_seen[i].op, sizeof(op));
            }
        for (int i = 0; i < g_noutside && !found; ++i)
            if (!g_outside[i].logged) {
                g_outside[i].logged = found = outside = true;
                WcsCopy(text, 260, g_outside[i].text);
                std::memcpy(op, g_outside[i].op, sizeof(op));
            }
        if (!found && g_seen_full) {
            g_seen_full = false;
            full = true;
        }
        ReleaseSRWLockExclusive(&g_seen_lock);
        if (full) Log("%s saves: more than %d places, the rest are not logged", Tag(), kMaxSeen);
        if (!found) return;
        char u[800];
        Utf8(text, u, sizeof(u));
        if (outside) Log("%s write outside the save root (%s): %s", Tag(), op, u);
        else Log("%s saves: %s %s -> scratch", Tag(), op, u[0] ? u : "(save root)");
    }
}

void NoteIn(Seen* table, int* count, int cap, const std::wstring& text, const char* op, bool* full) noexcept {
    bool added = false;
    AcquireSRWLockExclusive(&g_seen_lock);
    bool have = false;
    for (int i = 0; i < *count && !have; ++i) have = _wcsicmp(table[i].text, text.c_str()) == 0;
    if (!have) {
        if (*count < cap) {
            Seen& s = table[(*count)++];
            WcsCopy(s.text, 260, text.c_str());
            std::snprintf(s.op, sizeof(s.op), "%s", op);
            s.logged = false;
            added = true;
        } else if (full) {
            *full = true;
        }
    }
    ReleaseSRWLockExclusive(&g_seen_lock);
    if (added || (full && *full)) FlushSeen();
}

bool UnderDir(const wchar_t* path, const wchar_t* dir) noexcept {
    const std::size_t n = wcsnlen(dir, kPathCap);
    return n && _wcsnicmp(path, dir, n) == 0 && (path[n] == L'\\' || path[n] == L'/' || path[n] == 0);
}

// Writes outside the save root (discovery: where else does the game write?). Absolute
// drive paths only; our own folder (logs, screenshots) is not interesting.
void NoteOutside(const wchar_t* path, const char* op) noexcept {
    if (!g_rd_active.load(std::memory_order_relaxed) || !path) return;
    try {
        const std::size_t n = wcsnlen(path, 32768);
        if (n < 4 || n >= 32768 || path[1] != L':' || UnderDir(path, g_base_mod_dir)) return;
        std::wstring dir = saves::Normalize(std::wstring(path, n));
        const auto slash = dir.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash > 2) dir.resize(slash);
        NoteIn(g_outside, &g_noutside, kMaxOutside, dir, op, nullptr);
    } catch (...) {
    }
}

using FindFirstFileExW_t = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
FindFirstFileExW_t o_FindFirstFileExW;  // the trampoline (set when the hooks are created)

// Win32's own spelling of a path (relative -> absolute against the current directory,
// "C:x" drive-relative, trailing dots/spaces, "..") without touching the disk: what
// the file API will open. "" when it has none.
std::wstring Win32FullPath(const std::wstring& p) {
    const DWORD need = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (need == 0 || need > 32768) return {};
    std::wstring buf(need, L'\0');
    const DWORD got = GetFullPathNameW(p.c_str(), need, buf.data(), nullptr);
    if (got == 0 || got >= need) return {};
    buf.resize(got);
    return buf;
}

// Long name of the last component of `path` (8.3 expansion), through the unhooked
// FindFirstFileExW (no recursion into the hooks).
bool LongNameOf(const std::wstring& path, std::wstring* name) {
    if (!o_FindFirstFileExW) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = o_FindFirstFileExW(path.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    fd.cFileName[MAX_PATH - 1] = 0;
    *name = fd.cFileName;
    return true;
}

// 1: under a save root (*out = the scratch path), 0: not ours, -1: could not decide
// (out of memory): the hook fails the call. A path counts as under the root when its
// own lexical form, Win32's full path of it, or that with 8.3 names expanded is.
int MapW(const wchar_t* path, std::wstring* out, const char* op) noexcept {
    if (!g_rd_active.load(std::memory_order_acquire) || !path) return 0;
    try {
        const std::size_t n = wcsnlen(path, 32768);
        if (n == 0 || n >= 32768) return 0;
        std::wstring cand[3];
        int nc = 0;
        cand[nc++].assign(path, n);
        bool lp = false;
        const std::wstring lexical = saves::Normalize(cand[0], &lp);
        if (!lp) {
            std::wstring full = Win32FullPath(cand[0]);
            if (!full.empty() && full != cand[0]) cand[nc++] = std::move(full);
        }
        if (lexical.find(L'~') != std::wstring::npos || cand[nc - 1].find(L'~') != std::wstring::npos) {
            std::wstring longer = saves::ExpandShortNames(saves::Normalize(cand[nc - 1]), &LongNameOf);
            if (longer != saves::Normalize(cand[nc - 1])) cand[nc++] = std::move(longer);
        }
        std::wstring rel;
        const int roots = g_nroots.load(std::memory_order_acquire);
        for (int c = 0; c < nc; ++c) {
            for (int i = 0; i < roots; ++i) {
                if (saves::Map(cand[c], g_roots[i], g_root_scratch[i], out, &rel)) {
                    g_rd_calls.fetch_add(1, std::memory_order_relaxed);
                    if (i < g_nsave_roots) {
                        NoteIn(g_seen, &g_nseen, kMaxSeen, saves::LogPrefix(rel), op, &g_seen_full);
                    } else {  // a single redirected file (appdir settings.cfg): named in the log
                        const wchar_t* name = wcsrchr(g_roots[i], L'\\');
                        NoteIn(g_seen, &g_nseen, kMaxSeen, std::wstring(L"<game folder>\\") + (name ? name + 1 : g_roots[i]), op, &g_seen_full);
                    }
                    return 1;
                }
            }
        }
        return 0;
    } catch (...) {
        g_rd_failures.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
}

// A path argument of a hooked call: the caller's own, or the scratch copy's.
struct PathW {
    const wchar_t* use = nullptr;
    std::wstring mapped;
    bool redirected = false, fail = false;
    PathW(const wchar_t* p, const char* op) noexcept : use(p) {
        const int r = MapW(p, &mapped, op);
        if (r > 0) use = mapped.c_str(), redirected = true;
        fail = r < 0;
    }
};

struct PathA {
    const char* use = nullptr;
    std::string mapped;
    bool redirected = false, fail = false;
    PathA(const char* p, const char* op) noexcept : use(p) {
        if (!g_rd_active.load(std::memory_order_acquire) || !p) return;
        try {
            const std::size_t n = strnlen(p, 32768);
            if (n < 3 || n >= 32768) return;
            const int wn = MultiByteToWideChar(CP_ACP, 0, p, static_cast<int>(n), nullptr, 0);
            if (wn <= 0) return;
            std::wstring w(static_cast<std::size_t>(wn), L'\0');
            MultiByteToWideChar(CP_ACP, 0, p, static_cast<int>(n), w.data(), wn);
            std::wstring m;
            const int r = MapW(w.c_str(), &m, op);
            if (r < 0) {
                fail = true;
                return;
            }
            if (r == 0) return;
            BOOL lossy = FALSE;
            const int an = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, m.c_str(), static_cast<int>(m.size()), nullptr, 0, nullptr, &lossy);
            if (an <= 0 || lossy) {  // the scratch path has no ANSI spelling: never fall back to the real one
                fail = true;
                return;
            }
            mapped.assign(static_cast<std::size_t>(an), '\0');
            WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, m.c_str(), static_cast<int>(m.size()), mapped.data(), an, nullptr, nullptr);
            use = mapped.c_str();
            redirected = true;
        } catch (...) {
            g_rd_failures.fetch_add(1, std::memory_order_relaxed);
            fail = true;
        }
    }
};

bool WriteIntent(DWORD access, DWORD disposition) noexcept {
    return (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | FILE_WRITE_ATTRIBUTES)) != 0 ||
           disposition == CREATE_ALWAYS || disposition == CREATE_NEW || disposition == TRUNCATE_EXISTING;
}

// Fails the call when a path could not be decided (never passes a save path through).
#define ST_RD_GUARD(path, ret)                    \
    if ((path).fail) {                            \
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);    \
        return ret;                               \
    }

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFile2_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
using FindFirstFileW_t = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using FindFirstFileA_t = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using FindFirstFileExA_t = HANDLE(WINAPI*)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using GetAttrW_t = DWORD(WINAPI*)(LPCWSTR);
using GetAttrA_t = DWORD(WINAPI*)(LPCSTR);
using GetAttrExW_t = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using GetAttrExA_t = BOOL(WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using SetAttrW_t = BOOL(WINAPI*)(LPCWSTR, DWORD);
using PathOpW_t = BOOL(WINAPI*)(LPCWSTR);                       // DeleteFileW, RemoveDirectoryW
using PathOpA_t = BOOL(WINAPI*)(LPCSTR);                        // DeleteFileA, RemoveDirectoryA
using CreateDirW_t = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using CreateDirA_t = BOOL(WINAPI*)(LPCSTR, LPSECURITY_ATTRIBUTES);
using CreateDirExW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPSECURITY_ATTRIBUTES);
using MoveW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
using MoveA_t = BOOL(WINAPI*)(LPCSTR, LPCSTR);
using MoveExW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using MoveExA_t = BOOL(WINAPI*)(LPCSTR, LPCSTR, DWORD);
using MoveProgW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, DWORD);
using CopyW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, BOOL);
using CopyA_t = BOOL(WINAPI*)(LPCSTR, LPCSTR, BOOL);
using CopyExW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD);
using ReplaceW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
using SetAttrA_t = BOOL(WINAPI*)(LPCSTR, DWORD);
using Copy2_t = HRESULT(WINAPI*)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS*);
using HardLinkW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPSECURITY_ATTRIBUTES);

CreateFileW_t o_CreateFileW;
CreateFileA_t o_CreateFileA;
CreateFile2_t o_CreateFile2;
FindFirstFileW_t o_FindFirstFileW;
FindFirstFileA_t o_FindFirstFileA;
FindFirstFileExA_t o_FindFirstFileExA;
GetAttrW_t o_GetFileAttributesW;
GetAttrA_t o_GetFileAttributesA;
GetAttrExW_t o_GetFileAttributesExW;
GetAttrExA_t o_GetFileAttributesExA;
SetAttrW_t o_SetFileAttributesW;
PathOpW_t o_DeleteFileW, o_RemoveDirectoryW;
PathOpA_t o_DeleteFileA, o_RemoveDirectoryA;
CreateDirW_t o_CreateDirectoryW;
CreateDirA_t o_CreateDirectoryA;
CreateDirExW_t o_CreateDirectoryExW;
MoveW_t o_MoveFileW;
MoveA_t o_MoveFileA;
MoveExW_t o_MoveFileExW;
MoveExA_t o_MoveFileExA;
MoveProgW_t o_MoveFileWithProgressW;
CopyW_t o_CopyFileW;
CopyA_t o_CopyFileA;
CopyExW_t o_CopyFileExW;
ReplaceW_t o_ReplaceFileW;
SetAttrA_t o_SetFileAttributesA;
Copy2_t o_CopyFile2;
HardLinkW_t o_CreateHardLinkW;

HANDLE WINAPI H_CreateFileW(LPCWSTR f, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD fl, HANDLE t) {
    const DWORD e = GetLastError();
    const bool w = WriteIntent(a, d);
    PathW p(f, w ? "write" : "read");
    ST_RD_GUARD(p, INVALID_HANDLE_VALUE)
    if (w && !p.redirected) NoteOutside(f, "write");
    SetLastError(e);
    return o_CreateFileW(p.use, a, s, sa, d, fl, t);
}
HANDLE WINAPI H_CreateFileA(LPCSTR f, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD fl, HANDLE t) {
    const DWORD e = GetLastError();
    PathA p(f, WriteIntent(a, d) ? "write" : "read");
    ST_RD_GUARD(p, INVALID_HANDLE_VALUE)
    SetLastError(e);
    return o_CreateFileA(p.use, a, s, sa, d, fl, t);
}
HANDLE WINAPI H_CreateFile2(LPCWSTR f, DWORD a, DWORD s, DWORD d, LPCREATEFILE2_EXTENDED_PARAMETERS x) {
    const DWORD e = GetLastError();
    const bool w = WriteIntent(a, d);
    PathW p(f, w ? "write" : "read");
    ST_RD_GUARD(p, INVALID_HANDLE_VALUE)
    if (w && !p.redirected) NoteOutside(f, "write");
    SetLastError(e);
    return o_CreateFile2(p.use, a, s, d, x);
}
HANDLE WINAPI H_FindFirstFileW(LPCWSTR f, LPWIN32_FIND_DATAW fd) {
    const DWORD e = GetLastError();
    PathW p(f, "list");
    ST_RD_GUARD(p, INVALID_HANDLE_VALUE)
    SetLastError(e);
    return o_FindFirstFileW(p.use, fd);
}
HANDLE WINAPI H_FindFirstFileA(LPCSTR f, LPWIN32_FIND_DATAA fd) {
    const DWORD e = GetLastError();
    PathA p(f, "list");
    ST_RD_GUARD(p, INVALID_HANDLE_VALUE)
    SetLastError(e);
    return o_FindFirstFileA(p.use, fd);
}
HANDLE WINAPI H_FindFirstFileExW(LPCWSTR f, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID sf, DWORD fl) {
    const DWORD e = GetLastError();
    PathW p(f, "list");
    ST_RD_GUARD(p, INVALID_HANDLE_VALUE)
    SetLastError(e);
    return o_FindFirstFileExW(p.use, l, d, o, sf, fl);
}
HANDLE WINAPI H_FindFirstFileExA(LPCSTR f, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID sf, DWORD fl) {
    const DWORD e = GetLastError();
    PathA p(f, "list");
    ST_RD_GUARD(p, INVALID_HANDLE_VALUE)
    SetLastError(e);
    return o_FindFirstFileExA(p.use, l, d, o, sf, fl);
}
DWORD WINAPI H_GetFileAttributesW(LPCWSTR f) {
    const DWORD e = GetLastError();
    PathW p(f, "stat");
    ST_RD_GUARD(p, INVALID_FILE_ATTRIBUTES)
    SetLastError(e);
    return o_GetFileAttributesW(p.use);
}
DWORD WINAPI H_GetFileAttributesA(LPCSTR f) {
    const DWORD e = GetLastError();
    PathA p(f, "stat");
    ST_RD_GUARD(p, INVALID_FILE_ATTRIBUTES)
    SetLastError(e);
    return o_GetFileAttributesA(p.use);
}
BOOL WINAPI H_GetFileAttributesExW(LPCWSTR f, GET_FILEEX_INFO_LEVELS l, LPVOID d) {
    const DWORD e = GetLastError();
    PathW p(f, "stat");
    ST_RD_GUARD(p, FALSE)
    SetLastError(e);
    return o_GetFileAttributesExW(p.use, l, d);
}
BOOL WINAPI H_GetFileAttributesExA(LPCSTR f, GET_FILEEX_INFO_LEVELS l, LPVOID d) {
    const DWORD e = GetLastError();
    PathA p(f, "stat");
    ST_RD_GUARD(p, FALSE)
    SetLastError(e);
    return o_GetFileAttributesExA(p.use, l, d);
}
BOOL WINAPI H_SetFileAttributesW(LPCWSTR f, DWORD a) {
    const DWORD e = GetLastError();
    PathW p(f, "attrib");
    ST_RD_GUARD(p, FALSE)
    if (!p.redirected) NoteOutside(f, "attrib");
    SetLastError(e);
    return o_SetFileAttributesW(p.use, a);
}
BOOL WINAPI H_DeleteFileW(LPCWSTR f) {
    const DWORD e = GetLastError();
    PathW p(f, "delete");
    ST_RD_GUARD(p, FALSE)
    if (!p.redirected) NoteOutside(f, "delete");
    SetLastError(e);
    return o_DeleteFileW(p.use);
}
BOOL WINAPI H_DeleteFileA(LPCSTR f) {
    const DWORD e = GetLastError();
    PathA p(f, "delete");
    ST_RD_GUARD(p, FALSE)
    SetLastError(e);
    return o_DeleteFileA(p.use);
}
BOOL WINAPI H_RemoveDirectoryW(LPCWSTR f) {
    const DWORD e = GetLastError();
    PathW p(f, "rmdir");
    ST_RD_GUARD(p, FALSE)
    if (!p.redirected) NoteOutside(f, "rmdir");
    SetLastError(e);
    return o_RemoveDirectoryW(p.use);
}
BOOL WINAPI H_RemoveDirectoryA(LPCSTR f) {
    const DWORD e = GetLastError();
    PathA p(f, "rmdir");
    ST_RD_GUARD(p, FALSE)
    SetLastError(e);
    return o_RemoveDirectoryA(p.use);
}
BOOL WINAPI H_CreateDirectoryW(LPCWSTR f, LPSECURITY_ATTRIBUTES sa) {
    const DWORD e = GetLastError();
    PathW p(f, "mkdir");
    ST_RD_GUARD(p, FALSE)
    if (!p.redirected) NoteOutside(f, "mkdir");
    SetLastError(e);
    return o_CreateDirectoryW(p.use, sa);
}
BOOL WINAPI H_CreateDirectoryA(LPCSTR f, LPSECURITY_ATTRIBUTES sa) {
    const DWORD e = GetLastError();
    PathA p(f, "mkdir");
    ST_RD_GUARD(p, FALSE)
    SetLastError(e);
    return o_CreateDirectoryA(p.use, sa);
}
BOOL WINAPI H_CreateDirectoryExW(LPCWSTR tmpl, LPCWSTR f, LPSECURITY_ATTRIBUTES sa) {
    const DWORD e = GetLastError();
    PathW t(tmpl, "stat"), p(f, "mkdir");
    ST_RD_GUARD(t, FALSE)
    ST_RD_GUARD(p, FALSE)
    SetLastError(e);
    return o_CreateDirectoryExW(t.use, p.use, sa);
}
BOOL WINAPI H_MoveFileW(LPCWSTR a, LPCWSTR b) {
    const DWORD e = GetLastError();
    PathW pa(a, "move-from"), pb(b, "move-to");
    ST_RD_GUARD(pa, FALSE)
    ST_RD_GUARD(pb, FALSE)
    if (!pb.redirected) NoteOutside(b, "move-to");
    SetLastError(e);
    return o_MoveFileW(pa.use, pb.use);
}
BOOL WINAPI H_MoveFileA(LPCSTR a, LPCSTR b) {
    const DWORD e = GetLastError();
    PathA pa(a, "move-from"), pb(b, "move-to");
    ST_RD_GUARD(pa, FALSE)
    ST_RD_GUARD(pb, FALSE)
    SetLastError(e);
    return o_MoveFileA(pa.use, pb.use);
}
BOOL WINAPI H_MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD fl) {
    const DWORD e = GetLastError();
    PathW pa(a, "move-from"), pb(b, "move-to");
    ST_RD_GUARD(pa, FALSE)
    ST_RD_GUARD(pb, FALSE)
    if (b && !pb.redirected) NoteOutside(b, "move-to");
    SetLastError(e);
    return o_MoveFileExW(pa.use, pb.use, fl);
}
BOOL WINAPI H_MoveFileExA(LPCSTR a, LPCSTR b, DWORD fl) {
    const DWORD e = GetLastError();
    PathA pa(a, "move-from"), pb(b, "move-to");
    ST_RD_GUARD(pa, FALSE)
    ST_RD_GUARD(pb, FALSE)
    SetLastError(e);
    return o_MoveFileExA(pa.use, pb.use, fl);
}
BOOL WINAPI H_MoveFileWithProgressW(LPCWSTR a, LPCWSTR b, LPPROGRESS_ROUTINE r, LPVOID d, DWORD fl) {
    const DWORD e = GetLastError();
    PathW pa(a, "move-from"), pb(b, "move-to");
    ST_RD_GUARD(pa, FALSE)
    ST_RD_GUARD(pb, FALSE)
    SetLastError(e);
    return o_MoveFileWithProgressW(pa.use, pb.use, r, d, fl);
}
BOOL WINAPI H_CopyFileW(LPCWSTR a, LPCWSTR b, BOOL fail) {
    const DWORD e = GetLastError();
    PathW pa(a, "copy-from"), pb(b, "copy-to");
    ST_RD_GUARD(pa, FALSE)
    ST_RD_GUARD(pb, FALSE)
    if (!pb.redirected) NoteOutside(b, "copy-to");
    SetLastError(e);
    return o_CopyFileW(pa.use, pb.use, fail);
}
BOOL WINAPI H_CopyFileA(LPCSTR a, LPCSTR b, BOOL fail) {
    const DWORD e = GetLastError();
    PathA pa(a, "copy-from"), pb(b, "copy-to");
    ST_RD_GUARD(pa, FALSE)
    ST_RD_GUARD(pb, FALSE)
    SetLastError(e);
    return o_CopyFileA(pa.use, pb.use, fail);
}
BOOL WINAPI H_CopyFileExW(LPCWSTR a, LPCWSTR b, LPPROGRESS_ROUTINE r, LPVOID d, LPBOOL c, DWORD fl) {
    const DWORD e = GetLastError();
    PathW pa(a, "copy-from"), pb(b, "copy-to");
    ST_RD_GUARD(pa, FALSE)
    ST_RD_GUARD(pb, FALSE)
    SetLastError(e);
    return o_CopyFileExW(pa.use, pb.use, r, d, c, fl);
}
BOOL WINAPI H_ReplaceFileW(LPCWSTR a, LPCWSTR b, LPCWSTR bak, DWORD fl, LPVOID x, LPVOID y) {
    const DWORD e = GetLastError();
    PathW pa(a, "replace"), pb(b, "replace-from"), pc(bak, "replace-backup");
    ST_RD_GUARD(pa, FALSE)
    ST_RD_GUARD(pb, FALSE)
    ST_RD_GUARD(pc, FALSE)
    if (!pa.redirected) NoteOutside(a, "replace");
    SetLastError(e);
    return o_ReplaceFileW(pa.use, pb.use, pc.use, fl, x, y);
}
// Not imported by ds.exe; hooked (optional) so no other route writes into the save root.
BOOL WINAPI H_SetFileAttributesA(LPCSTR f, DWORD a) {
    const DWORD e = GetLastError();
    PathA p(f, "attrib");
    ST_RD_GUARD(p, FALSE)
    SetLastError(e);
    return o_SetFileAttributesA(p.use, a);
}
HRESULT WINAPI H_CopyFile2(PCWSTR a, PCWSTR b, COPYFILE2_EXTENDED_PARAMETERS* x) {
    const DWORD e = GetLastError();
    PathW pa(a, "copy-from"), pb(b, "copy-to");
    if (pa.fail || pb.fail) return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
    SetLastError(e);
    return o_CopyFile2(pa.use, pb.use, x);
}
BOOL WINAPI H_CreateHardLinkW(LPCWSTR link, LPCWSTR existing, LPSECURITY_ATTRIBUTES sa) {
    const DWORD e = GetLastError();
    PathW pl(link, "link"), pe(existing, "link-to");
    ST_RD_GUARD(pl, FALSE)
    ST_RD_GUARD(pe, FALSE)
    SetLastError(e);
    return o_CreateHardLinkW(pl.use, pe.use, sa);
}
#undef ST_RD_GUARD

// ---- hidden runs never open anything on the user's desktop: ds.exe imports
// ShellExecuteA (FUN_141a1a820, a URL/launch helper); whatever it would start (a browser
// tab, another program) would not be in the run's job nor on its private desktop.
using ShellExecuteA_t = HINSTANCE(WINAPI*)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
using ShellExecuteW_t = HINSTANCE(WINAPI*)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
using ShellExecuteExA_t = BOOL(WINAPI*)(void*);
using ShellExecuteExW_t = BOOL(WINAPI*)(void*);
ShellExecuteA_t o_ShellExecuteA;
ShellExecuteW_t o_ShellExecuteW;
ShellExecuteExA_t o_ShellExecuteExA;
ShellExecuteExW_t o_ShellExecuteExW;
std::atomic<unsigned long> g_shell_blocked{0};
std::atomic<int> g_shell_hooks{0};

void NoteShellBlocked(const char* api, const char* file) noexcept {
    g_shell_blocked.fetch_add(1, std::memory_order_relaxed);
    char f[200] = "?";
    if (file) std::snprintf(f, sizeof(f), "%.190s", file);
    if (g_log_ready.load(std::memory_order_acquire)) Log("[auto] blocked %s(%s): hidden runs start nothing outside the job", api, f);
}
HINSTANCE WINAPI H_ShellExecuteA(HWND, LPCSTR, LPCSTR file, LPCSTR, LPCSTR, INT) {
    NoteShellBlocked("ShellExecuteA", file ? file : "");
    SetLastError(ERROR_ACCESS_DENIED);
    return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(SE_ERR_ACCESSDENIED));
}
HINSTANCE WINAPI H_ShellExecuteW(HWND, LPCWSTR, LPCWSTR file, LPCWSTR, LPCWSTR, INT) {
    char f[200] = "";
    if (file) Utf8(file, f, sizeof(f));
    NoteShellBlocked("ShellExecuteW", f);
    SetLastError(ERROR_ACCESS_DENIED);
    return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(SE_ERR_ACCESSDENIED));
}
BOOL WINAPI H_ShellExecuteExA(void*) {
    NoteShellBlocked("ShellExecuteExA", "");
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}
BOOL WINAPI H_ShellExecuteExW(void*) {
    NoteShellBlocked("ShellExecuteExW", "");
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

// ---- exit tracer: who ends the process (bisecting a game that quits at start-up)
using RtlExitUserProcess_t = void(NTAPI*)(LONG);
using TerminateProcess_t = BOOL(WINAPI*)(HANDLE, UINT);
RtlExitUserProcess_t o_RtlExitUserProcess;
TerminateProcess_t o_TerminateProcess;
std::atomic<bool> g_exit_traced{false};

void AppendRaw(const wchar_t* path, const char* text) noexcept {
    HANDLE f = (o_CreateFileW ? o_CreateFileW : &CreateFileW)(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, text, static_cast<DWORD>(std::strlen(text)), &w, nullptr);
    CloseHandle(f);
}

void TraceExit(const char* how, unsigned long code) noexcept {
    if (g_exit_traced.exchange(true)) return;
    void* frames[28] = {};
    const USHORT n = CaptureStackBackTrace(2, 28, frames, nullptr);
    char text[4096];
    int len = std::snprintf(text, sizeof(text), "%s(%lu = 0x%08lX) on thread %lu, stack:", how, code, code, GetCurrentThreadId());
    for (USHORT i = 0; i < n && len > 0 && len < static_cast<int>(sizeof(text)) - 96; ++i) {
        HMODULE m = nullptr;
        wchar_t name[MAX_PATH] = {};
        char mod[MAX_PATH] = "?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(frames[i]), &m) &&
            GetModuleFileNameW(m, name, MAX_PATH)) {
            const wchar_t* base = wcsrchr(name, L'\\');
            Utf8(base ? base + 1 : name, mod, sizeof(mod));
        }
        const unsigned long long off =
            reinterpret_cast<unsigned long long>(frames[i]) - reinterpret_cast<unsigned long long>(m);
        if (m && _stricmp(mod, "ds.exe") == 0)  // as Ghidra shows it (image base 0x140000000)
            len += std::snprintf(text + len, sizeof(text) - len, " ds.exe+0x%llX(0x%llX)", off, 0x140000000ull + off);
        else
            len += std::snprintf(text + len, sizeof(text) - len, " %s+0x%llX", mod, m ? off : reinterpret_cast<unsigned long long>(frames[i]));
    }
    Log("[auto] exit: %s", text);
    wchar_t path[kPathCap + 32];
    std::swprintf(path, kPathCap + 32, L"%s\\exit-trace.txt", g_auto_dir);
    char line[4200];
    std::snprintf(line, sizeof(line), "%s\n", text);
    AppendRaw(path, line);
}

void NTAPI H_RtlExitUserProcess(LONG code) {
    TraceExit("ExitProcess", static_cast<unsigned long>(code));
    o_RtlExitUserProcess(code);
}
BOOL WINAPI H_TerminateProcess(HANDLE h, UINT code) {
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) TraceExit("TerminateProcess", code);
    return o_TerminateProcess(h, code);
}

struct HookSpec {
    const wchar_t* dll;  // kernelbase first: kernel32's exports of these are stubs into it
    const char* name;
    void* detour;
    void** original;
    bool required;       // ds.exe imports it
};

// ---- session.ini, read in DllMain (Win32 only)
bool ReadIniRaw(const wchar_t* path, std::string* out) noexcept {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[16384];
    DWORD got = 0;
    const BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    try {
        out->assign(buf, got);
    } catch (...) {
        return false;
    }
    return true;
}

void RefuseAndExit(const char* why) noexcept {
    // ds.exe must not run without the redirect: it would read and write the user's saves.
    const bool sandbox = g_mode.load() == 2;
    const unsigned long code = sandbox ? sandbox::kRefusedExitCode : saves::kRefusedExitCode;
    const wchar_t* dir = sandbox ? g_mod_dir : g_auto_dir;
    if (dir[0]) {
        wchar_t path[kPathCap + 64];
        std::swprintf(path, kPathCap + 64, L"%s\\%s", dir, sandbox ? sandbox::kRefusedFile : saves::kRefusedFile);
        char text[700];
        std::snprintf(text, sizeof(text), "%s=REFUSED\nreason=%s\nexit_code=0x%08lX\n", sandbox ? "sandbox" : "saves", why, code);
        CreateDirectoryW(dir, nullptr);
        if (sandbox) DeleteFileW(path);  // one reason per launch
        AppendRaw(path, text);
    }
    OutputDebugStringA(sandbox ? "[stranded_together] test-save game refused: the save redirect could not be installed\n"
                               : "[stranded_together] hidden run refused: the save redirect could not be installed\n");
    TerminateProcess(GetCurrentProcess(), code);
}

// ---- foreign saves (automation.hpp "foreign saves"): DllMain, after the redirect is proven
std::atomic<bool> g_foreign_requested{false}, g_foreign_bypassed{false};
char g_foreign_note[640];  // the "[auto] foreign saves: ..." text Init logs

// ds.exe's own image: [base, base + SizeOfImage) and the section of an RVA must be committed,
// executable and readable (VirtualQuery, never a read that could fault inside ds.exe).
bool ReadImage(const unsigned char* base, std::size_t image_size, std::uint32_t rva, std::uint8_t* out, std::uint32_t len) noexcept {
    if (!base || static_cast<std::size_t>(rva) + len > image_size) return false;
    const unsigned char* p = base + rva;
    for (const unsigned char* at = p; at < p + len;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(at, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        at = static_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    std::memcpy(out, p, len);
    return true;
}

void InstallForeignSaves() noexcept {
    namespace fs = foreign_saves;
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    std::size_t image_size = 0;
    if (base) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE &&
            nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            image_size = nt->OptionalHeader.SizeOfImage;
    }
    std::string why;
    bool ok = false;
    try {
        ok = image_size != 0 && fs::CheckSites([&](std::uint32_t rva, std::uint8_t* out, std::uint32_t len) {
                 return ReadImage(base, image_size, rva, out, len);
             }, &why);
        if (!image_size) why = "ds.exe image headers not readable";
    } catch (...) {
        ok = false, why = "exception while checking the sites";
    }
    if (!ok) {
        std::snprintf(g_foreign_note, sizeof(g_foreign_note), "NOT bypassed (nothing patched): %s", why.c_str());
        return;
    }
    // All windows match this ds.exe build: one byte each, jne rel8 -> jmp rel8.
    int done = 0;
    int len = std::snprintf(g_foreign_note, sizeof(g_foreign_note), "check bypassed at");
    for (const fs::Site& s : fs::kSites) {
        auto* at = const_cast<unsigned char*>(base) + s.rva + s.patch_at;
        DWORD old = 0;
        if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) break;
        *at = fs::kJmp8;
        DWORD ignored = 0;
        VirtualProtect(at, 1, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), at, 1);
        if (*at != fs::kJmp8) break;
        ++done;
        if (len > 0 && len < static_cast<int>(sizeof(g_foreign_note)) - 96)
            len += std::snprintf(g_foreign_note + len, sizeof(g_foreign_note) - len, "%s 0x%llX (%s)", done > 1 ? "," : "",
                                 static_cast<unsigned long long>(fs::PatchVa(s)), s.name);
    }
    if (done != fs::kSiteCount) {
        std::snprintf(g_foreign_note, sizeof(g_foreign_note), "PARTLY bypassed: %d of %d sites written (VirtualProtect failed, error %lu)", done,
                      fs::kSiteCount, GetLastError());
    }
    g_foreign_bypassed.store(done == fs::kSiteCount);
}

// Init (log open): the evidence line the supervisor waits for, then the places
// noted so far.
void LogSaves() noexcept {
    if (!g_rd_active.load()) {
        Log("%s saves=NOT redirected: the redirect is not active in this process (it is installed from DllMain of ds.exe)", Tag());
        return;
    }
    char scratch[kPathCap * 3];
    Utf8(g_scratch, scratch, sizeof(scratch));
    const int roots = g_nroots.load();
    for (int i = 0; i < roots; ++i) {
        char root[kPathCap * 3], to[kPathCap * 3];
        Utf8(g_roots[i], root, sizeof(root));
        Utf8(g_root_scratch[i], to, sizeof(to));
        Log("%s saves: root %d %s -> %s", Tag(), i, root, to);
    }
    Log("%s saves=redirected: %d save root(s) -> %s (%d file API hooks, self-test passed%s%s)", Tag(), g_nsave_roots, scratch,
        g_rd_hooks.load(), g_rd_missing[0] ? "; optional hooks missing: " : "", g_rd_missing);
    if (g_nroots.load() > g_nsave_roots) Log("%s appdir settings.cfg redirected too (%d file name(s))", Tag(), g_nroots.load() - g_nsave_roots);
    if (g_mode.load() == 1) Log("[auto] ShellExecute(Ex)A/W blocked for this run (%d hooks)", g_shell_hooks.load());
    if (g_foreign_requested.load())
        Log("%s foreign saves: %s", Tag(), g_foreign_note[0] ? g_foreign_note : "NOT bypassed (no result recorded)");
    if (sandbox::MutexRenameInstalled())
        Log("%s second instance: CreateMutexA/W(Ex) hooked, %s is renamed to %s%s in this process (self-test passed)", Tag(),
            sandbox::kAppMutex, sandbox::kAppMutex, sandbox::kMutexSuffix);
    g_log_ready.store(true, std::memory_order_release);
    FlushSeen();
}

// ------------------------------------------------------------------ audio
// Hidden runs are silent: every buffer the game hands to WASAPI (Wwise and
// XAudio2 both end in IAudioRenderClient::ReleaseBuffer) is flagged SILENT.
// Process-local; nothing is persisted (no volume-mixer change).
using ReleaseBufferFn = HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, DWORD);
ReleaseBufferFn g_orig_release = nullptr;
std::atomic<unsigned long> g_silenced{0};

HRESULT STDMETHODCALLTYPE HookReleaseBuffer(IAudioRenderClient* client, UINT32 frames, DWORD flags) {
    g_silenced.fetch_add(1, std::memory_order_relaxed);
    return g_orig_release(client, frames, flags | AUDCLNT_BUFFERFLAGS_SILENT);
}

void* FindReleaseBuffer(char* why, std::size_t cap) {
    void* target = nullptr;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    const char* step = "MMDeviceEnumerator";
    if (SUCCEEDED(hr)) step = "GetDefaultAudioEndpoint", hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) step = "Activate(IAudioClient)", hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) step = "GetMixFormat", hr = client->GetMixFormat(&fmt);
    if (SUCCEEDED(hr)) step = "Initialize", hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, fmt, nullptr);
    if (SUCCEEDED(hr)) step = "GetService(IAudioRenderClient)", hr = client->GetService(IID_PPV_ARGS(&render));
    if (SUCCEEDED(hr) && render) target = (*reinterpret_cast<void***>(render))[4];  // IUnknown(3), GetBuffer, ReleaseBuffer
    else std::snprintf(why, cap, "%s failed (hr=0x%08lX)", step, static_cast<unsigned long>(hr));
    if (render) render->Release();
    if (fmt) CoTaskMemFree(fmt);
    if (client) client->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    return target;
}

void InstallMute() {
    // COM on a short-lived thread of our own: the bootstrap thread's apartment stays untouched.
    // The context lives on the heap: when the audio stack hangs past the wait below,
    // the thread still writes into it after this function returned (it is leaked then).
    struct Ctx {
        char why[160] = "no audio device";
        void* target = nullptr;
    };
    Ctx* ctx = new (std::nothrow) Ctx;
    if (!ctx) return;
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* c = static_cast<Ctx*>(p);
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            c->target = FindReleaseBuffer(c->why, sizeof(c->why));
            CoUninitialize();
        }
        return 0;
    }, ctx, 0, nullptr);
    if (!t) {
        Log("[auto] audio not muted: no thread (error %lu)", GetLastError());
        delete ctx;
        return;
    }
    const bool finished = WaitForSingleObject(t, 5000) == WAIT_OBJECT_0;
    CloseHandle(t);
    if (!finished) {  // ctx stays allocated: the thread may still write into it
        Log("[auto] audio not muted: the audio device did not answer within 5 s");
        return;
    }
    char why[160];
    std::snprintf(why, sizeof(why), "%s", ctx->why);
    void* const target = ctx->target;
    delete ctx;
    if (!target) {
        Log("[auto] audio not muted: %s", why);
        return;
    }
    const MH_STATUS init = MH_Initialize();
    MH_STATUS s = init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED ? MH_OK : init;
    if (s == MH_OK) s = MH_CreateHook(target, reinterpret_cast<void*>(&HookReleaseBuffer), reinterpret_cast<void**>(&g_orig_release));
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s == MH_OK) Log("[auto] audio muted for this run (IAudioRenderClient::ReleaseBuffer -> SILENT)");
    else Log("[auto] audio not muted: hook failed (%s)", MH_StatusToString(s));
}

// ------------------------------------------------------------------ game pad state (read only)
// What ds.exe made of the pad, read through ReadProcessMemory (a bad page fails the read,
// it never faults inside the game). Static layout (docs/hidden-testing.md "Pad input in
// gameplay"): the input manager DAT_147BE6330 has 7 player slots of 0xF0 bytes; slot +0x30
// = its pad controller, +0x40..+0xEF = the 0xB0-byte pad state FUN_1419B3090 wrote (floats
// LX LY RX RY at +0x40.., +0xE4 = 1 read / 2 app inactive / 0 none), +0x108 = player id.
// Controller: +0x28 device (1 PlayStation, 2 Steam Input, 3 XInput, 4 HID), +0x68 mode
// (1 = gated by the app-active flag), +0x6C pad index. Platform DAT_14577D3F0 +0x68 = app active.
namespace gamepad {
constexpr std::uintptr_t kInputMgr = 0x147BE6330 - 0x140000000, kPlatform = 0x14577D3F0 - 0x140000000,
                         kDevRead = 0x1419B3090 - 0x140000000, kMouse = 0x147BE69F8 - 0x140000000,
                         kKeyboard = 0x147BE69F0 - 0x140000000;

template <class T>
bool Peek(std::uintptr_t addr, T* out) noexcept {
    SIZE_T got = 0;
    return addr > 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), out, sizeof(T), &got) &&
           got == sizeof(T);
}

bool Known() noexcept {
    static int known = -1;  // the pad reader's prologue (push rbx/rsi/rdi/r14; sub rsp,48h): this ds.exe build
    if (known < 0) {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::uint64_t head = 0;
        known = Peek(base + kDevRead, &head) && head == 0x8348'5641'5756'5340ull ? 1 : 0;
    }
    return known == 1;
}

std::string Describe() {
    if (!Known()) return "unknown ds.exe build";
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    char b[200];
    std::uintptr_t platform = 0;
    std::uint8_t active = 0xFF;
    if (Peek(base + kPlatform, &platform)) Peek(platform + 0x68, &active);
    // Keyboard/mouse activity as FUN_1432960C0 sees it (either one makes it "in use"): mouse
    // DAT_147BE69F8, record +0xBC + idx*0x24 (idx +0x104; FUN_1428CB6A0 checks the qwords at
    // +0x08/+0x1C and the bytes +0x14..+0x18), keyboard DAT_147BE69F0, 0x81 key bytes at
    // +0xD4 + idx*0x81 (idx +0x1D8; FUN_1419BA030).
    std::uintptr_t mouse = 0, kb = 0;
    Peek(base + kMouse, &mouse);
    Peek(base + kKeyboard, &kb);
    std::int32_t midx = -1, kidx = -1;
    std::uint8_t rec[0x24] = {}, keys[0x81] = {};
    bool mouse_active = false;
    int keys_down = 0;
    if (mouse && Peek(mouse + 0x104, &midx) && midx >= 0 && midx < 64 && Peek(mouse + 0xBC + static_cast<std::uintptr_t>(midx) * 0x24, &rec)) {
        std::uint64_t q08 = 0, q1c = 0;
        std::memcpy(&q08, rec + 0x08, 8), std::memcpy(&q1c, rec + 0x1C, 8);
        mouse_active = q08 || q1c || rec[0x14] || rec[0x15] || rec[0x16] || rec[0x17] || rec[0x18];
    }
    if (kb && Peek(kb + 0x1D8, &kidx) && kidx >= 0 && kidx < 64 && Peek(kb + 0xD4 + static_cast<std::uintptr_t>(kidx) * 0x81, &keys))
        for (const std::uint8_t k : keys) keys_down += k ? 1 : 0;
    std::snprintf(b, sizeof(b), "app_active=%d mouse=%s keys_down=%d", static_cast<int>(active), mouse_active ? "active" : "idle", keys_down);
    std::string out = b;
    for (int i = 0; i < 7; ++i) {
        const std::uintptr_t slot = base + kInputMgr + static_cast<std::uintptr_t>(i) * 0xF0;
        std::int32_t used = 0, player = 0;
        std::uintptr_t ctrl = 0;
        float st[4] = {};
        if (!Peek(slot + 0xE4, &used) || !used || !Peek(slot + 0x108, &player) || !Peek(slot + 0x30, &ctrl) || !Peek(slot + 0x40, &st))
            continue;
        std::int32_t dev = -1, mode = -1, pad = -1;
        if (ctrl) Peek(ctrl + 0x28, &dev), Peek(ctrl + 0x68, &mode), Peek(ctrl + 0x6C, &pad);
        std::snprintf(b, sizeof(b), " slot%d(player=%d state=%d device=%d mode=%d pad=%d sticks=%.2f,%.2f,%.2f,%.2f)", i, player, used,
                      dev, mode, pad, static_cast<double>(st[0]), static_cast<double>(st[1]), static_cast<double>(st[2]),
                      static_cast<double>(st[3]));
        out += b;
    }
    return out;
}
}  // namespace gamepad

// ------------------------------------------------------------------ game window
HWND GameWindow() {
    HWND h = overlay::input::Window();
    if (h && IsWindow(h)) return h;
    struct Find {
        HWND best = nullptr;
        LONG area = 0;
    } f;
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId() || GetWindow(w, GW_OWNER) || !IsWindowVisible(w)) return TRUE;
        RECT r{};
        GetWindowRect(w, &r);
        const LONG area = (r.right - r.left) * (r.bottom - r.top);
        auto* find = reinterpret_cast<Find*>(lp);
        if (area > find->area) find->best = w, find->area = area;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.best;
}

// ------------------------------------------------------------------ commands
struct Outcome {
    bool ok = false;
    std::string detail;
};

std::atomic<bool> g_quitting{false};

bool SleepFor(std::uint32_t ms) {
    const double end = NowMs() + ms;
    while (NowMs() < end) {
        const double left = end - NowMs();
        Sleep(static_cast<DWORD>(left > 50 ? 50 : (left > 1 ? left : 1)));
    }
    return true;
}

Outcome Fmt(bool ok, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return {ok, buf};
}

Outcome DoScreenshot(const Command& c) {
    if (!overlay::Installed()) return Fmt(false, "no overlay (debug_menu=0 or install failed): nothing can capture");
    const double t0 = NowMs();
    const std::uint32_t id = overlay::RequestCapture();
    if (!id) return Fmt(false, "too many captures pending");
    overlay::Image img;
    char why[200] = {};
    int r = 0;
    while ((r = overlay::TakeCapture(id, &img, why, sizeof(why))) == 0) {
        if (NowMs() - t0 > 20000) {
            overlay::CancelCapture(id);  // frees the slot: four timeouts must not block every later screenshot
            return Fmt(false, "no frame presented within 20 s (game not rendering?)");
        }
        Sleep(15);
    }
    if (r < 0) return Fmt(false, "capture failed: %s", why);
    const std::wstring path = g_out + L"\\" + Widen(c.text) + L".png";
    if (!overlay::WritePngFile(path, img)) return Fmt(false, "could not write %s.png", c.text.c_str());
    // Mean brightness: a quick hint whether the frame is black.
    unsigned long long sum = 0;
    const std::size_t n = img.rgb.size();
    for (std::size_t i = 0; i < n; i += 97) sum += img.rgb[i];
    const double mean = n ? static_cast<double>(sum) / static_cast<double>((n + 96) / 97) : 0;
    return Fmt(true, "%s.png %ux%u mean=%.1f after %.0f ms", c.text.c_str(), img.width, img.height, mean, NowMs() - t0);
}

Outcome DoPad(const Command& c) {
    const overlay::input::Stats is = overlay::input::GetStats();
    if (!overlay::Installed() || !is.xinput_hook)
        return Fmt(false, "virtual pad unavailable (overlay not installed or XInputGetState not hooked)");
    overlay::input::PadState p;
    p.connected = true;
    p.buttons = c.buttons;
    p.lx = c.lx, p.ly = c.ly, p.rx = c.rx, p.ry = c.ry, p.lt = c.lt, p.rt = c.rt;
    const unsigned long reads0 = is.virtual_pad_reads;
    // Defensive: ds.exe's pad reader returns an empty pad for "mode 1" controllers while its
    // app-active flag is clear (set only by WM_ACTIVATEAPP); the hidden window is never
    // activated by the user, so it is told again before every hold (see AssertActive).
    overlay::input::AssertActive();
    const debug::Snapshot before = debug::GetSnapshot();
    overlay::input::SetVirtualPad(p);
    std::string seen;
    if (c.ms >= 600) {  // what the game made of the held pad (its own input manager, read only)
        SleepFor(300);
        seen = gamepad::Describe();
        SleepFor(c.ms - 300);
    } else {
        SleepFor(c.ms);
    }
    if (!c.keep) overlay::input::SetVirtualPad(overlay::input::PadState{true});  // idle, still connected
    const unsigned long reads = overlay::input::GetStats().virtual_pad_reads - reads0;
    const debug::Snapshot after = debug::GetSnapshot();
    char moved[96] = "";
    if (before.local_valid && after.local_valid) {  // z is up in Decima: the walk is in x/y
        const double dx = after.local_pos[0] - before.local_pos[0], dy = after.local_pos[1] - before.local_pos[1];
        std::snprintf(moved, sizeof(moved), "; player moved %.2f m (speed %.2f)", std::sqrt(dx * dx + dy * dy),
                      static_cast<double>(after.speed));
    }
    return Fmt(true, "buttons=0x%04X held %u ms%s; the game read the pad %lu times%s%s%s", c.buttons, c.ms, c.keep ? " (kept)" : "", reads, moved,
               seen.empty() ? "" : "; game pad state: ", seen.c_str());
}

Outcome DoKey(const Command& c) {
    const HWND w = GameWindow();
    if (!w) return Fmt(false, "no game window yet");
    const UINT scan = MapVirtualKeyW(c.vk, MAPVK_VK_TO_VSC);
    const LPARAM down = 1 | (static_cast<LPARAM>(scan) << 16);
    const LPARAM up = down | (1LL << 30) | (1LL << 31);
    PostMessageW(w, WM_KEYDOWN, c.vk, down);
    SleepFor(c.ms);
    PostMessageW(w, WM_KEYUP, c.vk, up);
    return Fmt(true, "vk 0x%02X posted to window %p (window procedure only, not WH_KEYBOARD)", c.vk, static_cast<void*>(w));
}

Outcome DoMenu(const Command& c) {
    if (!overlay::Installed()) return Fmt(false, "no overlay");
    const bool want = c.text == "open" ? true : c.text == "close" ? false : !overlay::IsOpen();
    overlay::SetOpen(want);
    const double t0 = NowMs();
    while (overlay::IsOpen() != want) {
        if (NowMs() - t0 > 5000) return Fmt(false, "menu still %s after 5 s (no Present?)", want ? "closed" : "open");
        Sleep(15);
    }
    return Fmt(true, "menu %s", want ? "open" : "closed");
}

Outcome DoAction(const Command& c) {
    std::size_t n = 0;
    const debug::ActionInfo* rows = debug::Actions(&n);
    const debug::ActionInfo* found = nullptr;
    std::string names;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string norm = NormalizeActionName(rows[i].label ? rows[i].label : "");
        if (norm == NormalizeActionName(c.text)) found = &rows[i];
        names += (names.empty() ? "" : ", ") + norm;
    }
    if (!found) return Fmt(false, "unknown action '%s' (known: %s)", c.text.c_str(), names.c_str());
    debug::Request r;
    r.action = found->id;
    for (std::size_t i = 0; i < c.numbers.size() && i < 3; ++i) r.v[i] = c.numbers[i];
    for (std::size_t i = 0; i < c.numbers.size() && i < 2; ++i) r.f[i] = static_cast<float>(c.numbers[i]);
    if (c.numbers.size() == 1) r.i = static_cast<std::int32_t>(c.numbers[0]);  // voice_ping <line>
    r.flag = !c.numbers.empty() && c.numbers[0] != 0;
    const std::uint32_t serial = debug::Queue(r);
    if (!serial) return Fmt(false, "action queue full");
    const double t0 = NowMs();
    debug::Result res;
    while (!debug::ResultFor(serial, &res)) {
        if (NowMs() - t0 > 15000) return Fmt(false, "'%s' queued but not run within 15 s (no game tick?)", found->label);
        Sleep(15);
    }
    return Fmt(res.ok, "%s: %s", found->label, res.text);
}

std::string SnapshotText() {
    const debug::Snapshot s = debug::GetSnapshot();
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "serial=%llu role=%s mod_state=\"%s\" local_present=%d local_valid=%d pos=(%.2f, %.2f, %.2f) speed=%.2f "
                  "partner=%s link=%s tick_hz=%.1f ticks=%llu",
                  static_cast<unsigned long long>(s.serial), s.role, s.mod_state, s.local_present ? 1 : 0, s.local_valid ? 1 : 0,
                  s.local_pos[0], s.local_pos[1], s.local_pos[2], static_cast<double>(s.speed), s.partner_state, s.link_state,
                  static_cast<double>(s.tick_hz), static_cast<unsigned long long>(s.ticks));
    return buf;
}

Outcome DoWaitPlayer(const Command& c) {
    const double t0 = NowMs();
    for (;;) {
        const debug::Snapshot s = debug::GetSnapshot();
        if (s.local_valid)
            return Fmt(true, "local player at (%.2f, %.2f, %.2f) after %.1f s", s.local_pos[0], s.local_pos[1], s.local_pos[2],
                       (NowMs() - t0) / 1000.0);
        if (NowMs() - t0 > c.ms) return Fmt(false, "no local player within %u s (%s)", c.ms / 1000, SnapshotText().c_str());
        Sleep(100);
    }
}

Outcome DoWaitLog(const Command& c) {
    std::uint64_t after = LogNewestSeq();
    const double t0 = NowMs();
    static LogLine lines[64];  // automation thread only
    for (;;) {
        std::size_t n;
        while ((n = LogCopyRecent(after, lines, 64)) > 0) {
            for (std::size_t i = 0; i < n; ++i) {
                after = lines[i].seq;
                // our own "[auto]" lines echo the needle; never match them
                if (!std::strstr(lines[i].text, "[auto]") && std::strstr(lines[i].text, c.text.c_str()))
                    return Fmt(true, "%s", lines[i].text);
            }
        }
        if (NowMs() - t0 > c.ms) return Fmt(false, "no new log line with '%s' within %u s", c.text.c_str(), c.ms / 1000);
        Sleep(50);
    }
}

void DoQuit() {
    g_quitting.store(true);
    const HWND w = GameWindow();
    Log("[auto] quit: WM_CLOSE to the game window %p; the process ends within 20 s", static_cast<void*>(w));
    if (w) PostMessageW(w, WM_CLOSE, 0, 0);
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        Sleep(20000);
        Log("[auto] quit: the game did not exit after WM_CLOSE: ending the process");
        LogShutdown();
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

Outcome Execute(const Command& c) {
    switch (c.kind) {
        case Kind::wait: SleepFor(c.ms); return Fmt(true, "%u ms", c.ms);
        case Kind::log: Log("[auto] %s", c.text.c_str()); return Fmt(true, "logged");
        case Kind::screenshot: return DoScreenshot(c);
        case Kind::pad: return DoPad(c);
        case Kind::padoff: overlay::input::SetVirtualPad(overlay::input::PadState{}); return Fmt(true, "virtual pad disconnected");
        case Kind::key: return DoKey(c);
        case Kind::menu: return DoMenu(c);
        case Kind::action: return DoAction(c);
        case Kind::waitplayer: return DoWaitPlayer(c);
        case Kind::waitlog: return DoWaitLog(c);
        case Kind::snapshot: return Fmt(true, "%s", SnapshotText().c_str());
        case Kind::quit: return Fmt(true, "quitting");
        default: return Fmt(false, "unknown command");
    }
}

void OneLine(std::string& s) {
    for (char& ch : s)
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
}

void RunFile(const std::wstring& name) {
    const std::wstring path = g_in + L"\\" + name;
    std::string text;
    if (!ReadText(path, &text)) return;
    DeleteFileW(path.c_str());  // taken
    const std::wstring base = name.substr(0, name.size() - 4);  // without ".cmd"
    const std::string file = Narrow(base);
    bool all_ok = true;
    std::string last;
    int lineno = 0, ran = 0;
    bool quit = false;
    std::size_t at = 0;
    while (at <= text.size() && !quit) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        at = nl + 1;
        ++lineno;
        Command c;
        std::string err;
        Outcome o;
        if (!ParseCommand(line, &c, &err)) {
            if (err.empty()) continue;  // blank / comment
            o = {false, err};
        } else {
            const double t0 = NowMs();
            try {
                o = Execute(c);
            } catch (...) {
                o = {false, "exception in the automation thread"};
            }
            OneLine(o.detail);
            char took[32];
            std::snprintf(took, sizeof(took), " (%.0f ms)", NowMs() - t0);
            o.detail += took;
            quit = c.kind == Kind::quit;
        }
        ++ran;
        g_commands.fetch_add(1);
        if (!o.ok) all_ok = false, g_failures.fetch_add(1);
        std::string trimmed = detail::Trim(line);
        OneLine(trimmed);
        Log("[auto] %s:%d %s -> %s: %s", file.c_str(), lineno, trimmed.c_str(), o.ok ? "ok" : "FAIL", o.detail.c_str());
        Append(g_out + L"\\results.txt", file + ":" + std::to_string(lineno) + "\t" + (o.ok ? "ok" : "fail") + "\t" + trimmed +
                                             "\t" + o.detail + "\n");
        last = o.detail;
        if (!o.ok) break;  // a failed line ends its file
    }
    WriteAtomic(g_out + L"\\" + base + L".done", std::string(all_ok ? "ok" : "fail") + "\n" + std::to_string(ran) + "\n" + last + "\n");
    if (quit) DoQuit();
}

void WriteStatus(double started) {
    const overlay::Stats os = overlay::GetStats();
    const overlay::input::Stats is = overlay::input::GetStats();
    const overlay::input::PadState pad = overlay::input::VirtualPad();
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
                  "pid=%lu\nuptime_s=%.1f\ncommands=%lu\nfailures=%lu\noverlay_installed=%d\noverlay_disabled=%d\n"
                  "overlay_reason=%s\npresents=%lu\nfps=%.1f\nbackbuffer=%ux%u\nformat=%d\nqueue=%s\nmenu_open=%d\n"
                  "occluded_masked=%lu\nwindow=%p\nxinput_hook=%d\nxinput14_hook=%d\npad_connected=%d\npad_buttons=0x%04X\npad_reads=%lu\n"
                  "isolated_pad_reads=%lu\nforeground_answers=%lu\nactivate_app_true=%lu\nactivate_app_rewritten=%lu\n"
                  "activate_app_posted=%lu\nxinput_set_hook=%d\nvibration_kept=%lu\naudio_buffers_silenced=%lu\nsaves_redirected=%d\n"
                  "saves_redirected_calls=%lu\nsaves_redirect_failures=%lu\nlog_seq=%llu\nsnapshot=%s\n",
                  GetCurrentProcessId(), (NowMs() - started) / 1000.0, g_commands.load(), g_failures.load(),
                  os.installed ? 1 : 0, os.disabled ? 1 : 0, os.disabled_reason, os.presents, static_cast<double>(os.fps),
                  os.width, os.height, os.format, os.queue_source, overlay::IsOpen() ? 1 : 0, os.occluded_masked,
                  static_cast<void*>(overlay::input::Window()), is.xinput_hook ? 1 : 0, is.xinput14_hook ? 1 : 0,
                  pad.connected ? 1 : 0, pad.buttons, is.virtual_pad_reads, is.isolated_pad_reads, is.foreground_answers,
                  is.activate_app_true, is.activate_app_rewritten, is.activate_app_posted, is.xinput_set_hook ? 1 : 0,
                  is.vibration_kept, g_silenced.load(), g_rd_active.load() ? 1 : 0, g_rd_calls.load(), g_rd_failures.load(),
                  static_cast<unsigned long long>(LogNewestSeq()), SnapshotText().c_str());
    WriteAtomic(g_out + L"\\status.txt", buf);
}

DWORD WINAPI Thread(LPVOID) {
    const double started = NowMs();
    double last_status = -1e9;
    for (;;) {
        if (NowMs() - last_status >= 1000) {
            WriteStatus(started);
            last_status = NowMs();
        }
        if (!g_quitting.load()) {
            std::vector<std::wstring> names;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((g_in + L"\\*.cmd").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.emplace_back(fd.cFileName);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            std::sort(names.begin(), names.end());
            for (const std::wstring& n : names) {
                if (g_quitting.load()) break;
                try {
                    RunFile(n);
                } catch (...) {
                    Log("[auto] exception while running %s", Narrow(n).c_str());
                }
                if (NowMs() - last_status >= 1000) {
                    WriteStatus(started);
                    last_status = NowMs();
                }
            }
        }
        Sleep(100);
    }
}

}  // namespace

bool Init(const std::wstring& mod_dir) noexcept {
    try {
        std::ifstream in(mod_dir + L"\\session.ini");
        if (!in) return false;
        std::string line;
        bool on = false, mute = true, isolate = true, sandbox = false;
        while (std::getline(in, line)) {
            line = detail::Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = detail::Trim(line.substr(0, eq)), v = detail::Lower(detail::Trim(line.substr(eq + 1)));
            const bool b = v == "1" || v == "true" || v == "yes" || v == "on";
            if (k == "automation") on = b;
            else if (k == "automation_mute") mute = b;
            else if (k == "automation_isolate") isolate = b;
            else if (k == "sandbox_saves" && !v.empty()) sandbox = true;
        }
        if (!on && sandbox) {
            // Launcher test save (sandbox.hpp): DllMain installed the redirect, or the game must not run.
            if (!g_rd_active.load() || g_mode.load() != 2) {
                Log("[sandbox] saves=NOT redirected although session.ini asks for a test save: ending ds.exe before it can touch "
                    "your saves");
                LogShutdown();
                g_mode.store(2);
                RefuseAndExit("sandbox_saves is set but the save redirect was not installed in DllMain");
                return false;
            }
            Log("[sandbox] test save: the game reads and writes only the sandbox copy; your own saves are not opened");
            LogSaves();
            return false;
        }
        g_enabled.store(on);
        g_mute.store(mute);
        g_isolate.store(isolate);
        if (on)
            Log("[auto] automation=1 in session.ini: hidden test run (screenshots, command files%s%s)",
                isolate ? ", virtual pad, input/display isolation" : "; automation_isolate=0: no isolation hooks", mute ? ", muted" : "");
        if (on && !g_rd_active.load()) {
            // DllMain did not see automation=1 (session.ini unreadable at that moment?) or did not
            // finish the redirect: the game must not go on with the user's saves.
            Log("[auto] saves=NOT redirected although automation=1: ending ds.exe before it can touch the saves");
            LogShutdown();
            RefuseAndExit("automation=1 but the save redirect was not installed in DllMain");
            return false;
        }
        if (on) LogSaves();
        return on;
    } catch (...) {
        return false;
    }
}

namespace {

// Fault-free read of the game's own memory (DS's crash reporter fires even on SEH-caught faults).
bool ReadGamePtr(std::uintptr_t addr, std::uintptr_t& out) noexcept {
    SIZE_T n = 0;
    out = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), &out, sizeof(out), &n) && n == sizeof(out);
}

bool BytesAt(std::uintptr_t addr, const unsigned char* want, std::size_t len) noexcept {
    unsigned char got[16] = {};
    SIZE_T n = 0;
    return len <= sizeof(got) && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), got, len, &n) && n == len &&
           std::memcmp(got, want, len) == 0;
}

struct ModuleSpan {
    std::uintptr_t base = 0, end = 0;
    wchar_t name[64] = {};
    bool system = false;  // under the Windows folder (incl. System32\DriverStore)
};

// Every module of this process (no locks held by us afterwards: the list is copied first).
std::size_t ListModules(ModuleSpan* out, std::size_t cap) noexcept {
    HMODULE mods[512];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) return 0;
    wchar_t windir[MAX_PATH] = {};
    const UINT wl = GetWindowsDirectoryW(windir, MAX_PATH);
    std::size_t n = 0;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE) && n < cap; ++i) {
        MODULEINFO mi{};
        wchar_t path[MAX_PATH] = {};
        if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        GetModuleFileNameW(mods[i], path, MAX_PATH);
        ModuleSpan& m = out[n++];
        m.base = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
        m.end = m.base + mi.SizeOfImage;
        m.system = wl && _wcsnicmp(path, windir, wl) == 0;
        const wchar_t* slash = wcsrchr(path, L'\\');
        wcsncpy_s(m.name, slash ? slash + 1 : path, _TRUNCATE);
    }
    return n;
}

void LogThirdPartyModules(const char* when) noexcept {
    static ModuleSpan mods[512];
    const std::size_t n = ListModules(mods, 512);
    std::string list;
    for (std::size_t i = 0; i < n; ++i) {
        if (mods[i].system) continue;
        char name[96];
        std::snprintf(name, sizeof(name), "%s%ls", list.empty() ? "" : ", ", mods[i].name);
        list += name;
    }
    Log("[diag] %s: %zu modules; outside Windows: %s", when, n, list.c_str());
}

// The game's main thread: the oldest thread of this process.
DWORD MainThreadId() noexcept {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{sizeof(te)};
    DWORD best = 0;
    ULONGLONG best_time = ~0ull;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        const HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            const ULONGLONG t = (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
            if (t < best_time) best_time = t, best = te.th32ThreadID;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return best;
}

// Diagnosis of a game that does not finish creating its renderer (local test guest on a
// player's desktop, v0.5.4: d3d12.dll loaded at 0.4 s, no device for ~27 s, then "does not support
// DirectX 12"). Where is the main thread? Suspend it for a moment (only GetThreadContext and a
// ReadProcessMemory into a fixed buffer while it is suspended: it may hold the heap or loader
// lock), then name the module of RIP and of every stack value that points into a module.
void LogMainThreadWhere(const char* when) noexcept {
    static ModuleSpan mods[512];
    const std::size_t n = ListModules(mods, 512);
    const DWORD tid = MainThreadId();
    if (!tid || tid == GetCurrentThreadId()) return;
    const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return;
    static std::uintptr_t stack[1024];
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    SIZE_T got = 0;
    bool have = false;
    if (SuspendThread(h) != static_cast<DWORD>(-1)) {
        have = GetThreadContext(h, &ctx) != 0;
        if (have) ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(ctx.Rsp), stack, sizeof(stack), &got);
        ResumeThread(h);
    }
    CloseHandle(h);
    if (!have) return;
    const auto where = [&](std::uintptr_t a, char* buf, std::size_t cap) {
        for (std::size_t i = 0; i < n; ++i)
            if (a >= mods[i].base && a < mods[i].end) {
                std::snprintf(buf, cap, "%ls+0x%llX", mods[i].name, static_cast<unsigned long long>(a - mods[i].base));
                return true;
            }
        return false;
    };
    char rip[96] = "?";
    where(ctx.Rip, rip, sizeof(rip));
    std::string frames;
    int count = 0;
    for (std::size_t i = 0; i < got / sizeof(std::uintptr_t) && count < 24; ++i) {
        char b[96];
        if (!where(stack[i], b, sizeof(b))) continue;
        frames += (frames.empty() ? "" : " | ");
        frames += b;
        ++count;
    }
    Log("[diag] %s: main thread %lu at %s; stack values in modules: %s", when, tid, rip, frames.c_str());
}

}  // namespace

bool WaitForGameD3D12(unsigned timeout_ms) noexcept {
    const double t0 = NowMs();
    if (GetModuleHandleW(L"d3d12.dll")) {
        Log("[overlay] d3d12.dll was already loaded before the overlay: ds.exe may exit(0) at its device check (0x1419A6CE9)");
        return true;
    }
    while (!GetModuleHandleW(L"d3d12.dll")) {
        if (NowMs() - t0 > timeout_ms) {
            Log("[overlay] ds.exe did not load d3d12.dll within %u s: overlay not installed this run", timeout_ms / 1000);
            return false;
        }
        Sleep(2);
    }
    const double loaded_ms = NowMs();
    Log("[overlay] deferred %.0f ms until ds.exe loaded d3d12.dll itself (its device check at 0x1419A6980 exits(0) when "
        "d3d12.dll is loaded earlier)",
        loaded_ms - t0);

    // d3d12.dll is loaded inside the game's device creation (FUN_1419A6980), which then sets the
    // process-wide DRED settings and creates its device; Steam's overlay hooks D3D12/DXGI around
    // the same time on a player's desktop. Anything the overlay does to D3D12 in that window (its
    // WARP probe device, inline hooks on D3D12CreateDevice or the swapchain) broke the game's start
    // on a player's PC (v0.5.0 / v0.5.1: "Error initializing rendering configuration", then its
    // crash report crashed reading the missing device at 0x1419A4004). So the overlay touches
    // nothing until the game's renderer exists, read from the game's own globals (no hooks):
    //   0x145558F70: written at 0x1419A6D22 right after the device is created;
    //   [0x145558F80 + 0x20]: the render device object the crash report reads (0x1419A4000).
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    static const unsigned char kStoreF70[] = {0x48, 0x89, 0x05, 0x47, 0x22, 0xBB, 0x03};  // mov [rip+0x3BB2247], rax
    static const unsigned char kLeaF80[] = {0x48, 0x8D, 0x0D, 0x0B, 0x11, 0xC6, 0x03};    // lea rcx, [rip+0x3C6110B]
    static const unsigned char kReadF80[] = {0x48, 0x8B, 0x49, 0x20, 0x48, 0x8B, 0x01};    // mov rcx,[rcx+0x20]; mov rax,[rcx]
    const bool known = BytesAt(base + 0x19A6D22, kStoreF70, sizeof(kStoreF70)) && BytesAt(base + 0x18F7E6E, kLeaF80, sizeof(kLeaF80)) &&
                       BytesAt(base + 0x19A4000, kReadF80, sizeof(kReadF80));
    constexpr double kFallbackMs = 30000;  // the title screen comes about 20 s after start
    if (!known) {
        Log("[overlay] unknown ds.exe build (renderer globals not found): waiting %.0f s before installing", kFallbackMs / 1000);
        Sleep(static_cast<DWORD>(kFallbackMs));
        return true;
    }
    std::uintptr_t created = 0, render_device = 0, holder = 0;
    double seen_ms = 0;
    LogThirdPartyModules("modules when d3d12.dll loaded");
    int diag_step = 0;  // a renderer that takes long: log where the main thread is at 5, 12, 20 and 28 s
    static const double kDiagAtMs[] = {5000, 12000, 20000, 28000};
    for (;;) {
        if (diag_step < 4 && !(created && render_device) && NowMs() - loaded_ms > kDiagAtMs[diag_step]) {
            char when[64];
            std::snprintf(when, sizeof(when), "no renderer %.0f s after d3d12.dll loaded", kDiagAtMs[diag_step] / 1000);
            if (diag_step == 0) LogThirdPartyModules(when);
            LogMainThreadWhere(when);
            ++diag_step;
        }
        ReadGamePtr(base + 0x5558F70, created);
        if (ReadGamePtr(base + 0x5558F80 + 0x20, holder)) render_device = holder;
        if (created && render_device) {
            if (seen_ms == 0) seen_ms = NowMs();
            if (NowMs() - seen_ms >= 2000) break;  // let the game (and Steam's overlay) finish their set-up first
        } else {
            seen_ms = 0;
        }
        if (NowMs() - loaded_ms > kFallbackMs) {
            Log("[overlay] the game's renderer was not seen within %.0f s (device %s, render device %s): installing anyway",
                kFallbackMs / 1000, created ? "set" : "not set", render_device ? "set" : "not set");
            return true;
        }
        Sleep(10);
    }
    Log("[overlay] the game's renderer is up (%.0f ms after d3d12.dll loaded): the overlay installs after it", seen_ms - loaded_ms);
    if (g_enabled.load()) LogMainThreadWhere("hidden run self-test of the stall diagnosis");  // proves the code path
    return true;
}

bool Enabled() noexcept { return g_enabled.load(); }
std::wstring OutDir() noexcept { return g_enabled.load() ? g_out : std::wstring(); }
bool SavesRedirected() noexcept { return g_rd_active.load(); }
bool ForeignSavesBypassed() noexcept { return g_foreign_bypassed.load(); }
bool MuteRequested() noexcept { return g_mute.load(); }
bool IsolateRequested() noexcept { return g_isolate.load(); }

void Start(const std::wstring& mod_dir, const char* build) noexcept {
    if (!g_enabled.load() || g_started.exchange(true)) return;
    try {
        g_build = build ? build : "";
        const std::wstring root = mod_dir + L"\\automation";
        g_in = root + L"\\in";
        g_out = root + L"\\out";
        CreateDirectoryW(root.c_str(), nullptr);
        CreateDirectoryW(g_in.c_str(), nullptr);
        CreateDirectoryW(g_out.c_str(), nullptr);
        if (g_mute.load()) InstallMute();
        char ready[256];
        std::snprintf(ready, sizeof(ready), "ready\npid=%lu\nbuild=%s\noverlay=%d\n", GetCurrentProcessId(), g_build.c_str(),
                      overlay::Installed() ? 1 : 0);
        HANDLE t = CreateThread(nullptr, 0, &Thread, nullptr, 0, nullptr);
        if (!t) {
            Log("[auto] could not start the automation thread (error %lu)", GetLastError());
            return;
        }
        CloseHandle(t);
        WriteAtomic(g_out + L"\\ready.txt", ready);
        Log("[auto] channel ready: commands from %s", Narrow(g_in).c_str());
    } catch (...) {
        Log("[auto] automation start failed (exception)");
    }
}

// ------------------------------------------------------------------ DllMain: save redirect
namespace {

bool AddRoot(const std::wstring& root, const wchar_t* scratch) noexcept {
    try {
        if (root.size() < 4 || root.size() >= kPathCap) return false;
        const std::wstring norm = saves::Normalize(root);
        const int n = g_nroots.load();
        for (int i = 0; i < n; ++i)
            if (saves::Map(norm, g_roots[i], L"X:\\", nullptr) && saves::Map(g_roots[i], norm, L"X:\\", nullptr)) return true;  // same
        if (n >= kMaxRoots) return false;
        WcsCopy(g_roots[n], kPathCap, norm.c_str());
        WcsCopy(g_root_scratch[n], kPathCap, scratch);
        g_nroots.store(n + 1);
        return true;
    } catch (...) {
        return false;
    }
}

std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[kPathCap] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(kPathCap));
    return n > 0 && n < kPathCap ? std::wstring(buf, n) : std::wstring();
}

// The hooks: kernelbase.dll implements all of these (kernel32's exports are stubs into
// it), except MoveFileW/MoveFileA/MoveFileExA/CopyFileA (kernel32 only). MinHook patches
// the function bodies, so kernelbase's internal calls (FindFirstFileW -> FindFirstFileExW)
// go through them too; mapping is idempotent (the scratch root is outside every save root).
#define ST_HOOK(dll, name, req) {dll, #name, reinterpret_cast<void*>(&H_##name), reinterpret_cast<void**>(&o_##name), req}
const HookSpec kSaveHooks[] = {
    ST_HOOK(L"kernelbase.dll", CreateFileW, true),         ST_HOOK(L"kernelbase.dll", FindFirstFileW, true),
    ST_HOOK(L"kernelbase.dll", FindFirstFileExW, true),    ST_HOOK(L"kernelbase.dll", GetFileAttributesW, true),
    ST_HOOK(L"kernelbase.dll", SetFileAttributesW, true),  ST_HOOK(L"kernelbase.dll", DeleteFileW, true),
    ST_HOOK(L"kernelbase.dll", RemoveDirectoryW, true),    ST_HOOK(L"kernelbase.dll", CreateDirectoryW, true),
    ST_HOOK(L"kernelbase.dll", MoveFileExW, true),         ST_HOOK(L"kernelbase.dll", CopyFileW, true),
    ST_HOOK(L"kernel32.dll", MoveFileW, true),             ST_HOOK(L"kernelbase.dll", GetFileAttributesA, true),
    ST_HOOK(L"kernelbase.dll", CreateFileA, false),        ST_HOOK(L"kernelbase.dll", CreateFile2, false),
    ST_HOOK(L"kernelbase.dll", FindFirstFileA, false),     ST_HOOK(L"kernelbase.dll", FindFirstFileExA, false),
    ST_HOOK(L"kernelbase.dll", GetFileAttributesExW, false), ST_HOOK(L"kernelbase.dll", GetFileAttributesExA, false),
    ST_HOOK(L"kernelbase.dll", DeleteFileA, false),        ST_HOOK(L"kernelbase.dll", RemoveDirectoryA, false),
    ST_HOOK(L"kernelbase.dll", CreateDirectoryA, false),   ST_HOOK(L"kernelbase.dll", CreateDirectoryExW, false),
    ST_HOOK(L"kernelbase.dll", MoveFileWithProgressW, false), ST_HOOK(L"kernelbase.dll", CopyFileExW, false),
    ST_HOOK(L"kernelbase.dll", ReplaceFileW, false),       ST_HOOK(L"kernel32.dll", MoveFileA, false),
    ST_HOOK(L"kernel32.dll", MoveFileExA, false),          ST_HOOK(L"kernel32.dll", CopyFileA, false),
    ST_HOOK(L"kernelbase.dll", SetFileAttributesA, false), ST_HOOK(L"kernelbase.dll", CopyFile2, false),
    ST_HOOK(L"kernelbase.dll", CreateHardLinkW, false),
};
// shell32 is a static import of ds.exe (mapped before this DllMain runs); optional.
const HookSpec kShellHooks[] = {
    ST_HOOK(L"shell32.dll", ShellExecuteA, false),   ST_HOOK(L"shell32.dll", ShellExecuteW, false),
    ST_HOOK(L"shell32.dll", ShellExecuteExA, false), ST_HOOK(L"shell32.dll", ShellExecuteExW, false),
};
#undef ST_HOOK

void* Resolve(const wchar_t* dll, const char* name) noexcept {
    HMODULE m = GetModuleHandleW(dll);
    void* p = m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
    if (!p && _wcsicmp(dll, L"kernelbase.dll") == 0) {  // older systems: kernel32
        m = GetModuleHandleW(L"kernel32.dll");
        p = m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
    }
    return p;
}

void AppendMissing(const char* name) noexcept {
    const std::size_t at = std::strlen(g_rd_missing);
    std::snprintf(g_rd_missing + at, sizeof(g_rd_missing) - at, "%s%s", at ? " " : "", name);
}

// MinHook from DllMain: only kernel32/ntdll calls (VirtualAlloc, thread snapshot,
// Suspend/ResumeThread), no LoadLibrary; the process has no game threads yet.
bool InstallSaveHooks(char* why, std::size_t cap) noexcept {
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        std::snprintf(why, cap, "MinHook init failed (%s)", MH_StatusToString(init));
        return false;
    }
    std::vector<void*> created;
    try {
        created.reserve(sizeof(kSaveHooks) / sizeof(kSaveHooks[0]));
    } catch (...) {
        std::snprintf(why, cap, "out of memory");
        return false;
    }
    for (const HookSpec& h : kSaveHooks) {
        void* target = Resolve(h.dll, h.name);
        MH_STATUS s = target ? MH_CreateHook(target, h.detour, h.original) : MH_ERROR_FUNCTION_NOT_FOUND;
        if (s == MH_OK) {
            created.push_back(target);
            continue;
        }
        if (h.required) {
            std::snprintf(why, cap, "required hook %s failed (%s)", h.name, MH_StatusToString(s));
            for (void* t : created) MH_RemoveHook(t);
            return false;
        }
        AppendMissing(h.name);
    }
    for (void* t : created) {
        const MH_STATUS s = MH_EnableHook(t);
        if (s != MH_OK) {
            std::snprintf(why, cap, "enabling a file API hook failed (%s)", MH_StatusToString(s));
            return false;  // the caller ends the process: partially enabled hooks do not matter
        }
    }
    g_rd_hooks.store(static_cast<int>(created.size()));
    return true;
}

void InstallShellBlock() noexcept {
    int n = 0;
    for (const HookSpec& h : kShellHooks) {
        void* target = Resolve(h.dll, h.name);
        if (target && MH_CreateHook(target, h.detour, h.original) == MH_OK) {
            if (MH_EnableHook(target) == MH_OK) ++n;
            else MH_RemoveHook(target);
        }
    }
    g_shell_hooks.store(n);
}

void InstallExitTrace() noexcept {
    void* exit_target = Resolve(L"ntdll.dll", "RtlExitUserProcess");
    void* term_target = Resolve(L"kernelbase.dll", "TerminateProcess");
    if (exit_target && MH_CreateHook(exit_target, reinterpret_cast<void*>(&H_RtlExitUserProcess),
                                     reinterpret_cast<void**>(&o_RtlExitUserProcess)) == MH_OK)
        MH_EnableHook(exit_target);
    if (term_target && MH_CreateHook(term_target, reinterpret_cast<void*>(&H_TerminateProcess),
                                     reinterpret_cast<void**>(&o_TerminateProcess)) == MH_OK)
        MH_EnableHook(term_target);
}

}  // namespace

void EarlyInit() noexcept {
    wchar_t exe[kPathCap] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(kPathCap));
    if (n == 0 || n >= kPathCap) return;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return;
    *slash = 0;
    std::swprintf(g_base_mod_dir, kPathCap, L"%s\\stranded_together", exe);
    // ST_INSTANCE_DIR (sandbox.hpp): a second game on this PC keeps its own mod folder.
    try {
        const std::wstring dir = sandbox::ModDirFor(exe, EnvVar(sandbox::kInstanceEnv));
        if (dir.size() >= kPathCap - 64) return;
        WcsCopy(g_mod_dir, kPathCap, dir.c_str());
    } catch (...) {
        return;
    }
    std::swprintf(g_auto_dir, kPathCap, L"%s\\automation", g_mod_dir);
    std::string ini;
    wchar_t ini_path[kPathCap + 32];
    std::swprintf(ini_path, kPathCap + 32, L"%s\\session.ini", g_mod_dir);
    // A ds.exe that st_testenv_supervisor started must never run on the real saves: when its
    // automation session.ini is gone (deleted by another program between deploy and start) or
    // is not a hidden-run file, it ends here instead of running unredirected.
    const bool supervised = !EnvVar(saves::kSupervisedEnv).empty();
    // The same for a game the launcher's local two-player test started (ST_INSTANCE_DIR): its
    // instance session.ini is one-shot; without it the game would run on the real saves.
    const bool instanced = !EnvVar(sandbox::kInstanceEnv).empty();
    if (!ReadIniRaw(ini_path, &ini)) {
        if (supervised) {
            g_mode.store(1);
            RefuseAndExit("started by st_testenv_supervisor but there is no session.ini in the mod folder (deleted before the game "
                          "read it?): not running on the real saves");
        } else if (instanced) {
            g_mode.store(2);
            RefuseAndExit("started for the local two-player test (ST_INSTANCE_DIR) but there is no session.ini in its folder (already "
                          "used, or deleted?): not running on the real saves");
        }
        return;
    }
    bool exit_trace = true, foreign = false;
    sandbox::Request req;
    req.instance_env = instanced;
    std::wstring extra_root;
    try {
        std::size_t at = 0;
        while (at < ini.size()) {
            std::size_t nl = ini.find('\n', at);
            if (nl == std::string::npos) nl = ini.size();
            const std::string line = detail::Trim(ini.substr(at, nl - at));
            at = nl + 1;
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = detail::Trim(line.substr(0, eq)), v = detail::Trim(line.substr(eq + 1));
            const std::string lv = detail::Lower(v);
            const bool b = lv == "1" || lv == "true" || lv == "yes" || lv == "on";
            if (k == "automation") req.automation = b;
            else if (k == "automation_exit_trace") exit_trace = b;
            else if (k == "automation_foreign_saves") foreign = b;
            else if (k == "sandbox_saves") req.sandbox = v;
            else if (k == "sandbox_foreign_saves") req.sandbox_foreign = b;
            else if (k == "appdir_settings") req.appdir_settings = b;
            else if (k == "second_instance") req.second_instance = b;
            else if (k == "automation_save_root" && !v.empty()) {  // tests: an extra root (UTF-8)
                const int wn = MultiByteToWideChar(CP_UTF8, 0, v.data(), static_cast<int>(v.size()), nullptr, 0);
                extra_root.assign(static_cast<std::size_t>(wn > 0 ? wn : 0), L'\0');
                if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, v.data(), static_cast<int>(v.size()), extra_root.data(), wn);
            }
        }
    } catch (...) {
        req.automation = true;  // unreadable but present: refuse below rather than guess
        extra_root.clear();
    }
    const sandbox::Mode mode = sandbox::Decide(req);
    // (a test-save sandbox session is redirected too: allowed, the supervisor's --sandbox-session)
    if (supervised && mode != sandbox::Mode::automation && mode != sandbox::Mode::sandbox) {
        g_mode.store(1);
        RefuseAndExit("started by st_testenv_supervisor but session.ini has no automation=1 (not the run's file): not running on the "
                      "real saves");
        return;
    }
    if (mode == sandbox::Mode::none) return;  // every normal launch: nothing installed
    const bool automation = mode == sandbox::Mode::automation;
    g_mode.store(automation ? 1 : 2);
    WcsCopy(g_status_dir, kPathCap, automation ? g_auto_dir : g_mod_dir);

    char why[400] = {};
    if (mode == sandbox::Mode::refuse_bad_name) {
        std::snprintf(why, sizeof(why), "sandbox_saves must be a folder name of letters, digits, '_' and '-' (got '%.60s')", req.sandbox.c_str());
        RefuseAndExit(why);
        return;
    }
    if (mode == sandbox::Mode::refuse_orphan) {
        std::snprintf(why, sizeof(why),
                      "second_instance / appdir_settings / sandbox_foreign_saves need a save redirect (sandbox_saves= or a hidden run)");
        RefuseAndExit(why);
        return;
    }
    if (mode == sandbox::Mode::refuse_instance) {
        RefuseAndExit("started for the local two-player test (ST_INSTANCE_DIR) but its session.ini asks for no test save "
                      "(sandbox_saves=): not running on the real saves");
        return;
    }
    // Only st_testenv_supervisor starts a hidden run (it sets this variable for its child).
    // automation=1 left behind by an interrupted run must not turn a normal launch (Steam,
    // the user's desktop) into a redirected one whose progress lands in the scratch copy.
    if (automation && EnvVar(saves::kSupervisedEnv).empty()) {
        std::snprintf(why, sizeof(why),
                      "automation=1 in session.ini but ds.exe was not started by st_testenv_supervisor (%ls not set): left over from an "
                      "interrupted hidden run? Run tools\\testenv\\Restore-TestRun.ps1 for that run",
                      saves::kSupervisedEnv);
        RefuseAndExit(why);
        return;
    }
    wchar_t settings_scratch[kPathCap] = {};
    try {
        CreateDirectoryW(g_status_dir, nullptr);
        wchar_t stale[kPathCap + 64];
        std::swprintf(stale, kPathCap + 64, L"%s\\%s", g_status_dir, automation ? saves::kRefusedFile : sandbox::kRefusedFile);
        DeleteFileW(stale);
        std::swprintf(stale, kPathCap + 64, L"%s\\%s", g_status_dir, automation ? saves::kActiveFile : sandbox::kActiveFile);
        DeleteFileW(stale);
        if (automation) {
            std::swprintf(g_scratch, kPathCap, L"%s\\saves", g_auto_dir);
            std::swprintf(settings_scratch, kPathCap, L"%s\\settings.cfg", g_auto_dir);
        } else {
            // <game>\stranded_together\sandbox\<name>\ (shared by every instance folder; the launcher seeds it)
            const std::wstring name(req.sandbox.begin(), req.sandbox.end());
            std::swprintf(g_scratch, kPathCap, L"%s\\sandbox\\%s\\saves", g_base_mod_dir, name.c_str());
            std::swprintf(settings_scratch, kPathCap, L"%s\\sandbox\\%s\\settings.cfg", g_base_mod_dir, name.c_str());
        }
        // ds.exe: getenv("USERPROFILE") + "AppData\Local\KojimaProductions\..."; Steam's
        // AutoCloud and the user's view: %LOCALAPPDATA%\KojimaProductions.
        const std::wstring profile = EnvVar(L"USERPROFILE"), local = EnvVar(L"LOCALAPPDATA");
        if (!profile.empty()) AddRoot(profile + L"\\AppData\\Local\\KojimaProductions", g_scratch);
        if (!local.empty()) AddRoot(local + L"\\KojimaProductions", g_scratch);
        if (automation && !extra_root.empty() && !AddRoot(extra_root, g_scratch)) std::snprintf(why, sizeof(why), "automation_save_root not usable");
        g_nsave_roots = g_nroots.load();
        if (!why[0] && g_nsave_roots == 0) std::snprintf(why, sizeof(why), "no save root (USERPROFILE and LOCALAPPDATA empty)");
        for (int i = 0; !why[0] && i < g_nsave_roots; ++i)
            if (saves::Map(g_scratch, g_roots[i], L"X:\\", nullptr) || saves::Map(g_roots[i], g_scratch, L"X:\\", nullptr))
                std::snprintf(why, sizeof(why), "the scratch folder and a save root overlap");
        const DWORD sa = GetFileAttributesW(g_scratch);
        if (!why[0] && (sa == INVALID_FILE_ATTRIBUTES || !(sa & FILE_ATTRIBUTE_DIRECTORY)))
            std::snprintf(why, sizeof(why), automation ? "no seeded scratch copy at automation\\saves (the supervisor seeds it)"
                                                       : "no seeded test-save copy at stranded_together\\sandbox\\<name>\\saves (the launcher seeds it)");
        // appdir:settings.cfg (the graphics settings, read from the game folder): the file and the
        // temporary names a writer may use next to it.
        if (!why[0] && req.appdir_settings) {
            for (const wchar_t* suffix : {L"", L"temp", L".tmp"}) {
                wchar_t root[kPathCap], to[kPathCap];
                std::swprintf(root, kPathCap, L"%s\\settings.cfg%s", exe, suffix);
                std::swprintf(to, kPathCap, L"%s%s", settings_scratch, suffix);
                if (!AddRoot(root, to)) std::snprintf(why, sizeof(why), "appdir_settings: could not add %ls", root);
            }
        }
    } catch (...) {
        std::snprintf(why, sizeof(why), "exception while preparing the redirect");
    }
    if (!why[0]) InstallSaveHooks(why, sizeof(why));
    if (!why[0] && g_rd_hooks.load() == 0) std::snprintf(why, sizeof(why), "no hooks installed");
    if (!why[0]) {
        g_rd_active.store(true, std::memory_order_release);
        // Self-test through the hooks: the probe exists only in the scratch copy.
        for (int i = 0; !why[0] && i < g_nsave_roots; ++i) {
            wchar_t probe[kPathCap + 64];
            std::swprintf(probe, kPathCap + 64, L"%s\\%s", g_roots[i], saves::kProbeName);
            if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES) {
                std::snprintf(why, sizeof(why), "self-test: GetFileAttributesW(<root %d>\\%ls) was not answered from the scratch copy", i,
                              saves::kProbeName);
                break;
            }
            HANDLE f = CreateFileW(probe, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) std::snprintf(why, sizeof(why), "self-test: CreateFileW(<root %d>\\probe) failed", i);
            else CloseHandle(f);
            char probe_a[kPathCap * 2];
            BOOL lossy = FALSE;
            if (!why[0] && WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, probe, -1, probe_a, sizeof(probe_a), nullptr, &lossy) > 0 && !lossy &&
                GetFileAttributesA(probe_a) == INVALID_FILE_ATTRIBUTES)
                std::snprintf(why, sizeof(why), "self-test: GetFileAttributesA(<root %d>\\probe) was not answered from the scratch copy", i);
        }
    }
    // A second game on this PC: only with the redirect proven (two games never share one save tree).
    if (!why[0] && req.second_instance) sandbox::InstallMutexRename(why, sizeof(why));
    if (why[0]) {
        g_rd_active.store(false);
        RefuseAndExit(why);
        return;
    }
    if (automation) {
        if (exit_trace) InstallExitTrace();
        InstallShellBlock();
    }
    // Saves of another account: only here, after the redirect is installed and self-tested
    // (hidden runs: in a supervised automation run; test saves: sandbox_foreign_saves=1).
    const bool want_foreign = automation ? foreign : req.sandbox_foreign;
    g_foreign_requested.store(want_foreign);
    if (automation) {
        switch (foreign_saves::Decide(foreign, true, !EnvVar(saves::kSupervisedEnv).empty(), g_rd_active.load())) {
            case foreign_saves::Decision::off: break;
            case foreign_saves::Decision::install: InstallForeignSaves(); break;
            case foreign_saves::Decision::refuse_no_automation:
                std::snprintf(g_foreign_note, sizeof(g_foreign_note), "NOT bypassed: not a supervised automation run");
                break;
            case foreign_saves::Decision::refuse_no_redirect:
                std::snprintf(g_foreign_note, sizeof(g_foreign_note), "NOT bypassed: the save redirect is not active");
                break;
        }
    } else if (sandbox::SandboxForeignAllowed(req.sandbox_foreign, g_rd_active.load())) {
        InstallForeignSaves();
    }
    wchar_t active[kPathCap + 64];
    std::swprintf(active, kPathCap + 64, L"%s\\%s", g_status_dir, automation ? saves::kActiveFile : sandbox::kActiveFile);
    char text[kPathCap * 6];
    char scratch[kPathCap * 3];
    Utf8(g_scratch, scratch, sizeof(scratch));
    int len = std::snprintf(text, sizeof(text), "%s=redirected\npid=%lu\nhooks=%d\nscratch=%s\n", automation ? "saves" : "sandbox",
                            GetCurrentProcessId(), g_rd_hooks.load(), scratch);
    for (int i = 0; i < g_nroots.load() && len > 0 && len < static_cast<int>(sizeof(text)) - 32; ++i) {
        char root[kPathCap * 3];
        Utf8(g_roots[i], root, sizeof(root));
        len += std::snprintf(text + len, sizeof(text) - len, "%s=%s\n", i < g_nsave_roots ? "root" : "file_root", root);
    }
    if (want_foreign && len > 0 && len < static_cast<int>(sizeof(text)) - 64)
        len += std::snprintf(text + len, sizeof(text) - len, "foreign_saves=%s\n", g_foreign_bypassed.load() ? "bypassed" : "refused");
    if (req.second_instance && len > 0 && len < static_cast<int>(sizeof(text)) - 64)
        len += std::snprintf(text + len, sizeof(text) - len, "second_instance=mutex_rename_installed\n");
    AppendRaw(active, text);
}

}  // namespace st::automation
