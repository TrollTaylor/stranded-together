// st_testenv_fakegame: a stand-in for ds.exe in the end-to-end ctest
// (st_testenv_e2e). Copied as ds.exe into a scratch "game folder" next to the
// real mod (winhttp.dll): it imports winhttp.dll like ds.exe, so the mod loads,
// reads session.ini (automation=1), installs the overlay and starts the
// automation channel. game::Resolve finds nothing in this exe, so co-op stays
// off; the overlay, screenshots, the virtual pad and quit are exercised for real
// on the supervisor's private desktop.
//
// Like ds.exe it calls XInputGetState through a pointer from
// LoadLibrary("xinput1_4.dll") + GetProcAddress, and renders with D3D12 on a
// DIRECT queue into a flip-discard swapchain. Each press of A (edge) toggles the
// clear colour between kColors[0] and kColors[1]. Lines go to
// <exe dir>\stranded_together\fakegame.log. WM_CLOSE (the mod's quit) exits.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <winhttp.h>
#include <xinput.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

const float kColors[2][4] = {{0.10f, 0.30f, 0.40f, 1.0f}, {0.80f, 0.20f, 0.10f, 1.0f}};
FILE* g_log = nullptr;
bool g_quit = false;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(g_log, "[%8.3f] ", static_cast<double>(GetTickCount64() % 100000000ull) / 1000.0);
    std::vfprintf(g_log, fmt, ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
    va_end(ap);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {
        Log("WM_CLOSE: exiting");
        g_quit = true;
        DestroyWindow(h);
        return 0;
    }
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (m == WM_KEYDOWN) Log("WM_KEYDOWN vk=0x%02X", static_cast<unsigned>(w));
    return DefWindowProcW(h, m, w, l);
}

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string ReadAll(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "<open failed " + std::to_string(GetLastError()) + ">";
    char buf[256] = {};
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    return std::string(buf, got);
}

bool WriteAll(const std::wstring& path, const char* text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const BOOL ok = WriteFile(f, text, static_cast<DWORD>(std::strlen(text)), &w, nullptr);
    CloseHandle(f);
    return ok != FALSE;
}

// Like ds.exe's "user:" device, at start-up: read the profile, write a checkpoint
// through a temp file + MoveFileExW, make a folder, copy, delete, rename with MoveFileW,
// look a file up through the ANSI API, enumerate. The e2e points this at a scratch
// "real" save root (ST_FAKEGAME_SAVE_ROOT); the mod must redirect all of it.
void SaveExercise(const std::wstring& root) {
    const std::wstring base = root + L"\\DeathStrandingDC\\1";
    Log("save read profile.dat: '%s'", ReadAll(base + L"\\profile\\profile.dat").c_str());
    std::wstring slashed = base + L"/profile/profile.dat";
    Log("save read with '/' separators: '%s'", ReadAll(slashed).c_str());
    int ok = 0, total = 0;
    const auto step = [&](bool r, const char* what) {
        ++total;
        ok += r ? 1 : 0;
        Log("save %s: %s (error %lu)", what, r ? "ok" : "FAILED", r ? 0ul : GetLastError());
    };
    CreateDirectoryW((base + L"\\autosave0").c_str(), nullptr);
    step(WriteAll(base + L"\\autosave0\\checkpoint.dattemp", "fake checkpoint"), "write checkpoint.dattemp");
    step(MoveFileExW((base + L"\\autosave0\\checkpoint.dattemp").c_str(), (base + L"\\autosave0\\checkpoint.dat").c_str(),
                     MOVEFILE_REPLACE_EXISTING) != FALSE,
         "MoveFileExW temp -> checkpoint.dat");
    step(CreateDirectoryW((base + L"\\autosave1").c_str(), nullptr) != FALSE, "CreateDirectoryW autosave1");
    step(CopyFileW((base + L"\\autosave0\\checkpoint.dat").c_str(), (base + L"\\autosave1\\checkpoint.dat").c_str(), FALSE) != FALSE,
         "CopyFileW autosave0 -> autosave1");
    step(DeleteFileW((base + L"\\quicksave0\\checkpoint.dat").c_str()) != FALSE, "DeleteFileW quicksave0");
    step(WriteAll(base + L"\\manual.tmp", "m") && MoveFileW((base + L"\\manual.tmp").c_str(), (base + L"\\manual.dat").c_str()) != FALSE,
         "MoveFileW manual.tmp -> manual.dat");
    char narrow[1024] = {};
    WideCharToMultiByte(CP_ACP, 0, (base + L"\\profile\\profile.dat").c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
    step(GetFileAttributesA(narrow) != INVALID_FILE_ATTRIBUTES, "GetFileAttributesA profile.dat");
    WIN32_FIND_DATAW fd;
    int entries = 0;
    HANDLE h = FindFirstFileExW((base + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do ++entries;
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    step(entries > 2, "FindFirstFileExW lists the save folder");
    // Other spellings of the same folders (each must land in the scratch copy as well):
    // a "\\.\" device path, a trailing dot (Win32 drops it), a path relative to the
    // current directory, and the 8.3 short name of the folder (when the volume has them).
    step(WriteAll(L"\\\\.\\" + base + L"\\autosave0\\device.dat", "d"), "write through a \\\\.\\ path");
    step(WriteAll(base + L"\\autosave0.\\trailing-dot.dat", "t"), "write through 'autosave0.' (trailing dot)");
    wchar_t cwd[1024] = {};
    const DWORD cn = GetCurrentDirectoryW(1024, cwd);
    std::wstring parent(cwd, cn < 1024 ? cn : 0);
    parent = parent.substr(0, parent.find_last_of(L'\\'));
    const bool sibling = !parent.empty() && _wcsnicmp(base.c_str(), (parent + L"\\").c_str(), parent.size() + 1) == 0;
    step(sibling && WriteAll(L"..\\" + base.substr(parent.size() + 1) + L"\\autosave0\\relative.dat", "r"),
         "write through a path relative to the current directory");
    wchar_t short_base[1024] = {};
    const DWORD sn = GetShortPathNameW(base.c_str(), short_base, 1024);
    Log("save 8.3 spelling: %s", sn > 0 && sn < 1024 && _wcsicmp(short_base, base.c_str()) != 0 ? "yes" : "none on this volume");
    step(sn > 0 && sn < 1024 && WriteAll(std::wstring(short_base) + L"\\autosave0\\short.dat", "s"), "write through the 8.3 short path");
    Log("save exercise: %d/%d ok, %d entries", ok, total, entries);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // ds.exe imports winhttp.dll; so do we (the mod is the winhttp.dll next to us).
    const BOOL winhttp = WinHttpCheckPlatform();
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    CreateDirectoryW((dir + L"\\stranded_together").c_str(), nullptr);
    _wfopen_s(&g_log, (dir + L"\\stranded_together\\fakegame.log").c_str(), L"w");
    Log("fake game started (pid %lu), WinHttpCheckPlatform=%d", GetCurrentProcessId(), winhttp);
    wchar_t save_root[1024] = {};
    const DWORD sr = GetEnvironmentVariableW(L"ST_FAKEGAME_SAVE_ROOT", save_root, 1024);
    if (sr > 0 && sr < 1024) SaveExercise(save_root);
    Sleep(1500);  // a real game takes a while before its swapchain; the mod's overlay hooks are in by then

    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    HMODULE x14 = LoadLibraryW(L"xinput1_4.dll");
    const auto get_state = x14 ? reinterpret_cast<GetStateFn>(GetProcAddress(x14, "XInputGetState")) : nullptr;
    Log("xinput1_4 XInputGetState %s", get_state ? "found" : "MISSING");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"StTestEnvFakeGame";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Stranded Together fake game", WS_OVERLAPPEDWINDOW, 0, 0, 960, 540, nullptr,
                                nullptr, inst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* chain = nullptr;
    ID3D12DescriptorHeap* rtv = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fv = 0;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) && factory) {
        IDXGIAdapter* w = nullptr;
        factory->EnumWarpAdapter(IID_PPV_ARGS(&w));
        hr = D3D12CreateDevice(w, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        SafeRelease(w);
        Log("using WARP");
    }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = 960;
        sd.Height = 540;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 3;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* c1 = nullptr;
        hr = factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &c1);
        if (SUCCEEDED(hr)) hr = c1->QueryInterface(IID_PPV_ARGS(&chain));
        SafeRelease(c1);
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv));
        if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) hr = list->Close();
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    }
    Log("D3D12 setup hr=0x%08lX; GetForegroundWindow()==window: %s", static_cast<unsigned long>(hr),
        GetForegroundWindow() == hwnd ? "yes" : "no");
    if (FAILED(hr)) return 2;

    int color = 0;
    bool a_was_down = false;
    DWORD last_packet = 0, last_result = 0xFFFFFFFF;
    unsigned long frames = 0;
    const ULONGLONG end = GetTickCount64() + 300000;  // never outlives a run
    MSG msg;
    while (!g_quit && GetTickCount64() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) break;
        if (get_state) {
            XINPUT_STATE xs{};
            const DWORD r = get_state(0, &xs);
            if (r != last_result) {
                Log("pad 0 %s", r == ERROR_SUCCESS ? "connected" : "not connected");
                last_result = r;
            }
            if (r == ERROR_SUCCESS && xs.dwPacketNumber != last_packet) {
                last_packet = xs.dwPacketNumber;
                const bool a = (xs.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
                if (a && !a_was_down) {
                    color ^= 1;
                    Log("A pressed (packet %lu): colour %d", xs.dwPacketNumber, color);
                }
                a_was_down = a;
            }
        }
        ID3D12Resource* bb = nullptr;
        if (FAILED(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb)))) break;
        alloc->Reset();
        list->Reset(alloc, nullptr);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(bb, nullptr, h);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        list->ClearRenderTargetView(h, kColors[color], 0, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        bb->Release();
        chain->Present(1, 0);
        queue->Signal(fence, ++fv);
        if (fence->GetCompletedValue() < fv) {
            fence->SetEventOnCompletion(fv, ev);
            WaitForSingleObject(ev, 5000);
        }
        if (++frames % 300 == 0) Log("frames %lu", frames);
        if (frames == 120) Log("after 120 frames: GetForegroundWindow()==window: %s", GetForegroundWindow() == hwnd ? "yes" : "no");
    }
    Log("exit after %lu frames", frames);
    SafeRelease(fence);
    SafeRelease(list);
    SafeRelease(alloc);
    SafeRelease(rtv);
    SafeRelease(chain);
    SafeRelease(queue);
    SafeRelease(device);
    SafeRelease(factory);
    if (g_log) std::fclose(g_log);
    return 0;
}
