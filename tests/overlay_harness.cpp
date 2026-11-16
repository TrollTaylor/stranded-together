// st_overlay_harness: drives the in-game debug menu overlay without the game.
//
// It plays the game's part: a window with its own window procedure and its
// own WH_KEYBOARD thread hook (like ds.exe), a D3D12 device + DIRECT queue +
// flip-model swapchain created AFTER overlay::Install (like the mod, which
// installs at process start), and a frame loop that clears the back buffer,
// submits through ID3D12CommandQueue::ExecuteCommandLists and calls Present.
// A separate "tick" thread runs debug::OnTick like st::sync::Tick does.
//
// Checks (each prints PASS/FAIL; exit code 1 on any failure):
//   closed menu: nothing drawn, renderer never created, input reaches the game
//   open menu: pixels changed in the back buffer, ImGui draw data non-empty,
//              mouse/keyboard reach ImGui and NOT the game window/hook,
//              SetCursorPos/ClipCursor suppressed
//   actions: queued on the render thread, executed on the tick thread
//   ResizeBuffers: new size, then a 10-bit (R10G10B10A2) back buffer
//   shutdown: the window procedure is restored
//   automation (hidden test runs): back-buffer capture with the menu closed
//              (10-bit buffer), PNG encoder round trip through WIC, the virtual
//              XInput pad on xinput1_4 (real pads isolated, idle while the menu
//              is open), GetForegroundWindow answers the game window, cursor
//              calls never reach the system, focus loss keeps the menu open
// Screenshots (PNG) of every tab go to --out (default: current directory).
//
// Usage: st_overlay_harness [--out DIR] [--warp] [--show]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <xinput.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "debug_actions.hpp"
#include "imgui.h"
#include "log.hpp"
#include "overlay.hpp"
#include "overlay_input.hpp"
#include "overlay_menu.hpp"
#include "overlay_renderer.hpp"

#ifndef ST_BUILD_ID
#define ST_BUILD_ID "dev"
#endif

namespace st::sync::stub {
extern std::atomic<unsigned long> last_thread;
}

namespace {

int g_fail = 0;
int g_pass = 0;
void Check(bool ok, const char* what, const char* detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail[0] ? "  -- " : "", detail);
    std::fflush(stdout);
    (ok ? g_pass : g_fail)++;
}

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// ---------------------------------------------------------------- "game" window
std::atomic<int> g_game_keys{0}, g_game_mouse{0}, g_game_input{0}, g_game_chars{0}, g_game_hook_keys{0};
HHOOK g_game_hook = nullptr;

LRESULT CALLBACK GameWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m >= WM_KEYFIRST && m <= WM_KEYLAST) {
        if (m == WM_CHAR) ++g_game_chars;
        else ++g_game_keys;
        return 0;
    }
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) {
        ++g_game_mouse;
        return 0;
    }
    if (m == WM_INPUT) {
        ++g_game_input;
        return DefWindowProcW(h, m, w, l);
    }
    if (m == WM_DESTROY) return 0;
    return DefWindowProcW(h, m, w, l);
}

// Like ds.exe's own WH_KEYBOARD hook (0x1419B9920).
LRESULT CALLBACK GameKeyboardHook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) ++g_game_hook_keys;
    return CallNextHookEx(nullptr, code, w, l);
}

void Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ---------------------------------------------------------------- "game" D3D12
struct Gfx {
    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandQueue* compute = nullptr;  // a non-DIRECT queue the capture must ignore
    IDXGISwapChain3* chain = nullptr;
    ID3D12DescriptorHeap* rtv = nullptr;
    UINT rtv_step = 0;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fence_value = 0;
    HANDLE event = nullptr;
    // capture (overlay after-draw callback)
    ID3D12CommandAllocator* cap_alloc = nullptr;
    ID3D12GraphicsCommandList* cap_list = nullptr;
    ID3D12Resource* readback = nullptr;
    UINT64 readback_size = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    bool capture_next = false;
    bool captured = false;
    UINT cap_w = 0, cap_h = 0;
    DXGI_FORMAT cap_format = DXGI_FORMAT_UNKNOWN;
    std::atomic<int> after_draw_calls{0};
} g;

const float kClear[4] = {0.10f, 0.30f, 0.40f, 1.0f};

void WaitGpu() {
    ++g.fence_value;
    g.queue->Signal(g.fence, g.fence_value);
    if (g.fence->GetCompletedValue() < g.fence_value) {
        g.fence->SetEventOnCompletion(g.fence_value, g.event);
        WaitForSingleObject(g.event, 5000);
    }
}

bool CreateGfx(HWND hwnd, bool warp, UINT w, UINT h) {
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&g.factory)))) return false;
    HRESULT hr = E_FAIL;
    if (!warp) hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device));
    if (FAILED(hr)) {
        IDXGIAdapter* a = nullptr;
        g.factory->EnumWarpAdapter(IID_PPV_ARGS(&a));
        hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device));
        SafeRelease(a);
        std::printf("info  using the WARP adapter\n");
    }
    if (FAILED(hr)) return false;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)))) return false;
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (FAILED(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.compute)))) return false;
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w;
    sd.Height = h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* c1 = nullptr;
    if (FAILED(g.factory->CreateSwapChainForHwnd(g.queue, hwnd, &sd, nullptr, nullptr, &c1))) return false;
    c1->QueryInterface(IID_PPV_ARGS(&g.chain));
    SafeRelease(c1);
    g.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtv));
    g.rtv_step = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc));
    g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr, IID_PPV_ARGS(&g.list));
    g.list->Close();
    g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.cap_alloc));
    g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.cap_alloc, nullptr, IID_PPV_ARGS(&g.cap_list));
    g.cap_list->Close();
    g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g.chain && g.rtv && g.list && g.cap_list && g.fence;
}

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    l->ResourceBarrier(1, &br);
}

// Overlay after-draw callback: copy the back buffer (menu included) to a readback buffer.
void AfterDraw(IDXGISwapChain3* chain, ID3D12CommandQueue* queue, void*) {
    ++g.after_draw_calls;
    if (!g.capture_next) return;
    g.capture_next = false;
    ID3D12Resource* bb = nullptr;
    if (FAILED(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb)))) return;
    const D3D12_RESOURCE_DESC d = bb->GetDesc();
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&d, 0, 1, 0, &g.footprint, nullptr, nullptr, &total);
    if (!g.readback || g.readback_size < total) {
        SafeRelease(g.readback);
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = total;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&g.readback));
        g.readback_size = total;
    }
    g.cap_alloc->Reset();
    g.cap_list->Reset(g.cap_alloc, nullptr);
    Barrier(g.cap_list, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = g.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = g.footprint;
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    g.cap_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(g.cap_list, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    g.cap_list->Close();
    ID3D12CommandList* lists[] = {g.cap_list};
    queue->ExecuteCommandLists(1, lists);
    g.cap_w = static_cast<UINT>(d.Width);
    g.cap_h = d.Height;
    g.cap_format = d.Format;
    g.captured = true;
    bb->Release();
}

std::atomic<bool> g_tick_run{true};
std::atomic<unsigned long> g_tick_thread{0};

// One "game" frame: clear, submit, Present (the overlay draws inside Present).
HRESULT GameFrame() {
    ID3D12Resource* bb = nullptr;
    if (FAILED(g.chain->GetBuffer(g.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb)))) return E_FAIL;
    g.alloc->Reset();
    g.list->Reset(g.alloc, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtv->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(bb, nullptr, rtv);
    Barrier(g.list, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g.list->ClearRenderTargetView(rtv, kClear, 0, nullptr);
    Barrier(g.list, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g.list->Close();
    ID3D12CommandList* lists[] = {g.list};
    g.compute->ExecuteCommandLists(0, nullptr);  // noise for the queue capture (ignored: not DIRECT)
    g.queue->ExecuteCommandLists(1, lists);
    bb->Release();
    const HRESULT hr = g.chain->Present(0, 0);
    WaitGpu();
    Pump();
    return hr;
}

void Frames(int n) {
    for (int i = 0; i < n; ++i) GameFrame();
}

// Convert the captured buffer to BGRA8 rows. Returns false when nothing was captured.
bool CapturedPixels(std::vector<unsigned char>& bgra, UINT& w, UINT& h) {
    if (!g.captured || !g.readback) return false;
    g.captured = false;
    void* p = nullptr;
    D3D12_RANGE r{0, static_cast<SIZE_T>(g.readback_size)};
    if (FAILED(g.readback->Map(0, &r, &p))) return false;
    w = g.cap_w;
    h = g.cap_h;
    bgra.resize(static_cast<std::size_t>(w) * h * 4);
    const auto* base = static_cast<const unsigned char*>(p) + g.footprint.Offset;
    for (UINT y = 0; y < h; ++y) {
        const unsigned char* row = base + static_cast<std::size_t>(y) * g.footprint.Footprint.RowPitch;
        for (UINT x = 0; x < w; ++x) {
            unsigned char* o = &bgra[(static_cast<std::size_t>(y) * w + x) * 4];
            if (g.cap_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                std::uint32_t v;
                std::memcpy(&v, row + x * 4, 4);
                o[2] = static_cast<unsigned char>(((v >> 0) & 0x3FF) >> 2);
                o[1] = static_cast<unsigned char>(((v >> 10) & 0x3FF) >> 2);
                o[0] = static_cast<unsigned char>(((v >> 20) & 0x3FF) >> 2);
                o[3] = 255;
            } else {  // R8G8B8A8
                o[0] = row[x * 4 + 2];
                o[1] = row[x * 4 + 1];
                o[2] = row[x * 4 + 0];
                o[3] = 255;
            }
        }
    }
    D3D12_RANGE none{0, 0};
    g.readback->Unmap(0, &none);
    return true;
}

double ChangedFraction(const std::vector<unsigned char>& bgra) {
    const int cr = static_cast<int>(kClear[0] * 255 + 0.5f), cg = static_cast<int>(kClear[1] * 255 + 0.5f),
              cb = static_cast<int>(kClear[2] * 255 + 0.5f);
    std::size_t changed = 0, n = bgra.size() / 4;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char* p = &bgra[i * 4];
        if (std::abs(p[2] - cr) > 6 || std::abs(p[1] - cg) > 6 || std::abs(p[0] - cb) > 6) ++changed;
    }
    return n ? static_cast<double>(changed) / static_cast<double>(n) : 0;
}

bool WritePng(const std::wstring& path, const std::vector<unsigned char>& bgra, UINT w, UINT h) {
    IWICImagingFactory* f = nullptr;
    IWICStream* s = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
              SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    ok = ok && SUCCEEDED(frame->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
         SUCCEEDED(frame->WritePixels(h, w * 4, static_cast<UINT>(bgra.size()), const_cast<BYTE*>(bgra.data()))) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    SafeRelease(frame);
    SafeRelease(enc);
    SafeRelease(s);
    SafeRelease(f);
    return ok;
}

std::wstring g_out = L".";

// PNG bytes -> BGRA8 through WIC (an independent decoder for our encoder).
bool DecodePng(const std::string& png, std::vector<unsigned char>& bgra, UINT& w, UINT& h) {
    IWICImagingFactory* f = nullptr;
    IWICStream* s = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* conv = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(f->CreateStream(&s)) &&
              SUCCEEDED(s->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(png.data())), static_cast<DWORD>(png.size()))) &&
              SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame, &conv)) &&
              SUCCEEDED(conv->GetSize(&w, &h));
    if (ok) {
        bgra.resize(static_cast<std::size_t>(w) * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(bgra.size()), bgra.data()));
    }
    SafeRelease(conv);
    SafeRelease(frame);
    SafeRelease(dec);
    SafeRelease(s);
    SafeRelease(f);
    return ok;
}

// Opens/keeps the menu, renders a few frames, captures, checks and writes a PNG.
bool Shot(const wchar_t* name, const char* what, double min_changed) {
    Frames(2);
    g.capture_next = true;
    GameFrame();
    std::vector<unsigned char> px;
    UINT w = 0, h = 0;
    const bool got = CapturedPixels(px, w, h);
    char detail[200];
    const double frac = got ? ChangedFraction(px) : 0;
    std::snprintf(detail, sizeof(detail), "%ux%u, %.1f%% of pixels drawn by the overlay", w, h, frac * 100);
    Check(got && frac >= min_changed, what, detail);
    if (got) {
        const std::wstring path = g_out + L"\\" + name;
        const bool saved = WritePng(path, px, w, h);
        std::printf("info  screenshot %ls %s\n", path.c_str(), saved ? "written" : "NOT written");
    }
    return got;
}

LPARAM KeyLParam(UINT vk, bool down, bool alt = false) {
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | (static_cast<LPARAM>(scan) << 16);
    if (alt) lp |= 1LL << 29;
    if (!down) lp |= (1LL << 30) | (1LL << 31);
    return lp;
}

// Our WH_KEYBOARD procedure, called as the system would for a real key press
// (posted messages never reach WH_KEYBOARD hooks).
LRESULT HookKey(UINT vk, bool down, bool alt = false) {
    return st::overlay::input::KeyboardProcForTest(HC_ACTION, vk, KeyLParam(vk, down, alt));
}

void PostKey(HWND hwnd, UINT vk, bool down) {
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | (static_cast<LPARAM>(scan) << 16);
    if (!down) lp |= (1LL << 30) | (1LL << 31);
    PostMessageW(hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
}

}  // namespace

int main(int argc, char** argv) {
    bool warp = false, show = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--warp")) warp = true;
        else if (!std::strcmp(argv[i], "--show")) show = true;
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
            const std::string o = argv[++i];
            g_out.assign(o.begin(), o.end());
        }
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring logdir = g_out + L"\\overlay_harness";
    CreateDirectoryW(logdir.c_str(), nullptr);
    st::LogInit(logdir);  // <out>\overlay_harness\stranded_together.log
    st::Log("[harness] st_overlay_harness %s", ST_BUILD_ID);
    st::debug::SetRoleLabel("echo");
    st::debug::SetModState("harness (no game)");

    // ---- the "game" window + its own keyboard hook, like ds.exe
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = GameWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"StOverlayHarnessGame";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Stranded Together overlay harness", WS_OVERLAPPEDWINDOW, 40, 40,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::printf("FAIL  CreateWindowEx\n");
        return 1;
    }
    if (show) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    g_game_hook = SetWindowsHookExW(WH_KEYBOARD, &GameKeyboardHook, nullptr, GetCurrentThreadId());
    Check(g_game_hook != nullptr, "game WH_KEYBOARD hook installed (like ds.exe)");

    // ---- the mod: install the overlay before the game creates its device
    st::overlay::Options opt;
    opt.toggle_vk = VK_F9;
    opt.mod_dir = logdir;
    opt.build = ST_BUILD_ID;
    opt.startup_hint = false;
    Check(st::overlay::Install(opt), "overlay::Install (probe device, MinHook)");
    st::overlay::SetAfterDrawForTest(&AfterDraw, nullptr);

    // ---- the "game" device
    if (!CreateGfx(hwnd, warp, 1280, 720)) {
        std::printf("FAIL  creating the D3D12 device/swapchain\n");
        return 1;
    }

    // ---- the mod's tick thread
    std::thread tick([] {
        g_tick_thread = GetCurrentThreadId();
        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        while (g_tick_run.load()) {
            QueryPerformanceCounter(&t);
            st::debug::OnTick(static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart));
            Sleep(10);
        }
    });
    while (!g_tick_thread.load()) Sleep(1);

    // ---- 1. closed menu: zero effect
    Frames(10);
    st::overlay::Stats s = st::overlay::GetStats();
    char d[200];
    std::snprintf(d, sizeof(d), "presents=%lu drawn=%lu renderer_inits=%lu", s.presents, s.frames_drawn, s.renderer_inits);
    Check(s.presents >= 10 && s.frames_drawn == 0 && s.renderer_inits == 0 && g.after_draw_calls == 0,
          "closed menu: Present hooked, nothing drawn, no renderer created", d);
    st::overlay::input::Stats in = st::overlay::input::GetStats();
    Check(in.subclassed && in.keyboard_hook, "input attached: window subclassed + keyboard hook on the window thread");

    int k0 = g_game_keys, h0 = g_game_hook_keys, m0 = g_game_mouse;
    PostKey(hwnd, 'A', true);
    PostKey(hwnd, 'A', false);
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(100, 100));
    Pump();
    std::snprintf(d, sizeof(d), "window keys +%d, game hook keys +%d, mouse +%d", g_game_keys - k0, g_game_hook_keys - h0,
                  g_game_mouse - m0);
    const bool posted_keys_hit_hooks = g_game_hook_keys - h0 == 2;
    Check(g_game_keys - k0 == 2 && g_game_mouse - m0 == 1, "closed menu: keys and mouse reach the game", d);
    std::printf("info  posted WM_KEYDOWN %s WH_KEYBOARD hooks\n", posted_keys_hit_hooks ? "reaches" : "does NOT reach");

    Check(HookKey('A', true) != 1 && HookKey('A', false) != 1, "closed menu: our keyboard hook passes keys on to the game's hook");

    // ---- 2. open with the toggle key (through our keyboard hook)
    const LRESULT f9_down = HookKey(VK_F9, true);
    const LRESULT f9_up = HookKey(VK_F9, false);
    Frames(1);
    Check(f9_down == 1 && f9_up == 1, "the toggle key is swallowed (never reaches the game)");
    Check(st::overlay::IsOpen(), "F9 through the keyboard hook opens the menu");
    if (!st::overlay::IsOpen()) {
        st::overlay::SetOpen(true);
        Frames(1);
    }
    // A posted F9 (no hook involved) reaches the subclassed window: also a toggle, also swallowed.
    k0 = g_game_keys;
    PostKey(hwnd, VK_F9, true);
    PostKey(hwnd, VK_F9, false);
    Pump();
    Frames(1);
    Check(g_game_keys == k0, "posted F9 never reaches the game window");
    Check(!st::overlay::IsOpen(), "posted F9 (window message path) closes the menu");
    HookKey(VK_F9, true);
    HookKey(VK_F9, false);
    Frames(1);
    Check(st::overlay::IsOpen(), "menu open");
    Check(st::debug::MenuOpen(), "tick sees the menu open (publishes every frame, F6/F7 stand down)");

    // ---- 3. drawing
    Shot(L"overlay_menu_status.png", "open menu: Status tab drawn into the back buffer", 0.05);
    s = st::overlay::GetStats();
    std::snprintf(d, sizeof(d), "vertices=%d lists=%d queue=%s inits=%lu", s.last_vertices, s.last_draw_lists, s.queue_source,
                  s.renderer_inits);
    Check(s.frames_drawn > 0 && s.last_vertices > 100 && s.renderer_inits >= 1 &&
              s.renderer_inits - s.renderer_releases == 1,
          "ImGui draw data non-empty, one live renderer", d);
    Check(std::strstr(s.queue_source, "creation") != nullptr,
          "game queue taken from the swapchain's creation (CreateSwapChainForHwnd hook)", s.queue_source);
    const char* tabs[] = {"Partner", "Player", "Log", "Settings"};
    const wchar_t* files[] = {L"overlay_menu_partner.png", L"overlay_menu_player.png", L"overlay_menu_log.png",
                              L"overlay_menu_settings.png"};
    for (int i = 0; i < 4; ++i) {
        st::overlay::menu::SelectTab(i + 1);
        char what[64];
        std::snprintf(what, sizeof(what), "open menu: %s tab drawn", tabs[i]);
        Shot(files[i], what, 0.05);
    }
    st::overlay::menu::SelectTab(0);
    Frames(2);

    // ---- 4. input while open: ImGui gets it, the game does not
    k0 = g_game_keys, h0 = g_game_hook_keys, m0 = g_game_mouse;
    const int i0 = g_game_input;
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(300, 200));
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(300, 200));
    PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(300, 200));
    PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(300, 200));
    PostMessageW(hwnd, WM_INPUT, RIM_INPUT, 0);
    PostKey(hwnd, 'B', true);
    PostMessageW(hwnd, WM_CHAR, 'b', 0);
    Pump();
    Frames(1);
    std::snprintf(d, sizeof(d), "window keys +%d, game hook keys +%d, mouse +%d, WM_INPUT +%d", g_game_keys - k0,
                  g_game_hook_keys - h0, g_game_mouse - m0, g_game_input - i0);
    Check(g_game_keys == k0 && g_game_hook_keys == h0 && g_game_mouse == m0 && g_game_input == i0,
          "open menu: no key, mouse or raw input reaches the game", d);
    {
        const ImGuiIO& io = ImGui::GetIO();
        std::snprintf(d, sizeof(d), "MousePos=(%.0f, %.0f)", io.MousePos.x, io.MousePos.y);
        Check(std::fabs(io.MousePos.x - 300) < 1.5f && std::fabs(io.MousePos.y - 200) < 1.5f, "ImGui receives the mouse position",
              d);
    }
    const unsigned long chars0 = st::overlay::input::GetStats().characters;
    const LRESULT b_down = HookKey('B', true);
    Frames(3);  // ImGui trickles input: queued mouse-up from above is applied first
    {
        const ImGuiKeyData* kd = &ImGui::GetIO().KeysData[ImGuiKey_B - ImGuiKey_NamedKey_BEGIN];
        std::snprintf(d, sizeof(d), "hook returned %lld, B down=%d dur=%.3f", static_cast<long long>(b_down), kd ? kd->Down : -1,
                      kd ? kd->DownDuration : -1.0f);
    }
    Check(b_down == 1 && ImGui::IsKeyDown(ImGuiKey_B), "open menu: keys are swallowed by our hook and reach ImGui", d);
    Check(st::overlay::input::GetStats().characters == chars0 + 1, "open menu: typed text reaches ImGui ('b')");
    HookKey('B', false);
    Check(HookKey(VK_F4, true, true) != 1, "Alt+F4 still reaches the game while the menu is open");
    HookKey(VK_F4, false, true);
    PostKey(hwnd, 'B', false);
    Pump();
    in = st::overlay::input::GetStats();
    std::snprintf(d, sizeof(d), "keys=%lu messages=%lu raw=%lu", in.blocked_keys, in.blocked_messages, in.blocked_raw_input);
    Check(in.blocked_messages >= 4 && in.blocked_raw_input >= 1 && in.blocked_keys >= 2, "blocked-input counters", d);

    // A key held since before the menu opened: its release still reaches the game
    // (else it stays down in the game's input state), its new presses do not.
    st::overlay::input::MarkHeldForTest('W');
    {
        const LRESULT w_up = HookKey('W', false);
        const LRESULT w_down = HookKey('W', true);
        HookKey('W', false);
        k0 = g_game_keys;
        PostKey(hwnd, 'W', false);
        PostKey(hwnd, 'W', true);
        Pump();
        std::snprintf(d, sizeof(d), "hook up=%lld down=%lld, window keys +%d", static_cast<long long>(w_up),
                      static_cast<long long>(w_down), g_game_keys - k0);
        Check(w_up != 1 && w_down == 1 && g_game_keys == k0 + 1,
              "open menu: the release of a key held before opening reaches the game, presses do not", d);
    }
    POINT cur{};
    GetCursorPos(&cur);
    SetCursorPos(cur.x, cur.y);  // same position: harmless if the hook were missing
    RECT clip{};
    GetClipCursor(&clip);
    ClipCursor(&clip);
    in = st::overlay::input::GetStats();
    std::snprintf(d, sizeof(d), "SetCursorPos suppressed=%lu ClipCursor suppressed=%lu", in.suppressed_cursor_pos, in.suppressed_clip);
    Check(in.suppressed_cursor_pos >= 1 && in.suppressed_clip >= 1, "open menu: game cursor moves/clips are ignored", d);

    // ---- 5. actions: queued here (render side), executed on the tick thread
    st::debug::Request tp;
    tp.action = st::debug::Action::teleport;
    tp.v[0] = 1, tp.v[1] = 2, tp.v[2] = 3;
    const std::uint32_t serial = st::debug::Queue(tp);
    st::debug::Result res;
    for (int i = 0; i < 200 && !st::debug::ResultFor(serial, &res); ++i) Sleep(5);
    Check(st::debug::ResultFor(serial, &res) && !res.ok && std::strstr(res.text, "not implemented"),
          "teleport action ran (stub: not implemented yet)", res.text);
    const std::uint32_t s2 = st::debug::Queue(st::debug::Action::respawn_partner);
    for (int i = 0; i < 200 && !st::debug::ResultFor(s2, &res); ++i) Sleep(5);
    Check(st::sync::stub::last_thread.load() == g_tick_thread.load(), "actions execute on the tick thread, not the render thread");
    const st::debug::Snapshot snap = st::debug::GetSnapshot();
    Check(snap.serial > 0 && snap.local_valid && std::strcmp(snap.role, "echo") == 0, "menu reads the tick's published snapshot");

    // ---- 6. ResizeBuffers (hooked): bigger, then a 10-bit back buffer
    HRESULT hr = g.chain->ResizeBuffers(0, 1600, 900, DXGI_FORMAT_UNKNOWN, 0);
    Check(SUCCEEDED(hr), "ResizeBuffers 1600x900 (no back-buffer refs held by the overlay)");
    Shot(L"overlay_menu_resized.png", "menu drawn after ResizeBuffers", 0.03);
    s = st::overlay::GetStats();
    std::snprintf(d, sizeof(d), "%ux%u resizes=%lu", s.width, s.height, s.resizes);
    Check(s.width == 1600 && s.height == 900 && s.resizes >= 1, "overlay follows the new size", d);
    hr = g.chain->ResizeBuffers(0, 1280, 720, DXGI_FORMAT_R10G10B10A2_UNORM, 0);
    Check(SUCCEEDED(hr), "ResizeBuffers to R10G10B10A2_UNORM (10-bit / HDR10)");
    Shot(L"overlay_menu_10bit.png", "menu drawn into a 10-bit back buffer (pipeline rebuilt)", 0.03);
    s = st::overlay::GetStats();
    Check(s.format == DXGI_FORMAT_R10G10B10A2_UNORM && !s.disabled, "overlay uses the 10-bit RTV format");
    Check(st::overlay::RtvFormatFor(DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_FLOAT &&
              st::overlay::RtvFormatFor(DXGI_FORMAT_B8G8R8A8_TYPELESS) == DXGI_FORMAT_B8G8R8A8_UNORM &&
              st::overlay::RtvFormatFor(DXGI_FORMAT_D32_FLOAT) == DXGI_FORMAT_UNKNOWN,
          "RTV format mapping (scRGB, typeless, unsupported)");

    // ---- 7. close: zero effect again
    HookKey(VK_F9, true);
    HookKey(VK_F9, false);
    Frames(2);
    Check(!st::overlay::IsOpen() && !st::debug::MenuOpen(), "menu closed");
    const unsigned long drawn = st::overlay::GetStats().frames_drawn;
    Frames(5);
    Check(st::overlay::GetStats().frames_drawn == drawn, "closed again: nothing drawn");
    for (int i = 0; i < 130 && st::overlay::GetStats().renderer_inits != st::overlay::GetStats().renderer_releases; ++i) Frames(1);
    s = st::overlay::GetStats();
    std::snprintf(d, sizeof(d), "inits=%lu releases=%lu", s.renderer_inits, s.renderer_releases);
    Check(s.renderer_inits == s.renderer_releases && s.renderer_inits >= 1,
          "closed menu: renderer released (no swapchain/queue reference kept)", d);
    k0 = g_game_keys, m0 = g_game_mouse, h0 = g_game_hook_keys;
    PostKey(hwnd, 'C', true);
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
    Pump();
    Check(g_game_keys == k0 + 1 && g_game_mouse == m0 + 1 && (!posted_keys_hit_hooks || g_game_hook_keys == h0 + 1),
          "closed again: input reaches the game");

    // ---- 8. shutdown restores the window
    g_tick_run = false;
    tick.join();
    st::overlay::Shutdown();
    Check(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) == &GameWndProc, "shutdown restores the window procedure");
    Frames(2);  // Present unhooked
    Check(!st::overlay::Installed(), "overlay uninstalled");

    // ---- 9. reinstall with the startup hint: drawn while the menu is closed
    opt.startup_hint = true;
    Check(st::overlay::Install(opt), "overlay re-installed (startup hint on)");
    st::overlay::SetAfterDrawForTest(&AfterDraw, nullptr);
    Frames(3);
    Check(!st::overlay::IsOpen() && st::overlay::GetStats().frames_drawn > 0, "startup hint drawn with the menu closed");
    // Installed after the swapchain was created: the ExecuteCommandLists capture finds the queue.
    Check(std::strstr(st::overlay::GetStats().queue_source, "ExecuteCommandLists") != nullptr,
          "fallback: queue captured via ExecuteCommandLists when the swapchain predates the hooks",
          st::overlay::GetStats().queue_source);
    Shot(L"overlay_hint.png", "startup hint visible", 0.002);
    st::overlay::Shutdown();
    Frames(1);

    // ---- 10. automation (hidden test runs, docs/hidden-testing.md)
    opt.startup_hint = false;
    opt.automation = true;
    Check(st::overlay::Install(opt), "overlay installed in automation mode");
    st::overlay::SetAfterDrawForTest(nullptr, nullptr);
    Frames(2);
    {
        // A caller that gave up: cancelled requests free their slots (more than the 4 slots
        // in a row, no frame in between) and are never served.
        bool all = true;
        std::uint32_t last = 0;
        for (int i = 0; i < 6; ++i) {
            last = st::overlay::RequestCapture();
            all = all && last != 0;
            st::overlay::CancelCapture(last);
        }
        GameFrame();
        st::overlay::Image img;
        char why[200] = {};
        const int cr = st::overlay::TakeCapture(last, &img, why, sizeof(why));
        std::snprintf(d, sizeof(d), "ids all non-zero=%d, take after cancel=%d (%s)", all ? 1 : 0, cr, why);
        Check(all && cr == -1, "automation: cancelled captures free their slots and are not served", d);
    }
    {
        // Menu closed, nothing drawn: the capture is the game's own frame (the
        // 10-bit back buffer from step 6, cleared to kClear).
        const std::uint32_t id = st::overlay::RequestCapture();
        st::overlay::Image img;
        char why[200] = {};
        int cr = 0;
        for (int i = 0; i < 400 && (cr = st::overlay::TakeCapture(id, &img, why, sizeof(why))) == 0; ++i) GameFrame();
        const int er = static_cast<int>(kClear[0] * 1023 + 0.5f) >> 2, eg = static_cast<int>(kClear[1] * 1023 + 0.5f) >> 2,
                  eb = static_cast<int>(kClear[2] * 1023 + 0.5f) >> 2;
        bool px_ok = cr == 1 && img.width == 1280 && img.height == 720 && img.rgb.size() == 1280u * 720u * 3u;
        if (px_ok)
            for (const std::size_t at : {std::size_t{0}, std::size_t{(360u * 1280u + 640u) * 3u}, std::size_t{(719u * 1280u + 1279u) * 3u}})
                px_ok = px_ok && std::abs(img.rgb[at] - er) <= 2 && std::abs(img.rgb[at + 1] - eg) <= 2 && std::abs(img.rgb[at + 2] - eb) <= 2;
        std::snprintf(d, sizeof(d), "result=%d %ux%u first=(%d,%d,%d) expected=(%d,%d,%d) %s", cr, img.width, img.height,
                      img.rgb.empty() ? -1 : img.rgb[0], img.rgb.empty() ? -1 : img.rgb[1], img.rgb.empty() ? -1 : img.rgb[2], er, eg,
                      eb, why);
        Check(px_ok, "automation: back-buffer capture with the menu closed (10-bit -> RGB8)", d);
        Check(st::overlay::GetStats().frames_drawn == 0, "automation: capturing draws nothing into the game's frame");
        std::string png;
        std::vector<unsigned char> back;
        UINT w = 0, h = 0;
        const bool enc = st::overlay::EncodePng(img, &png);
        bool same = enc && DecodePng(png, back, w, h) && w == img.width && h == img.height;
        for (std::size_t i = 0; same && i < static_cast<std::size_t>(w) * h; i += 7919)
            same = back[i * 4 + 2] == img.rgb[i * 3] && back[i * 4 + 1] == img.rgb[i * 3 + 1] && back[i * 4] == img.rgb[i * 3 + 2];
        std::snprintf(d, sizeof(d), "%zu bytes, decoded %ux%u", png.size(), w, h);
        Check(same, "automation: PNG encoder output decodes (WIC) to the same pixels", d);
        const std::wstring path = g_out + L"\\automation_capture.png";
        Check(st::overlay::WritePngFile(path, img), "automation: screenshot written", "automation_capture.png");
        // A synthetic gradient through the encoder (multiple stored blocks, odd width).
        st::overlay::Image grad;
        grad.width = 333;
        grad.height = 211;
        grad.rgb.resize(333u * 211u * 3u);
        for (std::size_t i = 0; i < grad.rgb.size(); ++i) grad.rgb[i] = static_cast<std::uint8_t>((i * 7) ^ (i >> 5));
        same = st::overlay::EncodePng(grad, &png) && DecodePng(png, back, w, h) && w == 333 && h == 211;
        for (std::size_t i = 0; same && i < 333u * 211u; ++i)
            same = back[i * 4 + 2] == grad.rgb[i * 3] && back[i * 4 + 1] == grad.rgb[i * 3 + 1] && back[i * 4] == grad.rgb[i * 3 + 2];
        Check(same, "automation: PNG round trip of a 333x211 gradient (every pixel)");
    }
    {
        using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
        HMODULE x14 = LoadLibraryW(L"xinput1_4.dll");
        const auto get = x14 ? reinterpret_cast<GetStateFn>(GetProcAddress(x14, "XInputGetState")) : nullptr;
        Check(get != nullptr && st::overlay::input::GetStats().xinput14_hook, "automation: xinput1_4 XInputGetState hooked (what ds.exe calls)");
        if (get) {
            XINPUT_STATE xs{};
            Check(get(0, &xs) == ERROR_DEVICE_NOT_CONNECTED && get(1, &xs) == ERROR_DEVICE_NOT_CONNECTED,
                  "automation: no virtual pad yet -> no pad at all (real pads isolated)");
            st::overlay::input::PadState p;
            p.connected = true;
            p.buttons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START;
            p.lx = 12000;
            p.rt = 200;
            st::overlay::input::SetVirtualPad(p);
            std::memset(&xs, 0, sizeof(xs));
            const DWORD r0 = get(0, &xs);
            const DWORD packet0 = xs.dwPacketNumber;
            std::snprintf(d, sizeof(d), "r=%lu buttons=0x%04X lx=%d rt=%u", r0, xs.Gamepad.wButtons, xs.Gamepad.sThumbLX, xs.Gamepad.bRightTrigger);
            Check(r0 == ERROR_SUCCESS && xs.Gamepad.wButtons == (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START) && xs.Gamepad.sThumbLX == 12000 &&
                      xs.Gamepad.bRightTrigger == 200,
                  "automation: virtual pad 0 reports the requested buttons/sticks", d);
            Check(get(1, &xs) == ERROR_DEVICE_NOT_CONNECTED, "automation: pads 1..3 stay disconnected");
            st::overlay::input::PadState idle;
            idle.connected = true;
            st::overlay::input::SetVirtualPad(idle);
            std::memset(&xs, 0, sizeof(xs));
            Check(get(0, &xs) == ERROR_SUCCESS && xs.Gamepad.wButtons == 0 && xs.dwPacketNumber != packet0,
                  "automation: release -> connected idle pad with a new packet number");
            st::overlay::SetOpen(true);
            Frames(2);
            st::overlay::input::SetVirtualPad(p);
            std::memset(&xs, 0, sizeof(xs));
            Check(get(0, &xs) == ERROR_SUCCESS && xs.Gamepad.wButtons == 0, "automation: open menu keeps the virtual pad idle for the game");
            SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
            Frames(2);
            Check(st::overlay::IsOpen(), "automation: focus loss does not close the menu (the hidden window never has focus)");
            st::overlay::SetOpen(false);
            Frames(2);
            st::overlay::input::SetVirtualPad(st::overlay::input::PadState{});
            Check(get(0, &xs) == ERROR_DEVICE_NOT_CONNECTED, "automation: padoff disconnects the virtual pad");
        }
        Check(GetForegroundWindow() == hwnd && st::overlay::input::GetStats().foreground_answers > 0,
              "automation: GetForegroundWindow answers the game window");
        const unsigned long clip0 = st::overlay::input::GetStats().suppressed_clip, pos0 = st::overlay::input::GetStats().suppressed_cursor_pos;
        POINT here{};
        GetCursorPos(&here);
        SetCursorPos(here.x, here.y);
        ClipCursor(nullptr);
        in = st::overlay::input::GetStats();
        Check(in.suppressed_clip == clip0 + 1 && in.suppressed_cursor_pos == pos0 + 1,
              "automation: SetCursorPos/ClipCursor never reach the system (menu closed)");
        const HRESULT p0 = GameFrame(), p1 = g.chain->Present(0, DXGI_PRESENT_TEST);
        std::snprintf(d, sizeof(d), "Present=0x%08lX test=0x%08lX masked so far=%lu", static_cast<unsigned long>(p0),
                      static_cast<unsigned long>(p1), st::overlay::GetStats().occluded_masked);
        Check(p0 != DXGI_STATUS_OCCLUDED && p1 != DXGI_STATUS_OCCLUDED, "automation: Present never answers DXGI_STATUS_OCCLUDED", d);
    }
    st::overlay::Shutdown();
    Frames(1);

    WaitGpu();
    if (g_game_hook) UnhookWindowsHookEx(g_game_hook);
    DestroyWindow(hwnd);
    std::printf("%s: %d passed, %d failed (log: %ls\\stranded_together.log)\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail,
                logdir.c_str());
    return g_fail ? 1 : 0;
}
