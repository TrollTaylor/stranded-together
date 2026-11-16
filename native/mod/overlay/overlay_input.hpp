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

void SetOpen(bool open) noexcept;
bool IsOpen() noexcept;

std::size_t Drain(Event* out, std::size_t max) noexcept;
// Cursor position in client pixels of the attached window.
bool CursorClientPos(float* x, float* y) noexcept;

Stats GetStats() noexcept;

}  // namespace st::overlay::input
