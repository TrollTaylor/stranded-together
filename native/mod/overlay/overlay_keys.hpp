// Virtual-key helpers for the overlay (pure: no OS calls).
#pragma once
#include <cstddef>
#include <cstdint>

#include "imgui.h"

namespace st::overlay {

// Win32 virtual-key -> ImGuiKey (ImGuiKey_None when ImGui has no such key).
// `extended` is lParam bit 24 (tells numpad Enter from Enter).
ImGuiKey VkToImGuiKey(std::uint32_t vk, bool extended) noexcept;
// Short display name for the toggle key: "F9", "Insert", "VK 0xC0".
const char* VkName(std::uint32_t vk, char* buf, std::size_t cap) noexcept;

}  // namespace st::overlay
