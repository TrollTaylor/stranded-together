// The cargo menu's Sam figure (docs/real-partner-body.md section 15).
//
// The on-foot cargo menu ("Cargo Management", the game's "subspace" scene) shows a second Sam: a
// DSPlayerAKukanEntity the subspace work object creates once and wakes in a room of the world while
// the menu is open. The subspace gear (backpack, pod, boots ...) and the carried cargo are entities of
// their own parented onto it; its body is its ArtParts. Under the self view a player who is not Sam
// gets their own character there too: real_body.cpp keeps the self body alive while the menu is
// open, poses and places it from this figure, and this module hides the figure's ArtParts (only the
// top-level parts: every other part is effectively hidden under them, while the game's per-frame
// per-part toggles only set each part's own asked-visible bit) and puts them back.
//
// Game thread only. Every read is fault-free; every game call is byte-checked once and SEH-guarded
// (a fault switches the calls off for the session).
#pragma once
#include <cstddef>
#include <cstdint>

namespace st::menufig {

// The subspace work object and its figure, as read this tick.
struct View {
    std::uintptr_t w = 0;    // [0x147BE79B0], the subspace work object (0x9D0 bytes)
    std::int32_t state = -1;  // w+0x14: 0 no player, 1 create, 2 idle (asleep), 3 teleport + wake, 4 fade in, 5 open, 6 close, 7 cleanup
    std::int32_t req = -1;    // w+0x1C: requested state
    std::int32_t kind = -1;   // w+0x08: the menu kind in force
    std::int32_t kind_next = -1;  // w+0x0C: the menu kind asked for (UI pages change it while open)
    std::uint8_t flags = 0;   // w+0x9C8: 0x10 open asked, 0x20 close asked, 8 paused
    std::uintptr_t player = 0;  // w+0x50: the DSPlayerEntity it serves
    std::uintptr_t e = 0;     // w+0x58: the figure (DSPlayerAKukanEntity), checked: vtable, weak ref w+0x30
    std::uint64_t e_flags = 0;
    bool awake = false;       // the figure's entity is not asleep
    std::uintptr_t sm = 0;    // e+0xB8 SkinnedModel (vtable and back pointer checked)
    std::uintptr_t skel = 0;  // its current pose's skeleton (77 joints, Sam's layout), 0 otherwise
    std::uintptr_t apd = 0, arr = 0;  // its ArtPartsData and part array
    std::int32_t parts = 0;
    double pos[3] = {};       // e+0xC8
};

// Byte checks (once, logged). `why` gets the first mismatch.
bool SitesOk() noexcept;
const char* Why() noexcept;
// Reads the work object and its figure. False when the sites do not check out or no work object
// exists; `v` then has what could be read (w 0 when none).
bool Read(View* v) noexcept;
// True while the menu is opening (state 3: the figure is teleported and woken) or the figure is shown in
// it (state 4 fade-in or 5 open, awake), its model and parts checked.
bool Open(const View& v) noexcept;
// Only kind 0 (Manage Cargo, opened from the map: the one kind run in hidden tests, kind in force and
// kind asked both 0). Every other kind keeps Sam: the terminal cargo screens (2, 5, 8), the private
// room's uniform and cap previews (6, 7: the figure shows what the player is choosing) ...
bool KindAllowed(const View& v) noexcept;

// The figure's top-level ArtParts hidden by us (what to put back).
struct Hidden {
    std::uintptr_t e = 0, apd = 0, arr = 0;
    std::int32_t parts = 0;
    unsigned top = 0;        // top-level parts
    unsigned was_shown = 0;  // of those, asked visible when we hid them
    bool all_shown = false;  // every top-level part was asked visible (restored with the game's own all-parts call)
    std::uint32_t idx[64] = {};  // the top-level parts that were asked visible (when not all were)
    std::uint32_t top_idx[64] = {};  // every top-level part (Rehide reads only these when top <= 64)
    unsigned rehides = 0;    // the game asked a top-level part visible again; hidden again
};
// Hides every top-level ArtPart of the figure (the game's top-level setter 0x1422B25F0(apd, 0, 0)
// inside the model update bracket, as ApplyMeshControl does for the player's Root). False (nothing
// changed) when the figure does not check out or a call faults.
bool Hide(const View& v, Hidden* h, char* why, std::size_t cap) noexcept;
// Re-hides when the game asked a top-level part visible again; returns the parts found shown.
unsigned Rehide(Hidden* h) noexcept;
// Puts back what Hide changed, only when the figure still checks out (its vtable, its ArtPartsData
// pointing back at it, the same part array); tried even after an earlier call faulted. `h` is cleared
// either way.
bool Restore(Hidden* h, char* why, std::size_t cap) noexcept;

// Debug: every part's flags (asked / effective visible), parent and child count; the top-level ones.
void LogParts(const View& v) noexcept;

}  // namespace st::menufig
