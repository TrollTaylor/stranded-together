#include "overlay_tags.hpp"

#include <windows.h>

#include <atomic>
#include <cfloat>
#include <cstdio>

#include "imgui.h"
#include "log.hpp"

namespace st::overlay::tags {
namespace {

constexpr ULONGLONG kFreshMs = 500;  // a frame the game thread stopped refreshing is not drawn

SRWLOCK g_lock = SRWLOCK_INIT;
Frame g_frame;
ULONGLONG g_frame_tick = 0;
std::atomic<ViewReader> g_reader{nullptr};

SRWLOCK g_stats_lock = SRWLOCK_INIT;
Stats g_stats;

// Render thread only.
ULONGLONG g_next_log_tick = 0;
unsigned g_logged_draws = 0;
ULONGLONG g_next_fail_log_tick = 0;
bool g_was_drawn = false;

bool Fresh(ULONGLONG published) { return published && GetTickCount64() - published <= kFreshMs; }

}  // namespace

void SetViewReader(ViewReader reader) noexcept { g_reader.store(reader); }

void Publish(const Frame& f) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_frame = f;
    g_frame_tick = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
    AcquireSRWLockExclusive(&g_stats_lock);
    ++g_stats.published;
    ReleaseSRWLockExclusive(&g_stats_lock);
}

bool Wanted() noexcept {
    if (!g_reader.load()) return false;
    AcquireSRWLockShared(&g_lock);
    const bool want = g_frame.show && g_frame.count > 0 && g_frame.view && Fresh(g_frame_tick);
    ReleaseSRWLockShared(&g_lock);
    return want;
}

void Draw(ImDrawList* dl, float w, float h, float ui_scale) noexcept {
    const ViewReader reader = g_reader.load();
    if (!dl || !reader || !(w > 0) || !(h > 0)) return;
    Frame f;
    ULONGLONG tick = 0;
    AcquireSRWLockShared(&g_lock);
    f = g_frame;
    tick = g_frame_tick;
    ReleaseSRWLockShared(&g_lock);
    if (!f.show || !f.count || !f.view || !Fresh(tick)) return;

    nametag::View v;
    if (!reader(f.view, &v)) {
        AcquireSRWLockExclusive(&g_stats_lock);
        const unsigned long fails = ++g_stats.view_failures;
        ReleaseSRWLockExclusive(&g_stats_lock);
        const ULONGLONG now = GetTickCount64();
        if (now >= g_next_fail_log_tick) {
            g_next_fail_log_tick = now + 10000;
            Log("[tags] camera view unreadable this frame (%lu so far): tags not drawn", fails);
        }
        return;
    }

    ImFont* font = ImGui::GetFont();
    const float scale = ui_scale > 0 ? ui_scale : 1.0f;
    bool any = false;
    Stats last{};
    unsigned long hidden = 0;
    for (unsigned i = 0; i < f.count && i < kMaxTags; ++i) {
        const Tag& t = f.tags[i];
        if (!t.text[0]) continue;
        const nametag::Projected p = nametag::Project(v, t.anchor, w, h);
        const nametag::Style s = nametag::StyleFor(p.dist, scale);
        if (!p.visible || !s.visible) {
            ++hidden;
            continue;
        }
        char text[128];
        if (s.show_distance)
            std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %.0f m", t.text, static_cast<double>(p.dist));
        else
            std::snprintf(text, sizeof(text), "%s", t.text);
        const ImVec2 ts = font->CalcTextSizeA(s.size_px, FLT_MAX, 0.0f, text);
        const float pad_x = 0.45f * s.size_px, pad_y = 0.16f * s.size_px;
        const float bw = ts.x + 2 * pad_x, bh = ts.y + 2 * pad_y;
        const float tri = 0.28f * s.size_px;
        const float bottom = p.y - tri;  // the pointer's tip sits on the anchor
        // The box stays inside the back buffer (a partner at the edge of the view); the pointer stays on the anchor.
        float left = p.x - bw * 0.5f;
        if (left + bw > w - 2.0f) left = w - 2.0f - bw;
        if (left < 2.0f) left = 2.0f;
        const ImVec2 bmin(left, bottom - bh), bmax(left + bw, bottom);
        const float a = s.alpha;
        const ImU32 bg = IM_COL32(12, 16, 22, static_cast<int>(170 * a));
        dl->AddRectFilled(bmin, bmax, bg, bh * 0.5f);
        dl->AddRect(bmin, bmax, IM_COL32(255, 255, 255, static_cast<int>(48 * a)), bh * 0.5f, 0, 1.0f * scale);
        const float tx = p.x < bmin.x + tri + bh * 0.5f ? bmin.x + tri + bh * 0.5f : (p.x > bmax.x - tri - bh * 0.5f ? bmax.x - tri - bh * 0.5f : p.x);
        dl->AddTriangleFilled(ImVec2(tx - tri, bottom), ImVec2(tx + tri, bottom), ImVec2(p.x, p.y), bg);
        const ImVec2 at(bmin.x + pad_x, bmin.y + pad_y);
        dl->AddText(font, s.size_px, ImVec2(at.x + 1.0f * scale, at.y + 1.0f * scale), IM_COL32(0, 0, 0, static_cast<int>(170 * a)), text);
        dl->AddText(font, s.size_px, at, IM_COL32(245, 244, 238, static_cast<int>(255 * a)), text);
        any = true;
        last.last_x = p.x, last.last_y = p.y, last.last_w = bw, last.last_h = bh + tri;
        last.last_dist = p.dist, last.last_size = s.size_px;
        last.last_fitted = nametag::FitViewport(v.right / v.top, w, h).fitted;
    }
    AcquireSRWLockExclusive(&g_stats_lock);
    g_stats.offscreen += hidden;
    if (any) {
        ++g_stats.drawn_frames;
        g_stats.last_x = last.last_x, g_stats.last_y = last.last_y, g_stats.last_w = last.last_w, g_stats.last_h = last.last_h;
        g_stats.last_dist = last.last_dist, g_stats.last_size = last.last_size, g_stats.last_fitted = last.last_fitted;
    }
    ReleaseSRWLockExclusive(&g_stats_lock);

    // Evidence for logs (hidden runs): the first draws, then one line every 5 s while drawn, and
    // each change between drawn and not drawn. Names are logged as length + hash only.
    const ULONGLONG now = GetTickCount64();
    if (any != g_was_drawn || (any && (g_logged_draws < 3 || now >= g_next_log_tick))) {
        if (any) {
            ++g_logged_draws;
            g_next_log_tick = now + 5000;
            Log("[tags] drawn: label %zu B fnv %08X, anchor (%.0f, %.0f) px, box %.0fx%.0f, text %.1f px, %.1f m%s; "
                "back buffer %.0fx%.0f, view %s%.3f",
                std::strlen(f.tags[0].text), nametag::NameHash(f.tags[0].text), last.last_x, last.last_y, last.last_w, last.last_h,
                last.last_size, last.last_dist, last.last_fitted ? " (letterboxed)" : "", w, h, "aspect ",
                static_cast<double>(v.right / v.top));
        } else {
            Log("[tags] not drawn: the partner is behind the camera, outside the view or too far");
        }
        g_was_drawn = any;
    }
}

Stats GetStats() noexcept {
    AcquireSRWLockShared(&g_stats_lock);
    const Stats s = g_stats;
    ReleaseSRWLockShared(&g_stats_lock);
    return s;
}

}  // namespace st::overlay::tags
