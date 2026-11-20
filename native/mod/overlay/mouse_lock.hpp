// The mouse lock (session.ini mouse_lock=1; the launcher writes it for the Local test, Debug page "Lock the mouse to the
// focused game window"). Two windowed games side by side on one PC: without it the cursor walks out of the game you play
// into the other one (ds.exe clips the cursor only in gameplay, to one pixel at the window centre, and frees it in every
// menu, the title, the map and after any focus change). With it, the cursor stays inside the client area of the game
// window that really is in front; Alt+Tab, the Win key, minimising or a title-bar drag leave it, and the switch key (F10,
// or F11 when the menu key is F10) moves to the other game's window.
//
// This file is the pure decision (no Win32 calls; tests/overlay_tests.cpp checks every rule). overlay_input.cpp gathers
// the state every Present and on the window messages, and acts through the unhooked ClipCursor. Hidden runs (automation)
// never clip, move or activate anything: the cursor belongs to the user's desktop; they log what they would do.
#pragma once
#include <windows.h>

#include <cstdint>

namespace st::overlay::mouselock {

struct State {
    bool enabled = false;     // mouse_lock=1 and the debug menu's switch on
    bool automation = false;  // hidden run: never touch the real cursor
    bool foreground = false;  // the REAL foreground window is this game's (never the guest's hooked answer)
    bool iconic = false;      // minimised
    bool size_move = false;   // between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE (title-bar drag, resize)
    bool engaged = false;     // since we came to the front the cursor was inside the client area (cleared on focus loss)
    RECT client{};            // the client area in screen coordinates
    bool have_clip = false;   // a clip is in effect (GetClipCursor answered something smaller than the virtual screen)
    RECT clip{};
    bool ours = false;        // the clip in effect is the rect we set last
};

enum class Act : std::uint8_t { keep, clip, release };
enum class Why : std::uint8_t {
    off,          // the lock is off (released if the clip is ours)
    automation,   // hidden run: nothing applied
    not_front,    // another window is in front
    minimised,
    moving,       // title-bar drag / resize
    no_client,    // empty client area
    not_engaged,  // in front, but the cursor has not been inside the window yet
    game_clip,    // the game's own tighter clip (gameplay: one pixel at the window centre) is inside the window
    held,         // our clip to the window is in effect
    clipping,     // clip to the client area now
};

struct Decision {
    Act act = Act::keep;
    Why why = Why::off;
    RECT rect{};  // Act::clip: the rect to clip to
};

inline bool EmptyRect(const RECT& r) noexcept { return r.right <= r.left || r.bottom <= r.top; }
inline bool SameRect(const RECT& a, const RECT& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
inline bool RectInside(const RECT& inner, const RECT& outer) noexcept {
    return !EmptyRect(inner) && inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom;
}

// Rules, in order:
//  1 off -> release only our own clip
//  2 automation -> keep (the caller logs what it would do)
//  3 not in front / minimised / moving / empty client -> release only our own clip (never a clip someone else set)
//  4 not engaged -> keep (activating by the taskbar or the title bar does not grab the cursor)
//  5 the clip in effect lies inside the client area (the game's one-pixel centre clip, or ours) -> keep
//  6 otherwise -> clip to the client area
Decision Decide(const State& s) noexcept;
const char* WhyText(Why why) noexcept;

// What the game's own ClipCursor(nullptr) (its Suspend on focus loss, every menu, the map) becomes while the lock holds:
// true = replace it with the client area. The game's non-null clips are not changed.
bool ReplaceGameRelease(const State& s) noexcept;

// The key that switches to the other game window: F10, or F11 when the debug menu key is F10.
std::uint32_t SwitchKey(std::uint32_t menu_vk) noexcept;

}  // namespace st::overlay::mouselock
