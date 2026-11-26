// The game's crash reporter vs the mod's SEH guards (round 27; docs/enemy-sync.md 16.9).
//
// ds.exe registers a FIRST vectored exception handler at boot (0x141A16E7E: AddVectoredExceptionHandler(1, 0x141A19B30)).
// Vectored handlers run before any frame's __try/__except, and this one writes error.txt and shows the game's crash
// dialog for every access violation, in-page error, illegal instruction, misalignment, array-bounds, privileged
// instruction and stack overflow: first chance, whether or not a frame would have handled it. So a mod read under
// __try that faults (a stale pointer the guard exists for) still ends the game: hidden run 20261126-135429 (the guest's
// OnHit detour, a speculative attacker +0x338 read) froze in that dialog. The mod's per-frame GuardedTick boundary is
// defeated the same way.
//
// The fix: a byte-checked detour on that handler. For an access violation or in-page error whose dispatch would reach a
// __try/__except scope in THIS module (a virtual unwind from the fault, at most 48 frames, looking for a covering
// __C_specific_handler / __GSHandlerCheck_SEH scope, either variant, whose filter is the literal EXCEPTION_EXECUTE_HANDLER), it returns EXCEPTION_CONTINUE_SEARCH without the
// report, so the mod's own __except runs exactly as written. Everything else (no covering scope of ours, any other
// exception code, a fault inside the unwind walk itself) goes to the game's handler unchanged. A real crash in mod code
// that no guard covers is still reported by the game: here, or by its unhandled-exception filter.
#pragma once

#include <cstdint>

namespace st::sehguard {

// DllMain, once, before the other hooks (any role but none). Logs "[seh] ..." with the outcome.
void Install() noexcept;
// Faults the detour let reach a mod __except (any thread; atomics).
struct Stats {
    bool installed = false;
    std::uint64_t absorbed = 0;   // left to a mod __except
    std::uint64_t reported = 0;   // passed to the game's handler (its report)
    std::uint64_t walk_faults = 0;
    std::uint64_t last_rva = 0;   // the last absorbed fault's rip: an rva in this module, or a ds.exe VA (0x14...)
    std::uint64_t last_caller = 0, last_caller2 = 0;  // its two callers (the same kinds of address)
    std::uint32_t last_code = 0;
};
Stats Get() noexcept;
// Game thread, once a frame (dllmain): logs "[seh] absorbed ..." when the count moved (at most once a second).
void LogIfChanged() noexcept;

}  // namespace st::sehguard
