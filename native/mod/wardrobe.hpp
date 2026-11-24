// The private room's uniform menu (the wardrobe: subspace kind 6), read only (docs/real-partner-body.md section 40).
//
// The room menu's "uniform" entry (selection 3, 0x1427648E5) opens it through 0x142D1BD70: a flag (0x144C46990) is set and
// 0x14284CB30 copies Sam's outfit bytes (inventory +0x4BDC..+0x4BE1) into a staging copy (the suit colour as a float at
// 0x144C46988). Browsing writes the previewed category (0x144C46991: 3 = the suit colour, 8 = none) and value (0x144C46994);
// the figure's update colours the menu's Sam with it (0x14272EAAF). Confirming copies the cursor into the staging copy; the
// close 0x142D1BCE0 writes the WHOLE staging copy back into the inventory with no comparison (0x14283E62D) and clears the
// flag. So the inventory byte does not change when the colour picked is the one the menu opened on: only the close tells.
//
// Every address byte-checked once (SitesOk, logged); all reads fault-free; nothing is written here.
#pragma once
#include <cstddef>
#include <cstdint>

namespace st::wardrobe {

struct State {
    int open = -1;              // [0x144C46990]: 1 the uniform (or kind-7) menu is open, 0 closed, -1 unreadable
    int category = -1;          // [0x144C46991]: the previewed category (3 suit colour, 8 none)
    int value = -1;             // [0x144C46994] (u8): the previewed value
    int staging = -1;           // (int)[0x144C46988]: the suit colour the close will write (0..16; -1 not a colour)
};

bool SitesOk() noexcept;           // byte checks (lazy, once, logged)
const char* Why() noexcept;
bool Read(State* s) noexcept;      // false when the sites do not check out (s stays -1)
// Hidden tests only (the caller checks automation, redirected saves and the private room): the uniform menu opened with the
// call the room's own state 3 makes (0x142D1BD70: the flag, the staging copy, the UI event to [0x147C0D6C0]), on the game
// thread, SEH-guarded. False (nothing called) when the opener's bytes differ, the menu is open already or there is no UI
// object. `msg` says what happened.
bool TestOpen(char* msg, std::size_t cap) noexcept;
// The colour previewed on the menu's figure now (category 3, 0..16), else -1.
inline int PreviewColour(const State& s) { return s.open == 1 && s.category == 3 && s.value >= 0 && s.value <= 16 ? s.value : -1; }

}  // namespace st::wardrobe
