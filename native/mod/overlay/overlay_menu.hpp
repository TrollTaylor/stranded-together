// The debug menu's ImGui layout (render thread only). Reads the tick's
// published snapshot and queues debug actions; never touches game memory.
#pragma once
#include <cstdint>
#include <string>

#include "debug_actions.hpp"
#include "overlay.hpp"
#include "overlay_input.hpp"

namespace st::overlay::menu {

struct Context {
    const debug::Snapshot* snap = nullptr;
    const Stats* stats = nullptr;
    input::Stats input;
    std::uint32_t toggle_vk = 0x78;
    const char* build = "";
    float* ui_scale = nullptr;  // the overlay applies changes next frame
    bool close = false;         // set by the menu (Close button / Esc)
    const char* close_reason = "";  // with close: "by Esc" / "by the menu's close button" (the log line)
    const CoopView* coop = nullptr;  // a host-held save session: its status line (and the teleport offer)
    bool coop_teleport = false;      // set by the menu: "Teleport to host" pressed
};

// Loads <mod_dir>\positions.txt.
void Init(const std::wstring& mod_dir);
void Draw(Context& ctx);
// Selects a tab on the next Draw (0 Status, 1 Partner, 2 Player, 3 Log, 4 Settings, 5 Dev tools).
// Used by the harness for screenshots (and by the dev tools' hidden-run screenshot op, dev_tools.cpp).
void SelectTab(int index);

}  // namespace st::overlay::menu
