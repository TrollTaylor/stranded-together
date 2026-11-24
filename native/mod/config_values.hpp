// The session.ini values with more than on / off (config.cpp; pure: tests/enemy_rules_tests.cpp). Each returns the parsed
// value, or `keep` (the default already in Config) when the text is out of range or malformed.
#pragma once

#include <cstdlib>
#include <string>

namespace st::cfgval {

inline bool Bool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

// shots_sound: "auto" -1 (on while sound_sync=0), else on / off.
inline int ShotsSound(const std::string& v) { return v == "auto" ? -1 : Bool(v) ? 1 : 0; }

// enemy_freeze: "occupied" or "1" (the only lever built), anything else 0 (not built: enemy sync stays off).
inline int EnemyFreeze(const std::string& v) { return v == "occupied" || v == "1" ? 1 : 0; }

// enemy_mirror_radius: 20..1000 m.
inline double MirrorRadius(const std::string& v, double keep) {
    char* end = nullptr;
    const double r = std::strtod(v.c_str(), &end);
    if (v.empty() || !end || *end) return keep;
    return r >= 20.0 && r <= 1000.0 ? r : keep;
}

// enemy_pose_count: 0..8 posed mirrors.
inline int PoseCount(const std::string& v, int keep) {
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || !end || *end) return keep;
    return n >= 0 && n <= 8 ? static_cast<int>(n) : keep;
}

}  // namespace st::cfgval
