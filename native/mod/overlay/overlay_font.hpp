// The name tags' font: the game's own menu typeface (game_font.hpp), added to the overlay's ImGui
// atlas through a custom ImFontLoader (outlines rasterized on demand, in memory only), with Segoe UI
// merged in for glyphs the game font lacks (U+00B7, CJK, ...). Falls back to the overlay's default
// font when the game font cannot be read. docs/real-partner-body.md section 10.6.
#pragma once

struct ImFont;

namespace st::overlay::font {

// Bootstrap thread, before the first frame: whether to try the game font (session.ini name_tag_font).
void Configure(bool use_game_font) noexcept;
// Render thread, with the ImGui context current and OUTSIDE NewFrame..Render (the atlas is unlocked):
// starts the background read once, and adds the font to the atlas when it is ready.
void BetweenFrames() noexcept;
// Render thread: the tag font, or nullptr (use the default font).
ImFont* TagFont() noexcept;
// The tag font's size relative to the default font at the same pixel size (its capitals are smaller).
float TagSizeScale() noexcept;

}  // namespace st::overlay::font
