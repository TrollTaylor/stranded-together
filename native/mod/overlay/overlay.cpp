// In-game debug menu overlay: hooks, ImGui frame and lifecycle. See overlay.hpp.
#include "overlay.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "debug_actions.hpp"
#include "imgui.h"
#include "log.hpp"
#include "overlay_input.hpp"
#include "overlay_keys.hpp"
#include "overlay_menu.hpp"
#include "overlay_renderer.hpp"
#include "overlay_font.hpp"
#include "overlay_tags.hpp"

// ImGui assertion sink (overlay_imconfig.h): log once per site, keep running.
void StOverlayImGuiAssert(const char* expr, const char* file, int line) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static const char* seen_file[32] = {};
    static int seen_line[32] = {};
    static int seen = 0;
    AcquireSRWLockExclusive(&lock);
    bool first = true;
    for (int i = 0; i < seen; ++i)
        if (seen_line[i] == line && seen_file[i] == file) first = false;
    if (first && seen < 32) {
        seen_file[seen] = file;
        seen_line[seen] = line;
        ++seen;
    }
    ReleaseSRWLockExclusive(&lock);
    if (first) {
        const char* base = std::strrchr(file, '\\');
        st::Log("[overlay] ImGui assertion failed (ignored): %s at %s:%d", expr, base ? base + 1 : file, line);
    }
}

namespace st::overlay {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                              IUnknown* const*);
using EclFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
using SetFullscreenStateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);

// vtable slots (COM ABI)
constexpr int kVtSetFullscreenState = 10, kVtPresent = 8, kVtResizeBuffers = 13, kVtPresent1 = 22, kVtResizeBuffers1 = 39;
constexpr int kVtExecuteCommandLists = 10;
constexpr int kVtCreateSwapChain = 10, kVtCreateSwapChainForHwnd = 15;  // IDXGIFactory / IDXGIFactory2

Options g_opt;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_disabled{false};
SRWLOCK g_render = SRWLOCK_INIT;  // Present body, ResizeBuffers bookkeeping, Shutdown
Renderer g_renderer;
ImGuiContext* g_ctx = nullptr;
ImGuiStyle g_base_style;
float g_ui_scale = 0;  // 0 = auto from the back-buffer height
float g_applied_scale = -1;
bool g_open = false;   // render thread's view
std::atomic<int> g_open_request{-1};
thread_local bool t_in_present = false;

// Hooks
void* g_targets[10] = {};
std::size_t g_target_count = 0;
void* g_input_targets[8] = {};
std::size_t g_input_target_count = 0;
PresentFn g_orig_present = nullptr;
Present1Fn g_orig_present1 = nullptr;
ResizeFn g_orig_resize = nullptr;
Resize1Fn g_orig_resize1 = nullptr;
EclFn g_orig_ecl = nullptr;
CreateSwapChainFn g_orig_create_chain = nullptr;
CreateSwapChainForHwndFn g_orig_create_chain_hwnd = nullptr;
// Both creation hooks are in: the renderer is released before the game creates a swapchain. Name
// tags keep the renderer alive in gameplay, so they are drawn only with these hooks.
std::atomic<bool> g_create_hooks{false};
SetFullscreenStateFn g_orig_set_fullscreen = nullptr;
std::atomic<bool> g_logged_fullscreen{false};

// Co-op session screens (SetCoopView): a copy under a lock, drawn by the render thread.
SRWLOCK g_coop_lock = SRWLOCK_INIT;
CoopView g_coop;
std::atomic<bool> g_coop_shown{false};  // the waiting panel or a banner is up
std::atomic<unsigned> g_coop_requests{0};

// Screenshots (RequestCapture / TakeCapture). Serviced inside Present.
struct CaptureSlot {
    std::uint32_t id = 0;  // 0 = free
    int state = 0;         // 0 pending, 1 done, -1 failed
    unsigned waited = 0;   // frames spent waiting for the game's queue
    Image image;
    char why[160] = {};
};
constexpr int kCaptureSlots = 4;
SRWLOCK g_capture_lock = SRWLOCK_INIT;
CaptureSlot g_captures[kCaptureSlots];
std::atomic<std::uint32_t> g_capture_next{1};
std::atomic<int> g_capture_pending{0};

// The queue each swapchain was created on (IDXGIFactory::CreateSwapChain* hooks).
// Compare-only, no references: the queue is used only while that very swapchain
// is being presented, and a live swapchain holds its own queue reference.
SRWLOCK g_created_lock = SRWLOCK_INIT;
IDXGISwapChain* g_created_chain = nullptr;
ID3D12CommandQueue* g_created_queue = nullptr;

// Game DIRECT-queue capture (ExecuteCommandLists hook, fallback)
SRWLOCK g_cap_lock = SRWLOCK_INIT;
std::atomic<bool> g_cap_wanted{false};
std::atomic<ID3D12Device*> g_cap_device{nullptr};      // compared only, never dereferenced
std::atomic<ID3D12CommandQueue*> g_exclude_queue{nullptr};  // our own upload queue
ID3D12CommandQueue* g_cap_queue = nullptr;              // AddRef'd: newest DIRECT queue of the device
ID3D12CommandQueue* g_cap_queue_pt = nullptr;           // AddRef'd: same, submitted from the presenting thread
std::atomic<DWORD> g_present_thread{0};
unsigned long g_queue_wait_frames = 0;
bool g_logged_queue_wait = false;
unsigned g_closed_frames = 0;  // closed-menu frames with the renderer still alive

// Window attach
IDXGISwapChain* g_attached_chain = nullptr;  // compared only

// Stats / timing
SRWLOCK g_stats_lock = SRWLOCK_INIT;
Stats g_stats;
LARGE_INTEGER g_qpf{}, g_last_frame{}, g_fps_window{};
unsigned g_fps_frames = 0;
unsigned g_fail_streak = 0;
bool g_logged_unsupported = false;
bool g_logged_first_draw = false;

// Startup hint
bool g_hint_done = false;
double g_hint_until = 0;
double g_hint_valid_since = 0;  // the same local player valid, nothing loading, without a break since
std::uintptr_t g_hint_entity = 0;

double NowSeconds() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(g_qpf.QuadPart);
}

template <class T>
void SafeRelease(T*& p) noexcept {
    if (p) p->Release();
    p = nullptr;
}

void UpdateStats(void (*fn)(Stats&, const void*), const void* arg) noexcept {
    AcquireSRWLockExclusive(&g_stats_lock);
    fn(g_stats, arg);
    ReleaseSRWLockExclusive(&g_stats_lock);
}

void SetQueueSource(const char* text) noexcept {
    UpdateStats([](Stats& s, const void* a) { std::snprintf(s.queue_source, sizeof(s.queue_source), "%s", static_cast<const char*>(a)); },
                text);
}

// Keys and characters the open menu received (logged when it closes: proves typing reaches ImGui).
unsigned long g_open_keys0 = 0, g_open_chars0 = 0;

// `why`: what opened / closed it, for the log line ("by F9 (keyboard hook, HC_NOREMOVE)", "by Esc", ...).
void ApplyOpen(bool open, const char* why) noexcept {
    if (open == g_open) return;
    g_open = open;
    input::SetOpen(open);
    debug::SetMenuOpen(open);
    if (g_ctx) {
        ImGui::SetCurrentContext(g_ctx);
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        float x = 0, y = 0;
        const Stats st = GetStats();
        if (open && input::CursorClientPos(&x, &y)) {
            RECT rc{};
            if (GetClientRect(input::Window(), &rc) && rc.right > 0 && rc.bottom > 0 && st.width && st.height)
                ImGui::GetIO().AddMousePosEvent(x * static_cast<float>(st.width) / static_cast<float>(rc.right),
                                                y * static_cast<float>(st.height) / static_cast<float>(rc.bottom));
        }
    }
    if (open) g_hint_done = true;
    const input::Stats in = input::GetStats();
    if (open) {
        g_open_keys0 = in.blocked_keys, g_open_chars0 = in.characters;
        Log("[overlay] menu opened %s (game input blocked)", why);
    } else {
        Log("[overlay] menu closed %s (game input restored; the menu got %lu key(s), %lu character(s))", why,
            in.blocked_keys - g_open_keys0, in.characters - g_open_chars0);
    }
}

void ReleaseRenderer() noexcept;

// Turns the overlay off for the rest of the run. Render lock held.
void Disable(const char* reason) noexcept {
    if (g_disabled.exchange(true)) return;
    Log("[overlay] DISABLED for this run: %s (the game is unaffected)", reason);
    UpdateStats([](Stats& s, const void* a) {
        s.disabled = true;
        std::snprintf(s.disabled_reason, sizeof(s.disabled_reason), "%s", static_cast<const char*>(a));
    }, reason);
    ApplyOpen(false, "because the overlay was disabled");
    ReleaseRenderer();
    input::Detach();
}

void CreateImGui() noexcept {
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written into the game folder
    io.LogFilename = nullptr;
    io.BackendPlatformName = "stranded_together_overlay";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    wchar_t windir[MAX_PATH] = {};
    char font[MAX_PATH * 2] = {};
    bool loaded = false;
    if (GetWindowsDirectoryW(windir, MAX_PATH)) {
        std::snprintf(font, sizeof(font), "%ls\\Fonts\\segoeui.ttf", windir);
        if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) loaded = io.Fonts->AddFontFromFileTTF(font, 17.0f) != nullptr;
    }
    if (!loaded) io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6;
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.TabRounding = 4;
    st.WindowBorderSize = 1;
    st.Colors[ImGuiCol_WindowBg].w = 0.94f;
    g_base_style = st;
}

void ApplyScale(UINT height) noexcept {
    if (g_ui_scale <= 0) {
        float s = static_cast<float>(height) / 1080.0f;
        g_ui_scale = s < 1.0f ? 1.0f : (s > 3.0f ? 3.0f : s);
    }
    if (g_ui_scale < 0.5f) g_ui_scale = 0.5f;
    if (g_ui_scale > 4.0f) g_ui_scale = 4.0f;
    if (std::fabs(g_applied_scale - g_ui_scale) < 0.001f) return;
    ImGuiStyle& st = ImGui::GetStyle();
    st = g_base_style;
    st.ScaleAllSizes(g_ui_scale);
    st.FontScaleMain = g_ui_scale;
    g_applied_scale = g_ui_scale;
}

void FeedInput(UINT bb_w, UINT bb_h) noexcept {
    ImGuiIO& io = ImGui::GetIO();
    float sx = 1, sy = 1;
    RECT rc{};
    const HWND hwnd = input::Window();
    if (hwnd && GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
        sx = static_cast<float>(bb_w) / static_cast<float>(rc.right);
        sy = static_cast<float>(bb_h) / static_cast<float>(rc.bottom);
    }
    input::Event ev[128];
    std::size_t n;
    while ((n = input::Drain(ev, 128)) > 0) {
        for (std::size_t i = 0; i < n; ++i) {
            const input::Event& e = ev[i];
            switch (e.type) {
                case input::EventType::key: {
                    if (e.vk == VK_SHIFT || e.vk == VK_LSHIFT || e.vk == VK_RSHIFT) io.AddKeyEvent(ImGuiMod_Shift, e.down);
                    if (e.vk == VK_CONTROL || e.vk == VK_LCONTROL || e.vk == VK_RCONTROL) io.AddKeyEvent(ImGuiMod_Ctrl, e.down);
                    if (e.vk == VK_MENU || e.vk == VK_LMENU || e.vk == VK_RMENU) io.AddKeyEvent(ImGuiMod_Alt, e.down);
                    const ImGuiKey k = VkToImGuiKey(e.vk, e.extended);
                    if (k != ImGuiKey_None) io.AddKeyEvent(k, e.down);
                    break;
                }
                case input::EventType::character: io.AddInputCharacterUTF16(static_cast<ImWchar16>(e.ch)); break;
                case input::EventType::mouse_move: io.AddMousePosEvent(e.x * sx, e.y * sy); break;
                case input::EventType::mouse_button:
                    if (e.button >= 0 && e.button < 5) io.AddMouseButtonEvent(e.button, e.down);
                    break;
                case input::EventType::wheel: io.AddMouseWheelEvent(e.x, e.y); break;
            }
        }
    }
}

bool HintActive(double now) noexcept {
    if (!g_opt.startup_hint || !g_opt.menu || g_hint_done) return false;
    if (g_hint_until > 0) {
        if (now < g_hint_until) return true;
        g_hint_done = true;
        return false;
    }
    const debug::Snapshot s = debug::GetSnapshot();
    // The player entity comes and goes while a save loads (hidden run 20261120-092717: valid at 27.6 s, gone, another
    // entity at 31.3 s, the world on screen at 36.4 s; the published snapshot can miss the short gaps): the 12 s start
    // once the same entity stayed valid with nothing loading for 4 s, so the hint is seen in the world, not the loading.
    const bool loading = s.diag.module_valid && s.diag.load_mask != 0;
    if (!s.local_valid || loading || s.local_entity != g_hint_entity) {
        g_hint_entity = s.local_valid ? s.local_entity : 0;
        g_hint_valid_since = s.local_valid && !loading ? now : 0;
        return false;
    }
    if (g_hint_valid_since <= 0) g_hint_valid_since = now;
    if (now - g_hint_valid_since < 4.0) return false;
    g_hint_until = now + 12.0;
    return true;
}

CoopView CoopCopy() noexcept {
    AcquireSRWLockShared(&g_coop_lock);
    CoopView v = g_coop;
    ReleaseSRWLockShared(&g_coop_lock);
    return v;
}

// Text centred at y with an explicit size (the draw list: no window, no layout).
float CentredText(ImDrawList* dl, float y, float size, ImU32 color, const char* text) noexcept {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(size, ds.x * 0.9f, ds.x * 0.9f, text);
    dl->AddText(ImGui::GetFont(), size, ImVec2((ds.x - sz.x) * 0.5f, y), color, text, nullptr, ds.x * 0.9f);
    return sz.y;
}

// The guest's waiting screen: opaque, full screen, the host's progress and the Leave button.
void DrawCoopWaiting(const CoopView& v) noexcept {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float base = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ds);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.055f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("###st_coop_waiting", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float y = ds.y * 0.30f;
        y += CentredText(dl, y, base * 2.4f, IM_COL32(235, 240, 245, 255), v.title[0] ? v.title : "Waiting for the host to start the game");
        y += base * 1.6f;
        for (const auto& line : v.lines) {
            if (!line[0]) continue;
            y += CentredText(dl, y, base * 1.25f, IM_COL32(170, 182, 196, 255), line) + base * 0.5f;
        }
        if (v.progress >= 0.0f) {
            y += base * 1.0f;
            const float w = ds.x * 0.36f, h = base * 0.9f, x = (ds.x - w) * 0.5f;
            const float p = v.progress > 1.0f ? 1.0f : v.progress;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(40, 48, 60, 255), h * 0.3f);
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w * p, y + h), IM_COL32(90, 170, 230, 255), h * 0.3f);
            y += h + base * 0.5f;
            if (v.progress_text[0]) y += CentredText(dl, y, base * 1.1f, IM_COL32(170, 182, 196, 255), v.progress_text);
        }
        if (v.leave) {
            const ImVec2 bs(base * 9.0f, base * 2.2f);
            ImGui::SetCursorPos(ImVec2((ds.x - bs.x) * 0.5f, ds.y * 0.78f));
            if (ImGui::Button("Leave", bs)) {
                // Closed from here, not from the game thread's tick: Leave works even when that tick no longer runs.
                g_coop_requests.fetch_or(kCoopLeave);
                const HWND w = input::Window();
                if (w) PostMessageW(w, WM_CLOSE, 0, 0);
                Log("[overlay] co-op waiting screen: Leave pressed: %s", w ? "WM_CLOSE posted to the game window" : "no game window known");
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void DrawCoopBanner(const CoopView& v) noexcept {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float size = ImGui::GetFontSize() * 1.3f, pad = 10.0f * g_ui_scale;
    const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(size, ds.x * 0.8f, ds.x * 0.8f, v.banner);
    // Below the startup hint's box (DrawHint: 16 px from the top, its text plus 8 px padding each side), which it overlapped
    // in a small (local test) window.
    const float hint_bottom = (16.0f + 2 * 8.0f) * g_ui_scale + ImGui::GetFontSize();
    const ImVec2 at((ds.x - sz.x) * 0.5f, std::fmax(ds.y * 0.06f, hint_bottom + pad + 6.0f * g_ui_scale));
    dl->AddRectFilled(ImVec2(at.x - pad, at.y - pad), ImVec2(at.x + sz.x + pad, at.y + sz.y + pad), IM_COL32(10, 14, 20, 215), 6.0f);
    dl->AddText(ImGui::GetFont(), size, at, v.banner_warn ? IM_COL32(250, 192, 80, 255) : IM_COL32(235, 240, 245, 255), v.banner,
                nullptr, ds.x * 0.8f);
}

// The startup hint's text: the menu key, and in the Local test with the mouse lock the switch key and the way out.
void HintText(char* text, std::size_t size) noexcept {
    char key[24], sw[24];
    VkName(input::ToggleKey(), key, sizeof(key));
    const input::MouseLockStatus ml = input::GetMouseLockStatus();
    if (ml.configured && ml.switch_enabled)
        std::snprintf(text, size, "Stranded Together %s  |  %s: debug menu  |  %s: other game window  |  Alt+Tab: leave", g_opt.build.c_str(), key,
                      VkName(ml.switch_vk, sw, sizeof(sw)));
    else if (ml.configured)
        std::snprintf(text, size, "Stranded Together %s  |  %s: debug menu  |  Alt+Tab: leave", g_opt.build.c_str(), key);
    else
        std::snprintf(text, size, "Stranded Together %s  |  %s: debug menu", g_opt.build.c_str(), key);
}

void DrawHint() noexcept {
    char text[200];
    HintText(text, sizeof(text));
    static bool logged = false;
    if (!logged) Log("[overlay] startup hint shown: %s", text);
    logged = true;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float pad = 8.0f * g_ui_scale;
    const ImVec2 at(16.0f * g_ui_scale, 16.0f * g_ui_scale);
    dl->AddRectFilled(at, ImVec2(at.x + size.x + 2 * pad, at.y + size.y + 2 * pad), IM_COL32(10, 14, 20, 200), 6.0f);
    dl->AddText(ImVec2(at.x + pad, at.y + pad), IM_COL32(235, 240, 245, 255), text);
}

// Finds the DIRECT queue the swapchain presents on. Returns an AddRef'd queue
// or null (then the ExecuteCommandLists hook is asked to capture one).
ID3D12CommandQueue* FindQueue(IDXGISwapChain* chain, ID3D12Device* device) noexcept {
    ID3D12CommandQueue* q = nullptr;
    // D3D12 swapchains are created on a queue; DXGI hands it back as the "device".
    if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&q))) && q) {
        if (q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            SetQueueSource("swapchain (GetDevice)");
            return q;
        }
        SafeRelease(q);
    }
    // Recorded when the game created this swapchain (the reliable path: DXGI's
    // GetDevice above does not hand the queue back on current Windows).
    AcquireSRWLockShared(&g_created_lock);
    if (g_created_chain == chain && g_created_queue) {
        q = g_created_queue;
        q->AddRef();  // alive: the swapchain being presented holds a reference to it
    }
    ReleaseSRWLockShared(&g_created_lock);
    if (q) {
        if (q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            SetQueueSource("swapchain creation (CreateSwapChain hook)");
            return q;
        }
        SafeRelease(q);
    }
    // Captured by the ExecuteCommandLists hook. Prefer the queue the presenting
    // thread submits to (the game's frame queue), else the newest DIRECT queue.
    bool presenting = false;
    AcquireSRWLockExclusive(&g_cap_lock);
    if (g_cap_device.load() == device) {
        if (g_cap_queue_pt) q = g_cap_queue_pt, presenting = true;
        else q = g_cap_queue;
        if (q) q->AddRef();
    }
    if (!q) {  // (re)arm for this device; drop queues of an older device
        SafeRelease(g_cap_queue);
        SafeRelease(g_cap_queue_pt);
        g_cap_device.store(device);
    }
    ReleaseSRWLockExclusive(&g_cap_lock);
    if (q) {
        g_cap_wanted.store(false);
        SetQueueSource(presenting ? "ExecuteCommandLists (present thread)" : "ExecuteCommandLists");
        return q;
    }
    g_cap_wanted.store(true);
    return nullptr;
}

bool InitRenderer(IDXGISwapChain* chain) noexcept {
    IDXGISwapChain3* chain3 = nullptr;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3))) || !chain3) {
        Disable("the swapchain is not an IDXGISwapChain3 (not Direct3D 12 flip model)");
        return false;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        chain3->Release();
        Disable("the swapchain has no Direct3D 12 device");
        return false;
    }
    ID3D12CommandQueue* queue = FindQueue(chain, device);
    if (!queue) {
        device->Release();
        chain3->Release();
        ++g_queue_wait_frames;
        if (!g_logged_queue_wait) Log("[overlay] waiting for the game's DIRECT command queue (ExecuteCommandLists)");
        g_logged_queue_wait = true;
        if (g_queue_wait_frames > 600) Disable("the game's DIRECT command queue was never seen");
        return false;
    }
    // Delay-loaded by this DLL: make sure the loads cannot fail inside ImGui.
    if (!LoadLibraryW(L"d3dcompiler_47.dll")) {
        queue->Release();
        device->Release();
        chain3->Release();
        Disable("d3dcompiler_47.dll is not loadable");
        return false;
    }
    if (!g_ctx) CreateImGui();
    ImGui::SetCurrentContext(g_ctx);
    char why[160] = {};
    const bool ok = g_renderer.Init(chain3, queue, why, sizeof(why));
    queue->Release();
    device->Release();
    chain3->Release();
    if (!ok) {
        char reason[220];
        std::snprintf(reason, sizeof(reason), "renderer init: %s", why);
        Disable(reason);
        return false;
    }
    g_exclude_queue.store(g_renderer.UploadQueue());
    g_queue_wait_frames = 0;
    UINT w = 0, h = 0;
    DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
    g_renderer.BackBufferInfo(&w, &h, &f);
    UpdateStats([](Stats& s, const void*) { ++s.renderer_inits; }, nullptr);
    Stats snap = GetStats();
    Log("[overlay] renderer ready: %ux%u format %d, queue from %s", w, h, static_cast<int>(f), snap.queue_source);
    return true;
}

void AttachWindow(IDXGISwapChain* chain) noexcept {
    if (g_attached_chain == chain) return;
    g_attached_chain = chain;
    HWND hwnd = nullptr;
    IDXGISwapChain1* c1 = nullptr;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&c1))) && c1) {
        c1->GetHwnd(&hwnd);
        c1->Release();
    }
    if (!hwnd) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(chain->GetDesc(&d))) hwnd = d.OutputWindow;
    }
    if (!hwnd) {
        Log("[overlay] swapchain %p has no window (composition swapchain?): no menu input", static_cast<void*>(chain));
        return;
    }
    input::Attach(hwnd);
}

// Drops every D3D12 object (and queue reference) we hold. Render lock held.
void ReleaseRenderer() noexcept {
    if (g_ctx) ImGui::SetCurrentContext(g_ctx);
    if (g_renderer.Ready()) UpdateStats([](Stats& s, const void*) { ++s.renderer_releases; }, nullptr);
    g_renderer.Shutdown();
    g_exclude_queue.store(nullptr);
    AcquireSRWLockExclusive(&g_cap_lock);
    SafeRelease(g_cap_queue);
    SafeRelease(g_cap_queue_pt);
    ReleaseSRWLockExclusive(&g_cap_lock);
    g_cap_wanted.store(false);
    g_closed_frames = 0;
}

// Closed menu: hold nothing of the game's. A kept swapchain reference would make
// the game's next CreateSwapChainForHwnd on its window fail (one flip-model
// swapchain per HWND), and a kept queue keeps a removed device alive. Released
// once our last draw completed (never blocks), or after ~2 s of frames at most.
void ReleaseRendererWhenIdle() noexcept {
    if (!g_renderer.Ready()) return;
    if (g_renderer.Idle() || ++g_closed_frames > 120) ReleaseRenderer();
}

void FrameMain(IDXGISwapChain* chain) noexcept {
    const double now = NowSeconds();
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        ++g_fps_frames;
        const double window = static_cast<double>(t.QuadPart - g_fps_window.QuadPart) / static_cast<double>(g_qpf.QuadPart);
        if (g_fps_window.QuadPart == 0) g_fps_window = t;
        else if (window >= 1.0) {
            const float fps = static_cast<float>(g_fps_frames / window);
            UpdateStats([](Stats& s, const void* a) { s.fps = *static_cast<const float*>(a); }, &fps);
            g_fps_frames = 0;
            g_fps_window = t;
        }
        UpdateStats([](Stats& s, const void*) { ++s.presents; }, nullptr);
    }

    const int req = g_open_request.exchange(-1);
    if (g_opt.menu) {  // name tags only (debug_menu=0): no window hooks, no toggles, no menu
        AttachWindow(chain);
        input::PollToggleFallback();
        if (req >= 0) ApplyOpen(req != 0, "by request (scenario `menu` / SetOpen)");
        if (const unsigned toggles = input::TakeToggles()) {
            const input::ToggleInfo ti = input::LastToggle();
            char key[24], why[120];
            VkName(input::ToggleKey(), key, sizeof(key));
            std::snprintf(why, sizeof(why), "by %s (%s)", key, input::ToggleSourceName(ti.source));
            if (toggles & 1u) ApplyOpen(!g_open, why);
            else Log("[overlay] %s pressed %u times within one frame (%s): no change", key, toggles, input::ToggleSourceName(ti.source));
            const unsigned long long waited = ti.tick ? GetTickCount64() - ti.tick : 0;
            if (waited > 500) Log("[overlay] %s waited %llu ms for a Present (minimised or loading)", key, waited);
        }
        if (const input::CloseReason cr = input::TakeCloseRequest(); cr != input::CloseReason::none) {
            char why[96];
            std::snprintf(why, sizeof(why), "because %s", input::CloseReasonText(cr));
            ApplyOpen(false, why);
        }
        input::MouseLockFrame();
    }

    const bool hint = !g_open && HintActive(now);
    // Name tags (overlay_tags.hpp): drawn while the game thread publishes a fresh, visible frame.
    bool name_tags = tags::Wanted();
    if (name_tags && !g_create_hooks.load()) {
        static bool logged = false;
        if (!logged) Log("[overlay] name tags not drawn: the swapchain creation hooks are unavailable (a kept renderer could block the game's swapchain re-creation)");
        logged = true;
        name_tags = false;
    }
    // The co-op screens (SetCoopView) stay up for long, like the name tags: only with the swapchain creation hooks.
    bool coop = g_coop_shown.load(std::memory_order_acquire);
    if (coop && !g_create_hooks.load()) {
        static bool logged = false;
        if (!logged) Log("[overlay] co-op screens not drawn: the swapchain creation hooks are unavailable");
        logged = true;
        coop = false;
    }
    if (!g_open && !hint && !name_tags && !coop) {  // closed menu, no tags: nothing is drawn, nothing is kept
        ReleaseRendererWhenIdle();
        return;
    }
    g_closed_frames = 0;

    if (g_renderer.Ready() && g_renderer.Chain() != chain) {
        Log("[overlay] the game presents on a new swapchain: rebuilding the renderer");
        ReleaseRenderer();
    }
    if (!g_renderer.Ready() && !InitRenderer(chain)) return;

    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (!g_renderer.BackBufferInfo(&w, &h, &fmt) || !w || !h) {
        if (++g_fail_streak > 300) Disable("the back buffer could not be read for 300 frames");
        return;
    }
    if (!g_renderer.EnsureFormat(fmt)) {
        if (RtvFormatFor(fmt) == DXGI_FORMAT_UNKNOWN) {
            if (!g_logged_unsupported) Log("[overlay] back-buffer format %d is not supported: menu not drawn", static_cast<int>(fmt));
            g_logged_unsupported = true;
            // An open menu that cannot be seen must not keep the game's input.
            if (g_open) {
                char why[96];
                std::snprintf(why, sizeof(why), "again at once: back-buffer format %d is not supported", static_cast<int>(fmt));
                ApplyOpen(false, why);
            }
            return;
        }
        Disable("could not rebuild the ImGui pipeline for a new back-buffer format");
        return;
    }
    struct Size {
        UINT w, h;
        DXGI_FORMAT f;
    } size{w, h, fmt};
    UpdateStats([](Stats& s, const void* a) {
        const auto* z = static_cast<const Size*>(a);
        s.width = z->w, s.height = z->h, s.format = static_cast<int>(z->f);
    }, &size);

    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    io.DeltaTime = g_last_frame.QuadPart ? static_cast<float>(static_cast<double>(t.QuadPart - g_last_frame.QuadPart) /
                                                              static_cast<double>(g_qpf.QuadPart))
                                         : 1.0f / 60.0f;
    if (io.DeltaTime <= 0 || io.DeltaTime > 1.0f) io.DeltaTime = 1.0f / 60.0f;
    g_last_frame = t;
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    CoopView coop_view = coop || g_open ? CoopCopy() : CoopView{};
    if (!coop) coop_view.waiting = false, coop_view.banner[0] = 0;  // the menu header only
    io.MouseDrawCursor = g_open || (coop_view.waiting && coop_view.leave);
    ApplyScale(h);
    FeedInput(w, h);

    if (name_tags) font::BetweenFrames();  // the atlas is unlocked here (outside NewFrame..Render)
    ImGui::NewFrame();
    if (name_tags) tags::Draw(ImGui::GetBackgroundDrawList(), static_cast<float>(w), static_cast<float>(h), g_ui_scale);
    if (coop_view.waiting) DrawCoopWaiting(coop_view);  // under the menu
    if (g_open) {
        const debug::Snapshot snap = debug::GetSnapshot();
        const Stats stats = GetStats();
        menu::Context ctx;
        ctx.snap = &snap;
        ctx.stats = &stats;
        ctx.input = input::GetStats();
        ctx.toggle_vk = input::ToggleKey();
        ctx.build = g_opt.build.c_str();
        ctx.ui_scale = &g_ui_scale;
        ctx.coop = coop_view.active ? &coop_view : nullptr;
        menu::Draw(ctx);
        if (ctx.close) ApplyOpen(false, ctx.close_reason);
        if (ctx.coop_teleport) g_coop_requests.fetch_or(kCoopTeleport);
    } else if (hint && !coop_view.waiting) {
        DrawHint();
    }
    if (coop_view.banner[0]) DrawCoopBanner(coop_view);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (g_renderer.Draw(dd)) {
        g_fail_streak = 0;
        struct DrawInfo {
            int v, l;
        } di{dd->TotalVtxCount, dd->CmdListsCount};
        UpdateStats([](Stats& s, const void* a) {
            const auto* d = static_cast<const DrawInfo*>(a);
            ++s.frames_drawn;
            s.last_vertices = d->v;
            s.last_draw_lists = d->l;
        }, &di);
        if (!g_logged_first_draw) {
            g_logged_first_draw = true;
            Log("[overlay] first frame drawn (%d vertices)", di.v);
        }
    } else if (++g_fail_streak > 300) {
        Disable("300 consecutive frames could not be drawn");
    }
}

void FailCapture(CaptureSlot& c, const char* why) noexcept {
    c.state = -1;
    std::snprintf(c.why, sizeof(c.why), "%s", why);
    g_capture_pending.fetch_sub(1);
}

// Pending screenshots: a copy of the back buffer being presented, taken on the
// game's DIRECT queue after the frame (and our menu draw, if any). Render lock held.
void ServiceCaptures(IDXGISwapChain* chain) noexcept {
    if (g_capture_pending.load(std::memory_order_acquire) <= 0) return;
    IDXGISwapChain3* chain3 = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    char why[160] = "the swapchain is not a Direct3D 12 flip-model swapchain";
    bool fatal = false;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3))) || !chain3 || FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || !device)
        fatal = true;
    // Same lookup as the renderer: the queue this swapchain presents on.
    if (!fatal) queue = FindQueue(chain, device);
    Image img;
    bool ok = false;
    if (queue) ok = CaptureBackBuffer(chain3, queue, &img, why, sizeof(why), 3000);
    AcquireSRWLockExclusive(&g_capture_lock);
    bool served = false;
    for (CaptureSlot& c : g_captures) {
        if (!c.id || c.state != 0) continue;
        if (fatal) {
            FailCapture(c, why);
        } else if (!queue) {
            if (++c.waited > 300) FailCapture(c, "the game's DIRECT command queue was not found within 300 frames");
        } else if (!ok) {
            FailCapture(c, why);
        } else if (!served) {
            c.image = std::move(img);  // noexcept move
            c.state = 1;
            served = true;
            g_capture_pending.fetch_sub(1);
        } else {
            break;  // one capture per frame; the next request gets the next frame
        }
    }
    ReleaseSRWLockExclusive(&g_capture_lock);
    if (ok) {
        static LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 3) Log("[overlay] screenshot taken (%ux%u)", img.width, img.height);
    }
    if (queue) queue->Release();
    if (device) device->Release();
    if (chain3) chain3->Release();
    // A queue captured only for this screenshot is not kept while the menu is closed.
    if (!g_renderer.Ready()) {
        AcquireSRWLockExclusive(&g_cap_lock);
        if (g_cap_queue || g_cap_queue_pt) {
            SafeRelease(g_cap_queue);
            SafeRelease(g_cap_queue_pt);
            if (g_capture_pending.load() <= 0) g_cap_wanted.store(false);
        }
        ReleaseSRWLockExclusive(&g_cap_lock);
    }
}

void Frame(IDXGISwapChain* chain) noexcept {
    FrameMain(chain);
    ServiceCaptures(chain);
}

// Last-resort guard: no C++ objects with destructors live in this frame.
bool GuardedFrame(IDXGISwapChain* chain) noexcept {
    __try {
        Frame(chain);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The overlay turned itself off: a press of the menu key gets a log line with the reason (at most one a second), so a
// play-test log says why F9 did nothing.
void NoteToggleWhileDisabled() noexcept {
    if (!g_opt.menu || input::Automation()) return;
    static bool prev = false;
    static ULONGLONG last = 0;
    const bool down = input::RealForegroundIsOurs() && (GetAsyncKeyState(static_cast<int>(input::ToggleKey())) & 0x8000) != 0;
    const ULONGLONG now = GetTickCount64();
    if (down && !prev && now - last > 1000) {
        last = now;
        char key[24];
        const Stats st = GetStats();
        Log("[overlay] %s ignored: the overlay is disabled (%s)", VkName(input::ToggleKey(), key, sizeof(key)), st.disabled_reason);
    }
    prev = down;
}

void OnPresent(IDXGISwapChain* chain, UINT flags) noexcept {
    if (chain && !(flags & DXGI_PRESENT_TEST) && g_disabled.load(std::memory_order_acquire) && !t_in_present) NoteToggleWhileDisabled();
    if ((flags & DXGI_PRESENT_TEST) || !chain || g_disabled.load(std::memory_order_acquire) || t_in_present) return;
    t_in_present = true;
    g_present_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_render);
    if (!GuardedFrame(chain)) Disable("exception inside the overlay frame");
    ReleaseSRWLockExclusive(&g_render);
    t_in_present = false;
}

// Automation: on a never-displayed desktop every Present answers DXGI_STATUS_OCCLUDED
// (measured by st_testenv_probe), and games commonly stop rendering while occluded.
// The hidden game is told its frames are shown; the frames are still rendered and
// paced by vsync (the probe measured 144 Hz with Present(1, 0)).
HRESULT MaskOcclusion(HRESULT hr) noexcept {
    if (hr != DXGI_STATUS_OCCLUDED || !input::Automation()) return hr;
    UpdateStats([](Stats& s, const void*) { ++s.occluded_masked; }, nullptr);
    return S_OK;
}

void AfterPresent(IDXGISwapChain* chain, HRESULT hr) noexcept {
    if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) return;
    AcquireSRWLockExclusive(&g_render);
    if (g_renderer.Ready() && g_renderer.Chain() == chain) {
        Log("[overlay] device lost (hr=0x%08lX): renderer released; it is rebuilt on the next device", static_cast<unsigned long>(hr));
        ReleaseRenderer();
    }
    ReleaseSRWLockExclusive(&g_render);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* chain, UINT sync, UINT flags) {
    const DWORD err = GetLastError();
    OnPresent(chain, flags);
    SetLastError(err);
    const HRESULT hr = g_orig_present(chain, sync, flags);
    AfterPresent(chain, hr);
    return MaskOcclusion(hr);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* chain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    const DWORD err = GetLastError();
    OnPresent(chain, flags);
    SetLastError(err);
    const HRESULT hr = g_orig_present1(chain, sync, flags, p);
    AfterPresent(chain, hr);
    return MaskOcclusion(hr);
}

void BeforeResize(IDXGISwapChain* chain) noexcept {
    AcquireSRWLockExclusive(&g_render);
    if (g_renderer.Ready() && g_renderer.Chain() == chain && !g_renderer.WaitIdle(2000))
        Log("[overlay] ResizeBuffers: our last draw did not finish within 2 s");
    UpdateStats([](Stats& s, const void*) { ++s.resizes; }, nullptr);
    ReleaseSRWLockExclusive(&g_render);
}

void AfterResize(HRESULT hr, UINT w, UINT h, DXGI_FORMAT f) noexcept {
    static LONG logged = 0;
    if (FAILED(hr) || InterlockedIncrement(&logged) <= 5)
        Log("[overlay] game ResizeBuffers(%u x %u, format %d) -> 0x%08lX", w, h, static_cast<int>(f), static_cast<unsigned long>(hr));
}

HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* chain, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    BeforeResize(chain);
    const HRESULT hr = g_orig_resize(chain, n, w, h, f, flags);
    AfterResize(hr, w, h, f);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResize1(IDXGISwapChain3* chain, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
                                      const UINT* masks, IUnknown* const* queues) {
    BeforeResize(chain);
    const HRESULT hr = g_orig_resize1(chain, n, w, h, f, flags, masks, queues);
    AfterResize(hr, w, h, f);
    return hr;
}

void CaptureQueue(ID3D12CommandQueue* q) noexcept {
    if (q == g_exclude_queue.load(std::memory_order_relaxed)) return;
    if (q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    ID3D12Device* d = nullptr;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&d))) || !d) return;
    const bool same = d == g_cap_device.load();
    d->Release();
    if (!same) return;
    const bool presenting = GetCurrentThreadId() == g_present_thread.load(std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_cap_lock);
    ID3D12CommandQueue*& slot = presenting ? g_cap_queue_pt : g_cap_queue;
    if (slot != q) {
        if (slot) slot->Release();
        slot = q;
        q->AddRef();
    }
    ReleaseSRWLockExclusive(&g_cap_lock);
}

void STDMETHODCALLTYPE HookExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (g_cap_wanted.load(std::memory_order_relaxed) && q) CaptureQueue(q);
    g_orig_ecl(q, n, lists);
}

// A new swapchain: remember the D3D12 queue it presents on (no references kept).
void RecordCreatedChain(IUnknown* device, IDXGISwapChain* chain) noexcept {
    if (!device || !chain) return;
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&q)))) q = nullptr;  // a D3D11 device: not ours to draw on
    AcquireSRWLockExclusive(&g_created_lock);
    if (q) {
        g_created_chain = chain;
        g_created_queue = q;
    } else if (g_created_chain == chain) {  // same address reused by a non-D3D12 swapchain
        g_created_chain = nullptr;
        g_created_queue = nullptr;
    }
    ReleaseSRWLockExclusive(&g_created_lock);
    if (q) q->Release();
}

// The game creates a swapchain (start, or a display change): drop our renderer first. It holds
// a reference to the old swapchain, and DXGI allows one flip-model swapchain per window, so a
// kept reference would make the game's creation fail. Matters while name tags keep the
// renderer alive in gameplay; the menu and the hint release it when closed anyway.
void ReleaseRendererForNewChain() noexcept {
    if (t_in_present) return;  // not from inside our own frame (never seen; the lock is not recursive)
    AcquireSRWLockExclusive(&g_render);
    if (g_renderer.Ready()) {
        Log("[overlay] the game creates a new swapchain: renderer released first");
        ReleaseRenderer();
    }
    ReleaseSRWLockExclusive(&g_render);
}

// Automation: a hidden run must never take the user's display (exclusive fullscreen).
void LogFullscreenRefused(const char* where) noexcept {
    if (!g_logged_fullscreen.exchange(true)) Log("[overlay] automation: exclusive fullscreen refused (%s): the game stays windowed", where);
}

HRESULT STDMETHODCALLTYPE HookSetFullscreenState(IDXGISwapChain* chain, BOOL fullscreen, IDXGIOutput* target) {
    if (fullscreen && input::Automation()) {
        LogFullscreenRefused("SetFullscreenState");
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;  // documented answer for "not now"; games stay windowed
    }
    return g_orig_set_fullscreen(chain, fullscreen, target);
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* f, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    DXGI_SWAP_CHAIN_DESC windowed{};
    if (desc && !desc->Windowed && input::Automation()) {
        windowed = *desc;
        windowed.Windowed = TRUE;
        desc = &windowed;
        LogFullscreenRefused("CreateSwapChain");
    }
    ReleaseRendererForNewChain();
    const HRESULT hr = g_orig_create_chain(f, device, desc, out);
    if (SUCCEEDED(hr) && out) {
        const DWORD err = GetLastError();
        RecordCreatedChain(device, *out);
        SetLastError(err);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output,
                                                     IDXGISwapChain1** out) {
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC windowed{};
    if (fs && !fs->Windowed && input::Automation()) {
        windowed = *fs;
        windowed.Windowed = TRUE;
        fs = &windowed;
        LogFullscreenRefused("CreateSwapChainForHwnd");
    }
    ReleaseRendererForNewChain();
    const HRESULT hr = g_orig_create_chain_hwnd(f, device, hwnd, desc, fs, output, out);
    if (SUCCEEDED(hr) && out) {
        const DWORD err = GetLastError();
        RecordCreatedChain(device, *out);
        SetLastError(err);
    }
    return hr;
}

// ---------------------------------------------------------------- install
struct VTables {
    void* set_fullscreen = nullptr;
    void* present = nullptr;
    void* present1 = nullptr;
    void* resize = nullptr;
    void* resize1 = nullptr;
    void* ecl = nullptr;
    void* create_chain = nullptr;
    void* create_chain_hwnd = nullptr;
};

bool ReadVTables(HMODULE d3d12, HMODULE dxgi, VTables& vt, char* why, std::size_t cap) noexcept {
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto create_factory = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    const auto create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (!create_factory || !create_device) {
        std::snprintf(why, cap, "CreateDXGIFactory1 / D3D12CreateDevice exports missing");
        return false;
    }
    const wchar_t* cls_name = L"StrandedTogetherOverlayProbe";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls_name;
    const bool registered = RegisterClassExW(&wc) != 0;
    const HWND hwnd = CreateWindowExW(0, cls_name, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter* warp = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain1* chain = nullptr;
    IDXGISwapChain3* chain3 = nullptr;
    HRESULT hr = hwnd ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    const char* step = "CreateWindowEx";
    if (SUCCEEDED(hr)) step = "CreateDXGIFactory1", hr = create_factory(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        // WARP keeps the probe off the game's GPU device; the runtime objects
        // (and their vtables) are the same classes either way.
        step = "D3D12CreateDevice";
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
            hr = create_device(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (!device) hr = create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    }
    if (SUCCEEDED(hr)) {
        step = "CreateCommandQueue";
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    }
    if (SUCCEEDED(hr)) {
        step = "CreateSwapChainForHwnd";
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = 64;
        sd.Height = 64;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &chain);
    }
    if (SUCCEEDED(hr)) {
        void** v = *reinterpret_cast<void***>(chain);
        vt.present = v[kVtPresent];
        vt.set_fullscreen = v[kVtSetFullscreenState];
        vt.resize = v[kVtResizeBuffers];
        vt.present1 = v[kVtPresent1];
        if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3))) && chain3)
            vt.resize1 = (*reinterpret_cast<void***>(chain3))[kVtResizeBuffers1];
        vt.ecl = (*reinterpret_cast<void***>(queue))[kVtExecuteCommandLists];
        void** fv = *reinterpret_cast<void***>(factory);  // IDXGIFactory4 extends IDXGIFactory2
        vt.create_chain = fv[kVtCreateSwapChain];
        vt.create_chain_hwnd = fv[kVtCreateSwapChainForHwnd];
    } else {
        std::snprintf(why, cap, "%s failed (hr=0x%08lX)", step, static_cast<unsigned long>(hr));
    }
    SafeRelease(chain3);
    SafeRelease(chain);
    SafeRelease(queue);
    SafeRelease(device);
    SafeRelease(warp);
    SafeRelease(factory);
    if (hwnd) DestroyWindow(hwnd);
    if (registered) UnregisterClassW(cls_name, wc.hInstance);
    return SUCCEEDED(hr);
}

bool Hook(void* target, void* detour, void** original, const char* name, char* why, std::size_t cap) noexcept {
    if (g_target_count >= sizeof(g_targets) / sizeof(g_targets[0])) {
        std::snprintf(why, cap, "too many hooks (%s)", name);
        return false;
    }
    const MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) {
        std::snprintf(why, cap, "MH_CreateHook(%s) failed (%s)", name, MH_StatusToString(s));
        return false;
    }
    g_targets[g_target_count++] = target;
    return true;
}

void RemoveHooks() noexcept {
    for (std::size_t i = 0; i < g_target_count; ++i) {
        MH_DisableHook(g_targets[i]);
        MH_RemoveHook(g_targets[i]);
    }
    g_target_count = 0;
    input::RemoveApiHooks();
    g_input_target_count = 0;
}

}  // namespace

bool Install(const Options& options) noexcept {
    if (g_installed.load()) return true;
    QueryPerformanceFrequency(&g_qpf);
    g_opt = options;
    font::Configure(options.game_font);
    input::SetAutomation(options.automation);
    input::SetToggleKey(options.toggle_vk);
    menu::Init(options.mod_dir);

    // The game delay-loads both; load them the same way (same search order).
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!d3d12 || !dxgi) {
        Log("[overlay] not installed: d3d12.dll/dxgi.dll not loadable (error %lu)", GetLastError());
        return false;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        Log("[overlay] not installed: MinHook init failed (%s)", MH_StatusToString(init));
        return false;
    }
    char why[200] = {};
    VTables vt;
    if (!ReadVTables(d3d12, dxgi, vt, why, sizeof(why))) {
        Log("[overlay] not installed: probe device: %s", why);
        return false;
    }
    bool ok = Hook(vt.present, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_orig_present), "Present", why,
                   sizeof(why)) &&
              Hook(vt.resize, reinterpret_cast<void*>(&HookResize), reinterpret_cast<void**>(&g_orig_resize), "ResizeBuffers", why,
                   sizeof(why)) &&
              Hook(vt.ecl, reinterpret_cast<void*>(&HookExecuteCommandLists), reinterpret_cast<void**>(&g_orig_ecl),
                   "ExecuteCommandLists", why, sizeof(why));
    // Present1 / ResizeBuffers1 are optional (only if the game uses them).
    if (ok && vt.present1 && vt.present1 != vt.present)
        Hook(vt.present1, reinterpret_cast<void*>(&HookPresent1), reinterpret_cast<void**>(&g_orig_present1), "Present1", why,
             sizeof(why));
    if (ok && vt.resize1 && vt.resize1 != vt.resize)
        Hook(vt.resize1, reinterpret_cast<void*>(&HookResize1), reinterpret_cast<void**>(&g_orig_resize1), "ResizeBuffers1", why,
             sizeof(why));
    // Swapchain creation (optional): tells which queue the game's swapchain presents
    // on. Without it the ExecuteCommandLists capture is the fallback.
    bool create_hooks = false;
    if (ok && vt.create_chain && vt.create_chain_hwnd) {
        create_hooks = Hook(vt.create_chain, reinterpret_cast<void*>(&HookCreateSwapChain),
                            reinterpret_cast<void**>(&g_orig_create_chain), "CreateSwapChain", why, sizeof(why));
        if (create_hooks && vt.create_chain_hwnd != vt.create_chain)
            create_hooks = Hook(vt.create_chain_hwnd, reinterpret_cast<void*>(&HookCreateSwapChainForHwnd),
                                reinterpret_cast<void**>(&g_orig_create_chain_hwnd), "CreateSwapChainForHwnd", why, sizeof(why));
        if (!create_hooks) Log("[overlay] swapchain creation hooks unavailable (%s): the queue is found via ExecuteCommandLists; no name tags", why);
        why[0] = 0;
    }
    if (ok && options.automation && vt.set_fullscreen &&
        !Hook(vt.set_fullscreen, reinterpret_cast<void*>(&HookSetFullscreenState), reinterpret_cast<void**>(&g_orig_set_fullscreen),
              "SetFullscreenState", why, sizeof(why)))
        ok = false;  // a hidden run must be able to refuse exclusive fullscreen
    if (!ok) {
        RemoveHooks();
        Log("[overlay] not installed: %s", why);
        return false;
    }
    const bool input_hooks = input::CreateApiHooks(g_input_targets, &g_input_target_count, 8);
    if (!input_hooks)
        Log("[overlay] cursor hooks unavailable: the game may move/clip the cursor while the menu is open");

    for (std::size_t i = 0; i < g_target_count; ++i) MH_QueueEnableHook(g_targets[i]);
    for (std::size_t i = 0; i < g_input_target_count; ++i) MH_QueueEnableHook(g_input_targets[i]);
    const MH_STATUS applied = MH_ApplyQueued();
    if (applied != MH_OK) {
        RemoveHooks();
        Log("[overlay] not installed: enabling hooks failed (%s)", MH_StatusToString(applied));
        return false;
    }
    g_create_hooks.store(create_hooks);
    g_installed.store(true);
    UpdateStats([](Stats& s, const void*) { s.installed = true; }, nullptr);
    char key[24];
    const input::Stats ist = input::GetStats();
    Log("[overlay] installed (Direct3D 12): toggle %s; hooked Present%s, ResizeBuffers%s, ExecuteCommandLists%s%s%s%s",
        VkName(input::ToggleKey(), key, sizeof(key)), g_orig_present1 ? "+Present1" : "",
        g_orig_resize1 ? "+ResizeBuffers1" : "", create_hooks ? ", CreateSwapChain(ForHwnd)" : "",
        input_hooks ? ", SetCursorPos, ClipCursor" : "", ist.xinput14_hook ? ", XInputGetState(xinput1_4)" : "",
        options.automation ? "; AUTOMATION: virtual pad only, GetForegroundWindow, cursor isolated, no exclusive fullscreen" : "");
    return true;
}

void Shutdown() noexcept {
    if (!g_installed.load()) return;
    // Hooks first (MinHook freezes threads while unpatching), then the objects.
    RemoveHooks();
    AcquireSRWLockExclusive(&g_render);
    ApplyOpen(false, "by the overlay's shutdown");
    ReleaseRenderer();
    if (g_ctx) {
        ImGui::DestroyContext(g_ctx);
        g_ctx = nullptr;
    }
    input::Detach();
    g_attached_chain = nullptr;
    ReleaseSRWLockExclusive(&g_render);
    AcquireSRWLockExclusive(&g_cap_lock);
    SafeRelease(g_cap_queue);
    SafeRelease(g_cap_queue_pt);
    g_cap_device.store(nullptr);
    ReleaseSRWLockExclusive(&g_cap_lock);
    g_cap_wanted.store(false);
    AcquireSRWLockExclusive(&g_created_lock);
    g_created_chain = nullptr;
    g_created_queue = nullptr;
    ReleaseSRWLockExclusive(&g_created_lock);
    AcquireSRWLockExclusive(&g_capture_lock);
    for (CaptureSlot& c : g_captures)
        if (c.id && c.state == 0) FailCapture(c, "overlay shut down");
    ReleaseSRWLockExclusive(&g_capture_lock);
    // The g_orig_* trampolines stay: a thread may still be inside a detour.
    g_logged_queue_wait = false;
    g_queue_wait_frames = 0;
    g_installed.store(false);
    g_disabled.store(false);
    g_applied_scale = -1;
    g_logged_first_draw = false;
    g_hint_done = false;
    g_hint_until = 0;
    g_hint_valid_since = 0;
    g_hint_entity = 0;
    AcquireSRWLockExclusive(&g_stats_lock);
    g_stats = Stats{};
    ReleaseSRWLockExclusive(&g_stats_lock);
    Log("[overlay] shut down");
}

bool Installed() noexcept { return g_installed.load(); }

void SetCoopView(const CoopView& v) noexcept {
    AcquireSRWLockExclusive(&g_coop_lock);
    g_coop = v;
    ReleaseSRWLockExclusive(&g_coop_lock);
    g_coop_shown.store(v.waiting || v.banner[0] != 0, std::memory_order_release);
    // The waiting screen keeps the game's input (as the open menu does); only with the menu's input hooks.
    if (g_opt.menu || !v.block_input) input::SetWaitBlock(v.block_input);
}

unsigned TakeCoopRequests() noexcept { return g_coop_requests.exchange(0); }
bool Disabled() noexcept { return g_disabled.load(); }
bool IsOpen() noexcept { return input::IsOpen(); }
void SetOpen(bool open) noexcept { g_open_request.store(open ? 1 : 0); }  // ignored without the menu (Options::menu)

Stats GetStats() noexcept {
    AcquireSRWLockShared(&g_stats_lock);
    Stats s = g_stats;
    ReleaseSRWLockShared(&g_stats_lock);
    return s;
}

std::uint32_t RequestCapture() noexcept {
    std::uint32_t id = g_capture_next.fetch_add(1);
    if (id == 0) id = g_capture_next.fetch_add(1);
    AcquireSRWLockExclusive(&g_capture_lock);
    CaptureSlot* slot = nullptr;
    for (CaptureSlot& c : g_captures)
        if (!c.id) {
            slot = &c;
            break;
        }
    if (!slot)  // all busy: reuse the oldest finished one (its owner gave up)
        for (CaptureSlot& c : g_captures)
            if (c.state != 0 && (!slot || c.id < slot->id)) slot = &c;
    if (slot) {
        slot->id = id;
        slot->state = 0;
        slot->waited = 0;
        slot->image = Image{};
        slot->why[0] = 0;
        g_capture_pending.fetch_add(1);
        if (!g_installed.load() || g_disabled.load()) FailCapture(*slot, g_disabled.load() ? "the overlay is disabled" : "the overlay is not installed");
    } else {
        id = 0;
    }
    ReleaseSRWLockExclusive(&g_capture_lock);
    return id;  // 0 only when four captures are pending at once
}

int TakeCapture(std::uint32_t id, Image* out, char* why, std::size_t why_cap) noexcept {
    int result = -1;
    AcquireSRWLockExclusive(&g_capture_lock);
    CaptureSlot* slot = nullptr;
    for (CaptureSlot& c : g_captures)
        if (id && c.id == id) slot = &c;
    // A disabled overlay never presents through us again: nothing pending will be served.
    if (slot && slot->state == 0 && g_disabled.load()) FailCapture(*slot, "the overlay is disabled");
    if (!slot) {
        if (why && why_cap) std::snprintf(why, why_cap, "unknown capture id %u", id);
    } else if (slot->state == 0) {
        result = 0;
    } else if (slot->state == 1) {
        if (out) *out = std::move(slot->image);
        slot->image = Image{};
        slot->id = 0;
        result = 1;
    } else {
        if (why && why_cap) std::snprintf(why, why_cap, "%s", slot->why);
        slot->id = 0;
    }
    ReleaseSRWLockExclusive(&g_capture_lock);
    return result;
}

void CancelCapture(std::uint32_t id) noexcept {
    if (!id) return;
    AcquireSRWLockExclusive(&g_capture_lock);
    for (CaptureSlot& c : g_captures) {
        if (c.id != id) continue;
        if (c.state == 0) g_capture_pending.fetch_sub(1);
        c.id = 0;
        c.state = -1;
        c.image = Image{};
        c.why[0] = 0;
    }
    ReleaseSRWLockExclusive(&g_capture_lock);
}

void SetAfterDrawForTest(AfterDrawFn fn, void* user) noexcept {
    AcquireSRWLockExclusive(&g_render);
    g_renderer.SetAfterDraw(fn, user);
    ReleaseSRWLockExclusive(&g_render);
}

}  // namespace st::overlay
