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
//   - our own WH_KEYBOARD hook on the window thread (installed after the game's,
//     so it runs first) swallows every key: the game's hook and window never
//     see them; the keys and their characters go to ImGui instead,
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
// or GetAsyncKeyState polling when neither saw the key).
unsigned TakeToggles() noexcept;
void PollToggleFallback() noexcept;
// Requested by focus loss (WM_KILLFOCUS / WM_ACTIVATEAPP): close the menu.
bool TakeCloseRequest() noexcept;
void RequestToggleForTest() noexcept;
// Runs our WH_KEYBOARD procedure as the system would (call it on the window's
// thread). Posted key messages never reach WH_KEYBOARD hooks, so the harness
// drives the hook through this.
LRESULT KeyboardProcForTest(int code, WPARAM vk, LPARAM lp) noexcept;
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

std::size_t Drain(Event* out, std::size_t max) noexcept;
// Cursor position in client pixels of the attached window.
bool CursorClientPos(float* x, float* y) noexcept;

Stats GetStats() noexcept;

}  // namespace st::overlay::input
