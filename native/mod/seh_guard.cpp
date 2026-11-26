#include "seh_guard.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "MinHook.h"
#include "log.hpp"
#include "partner_source.hpp"

namespace st::sehguard {
namespace {

namespace mem = partner::mem;

constexpr std::uintptr_t kImage = 0x140000000ull;
constexpr std::uintptr_t kVeh = 0x141A19B30;     // the game's first vectored handler (its crash report)
constexpr std::uintptr_t kVehReg = 0x141A16E7E;  // lea rdx,[rip+X] (X = kVeh); mov ecx,1; call [AddVectoredExceptionHandler]
constexpr int kMaxFrames = 48;

using VehFn = LONG(CALLBACK*)(EXCEPTION_POINTERS*);
VehFn g_orig = nullptr;
std::atomic<bool> g_installed{false};
DWORD64 g_base = 0, g_end = 0;  // this module
// this module's SEH language handlers: __C_specific_handler(_noexcept) and __GSHandlerCheck_SEH(_noexcept), deduplicated
PEXCEPTION_ROUTINE g_handlers[4] = {};
int g_nhandlers = 0;

std::atomic<std::uint64_t> g_absorbed{0}, g_reported{0}, g_walk_faults{0}, g_last_rva{0}, g_last_caller{0}, g_last_caller2{0};
std::atomic<std::uint32_t> g_last_code{0};
thread_local bool t_in_walk = false;

std::uintptr_t A(std::uintptr_t va) noexcept { return mem::Base() + (va - kImage); }

// ---------------------------------------------------------------- this module's SEH language handlers
// Four probes compiled into this module: a plain __try (__C_specific_handler) and one with a /GS buffer
// (__GSHandlerCheck_SEH when /GS is on), each once noexcept (under /EHsc the compiler then picks the _noexcept variant
// of the handler) and once not. Their UNWIND_INFO names the handler; a guard may use any of the four.
__declspec(noinline) int ProbePlain(volatile int* p) noexcept {
    __try {
        return *p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
__declspec(noinline) void FillBuf(char* b, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<char>(i);
}
__declspec(noinline) int ProbeGs(volatile int* p, std::size_t k) noexcept {
    char buf[64];
    FillBuf(buf, sizeof(buf));
    __try {
        return *p + buf[k & 63];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
__declspec(noinline) int ProbePlainX(volatile int* p) {
    __try {
        return *p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}
__declspec(noinline) int ProbeGsX(volatile int* p, std::size_t k) {
    char buf[64];
    FillBuf(buf, sizeof(buf));
    __try {
        return *p + buf[k & 63];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

bool IsOurHandler(PEXCEPTION_ROUTINE h) noexcept {
    for (int i = 0; i < g_nhandlers; ++i)
        if (g_handlers[i] == h) return true;
    return false;
}
void AddHandler(PEXCEPTION_ROUTINE h) noexcept {
    if (h && !IsOurHandler(h) && g_nhandlers < 4) g_handlers[g_nhandlers++] = h;
}

PEXCEPTION_ROUTINE HandlerOf(const void* fn) noexcept {
    DWORD64 ib = 0;
    const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(fn), &ib, nullptr);
    if (!f || ib != g_base) return nullptr;
    const auto* ui = reinterpret_cast<const std::uint8_t*>(ib + f->UnwindData);
    if (!((ui[0] >> 3) & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER))) return nullptr;
    const std::uint8_t codes = ui[2];
    std::uint32_t rva = 0;
    std::memcpy(&rva, ui + 4 + 2 * ((codes + 1u) & ~1u), 4);
    return rva ? reinterpret_cast<PEXCEPTION_ROUTINE>(ib + rva) : nullptr;
}

// ---------------------------------------------------------------- the walk
// 1: a __try/__except scope of this module covers the fault or a caller's call site (dispatch would reach it); 0: none
// within kMaxFrames (or a frame with no rip); -1: the walk itself faulted.
int Covered(const CONTEXT* in, DWORD64* callers) noexcept {
    __try {
        CONTEXT c;
        std::memcpy(&c, in, sizeof(c));
        for (int d = 0; d < kMaxFrames; ++d) {
            const DWORD64 pc = c.Rip;
            if (pc < 0x10000) return 0;
            if (d == 1 || d == 2) callers[d - 1] = pc;
            DWORD64 ib = 0;
            const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &ib, nullptr);
            if (!f) {  // a leaf (or code without unwind data): its return address is at rsp
                c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
                c.Rsp += 8;
                continue;
            }
            void* hdata = nullptr;
            DWORD64 frame = 0;
            const PEXCEPTION_ROUTINE h = RtlVirtualUnwind(UNW_FLAG_EHANDLER, ib, pc, f, &c, &hdata, &frame, nullptr);
            if (h && hdata && ib == g_base && IsOurHandler(h)) {
                const auto* st = static_cast<const SCOPE_TABLE_AMD64*>(hdata);
                const DWORD64 rel = pc - ib;
                const DWORD n = st->Count < 512 ? st->Count : 512;
                for (DWORD i = 0; i < n; ++i) {
                    const auto& s = st->ScopeRecord[i];
                    // only a literal __except(EXCEPTION_EXECUTE_HANDLER): the compiler stores HandlerAddress 1 for it
                    // (a __finally has JumpTarget 0; a filter function, like the CRT's thread_start or
                    // dllmain_dispatch scopes, may decline the fault, so it stays the game's to report)
                    if (s.JumpTarget && s.HandlerAddress == EXCEPTION_EXECUTE_HANDLER && rel >= s.BeginAddress &&
                        rel < s.EndAddress)
                        return 1;
                }
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// an rva in this module, a ds.exe VA (0x14...), or the raw address
DWORD64 Where(DWORD64 a) noexcept {
    if (a >= g_base && a < g_end) return a - g_base;
    const DWORD64 b = mem::Base();
    return b && a >= b && a - b < 0x10000000ull ? a - b + kImage : a;
}

LONG CALLBACK H_Veh(EXCEPTION_POINTERS* ep) {
    // a fault inside Covered: let its own __except have it (the outer call then reports the original fault)
    if (t_in_walk) return EXCEPTION_CONTINUE_SEARCH;
    if (ep && ep->ExceptionRecord && ep->ContextRecord) {
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
            !(ep->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) {
            t_in_walk = true;
            DWORD64 callers[2] = {0, 0};
            const int r = Covered(ep->ContextRecord, callers);
            t_in_walk = false;
            if (r == 1) {
                const DWORD64 rip = ep->ContextRecord->Rip;
                g_last_rva.store(Where(rip), std::memory_order_relaxed);
                g_last_caller.store(Where(callers[0]), std::memory_order_relaxed);
                g_last_caller2.store(Where(callers[1]), std::memory_order_relaxed);
                g_last_code.store(code, std::memory_order_relaxed);
                g_absorbed.fetch_add(1, std::memory_order_relaxed);
                return EXCEPTION_CONTINUE_SEARCH;  // the dispatch goes on to the frames: the mod's __except runs
            }
            if (r < 0) g_walk_faults.fetch_add(1, std::memory_order_relaxed);
            g_reported.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return g_orig(ep);
}

bool BytesAt(std::uintptr_t va, const char* hex) noexcept {
    std::uint8_t want[64], mask[64], got[64];
    std::size_t n = 0;
    for (const char* p = hex; *p && n < sizeof(want);) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?') {
            want[n] = 0, mask[n++] = 0;
            p += (p[1] == '?') ? 2 : 1;
            continue;
        }
        auto hx = [](char ch) { return ch >= 'a' ? ch - 'a' + 10 : ch >= 'A' ? ch - 'A' + 10 : ch - '0'; };
        want[n] = static_cast<std::uint8_t>(hx(p[0]) * 16 + hx(p[1]));
        mask[n++] = 0xFF;
        p += 2;
    }
    if (!mem::Read(A(va), got, n)) return false;
    for (std::size_t i = 0; i < n; ++i)
        if ((got[i] & mask[i]) != want[i]) return false;
    return true;
}

std::uint64_t g_logged = 0;
ULONGLONG g_log_ms = 0;

}  // namespace

static unsigned long long Rva(int i) noexcept {
    return i < g_nhandlers ? static_cast<unsigned long long>(reinterpret_cast<DWORD64>(g_handlers[i]) - g_base) : 0ull;
}

void Install() noexcept {
    if (g_installed.load()) return;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&Install), &self) ||
        !self) {
        Log("[seh] OFF: this module's handle is unknown (a fault inside a mod guard still ends the game)");
        return;
    }
    g_base = reinterpret_cast<DWORD64>(self);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    g_end = g_base + nt->OptionalHeader.SizeOfImage;
    AddHandler(HandlerOf(reinterpret_cast<const void*>(&ProbePlain)));
    AddHandler(HandlerOf(reinterpret_cast<const void*>(&ProbeGs)));
    AddHandler(HandlerOf(reinterpret_cast<const void*>(&ProbePlainX)));
    AddHandler(HandlerOf(reinterpret_cast<const void*>(&ProbeGsX)));
    if (!g_nhandlers) {
        Log("[seh] OFF: this module's __C_specific_handler was not found (a fault inside a mod guard still ends the game)");
        return;
    }
    // the handler's prologue, and the boot code that registers it first in the chain (rip target checked)
    std::int32_t rel = 0;
    const bool body = BytesAt(kVeh, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 E8");
    const bool reg = BytesAt(kVehReg, "48 8D 15 ?? ?? ?? ?? B9 01 00 00 00 FF 15") && mem::Read(A(kVehReg + 3), &rel, 4) &&
                     kVehReg + 7 + static_cast<std::intptr_t>(rel) == kVeh;
    if (!body || !reg) {
        Log("[seh] OFF: the game's crash reporter differs (byte check failed: %s); a fault inside a mod guard still ends the game",
            !body ? "its handler 0x141A19B30" : "its registration 0x141A16E7E");
        return;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        Log("[seh] OFF: MinHook init failed (%s)", MH_StatusToString(init));
        return;
    }
    void* target = reinterpret_cast<void*>(A(kVeh));
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&H_Veh), reinterpret_cast<void**>(&g_orig));
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        Log("[seh] OFF: the detour on the game's crash reporter failed (%s)", MH_StatusToString(s));
        return;
    }
    g_installed.store(true);
    Log("[seh] the game's crash reporter (0x141A19B30, first vectored handler): an access violation that a __try/__except of the mod "
        "covers goes to that guard (%d handlers, rva %llX %llX %llX %llX); every other fault is reported as before",
        g_nhandlers, Rva(0), Rva(1), Rva(2), Rva(3));
}

Stats Get() noexcept {
    Stats s;
    s.installed = g_installed.load();
    s.absorbed = g_absorbed.load(std::memory_order_relaxed);
    s.reported = g_reported.load(std::memory_order_relaxed);
    s.walk_faults = g_walk_faults.load(std::memory_order_relaxed);
    s.last_rva = g_last_rva.load(std::memory_order_relaxed);
    s.last_caller = g_last_caller.load(std::memory_order_relaxed);
    s.last_caller2 = g_last_caller2.load(std::memory_order_relaxed);
    s.last_code = g_last_code.load(std::memory_order_relaxed);
    return s;
}

void LogIfChanged() noexcept {
    const std::uint64_t n = g_absorbed.load(std::memory_order_relaxed);
    if (n == g_logged) return;
    const ULONGLONG now = GetTickCount64();
    if (g_log_ms && now - g_log_ms < 1000) return;
    g_log_ms = now;
    g_logged = n;
    const Stats s = Get();
    auto where = [](std::uint64_t a) { return a < 0x10000000ull ? "mod rva" : a < 0x150000000ull ? "ds.exe" : "at"; };
    Log("[seh] absorbed %llu fault(s) inside the mod's guards (last: code 0x%08X at %s 0x%llX, from %s 0x%llX, %s 0x%llX); passed to the "
        "game's report %llu, walk faults %llu",
        static_cast<unsigned long long>(s.absorbed), s.last_code, where(s.last_rva), static_cast<unsigned long long>(s.last_rva),
        where(s.last_caller), static_cast<unsigned long long>(s.last_caller), where(s.last_caller2),
        static_cast<unsigned long long>(s.last_caller2),
        static_cast<unsigned long long>(s.reported), static_cast<unsigned long long>(s.walk_faults));
}

}  // namespace st::sehguard
