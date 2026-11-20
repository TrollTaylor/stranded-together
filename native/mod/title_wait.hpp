// The guest's title screen in the game's own menu (docs/coop-session.md "The guest's title screen"). While a host-held
// save session's guest waits at its title, the game's own title menu carries the session's state:
//
//   * NEW GAME's label (data-source key 0x06F2B2AF) reads "Waiting for <host> to start the game" / "Receiving <host>'s
//     save: 45%" / "Loading <host>'s save..." / ... (rules::TitleLabel);
//   * CONTINUE and LOAD GAME are hidden through the data source's "has a save" bool (0x7339FA72), the PS4->PS5 item
//     through its own bool (0x1A8722BC); OPTIONS, UPDATE INFO and CREDITS with the menu's SetVisible (by element name);
//   * pressing the label does nothing (OnNewGame / OnContinueGame / OnSelectLoadSlot return at once); EXIT GAME is
//     untouched and quits the game as always.
//
// Everything is done with the writes and calls the game's own title menu makes, from post-hooks on the menu's two
// per-frame updates (DSUITitleMenuDataSource::Update vt[5], DSUITitleMenuFunction::Update vt[14]): on the game's own
// menu thread, after the original, each guarded by a vtable check. When the session stops (or wants the game's menu
// back) the next update puts back what was changed: the saved labels and bools, the hidden items. Every address is
// byte-checked at start-up (kSites in title_wait.cpp); a mismatch leaves this off and the full-screen waiting panel
// (overlay CoopView) is used instead.
#pragma once
#include <cstddef>

namespace st::title_wait {

// Bootstrap thread (coop::Install, the guest of a host-held save session, MinHook initialised): byte checks and hooks.
// False with a one-line reason in `why` when off (the overlay's full-screen waiting panel is used then).
bool Install(char* why, std::size_t cap) noexcept;
bool Installed() noexcept;
// Game thread (coop Tick): whether the guest's title shows the session (`label` = NEW GAME's text, ASCII) or the game's
// own menu. Applied by the menu's next update. Logs each new label.
void Set(bool want, const char* label) noexcept;
// The relabelled menu is on screen: wanted, the label written into the current title page's data source and the
// title page's per-frame update seen within the last 1.5 s (now_ms on link::NowMsPrecise's clock).
bool Showing(double now_ms) noexcept;
// One line for the log / the hidden tests: applied state, the items hidden, the presses ignored.
void Describe(char* out, std::size_t cap) noexcept;

}  // namespace st::title_wait
