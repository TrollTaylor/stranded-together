#include "overlay_renderer.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "log.hpp"

namespace st::overlay {
namespace {

template <class T>
void SafeRelease(T*& p) noexcept {
    if (p) p->Release();
    p = nullptr;
}

void Why(char* why, std::size_t cap, const char* what, HRESULT hr) noexcept {
    if (why && cap) std::snprintf(why, cap, "%s failed (hr=0x%08lX)", what, static_cast<unsigned long>(hr));
}

}  // namespace

DXGI_FORMAT RtvFormatFor(DXGI_FORMAT f) noexcept {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:   // HDR10 / 10-bit
        case DXGI_FORMAT_R16G16B16A16_FLOAT:  // scRGB HDR
            return f;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

void Renderer::SrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    auto* self = static_cast<Renderer*>(info->UserData);
    cpu->ptr = 0;
    gpu->ptr = 0;
    for (UINT i = 0; i < kSrvDescriptors; ++i) {
        if (self->srv_used_[i]) continue;
        self->srv_used_[i] = true;
        D3D12_CPU_DESCRIPTOR_HANDLE c = self->srv_heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE g = self->srv_heap_->GetGPUDescriptorHandleForHeapStart();
        c.ptr += static_cast<SIZE_T>(i) * self->srv_step_;
        g.ptr += static_cast<UINT64>(i) * self->srv_step_;
        *cpu = c;
        *gpu = g;
        return;
    }
    Log("[overlay] out of SRV descriptors for ImGui textures");
}

void Renderer::SrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    auto* self = static_cast<Renderer*>(info->UserData);
    const SIZE_T base = self->srv_heap_->GetCPUDescriptorHandleForHeapStart().ptr;
    if (cpu.ptr < base || !self->srv_step_) return;
    const SIZE_T i = (cpu.ptr - base) / self->srv_step_;
    if (i < kSrvDescriptors) self->srv_used_[i] = false;
}

bool Renderer::Init(IDXGISwapChain3* chain, ID3D12CommandQueue* game_queue, char* why, std::size_t why_cap) noexcept {
    Shutdown();
    HRESULT hr = chain->GetDevice(IID_PPV_ARGS(&device_));
    if (FAILED(hr) || !device_) {
        Why(why, why_cap, "IDXGISwapChain::GetDevice(ID3D12Device)", hr);
        device_ = nullptr;
        return false;
    }
    chain_ = chain;
    chain_->AddRef();
    game_queue_ = game_queue;
    game_queue_->AddRef();

    const auto fail = [&](const char* what, HRESULT h) {
        Why(why, why_cap, what, h);
        Shutdown();
        return false;
    };

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    hr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&upload_queue_));
    if (FAILED(hr)) return fail("CreateCommandQueue(upload)", hr);
    upload_queue_->SetName(L"StrandedTogether overlay uploads");

    D3D12_DESCRIPTOR_HEAP_DESC rd = {};
    rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rd.NumDescriptors = kSlots;
    hr = device_->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtv_heap_));
    if (FAILED(hr)) return fail("CreateDescriptorHeap(RTV)", hr);
    rtv_step_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC sd = {};
    sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sd.NumDescriptors = kSrvDescriptors;
    sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device_->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&srv_heap_));
    if (FAILED(hr)) return fail("CreateDescriptorHeap(SRV)", hr);
    srv_step_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (Slot& s : slots_) {
        hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator));
        if (FAILED(hr)) return fail("CreateCommandAllocator", hr);
        s.fence_value = 0;
    }
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slots_[0].allocator, nullptr, IID_PPV_ARGS(&list_));
    if (FAILED(hr)) return fail("CreateCommandList", hr);
    list_->Close();
    list_->SetName(L"StrandedTogether overlay");

    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr)) return fail("CreateFence", hr);
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) return fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
    fence_last_ = 0;

    UINT w = 0, h = 0;
    DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
    if (!BackBufferInfo(&w, &h, &f)) return fail("GetBuffer(current)", E_FAIL);
    if (!render_target_ok_) {
        // A back buffer without ALLOW_RENDER_TARGET (compute-only swapchain usage)
        // cannot take an RTV; creating one anyway is undefined behaviour.
        if (why) std::snprintf(why, why_cap, "the back buffer is not a render target (no ALLOW_RENDER_TARGET)");
        Shutdown();
        return false;
    }
    const DXGI_FORMAT rtv = RtvFormatFor(f);
    if (rtv == DXGI_FORMAT_UNKNOWN) {
        if (why) std::snprintf(why, why_cap, "unsupported back-buffer format %d", static_cast<int>(f));
        Shutdown();
        return false;
    }
    if (!InitBackend(rtv)) return fail("ImGui DX12 backend (pipeline/root signature)", E_FAIL);
    return true;
}

bool Renderer::InitBackend(DXGI_FORMAT rtv_format) noexcept {
    ImGui_ImplDX12_InitInfo info;
    info.Device = device_;
    info.CommandQueue = upload_queue_;  // texture uploads wait on our queue, never the game's
    info.NumFramesInFlight = static_cast<int>(kSlots);
    info.RTVFormat = rtv_format;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.UserData = this;
    info.SrvDescriptorHeap = srv_heap_;
    info.SrvDescriptorAllocFn = &Renderer::SrvAlloc;
    info.SrvDescriptorFreeFn = &Renderer::SrvFree;
    if (!ImGui_ImplDX12_Init(&info)) return false;
    backend_ = true;
    // Build the pipeline now so a failure is a clean "no overlay", not an
    // assertion inside NewFrame.
    if (!ImGui_ImplDX12_CreateDeviceObjects()) {
        ShutdownBackend();
        return false;
    }
    rtv_format_ = rtv_format;
    return true;
}

void Renderer::ShutdownBackend() noexcept {
    if (!backend_) return;
    ImGui_ImplDX12_Shutdown();
    backend_ = false;
    rtv_format_ = DXGI_FORMAT_UNKNOWN;
    for (bool& u : srv_used_) u = false;
}

bool Renderer::BackBufferInfo(UINT* width, UINT* height, DXGI_FORMAT* format) noexcept {
    if (!chain_) return false;
    ID3D12Resource* bb = nullptr;
    const UINT idx = chain_->GetCurrentBackBufferIndex();
    if (FAILED(chain_->GetBuffer(idx, IID_PPV_ARGS(&bb))) || !bb) return false;
    const D3D12_RESOURCE_DESC d = bb->GetDesc();
    bb->Release();
    render_target_ok_ = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 && d.SampleDesc.Count == 1;
    *width = static_cast<UINT>(d.Width);
    *height = d.Height;
    *format = d.Format;
    return true;
}

bool Renderer::EnsureFormat(DXGI_FORMAT buffer_format) noexcept {
    const DXGI_FORMAT rtv = RtvFormatFor(buffer_format);
    if (rtv == DXGI_FORMAT_UNKNOWN) return false;
    if (backend_ && rtv == rtv_format_) return true;
    if (!WaitIdle(2000)) return false;
    Log("[overlay] back-buffer format %d -> rebuilding the ImGui pipeline for RTV format %d", static_cast<int>(buffer_format),
        static_cast<int>(rtv));
    ShutdownBackend();
    return InitBackend(rtv);
}

bool Renderer::WaitFor(UINT64 value, DWORD timeout_ms) noexcept {
    if (!fence_ || value == 0) return true;
    if (fence_->GetCompletedValue() >= value) return true;
    if (FAILED(fence_->SetEventOnCompletion(value, fence_event_))) return false;
    return WaitForSingleObject(fence_event_, timeout_ms) == WAIT_OBJECT_0;
}

bool Renderer::WaitIdle(DWORD timeout_ms) noexcept { return WaitFor(fence_last_, timeout_ms); }

bool Renderer::Idle() const noexcept { return !fence_ || fence_last_ == 0 || fence_->GetCompletedValue() >= fence_last_; }

bool Renderer::Draw(ImDrawData* data) noexcept {
    if (!backend_ || !data || data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0) return false;
    Slot& slot = slots_[draws_ % kSlots];
    if (!WaitFor(slot.fence_value, 2000)) return false;  // GPU far behind: skip, never block the game longer

    // ImGui texture uploads (font atlas glyphs) run on our upload queue inside
    // RenderDrawData, with barriers on the atlas texture. The game queue may
    // still be sampling that texture for an earlier menu frame, and two queues
    // touching one resource need explicit ordering: let our earlier draws finish.
    if (data->Textures) {
        bool uploads = false;
        for (ImTextureData* t : *data->Textures)
            if (t && t->Status != ImTextureStatus_OK) uploads = true;
        if (uploads && !WaitIdle(2000)) return false;
    }

    const UINT idx = chain_->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    if (FAILED(chain_->GetBuffer(idx, IID_PPV_ARGS(&bb))) || !bb) return false;
    const D3D12_RESOURCE_DESC desc = bb->GetDesc();
    if (RtvFormatFor(desc.Format) != rtv_format_ || !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        desc.SampleDesc.Count != 1) {
        bb->Release();
        return false;  // the caller re-runs EnsureFormat next frame
    }

    if (FAILED(slot.allocator->Reset()) || FAILED(list_->Reset(slot.allocator, nullptr))) {
        bb->Release();
        return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(draws_ % kSlots) * rtv_step_;
    D3D12_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = rtv_format_;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device_->CreateRenderTargetView(bb, &rd, rtv);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = bb;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list_->ResourceBarrier(1, &barrier);
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {srv_heap_};
    list_->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(data, list_);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list_->ResourceBarrier(1, &barrier);
    const HRESULT closed = list_->Close();
    bb->Release();  // the swapchain owns it; the GPU work is ordered before Present on this queue
    if (FAILED(closed)) {
        ++draws_;  // the backend advanced its frame index; stay in step with it
        return false;
    }
    ID3D12CommandList* lists[] = {list_};
    game_queue_->ExecuteCommandLists(1, lists);
    // Only a value that was really signalled may be waited on later (an unsignalled
    // one would make every WaitIdle/teardown sit out its full timeout).
    const UINT64 value = fence_last_ + 1;
    if (SUCCEEDED(game_queue_->Signal(fence_, value))) {
        fence_last_ = value;
        slot.fence_value = value;
    }
    ++draws_;
    if (after_draw_) after_draw_(chain_, game_queue_, after_draw_user_);
    return true;
}

void Renderer::Shutdown() noexcept {
    if (fence_) WaitIdle(2000);
    ShutdownBackend();
    SafeRelease(list_);
    for (Slot& s : slots_) {
        SafeRelease(s.allocator);
        s.fence_value = 0;
    }
    SafeRelease(fence_);
    if (fence_event_) CloseHandle(fence_event_);
    fence_event_ = nullptr;
    fence_last_ = 0;
    SafeRelease(srv_heap_);
    SafeRelease(rtv_heap_);
    SafeRelease(upload_queue_);
    SafeRelease(game_queue_);
    SafeRelease(chain_);
    SafeRelease(device_);
    draws_ = 0;
}

// ---------------------------------------------------------------- screenshots
namespace {

float HalfToFloat(std::uint16_t h) noexcept {
    const std::uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31) v = mant ? 0.0f : 65504.0f;  // NaN -> 0, Inf -> max
    else v = std::ldexp(static_cast<float>(mant | 0x400u), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

std::uint8_t LinearToSrgb8(float v) noexcept {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(s * 255.0f + 0.5f);
}

bool Convertible(DXGI_FORMAT f) noexcept {
    std::uint8_t px[8] = {};
    std::uint8_t rgb[3];
    return ConvertRowToRgb(f, px, 1, rgb);
}

}  // namespace

bool ConvertRowToRgb(DXGI_FORMAT f, const std::uint8_t* row, unsigned width, std::uint8_t* rgb) noexcept {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            for (unsigned x = 0; x < width; ++x) {
                rgb[x * 3 + 0] = row[x * 4 + 0];
                rgb[x * 3 + 1] = row[x * 4 + 1];
                rgb[x * 3 + 2] = row[x * 4 + 2];
            }
            return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            for (unsigned x = 0; x < width; ++x) {
                rgb[x * 3 + 0] = row[x * 4 + 2];
                rgb[x * 3 + 1] = row[x * 4 + 1];
                rgb[x * 3 + 2] = row[x * 4 + 0];
            }
            return true;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            for (unsigned x = 0; x < width; ++x) {
                std::uint32_t v;
                std::memcpy(&v, row + x * 4, 4);
                rgb[x * 3 + 0] = static_cast<std::uint8_t>((v & 0x3FFu) >> 2);
                rgb[x * 3 + 1] = static_cast<std::uint8_t>(((v >> 10) & 0x3FFu) >> 2);
                rgb[x * 3 + 2] = static_cast<std::uint8_t>(((v >> 20) & 0x3FFu) >> 2);
            }
            return true;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            for (unsigned x = 0; x < width; ++x) {
                for (unsigned c = 0; c < 3; ++c) {
                    std::uint16_t h;
                    std::memcpy(&h, row + x * 8 + c * 2, 2);
                    rgb[x * 3 + c] = LinearToSrgb8(HalfToFloat(h));
                }
            }
            return true;
        default: return false;
    }
}

bool CaptureBackBuffer(IDXGISwapChain3* chain, ID3D12CommandQueue* queue, Image* out, char* why, std::size_t why_cap,
                       DWORD timeout_ms) noexcept {
    if (!chain || !queue || !out) {
        if (why && why_cap) std::snprintf(why, why_cap, "no swapchain/queue");
        return false;
    }
    ID3D12Device* device = nullptr;
    ID3D12Resource* bb = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    bool ok = false;
    HRESULT hr = S_OK;
    do {
        hr = chain->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(hr) || !device) {
            Why(why, why_cap, "IDXGISwapChain::GetDevice", hr);
            break;
        }
        hr = chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
        if (FAILED(hr) || !bb) {
            Why(why, why_cap, "GetBuffer(current)", hr);
            break;
        }
        const D3D12_RESOURCE_DESC d = bb->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 || d.DepthOrArraySize != 1 ||
            d.Width == 0 || d.Height == 0 || d.Width > 16384 || d.Height > 16384) {
            if (why && why_cap)
                std::snprintf(why, why_cap, "back buffer %llux%u samples %u is not a plain 2D texture",
                              static_cast<unsigned long long>(d.Width), d.Height, d.SampleDesc.Count);
            break;
        }
        if (!Convertible(d.Format)) {
            if (why && why_cap) std::snprintf(why, why_cap, "back-buffer format %d cannot be converted", static_cast<int>(d.Format));
            break;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT rows = 0;
        UINT64 row_bytes = 0, total = 0;
        device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &row_bytes, &total);
        if (total == 0 || total == ~0ull || rows < d.Height) {
            if (why && why_cap) std::snprintf(why, why_cap, "GetCopyableFootprints gave nothing");
            break;
        }
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
        hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&readback));
        if (FAILED(hr)) {
            Why(why, why_cap, "CreateCommittedResource(readback)", hr);
            break;
        }
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) {
            Why(why, why_cap, "capture command objects", hr);
            break;
        }
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
            Why(why, why_cap, "CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
            break;
        }
        list->SetName(L"StrandedTogether screenshot");
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = bb;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &b);
        hr = list->Close();
        if (FAILED(hr)) {
            Why(why, why_cap, "capture list Close", hr);
            break;
        }
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        hr = queue->Signal(fence, 1);
        if (FAILED(hr)) {
            // Nothing tells us when the copy is done: never free what the GPU may still write.
            if (why && why_cap) std::snprintf(why, why_cap, "Signal failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
            return false;  // deliberate leak of the capture objects (device removed in practice)
        }
        if (fence->GetCompletedValue() < 1) {
            const bool armed = SUCCEEDED(fence->SetEventOnCompletion(1, event));
            DWORD waited = armed ? WaitForSingleObject(event, timeout_ms) : WAIT_FAILED;
            if (waited != WAIT_OBJECT_0 && armed) waited = WaitForSingleObject(event, 10000);  // long GPU frame: once more
            if (waited != WAIT_OBJECT_0 && fence->GetCompletedValue() < 1) {
                if (why && why_cap) std::snprintf(why, why_cap, "the GPU copy did not finish in time (device hung?)");
                return false;  // deliberate leak: releasing objects under a live copy is undefined
            }
        }
        void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
        hr = readback->Map(0, &range, &mapped);
        if (FAILED(hr) || !mapped) {
            Why(why, why_cap, "readback Map", hr);
            break;
        }
        const unsigned w = static_cast<unsigned>(d.Width), h = d.Height;
        bool sized = false;
        try {
            out->rgb.assign(static_cast<std::size_t>(w) * h * 3, 0);
            sized = true;
        } catch (...) {
        }
        if (sized) {
            out->width = w;
            out->height = h;
            const auto* base = static_cast<const std::uint8_t*>(mapped) + fp.Offset;
            for (unsigned y = 0; y < h; ++y)
                ConvertRowToRgb(d.Format, base + static_cast<std::size_t>(y) * fp.Footprint.RowPitch, w,
                                out->rgb.data() + static_cast<std::size_t>(y) * w * 3);
            ok = true;
        } else if (why && why_cap) {
            std::snprintf(why, why_cap, "out of memory for a %ux%u image", w, h);
        }
        D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
    } while (false);
    if (event) CloseHandle(event);
    SafeRelease(fence);
    SafeRelease(list);
    SafeRelease(alloc);
    SafeRelease(readback);
    SafeRelease(bb);
    SafeRelease(device);
    return ok;
}

namespace {

std::uint32_t Crc32(const std::uint8_t* p, std::size_t n) noexcept {
    struct Table {
        std::uint32_t t[256];
        Table() noexcept {
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
        }
    };
    static const Table table;
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) crc = table.t[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

void Be32(std::string& s, std::uint32_t v) {
    s += static_cast<char>(v >> 24);
    s += static_cast<char>((v >> 16) & 0xFF);
    s += static_cast<char>((v >> 8) & 0xFF);
    s += static_cast<char>(v & 0xFF);
}

void Chunk(std::string& png, const char* type, const std::string& data) {
    Be32(png, static_cast<std::uint32_t>(data.size()));
    const std::size_t at = png.size();
    png.append(type, 4);
    png += data;
    Be32(png, Crc32(reinterpret_cast<const std::uint8_t*>(png.data() + at), png.size() - at));
}

}  // namespace

bool EncodePng(const Image& img, std::string* png) noexcept {
    if (!png || !img.width || !img.height || img.rgb.size() != static_cast<std::size_t>(img.width) * img.height * 3) return false;
    try {
        const std::size_t stride = static_cast<std::size_t>(img.width) * 3 + 1;  // filter byte + RGB
        const std::size_t raw = stride * img.height;
        if (raw > 0x70000000u) return false;
        std::string z;
        z.reserve(raw + raw / 65535 * 5 + 16);
        z += static_cast<char>(0x78);
        z += static_cast<char>(0x01);
        std::uint32_t a = 1, b = 0;  // Adler-32 of the uncompressed stream
        std::size_t left = raw, row = 0, col = 0;
        while (left > 0) {
            const std::size_t n = left < 65535 ? left : 65535;
            left -= n;
            z += static_cast<char>(left == 0 ? 1 : 0);  // BFINAL, BTYPE=00 (stored)
            z += static_cast<char>(n & 0xFF);
            z += static_cast<char>((n >> 8) & 0xFF);
            z += static_cast<char>(~n & 0xFF);
            z += static_cast<char>((~n >> 8) & 0xFF);
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint8_t byte = col == 0 ? std::uint8_t{0} : img.rgb[row * (stride - 1) + col - 1];
                z += static_cast<char>(byte);
                a = (a + byte) % 65521u;
                b = (b + a) % 65521u;
                if (++col == stride) col = 0, ++row;
            }
        }
        Be32(z, (b << 16) | a);
        std::string ihdr;
        Be32(ihdr, img.width);
        Be32(ihdr, img.height);
        ihdr += static_cast<char>(8);  // bit depth
        ihdr += static_cast<char>(2);  // colour type: RGB
        ihdr += std::string(3, '\0');  // deflate, adaptive filtering, no interlace
        png->assign("\x89PNG\r\n\x1a\n", 8);
        Chunk(*png, "IHDR", ihdr);
        Chunk(*png, "IDAT", z);
        Chunk(*png, "IEND", std::string());
        return true;
    } catch (...) {
        return false;
    }
}

bool WritePngFile(const std::wstring& path, const Image& image) noexcept {
    std::string png;
    if (!EncodePng(image, &png)) return false;
    try {
        const std::wstring tmp = path + L".tmp";
        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        const bool ok = WriteFile(f, png.data(), static_cast<DWORD>(png.size()), &wrote, nullptr) && wrote == png.size();
        CloseHandle(f);
        if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace st::overlay
