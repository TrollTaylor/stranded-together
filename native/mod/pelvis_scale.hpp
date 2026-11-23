// The hips (pelvis joint) translation of a body smaller or larger than Sam (docs/real-partner-body.md section 38.3). The pure
// part: no game memory, no Windows; unit-tested in tests/self_predict_tests.cpp. Used by real_body.cpp's ComposeLive (the
// self view), ComposePartner (the partner body) and the fit (Sam's hidden skeleton under the self view).
//
// Since v0.7 the hips translation was Sam's times the pelvis ratio (her rest hips height over his: 0.957 for Fragile's room
// body, 0.975 for her porter suit): right
// while the hips sit about a metre above the root joint (standing, walking, seated), where it lowers her hips by her shorter
// legs. The private room's anchored animations put Sam's hips 1.5-2.5 m from his root (the bed close-up, the chair), and
// there the ratio pulled her whole body 6-11 cm toward the root, and that offset jumped when the room re-anchored him.
// From 1.45 m on the scale is a constant instead: Sam's translation minus her rest offset (her hips sit lower by the same
// 4.3 cm as at rest); 1.15-1.45 m blend smoothly; below 1.15 m the result is exactly the ratio's, as before.
#pragma once
#include <cmath>

namespace st::pelvis {

constexpr float kNearM = 1.15f;  // up to this hips distance from the root: the ratio, bit-identical to v0.26
constexpr float kFarM = 1.45f;   // from this distance on: the constant rest offset

// Her rest offset: (1 - ratio) x Sam's rest hips length along her rest hips direction (`her_rest_t`, her own latched hips
// translation). False (and `off` zero) when that direction is not usable (not finite, under 0.3 m, a ratio of 1).
inline bool RestOffset(const float* her_rest_t, float ratio, float sam_rest_len, float (&off)[3]) {
    off[0] = off[1] = off[2] = 0;
    if (!her_rest_t || !std::isfinite(ratio) || !std::isfinite(sam_rest_len) || !(sam_rest_len > 0)) return false;
    const double x = her_rest_t[0], y = her_rest_t[1], z = her_rest_t[2];
    const double len = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(len) || len < 0.3 || std::fabs(1.0 - ratio) < 1e-6) return false;
    const double k = (1.0 - ratio) * sam_rest_len / len;
    off[0] = static_cast<float>(x * k), off[1] = static_cast<float>(y * k), off[2] = static_cast<float>(z * k);
    return std::isfinite(off[0]) && std::isfinite(off[1]) && std::isfinite(off[2]);
}

// The body's hips translation from Sam's `t`. `off` null (no rest offset, or the rule off): the ratio everywhere.
inline void Scaled(const float* t, float ratio, const float* off, float* o) {
    const float n0 = t[0] * ratio, n1 = t[1] * ratio, n2 = t[2] * ratio;
    o[0] = n0, o[1] = n1, o[2] = n2;
    if (!off) return;
    const double len = std::sqrt(static_cast<double>(t[0]) * t[0] + static_cast<double>(t[1]) * t[1] + static_cast<double>(t[2]) * t[2]);
    if (!std::isfinite(len) || len <= kNearM) return;
    double w = (len - kNearM) / (kFarM - kNearM);
    w = w >= 1.0 ? 1.0 : w * w * (3.0 - 2.0 * w);
    const float f0 = t[0] - off[0], f1 = t[1] - off[1], f2 = t[2] - off[2];
    o[0] = static_cast<float>(n0 + w * (f0 - n0));
    o[1] = static_cast<float>(n1 + w * (f1 - n1));
    o[2] = static_cast<float>(n2 + w * (f2 - n2));
}

}  // namespace st::pelvis
