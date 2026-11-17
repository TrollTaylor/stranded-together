#include "overlay_font.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game_font.hpp"
#include "game_font_raster.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "log.hpp"

namespace st::overlay::font {
namespace {

using gamefont::Font;
using gamefont::Glyph;

constexpr float kBaseSize = 17.0f;   // as the overlay's default font
constexpr float kSegoeCap = 0.5265f;  // Segoe UI: cap height 1434 / (ascent 2210 + descent 514), per pixel of size

std::atomic<bool> g_use{false};
std::atomic<int> g_state{0};  // 0 idle, 1 reading, 2 ready, -1 failed (default font)
const Font* g_font = nullptr;  // written once by the reader thread before g_state = 2; never freed
DWORD g_read_ms = 0;
char g_why[200] = {};

// Render thread only.
bool g_added = false;
ImFont* g_imfont = nullptr;
float g_size_scale = 1.0f;

DWORD WINAPI ReadThread(void*) {
    const ULONGLONG t0 = GetTickCount64();
    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])));
    std::wstring dir(exe, n);
    const std::size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash);
    Font* f = nullptr;
    try {
        f = new Font();
    } catch (...) {
    }
    char why[200] = {};
    const bool ok = f && !dir.empty() && gamefont::LoadMenuFont(dir, true, f, why, sizeof(why));
    g_read_ms = static_cast<DWORD>(GetTickCount64() - t0);
    if (!ok) {
        std::snprintf(g_why, sizeof(g_why), "%s", f ? why : "out of memory");
        delete f;
        g_state.store(-1, std::memory_order_release);
        return 0;
    }
    g_font = f;
    g_state.store(2, std::memory_order_release);
    return 0;
}

const Font* SrcFont(const ImFontConfig* src) { return static_cast<const Font*>(src->FontLoaderData); }

bool SrcInit(ImFontAtlas*, ImFontConfig* src) {
    src->FontLoaderData = const_cast<Font*>(g_font);
    return g_font != nullptr;
}
void SrcDestroy(ImFontAtlas*, ImFontConfig* src) { src->FontLoaderData = nullptr; }

bool SrcContains(ImFontAtlas*, ImFontConfig* src, ImWchar cp) {
    const Font* f = SrcFont(src);
    return f && f->Find(cp) != nullptr;
}

bool BakedInit(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void*) {
    const Font* f = SrcFont(src);
    if (!f) return false;
    if (!src->MergeMode) {
        const float s = baked->Size / f->height;
        baked->Ascent = ImCeil(f->ascent * s);
        baked->Descent = ImFloor(-f->descent * s);
    }
    return true;
}

bool BakedLoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, ImWchar cp, ImFontGlyph* out_glyph,
                    float* out_advance_x) {
    const Font* f = SrcFont(src);
    const Glyph* g = f ? f->Find(cp) : nullptr;
    if (!g) return false;
    const float layout = baked->Size / f->height;
    if (out_advance_x) {
        *out_advance_x = g->advance * layout;
        return true;
    }
    int os_h = 1, os_v = 1;
    ImFontAtlasBuildGetOversampleFactors(src, baked, &os_h, &os_v);
    const float density = src->RasterizerDensity * baked->RasterizerDensity;
    const float sx = layout * density * static_cast<float>(os_h), sy = layout * density * static_cast<float>(os_v);
    out_glyph->Codepoint = cp;
    out_glyph->AdvanceX = g->advance * layout;
    const gamefont::PixelBox b = gamefont::GlyphBox(*g, sx, sy, 0, 0);
    if (b.x0 == b.x1 || b.y0 == b.y1) return true;  // a space: advance only
    const int w = b.x1 - b.x0 + os_h - 1, h = b.y1 - b.y0 + os_v - 1;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return false;
    const ImFontAtlasRectId pack = ImFontAtlasPackAddRect(atlas, w, h);
    if (pack == ImFontAtlasRectId_Invalid) return false;
    ImTextureRect* r = ImFontAtlasPackGetRect(atlas, pack);
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    float sub_x = 0, sub_y = 0;
    if (!gamefont::RasterGlyph(*f, *g, sx, sy, 0, 0, os_h, os_v, px.data(), w, h, w, &sub_x, &sub_y)) return false;
    const float off_x = sub_x, off_y = sub_y + IM_ROUND(baked->Ascent);
    const float rh = 1.0f / (static_cast<float>(os_h) * density), rv = 1.0f / (static_cast<float>(os_v) * density);
    out_glyph->X0 = static_cast<float>(b.x0) * rh + off_x;
    out_glyph->Y0 = static_cast<float>(b.y0) * rv + off_y;
    out_glyph->X1 = static_cast<float>(b.x0 + static_cast<int>(r->w)) * rh + off_x;
    out_glyph->Y1 = static_cast<float>(b.y0 + static_cast<int>(r->h)) * rv + off_y;
    out_glyph->Visible = true;
    out_glyph->PackId = pack;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out_glyph, r, px.data(), ImTextureFormat_Alpha8, w);
    return true;
}

const ImFontLoader* Loader() {
    static ImFontLoader l;
    l.Name = "ds_font_resource";
    l.FontSrcInit = SrcInit;
    l.FontSrcDestroy = SrcDestroy;
    l.FontSrcContainsGlyph = SrcContains;
    l.FontBakedInit = BakedInit;
    l.FontBakedLoadGlyph = BakedLoadGlyph;
    return &l;
}

void AddToAtlas() {
    g_added = true;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (!atlas || atlas->Locked) {
        g_added = false;  // try again next frame
        return;
    }
    ImFontConfig cfg;
    cfg.FontLoader = Loader();
    cfg.FontDataOwnedByAtlas = false;
    cfg.SizePixels = kBaseSize;
    std::snprintf(cfg.Name, sizeof(cfg.Name), "game menu font");
    ImFont* f = atlas->AddFont(&cfg);
    if (!f) {
        Log("[tags] the game font could not be added to the overlay's atlas: the default font is used");
        return;
    }
    // Glyphs the game font lacks (the " · " separator, CJK, Arabic, ...) come from Segoe UI.
    wchar_t windir[MAX_PATH] = {};
    char path[MAX_PATH * 2] = {};
    bool merged = false;
    if (GetWindowsDirectoryW(windir, MAX_PATH)) {
        std::snprintf(path, sizeof(path), "%ls\\Fonts\\segoeui.ttf", windir);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            ImFontConfig m;
            m.MergeMode = true;
            m.DstFont = f;
            merged = atlas->AddFontFromFileTTF(path, kBaseSize, &m) != nullptr;
        }
    }
    // Same cap height as the default font, plus 10% (the game font's lower case is small capitals).
    const Glyph* cap = g_font->Find('H');
    const float cap_h = cap && g_font->height > 0 ? cap->max_y / g_font->height : 0.0f;
    g_size_scale = cap_h > 0.2f ? 1.10f * kSegoeCap / cap_h : 1.0f;
    if (g_size_scale < 0.8f) g_size_scale = 0.8f;
    if (g_size_scale > 1.5f) g_size_scale = 1.5f;
    g_imfont = f;
    Log("[tags] name tags use the game's menu font '%s' (%zu glyphs, %zu contours, read from the installed game in %lu ms, "
        "memory only; cap height %.3f, size x%.2f)%s",
        g_font->typeface.c_str(), g_font->glyphs.size(), g_font->contours.size(), static_cast<unsigned long>(g_read_ms),
        static_cast<double>(cap_h), static_cast<double>(g_size_scale), merged ? "; missing glyphs from Segoe UI" : "");
}

}  // namespace

void Configure(bool use_game_font) noexcept { g_use.store(use_game_font); }

void BetweenFrames() noexcept {
    if (!g_use.load()) return;
    int s = g_state.load(std::memory_order_acquire);
    if (s == 0) {
        g_state.store(1);
        const HANDLE t = CreateThread(nullptr, 0, ReadThread, nullptr, 0, nullptr);
        if (!t) {
            g_state.store(-1);
            Log("[tags] the game font reader thread could not start: the default font is used");
            return;
        }
        SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(t);
        return;
    }
    if (s == -1) {
        static bool logged = false;
        if (!logged) Log("[tags] the game's menu font is unavailable (%s): name tags use the default font", g_why);
        logged = true;
        return;
    }
    if (s == 2 && !g_added) AddToAtlas();
}

ImFont* TagFont() noexcept { return g_imfont; }
float TagSizeScale() noexcept { return g_imfont ? g_size_scale : 1.0f; }

}  // namespace st::overlay::font
