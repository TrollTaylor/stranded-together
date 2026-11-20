// The host-held save session, game side (docs/coop-session.md; design milestone M1). Active only with
// session.ini host_save=1 for role host or guest; otherwise nothing here is installed.
//
// "The host holds the save, the guest never has the save." The host's game plays on the host's own saves; the guest's
// game runs on its redirected co-op folder (DllMain refuses it otherwise, sandbox.hpp) and waits on its title screen,
// whose own menu then shows only "Waiting for <host> to start the game" and Quit Game (title_wait.hpp; the full-screen
// waiting panel covers the boot screens and stands in when the game's menu cannot be used), until the host starts a
// world. Then:
//
//   host (game thread, hooks)   the title load / converted load / New Game / in-world load request (Retry, pause-menu
//                                Load) are detected; the slot is named through the game's own slot cache.
//   host (worker thread)        that slot's checkpoint.dat is read READ ONLY (save_xfer_io.cpp: two reads ~400 ms apart,
//                                equal SHA-256, up to 3 tries), validated and offered over the link's channel 4 (STX1).
//   guest (worker thread)        receives it into <co-op folder>\incoming\<sha>.part, checks the SHA-256 and the file,
//                                writes it as manualsave5 / manualsave6 (alternating) below the redirected folder only.
//   guest (game thread)          refreshes the game's slot cache, checks the slot exists, and loads it: at the title
//                                through the title load (the gate opened for that one call), in the world through the
//                                pause menu's Load.
//
// The guest's hard gate: the title menu's start flow, the load page's decision, the title load, the converted load and
// the new world are detoured; each returns without acting unless the mod's own load call is running (game thread).
// Every game address is byte-checked at start-up (kSites in coop_session.cpp); a mismatch turns the session off with
// a one-line reason in the log and on the overlay. Game calls run on the game thread under SEH.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"

namespace st::coop {

// Bootstrap thread, right after LoadConfig and BEFORE the overlay waits for the game's d3d12 (the title menu must never
// be reachable before the guest's gate is in): byte checks, hooks (MinHook), the worker thread, the first screen.
// No-op unless host_save=1 and the role is host or guest.
void Install(const Config& config) noexcept;
// A host-held save session is running on this game (installed, byte checks passed).
bool Active() noexcept;
// Why it is not ("" while Active): "host_save=0", the byte check / hook that failed, "stopped: ...", "not installed".
const char* OffReason() noexcept;
// host_save=0 for a host or a guest: nothing of the session runs, but the overlay says so (NoticeTick). The overlay is
// installed for it even with debug_menu=0 and name_tags=0.
bool PlainNoticeWanted() noexcept;
// Game thread, every tick (sync::Tick), with the co-op features this game has off ("" = none; player_sync.cpp
// FeaturesOff): with host_save=0 an amber banner "Co-op features OFF: <text>" at the title, while loading and for the
// first minute in the world (again after each return to the title), and the menu header's status line. A running
// session shows its own notices (SetVehicleNotice); nothing here then.
void NoticeTick(const char* features_off, bool local_in_world, double now_ms) noexcept;
// 1 host, 2 guest while a session is running (Active), else 0.
int SessionRole() noexcept;
// Game thread. This game is in the session's world: the host in its phase "world", the guest with the host's save loaded,
// its player stable and no load of its own running. `load_counter` = that world's load number (the host's counter; the
// guest: the loaded save's). False at the title, while loading, or without a session. vehicle_share.cpp asks the host for
// its vehicles each time the guest's counter changes.
bool WorldReady(std::uint32_t* load_counter) noexcept;
// The shared vehicles' notice in the co-op banner (any thread): shown until `until_ms` (the tick clock) while no session
// notice is. "" clears it.
void SetVehicleNotice(const char* text, bool warn, double until_ms) noexcept;
// Game thread, every tick (sync::Tick): the game's phase, the guest's load calls, the session events, the screens.
void Tick(double now_ms) noexcept;
// Everything Tick maintains, undone when Tick will never run again (the per-frame tick faulted, or Bootstrap stopped
// before the frame pump was hooked): the session OFF (the guest's gate open, the worker idle), the waiting screen and
// its input block gone, an amber banner "Co-op save session stopped: <reason>", one log line. Any thread; idempotent.
void EmergencyStop(const char* reason) noexcept;
// Debug action "Coop session" (debug_actions.cpp; game thread): 0 log the status, 1 <ms> hidden tests: drop every
// received datagram for ms (the link-loss scenario), 2 host: offer the newest save now, 3 guest: accept the teleport
// offer, 4 host: log the save slots; hidden tests: 5..9 (8 <0..3>: the waiting panel / input block / the game's own
// title menu back for a gate test).
bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept;

}  // namespace st::coop
