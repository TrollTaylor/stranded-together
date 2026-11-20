// See overlay_input.hpp for what is blocked and why.
#include "overlay_input.hpp"

#include <windowsx.h>
#include <xinput.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "log.hpp"
#include "mouse_lock.hpp"

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
// Per XInput index (the game polls 0..3; play test 2: the user's pad need not be index 0).
std::atomic<std::uint16_t> g_ride_buttons[4]{};
std::atomic<std::uint64_t> g_ride_buttons_tick[4]{};
std::atomic<std::uint32_t> g_ride_vk{'F'};
std::atomic<bool> g_ride_key{false};
std::atomic<unsigned long> g_carry_pads{0}, g_carry_keys{0}, g_carry_raw{0};
std::atomic<std::uint32_t> g_toggle_vk{0x78};
std::atomic<unsigned> g_toggles{0};
std::atomic<ToggleSource> g_toggle_source{ToggleSource::none};  // where the last toggle came from (the log line)
std::atomic<ULONGLONG> g_toggle_tick{0};                        // when it came
std::atomic<CloseReason> g_close_request{CloseReason::none};
std::atomic<bool> g_toggle_seen_by_messages{false};  // the hook/WndProc saw the key at least once
// The key-state poll (PollToggleFallback, render thread): a press of the toggle key the hook and the window never
// reported within kPollGraceMs toggles from the poll; a late hook sighting of that press is then not toggled again.
constexpr ULONGLONG kPollGraceMs = 400, kPollClaimMs = 5000;
// The hook / window saw a fresh toggle-key press (not autorepeat) since the poll last saw the key up with no edge pending:
// the poll's pending edge is then reported (whenever the next Present comes) and never toggles again.
std::atomic<bool> g_press_since_up{false};
std::atomic<ULONGLONG> g_poll_claim_tick{0};   // the poll toggled for a press nobody else reported
std::atomic<unsigned> g_poll_ups{0};           // the poll saw the key go up (counts releases)
std::atomic<unsigned> g_poll_claim_ups{0};     // g_poll_ups when the poll claimed: the claimed press is still held if equal
bool g_poll_prev = false;
ULONGLONG g_poll_edge_tick = 0;
// Keyboard hook health (the log lines prove the pump order and the hook order on the player's machine).
std::atomic<unsigned long> g_hook_noremove{0}, g_hook_action{0};
std::atomic<bool> g_first_key_logged{false};
std::atomic<ULONGLONG> g_hook_first_tick{0};
std::atomic<bool> g_hook_health_logged{false};
// The last HC_NOREMOVE sighting (window thread only): the same message at HC_ACTION right after it was already handled.
bool g_prev_valid = false;
WPARAM g_prev_vk = 0;
LPARAM g_prev_lp = 0;

// Mouse lock (mouse_lock.hpp; session.ini mouse_lock=1).
std::atomic<bool> g_lock_cfg{false};      // mouse_lock=1
std::atomic<bool> g_lock_on{false};       // the runtime switch (debug menu), on with the session key
std::atomic<bool> g_lock_engaged{false};  // since we came to the front the cursor was inside the client area
std::atomic<bool> g_size_move{false};
std::atomic<std::uint32_t> g_switch_vk{VK_F10};
SRWLOCK g_lock_srw = SRWLOCK_INIT;
bool g_lock_ours = false;  // the clip in effect is ours (g_lock_rect)
RECT g_lock_rect{};
// The last clip this process applied (the game's own non-null rect that went through, or the lock's): an unfocused
// game's release only frees a clip it set itself, never the focused game's (the clip is desktop-global).
bool g_applied = false;
RECT g_applied_rect{};
std::atomic<unsigned long> g_lock_clips{0}, g_lock_releases{0}, g_lock_replaced{0}, g_switches{0}, g_foreign_releases{0};
std::atomic<int> g_lock_why{-1};          // the last decision (mouselock::Why), for the status line
int g_lock_logged_class = -1;             // render thread: the last logged decision class
RECT g_lock_logged_rect{};
constexpr wchar_t kSlotProp[] = L"StrandedTogether.LocalSlot";  // on each Local test game window: 1 left, 2 right

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

void Toggle(ToggleSource source) noexcept {
    g_toggle_source.store(source, std::memory_order_relaxed);
    g_toggle_tick.store(GetTickCount64(), std::memory_order_relaxed);
    g_toggles.fetch_add(1);
    g_toggle_count.fetch_add(1);
}

// A fresh press of the toggle key seen by our keyboard hook or the window procedure.
void PressToggle(ToggleSource source) noexcept {
    const ULONGLONG now = GetTickCount64();
    g_press_since_up.store(true, std::memory_order_release);
    const ULONGLONG claim = g_poll_claim_tick.exchange(0, std::memory_order_acq_rel);
    if (claim && now - claim < kPollClaimMs) {
        // The poll's claim matches this sighting only if it is the same press: the key is still held with no release seen
        // by the poll since the claim, or it is already up (a late sighting of a tap: the pump was late). Held again after
        // a release the poll saw = a new press, which toggles.
        const bool held = (GetAsyncKeyState(static_cast<int>(g_toggle_vk.load(std::memory_order_relaxed))) & 0x8000) != 0;
        const bool same = !held || g_poll_ups.load(std::memory_order_relaxed) == g_poll_claim_ups.load(std::memory_order_relaxed);
        if (same) {  // the poll already toggled for this press (the pump was late)
            Log("[overlay] menu key seen by %s %llu ms after the key-state poll toggled for it: not toggled again",
                ToggleSourceName(source), static_cast<unsigned long long>(now - claim));
            return;
        }
        Log("[overlay] menu key seen by %s: a new press after the key-state poll's toggle (released since): toggled",
            ToggleSourceName(source));
    }
    Toggle(source);
}

HWND RealForeground() noexcept { return g_orig_foreground ? g_orig_foreground() : GetForegroundWindow(); }
BOOL RealClipCursor(const RECT* r) noexcept;

// ---- mouse lock (mouse_lock.hpp)
RECT VirtualScreen() noexcept {
    RECT r{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

bool ClientOnScreen(HWND hwnd, RECT* out) noexcept {
    RECT c{};
    if (!hwnd || !GetClientRect(hwnd, &c)) return false;
    POINT p[2] = {{c.left, c.top}, {c.right, c.bottom}};
    if (!ClientToScreen(hwnd, &p[0]) || !ClientToScreen(hwnd, &p[1])) return false;
    *out = RECT{p[0].x, p[0].y, p[1].x, p[1].y};
    return true;
}

// Automation: a hidden game is "in front" (the hooked answer) and engaged, so the decision path runs and is logged;
// nothing is applied.
mouselock::State GatherLock() noexcept {
    mouselock::State s;
    const HWND hwnd = g_hwnd.load(std::memory_order_acquire);
    s.enabled = g_lock_cfg.load(std::memory_order_relaxed) && g_lock_on.load(std::memory_order_relaxed) && hwnd;
    s.automation = g_automation.load(std::memory_order_relaxed);
    if (!hwnd) return s;
    s.foreground = s.automation || RealForeground() == hwnd;
    s.engaged = s.automation || g_lock_engaged.load(std::memory_order_relaxed);
    s.iconic = IsIconic(hwnd) != FALSE;
    s.size_move = g_size_move.load(std::memory_order_relaxed);
    ClientOnScreen(hwnd, &s.client);
    RECT clip{};
    if (GetClipCursor(&clip)) {
        const RECT all = VirtualScreen();
        s.have_clip = !mouselock::SameRect(clip, all) && !mouselock::RectInside(all, clip);
        s.clip = clip;
    }
    AcquireSRWLockShared(&g_lock_srw);
    s.ours = g_lock_ours && s.have_clip && mouselock::SameRect(s.clip, g_lock_rect);
    ReleaseSRWLockShared(&g_lock_srw);
    return s;
}

// Under g_lock_srw: what this process applied last.
void NoteAppliedLocked(const RECT* r) noexcept {
    g_applied = r != nullptr;
    if (r) g_applied_rect = *r;
}

// The lock's clip. The check and the clip happen under g_lock_srw, and LockLetGo clears g_lock_engaged before it takes
// the lock: a focus loss / minimise on the window thread between the render thread's GatherLock and this clip either
// stops it here or releases it right after (never a clip left on a window that is no longer in front).
bool LockClipTo(const RECT& r) noexcept {
    const HWND hwnd = g_hwnd.load(std::memory_order_acquire);
    AcquireSRWLockExclusive(&g_lock_srw);
    const bool still = hwnd && g_lock_engaged.load(std::memory_order_relaxed) && RealForeground() == hwnd && !IsIconic(hwnd) &&
                       !g_size_move.load(std::memory_order_relaxed);
    if (still) {
        g_lock_ours = true;
        g_lock_rect = r;
        RealClipCursor(&r);
        NoteAppliedLocked(&r);
    }
    ReleaseSRWLockExclusive(&g_lock_srw);
    if (still) g_lock_clips.fetch_add(1, std::memory_order_relaxed);
    return still;
}

// Frees our own clip (never one the game or another program set). Window thread or render thread.
void LockReleaseOurs(const char* why) noexcept {
    if (g_automation.load(std::memory_order_relaxed)) return;
    AcquireSRWLockExclusive(&g_lock_srw);
    const bool ours = g_lock_ours;
    g_lock_ours = false;
    RECT clip{};
    const bool release = ours && GetClipCursor(&clip) && mouselock::SameRect(clip, g_lock_rect);
    if (release) {
        RealClipCursor(nullptr);
        NoteAppliedLocked(nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock_srw);
    if (release) {
        g_lock_releases.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 40) Log("[mouse] lock: released (%s)", why);
    }
}

// A release of the desktop-global clip asked for by this game (its own ClipCursor(NULL), or the menu opening). In front:
// it goes. Not in front, in the Local test or with the lock: only a clip this process applied itself is freed; the
// focused game's clip (the lock's rect, or its one-pixel centre clip in gameplay) stays. ds.exe's ClipCentre calls
// ClipCursor(NULL) whenever GetFocus() is not its window, so the unfocused game does that on every cursor-request edge.
bool ReleaseFromHere() noexcept {
    const bool guarded = g_lock_cfg.load(std::memory_order_relaxed) || g_local.load(std::memory_order_relaxed) != LocalMode::off;
    const HWND hwnd = g_hwnd.load(std::memory_order_acquire);
    AcquireSRWLockExclusive(&g_lock_srw);
    bool go = !guarded || (hwnd && RealForeground() == hwnd);
    if (!go) {
        RECT clip{};
        go = g_applied && GetClipCursor(&clip) && mouselock::SameRect(clip, g_applied_rect);
    }
    if (go) {
        g_lock_ours = false;
        RealClipCursor(nullptr);
        NoteAppliedLocked(nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock_srw);
    if (!go && g_foreign_releases.fetch_add(1, std::memory_order_relaxed) == 0)
        Log("[mouse] the game freed the cursor while another window is in front: not applied (the clip in effect is not this "
            "game's)");
    return go;
}

// The window lost the focus / is minimised / is being moved: the lock lets go at once, before the game's own handling.
void LockLetGo(const char* why) noexcept {
    g_lock_engaged.store(false, std::memory_order_relaxed);
    if (g_lock_cfg.load(std::memory_order_relaxed)) LockReleaseOurs(why);
}

bool SwitchEnabled() noexcept {
    return g_lock_cfg.load(std::memory_order_relaxed) && g_local.load(std::memory_order_relaxed) != LocalMode::off &&
           g_window_slot.load(std::memory_order_relaxed) != WindowSlot::none;
}

UINT SwitchMessage() noexcept {
    static const UINT msg = RegisterWindowMessageW(L"StrandedTogether.SwitchWindow");
    return msg;
}

// The other Local test game's window: a visible top-level window of another process carrying the other slot's property
// (PlaceWindowNow sets it). Only a game running this mod in the Local test on the same desktop has it.
HWND FindPeerWindow(HWND own) noexcept {
    const WindowSlot mine = g_window_slot.load(std::memory_order_relaxed);
    struct Find {
        HWND own;
        HANDLE want;
        HWND found;
    } f{own, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(mine == WindowSlot::left ? 2 : 1)), nullptr};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* find = reinterpret_cast<Find*>(lp);
        if (w == find->own || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) return TRUE;
        if (GetPropW(w, kSlotProp) != find->want) return TRUE;
        find->found = w;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.found;
}

// Window thread (SwitchMessage, posted by the keyboard hook): our clip goes, the cursor goes to the centre of the other
// game's client area and that window comes to the front (allowed: this process is in front and just got the key).
void SwitchNow(HWND own) noexcept {
    char key[24];
    std::snprintf(key, sizeof(key), "F%u", static_cast<unsigned>(g_switch_vk.load(std::memory_order_relaxed) - VK_F1 + 1));
    const bool left = g_window_slot.load(std::memory_order_relaxed) == WindowSlot::left;
    const char* other = left ? "right (guest)" : "left (host)";
    const HWND peer = FindPeerWindow(own);
    if (g_automation.load(std::memory_order_relaxed)) {
        Log("[mouse] %s: would switch to the %s game window (%p)%s", key, other, static_cast<void*>(peer),
            peer ? " -- hidden run: nothing moved or activated" : ": no other game window found");
        return;
    }
    if (!peer) {
        Log("[mouse] %s: no other game window found (the other Local test game is not running?)", key);
        return;
    }
    // Every clip goes (ours, and the game's one-pixel centre clip in gameplay, which would hold the cursor here): this game
    // is in front and the player asked to leave it.
    g_lock_engaged.store(false, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_lock_srw);
    g_lock_ours = false;
    RealClipCursor(nullptr);
    NoteAppliedLocked(nullptr);
    ReleaseSRWLockExclusive(&g_lock_srw);
    RECT c{};
    if (ClientOnScreen(peer, &c) && !mouselock::EmptyRect(c)) {
        const int x = (c.left + c.right) / 2, y = (c.top + c.bottom) / 2;
        if (g_orig_setcursorpos) g_orig_setcursorpos(x, y);
        else SetCursorPos(x, y);
    }
    if (IsIconic(peer)) ShowWindowAsync(peer, SW_RESTORE);
    const BOOL ok = SetForegroundWindow(peer);
    g_switches.fetch_add(1, std::memory_order_relaxed);
    Log("[mouse] %s: switched to the %s game window (%p)%s", key, other, static_cast<void*>(peer),
        ok ? "" : " -- SetForegroundWindow refused: click it once");
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

void NoteHookKey(int code, std::uint32_t vk) noexcept {
    const ULONGLONG now = GetTickCount64();
    if (!g_first_key_logged.exchange(true, std::memory_order_relaxed)) {
        g_hook_first_tick.store(now, std::memory_order_relaxed);
        Log("[overlay] keyboard hook: first key seen (vk 0x%02X, %s)", vk, code == HC_ACTION ? "HC_ACTION" : "HC_NOREMOVE");
        return;
    }
    const ULONGLONG first = g_hook_first_tick.load(std::memory_order_relaxed);
    if (now - first >= 30000 && !g_hook_health_logged.exchange(true, std::memory_order_relaxed)) {
        const unsigned long nr = g_hook_noremove.load(std::memory_order_relaxed), ac = g_hook_action.load(std::memory_order_relaxed);
        Log("[overlay] keyboard hook: %lu HC_NOREMOVE / %lu HC_ACTION calls in the first 30 s of keys%s", nr, ac,
            ac == 0 && nr > 20 ? " (no HC_ACTION: ds.exe's own WH_KEYBOARD hook sits above ours and keeps every key it gets)" : "");
    }
}

// ds.exe's message pump (FUN_141A19350) peeks with PM_NOREMOVE before its GetMessage, so every key reaches this hook
// first with HC_NOREMOVE and, only if no hook ate it there, once more with HC_ACTION. A nonzero answer at HC_NOREMOVE
// removes the message for good (it never comes back with HC_ACTION). So every key is handled at its FIRST sighting,
// whatever the code; the HC_ACTION sighting of a message we already saw at HC_NOREMOVE only repeats the answer. (v0.19
// and before acted at HC_ACTION only and ate the toggle key at HC_NOREMOVE: F9 never opened the menu in the real game,
// play test 2; and when ds.exe's own hook, which keeps every HC_ACTION key, sits above ours, HC_NOREMOVE is the only
// sighting we get at all.)
LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code < 0) return CallNextHookEx(nullptr, code, wp, lp);
    const bool action = code == HC_ACTION;
    (action ? g_hook_action : g_hook_noremove).fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t vk = static_cast<std::uint32_t>(wp);
    const bool up = (lp & (1LL << 31)) != 0;
    const bool was_down = (lp & (1LL << 30)) != 0;
    const bool alt = (lp & (1LL << 29)) != 0;
    const bool extended = (lp & (1LL << 24)) != 0;
    NoteHookKey(code, vk);
    const bool first = !(action && g_prev_valid && g_prev_vk == wp && g_prev_lp == lp);
    g_prev_valid = !action;
    g_prev_vk = wp;
    g_prev_lp = lp;
    if (vk == g_ride_vk.load(std::memory_order_relaxed)) g_ride_key.store(!up, std::memory_order_relaxed);  // state: every sighting
    if (vk == g_toggle_vk.load(std::memory_order_relaxed)) {
        g_toggle_seen_by_messages.store(true, std::memory_order_relaxed);
        if (!up && !was_down && first) PressToggle(action ? ToggleSource::hook_action : ToggleSource::hook_noremove);
        return 1;  // the toggle key never reaches the game
    }
    if (vk == g_switch_vk.load(std::memory_order_relaxed) && SwitchEnabled()) {
        const HWND hwnd = g_hwnd.load(std::memory_order_acquire);
        if (!up && !was_down && first && hwnd) PostMessageW(hwnd, SwitchMessage(), 0, 0);  // acted on in WndProc
        return 1;  // the switch key never reaches the game (nor DefWindowProc's F10 menu mode)
    }
    if (!Blocking() && Carrying() && !up && CarryBlocksKey(vk)) {
        if (first) g_carry_keys.fetch_add(1, std::memory_order_relaxed);
        return 1;  // carried: the key's press never reaches the game (its release does)
    }
    if (Blocking()) {
        if (vk == VK_F4 && alt) return CallNextHookEx(nullptr, code, wp, lp);  // Alt+F4 still closes the game
        if (WaitOnly() && ConfirmKey(vk)) return CallNextHookEx(nullptr, code, wp, lp);  // waiting screen: confirm
        if (up && Held(vk)) {  // releasing a key held since before the menu opened
            if (first) PushKey(vk, false, extended);
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        if (first) {
            PushKey(vk, !up, extended);
            if (!up) PushCharsFor(vk, lp);
            g_blocked_keys.fetch_add(1, std::memory_order_relaxed);
        }
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// ds.exe's own WH_KEYBOARD procedure (installed by its input init FUN_1419B8130; hook handle at 0x145559220): at
// HC_ACTION it maps the key through its table and updates the game's key state (0x1419BA230), returns 0 and never chains.
// Hidden runs only (`hookkey`): the game's keyboard path for a key our hook let through. Byte-checked once; never called
// when the bytes differ.
constexpr std::uintptr_t kGameKbProc = 0x1419B9920;
constexpr unsigned char kGameKbProcSig[] = {0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x49, 0x8B, 0xF8, 0x48, 0x8B, 0xF2, 0x85, 0xC9, 0x0F, 0x85};
std::atomic<unsigned long> g_game_kb_calls{0};
void CallGameKeyboardProc(WPARAM wp, LPARAM lp) noexcept {
    static int ok = -1;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const std::uintptr_t at = base + (kGameKbProc - 0x140000000ull);
    if (ok < 0) {
        unsigned char got[sizeof(kGameKbProcSig)] = {};
        SIZE_T n = 0;
        ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), got, sizeof(got), &n) && n == sizeof(got) &&
                     std::memcmp(got, kGameKbProcSig, sizeof(got)) == 0
                 ? 1
                 : 0;
        Log("[overlay] hookkey: ds.exe's own keyboard procedure at 0x%llX %s", static_cast<unsigned long long>(kGameKbProc),
            ok ? "checks: keys our hook lets through reach it at HC_ACTION" : "does NOT check (this build differs): keys are only posted");
    }
    if (ok != 1) return;
    reinterpret_cast<LRESULT(CALLBACK*)(int, WPARAM, LPARAM)>(at)(HC_ACTION, wp, lp);
    g_game_kb_calls.fetch_add(1, std::memory_order_relaxed);
}

UINT HookKeyMessageId() noexcept {
    static const UINT msg = RegisterWindowMessageW(L"StrandedTogether.HookKey");
    return msg;
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
    if (m != 0 && m == SwitchMessage()) {
        SwitchNow(hwnd);
        return 0;
    }
    if (m != 0 && m == HookKeyMessageId()) {
        // Hidden runs (scenario `hookkey`): our keyboard hook in ds.exe's call order, on this (the window's) thread.
        // Posted key messages never reach WH_KEYBOARD hooks and SendInput would type on the user's desktop, so this is
        // the closest a hidden run gets to a physical key: HC_NOREMOVE first (the pump's PM_NOREMOVE peek), HC_ACTION
        // only if that did not eat it, and a key nobody ate goes on to the game window as WM_KEYDOWN / WM_KEYUP.
        // A key nobody ate also reaches ds.exe's own WH_KEYBOARD procedure at HC_ACTION, as the system calls it for a
        // physical key (it fills the game's key table: the game's own keyboard input, e.g. F = "Use Vehicle").
        if (!g_automation.load(std::memory_order_relaxed)) return 0;
        if (KeyboardProc(HC_NOREMOVE, wp, lp)) return 1;
        if (KeyboardProc(HC_ACTION, wp, lp)) return 2;
        CallGameKeyboardProc(wp, lp);
        PostMessageW(hwnd, (lp & (1LL << 31)) ? WM_KEYUP : WM_KEYDOWN, wp, lp);
        return 3;
    }
    // Mouse lock: let go at once when the window loses the focus, is minimised or moved (before the game's own handling,
    // whose Suspend then frees the cursor as usual).
    if (m == WM_KILLFOCUS) LockLetGo("the game window lost the focus");
    else if (m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) LockLetGo("the game window was deactivated");
    else if (m == WM_ACTIVATEAPP && wp == FALSE) LockLetGo("another program came to the front");
    else if (m == WM_ENTERSIZEMOVE) {
        g_size_move.store(true, std::memory_order_relaxed);
        LockLetGo("the window is being moved or resized");
    } else if (m == WM_EXITSIZEMOVE) g_size_move.store(false, std::memory_order_relaxed);
    else if (m == WM_SIZE && wp == SIZE_MINIMIZED) LockLetGo("the game window was minimised");
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
        RemovePropW(hwnd, kSlotProp);
        LockLetGo("the game window closed");
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
        if (g_open.load()) g_close_request.store(CloseReason::app_inactive);
        return prev ? CallWindowProcW(prev, hwnd, m, TRUE, lp) : DefWindowProcW(hwnd, m, TRUE, lp);
    }
    if (m == WM_KILLFOCUS || (m == WM_ACTIVATEAPP && wp == FALSE)) {
        // Automation: the hidden window is never really focused; keep the menu.
        if (g_open.load() && !g_automation.load(std::memory_order_relaxed))
            g_close_request.store(m == WM_KILLFOCUS ? CloseReason::focus_lost : CloseReason::app_inactive);
        return pass();
    }

    const std::uint32_t toggle = g_toggle_vk.load(std::memory_order_relaxed);
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP) && wp == toggle) {
        // Reaches here when our keyboard hook is not installed, or for a posted key (posted keys never reach WH_KEYBOARD).
        g_toggle_seen_by_messages.store(true, std::memory_order_relaxed);
        if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !(lp & (1LL << 30))) PressToggle(ToggleSource::window_message);
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
    if (!r && g_lock_cfg.load(std::memory_order_relaxed)) {
        // Mouse lock: the game frees the cursor in every menu, the title and the map (its window-centre clip is for
        // gameplay only); while the lock holds, the cursor stays inside the window instead.
        const mouselock::State ls = GatherLock();
        if (mouselock::ReplaceGameRelease(ls)) {
            if (g_lock_replaced.fetch_add(1, std::memory_order_relaxed) == 0)
                Log("[mouse] lock: the game freed the cursor (ClipCursor(NULL)): kept inside the game window instead");
            if (LockClipTo(ls.client)) return TRUE;
        }
    }
    if (!r) {
        ReleaseFromHere();  // not in front: only a clip this game applied itself (never the focused game's)
        return TRUE;
    }
    AcquireSRWLockExclusive(&g_lock_srw);
    const BOOL ok = g_orig_clipcursor(r);
    if (ok) {
        g_lock_ours = g_lock_ours && mouselock::SameRect(*r, g_lock_rect);
        NoteAppliedLocked(r);
    }
    ReleaseSRWLockExclusive(&g_lock_srw);
    return ok;
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
        g_ride_buttons[0].store(p.buttons, std::memory_order_relaxed);
        g_ride_buttons_tick[0].store(GetTickCount64(), std::memory_order_relaxed);
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
    if (r == ERROR_SUCCESS && state && index < 4) {
        g_ride_buttons[index].store(state->Gamepad.wButtons, std::memory_order_relaxed);
        g_ride_buttons_tick[index].store(GetTickCount64(), std::memory_order_relaxed);
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

void SetToggleKey(std::uint32_t vk) noexcept {
    g_toggle_vk.store(vk ? vk : 0x78);
    g_switch_vk.store(mouselock::SwitchKey(g_toggle_vk.load()));
}
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
    if (g_lock_cfg.load()) {
        char key[8];
        std::snprintf(key, sizeof(key), "F%u", static_cast<unsigned>(g_switch_vk.load() - VK_F1 + 1));
        if (SwitchEnabled())
            Log("[mouse] lock ON (local test): the cursor stays in the focused game window; %s switches to the other game's "
                "window, Alt+Tab leaves%s", key, g_automation.load() ? " (hidden run: decisions logged, nothing applied)" : "");
        else
            Log("[mouse] lock ON: the cursor stays in the game window while it is in front; Alt+Tab leaves%s",
                g_automation.load() ? " (hidden run: decisions logged, nothing applied)" : "");
    }
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
    // The mouse lock's switch key finds the other Local test game by this property (FindPeerWindow).
    SetPropW(hwnd, kSlotProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(slot == WindowSlot::left ? 1 : 2)));
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
    LockLetGo("the overlay detached from the game window");
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
    // A physical press of the toggle key while our window really is in front (never in a hidden run: the keyboard is the
    // user's). Normally the hook (or the window) reports it within a frame or two and the poll stands down; a press nobody
    // reported within kPollGraceMs toggles from here, so the menu opens whatever happens to the key on its way.
    if (g_automation.load(std::memory_order_relaxed)) return;
    const HWND hwnd = g_hwnd.load();
    const bool fg = hwnd && RealForeground() == hwnd;
    const bool down = fg && (GetAsyncKeyState(static_cast<int>(g_toggle_vk.load())) & 0x8000) != 0;
    const ULONGLONG now = GetTickCount64();
    if (!down && g_poll_prev) g_poll_ups.fetch_add(1, std::memory_order_relaxed);
    // Key up with nothing pending: a sighting from here on belongs to the next press. (While an edge is pending the flag
    // stays: a tap released before the hook reported it is still matched by a late sighting.)
    if (!down && !g_poll_edge_tick) g_press_since_up.store(false, std::memory_order_release);
    if (down && !g_poll_prev && !g_poll_edge_tick) g_poll_edge_tick = now;
    g_poll_prev = down;
    if (!g_poll_edge_tick) return;
    if (g_press_since_up.load(std::memory_order_acquire)) {  // the hook / the window reported this press (before or after)
        g_poll_edge_tick = 0;
        return;
    }
    if (now - g_poll_edge_tick < kPollGraceMs) return;
    g_poll_edge_tick = 0;
    g_poll_claim_ups.store(g_poll_ups.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_poll_claim_tick.store(now, std::memory_order_release);
    Toggle(ToggleSource::key_poll);
}

CloseReason TakeCloseRequest() noexcept { return g_close_request.exchange(CloseReason::none); }
ToggleInfo LastToggle() noexcept {
    ToggleInfo t;
    t.source = g_toggle_source.load(std::memory_order_relaxed);
    t.tick = g_toggle_tick.load(std::memory_order_relaxed);
    return t;
}
const char* ToggleSourceName(ToggleSource s) noexcept {
    switch (s) {
        case ToggleSource::none: return "-";
        case ToggleSource::hook_noremove: return "keyboard hook, HC_NOREMOVE";
        case ToggleSource::hook_action: return "keyboard hook, HC_ACTION";
        case ToggleSource::window_message: return "window message";
        case ToggleSource::key_poll: return "key-state poll: the keyboard hook did not report the press";
        case ToggleSource::test: return "test";
    }
    return "?";
}
const char* CloseReasonText(CloseReason r) noexcept {
    switch (r) {
        case CloseReason::none: return "-";
        case CloseReason::focus_lost: return "the window lost the focus (WM_KILLFOCUS)";
        case CloseReason::app_inactive: return "another program came to the front (WM_ACTIVATEAPP)";
    }
    return "?";
}
void RequestToggleForTest() noexcept { Toggle(ToggleSource::test); }
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
        // attaches, and the clip is desktop-global); with the mouse lock holding, the menu's cursor stays in the window
        if (!g_automation.load() && hwnd) {
            const mouselock::State ls = GatherLock();
            if (!mouselock::ReplaceGameRelease(ls) || !LockClipTo(ls.client)) ReleaseFromHere();
        }
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
        // The clip is desktop-global: never impose it on another focused application (the real foreground: the guest's
        // GetForegroundWindow is hooked to answer its own window). A release the lock replaces keeps the cursor inside.
        if (apply && hwnd && !g_automation.load() && RealForeground() == hwnd) {
            const mouselock::State ls = GatherLock();
            if (null_clip) {
                if (!mouselock::ReplaceGameRelease(ls) || !LockClipTo(ls.client)) ReleaseFromHere();
            } else {
                AcquireSRWLockExclusive(&g_lock_srw);
                if (RealClipCursor(&r)) NoteAppliedLocked(&r);
                ReleaseSRWLockExclusive(&g_lock_srw);
            }
        }
        ClearEvents();
        ClearHeld();
    }
}

void SetOpen(bool open) noexcept {
    const bool before = Blocking();
    const bool was = g_open.exchange(open);
    if (was == open) return;
    if (open) g_close_request.store(CloseReason::none);
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
std::uint16_t RidePadButtons(unsigned max_age_ms, int* index) noexcept {
    const std::uint64_t now = GetTickCount64();
    std::uint16_t out = 0;
    if (index) *index = -1;
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t t = g_ride_buttons_tick[i].load(std::memory_order_relaxed);
        if (!t || now - t > max_age_ms) continue;
        const std::uint16_t b = g_ride_buttons[i].load(std::memory_order_relaxed);
        if (b && index && *index < 0) *index = i;
        out = static_cast<std::uint16_t>(out | b);
    }
    return out;
}
int RidePadAges(unsigned (&age_ms)[4]) noexcept {
    const std::uint64_t now = GetTickCount64();
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t t = g_ride_buttons_tick[i].load(std::memory_order_relaxed);
        age_ms[i] = t && now - t < 999999 ? static_cast<unsigned>(now - t) : 999999u;
        if (t && now - t < 1000) ++n;
    }
    return n;
}
// The hook's state, confirmed by the key's physical state: a release missed during a focus change (the hook never saw it)
// cannot leave the key "down" (stage 1's known gap). Hidden runs: the key reaches our hook only through the `hookkey`
// command (its release clears it; the hidden window never gets the user's keys), so the hook's state is the answer.
bool RideKeyDown() noexcept {
    if (!g_ride_key.load(std::memory_order_relaxed)) return false;
    if (g_automation.load(std::memory_order_relaxed)) return true;
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

// ---------------------------------------------------------------- mouse lock
void SetMouseLock(bool on) noexcept {
    g_lock_cfg.store(on);
    g_lock_on.store(on);
}
bool MouseLockConfigured() noexcept { return g_lock_cfg.load(); }
void SetMouseLockOn(bool on) noexcept {
    if (!g_lock_cfg.load() || g_lock_on.exchange(on) == on) return;
    Log("[mouse] lock switched %s in the debug menu", on ? "on" : "off");
    if (!on) LockReleaseOurs("switched off in the debug menu");
}

void MouseLockFrame() noexcept {
    if (!g_lock_cfg.load(std::memory_order_relaxed)) return;
    const HWND hwnd = g_hwnd.load(std::memory_order_acquire);
    if (!hwnd) return;
    const bool automation = g_automation.load(std::memory_order_relaxed);
    if (!automation) {
        // Engaged: the cursor was inside the client area since the window came to the front (a click into it, moving
        // in, or the switch key). Coming to the front by the taskbar or Alt+Tab with the cursor elsewhere does not grab it.
        if (RealForeground() != hwnd) {
            g_lock_engaged.store(false, std::memory_order_relaxed);
        } else if (!g_lock_engaged.load(std::memory_order_relaxed)) {
            POINT p{};
            RECT c{};
            if (GetCursorPos(&p) && ClientOnScreen(hwnd, &c) && PtInRect(&c, p)) g_lock_engaged.store(true, std::memory_order_relaxed);
        }
    }
    const mouselock::State s = GatherLock();
    mouselock::State shown = s;
    shown.automation = false;  // a hidden run logs what it would do
    const mouselock::Decision d = mouselock::Decide(automation ? shown : s);
    g_lock_why.store(static_cast<int>(d.why), std::memory_order_relaxed);
    const bool holding = d.why == mouselock::Why::held || d.why == mouselock::Why::game_clip || d.why == mouselock::Why::clipping;
    const int cls = holding ? 100 : static_cast<int>(d.why);
    if (cls != g_lock_logged_class || (holding && !mouselock::SameRect(s.client, g_lock_logged_rect))) {
        static unsigned logged = 0;
        if (++logged <= 300 || logged % 100 == 0) {
            if (holding)
                Log("[mouse] lock%s: %s the cursor inside the game window (client %ld,%ld %ldx%ld)", automation ? " (hidden run, not applied)" : "",
                    automation ? "would hold" : "holding", s.client.left, s.client.top, s.client.right - s.client.left,
                    s.client.bottom - s.client.top);
            else
                Log("[mouse] lock%s: cursor free: %s", automation ? " (hidden run, not applied)" : "", mouselock::WhyText(d.why));
        }
        g_lock_logged_class = cls;
        g_lock_logged_rect = s.client;
    }
    if (automation) return;
    if (d.act == mouselock::Act::clip) LockClipTo(d.rect);
    else if (d.act == mouselock::Act::release) LockReleaseOurs(mouselock::WhyText(d.why));
}

MouseLockStatus GetMouseLockStatus() noexcept {
    MouseLockStatus m;
    m.configured = g_lock_cfg.load();
    m.on = g_lock_on.load();
    m.engaged = g_lock_engaged.load();
    m.switch_enabled = SwitchEnabled();
    m.switch_vk = g_switch_vk.load();
    const int why = g_lock_why.load();
    m.state = why < 0 ? "-" : mouselock::WhyText(static_cast<mouselock::Why>(why));
    m.clips = g_lock_clips.load();
    m.releases = g_lock_releases.load();
    m.replaced = g_lock_replaced.load();
    m.switches = g_switches.load();
    return m;
}

std::uint32_t SwitchKey() noexcept { return g_switch_vk.load(); }

// DLL_PROCESS_DETACH, under the loader lock. On ExitProcess every other thread is already gone, maybe while holding
// g_lock_srw (the render thread takes it every Present): never wait for it. Without the lock the values are read as they
// are (nobody else runs any more); the clip is freed only if it still is our rect.
void ReleaseMouseLockAtExit() noexcept {
    if (!g_lock_cfg.load() || g_automation.load()) return;
    const bool locked = TryAcquireSRWLockExclusive(&g_lock_srw) != FALSE;
    const bool ours = g_lock_ours;
    const RECT r = g_lock_rect;
    g_lock_ours = false;
    if (locked) ReleaseSRWLockExclusive(&g_lock_srw);
    RECT clip{};
    if (ours && GetClipCursor(&clip) && mouselock::SameRect(clip, r)) RealClipCursor(nullptr);
}
UINT HookKeyMessage() noexcept { return HookKeyMessageId(); }

bool RealForegroundIsOurs() noexcept {
    if (g_automation.load(std::memory_order_relaxed)) return false;  // the keyboard is the user's
    DWORD pid = 0;
    GetWindowThreadProcessId(RealForeground(), &pid);
    return pid == GetCurrentProcessId();
}

HookCounts GetHookCounts() noexcept {
    HookCounts h;
    h.noremove = g_hook_noremove.load();
    h.action = g_hook_action.load();
    return h;
}

}  // namespace st::overlay::input
