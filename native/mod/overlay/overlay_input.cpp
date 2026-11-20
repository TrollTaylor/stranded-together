// See overlay_input.hpp for what is blocked and why.
#include "overlay_input.hpp"

#include <windowsx.h>
#include <xinput.h>

#include <atomic>
#include <cstring>

#include "MinHook.h"
#include "log.hpp"

namespace st::overlay::input {
namespace {

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using GetForegroundWindowFn = HWND(WINAPI*)();

std::atomic<bool> g_open{false};
// The co-op waiting screen (SetWaitBlock; coop_session.cpp): blocked like the open menu, except the confirm inputs.
std::atomic<bool> g_wait{false};
// Riding together (ride_together.cpp, docs/real-partner-body.md section 24): while the local Sam is carried as a passenger,
// movement and actions are kept from the game (the pad keeps its right stick only; key downs of the movement and
// action keys are swallowed, releases pass; mouse button presses are dropped), look stays. The pad buttons and the ride
// key are recorded before any masking, so the mod sees the hold that gets off.
std::atomic<bool> g_carry{false};
std::atomic<std::uint16_t> g_ride_buttons{0};
std::atomic<std::uint64_t> g_ride_buttons_tick{0};
std::atomic<std::uint32_t> g_ride_vk{'F'};
std::atomic<bool> g_ride_key{false};
std::atomic<unsigned long> g_carry_pads{0}, g_carry_keys{0}, g_carry_raw{0};
std::atomic<std::uint32_t> g_toggle_vk{0x78};
std::atomic<unsigned> g_toggles{0};
std::atomic<bool> g_close_request{false};
std::atomic<bool> g_toggle_seen_by_messages{false};  // the hook/WndProc saw the key at least once
bool g_poll_prev = false;

// Window + hooks
std::atomic<HWND> g_hwnd{nullptr};
std::atomic<WNDPROC> g_prev_proc{nullptr};
HHOOK g_kb_hook = nullptr;
std::atomic<bool> g_kb_hook_on{false};

// API hooks
void* g_setcursorpos_target = nullptr;
void* g_clipcursor_target = nullptr;
void* g_xinput_target = nullptr;
void* g_xinput14_target = nullptr;
void* g_foreground_target = nullptr;
void* g_xinput_set_target = nullptr;    // automation only
void* g_xinput14_set_target = nullptr;  // automation only
SetCursorPosFn g_orig_setcursorpos = nullptr;
ClipCursorFn g_orig_clipcursor = nullptr;
XInputGetStateFn g_orig_xinput = nullptr;
XInputGetStateFn g_orig_xinput14 = nullptr;
GetForegroundWindowFn g_orig_foreground = nullptr;
XInputSetStateFn g_orig_xinput_set = nullptr;
XInputSetStateFn g_orig_xinput14_set = nullptr;

// Automation (hidden test runs)
std::atomic<bool> g_automation{false};
// Local two-instance test (SetLocalMode, SetWindowSlot)
std::atomic<LocalMode> g_local{LocalMode::off};
std::atomic<WindowSlot> g_window_slot{WindowSlot::none};
std::atomic<bool> g_window_placed{false};
// The placed window's size and the monitor it was placed on (window thread only).
std::atomic<bool> g_window_kept{false};
int g_keep_w = 0, g_keep_h = 0;
RECT g_keep_monitor{};
std::atomic<unsigned long> g_window_reverts{0};
std::atomic<unsigned long> g_local_hidden_pads{0}, g_local_cursor_kept{0}, g_local_activate_rewritten{0};
SRWLOCK g_pad_lock = SRWLOCK_INIT;
PadState g_pad;
DWORD g_pad_packet = 1;

// Cursor clip bookkeeping (SetOpen on the render thread, ClipCursor hook on any thread)
SRWLOCK g_clip_lock = SRWLOCK_INIT;
bool g_clip_saved = false;
bool g_clip_was_free = true;
RECT g_clip_rect{};
bool g_game_clip_set = false;   // the game called ClipCursor while the menu was open
bool g_game_clip_null = true;
RECT g_game_clip{};

// Keys/buttons physically down when the menu opened. Their releases still reach
// the game (raw input, legacy messages, keyboard hook): otherwise a key held
// while opening the menu (W, a mouse button) stays "down" in the game's own input
// state after the menu closes. Written on open (render thread), read on the
// window thread.
std::atomic<std::uint64_t> g_held[4];
std::atomic<bool> g_mouse_held{false};

bool Held(std::uint32_t vk) noexcept {
    return vk < 256 && (g_held[vk >> 6].load(std::memory_order_relaxed) >> (vk & 63) & 1u) != 0;
}
void MarkHeld(std::uint32_t vk) noexcept {
    if (vk >= 256) return;
    g_held[vk >> 6].fetch_or(std::uint64_t{1} << (vk & 63), std::memory_order_relaxed);
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
        g_mouse_held.store(true, std::memory_order_relaxed);
}
void ClearHeld() noexcept {
    for (auto& h : g_held) h.store(0, std::memory_order_relaxed);
    g_mouse_held.store(false, std::memory_order_relaxed);
}
void CaptureHeld(std::uint32_t toggle) noexcept {
    ClearHeld();
    for (std::uint32_t vk = 1; vk < 255; ++vk)
        if (vk != toggle && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000)) MarkHeld(vk);
}

// WM_INPUT that only releases something held since before the menu opened.
// Reading the packet does not consume it: the game's own GetRawInputData still
// gets it when we pass the message on.
bool IsHeldRelease(LPARAM lp) noexcept {
    RAWINPUT ri;
    UINT size = sizeof(ri);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return false;  // HID packets (bigger) and bad handles: blocked
    if (ri.header.dwType == RIM_TYPEKEYBOARD)
        return (ri.data.keyboard.Flags & RI_KEY_BREAK) != 0 && Held(ri.data.keyboard.VKey);
    if (ri.header.dwType == RIM_TYPEMOUSE && g_mouse_held.load(std::memory_order_relaxed)) {
        const USHORT f = ri.data.mouse.usButtonFlags;
        constexpr USHORT kUp = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP |
                               RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
        return (f & kUp) != 0 && (f & ~kUp) == 0;  // releases only: no press, no wheel
    }
    return false;
}

// Carried: the keys of the game's default movement / action bindings (bindings.cfg) are kept from the game; Escape, the
// function keys, Tab, Alt (Alt+F4) and the rest pass.
bool CarryBlocksKey(std::uint32_t vk) noexcept {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return true;
    switch (vk) {
        case VK_SPACE: case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
            return true;
        default:
            return false;
    }
}
bool Carrying() noexcept { return g_carry.load(std::memory_order_acquire) && !g_open.load(std::memory_order_acquire); }
// Carried: the pad buttons the game still gets. Only a tap of Start (its first 250 ms: the pause menu opens, while a hold,
// the game's "Dispose of Vehicle" beside a vehicle, never reaches the game); every other button reads idle. (Riding
// together lifts the whole block while Sam is asleep, so the game's menu then gets the pad as usual.)
constexpr ULONGLONG kCarryStartTapMs = 250;
std::atomic<ULONGLONG> g_carry_start_since{0};
WORD CarryPassButtons(WORD buttons) noexcept {
    if (!(buttons & XINPUT_GAMEPAD_START)) {
        g_carry_start_since.store(0, std::memory_order_relaxed);
        return 0;
    }
    const ULONGLONG now = GetTickCount64();
    ULONGLONG since = g_carry_start_since.load(std::memory_order_relaxed);
    if (!since) g_carry_start_since.store(since = now, std::memory_order_relaxed);
    return now - since <= kCarryStartTapMs ? static_cast<WORD>(XINPUT_GAMEPAD_START) : static_cast<WORD>(0);
}
// Carried: a raw keyboard press of a blocked key, or a raw mouse packet with a button press, is dropped (releases pass).
bool CarryDropsRaw(LPARAM lp) noexcept {
    RAWINPUT ri;
    UINT size = sizeof(ri);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return false;
    if (ri.header.dwType == RIM_TYPEKEYBOARD) return (ri.data.keyboard.Flags & RI_KEY_BREAK) == 0 && CarryBlocksKey(ri.data.keyboard.VKey);
    if (ri.header.dwType == RIM_TYPEMOUSE) {
        constexpr USHORT kDown = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN |
                                 RI_MOUSE_BUTTON_5_DOWN;
        return (ri.data.mouse.usButtonFlags & kDown) != 0;
    }
    return false;
}

bool IsButtonUp(UINT m) noexcept { return m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP || m == WM_XBUTTONUP; }

// The game's input is kept away while the menu is open or the co-op waiting screen is up.
bool Blocking() noexcept { return g_open.load(std::memory_order_acquire) || g_wait.load(std::memory_order_acquire); }
// The waiting screen alone (menu closed): pad A, Enter and Space still reach the game, so a notice behind the screen
// (the offline-mode notice) can be dismissed; the co-op gate refuses any world start the title menu would make.
bool WaitOnly() noexcept { return g_wait.load(std::memory_order_acquire) && !g_open.load(std::memory_order_acquire); }
bool ConfirmKey(std::uint32_t vk) noexcept { return vk == VK_RETURN || vk == VK_SPACE; }
bool RawConfirmKey(LPARAM lp) noexcept {
    RAWINPUT ri;
    UINT size = sizeof(ri);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return false;
    return ri.header.dwType == RIM_TYPEKEYBOARD && ConfirmKey(ri.data.keyboard.VKey);
}

// Event ring
constexpr std::size_t kCap = 512;
SRWLOCK g_ev_lock = SRWLOCK_INIT;
Event g_events[kCap];
std::size_t g_ev_head = 0, g_ev_count = 0;

// Stats
std::atomic<unsigned long> g_blocked_messages{0}, g_blocked_raw{0}, g_blocked_keys{0}, g_supp_clip{0}, g_supp_pos{0},
    g_idle_pads{0}, g_toggle_count{0}, g_chars{0}, g_virtual_reads{0}, g_isolated_reads{0}, g_foreground_answers{0},
    g_activate_true{0}, g_activate_rewritten{0}, g_activate_posted{0}, g_focus_logged{0}, g_vibration_kept{0};

void Push(const Event& e) noexcept {
    AcquireSRWLockExclusive(&g_ev_lock);
    g_events[(g_ev_head + g_ev_count) % kCap] = e;
    if (g_ev_count < kCap) ++g_ev_count;
    else g_ev_head = (g_ev_head + 1) % kCap;
    ReleaseSRWLockExclusive(&g_ev_lock);
}

void ClearEvents() noexcept {
    AcquireSRWLockExclusive(&g_ev_lock);
    g_ev_head = g_ev_count = 0;
    ReleaseSRWLockExclusive(&g_ev_lock);
}

void Toggle() noexcept {
    g_toggles.fetch_add(1);
    g_toggle_count.fetch_add(1);
}

void PushKey(std::uint32_t vk, bool down, bool extended) noexcept {
    Event e;
    e.type = EventType::key;
    e.vk = vk;
    e.down = down;
    e.extended = extended;
    Push(e);
}

void PushChar(std::uint32_t ch) noexcept {
    if (ch < 0x20 || ch == 0x7F) return;  // control characters are keys, not text
    Event e;
    e.type = EventType::character;
    e.ch = ch;
    Push(e);
    g_chars.fetch_add(1, std::memory_order_relaxed);
}

void PushButton(int button, bool down) noexcept {
    Event e;
    e.type = EventType::mouse_button;
    e.button = button;
    e.down = down;
    Push(e);
}

// Characters for a key swallowed by our keyboard hook (no WM_CHAR is generated
// for it). Runs on the window thread. Modifier state comes from the physical
// keys: the thread's own key state never saw the swallowed modifier presses.
void PushCharsFor(std::uint32_t vk, LPARAM lp) noexcept {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    if (ctrl && !alt) return;  // shortcuts (Ctrl+C...) are keys; Ctrl+Alt is AltGr text
    BYTE keys[256] = {};
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) keys[VK_SHIFT] = keys[VK_LSHIFT] = 0x80;
    if (ctrl) keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
    if (alt) keys[VK_MENU] = keys[VK_RMENU] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) keys[VK_CAPITAL] = 0x01;
    keys[vk & 0xFF] |= 0x80;
    wchar_t text[8] = {};
    const UINT scan = static_cast<UINT>((lp >> 16) & 0xFF);
    // Flag 4: do not change the keyboard state (dead keys stay intact for the game).
    const int n = ToUnicodeEx(vk, scan, keys, text, 8, 4, GetKeyboardLayout(0));
    for (int i = 0; i < n && i < 8; ++i) PushChar(text[i]);
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code < 0) return CallNextHookEx(nullptr, code, wp, lp);
    const std::uint32_t vk = static_cast<std::uint32_t>(wp);
    const bool up = (lp & (1LL << 31)) != 0;
    const bool was_down = (lp & (1LL << 30)) != 0;
    const bool alt = (lp & (1LL << 29)) != 0;
    const bool extended = (lp & (1LL << 24)) != 0;
    if (vk == g_toggle_vk.load(std::memory_order_relaxed)) {
        g_toggle_seen_by_messages.store(true, std::memory_order_relaxed);
        if (code == HC_ACTION && !up && !was_down) Toggle();
        return 1;  // the toggle key never reaches the game
    }
    if (code == HC_ACTION && vk == g_ride_vk.load(std::memory_order_relaxed)) g_ride_key.store(!up, std::memory_order_relaxed);
    if (!Blocking() && Carrying() && !up && CarryBlocksKey(vk)) {
        if (code == HC_ACTION) g_carry_keys.fetch_add(1, std::memory_order_relaxed);
        return 1;  // carried: the key's press never reaches the game (its release does)
    }
    if (Blocking()) {
        if (vk == VK_F4 && alt) return CallNextHookEx(nullptr, code, wp, lp);  // Alt+F4 still closes the game
        if (WaitOnly() && ConfirmKey(vk)) return CallNextHookEx(nullptr, code, wp, lp);  // waiting screen: confirm
        if (up && Held(vk)) {  // releasing a key held since before the menu opened
            if (code == HC_ACTION) PushKey(vk, false, extended);
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        if (code == HC_ACTION) {
            PushKey(vk, !up, extended);
            if (!up) PushCharsFor(vk, lp);
            g_blocked_keys.fetch_add(1, std::memory_order_relaxed);
        }
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

bool IsMouseMessage(UINT m) noexcept { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }
bool IsKeyMessage(UINT m) noexcept { return m >= WM_KEYFIRST && m <= WM_KEYLAST; }

LRESULT CALLBACK WndProc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
    const WNDPROC prev = g_prev_proc.load(std::memory_order_acquire);
    const auto pass = [&]() -> LRESULT {
        return prev ? CallWindowProcW(prev, hwnd, m, wp, lp) : DefWindowProcW(hwnd, m, wp, lp);
    };

    if (m != 0 && m == PlaceMessage()) {
        PlaceWindowNow(hwnd);
        return 0;
    }
    if (g_window_kept.load(std::memory_order_relaxed)) {
        // ds.exe puts its window back to the whole monitor (borderless) after it set up its
        // swapchain (hidden run 20261116-162418: both windows 2560x1440 at 0,0 again). Keep the
        // placed size and title bar; a move by the user (other position, same size) still works.
        if (m == WM_WINDOWPOSCHANGING && lp) {
            auto* wpos = reinterpret_cast<WINDOWPOS*>(lp);
            if (!(wpos->flags & SWP_NOSIZE) && (wpos->cx != g_keep_w || wpos->cy != g_keep_h)) {
                const bool fullscreen_request = wpos->cx >= g_keep_monitor.right - g_keep_monitor.left &&
                                                wpos->cy >= g_keep_monitor.bottom - g_keep_monitor.top;
                wpos->cx = g_keep_w;
                wpos->cy = g_keep_h;
                if (fullscreen_request) wpos->flags |= SWP_NOMOVE;  // stay where it was placed
                if (g_window_reverts.fetch_add(1, std::memory_order_relaxed) < 4)
                    Log("[overlay] local test: kept the game window at %dx%d (the game asked for a full-monitor window)", g_keep_w, g_keep_h);
            }
        } else if (m == WM_STYLECHANGING && wp == static_cast<WPARAM>(GWL_STYLE) && lp) {
            auto* ss = reinterpret_cast<STYLESTRUCT*>(lp);
            ss->styleNew = (ss->styleNew & ~static_cast<DWORD>(WS_POPUP | WS_MAXIMIZE)) | WS_OVERLAPPEDWINDOW;
        }
    }
    if (m == WM_SIZE && g_window_slot.load(std::memory_order_relaxed) != WindowSlot::none) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 6)
            Log("[overlay] local test: game window client size now %u x %u", static_cast<unsigned>(LOWORD(lp)), static_cast<unsigned>(HIWORD(lp)));
    }
    if (m == WM_NCDESTROY) {
        // The window is going away: unhook ourselves (only if still on top).
        if (prev && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) == &WndProc)
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(prev));
        g_hwnd.store(nullptr);
        g_open.store(false);
        return pass();
    }
    if (g_automation.load(std::memory_order_relaxed) &&
        (m == WM_ACTIVATEAPP || m == WM_ACTIVATE || m == WM_NCACTIVATE || m == WM_SETFOCUS || m == WM_KILLFOCUS)) {
        // Discovery: what the hidden window is told about activation (first 24 messages).
        if (g_focus_logged.fetch_add(1, std::memory_order_relaxed) < 24)
            Log("[overlay] automation: window message 0x%04X wParam=0x%llX lParam=0x%llX", m,
                static_cast<unsigned long long>(wp), static_cast<unsigned long long>(lp));
        if (m == WM_ACTIVATEAPP) {
            // ds.exe keeps "application active" only from WM_ACTIVATEAPP (window procedure
            // 0x141A19BE0, case 0x1C: platform+0x68 = wParam == 1) and its pad reader
            // FUN_1419B3090 answers an empty pad while that flag is clear. The hidden window
            // is never activated by the user, so the game is always told it is active.
            if (wp == TRUE) {
                g_activate_true.fetch_add(1, std::memory_order_relaxed);
                return pass();
            }
            g_activate_rewritten.fetch_add(1, std::memory_order_relaxed);
            return prev ? CallWindowProcW(prev, hwnd, m, TRUE, lp) : DefWindowProcW(hwnd, m, TRUE, lp);
        }
    }
    if (m == WM_ACTIVATEAPP && wp == FALSE && !g_automation.load(std::memory_order_relaxed) &&
        g_local.load(std::memory_order_relaxed) != LocalMode::off) {
        // Local two-instance test: the other game's window takes the focus all the time; this one
        // must keep its app-active flag (pad reader, no pause), so it is told it stays active.
        if (g_local_activate_rewritten.fetch_add(1, std::memory_order_relaxed) == 0)
            Log("[overlay] local test: WM_ACTIVATEAPP(FALSE) handed to the game as TRUE (it keeps running unfocused)");
        if (g_open.load()) g_close_request.store(true);
        return prev ? CallWindowProcW(prev, hwnd, m, TRUE, lp) : DefWindowProcW(hwnd, m, TRUE, lp);
    }
    if (m == WM_KILLFOCUS || (m == WM_ACTIVATEAPP && wp == FALSE)) {
        // Automation: the hidden window is never really focused; keep the menu.
        if (g_open.load() && !g_automation.load(std::memory_order_relaxed)) g_close_request.store(true);
        return pass();
    }

    const std::uint32_t toggle = g_toggle_vk.load(std::memory_order_relaxed);
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP) && wp == toggle) {
        // Only reaches here when our keyboard hook is not installed.
        g_toggle_seen_by_messages.store(true, std::memory_order_relaxed);
        if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !(lp & (1LL << 30))) Toggle();
        return 0;
    }
    if (!Blocking()) {
        if (Carrying()) {
            if (m == WM_INPUT && CarryDropsRaw(lp)) {
                g_carry_raw.fetch_add(1, std::memory_order_relaxed);
                return DefWindowProcW(hwnd, m, wp, lp);
            }
            if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN || m == WM_XBUTTONDOWN || m == WM_LBUTTONDBLCLK ||
                m == WM_RBUTTONDBLCLK || m == WM_MBUTTONDBLCLK || m == WM_XBUTTONDBLCLK)
                return m == WM_XBUTTONDOWN || m == WM_XBUTTONDBLCLK ? TRUE : 0;
            if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && CarryBlocksKey(static_cast<std::uint32_t>(wp))) return 0;
        }
        return pass();
    }

    // ---- menu open (or the co-op waiting screen): nothing below reaches the game
    if (m == WM_INPUT) {
        if (IsHeldRelease(lp)) return pass();
        if (WaitOnly() && RawConfirmKey(lp)) return pass();
        g_blocked_raw.fetch_add(1, std::memory_order_relaxed);
        return DefWindowProcW(hwnd, m, wp, lp);  // lets the system clean up the raw input handle
    }
    if (IsMouseMessage(m)) {
        switch (m) {
            case WM_MOUSEMOVE: {
                Event e;
                e.type = EventType::mouse_move;
                e.x = static_cast<float>(GET_X_LPARAM(lp));
                e.y = static_cast<float>(GET_Y_LPARAM(lp));
                Push(e);
                break;
            }
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: PushButton(0, true); break;
            case WM_LBUTTONUP: PushButton(0, false); break;
            case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: PushButton(1, true); break;
            case WM_RBUTTONUP: PushButton(1, false); break;
            case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: PushButton(2, true); break;
            case WM_MBUTTONUP: PushButton(2, false); break;
            case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: PushButton(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 3 : 4, true); break;
            case WM_XBUTTONUP: PushButton(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 3 : 4, false); break;
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
                Event e;
                e.type = EventType::wheel;
                const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / static_cast<float>(WHEEL_DELTA);
                (m == WM_MOUSEWHEEL ? e.y : e.x) = notches;
                Push(e);
                break;
            }
            default: break;
        }
        if (IsButtonUp(m) && g_mouse_held.load(std::memory_order_relaxed)) return pass();  // held since before opening
        g_blocked_messages.fetch_add(1, std::memory_order_relaxed);
        return m == WM_XBUTTONDOWN || m == WM_XBUTTONUP || m == WM_XBUTTONDBLCLK ? TRUE : 0;
    }
    if (IsKeyMessage(m)) {
        if ((m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && wp == VK_F4) return pass();  // Alt+F4
        if (WaitOnly() && (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR) && ConfirmKey(static_cast<std::uint32_t>(wp)))
            return pass();
        const bool hooked = g_kb_hook_on.load(std::memory_order_relaxed);
        if (!hooked) {
            if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP)
                PushKey(static_cast<std::uint32_t>(wp), m == WM_KEYDOWN || m == WM_SYSKEYDOWN, (lp & (1LL << 24)) != 0);
            else if (m == WM_CHAR || m == WM_SYSCHAR)
                PushChar(static_cast<std::uint32_t>(wp));
        }
        if ((m == WM_KEYUP || m == WM_SYSKEYUP) && Held(static_cast<std::uint32_t>(wp))) return pass();  // held since before opening
        g_blocked_messages.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (m == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(nullptr);  // the menu draws its own cursor
        return TRUE;
    }
    return pass();
}

// Local two-instance test: only the game whose window really is in front may move or clip
// the cursor (the guest never: it has no mouse). The real foreground, not the guest's answer.
bool LocalCursorKept() noexcept {
    const LocalMode mode = g_local.load(std::memory_order_relaxed);
    if (mode == LocalMode::off || g_automation.load(std::memory_order_relaxed)) return false;
    if (mode != LocalMode::guest) {
        const HWND fg = g_orig_foreground ? g_orig_foreground() : GetForegroundWindow();
        const HWND own = g_hwnd.load(std::memory_order_acquire);
        if (own && fg == own) return false;
    }
    g_local_cursor_kept.fetch_add(1, std::memory_order_relaxed);
    return true;
}

BOOL WINAPI HookSetCursorPos(int x, int y) {
    if (Blocking() || g_automation.load(std::memory_order_relaxed) || LocalCursorKept()) {
        g_supp_pos.fetch_add(1, std::memory_order_relaxed);
        return TRUE;
    }
    return g_orig_setcursorpos(x, y);
}

BOOL WINAPI HookClipCursor(const RECT* r) {
    if (g_automation.load(std::memory_order_relaxed)) {  // the cursor belongs to the user's desktop
        g_supp_clip.fetch_add(1, std::memory_order_relaxed);
        return TRUE;
    }
    if (r && LocalCursorKept()) {  // releasing a clip (nullptr) always goes through
        g_supp_clip.fetch_add(1, std::memory_order_relaxed);
        return TRUE;
    }
    if (Blocking()) {
        AcquireSRWLockExclusive(&g_clip_lock);
        g_game_clip_set = true;
        g_game_clip_null = r == nullptr;
        if (r) g_game_clip = *r;
        ReleaseSRWLockExclusive(&g_clip_lock);
        g_supp_clip.fetch_add(1, std::memory_order_relaxed);
        return TRUE;
    }
    return g_orig_clipcursor(r);
}

DWORD AnswerXInput(XInputGetStateFn orig, DWORD index, XINPUT_STATE* state) {
    if (g_automation.load(std::memory_order_relaxed)) {
        // Only the virtual pad exists: a real pad never reaches the hidden game.
        AcquireSRWLockShared(&g_pad_lock);
        const PadState p = g_pad;
        const DWORD packet = g_pad_packet;
        ReleaseSRWLockShared(&g_pad_lock);
        if (index != 0 || !p.connected) {
            g_isolated_reads.fetch_add(1, std::memory_order_relaxed);
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        if (!state) return ERROR_BAD_ARGUMENTS;
        std::memset(state, 0, sizeof(*state));
        state->dwPacketNumber = packet;
        g_ride_buttons.store(p.buttons, std::memory_order_relaxed);
        g_ride_buttons_tick.store(GetTickCount64(), std::memory_order_relaxed);
        if (!Blocking()) {  // an open menu keeps the pad idle, as for a real pad
            state->Gamepad.wButtons = p.buttons;
            state->Gamepad.bLeftTrigger = p.lt;
            state->Gamepad.bRightTrigger = p.rt;
            state->Gamepad.sThumbLX = p.lx;
            state->Gamepad.sThumbLY = p.ly;
            state->Gamepad.sThumbRX = p.rx;
            state->Gamepad.sThumbRY = p.ry;
            if (Carrying()) {
                state->Gamepad.wButtons = CarryPassButtons(p.buttons);  // Start held beside a vehicle disposes of it, Back likes it: a Start tap only
                state->Gamepad.bLeftTrigger = state->Gamepad.bRightTrigger = 0;
                state->Gamepad.sThumbLX = state->Gamepad.sThumbLY = 0;
                g_carry_pads.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (WaitOnly()) {
            state->Gamepad.wButtons = p.buttons & XINPUT_GAMEPAD_A;  // the waiting screen: A only (dismiss a notice)
        }
        g_virtual_reads.fetch_add(1, std::memory_order_relaxed);
        return ERROR_SUCCESS;
    }
    if (g_local.load(std::memory_order_relaxed) == LocalMode::host) {
        // Local test, host on keyboard + mouse: the pads belong to the guest game.
        g_local_hidden_pads.fetch_add(1, std::memory_order_relaxed);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!orig) return ERROR_DEVICE_NOT_CONNECTED;
    const DWORD r = orig(index, state);
    if (r == ERROR_SUCCESS && state && index == 0) {
        g_ride_buttons.store(state->Gamepad.wButtons, std::memory_order_relaxed);
        g_ride_buttons_tick.store(GetTickCount64(), std::memory_order_relaxed);
    }
    if (r == ERROR_SUCCESS && state && !Blocking() && Carrying()) {
        state->Gamepad.wButtons = CarryPassButtons(state->Gamepad.wButtons);  // Start held beside a vehicle disposes of it, Back likes it: a Start tap only
        state->Gamepad.bLeftTrigger = state->Gamepad.bRightTrigger = 0;
        state->Gamepad.sThumbLX = state->Gamepad.sThumbLY = 0;
        g_carry_pads.fetch_add(1, std::memory_order_relaxed);
    }
    if (r == ERROR_SUCCESS && state && Blocking()) {
        const WORD keep = WaitOnly() ? static_cast<WORD>(state->Gamepad.wButtons & XINPUT_GAMEPAD_A) : 0;  // waiting screen: A
        std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));  // connected, idle
        state->Gamepad.wButtons = keep;
        g_idle_pads.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

DWORD WINAPI HookXInputGetState(DWORD index, XINPUT_STATE* state) { return AnswerXInput(g_orig_xinput, index, state); }
DWORD WINAPI HookXInput14GetState(DWORD index, XINPUT_STATE* state) { return AnswerXInput(g_orig_xinput14, index, state); }

// Automation: ds.exe also GetProcAddress'es XInputSetState (pointer at +0x16820, called by
// its vibration routine 0x1419B24F0 with the pad index it reads). The hidden game's pad 0
// is the virtual one, but the real xinput1_4 would send that rumble to the user's real
// controller in slot 0. Rumble for the virtual pad is accepted and dropped; other indices
// are "not connected", as for XInputGetState. Never reaches the system.
DWORD AnswerXInputSet(XInputSetStateFn orig, DWORD index, XINPUT_VIBRATION* vib) {
    if (g_automation.load(std::memory_order_relaxed)) {
        AcquireSRWLockShared(&g_pad_lock);
        const bool connected = g_pad.connected;
        ReleaseSRWLockShared(&g_pad_lock);
        if (index != 0 || !connected) return ERROR_DEVICE_NOT_CONNECTED;
        if (!vib) return ERROR_BAD_ARGUMENTS;
        if (g_vibration_kept.fetch_add(1, std::memory_order_relaxed) == 0)
            Log("[overlay] automation: XInputSetState(0) vibration %u/%u kept from the real pads (virtual pad only)",
                static_cast<unsigned>(vib->wLeftMotorSpeed), static_cast<unsigned>(vib->wRightMotorSpeed));
        return ERROR_SUCCESS;
    }
    if (g_local.load(std::memory_order_relaxed) == LocalMode::host) return ERROR_DEVICE_NOT_CONNECTED;  // the guest's pads
    return orig ? orig(index, vib) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI HookXInputSetState(DWORD index, XINPUT_VIBRATION* vib) { return AnswerXInputSet(g_orig_xinput_set, index, vib); }
DWORD WINAPI HookXInput14SetState(DWORD index, XINPUT_VIBRATION* vib) { return AnswerXInputSet(g_orig_xinput14_set, index, vib); }

// Before the first Present attaches us to the game window: the process's largest
// visible top-level window (cached for 250 ms; the hidden desktop holds only ours).
HWND OwnMainWindow() noexcept {
    static std::atomic<HWND> cached{nullptr};
    static std::atomic<ULONGLONG> when{0};
    const ULONGLONG now = GetTickCount64();
    if (now - when.load(std::memory_order_relaxed) < 250) return cached.load(std::memory_order_relaxed);
    struct Find {
        HWND best = nullptr;
        LONG area = 0;
    } f;
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId() || GetWindow(w, GW_OWNER) || !IsWindowVisible(w)) return TRUE;
        RECT r{};
        GetWindowRect(w, &r);
        auto* find = reinterpret_cast<Find*>(lp);
        const LONG area = (r.right - r.left) * (r.bottom - r.top);
        if (area > find->area) find->best = w, find->area = area;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    cached.store(f.best, std::memory_order_relaxed);
    when.store(now, std::memory_order_relaxed);
    return f.best;
}

HWND WINAPI HookGetForegroundWindow() {
    if (g_automation.load(std::memory_order_relaxed) || g_local.load(std::memory_order_relaxed) == LocalMode::guest) {
        HWND h = g_hwnd.load(std::memory_order_acquire);
        if (!h) h = OwnMainWindow();
        if (h) {
            g_foreground_answers.fetch_add(1, std::memory_order_relaxed);
            return h;
        }
    }
    return g_orig_foreground ? g_orig_foreground() : nullptr;
}

BOOL RealClipCursor(const RECT* r) noexcept { return g_orig_clipcursor ? g_orig_clipcursor(r) : ClipCursor(r); }

bool Create(void* target, void* detour, void** original, const char* name) noexcept {
    const MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) {
        Log("[overlay] input hook %s: MH_CreateHook failed (%s)", name, MH_StatusToString(s));
        return false;
    }
    return true;
}

}  // namespace

bool CreateApiHooks(void** targets, std::size_t* count, std::size_t cap) noexcept {
    HMODULE user = GetModuleHandleW(L"user32.dll");
    if (!user) user = LoadLibraryW(L"user32.dll");
    if (!user || *count + 7 > cap) return false;
    void* scp = reinterpret_cast<void*>(GetProcAddress(user, "SetCursorPos"));
    void* clip = reinterpret_cast<void*>(GetProcAddress(user, "ClipCursor"));
    if (!scp || !clip) {
        Log("[overlay] user32 SetCursorPos/ClipCursor not found");
        return false;
    }
    if (!Create(scp, reinterpret_cast<void*>(&HookSetCursorPos), reinterpret_cast<void**>(&g_orig_setcursorpos), "SetCursorPos"))
        return false;
    g_setcursorpos_target = scp;
    targets[(*count)++] = scp;
    if (!Create(clip, reinterpret_cast<void*>(&HookClipCursor), reinterpret_cast<void**>(&g_orig_clipcursor), "ClipCursor")) {
        RemoveApiHooks();
        return false;
    }
    g_clipcursor_target = clip;
    targets[(*count)++] = clip;

    // The game's XInput: ds.exe loads xinput1_4.dll by name and calls its
    // XInputGetState through a function pointer (0x1419B1C57); XINPUTUAP.dll
    // (delay-loaded) is only its fallback. Both are hooked (optional).
    HMODULE x14 = GetModuleHandleW(L"xinput1_4.dll");
    if (!x14) x14 = LoadLibraryW(L"xinput1_4.dll");
    void* x14get = x14 ? reinterpret_cast<void*>(GetProcAddress(x14, "XInputGetState")) : nullptr;
    if (x14get && Create(x14get, reinterpret_cast<void*>(&HookXInput14GetState), reinterpret_cast<void**>(&g_orig_xinput14),
                         "XInputGetState (xinput1_4)")) {
        g_xinput14_target = x14get;
        targets[(*count)++] = x14get;
    } else {
        Log("[overlay] xinput1_4 XInputGetState not hooked (%s)", x14 ? "export missing or hook failed" : "dll not loadable");
    }
    HMODULE xi = GetModuleHandleW(L"XINPUTUAP.dll");
    if (!xi) xi = LoadLibraryW(L"XINPUTUAP.dll");
    void* xget = xi ? reinterpret_cast<void*>(GetProcAddress(xi, "XInputGetState")) : nullptr;
    if (xget && xget != x14get &&
        Create(xget, reinterpret_cast<void*>(&HookXInputGetState), reinterpret_cast<void**>(&g_orig_xinput),
               "XInputGetState (XINPUTUAP)")) {
        g_xinput_target = xget;
        targets[(*count)++] = xget;
    } else {
        Log("[overlay] XINPUTUAP XInputGetState not hooked (%s)", xi ? "export missing or hook failed" : "dll not loadable");
    }
    const LocalMode local = g_automation.load() ? LocalMode::off : g_local.load();
    if (local == LocalMode::host || local == LocalMode::host_pad) {
        // The real foreground is read through this pointer (LocalCursorKept); not hooked.
        g_orig_foreground = reinterpret_cast<GetForegroundWindowFn>(GetProcAddress(user, "GetForegroundWindow"));
    }
    if (g_automation.load() || local == LocalMode::guest || local == LocalMode::host) {
        void* fg = (g_automation.load() || local == LocalMode::guest) ? reinterpret_cast<void*>(GetProcAddress(user, "GetForegroundWindow")) : nullptr;
        if (fg && Create(fg, reinterpret_cast<void*>(&HookGetForegroundWindow), reinterpret_cast<void**>(&g_orig_foreground),
                         "GetForegroundWindow")) {
            g_foreground_target = fg;
            targets[(*count)++] = fg;
        } else if (fg) {
            Log("[overlay] automation: GetForegroundWindow not hooked; the game may treat its hidden window as unfocused");
        }
        // Rumble for the virtual pad must not reach a real controller (see AnswerXInputSet).
        void* x14set = x14 ? reinterpret_cast<void*>(GetProcAddress(x14, "XInputSetState")) : nullptr;
        if (x14set && Create(x14set, reinterpret_cast<void*>(&HookXInput14SetState), reinterpret_cast<void**>(&g_orig_xinput14_set),
                             "XInputSetState (xinput1_4)")) {
            g_xinput14_set_target = x14set;
            targets[(*count)++] = x14set;
        } else {
            Log("[overlay] automation: xinput1_4 XInputSetState not hooked; the game's rumble may reach a real pad 0");
        }
        void* xset = xi ? reinterpret_cast<void*>(GetProcAddress(xi, "XInputSetState")) : nullptr;
        if (xset && xset != x14set &&
            Create(xset, reinterpret_cast<void*>(&HookXInputSetState), reinterpret_cast<void**>(&g_orig_xinput_set),
                   "XInputSetState (XINPUTUAP)")) {
            g_xinput_set_target = xset;
            targets[(*count)++] = xset;
        }
    }
    return true;
}

void RemoveApiHooks() noexcept {
    for (void** t : {&g_setcursorpos_target, &g_clipcursor_target, &g_xinput_target, &g_xinput14_target, &g_foreground_target,
                     &g_xinput_set_target, &g_xinput14_set_target}) {
        if (*t) {
            MH_DisableHook(*t);
            MH_RemoveHook(*t);
            *t = nullptr;
        }
    }
    g_orig_setcursorpos = nullptr;
    g_orig_clipcursor = nullptr;
    g_orig_xinput = nullptr;
    g_orig_xinput14 = nullptr;
    g_orig_foreground = nullptr;
    g_orig_xinput_set = nullptr;
    g_orig_xinput14_set = nullptr;
}

void SetToggleKey(std::uint32_t vk) noexcept { g_toggle_vk.store(vk ? vk : 0x78); }
std::uint32_t ToggleKey() noexcept { return g_toggle_vk.load(); }

bool Attach(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (g_hwnd.load() == hwnd) return true;
    if (g_hwnd.load()) Detach();
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!tid || pid != GetCurrentProcessId()) {
        Log("[overlay] window %p is not ours; input not attached", hwnd);
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    const auto prev = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
    if (!prev && GetLastError() != ERROR_SUCCESS) {
        Log("[overlay] could not subclass the game window (error %lu): menu input disabled", GetLastError());
        return false;
    }
    g_prev_proc.store(prev);
    g_hwnd.store(hwnd);
    // Our hook goes on top of the game's own WH_KEYBOARD hook (newest runs first).
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD, &KeyboardProc, nullptr, tid);
    if (!g_kb_hook) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&KeyboardProc), &self);
        g_kb_hook = SetWindowsHookExW(WH_KEYBOARD, &KeyboardProc, self, tid);
    }
    g_kb_hook_on.store(g_kb_hook != nullptr);
    Log("[overlay] input attached to window %p (thread %lu): subclassed, keyboard hook %s", hwnd, tid,
        g_kb_hook ? "on" : "FAILED (keys may reach the game while the menu is open)");
    if (g_automation.load() && AssertActive())
        Log("[overlay] automation: WM_ACTIVATEAPP(TRUE) posted to the game window (its pad input needs the app-active flag)");
    PlaceWindow(hwnd);
    return true;
}

void SetWindowSlot(WindowSlot slot) noexcept { g_window_slot.store(slot); }

void PlaceWindow(HWND hwnd) noexcept {
    const WindowSlot slot = g_window_slot.load();
    if (slot == WindowSlot::none || !hwnd || g_window_placed.exchange(true)) return;
    // Attach runs on the render thread; style and position changes send messages to the window's
    // own thread, which may be waiting on the render thread (hidden run 20261116-160909: both
    // games stopped presenting). So the subclass does it on the window thread.
    const UINT msg = PlaceMessage();
    if (!msg || !PostMessageW(hwnd, msg, 0, 0)) Log("[overlay] local test: could not ask the game window to move (error %lu)", GetLastError());
}

UINT PlaceMessage() noexcept {
    static const UINT msg = RegisterWindowMessageW(L"StrandedTogether.PlaceWindow");
    return msg;
}

// On the game window's thread (WndProc): the left or right half of the monitor the game opened
// on, 16:9, with a normal title bar (two games side by side).
void PlaceWindowNow(HWND hwnd) noexcept {
    const WindowSlot slot = g_window_slot.load();
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    const RECT work = mi.rcWork;
    const LONG half = (work.right - work.left) / 2;
    LONG cw = half - 16, ch = cw * 9 / 16;
    const DWORD style = (static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)) & ~static_cast<DWORD>(WS_POPUP | WS_MAXIMIZE)) | WS_OVERLAPPEDWINDOW;
    const DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)) & ~static_cast<DWORD>(WS_EX_TOPMOST);
    RECT r{0, 0, cw, ch};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    if (r.bottom - r.top > work.bottom - work.top) {  // a tall monitor: fit the height instead
        ch = (work.bottom - work.top) - (r.bottom - r.top - ch);
        cw = ch * 16 / 9;
        r = RECT{0, 0, cw, ch};
        AdjustWindowRectEx(&r, style, FALSE, ex);
    }
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int x = slot == WindowSlot::left ? work.left + (half - w) / 2 : work.left + half + (half - w) / 2;
    const int y = work.top + ((work.bottom - work.top) - h) / 2;
    SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(style));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(ex));
    g_keep_w = w, g_keep_h = h, g_keep_monitor = mi.rcMonitor;
    g_window_kept.store(true);
    const BOOL ok = SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Log("[overlay] local test: game window placed on the %s half (%d,%d %dx%d, client %ldx%ld)%s", slot == WindowSlot::left ? "left" : "right", x,
        y, w, h, cw, ch, ok ? "" : " -- SetWindowPos failed");
}

bool AssertActive() noexcept {
    const HWND hwnd = g_hwnd.load();
    if (!g_automation.load() || !hwnd) return false;
    // Posted to the window itself (never the user's desktop); the subclass passes it to
    // ds.exe's window procedure, which sets its app-active flag. lParam: the window's own
    // thread (WM_ACTIVATEAPP's "other thread"), nothing outside the process is named.
    if (!PostMessageW(hwnd, WM_ACTIVATEAPP, TRUE, static_cast<LPARAM>(GetWindowThreadProcessId(hwnd, nullptr)))) return false;
    g_activate_posted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Detach() noexcept {
    SetOpen(false);
    if (g_kb_hook) {
        UnhookWindowsHookEx(g_kb_hook);
        g_kb_hook = nullptr;
    }
    g_kb_hook_on.store(false);
    const HWND hwnd = g_hwnd.exchange(nullptr);
    const WNDPROC prev = g_prev_proc.load();
    if (hwnd && prev && IsWindow(hwnd) && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) == &WndProc)
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(prev));
    // g_prev_proc stays: a message already inside WndProc may still call it.
    ClearEvents();
}

HWND Window() noexcept { return g_hwnd.load(); }

unsigned TakeToggles() noexcept { return g_toggles.exchange(0); }

void PollToggleFallback() noexcept {
    // Only when neither our keyboard hook nor the window ever reported the key.
    if (g_kb_hook_on.load() || g_toggle_seen_by_messages.load()) return;
    const HWND hwnd = g_hwnd.load();
    const bool fg = hwnd && GetForegroundWindow() == hwnd;
    const bool down = fg && (GetAsyncKeyState(static_cast<int>(g_toggle_vk.load())) & 0x8000) != 0;
    if (down && !g_poll_prev) Toggle();
    g_poll_prev = down;
}

bool TakeCloseRequest() noexcept { return g_close_request.exchange(false); }
void RequestToggleForTest() noexcept { Toggle(); }
LRESULT KeyboardProcForTest(int code, WPARAM vk, LPARAM lp) noexcept { return KeyboardProc(code, vk, lp); }

// The game's input became blocked (menu opened / waiting screen shown) or free again: the cursor clip bookkeeping.
static void OnBlockChange(bool open) noexcept {
    const HWND hwnd = g_hwnd.load();
    if (open) {
        ClearEvents();
        CaptureHeld(g_toggle_vk.load());
        AcquireSRWLockExclusive(&g_clip_lock);
        RECT r{};
        g_clip_saved = GetClipCursor(&r) != FALSE;
        RECT desk{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        desk.right = desk.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        desk.bottom = desk.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        g_clip_was_free = !g_clip_saved || EqualRect(&r, &desk);
        g_clip_rect = r;
        g_game_clip_set = false;
        ReleaseSRWLockExclusive(&g_clip_lock);
        // free the cursor for the menu (only once our window is known: the co-op waiting screen is set before the overlay
        // attaches, and the clip is desktop-global)
        if (!g_automation.load() && hwnd) RealClipCursor(nullptr);
    } else {
        AcquireSRWLockExclusive(&g_clip_lock);
        bool apply = false, null_clip = true;
        RECT r{};
        if (g_game_clip_set) {  // what the game asked for while we held the cursor
            apply = true;
            null_clip = g_game_clip_null;
            r = g_game_clip;
        } else if (g_clip_saved) {
            apply = true;
            null_clip = g_clip_was_free;
            r = g_clip_rect;
        }
        g_clip_saved = g_game_clip_set = false;
        ReleaseSRWLockExclusive(&g_clip_lock);
        // The clip is desktop-global: never impose it on another focused application.
        if (apply && hwnd && !g_automation.load() && GetForegroundWindow() == hwnd) RealClipCursor(null_clip ? nullptr : &r);
        ClearEvents();
        ClearHeld();
    }
}

void SetOpen(bool open) noexcept {
    const bool before = Blocking();
    const bool was = g_open.exchange(open);
    if (was == open) return;
    if (open) g_close_request.store(false);
    if (before != Blocking()) OnBlockChange(open);
    else ClearEvents();  // the menu over the waiting screen (or back to it): the input stays blocked
}

void SetWaitBlock(bool on) noexcept {
    const bool before = Blocking();
    if (g_wait.exchange(on) == on) return;
    if (before != Blocking()) OnBlockChange(on);
    Log("[overlay] co-op waiting screen %s", on ? "shown: game input blocked (pad A, Enter and Space still reach the game)"
                                                 : "hidden: game input restored");
}
bool WaitBlocked() noexcept { return g_wait.load(); }

void SetRideCarry(bool on) noexcept {
    if (g_carry.exchange(on, std::memory_order_acq_rel) != on)
        Log("[overlay] riding: the game's movement and action input %s", on ? "blocked (carried as a passenger; look passes)" : "back");
}
bool RideCarry() noexcept { return g_carry.load(std::memory_order_acquire); }
std::uint16_t RidePadButtons(unsigned max_age_ms) noexcept {
    const std::uint64_t t = g_ride_buttons_tick.load(std::memory_order_relaxed);
    if (!t || GetTickCount64() - t > max_age_ms) return 0;
    return g_ride_buttons.load(std::memory_order_relaxed);
}
// The hook's state, confirmed by the key's physical state: a release missed during a focus change (the hook never saw it)
// cannot leave the key "down" (stage 1's known gap).
bool RideKeyDown() noexcept {
    if (g_automation.load(std::memory_order_relaxed) || !g_ride_key.load(std::memory_order_relaxed)) return false;
    if ((GetAsyncKeyState(static_cast<int>(g_ride_vk.load(std::memory_order_relaxed))) & 0x8000) != 0) return true;
    g_ride_key.store(false, std::memory_order_relaxed);
    return false;
}
void SetRideKey(std::uint32_t vk) noexcept {
    if (vk > 0 && vk < 256) g_ride_vk.store(vk, std::memory_order_relaxed);
}
void RideCounters(unsigned long* pads, unsigned long* keys, unsigned long* raw) noexcept {
    if (pads) *pads = g_carry_pads.load(std::memory_order_relaxed);
    if (keys) *keys = g_carry_keys.load(std::memory_order_relaxed);
    if (raw) *raw = g_carry_raw.load(std::memory_order_relaxed);
}

void MarkHeldForTest(std::uint32_t vk) noexcept { MarkHeld(vk); }
void ClearHeldForTest() noexcept { ClearHeld(); }

bool IsOpen() noexcept { return g_open.load(); }

std::size_t Drain(Event* out, std::size_t max) noexcept {
    AcquireSRWLockExclusive(&g_ev_lock);
    const std::size_t n = g_ev_count < max ? g_ev_count : max;
    for (std::size_t i = 0; i < n; ++i) out[i] = g_events[(g_ev_head + i) % kCap];
    g_ev_head = (g_ev_head + n) % kCap;
    g_ev_count -= n;
    ReleaseSRWLockExclusive(&g_ev_lock);
    return n;
}

bool CursorClientPos(float* x, float* y) noexcept {
    const HWND hwnd = g_hwnd.load();
    POINT p{};
    if (!hwnd || !GetCursorPos(&p) || !ScreenToClient(hwnd, &p)) return false;
    *x = static_cast<float>(p.x);
    *y = static_cast<float>(p.y);
    return true;
}

Stats GetStats() noexcept {
    Stats s;
    s.subclassed = g_hwnd.load() != nullptr;
    s.keyboard_hook = g_kb_hook_on.load();
    s.cursor_hooks = g_setcursorpos_target && g_clipcursor_target;
    s.xinput_hook = g_xinput_target != nullptr || g_xinput14_target != nullptr;
    s.xinput14_hook = g_xinput14_target != nullptr;
    s.automation = g_automation.load();
    s.local = g_local.load();
    s.local_hidden_pad_reads = g_local_hidden_pads.load();
    s.local_cursor_kept = g_local_cursor_kept.load();
    s.virtual_pad_reads = g_virtual_reads.load();
    s.isolated_pad_reads = g_isolated_reads.load();
    s.foreground_answers = g_foreground_answers.load();
    s.activate_app_true = g_activate_true.load();
    s.activate_app_rewritten = g_activate_rewritten.load();
    s.activate_app_posted = g_activate_posted.load();
    s.vibration_kept = g_vibration_kept.load();
    s.xinput_set_hook = g_xinput14_set_target != nullptr || g_xinput_set_target != nullptr;
    s.blocked_messages = g_blocked_messages.load();
    s.blocked_raw_input = g_blocked_raw.load();
    s.blocked_keys = g_blocked_keys.load();
    s.characters = g_chars.load();
    s.suppressed_clip = g_supp_clip.load();
    s.suppressed_cursor_pos = g_supp_pos.load();
    s.idle_pads = g_idle_pads.load();
    s.toggles = g_toggle_count.load();
    return s;
}

void SetAutomation(bool on) noexcept { g_automation.store(on); }
void SetLocalMode(LocalMode mode) noexcept { g_local.store(mode); }
LocalMode GetLocalMode() noexcept { return g_local.load(); }
bool Automation() noexcept { return g_automation.load(); }

void SetVirtualPad(const PadState& pad) noexcept {
    AcquireSRWLockExclusive(&g_pad_lock);
    const bool changed = pad.connected != g_pad.connected || pad.buttons != g_pad.buttons || pad.lt != g_pad.lt ||
                         pad.rt != g_pad.rt || pad.lx != g_pad.lx || pad.ly != g_pad.ly || pad.rx != g_pad.rx ||
                         pad.ry != g_pad.ry;
    g_pad = pad;
    if (changed) ++g_pad_packet;  // XInput: a new packet number whenever the state changes
    ReleaseSRWLockExclusive(&g_pad_lock);
}

PadState VirtualPad() noexcept {
    AcquireSRWLockShared(&g_pad_lock);
    const PadState p = g_pad;
    ReleaseSRWLockShared(&g_pad_lock);
    return p;
}

}  // namespace st::overlay::input
