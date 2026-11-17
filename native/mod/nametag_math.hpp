// Name tags (docs/real-partner-body.md section 10): the pure parts, unit-tested in st_net_tests.
//   * Project: a world point through the game's own view snapshot (the camera view object's
//     ViewFrustumInfo copy, nametags.cpp) to back-buffer pixels.
//   * StyleFor: text size and opacity by distance.
//   * ComposeLabel: the text shown for a partner.
// No game, Windows or ImGui dependencies.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace st::nametag {

// The game's view snapshot (ds.exe camera view object + 0x28 .. + 0x84, nametags.cpp), in
// world units (metres, Z up). axis[0] = right, axis[1] = forward, axis[2] = up.
struct View {
    double pos[3] = {};
    float axis[3][3] = {};
    float near_plane = 0.1f, far_plane = 0;
    float top = 0;     // near * tan(fov_v / 2)
    float right = 0;   // top * aspect
    bool perspective = true;
};

struct Projected {
    bool visible = false;
    float x = 0, y = 0;  // back-buffer pixels, origin top left
    float depth = 0;     // along the camera's forward axis (m)
    float dist = 0;      // straight-line distance camera -> point (m)
};

// The part of a w x h back buffer the game's view covers: the whole buffer, or a centred
// letter-/pillarbox when the view's aspect (right / top) differs from w / h by more than 1 %.
struct Viewport {
    float x = 0, y = 0, w = 0, h = 0;
    bool fitted = false;  // letter- or pillarboxed
};
inline Viewport FitViewport(float view_aspect, float w, float h) {
    Viewport v{0, 0, w, h, false};
    if (!(view_aspect > 0) || !(w > 0) || !(h > 0)) return v;
    const float buf = w / h;
    if (std::fabs(view_aspect - buf) <= 0.01f * buf) return v;
    v.fitted = true;
    if (view_aspect > buf) {  // wider: bars top and bottom
        v.h = w / view_aspect;
        v.y = (h - v.h) * 0.5f;
    } else {  // narrower: bars left and right
        v.w = h * view_aspect;
        v.x = (w - v.w) * 0.5f;
    }
    return v;
}

inline bool ViewUsable(const View& v) {
    if (!v.perspective || !(v.near_plane > 0) || !(v.top > 0) || !(v.right > 0)) return false;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(v.pos[i])) return false;
        double n = 0;
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(v.axis[i][k])) return false;
            n += double(v.axis[i][k]) * v.axis[i][k];
        }
        if (n < 0.9 || n > 1.1) return false;  // unit axes
    }
    return true;
}

// Projects `world` into a w x h back buffer. Not visible: unusable view, behind or on the near
// plane, or outside the view (with `margin` pixels of slack on every side).
inline Projected Project(const View& v, const double (&world)[3], float w, float h, float margin = 0.0f) {
    Projected p;
    if (!ViewUsable(v) || !(w > 0) || !(h > 0)) return p;
    const double d[3] = {world[0] - v.pos[0], world[1] - v.pos[1], world[2] - v.pos[2]};
    auto dot = [&](int a) { return d[0] * v.axis[a][0] + d[1] * v.axis[a][1] + d[2] * v.axis[a][2]; };
    const double xc = dot(0), zc = dot(1), yc = dot(2);
    p.depth = static_cast<float>(zc);
    p.dist = static_cast<float>(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    if (!(zc > v.near_plane)) return p;
    const double ndc_x = (xc / zc) * v.near_plane / v.right;
    const double ndc_y = (yc / zc) * v.near_plane / v.top;
    const Viewport vp = FitViewport(v.right / v.top, w, h);
    p.x = static_cast<float>(vp.x + (0.5 + 0.5 * ndc_x) * vp.w);
    p.y = static_cast<float>(vp.y + (0.5 - 0.5 * ndc_y) * vp.h);
    p.visible = std::isfinite(p.x) && std::isfinite(p.y) && p.x >= vp.x - margin && p.x <= vp.x + vp.w + margin &&
                p.y >= vp.y - margin && p.y <= vp.y + vp.h + margin;
    return p;
}

// Text size and opacity by distance (metres). size_px is for a 1080p-sized UI and is multiplied
// by ui_scale (the overlay's back-buffer scale: 1 at 1080p, 1.33 at 1440p).
struct Style {
    bool visible = false;
    float size_px = 0;
    float alpha = 0;         // 0..1
    bool show_distance = false;
};
inline constexpr float kNearSizeM = 5.0f, kFarSizeM = 60.0f;          // 26 px at <= 5 m .. 15 px at >= 60 m
inline constexpr float kNearPx = 26.0f, kFarPx = 15.0f;
inline constexpr float kFadeStartM = 150.0f, kFadeEndM = 300.0f;      // fully gone at 300 m
inline constexpr float kMinDistM = 0.8f;                             // closer: the camera is inside the body
inline constexpr float kShowDistanceM = 25.0f;                       // " - 42 m" from here
inline Style StyleFor(float dist_m, float ui_scale) {
    Style s;
    if (!std::isfinite(dist_m) || dist_m < kMinDistM || dist_m >= kFadeEndM) return s;
    float t = (dist_m - kNearSizeM) / (kFarSizeM - kNearSizeM);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float scale = ui_scale > 0 ? ui_scale : 1.0f;
    s.size_px = (kNearPx + (kFarPx - kNearPx) * t) * scale;
    s.alpha = dist_m <= kFadeStartM ? 1.0f : 1.0f - (dist_m - kFadeStartM) / (kFadeEndM - kFadeStartM);
    s.visible = s.alpha > 0.02f;
    s.show_distance = dist_m >= kShowDistanceM;
    return s;
}

// What the overlay draws for a partner. `peer` is their sanitized name ("" = none sent yet),
// `own` this player's own. Two games on one Steam account (the local two-player test) send the
// same name: then the peer's role is appended ("Sam (host)" / "Sam (guest)"), as it is when the
// peer sent nothing ("Host" / "Guest"). `echo`: the partner is our own replay ("<own> (echo)").
inline std::size_t ComposeLabel(const char* peer, const char* own, bool peer_is_host, bool echo, char* out, std::size_t cap) {
    if (!out || !cap) return 0;
    const char* role = peer_is_host ? "host" : "guest";
    int n;
    if (echo) n = std::snprintf(out, cap, "%s (echo)", own && *own ? own : "You");
    else if (!peer || !*peer) n = std::snprintf(out, cap, "%s", peer_is_host ? "Host" : "Guest");
    else if (own && std::strcmp(peer, own) == 0) n = std::snprintf(out, cap, "%s (%s)", peer, role);
    else n = std::snprintf(out, cap, "%s", peer);
    if (n < 0) {
        out[0] = 0;
        return 0;
    }
    if (static_cast<std::size_t>(n) < cap) return static_cast<std::size_t>(n);
    // Cut: never end inside a UTF-8 sequence (snprintf cuts at a byte).
    std::size_t len = cap - 1, start = len;
    while (start > 0 && (static_cast<unsigned char>(out[start - 1]) & 0xC0) == 0x80) --start;
    if (start > 0) {
        const unsigned lead = static_cast<unsigned char>(out[start - 1]);
        const std::size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (len - (start - 1) < need) len = start - 1;
    }
    out[len] = 0;
    return len;
}

// FNV-1a of a name, for logs (names are not logged in full).
inline unsigned NameHash(const char* s) {
    unsigned h = 2166136261u;
    for (; s && *s; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 16777619u;
    return h;
}

}  // namespace st::nametag
