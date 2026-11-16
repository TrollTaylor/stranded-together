// Dear ImGui user config for the mod's in-game overlay build (st_mod_imgui):
// pulled in by imgui.h through IMGUI_USER_CONFIG. Not used by the launcher.
#pragma once

// A failed ImGui assertion must never abort the game: it is logged once per
// site by StOverlayImGuiAssert (overlay.cpp) and execution continues.
void StOverlayImGuiAssert(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : StOverlayImGuiAssert(#_EXPR, __FILE__, __LINE__))

// No imm32 / shell32 dependencies inside the game process.
#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
