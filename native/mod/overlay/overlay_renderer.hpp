// Direct3D 12 side of the overlay: draws an ImDrawData into the swapchain's
// current back buffer on the game's DIRECT queue, right before Present.
//
// Per draw: GetBuffer(current index) -> RTV in the buffer's own format ->
// PRESENT->RENDER_TARGET barrier -> ImGui -> RENDER_TARGET->PRESENT -> Execute
// on the game's queue -> Signal our fence. Nothing holds a back-buffer
// reference between frames, so the game's ResizeBuffers never waits on us.
// Three ring slots (allocator + fence value) are reused only after the GPU
// finished them; a slot that does not finish in 2 s skips the frame.
//
// Present thread only (the overlay's render lock is held around every call).
#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "overlay.hpp"

struct ImDrawData;
struct ImGui_ImplDX12_InitInfo;

namespace st::overlay {

// The RTV/PSO format for a back-buffer format, or DXGI_FORMAT_UNKNOWN when the
// overlay does not draw into it. Typeless formats map to their UNORM/FLOAT view.
DXGI_FORMAT RtvFormatFor(DXGI_FORMAT buffer_format) noexcept;

// ---- screenshots
// Copies the swapchain's current back buffer (the frame about to be presented)
// on `queue` (the game's DIRECT queue, so it is ordered after the frame's work),
// waits for it (bounded) and converts it to RGB8. Present thread. Creates and
// releases its own allocator/list/fence/readback buffer; keeps no reference.
bool CaptureBackBuffer(IDXGISwapChain3* chain, ID3D12CommandQueue* queue, Image* out, char* why, std::size_t why_cap,
                       DWORD timeout_ms) noexcept;
// One row of `format` texels -> RGB8. False for a format we cannot convert.
// 8-bit RGBA/BGRA (UNORM/SRGB/TYPELESS) as stored, R10G10B10A2 truncated to 8
// bits, R16G16B16A16_FLOAT (scRGB, linear) clamped and sRGB-encoded.
bool ConvertRowToRgb(DXGI_FORMAT format, const std::uint8_t* row, unsigned width, std::uint8_t* rgb) noexcept;
// 8-bit RGB PNG (zlib stored blocks: no compression, no dependencies).
bool EncodePng(const Image& image, std::string* png) noexcept;
// EncodePng + write to `path` atomically (temp file + rename).
bool WritePngFile(const std::wstring& path, const Image& image) noexcept;

class Renderer {
public:
    // Creates every object; false (with a reason) leaves nothing allocated.
    bool Init(IDXGISwapChain3* chain, ID3D12CommandQueue* game_queue, char* why, std::size_t why_cap) noexcept;
    bool Ready() const noexcept { return device_ != nullptr; }
    IDXGISwapChain* Chain() const noexcept { return chain_; }
    ID3D12CommandQueue* UploadQueue() const noexcept { return upload_queue_; }

    // Size/format of the current back buffer (no reference kept).
    bool BackBufferInfo(UINT* width, UINT* height, DXGI_FORMAT* format) noexcept;
    // Makes the ImGui backend match `format` (re-creates its pipeline on a change).
    bool EnsureFormat(DXGI_FORMAT buffer_format) noexcept;
    // Records and submits the draw. False = this frame was skipped.
    bool Draw(ImDrawData* data) noexcept;

    // Waits (bounded) for every submission of ours. Before ResizeBuffers / teardown.
    bool WaitIdle(DWORD timeout_ms) noexcept;
    // Non-blocking: every submission of ours has completed on the GPU.
    bool Idle() const noexcept;
    // Releases everything (waits first). Safe to call when not ready.
    void Shutdown() noexcept;

    // Test seam: called after our command list was submitted, before Present.
    using AfterDrawFn = void (*)(IDXGISwapChain3* chain, ID3D12CommandQueue* queue, void* user);
    void SetAfterDraw(AfterDrawFn fn, void* user) noexcept { after_draw_ = fn, after_draw_user_ = user; }

    DXGI_FORMAT Format() const noexcept { return rtv_format_; }
    std::uint64_t DrawCount() const noexcept { return draws_; }

private:
    static constexpr UINT kSlots = 3;
    static constexpr UINT kSrvDescriptors = 64;
    struct Slot {
        ID3D12CommandAllocator* allocator = nullptr;
        UINT64 fence_value = 0;
    };

    bool InitBackend(DXGI_FORMAT rtv_format) noexcept;
    void ShutdownBackend() noexcept;
    bool WaitFor(UINT64 value, DWORD timeout_ms) noexcept;

    static void SrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
    static void SrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

    IDXGISwapChain3* chain_ = nullptr;          // AddRef'd
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* game_queue_ = nullptr;  // AddRef'd; where the draw goes
    ID3D12CommandQueue* upload_queue_ = nullptr;  // ours: ImGui texture uploads
    ID3D12DescriptorHeap* rtv_heap_ = nullptr;
    ID3D12DescriptorHeap* srv_heap_ = nullptr;
    UINT rtv_step_ = 0, srv_step_ = 0;
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE fence_event_ = nullptr;
    UINT64 fence_last_ = 0;
    Slot slots_[kSlots];
    std::uint64_t draws_ = 0;
    bool backend_ = false;
    bool render_target_ok_ = false;  // last BackBufferInfo: ALLOW_RENDER_TARGET, 1 sample
    DXGI_FORMAT rtv_format_ = DXGI_FORMAT_UNKNOWN;
    bool srv_used_[kSrvDescriptors] = {};
    AfterDrawFn after_draw_ = nullptr;
    void* after_draw_user_ = nullptr;
};

}  // namespace st::overlay
