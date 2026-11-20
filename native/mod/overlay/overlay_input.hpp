// Input for the in-game debug menu: the toggle key, feeding ImGui, and keeping
// mouse/keyboard (and XInput pads) away from the game while the menu is open.
//
// How Death Stranding reads input (static analysis of ds.exe, see
// docs/debug-menu.md "Input"):
//   keyboard  a WH_KEYBOARD thread hook (SetWindowsHookExA at 0x1419B8250, proc
//             0x1419B9920) plus raw input (RegisterRawInputDevices usage 1/6)
//   mouse     raw input (usage 1/2, RIDEV_DEVNOTIFY, legacy messages still on),
//             read with GetRawInputData inside the window procedure 0x141A19BE0;
//             SetCursorPos / ClipCursor / ShowCursor for its own cursor handling
//   pads      XInputGetState: ds.exe LoadLibrary("xinput1_4.dll") + GetProcAddress at
//             0x1419B1C57 (function pointer at +0x16818 of its pad poller, called at
//             0x1419B1DC7), XINPUTUAP.dll (delay-loaded) only as the fallback; HID (DualSense)
// While the menu is open:
//   - our own WH_KEYBOARD hook on the window thread swallows every key at its first
//     sighting (HC_NOREMOVE in ds.exe's pump, see TakeToggles): the game's hook and
//     window never see them; the keys and their characters go to ImGui instead,
//   - a window-procedure subclass eats WM_INPUT and mouse/keyboard messages,
//   - SetCursorPos and ClipCursor calls are ignored (the game's last clip
//     request is applied when the menu closes), XInputGetState returns an idle pad,
//   - exception: the RELEASE of a key or mouse button that was already down when
//     the menu opened still reaches the game (hook, messages and raw input), so
//     nothing stays stuck "down" in the game after the menu closes.
// Closed menu: everything passes straight through (the toggle key excepted).
//
// Automation (hidden test runs, docs/hidden-testing.md), set before CreateApiHooks:
//   - XInputGetState answers ONLY the virtual pad on index 0 (SetVirtualPad);
//     real pads are reported disconnected, so a pad in the user's hands never
//     drives the hidden game,
//   - GetForegroundWindow answers the attached game window (a window on a
//     never-displayed desktop is never the system's foreground window),
//   - SetCursorPos / ClipCursor never reach the system (the cursor is the user's),
//   - focus loss does not close the menu.
#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace st::overlay::input {

enum class EventType : std::uint8_t { key, character, mouse_move, mouse_button, wheel };
struct Event {
    EventType type = EventType::key;
    bool down = false;
    bool extended = false;
    std::uint32_t vk = 0;  // key
    std::uint32_t ch = 0;  // character (UTF-16 unit)
    int button = 0;        // 0 left, 1 right, 2 middle, 3 X1, 4 X2
    float x = 0, y = 0;    // mouse_move: client pixels; wheel: x horizontal, y vertical (notches)
};

// Local two-instance test mode (SetLocalMode below).
enum class LocalMode : std::uint8_t { off, host, host_pad, guest };

struct Stats {
    bool subclassed = false;
    bool keyboard_hook = false;
    bool cursor_hooks = false;
    bool xinput_hook = false;
    unsigned long blocked_messages = 0;   // mouse/keyboard window messages kept from the game
    unsigned long blocked_raw_input = 0;  // WM_INPUT kept from the game
    unsigned long blocked_keys = 0;       // keys swallowed by our keyboard hook
    unsigned long characters = 0;         // text characters handed to ImGui
    unsigned long suppressed_clip = 0;
    unsigned long suppressed_cursor_pos = 0;
    unsigned long idle_pads = 0;          // XInputGetState answers replaced by an idle pad
    unsigned long toggles = 0;
    bool xinput14_hook = false;           // xinput1_4.dll!XInputGetState (what ds.exe calls)
    bool automation = false;
    unsigned long virtual_pad_reads = 0;  // XInputGetState answered with the virtual pad
    unsigned long isolated_pad_reads = 0; // automation: other indices / no virtual pad -> not connected
    unsigned long foreground_answers = 0; // automation: GetForegroundWindow -> the game window
    unsigned long activate_app_true = 0;      // automation: WM_ACTIVATEAPP(TRUE) seen by the game window
    unsigned long activate_app_rewritten = 0; // automation: WM_ACTIVATEAPP(FALSE) handed to the game as TRUE
    unsigned long activate_app_posted = 0;    // automation: WM_ACTIVATEAPP(TRUE) posted by AssertActive
    unsigned long vibration_kept = 0;         // automation: XInputSetState(0) answered here, never reaching a real pad
    bool xinput_set_hook = false;             // automation: XInputSetState hooked (xinput1_4 / XINPUTUAP)
    LocalMode local = LocalMode::off;         // local two-instance test mode
    unsigned long local_hidden_pad_reads = 0; // local host: XInput reads answered "not connected"
    unsigned long local_cursor_kept = 0;      // local: SetCursorPos/ClipCursor kept from the system (window not in front)
};

// Virtual XInput pad 0 (automation). `connected` stays true after a hold ends
// (idle pad) so the game never sees a pad disconnect in the middle of a run.
struct PadState {
    bool connected = false;
    std::uint16_t buttons = 0;  // XINPUT_GAMEPAD_* bits
    std::uint8_t lt = 0, rt = 0;
    std::int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
void SetAutomation(bool on) noexcept;  // before CreateApiHooks (adds the GetForegroundWindow hook)
bool Automation() noexcept;

// Local two-instance test (docs/sandbox-and-local-test.md): two visible games on one PC.
// Set before the overlay installs (CreateApiHooks / Attach). In every local mode the game
// window is told it stays active (WM_ACTIVATEAPP(FALSE) handed on as TRUE: ds.exe's
// app-active flag gates its pad reader), so the unfocused game keeps running and reading
// its pad; the cursor is only clipped/moved by the game whose window really is in the
// foreground (the guest never touches it).
//   host      keyboard + mouse; XInput pads read as disconnected (and get no rumble)
//   host_pad  keyboard + mouse + XInput pads (the user asked for a pad on the host too)
//   guest     XInput pads; GetForegroundWindow answers the game window (ds.exe clears its
//             keyboard state when the foreground window is not its own: harmless for a pad)
// Automation (hidden runs) wins over this.
void SetLocalMode(LocalMode mode) noexcept;
LocalMode GetLocalMode() noexcept;
// Two games side by side (session.ini local_window=left|right): when the overlay first attaches
// to the game window it gets a normal title bar and the left / right half of its monitor
// (16:9, SetWindowPos asynchronous: the window belongs to the game's thread). Works with
// automation too (hidden two-game runs place their windows on the private desktop).
enum class WindowSlot : std::uint8_t { none, left, right };
void SetWindowSlot(WindowSlot slot) noexcept;
void PlaceWindow(HWND hwnd) noexcept;   // Attach calls it once: posts PlaceMessage() to the window
UINT PlaceMessage() noexcept;            // handled by the subclass on the window's thread
void PlaceWindowNow(HWND hwnd) noexcept;
void SetVirtualPad(const PadState& pad) noexcept;  // any thread
// Automation: post WM_ACTIVATEAPP(TRUE) to the attached game window (any thread). ds.exe
// sets its app-active flag only from that message (window procedure 0x141A19BE0, case
// 0x1C -> platform+0x68) and its pad reader (FUN_1419B3090) returns an empty pad while the
// flag is clear: sticks and buttons are ignored in gameplay. The subclass also hands every
// WM_ACTIVATEAPP(FALSE) to the game as TRUE. False when not attached / not automation.
bool AssertActive() noexcept;
PadState VirtualPad() noexcept;

// Bootstrap thread: create (not enable) the process-wide API hooks:
// SetCursorPos, ClipCursor (user32), XInputGetState (xinput1_4.dll and XINPUTUAP.dll,
// optional) and, in automation, GetForegroundWindow and XInputSetState (the game's rumble
// for the virtual pad never reaches a real controller).
// Appends each created target to `targets`. False if a required one failed.
bool CreateApiHooks(void** targets, std::size_t* count, std::size_t cap) noexcept;
void RemoveApiHooks() noexcept;

void SetToggleKey(std::uint32_t vk) noexcept;
std::uint32_t ToggleKey() noexcept;

// Render thread, first sight of the game window: subclass it and put our
// keyboard hook on its thread. Idempotent for the same window.
bool Attach(HWND hwnd) noexcept;
void Detach() noexcept;
HWND Window() noexcept;

// Toggle presses since the last call (from the keyboard hook / window messages,
// or the key-state poll for a press neither reported).
//
// The toggle key and ds.exe's message pump: the pump (FUN_141A19350) peeks with PM_NOREMOVE before GetMessage, so every
// key reaches WH_KEYBOARD hooks first with HC_NOREMOVE, and a hook that eats it there removes the message (no HC_ACTION
// follows). Our hook handles each key at its first sighting, whatever the code (v0.19 toggled at HC_ACTION only and ate
// F9 at HC_NOREMOVE: the menu never opened in the real game).
unsigned TakeToggles() noexcept;
enum class ToggleSource : std::uint8_t { none, hook_noremove, hook_action, window_message, key_poll, test };
struct ToggleInfo {
    ToggleSource source = ToggleSource::none;  // the last toggle's
    unsigned long long tick = 0;               // GetTickCount64 when the key came
};
ToggleInfo LastToggle() noexcept;
const char* ToggleSourceName(ToggleSource s) noexcept;
// Render thread, every Present: a physical press of the toggle key (GetAsyncKeyState) while our window is really in
// front that neither the hook nor the window reported within 400 ms toggles from here (never in a hidden run).
void PollToggleFallback() noexcept;
// Requested by focus loss (WM_KILLFOCUS / WM_ACTIVATEAPP): close the menu.
enum class CloseReason : std::uint8_t { none, focus_lost, app_inactive };
CloseReason TakeCloseRequest() noexcept;
const char* CloseReasonText(CloseReason r) noexcept;
void RequestToggleForTest() noexcept;
// Runs our WH_KEYBOARD procedure as the system would (call it on the window's
// thread). Posted key messages never reach WH_KEYBOARD hooks, so the harness
// drives the hook through this.
LRESULT KeyboardProcForTest(int code, WPARAM vk, LPARAM lp) noexcept;
// Hidden runs (scenario `hookkey`): SendMessage this registered message to the game window with (vk, keystroke lParam);
// on the window's thread our hook runs in ds.exe's order (HC_NOREMOVE, then HC_ACTION only if not eaten; a key nobody
// ate is posted on as WM_KEYDOWN/UP). Answers 1 eaten at HC_NOREMOVE, 2 eaten at HC_ACTION, 3 passed to the game, 0 not
// a hidden run.
UINT HookKeyMessage() noexcept;
struct HookCounts {
    unsigned long noremove = 0, action = 0;  // our hook's calls by code
};
HookCounts GetHookCounts() noexcept;
// The window that really is in front (never the guest's hooked GetForegroundWindow answer) belongs to this process.
// Always false in a hidden run (the keyboard and the desktop are the user's).
bool RealForegroundIsOurs() noexcept;

// Mouse lock (mouse_lock.hpp; session.ini mouse_lock=1, the launcher's Local test). Set before Attach.
void SetMouseLock(bool on) noexcept;
bool MouseLockConfigured() noexcept;
void SetMouseLockOn(bool on) noexcept;  // the debug menu's switch (only with mouse_lock=1)
// Render thread, every Present: gathers the state, decides and clips / releases through the unhooked ClipCursor (a hidden
// run logs what it would do and touches nothing).
void MouseLockFrame() noexcept;
struct MouseLockStatus {
    bool configured = false, on = false, engaged = false, switch_enabled = false;
    std::uint32_t switch_vk = 0;
    const char* state = "-";  // the last decision, in words
    unsigned long clips = 0, releases = 0, replaced = 0, switches = 0;
};
MouseLockStatus GetMouseLockStatus() noexcept;
std::uint32_t SwitchKey() noexcept;  // F10, or F11 when the menu key is F10
// DllMain, DLL_PROCESS_DETACH (the game quits with its window still up, so WndProc never sees WM_NCDESTROY): our own clip
// does not outlive the game. No logging (loader lock).
void ReleaseMouseLockAtExit() noexcept;
// Pretends `vk` was physically down when the menu opened (call after opening).
void MarkHeldForTest(std::uint32_t vk) noexcept;
// Forgets what CaptureHeld saw physically down (the harness runs on a desktop someone may be using).
void ClearHeldForTest() noexcept;

void SetOpen(bool open) noexcept;
bool IsOpen() noexcept;
// The co-op waiting screen (host-held save session, docs/coop-session.md): the game's input is blocked exactly as
// for the open menu (keys, mouse, raw input, cursor, XInput), except pad A, Enter and Space, which still reach the
// game so a notice behind the screen (the offline-mode notice) can be dismissed; the mod's own gate refuses any world
// start the title menu would make. Any thread. Independent of the menu (closing the menu keeps this block).
void SetWaitBlock(bool on) noexcept;
bool WaitBlocked() noexcept;
// Riding together (ride_together.cpp; docs/real-partner-body.md section 24): while on, the local Sam is carried as a
// passenger. The pad answers keep the right stick (look) only (Start held beside a vehicle is the game's Dispose of
// Vehicle, Back its Like: never while carried); key presses of the movement / action keys
// (letters, digits, Space, Shift, Ctrl, arrows) are swallowed by our keyboard hook and dropped from raw input and the
// window messages (their releases pass, so nothing stays down); mouse button presses are dropped (raw packets with a press
// lose that packet's motion). The open menu and the waiting screen block as before. Any thread.
void SetRideCarry(bool on) noexcept;
bool RideCarry() noexcept;
// The XInput buttons of every pad (indices 0..3, OR'ed) as the game last read them, BEFORE any masking (automation: the
// virtual pad); an index the game has not read for `max_age_ms` counts as up. `index`: the first index with a button
// down, -1 none. (v0.19 read index 0 only.) Pads the game reads without XInput (a PlayStation pad through scePad, Steam
// Input, HID) never show here: ride_together reads the game's own pad state for those.
std::uint16_t RidePadButtons(unsigned max_age_ms, int* index = nullptr) noexcept;
// Per index: ms since the game last read it (999999 never); returns how many were read in the last second.
int RidePadAges(unsigned (&age_ms)[4]) noexcept;
// The ride key (default F, the game's RideAndGetOff binding) is down, as our keyboard hook saw it at either hook code
// (confirmed by the key's physical state; hidden runs: the `hookkey` command's press).
bool RideKeyDown() noexcept;
void SetRideKey(std::uint32_t vk) noexcept;
void RideCounters(unsigned long* pads, unsigned long* keys, unsigned long* raw) noexcept;

std::size_t Drain(Event* out, std::size_t max) noexcept;
// Cursor position in client pixels of the attached window.
bool CursorClientPos(float* x, float* y) noexcept;

Stats GetStats() noexcept;

}  // namespace st::overlay::input
