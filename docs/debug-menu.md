# In-game debug menu

A Dear ImGui menu drawn inside Death Stranding (Direct3D 12), in the spirit of the
F9 menu of Diamond Dogs Together. It shows what the mod sees (link, Sam, partner)
and runs debug actions (respawn the partner, teleport, tuning) without leaving the game.

## Turning it on

The menu exists only in a modded run: `<game>\stranded_together\session.ini` with a
role other than `none` (`host`, `guest`, `echo`, or `solo` = debug tools only, no
partner and no network). In `session.ini`:

| key | default | meaning |
| --- | --- | --- |
| `debug_menu` | `1` | `0` = no hooks at all, the mod behaves as before |
| `debug_menu_key` | `F9` | `F1`..`F12` or a virtual-key code |

The launcher writes both (Options: "in-game debug menu", key). About 12 s after Sam
is first in the world a small toast in the top-left corner says
`Stranded Together <build> | F9: debug menu`.

Press the key to open or close the menu. While it is open the mouse cursor is shown,
the game gets **no** keyboard, mouse or XInput pad input, and the menu closes by
itself when the game window loses focus. `Esc` or the window's close button also
closes it. Alt+F4 still reaches the game.

## Tabs

* **Status**: build, role, mod state (for example "ds.exe build not supported" when
  co-op could not start), link state / transport / route / RTT / peer / rx / tx bytes,
  Sam (entity, flags, awake, position, speed), a **Game** block (decoded game-state
  flags, post-load phase, GameModule load mask, area, time of day and day/night cycle,
  Sam's health, whether a teleport would be accepted right now and why not, the last
  teleport's verification, and any feature disabled by a failed byte check), the partner
  (entity, state, source, flags, position, age of the newest remote pose, spawn
  held/disabled), tuning, tick rate, overlay FPS, back-buffer size/format and how the
  game's queue was found. **Copy diagnostics** puts all of it on the clipboard and in the log.
* **Partner**: Respawn partner (what F7 does), Remove partner (and hold spawning until
  Respawn), Show / Hide, Log partner details, Voice ping (one echo voice line from the
  partner), live echo delay and sideways offset sliders (echo role), and the outcome of
  recent actions.
* **Player**: Sam's position with Copy (clipboard, `x y z`) and Use as target;
  Teleport to coordinates (three fields, or paste `x y z` / `(x, y, z)` and Enter);
  Snap to ground (x, y) (ignores z; the game drops Sam on the topmost surface); Teleport
  to partner; saved positions (Save current position with a name, Teleport to saved
  position, Set as target, Copy, Delete, Reload) persisted to
  `<game>\stranded_together\positions.txt`; time of day (slider + Apply, Freeze time);
  Unstuck (back to the last safe ground position the game recorded), Heal, Log positions
  (what F6 does).
* **Log**: the last ~300 lines of `stranded_together.log` from an in-memory ring
  (512 lines kept), a filter box, auto-scroll, Copy (the filtered lines) and Clear view.
* **Settings**: Verbose logging toggle, UI scale (Auto = back-buffer height / 1080,
  clamped 1..3), the menu key, input-blocking counters, Log status.

`positions.txt` is plain text, one position per line, coordinates first so names can
contain spaces; `#` starts a comment:

```
# x y z name
1234.500 56.250 -789.125 Capital Knot City gate
```

### Game actions

Teleport, unstuck, heal, time of day, voice ping and the Game readout are implemented in
`native/mod/game_actions.cpp`; [debug-actions.md](debug-actions.md) documents each one:
mechanism, preconditions, addresses, byte checks and what is still untested. Every
teleport (coordinates, saved position, partner, snap) ends in
`st::game::TeleportLocalPlayer(x, y, z, mode, ...)` = `PlayerGame::PlaceOn`, on the game
thread, after all preconditions pass; the result chip says why one was refused.

### Adding an action

One enum value in `st::debug::Action` (`debug_actions.hpp`) and one row in `kRows`
(`debug_actions.cpp`): tab (`status`, `partner`, `player`, `tools`), button label,
tooltip and handler. Rows with `button = true` appear as buttons on their tab
automatically. Handlers run on the game thread and write a short result line that the
menu shows next to the button and under "Recent actions". Game-side work goes in
`game_actions.cpp` (byte-checked, fault-free reads) and gets a stand-in in
`tests/overlay_sync_stubs.cpp`, since the tests and the harness link `debug_actions.cpp`
without game code.

## Design

Code: `native/mod/overlay/` (hooks, renderer, input, menu, keys, saved positions),
`native/mod/debug_actions.*` (action queue, snapshot, handlers),
`third_party/imgui` + `backends/imgui_impl_dx12.*` (v1.92.6, built as `st_mod_imgui`
with `overlay_imconfig.h`: `IM_ASSERT` logs once and never aborts).

### Hooks (bootstrap thread, once)

`dllmain.cpp` calls `st::overlay::Install` only when `session.ini` asks for a role and
`debug_menu=1`, before the game-function resolver, so the Status tab can show why
co-op did not start. Install:

1. `LoadLibrary("d3d12.dll")`, `LoadLibrary("dxgi.dll")` (ds.exe delay-loads both; no
   Agility SDK export in ds.exe, so the system runtime is the one the game gets).
2. Builds a throwaway WARP device + DIRECT queue + 64x64 flip-discard swapchain on a
   hidden window, reads the vtables and destroys everything.
3. MinHooks `IDXGISwapChain::Present` (slot 8), `ResizeBuffers` (13),
   `IDXGISwapChain1::Present1` (22), `IDXGISwapChain3::ResizeBuffers1` (39),
   `ID3D12CommandQueue::ExecuteCommandLists` (10), `IDXGIFactory::CreateSwapChain` (10)
   and `IDXGIFactory2::CreateSwapChainForHwnd` (15) (optional; they record which queue a
   new swapchain presents on), plus `SetCursorPos`, `ClipCursor` and
   `XInputGetState` in both `xinput1_4.dll` (what ds.exe actually calls, via its own GetProcAddress) and `XINPUTUAP.dll` (fallback) for input blocking. All hooks are enabled in
   one `MH_ApplyQueued`. The bootstrap thread runs at DLL load, before ds.exe creates
   its swapchain, so the creation hooks normally see it.

Any failure logs one `[overlay] not installed: ...` line and the game runs as if the
menu did not exist. The hooks patch the runtime functions themselves (not vtable
entries), so they chain with the Steam overlay's hooks in either order.

### Each Present (render thread)

* Closed menu and no toast: the hook only counts the frame, attaches input to the
  swapchain's window once, and calls the original Present. No ImGui, no D3D objects.
* First drawn frame: finds the game's DIRECT queue, in this order:
  `GetDevice(ID3D12CommandQueue)` on the swapchain (does not work on current Windows,
  kept in case it does); the queue recorded when the game created that swapchain
  (CreateSwapChain/CreateSwapChainForHwnd hooks; stored without a reference and only
  used while that swapchain is being presented, which holds the queue alive); otherwise
  the ExecuteCommandLists hook is armed and captures a DIRECT queue of the swapchain's
  device, preferring one submitted from the presenting thread (600 frames without one
  disables the overlay). Our own upload queue is excluded. The Settings tab
  diagnostics and the `renderer ready ... queue from ...` log line say which path won.
* The renderer only exists while the menu or the startup toast is on screen. Once
  closed, it is released as soon as our last draw has completed on the GPU (checked
  without blocking; forced after ~120 frames), together with every reference to the
  game's swapchain and queues. A kept swapchain reference would make the game's next
  `CreateSwapChainForHwnd` on the same window fail (one flip-model swapchain per
  window), and a kept queue would keep a removed device alive.
* Renderer (`overlay_renderer.cpp`): RTV/SRV heaps, one command list, three
  allocator/fence slots; ImGui DX12 backend with its own upload queue. Per frame:
  `GetBuffer(GetCurrentBackBufferIndex())` -> RTV in the buffer's format ->
  PRESENT->RENDER_TARGET -> ImGui -> RENDER_TARGET->PRESENT -> execute on the game queue
  -> signal our fence. The back-buffer reference is released right away. A slot is
  reused only after its fence passed; a slot stuck for 2 s skips the frame. A frame
  that carries ImGui texture uploads (new font glyphs) first waits for our earlier
  draws: the upload runs on our own queue, the atlas is sampled on the game's.
* Formats: R8G8B8A8 / B8G8R8A8 (incl. sRGB and typeless), R10G10B10A2 (HDR10) and
  R16G16B16A16_FLOAT (scRGB) are drawn; the pipeline is rebuilt when the format
  changes. Any other format logs once, draws nothing and closes the menu (an invisible
  menu must not keep the game's input). A back buffer without
  `ALLOW_RENDER_TARGET` is never given an RTV.
* `ResizeBuffers(1)`: waits (bounded, 2 s) for our submissions, then calls the
  original. We hold no back-buffer references, so the game's resize never fails
  because of us. A Present returning DEVICE_REMOVED/RESET releases the renderer; a new
  swapchain rebuilds it.

### Safety

DS's crash reporter fires on any access violation in the process, even a handled one,
so the overlay is written not to fault rather than to catch faults: every COM call's
HRESULT is checked, the menu never reads game memory (see threading), all pointers it
uses are its own or come from DXGI/D3D12 calls. A `__try` around the frame remains as a
last resort only. Any init/draw failure (renderer init, 300 consecutive failed frames,
an exception) calls `Disable`: one `[overlay] DISABLED for this run: <reason>` line,
the menu closes, input is restored and every later Present goes straight to the game.

### Input

How ds.exe reads input (static analysis; imports confirmed in the binary): keyboard via
a `WH_KEYBOARD` thread hook (`SetWindowsHookExA`, set up at 0x1419B8250, proc
0x1419B9920) and raw input; mouse via raw input (`RegisterRawInputDevices` usage 1/2,
`GetRawInputData` in the window procedure 0x141A19BE0; no `GetRawInputBuffer`) plus
`SetCursorPos` / `ClipCursor` / `GetCursorPos`; pads via `XInputGetState` from the
delay-loaded `XINPUTUAP.dll` and HID (`HidD_*`, DualSense). No DirectInput, no
`GetAsyncKeyState`.

While the menu is open:

* Our own `WH_KEYBOARD` hook on the game window's thread (installed after the game's,
  so it runs first) swallows every key and hands it (and its character via
  `ToUnicode`) to ImGui; the game's hook and window never see it. Alt+F4 passes.
* A window-procedure subclass eats `WM_INPUT` (passed to `DefWindowProc` so the system
  frees the handle) and all mouse/keyboard messages, turning mouse messages into ImGui
  events; `WM_SETCURSOR` hides the system cursor (ImGui draws its own).
* `SetCursorPos` and `ClipCursor` from the game are ignored (the game's last clip
  request is applied when the menu closes); `XInputGetState` reports a connected but
  idle pad.
* Focus loss (`WM_KILLFOCUS`, `WM_ACTIVATEAPP` false) closes the menu.
* Exception: keys and mouse buttons that were physically down when the menu opened
  (`GetAsyncKeyState` at open) get their RELEASE through to the game: in our keyboard
  hook, as `WM_KEYUP`/button-up messages, and as `WM_INPUT` packets that only carry
  that release (`GetRawInputData` peeks the packet; the game still reads it). Otherwise
  a key held while pressing F9 (W while walking, a held mouse button) would stay down
  in the game's own input state after the menu closes. A raw mouse packet with a
  button release can carry a little movement, which then reaches the game too.
  ds.exe registers keyboard raw input with `RIDEV_NOHOTKEYS|RIDEV_DEVNOTIFY` and the
  mouse with `RIDEV_DEVNOTIFY` (no `RIDEV_NOLEGACY`), so the legacy messages our
  keyboard hook and subclass rely on are generated.

The toggle key itself is always swallowed (keyboard hook, or window messages when the
hook is missing, or `GetAsyncKeyState` polling as a last fallback). With the menu
closed everything else passes through untouched. The mod's own F6/F7 hotkeys stand
down while the menu is open.

### Threading

```
render thread (Present)  --st::debug::Queue(Request)-->  locked queue  --Drain-->  game thread (st::sync::Tick)
game thread (Tick)       --st::debug::Publish(Snapshot)-->  locked copy  --GetSnapshot-->  menu
```

ImGui runs only inside Present under one lock. Every game action is a request
executed by `st::debug::OnTick` on the tick (the game's main thread, once per frame);
the menu shows a snapshot copy the tick publishes (every tick while the menu is open,
2 Hz otherwise). The Log tab reads `st::LogCopyRecent` (lock-protected ring in
`log.cpp`). Saved positions are file I/O on the render thread, written atomically
(temp file + `MoveFileEx`).

## Testing without the game

* `st_overlay_tests` (ctest): action queue/registry, every handler against the stubs,
  snapshot publication (including the game diagnostics block), the game-state / load-mask /
  area decoders, the copy-diagnostics report, saved-position parsing/formatting,
  coordinate parsing, key names, RTV format mapping.
* `st_overlay_harness` (ctest; needs a D3D12-capable adapter or WARP and a desktop
  session): its own window, D3D12 device, DIRECT queue and flip-model swapchain, a
  stand-in for the game's `WH_KEYBOARD` hook, and the same `overlay::Install` path as
  the mod. It checks that a closed menu draws nothing and passes input, the toggle key,
  that each tab draws (read back from the back buffer), that no key/mouse/raw input
  reaches the "game" while open (except the release of a key held before opening),
  that the queue comes from the swapchain-creation hook (and from the
  ExecuteCommandLists fallback after a re-install), that actions run on the tick
  thread, ResizeBuffers to 1600x900 and to a 10-bit format, that the renderer is
  released after closing, close/uninstall/re-install and the startup toast.
  Screenshots go to the output directory:

  ```
  cmake --build build-overlay --config Release --target st_overlay_harness
  build-overlay\Release\st_overlay_harness.exe --out build-overlay
  ```

  writes `overlay_menu_{status,partner,player,log,settings,resized,10bit}.png` and
  `overlay_hint.png`, and its log to `build-overlay\overlay_harness\stranded_together.log`.

## Known limits

* Not yet run inside ds.exe. The first in-game run should check the log for
  `[overlay] installed`, `renderer ready ... queue from ...` and `first frame drawn`.
* DualSense pads read through HID (not XInput) still reach the game while the menu is
  open; so does anything Steam Input injects outside XInput.
* The menu is drawn on the DIRECT queue the swapchain was created on (recorded by the
  creation hook). Only when the swapchain predates the hooks does the
  ExecuteCommandLists capture pick the queue, and that guess (the queue the presenting
  thread submits to) has not been checked against the real game.
* While the menu is open the renderer holds the game's swapchain; if the game
  recreated its swapchain during that time (it gets no input then, so only on its own
  initiative) the new one could fail to be created. Closing the menu releases it.
* HDR: in an HDR10 (PQ) back buffer the menu's colours are written as plain UNORM values,
  so white text is very bright; in scRGB it is dim. Readable, not colour-managed.
* The game actions have not run inside ds.exe yet (checklist in debug-actions.md).
