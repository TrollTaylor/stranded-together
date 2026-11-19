// In-game debug menu overlay for Death Stranding (Direct3D 12).
//
// Install (bootstrap thread, once): loads d3d12/dxgi like the game would,
// builds a throwaway WARP device + DIRECT queue + flip-model swapchain on a
// hidden window to read the vtables, and MinHooks:
//   IDXGISwapChain::Present / IDXGISwapChain1::Present1   draw the menu
//   IDXGISwapChain::ResizeBuffers / IDXGISwapChain3::ResizeBuffers1  wait for our GPU work
//   IDXGIFactory::CreateSwapChain / IDXGIFactory2::CreateSwapChainForHwnd
//                                                          which queue the game's swapchain presents on
//   ID3D12CommandQueue::ExecuteCommandLists               fallback: capture the game's DIRECT queue
//   SetCursorPos, ClipCursor, XInputGetState               input blocking (overlay_input)
// Everything after that happens inside the game's Present, under one lock,
// and every failure disables the overlay for the run with one log line.
// The D3D12 renderer (and every reference to the game's swapchain/queue) exists
// only while the menu, the startup hint or a name tag (overlay_tags.hpp) is on screen, and
// is dropped before the game creates a new swapchain.
// See docs/debug-menu.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct IDXGISwapChain3;
struct ID3D12CommandQueue;

namespace st::overlay {

struct Options {
    std::uint32_t toggle_vk = 0x78;  // VK_F9
    std::wstring mod_dir;            // <game>\stranded_together (positions.txt lives here)
    std::string build;               // shown in the menu title
    bool startup_hint = true;        // "F9: debug menu" toast once Sam is in the world
    // false: the overlay is installed only for the name tags (session.ini debug_menu=0,
    // name_tags=1): no menu, no toggle key, no hint, the game window is not subclassed.
    bool menu = true;
    // Name tags in the game's own menu font (overlay_font.hpp; session.ini name_tag_font).
    bool game_font = true;
    // Hidden test runs (session.ini automation=1, docs/hidden-testing.md): the
    // virtual pad answers XInputGetState, GetForegroundWindow answers the game
    // window, SetCursorPos/ClipCursor never reach the system, the menu ignores
    // focus loss and the game cannot go exclusive fullscreen.
    bool automation = false;
};

// Bootstrap thread. False = no overlay this run (reason logged).
bool Install(const Options& options) noexcept;
// Unhooks and releases everything. Used by the harness; the game just exits.
void Shutdown() noexcept;

bool Installed() noexcept;
bool Disabled() noexcept;  // installed but turned itself off after a failure
bool IsOpen() noexcept;
void SetOpen(bool open) noexcept;  // any thread; applied on the next Present

struct Stats {
    bool installed = false;
    bool disabled = false;
    char disabled_reason[160] = {};
    char queue_source[48] = "none";  // how the game's DIRECT queue was found
    unsigned long presents = 0;
    unsigned long frames_drawn = 0;
    unsigned long resizes = 0;
    unsigned long renderer_inits = 0;
    unsigned long renderer_releases = 0;  // renderer dropped (menu closed, new swapchain, device lost)
    unsigned width = 0, height = 0;
    int format = 0;                  // DXGI_FORMAT of the back buffer
    float fps = 0;
    int last_vertices = 0;           // ImDrawData::TotalVtxCount of the last drawn frame
    int last_draw_lists = 0;
    unsigned long occluded_masked = 0;  // automation: DXGI_STATUS_OCCLUDED answered as S_OK
};
Stats GetStats() noexcept;

// ---- screenshots (automation, harness)
// A tightly packed 8-bit RGB image, top row first.
struct Image {
    unsigned width = 0, height = 0;
    std::vector<std::uint8_t> rgb;
};
// Any thread. The capture runs inside the game's next Present, after the menu
// or hint was drawn (if any), menu open or closed: a copy of exactly the frame
// being presented. Returns a request id (never 0).
std::uint32_t RequestCapture() noexcept;
// 1 = done (image moved into *out), 0 = still pending, -1 = failed or unknown id (why filled).
int TakeCapture(std::uint32_t id, Image* out, char* why, std::size_t why_cap) noexcept;
// Gives up a request (a caller that timed out): its slot is free again and a
// pending capture is not taken. Unknown ids are ignored.
void CancelCapture(std::uint32_t id) noexcept;

// Test seam (harness): called inside Present after the menu's command list was
// submitted on the game queue and before the real Present.
using AfterDrawFn = void (*)(IDXGISwapChain3* chain, ID3D12CommandQueue* queue, void* user);
void SetAfterDrawForTest(AfterDrawFn fn, void* user) noexcept;

// ---- Co-op session screens (the host-held save session, coop_session.cpp; docs/coop-session.md). Any thread.
// The co-op module publishes what to show; the overlay draws it (and keeps its renderer while anything is shown).
struct CoopView {
    bool active = false;               // a host-held save session (the menu header shows `status`)
    // The guest's full-screen waiting panel; the game's input is blocked as for the open menu (input::SetWaitBlock).
    bool waiting = false;
    // The game's input blocked as for the open menu, except the confirm inputs (input::SetWaitBlock): with the waiting
    // panel (and while a hidden test hides the panel for a screenshot of the title behind it).
    bool block_input = false;
    char title[160] = {};              // "Waiting for <host> to start the game"
    char lines[4][200] = {};           // link, host phase, notes ("" = none)
    float progress = -1.0f;            // the transfer bar, 0..1 (< 0: hidden)
    char progress_text[120] = {};
    bool leave = false;                // the Leave button (quits the game)
    char banner[240] = {};             // a notice at the top centre ("" = none): in the world or over the title
    bool banner_warn = false;          // amber instead of white
    bool teleport_offer = false;       // the menu header offers "Teleport to host"
    char status[200] = {};             // one line in the menu header
};
void SetCoopView(const CoopView& v) noexcept;
// What the player pressed on the co-op screens since the last call: kCoopLeave, kCoopTeleport.
inline constexpr unsigned kCoopLeave = 1u, kCoopTeleport = 2u;
unsigned TakeCoopRequests() noexcept;

}  // namespace st::overlay
