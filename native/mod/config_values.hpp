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

// enemy_hit_owner (P2, docs/enemy-sync.md 9): credit 0 (the partner body, the player's attacker id stamped), body 1, none 2, sam 3.
// The numbers are the HITRES owner mode.
inline int HitOwner(const std::string& v, int keep) {
    return v == "credit" ? 0 : v == "body" ? 1 : v == "none" ? 2 : v == "sam" ? 3 : keep;
}
inline const char* HitOwnerName(int o) { return o == 0 ? "credit" : o == 1 ? "body" : o == 2 ? "none" : o == 3 ? "sam" : "?"; }
// enemy_hit_path: direct 0 (M1: the bullet's path) or query 1 (M2: the attack sphere query).
inline int HitPath(const std::string& v, int keep) { return v == "direct" ? 0 : v == "query" ? 1 : keep; }
// enemy_hit_rate: 1..120 HITs a second per enemy.
inline int HitRate(const std::string& v, int keep) {
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || !end || *end) return keep;
    return n >= 1 && n <= 120 ? static_cast<int>(n) : keep;
}

// The enemy keys' defaults once every line is read (config.cpp): enemy_sync follows host_save, enemy_hits follows enemy_sync
// (P2), enemy_owner stays off (P1b: its gate has not passed) unless given. A key that was given keeps its value.
struct EnemyKeys {
    bool sync = false, hits = false, owner = false;
};
inline EnemyKeys EnemyDefaults(bool host_save, bool sync_set, bool sync, bool hits_set, bool hits, bool owner_set, bool owner) {
    EnemyKeys k;
    k.sync = sync_set ? sync : host_save;
    k.hits = hits_set ? hits : k.sync;
    k.owner = owner_set ? owner : false;
    return k;
}

}  // namespace st::cfgval
