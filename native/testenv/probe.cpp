// st_testenv_probe: what Direct3D 12 does on a private, never-displayed desktop.
// Started by st_testenv_supervisor --probe (STARTUPINFO.lpDesktop = the private
// desktop). No game involved. Measures:
//   - the window: visible / foreground / active / focus answers on that desktop
//   - a hardware D3D12 device + DIRECT queue + flip-discard swapchain (like ds.exe)
//   - Present results (S_OK vs DXGI_STATUS_OCCLUDED) and frame rate, with vsync
//     (Present(1,0)) and without (Present(0,0)), plus a DXGI_PRESENT_TEST answer
//   - back-buffer capture with the mod's own code (st::overlay::CaptureBackBuffer):
//     every captured pixel must equal the frame's clear colour
//   - (diagnostic, not part of pass) adapters/outputs/mode lists, GDI current mode,
//     MonitorFromWindow: what a game asks before creating its device
// Writes <out>\probe.json and <out>\probe-frame.png. Exit 0 = frames rendered and captured.
//
//   st_testenv_probe --out <dir> [--seconds N] [--warp]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <shellapi.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "log.hpp"
#include "overlay_renderer.hpp"
#include "testenv_core.hpp"

namespace te = st::testenv;

// overlay_renderer.cpp links the overlay's ImGui build; its assertion sink lives in overlay.cpp.
void StOverlayImGuiAssert(const char*, const char*, int) {}

namespace {

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string DesktopName(HDESK d) {
    wchar_t n[256] = {};
    DWORD need = 0;
    if (!d || !GetUserObjectInformationW(d, UOI_NAME, n, sizeof(n) - sizeof(wchar_t), &need)) return "?";
    return te::Narrow(n);
}

std::string InputDesktopName() {
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!d) return "unavailable(" + std::to_string(GetLastError()) + ")";
    const std::string n = DesktopName(d);
    CloseDesktop(d);
    return n;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

struct Gfx {
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* chain = nullptr;
    ID3D12DescriptorHeap* rtv = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fence_value = 0;
    HANDLE event = nullptr;
    std::string adapter_name;
    bool warp = false;
};

void WaitGpu(Gfx& g) {
    g.queue->Signal(g.fence, ++g.fence_value);
    if (g.fence->GetCompletedValue() < g.fence_value) {
        g.fence->SetEventOnCompletion(g.fence_value, g.event);
        WaitForSingleObject(g.event, 5000);
    }
}

void Clear(Gfx& g, const float c[4]) {
    ID3D12Resource* bb = nullptr;
    if (FAILED(g.chain->GetBuffer(g.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb)))) return;
    g.alloc->Reset();
    g.list->Reset(g.alloc, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtv->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(bb, nullptr, h);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->ResourceBarrier(1, &b);
    g.list->ClearRenderTargetView(h, c, 0, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.list->ResourceBarrier(1, &b);
    g.list->Close();
    ID3D12CommandList* lists[] = {g.list};
    g.queue->ExecuteCommandLists(1, lists);
    bb->Release();
}

std::string Hr(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::wstring out = L".";
    unsigned seconds = 6;
    bool want_warp = false;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--out" && i + 1 < argc) out = argv[++i];
        else if (a == L"--seconds" && i + 1 < argc) seconds = static_cast<unsigned>(_wtoi(argv[++i]));
        else if (a == L"--warp") want_warp = true;
    }
    if (argv) LocalFree(argv);
    if (seconds < 2) seconds = 2;
    if (seconds > 60) seconds = 60;
    te::EnsureDir(out);
    st::LogInit(out);  // CaptureBackBuffer logs nothing; keeps Log() calls safe

    std::string j = "{";
    const auto add = [&](const char* k, const std::string& v, bool quote = true) {
        if (j.size() > 1) j += ",";
        j += "\"" + std::string(k) + "\":" + (quote ? "\"" + te::JsonEscape(v) + "\"" : v);
    };
    const std::string own_desktop = DesktopName(GetThreadDesktop(GetCurrentThreadId()));
    const std::string input_before = InputDesktopName();
    add("thread_desktop", own_desktop);
    add("input_desktop_before", input_before);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"StTestEnvProbe";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Stranded Together testenv probe", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        add("error", "CreateWindowEx failed " + std::to_string(GetLastError()));
        te::WriteFileAtomic(out + L"\\probe.json", j + "}\n");
        return 2;
    }
    ShowWindow(hwnd, SW_SHOW);  // what a game does; this desktop is never displayed
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    Pump();
    add("window_visible", IsWindowVisible(hwnd) ? "true" : "false", false);
    add("window_iconic", IsIconic(hwnd) ? "true" : "false", false);
    add("foreground_is_window", GetForegroundWindow() == hwnd ? "true" : "false", false);
    add("foreground_null", GetForegroundWindow() == nullptr ? "true" : "false", false);
    add("active_is_window", GetActiveWindow() == hwnd ? "true" : "false", false);
    add("focus_is_window", GetFocus() == hwnd ? "true" : "false", false);

    Gfx g;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&g.factory));
    if (SUCCEEDED(hr) && !want_warp) {
        for (UINT i = 0; g.factory->EnumAdapters1(i, &g.adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            g.adapter->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(g.adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device)))) {
                g.adapter_name = te::Narrow(d.Description);
                break;
            }
            SafeRelease(g.adapter);
        }
    }
    if (SUCCEEDED(hr) && !g.device) {
        IDXGIAdapter* w = nullptr;
        g.factory->EnumWarpAdapter(IID_PPV_ARGS(&w));
        hr = D3D12CreateDevice(w, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device));
        SafeRelease(w);
        g.warp = true;
        g.adapter_name = "WARP";
    }
    add("adapter", g.adapter_name);
    add("warp", g.warp ? "true" : "false", false);
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(hr) && g.device) hr = g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
    else if (SUCCEEDED(hr)) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = 1280;
        sd.Height = 720;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 3;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* c1 = nullptr;
        hr = g.factory->CreateSwapChainForHwnd(g.queue, hwnd, &sd, nullptr, nullptr, &c1);
        add("create_swapchain_hr", Hr(hr));
        if (SUCCEEDED(hr)) hr = c1->QueryInterface(IID_PPV_ARGS(&g.chain));
        SafeRelease(c1);
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        hr = g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtv));
        if (SUCCEEDED(hr)) hr = g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc));
        if (SUCCEEDED(hr)) hr = g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr, IID_PPV_ARGS(&g.list));
        if (SUCCEEDED(hr)) hr = g.list->Close();
        if (SUCCEEDED(hr)) hr = g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
        g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (FAILED(hr) || !g.chain) {
        add("error", "D3D12 setup failed " + Hr(hr));
        add("input_desktop_after", InputDesktopName());
        te::WriteFileAtomic(out + L"\\probe.json", j + "}\n");
        return 2;
    }
    IDXGIOutput* output = nullptr;
    add("containing_output_hr", Hr(g.chain->GetContainingOutput(&output)));
    SafeRelease(output);

    // What a game's start-up asks about the display before it creates its device
    // (adapters, outputs, mode lists, GDI display settings). Diagnostic only: not part of "pass".
    {
        std::string ad = "[";
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; i < 8 && g.factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            unsigned outputs = 0, modes = 0, attached = 0;
            HRESULT modes_hr = S_FALSE;
            IDXGIOutput* o = nullptr;
            for (UINT k = 0; k < 8 && a->EnumOutputs(k, &o) != DXGI_ERROR_NOT_FOUND; ++k) {
                ++outputs;
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(o->GetDesc(&od)) && od.AttachedToDesktop) ++attached;
                if (k == 0) {
                    UINT n = 0;
                    modes_hr = o->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &n, nullptr);
                    modes = n;
                }
                SafeRelease(o);
            }
            char b[300];
            std::snprintf(b, sizeof(b), "%s{\"name\":\"%s\",\"software\":%s,\"outputs\":%u,\"attached\":%u,\"modes_hr\":\"%s\",\"modes\":%u}",
                          i ? "," : "", te::JsonEscape(te::Narrow(d.Description)).c_str(),
                          (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "true" : "false", outputs, attached, Hr(modes_hr).c_str(), modes);
            ad += b;
            SafeRelease(a);
        }
        add("adapters", ad + "]", false);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        const BOOL cur = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
        char b[160];
        std::snprintf(b, sizeof(b), "{\"ok\":%s,\"width\":%lu,\"height\":%lu,\"hz\":%lu,\"error\":%lu}", cur ? "true" : "false",
                      cur ? dm.dmPelsWidth : 0ul, cur ? dm.dmPelsHeight : 0ul, cur ? dm.dmDisplayFrequency : 0ul, cur ? 0ul : GetLastError());
        add("gdi_current_mode", b, false);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        add("monitor_from_window", mon && GetMonitorInfoW(mon, &mi) ? "true" : "false", false);
        add("screen_size", std::to_string(GetSystemMetrics(SM_CXSCREEN)) + "x" + std::to_string(GetSystemMetrics(SM_CYSCREEN)));
    }

    // Phases: vsync, then uncapped.
    struct Phase {
        const char* name;
        UINT sync;
        unsigned frames = 0, ok = 0, occluded = 0, other = 0;
        HRESULT last_other = S_OK;
        double seconds = 0;
    } phases[2] = {{"vsync", 1}, {"uncapped", 0}};
    unsigned captures = 0, capture_ok = 0, frame_no = 0;
    std::string capture_note;
    st::overlay::Image last;
    LARGE_INTEGER qpf, t0, t1;
    QueryPerformanceFrequency(&qpf);
    for (Phase& ph : phases) {
        QueryPerformanceCounter(&t0);
        const double budget = seconds / 2.0;
        for (;;) {
            QueryPerformanceCounter(&t1);
            const double el = static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(qpf.QuadPart);
            if (el >= budget) {
                ph.seconds = el;
                break;
            }
            ++frame_no;
            const float c[4] = {static_cast<float>(frame_no % 256) / 255.0f, 0.5f, 0.25f, 1.0f};
            Clear(g, c);
            if (frame_no % 37 == 5) {  // capture exactly what is about to be presented
                st::overlay::Image img;
                char why[200] = {};
                ++captures;
                if (st::overlay::CaptureBackBuffer(g.chain, g.queue, &img, why, sizeof(why), 3000)) {
                    const int er = static_cast<int>(std::lround(c[0] * 255.0f)), eg = static_cast<int>(std::lround(c[1] * 255.0f)),
                              eb = static_cast<int>(std::lround(c[2] * 255.0f));
                    bool all = img.width == 1280 && img.height == 720;
                    for (std::size_t i = 0; all && i < img.rgb.size(); i += 3)
                        all = std::abs(img.rgb[i] - er) <= 1 && std::abs(img.rgb[i + 1] - eg) <= 1 && std::abs(img.rgb[i + 2] - eb) <= 1;
                    if (all) ++capture_ok;
                    else if (capture_note.empty()) capture_note = "pixel mismatch at frame " + std::to_string(frame_no);
                    last = std::move(img);
                } else if (capture_note.empty()) {
                    capture_note = why;
                }
            }
            const HRESULT p = g.chain->Present(ph.sync, 0);
            ++ph.frames;
            if (p == S_OK) ++ph.ok;
            else if (p == DXGI_STATUS_OCCLUDED) ++ph.occluded;
            else ++ph.other, ph.last_other = p;
            WaitGpu(g);
            Pump();
        }
    }
    const HRESULT test = g.chain->Present(0, DXGI_PRESENT_TEST);
    for (const Phase& ph : phases) {
        char b[300];
        std::snprintf(b, sizeof(b), "{\"frames\":%u,\"fps\":%.1f,\"present_ok\":%u,\"present_occluded\":%u,\"present_other\":%u,\"last_other\":\"%s\"}",
                      ph.frames, ph.seconds > 0 ? ph.frames / ph.seconds : 0.0, ph.ok, ph.occluded, ph.other, Hr(ph.last_other).c_str());
        add(ph.name, b, false);
    }
    add("present_test_hr", Hr(test) + (test == DXGI_STATUS_OCCLUDED ? " (DXGI_STATUS_OCCLUDED)" : test == S_OK ? " (S_OK)" : ""));
    add("captures", std::to_string(captures), false);
    add("captures_exact", std::to_string(capture_ok), false);
    add("capture_note", capture_note);
    const bool png = last.width && st::overlay::WritePngFile(out + L"\\probe-frame.png", last);
    add("png", png ? "probe-frame.png" : "");
    add("window_visible_after", IsWindowVisible(hwnd) ? "true" : "false", false);
    add("foreground_is_window_after", GetForegroundWindow() == hwnd ? "true" : "false", false);
    const std::string input_after = InputDesktopName();
    add("input_desktop_after", input_after);
    const bool rendered = phases[0].frames > 0 && phases[1].frames > 0;
    const bool pass = rendered && captures > 0 && capture_ok == captures && input_after == input_before;
    add("pass", pass ? "true" : "false", false);
    te::WriteFileAtomic(out + L"\\probe.json", j + "}\n");

    WaitGpu(g);
    SafeRelease(g.fence);
    SafeRelease(g.list);
    SafeRelease(g.alloc);
    SafeRelease(g.rtv);
    SafeRelease(g.chain);
    SafeRelease(g.queue);
    SafeRelease(g.device);
    SafeRelease(g.adapter);
    SafeRelease(g.factory);
    if (g.event) CloseHandle(g.event);
    DestroyWindow(hwnd);
    st::LogShutdown();
    return pass ? 0 : 1;
}
