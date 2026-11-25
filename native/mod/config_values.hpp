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

// enemy_entry_err_max (M0): 0 (off) or 5..1000 m.
inline double EntryErrMax(const std::string& v, double keep) {
    char* end = nullptr;
    const double r = std::strtod(v.c_str(), &end);
    if (v.empty() || !end || *end) return keep;
    return r == 0.0 || (r >= 5.0 && r <= 1000.0) ? r : keep;
}

// enemy_host_far (M0's quick path): 0 (off) or 300..10000 m.
inline double HostFar(const std::string& v, double keep) {
    char* end = nullptr;
    const double r = std::strtod(v.c_str(), &end);
    if (v.empty() || !end || *end) return keep;
    return r == 0.0 || (r >= 300.0 && r <= 10000.0) ? r : keep;
}

// exo_rig_extra (round 20, docs/real-partner-body.md section 41): the suit rig's optional parts, a mask 0..15 (1 legbelt, 2 cantmount,
// 4 subpouch, 8 bodyparts).
inline int RigExtra(const std::string& v, int keep) {
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || !end || *end) return keep;
    return n >= 0 && n <= 15 ? static_cast<int>(n) : keep;
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
// (P2), enemy_owner follows enemy_hits when kOwnerDefaultOn (P1c: on only once its gate passed in a game), else off unless given.
// A key that was given keeps its value.
inline constexpr bool kOwnerDefaultOn = true;  // round 21: the P1c gate two-games-enemy-camp passed (run 20261125-083424)
struct EnemyKeys {
    bool sync = false, hits = false, owner = false;
};
inline EnemyKeys EnemyDefaults(bool host_save, bool sync_set, bool sync, bool hits_set, bool hits, bool owner_set, bool owner) {
    EnemyKeys k;
    k.sync = sync_set ? sync : host_save;
    k.hits = hits_set ? hits : k.sync;
    k.owner = owner_set ? owner : kOwnerDefaultOn && k.hits;
    return k;
}
// The P1c keys (round 21), each on unless given 0: enemy_owner_outcomes, enemy_follow_guard, enemy_attack_registry, enemy_owner_hits.
// SetOwnerKey: true when `key` is one of them (its value parsed into `k`); review rv-correctness N9.
struct OwnerKeys {
    bool outcomes = true, follow_guard = true, attack_registry = true, hits = true;
};
inline bool SetOwnerKey(OwnerKeys& k, const std::string& key, bool v) {
    bool* const p = key == "enemy_owner_outcomes"    ? &k.outcomes
                    : key == "enemy_follow_guard"    ? &k.follow_guard
                    : key == "enemy_attack_registry" ? &k.attack_registry
                    : key == "enemy_owner_hits"      ? &k.hits
                                                     : nullptr;
    if (p) *p = v;
    return p != nullptr;
}
// The round-23 keys (docs/enemy-sync.md 14), each on unless given 0: enemy_restrain_relay (the tie-up relay: HIT kind 5 both ways,
// the copy's pose held), enemy_replay_flush (a shooter's queued shot replays dropped when it goes down). SetRestrainKey: true when
// `key` is one of them.
struct RestrainKeys {
    bool relay = true, flush = true;
};
inline bool SetRestrainKey(RestrainKeys& k, const std::string& key, bool v) {
    bool* const p = key == "enemy_restrain_relay" ? &k.relay : key == "enemy_replay_flush" ? &k.flush : nullptr;
    if (p) *p = v;
    return p != nullptr;
}
// The round-23 B keys (docs/enemy-sync.md 14.8), each on unless given 0: enemy_mirror_hold_check (a mirror that does not hold its
// owner's place after 3 teleports in 2 s is given back and hidden, retried every 5 s), enemy_mirror_vehicle (the in-vehicle test
// every 1 s on both sides, the SEN1 kInVehicle bit, a seated copy released to its own game). SetMirrorKey: true when `key` is one.
struct MirrorKeys {
    bool hold_check = true, vehicle = true;
};
inline bool SetMirrorKey(MirrorKeys& k, const std::string& key, bool v) {
    bool* const p = key == "enemy_mirror_hold_check" ? &k.hold_check : key == "enemy_mirror_vehicle" ? &k.vehicle : nullptr;
    if (p) *p = v;
    return p != nullptr;
}
// The round-23 D keys (docs/enemy-sync.md 14.10), each on unless given 0: vehicle_mule_log (classify the trucks, log their MULE
// drivers), vehicle_mule_hold (a truck a non-player drives, or a MULE truck no player drives, is left to each game).
// SetTruckKey: true when `key` is one.
struct TruckKeys {
    bool log = true, hold = true;
};
inline bool SetTruckKey(TruckKeys& k, const std::string& key, bool v) {
    bool* const p = key == "vehicle_mule_log" ? &k.log : key == "vehicle_mule_hold" ? &k.hold : nullptr;
    if (p) *p = v;
    return p != nullptr;
}
// The round-23 E key (docs/enemy-sync.md 14.11): enemy_m2_probes (off unless given 1: the debug action enemy_m2, the M2 probes P1-P6,
// in hidden runs only). SetProbeKey: true when `key` is it.
inline bool SetProbeKey(bool& on, const std::string& key, bool v) {
    if (key != "enemy_m2_probes") return false;
    on = v;
    return true;
}
// The round-23 C keys (docs/enemy-sync.md 14.9): enemy_down_hold (on unless given 0: while the other player is down - death or
// KnotSpace, never a faint - the copies of that game's fights are held frozen in their last pose or hidden here, never given back
// alive, and no ownership changes), enemy_down_hold_radius (m from our player: nearer copies are held or hidden, farther standing
// ones follow the old rules; 20..1000, else the default). SetDownKey: true when `key` is enemy_down_hold.
inline constexpr double kDownHoldRadiusDefault = 120.0;
inline bool SetDownKey(bool& hold, const std::string& key, bool v) {
    if (key != "enemy_down_hold") return false;
    hold = v;
    return true;
}
inline double DownRadius(const std::string& v, double keep) { return MirrorRadius(v, keep); }

}  // namespace st::cfgval
