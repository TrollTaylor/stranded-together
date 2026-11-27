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
// The round-23 E key (docs/enemy-sync.md 14.11): enemy_m2_probes (on unless given 0 since round 26: her single takes' read-only P2
// window D1-D8; the debug action enemy_m2, the M2 probes P1-P6, in hidden runs only). SetProbeKey: true when `key` is it.
inline bool SetProbeKey(bool& on, const std::string& key, bool v) {
    if (key != "enemy_m2_probes") return false;
    on = v;
    return true;
}
// Round 28 P1: session.ini enemy_phit_probe (the attack probe, enemy_tgt 14). SetPhitKey: true when `key` is it.
inline bool SetPhitKey(bool& on, const std::string& key, bool v) {
    if (key != "enemy_phit_probe") return false;
    on = v;
    return true;
}
// Round 28 E2: session.ini enemy_partner_melee (0 / 1 / 2, anything else 0) and enemy_melee_face. True when `key` is one of them.
inline bool SetPartnerMeleeKey(int& mode, bool& face, const std::string& key, const std::string& v, bool vbool) {
    if (key == "enemy_partner_melee") {
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        mode = end != v.c_str() && (n == 1 || n == 2) ? static_cast<int>(n) : 0;
        return true;
    }
    if (key == "enemy_melee_face") {
        face = vbool;
        return true;
    }
    return false;
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

// The round-25 keys (docs/enemy-sync.md 14.18), ON by default since round 26 (14.19, at the user's request; each off with =0); the
// levers that write game state act at a take of one MULE alone, in real games too.
//   enemy_take_know 0|1|2       K1: her game forces a player visual (her Sam) into a MULE she takes alone (1 once, 2 then 2 Hz)
//   enemy_take_know_secs 1..10  K1's window for mode 2 (default 3 s)
//   enemy_take_camp 0|1|2       C1: her camp forced to phase 5 before the take (1 only when it was >= 4, 2 always)
//   enemy_ticket_return 0|1     T1: attack tickets held by her frozen copies returned to their camp's group
//   enemy_single_hold_vehicle   V1: while a single of a camp is hers, that camp's in-vehicle mirror release waits (no game writes)
//   enemy_single_rule 0|1       R1: the host's single takes follow RULE-W / RULE-B (a camp take or a hold), no game writes
//   enemy_take_role 0|1         R2: her taken MULE's camp role 1 reads 0 while it is hers (round 25 e)
//   enemy_take_face 0|1         F1: her taken MULE turned in place to face her Sam at the take when she is > 60 deg off (round 25 e)
// SetTakeKey: true when `key` is one of them (a bad value keeps the old one).
// (the defaults: config.hpp's; round 26: K1 mode 2, C1 mode 1, the rest on)
struct TakeKeys {
    int know = 2;
    double know_secs = 3.0;
    int camp = 1;
    bool tickets = true;
    bool hold_vehicle = true;
    bool rule = true;
    bool role = true;
    bool face = true;
};
inline bool TakeBool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }
inline bool SetTakeKey(TakeKeys& k, const std::string& key, const std::string& v) {
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    const bool num = !v.empty() && end && !*end;
    if (key == "enemy_take_know" || key == "enemy_take_camp") {
        int& f = key == "enemy_take_know" ? k.know : k.camp;
        if (num && (d == 0.0 || d == 1.0 || d == 2.0)) f = static_cast<int>(d);
        return true;
    }
    if (key == "enemy_take_know_secs") {
        if (num && d >= 1.0 && d <= 10.0) k.know_secs = d;
        return true;
    }
    bool* const p = key == "enemy_ticket_return" ? &k.tickets : key == "enemy_single_hold_vehicle" ? &k.hold_vehicle
                                                              : key == "enemy_single_rule"         ? &k.rule
                                                              : key == "enemy_take_role"           ? &k.role
                                                              : key == "enemy_take_face"           ? &k.face
                                                                                                   : nullptr;
    if (p) *p = TakeBool(v);
    return p != nullptr;
}

// The round-27 M2 / M3 / M4 keys (docs/enemy-sync.md 16; never written by the launcher). Round 27 M4: on by default with
// enemy_hits (TargetDefaults; the destroy guard closed the body-destroy gap, 16.2); an explicit key always wins (0 too):
//   enemy_partner_stim 0|1          M2: the host gives the partner body a visual stimulus made with its own Sam's resource (L1;
//                                   only while the destroy guard is in); both games send SPV1. Default 1 with enemy_hits
//   enemy_partner_vis 0|1           its +0xA4 follows the partner's own (SPV1); 0: always 1.0 (default 1)
//   enemy_partner_stim_radius       m: a realized human enemy this near the partner body, else no stimulus (20..2000, default 300)
//   enemy_target_pick 0..127        M3 bits: 1 the PlayerVisual reorder / suppress, 2 the MULE-only perceivers (H5), 4 the
//                                   classifier flag fix (H2), 8 her hits force a visual (H3), 16 the line-of-sight keep, 32 her
//                                   record from ours (needs 1 and 16), 64 take first (M4: a camp about to go to her stays calm).
//                                   Default 71 (1 + 2 + 4 + 64) with enemy_hits
//   enemy_target_redirect 0..255    M3 bits: 1 CanShoot's point, 2 turn / range check, 4 approach, 16 the vehicle chase, 32 the
//                                   shoot actor (8: the PlayerInfo shadow, not built: ignored). Default 35 (1 + 2 + 32) with enemy_hits.
//                                   Bit 64 (the melee face) is ignored here: only enemy_melee_face=1 sets it
//   enemy_target_fresh_ms 500..3000 how long a target missing from the bucket stays "visible" (default 1500)
// SetTargetKey: true when `key` is one of them (a bad value keeps the old one); the *_set flags: a valid value was given.
inline constexpr int kTargetStimDefault = 1, kTargetPickDefault = 71, kTargetRedirectDefault = 35, kTargetPickMax = 127;
struct TargetKeys {
    int stim = 0;
    bool vis = true;
    double radius = 300.0;
    int pick = 0;
    int redirect = 0;
    int fresh_ms = 1500;
    bool stim_set = false, pick_set = false, red_set = false;
};
// The defaults (round 27 M4): the keys not given follow enemy_hits (on: 1 / 71 / 35; off: 0 / 0 / 0); a given key is kept.
inline TargetKeys TargetDefaults(bool hits, TargetKeys k, bool stim_set, bool pick_set, bool red_set) {
    if (!stim_set) k.stim = hits ? kTargetStimDefault : 0;
    if (!pick_set) k.pick = hits ? kTargetPickDefault : 0;
    if (!red_set) k.redirect = hits ? kTargetRedirectDefault : 0;
    return k;
}
inline bool SetTargetKey(TargetKeys& k, const std::string& key, const std::string& v) {
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    const bool num = !v.empty() && end && !*end && d == d;
    if (key == "enemy_partner_stim") {
        if (num && (d == 0.0 || d == 1.0)) k.stim = static_cast<int>(d), k.stim_set = true;
        return true;
    }
    if (key == "enemy_partner_vis") {
        k.vis = TakeBool(v);
        return true;
    }
    if (key == "enemy_partner_stim_radius") {
        if (num && d >= 20.0 && d <= 2000.0) k.radius = d;
        return true;
    }
    if (key == "enemy_target_pick" || key == "enemy_target_redirect") {
        const bool pick = key == "enemy_target_pick";
        const double top = pick ? static_cast<double>(kTargetPickMax) : 255.0;
        if (num && d >= 0.0 && d <= top && d == static_cast<double>(static_cast<int>(d))) {
            (pick ? k.pick : k.redirect) = static_cast<int>(d);
            (pick ? k.pick_set : k.red_set) = true;
        }
        return true;
    }
    if (key == "enemy_target_fresh_ms") {
        if (num) k.fresh_ms = d < 500.0 ? 500 : d > 3000.0 ? 3000 : static_cast<int>(d);
        return true;
    }
    return false;
}

}  // namespace st::cfgval
