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
};

// Loads <mod_dir>\positions.txt.
void Init(const std::wstring& mod_dir);
void Draw(Context& ctx);
// Selects a tab on the next Draw (0 Status, 1 Partner, 2 Player, 3 Log, 4 Settings).
// Used by the harness for screenshots.
void SelectTab(int index);

}  // namespace st::overlay::menu
