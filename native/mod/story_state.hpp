// Story tools (docs/dev-tools.md "Story tools"): the game's story state read and logged (episode, the missions in progress,
// what is done), shown on the Dev tools page, and "go to a story point": a save of the story library loaded in a sandboxed
// session. Nothing here writes a fact or a mission state (the research found no safe way to start or skip a story mission:
// docs/dev-tools.md "Why no mission jump").
//
// Threads: Tick and Command on the game thread (dev::Tick, the debug actions); GetView / GetSummary on any thread.
// Every game address is byte-checked on the first tick; a mismatch turns the probe (or the load) off with one log line and
// the reason on the page. The two game reads (the FactDatabase int getter and the mission info lookup) are pure lookups,
// called only behind the in-world gate after every pointer was checked; SEH-wrapped as a last resort only.
#pragma once
#include <cstddef>
#include <cstdint>

#include "story_rules.hpp"

namespace st::story {

// dev::Tick (game thread), every tick: the byte checks (once), the probe (1 Hz while in a world), the slot list, a pending load.
void Tick(double now_ms, bool enabled, const devrules::Session& session) noexcept;

// The debug actions (docs/debug-actions.md "Story tools"):
//   story       v0 = 0 the summary to the log, 1 every main / sub mission with its state, 2 the story-point slots,
//               3 whether story_load is allowed in this session (and why not)
//   story_load  v0 = manual slot (the library's are 10..19): the game's own Load of manualsave<v0>, sandboxed saves only
enum class Op : std::uint8_t { show, load };
bool Command(Op op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

// For future story sync: the newest summary read in this world (valid = false outside a world).
storyrules::Summary GetSummary() noexcept;

// What the Dev tools page shows (a copy; published by Tick).
struct Slot {
    int index = -1;            // manualsave<index>
    char title[96] = {};
    char detail[96] = {};
};
struct View {
    std::uint64_t serial = 0;
    bool probe_ok = false;     // the probe's byte checks passed
    bool load_ok = false;      // the load's byte checks passed
    char off[160] = {};        // what is off and why ("" = all on)
    storyrules::Summary summary;
    char line[320] = {};       // FormatSummary of `summary`
    bool load_allowed = false;
    char load_why[200] = {};   // why story_load is refused here
    char load_state[160] = {}; // the last / pending load
    int slot_count = 0;
    static constexpr int kMaxSlots = storyrules::kMaxManualSlot + 1;  // every manual slot of the sandbox
    Slot slots[kMaxSlots];
};
View GetView() noexcept;

}  // namespace st::story
