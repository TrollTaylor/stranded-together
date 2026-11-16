#include "overlay_keys.hpp"

#include <cstdio>

namespace st::overlay {

ImGuiKey VkToImGuiKey(std::uint32_t vk, bool extended) noexcept {
    if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + static_cast<int>(vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + static_cast<int>(vk - 'A'));
    if (vk >= 0x70 && vk <= 0x87) return static_cast<ImGuiKey>(ImGuiKey_F1 + static_cast<int>(vk - 0x70));  // F1..F24
    if (vk >= 0x60 && vk <= 0x69) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + static_cast<int>(vk - 0x60));
    switch (vk) {
        case 0x09: return ImGuiKey_Tab;
        case 0x25: return ImGuiKey_LeftArrow;
        case 0x27: return ImGuiKey_RightArrow;
        case 0x26: return ImGuiKey_UpArrow;
        case 0x28: return ImGuiKey_DownArrow;
        case 0x21: return ImGuiKey_PageUp;
        case 0x22: return ImGuiKey_PageDown;
        case 0x24: return ImGuiKey_Home;
        case 0x23: return ImGuiKey_End;
        case 0x2D: return ImGuiKey_Insert;
        case 0x2E: return ImGuiKey_Delete;
        case 0x08: return ImGuiKey_Backspace;
        case 0x20: return ImGuiKey_Space;
        case 0x0D: return extended ? ImGuiKey_KeypadEnter : ImGuiKey_Enter;
        case 0x1B: return ImGuiKey_Escape;
        case 0xDE: return ImGuiKey_Apostrophe;
        case 0xBC: return ImGuiKey_Comma;
        case 0xBD: return ImGuiKey_Minus;
        case 0xBE: return ImGuiKey_Period;
        case 0xBF: return ImGuiKey_Slash;
        case 0xBA: return ImGuiKey_Semicolon;
        case 0xBB: return ImGuiKey_Equal;
        case 0xDB: return ImGuiKey_LeftBracket;
        case 0xDC: return ImGuiKey_Backslash;
        case 0xDD: return ImGuiKey_RightBracket;
        case 0xC0: return ImGuiKey_GraveAccent;
        case 0x14: return ImGuiKey_CapsLock;
        case 0x91: return ImGuiKey_ScrollLock;
        case 0x90: return ImGuiKey_NumLock;
        case 0x2C: return ImGuiKey_PrintScreen;
        case 0x13: return ImGuiKey_Pause;
        case 0x6E: return ImGuiKey_KeypadDecimal;
        case 0x6F: return ImGuiKey_KeypadDivide;
        case 0x6A: return ImGuiKey_KeypadMultiply;
        case 0x6D: return ImGuiKey_KeypadSubtract;
        case 0x6B: return ImGuiKey_KeypadAdd;
        case 0x10: case 0xA0: return ImGuiKey_LeftShift;  // VK_SHIFT arrives without side from WH_KEYBOARD
        case 0xA1: return ImGuiKey_RightShift;
        case 0x11: return extended ? ImGuiKey_RightCtrl : ImGuiKey_LeftCtrl;
        case 0xA2: return ImGuiKey_LeftCtrl;
        case 0xA3: return ImGuiKey_RightCtrl;
        case 0x12: return extended ? ImGuiKey_RightAlt : ImGuiKey_LeftAlt;
        case 0xA4: return ImGuiKey_LeftAlt;
        case 0xA5: return ImGuiKey_RightAlt;
        case 0x5B: return ImGuiKey_LeftSuper;
        case 0x5C: return ImGuiKey_RightSuper;
        case 0x5D: return ImGuiKey_Menu;
        default: return ImGuiKey_None;
    }
}

const char* VkName(std::uint32_t vk, char* buf, std::size_t cap) noexcept {
    if (!buf || !cap) return "";
    if (vk >= 0x70 && vk <= 0x87) {
        std::snprintf(buf, cap, "F%u", vk - 0x70 + 1);
        return buf;
    }
    const char* name = nullptr;
    switch (vk) {
        case 0x2D: name = "Insert"; break;
        case 0x2E: name = "Delete"; break;
        case 0x24: name = "Home"; break;
        case 0x23: name = "End"; break;
        case 0x21: name = "Page Up"; break;
        case 0x22: name = "Page Down"; break;
        case 0x13: name = "Pause"; break;
        case 0x91: name = "Scroll Lock"; break;
        case 0xC0: name = "` (grave)"; break;
        default: break;
    }
    if (name) std::snprintf(buf, cap, "%s", name);
    else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) std::snprintf(buf, cap, "%c", static_cast<char>(vk));
    else std::snprintf(buf, cap, "VK 0x%02X", vk);
    return buf;
}

}  // namespace st::overlay
