// Round 31 stage 7 (bt_catcher): this feature's own pure logic (no game, no Windows), tested by tests/bt_catcher_tests.cpp. The
// shared codecs, caps and the stage-1 gates stay in bt_rules.hpp (included); new pure helpers of this feature go here, so the
// seven builders never edit one file at once.
//
// CS (the build spec, Stage 7; PF 18): the sender reads the catcher manager's records (cm = [0x147BEE060], count cm+0x88, data
// cm+0x90, 0x110 each) at 4 Hz and diffs them (CatchSender: the primary source); the notify FUN_142A1C0E0(cm, id, hash, 0) adds
// its hash (SPAWN 0x63DB9ED5 proven; GONE 0x2FC57393 proven; DEFEAT 0x4C8F5A9D / ANNIHILATE 0x033DB880 decoded, never seen). The
// receiver keeps the partner's catchers (CatchView) for the log, the marker, the HUD line and the rain spot. RW (REWARD, crystals)
// is DROPPED this round: nothing here gives crystals.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bt_rules.hpp"

namespace st::btrules::catcher {

constexpr int kStage = 7;

// ---------------------------------------------------------------- the record (ds.exe DC; byte-checked by bt_catcher.cpp)
// +0x00 the entity ref (0: none) | +0x20 the actor id (the notify's key: FUN_142A1C0E0 finds the record by +0x20 at 0x142A1C12C)
// | +0x28 u8 the class (EDSCatcherType, movzx at 0x142A1C18C) | +0x68 f64 pos[3] (stored at 0x142A87955) | +0xB0 f64 bar[3] (the
// health bar's anchor, 0x142A7CE2B) | +0xF0 u8 gone (bt_sync's census) | +0xF4 f32 hp (0x142A7CE1F) | +0x100 f32 sub-life
// (0x142A7CE3E).
constexpr std::size_t kRecSize = 0x110, kRecEnt = 0x00, kRecKey = 0x20, kRecClass = 0x28, kRecPos = 0x68, kRecBar = 0xB0, kRecGone = 0xF0,
                      kRecHp = 0xF4, kRecSub = 0x100;
constexpr int kRecMax = 64;  // bt_sync's census bound on cm+0x88
static_assert(kRecSub + 4 <= kRecSize && kRecBar + 24 <= kRecSize, "the record's fields fit in 0x110");

struct CatchRec {
    std::uint32_t key = 0;
    std::uint8_t class1 = 0;  // +0x28 + 1 (0: unknown, above 7)
    bool entity = false, inactive = false, pos_ok = false;
    double pos[3] = {0, 0, 0}, bar[3] = {0, 0, 0};
    float hp = -1, sub = -1;
};
// A position the game set: sane and not the zero a new record starts with (PF 18: two of three records read (0 0 0) for 1 s).
inline bool RecPosOk(const double* p) {
    if (!PosSane(p)) return false;
    return std::fabs(p[0]) > 0.001 || std::fabs(p[1]) > 0.001 || std::fabs(p[2]) > 0.001;
}
// The ring's height: the bar anchor's z when it is set and within 50 m of the root (PF 18: 7 m above it), else the root's z.
inline float BarZ(const double* pos, const double* bar) {
    if (RecPosOk(bar) && std::fabs(bar[2] - pos[2]) <= 50.0) return static_cast<float>(bar[2]);
    return static_cast<float>(pos[2]);
}
inline std::uint8_t Class1Of(std::uint8_t type) { return type <= 7 ? static_cast<std::uint8_t>(type + 1) : 0; }
// One record from its 0x110 bytes.
inline CatchRec ParseRec(const std::uint8_t* r) {
    CatchRec c;
    std::uint64_t ent = 0;
    std::memcpy(&ent, r + kRecEnt, 8);
    std::memcpy(&c.key, r + kRecKey, 4);
    c.class1 = Class1Of(r[kRecClass]);
    std::memcpy(c.pos, r + kRecPos, sizeof(c.pos));
    std::memcpy(c.bar, r + kRecBar, sizeof(c.bar));
    std::memcpy(&c.hp, r + kRecHp, 4);
    std::memcpy(&c.sub, r + kRecSub, 4);
    c.entity = ent != 0;
    c.inactive = r[kRecGone] != 0;
    c.pos_ok = RecPosOk(c.pos);
    if (!std::isfinite(c.hp)) c.hp = -1;
    if (!std::isfinite(c.sub)) c.sub = -1;
    return c;
}

// ---------------------------------------------------------------- the notify's hashes
constexpr std::uint32_t kNotifyGone = 0x2FC57393;  // PF 18 (b2:818, type 255, thread other): the only end edge ever seen
inline std::uint8_t EdgeOfNotify(std::uint32_t hash) { return hash == kNotifyGone ? kCeGone : CatchEdgeOfNotify(hash); }
inline const char* NotifyName(std::uint32_t hash) {
    const std::uint8_t e = EdgeOfNotify(hash);
    return e ? CatchEdgeName(e) : "other";
}

// A percent of the most seen (TroubleTracker's rule): clamped 0..100; -1 when nothing above 0 was ever seen.
inline float Pct(float v, float most) {
    if (!std::isfinite(v) || !std::isfinite(most) || most <= 0 || v < 0) return -1.0f;
    const float p = v * 100.0f / most;
    return p > 100.0f ? 100.0f : p;
}

// ---------------------------------------------------------------- the sender (game thread, 4 Hz)
// A track per catcher key (at most kSendTrackMax). SPAWN: a positioned active record with a new key (the diff) or the spawn notify
// (sent once its record has a position); the end: DEFEAT / ANNIHILATE / GONE from the notify, or GONE from the diff (a key seen
// active that went inactive or missing). A notify-only key never seen in the records is dropped silently after kCatchMissSteps
// passes. Each edge goes in the next message (edges first, then the live ones by turns, at most kBtsCatchMax) and stays pending
// until Sent() confirms that message went; an ended track goes with its end entry.
constexpr unsigned kSendTrackMax = 8;
constexpr double kSendGapMs = 250.0;  // 4 Hz
struct SendTrack {
    std::uint32_t key = 0;
    std::uint8_t class1 = 0, pending = kCeNone;
    bool entity = false, have_pos = false, in_recs = false, ended = false, from_notify = false;
    unsigned misses = 0;
    float pos[3] = {0, 0, 0}, bar_z = 0, hp = -1, sub = -1, hp_most = 0, sub_most = 0;
    std::uint32_t hash = 0;  // the last notify hash for this key (0 none)
    double spawn_ms = 0;
};
struct EdgeNote {
    std::uint32_t key = 0;
    std::uint8_t class1 = 0, edge = kCeNone;
    bool notify = false;  // from the notify (else the record diff)
    float pos[3] = {0, 0, 0}, hp = -1;
    double age_ms = 0;  // the end edges: since the spawn
};
struct CatchSender {
    SendTrack t[kSendTrackMax];
    unsigned n = 0, rot = 0;
    std::uint32_t incl[kBtsCatchMax] = {};
    unsigned n_incl = 0;

    int Find(std::uint32_t key) const {
        for (unsigned i = 0; i < n; ++i)
            if (t[i].key == key) return static_cast<int>(i);
        return -1;
    }
    void Remove(unsigned i) { t[i] = t[--n]; }
    void Reset() { n = 0, n_incl = 0; }
    // Ended tracks are dropped (the partner is gone: their end entries would go nowhere).
    void DropEnded() {
        for (unsigned i = 0; i < n;)
            if (t[i].ended) Remove(i);
            else ++i;
        n_incl = 0;
    }
    unsigned Live() const {
        unsigned k = 0;
        for (unsigned i = 0; i < n; ++i) k += (!t[i].ended && t[i].have_pos) ? 1u : 0u;
        return k;
    }
    static EdgeNote Note(const SendTrack& s, std::uint8_t edge, bool notify, double now_ms) {
        EdgeNote e;
        e.key = s.key, e.class1 = s.class1, e.edge = edge, e.notify = notify;
        std::memcpy(e.pos, s.pos, sizeof(e.pos));
        e.hp = Pct(s.hp, s.hp_most);
        e.age_ms = edge == kCeSpawn ? 0 : now_ms - s.spawn_ms;
        return e;
    }
    // The notify (drained on the game thread). The edge it made (kCeNone: nothing new); *note is filled for an end edge.
    std::uint8_t Notify(std::uint32_t key, std::uint32_t hash, double now_ms, EdgeNote* note) {
        const std::uint8_t e = EdgeOfNotify(hash);
        const int k = Find(key);
        if (k >= 0) t[k].hash = hash;
        if (!e) return kCeNone;
        if (e == kCeSpawn) {
            if (k >= 0 || n >= kSendTrackMax) return kCeNone;
            SendTrack s;
            s.key = key, s.pending = kCeSpawn, s.from_notify = true, s.spawn_ms = now_ms, s.hash = hash;
            t[n++] = s;  // its SPAWN line comes from Step, once the record has a position
            return kCeSpawn;
        }
        if (k < 0 || t[k].ended) return kCeNone;  // an end for a key the partner never heard of, or after the end
        t[k].ended = true, t[k].pending = e;
        if (note) *note = Note(t[k], e, true, now_ms);
        return e;
    }
    // One record pass; the edges it made in *out (at most cap; the count returned).
    unsigned Step(const CatchRec* recs, unsigned nrecs, double now_ms, EdgeNote* out, unsigned cap) {
        unsigned k = 0;
        auto put = [&](const EdgeNote& e) {
            if (out && k < cap) out[k++] = e;
        };
        for (unsigned i = 0; recs && i < nrecs; ++i) {
            const CatchRec& r = recs[i];
            if (r.inactive || !r.pos_ok) continue;
            int at = Find(r.key);
            if (at < 0) {
                if (n >= kSendTrackMax) continue;
                SendTrack s;
                s.key = r.key, s.pending = kCeSpawn, s.spawn_ms = now_ms;
                t[n] = s;
                at = static_cast<int>(n++);
            }
            SendTrack& s = t[at];
            if (s.ended) continue;
            const bool fresh = !s.in_recs;
            if (r.class1) s.class1 = r.class1;
            s.entity = r.entity, s.have_pos = true, s.in_recs = true, s.misses = 0;
            for (int j = 0; j < 3; ++j) s.pos[j] = static_cast<float>(r.pos[j]);
            s.bar_z = BarZ(r.pos, r.bar);
            if (r.hp >= 0) s.hp = r.hp, s.hp_most = r.hp > s.hp_most ? r.hp : s.hp_most;
            if (r.sub >= 0) s.sub = r.sub, s.sub_most = r.sub > s.sub_most ? r.sub : s.sub_most;
            if (fresh && s.pending == kCeSpawn) put(Note(s, kCeSpawn, s.from_notify, now_ms));
        }
        for (unsigned i = 0; i < n;) {
            SendTrack& s = t[i];
            if (s.ended) {
                ++i;
                continue;
            }
            const CatchRec* r = nullptr;
            for (unsigned j = 0; recs && j < nrecs; ++j)
                if (recs[j].key == s.key) r = &recs[j];
            if (r && !r->inactive) {
                if (!s.in_recs && ++s.misses >= kCatchMissSteps + 8) {  // a record that never gets a position (about 3 s)
                    Remove(i);
                    continue;
                }
                ++i;
                continue;
            }
            if (!s.in_recs) {  // known from the notify only
                if (r || ++s.misses >= kCatchMissSteps) {
                    Remove(i);
                    continue;
                }
                ++i;
                continue;
            }
            s.ended = true, s.pending = kCeGone;
            put(Note(s, kCeGone, false, now_ms));
            ++i;
        }
        return k;
    }
    static CatchEntry Entry(const SendTrack& s) {
        CatchEntry e;
        e.key = s.key, e.class1 = s.class1, e.edge = s.pending;
        e.flags = static_cast<std::uint8_t>((s.entity && !s.ended ? kCfCombat : 0) | (s.pending == kCeAnnihilate ? kCfAnnihilating : 0) |
                                            (s.pending == kCeDefeat ? kCfDefeated : 0));
        std::memcpy(e.pos, s.pos, sizeof(e.pos));
        e.bar_z = s.bar_z;
        e.hp = Pct(s.hp, s.hp_most);
        e.sub = Pct(s.sub, s.sub_most);
        return e;
    }
    // The next message (m.n 0: nothing to send). *edges: an edge entry is in it.
    unsigned Build(BtsCatch& m, std::uint32_t sender_ms, bool* edges) {
        m = BtsCatch{};
        m.sender_ms = sender_ms;
        n_incl = 0;
        if (edges) *edges = false;
        for (unsigned i = 0; i < n;) {  // an ended track that never had a position: nothing to tell
            if (t[i].ended && !t[i].have_pos) Remove(i);
            else ++i;
        }
        auto add = [&](const SendTrack& s) {
            m.e[m.n++] = Entry(s);
            incl[n_incl++] = s.key;
        };
        for (unsigned i = 0; i < n && m.n < kBtsCatchMax; ++i)
            if (t[i].pending && t[i].have_pos) {
                add(t[i]);
                if (edges) *edges = true;
            }
        for (unsigned j = 0; j < n && m.n < kBtsCatchMax; ++j) {
            const SendTrack& s = t[(rot + j) % n];
            if (!s.pending && !s.ended && s.have_pos) add(s);
        }
        return m.n;
    }
    // The message Build made went: its edges are told, its ended tracks gone.
    void Sent() {
        for (unsigned k = 0; k < n_incl; ++k) {
            const int i = Find(incl[k]);
            if (i < 0) continue;
            if (t[i].ended) Remove(static_cast<unsigned>(i));
            else t[i].pending = kCeNone;
        }
        n_incl = 0;
        ++rot;
    }
};

// ---------------------------------------------------------------- the receiver (game thread)
// The partner's catchers by key (at most kViewMax; a new one past that replaces the stalest). Take: kVeNew (a key not seen:
// SPAWN or already up), kVeHp (health moved 10 points or more since the last line), kVeEnd (DEFEAT / ANNIHILATE / GONE: the key
// is dropped). Expire: no CATCH for a key for kMarkLifeMs (3 s): dropped quietly (not a GONE).
constexpr unsigned kViewMax = 8;
constexpr float kViewHpStep = 10.0f, kToastHpStep = 20.0f;
enum ViewEv : std::uint8_t { kVeNone = 0, kVeNew = 1, kVeHp = 2, kVeEnd = 3 };
struct ViewEntry {
    std::uint32_t key = 0;
    std::uint8_t class1 = 0, flags = 0;
    float pos[3] = {0, 0, 0}, bar_z = 0, hp = -1, sub = -1, logged_hp = -1, toast_hp = -1;
    double first_ms = 0, seen_ms = 0;
};
struct ViewOut {
    std::uint8_t ev = kVeNone, edge = kCeNone;
    bool known = false;  // kVeEnd: the key was in the view
    ViewEntry e;
    double age_ms = 0;  // kVeEnd: since its first CATCH here
};
inline bool EndEdge(std::uint8_t edge) { return edge == kCeDefeat || edge == kCeAnnihilate || edge == kCeGone; }
struct CatchView {
    ViewEntry v[kViewMax];
    unsigned n = 0;
    int Find(std::uint32_t key) const {
        for (unsigned i = 0; i < n; ++i)
            if (v[i].key == key) return static_cast<int>(i);
        return -1;
    }
    void Clear() { n = 0; }
    ViewOut Take(const CatchEntry& c, double now_ms) {
        ViewOut o;
        o.edge = c.edge;
        int k = Find(c.key);
        ViewEntry fresh;
        fresh.key = c.key, fresh.first_ms = now_ms;
        ViewEntry& e = k >= 0 ? v[k] : fresh;
        if (c.class1) e.class1 = c.class1;
        e.flags = c.flags;
        std::memcpy(e.pos, c.pos, sizeof(e.pos));
        e.bar_z = c.bar_z, e.hp = c.hp, e.sub = c.sub, e.seen_ms = now_ms;
        if (EndEdge(c.edge)) {
            o.ev = kVeEnd, o.known = k >= 0, o.e = e;
            o.age_ms = k >= 0 ? now_ms - e.first_ms : 0;
            if (k >= 0) v[k] = v[--n];
            return o;
        }
        if (k < 0) {
            fresh.logged_hp = c.hp;
            if (n < kViewMax) {
                v[n++] = fresh;
            } else {
                unsigned old = 0;
                for (unsigned i = 1; i < n; ++i)
                    if (v[i].seen_ms < v[old].seen_ms) old = i;
                v[old] = fresh;
            }
            o.ev = kVeNew, o.e = fresh;
            return o;
        }
        if (std::fabs(e.hp - e.logged_hp) >= kViewHpStep) {
            e.logged_hp = e.hp;
            o.ev = kVeHp;
        }
        o.e = e;
        return o;
    }
    unsigned Expire(double now_ms, ViewEntry* out, unsigned cap) {
        unsigned k = 0;
        for (unsigned i = 0; i < n;)
            if (now_ms - v[i].seen_ms > kMarkLifeMs) {
                if (out && k < cap) out[k] = v[i];
                ++k;
                v[i] = v[--n];
            } else {
                ++i;
            }
        return k;
    }
    // The one the marker and the rain spot follow: a fighting one (combat) before the rest, then the lowest health, then the oldest.
    const ViewEntry* Primary() const {
        const ViewEntry* p = nullptr;
        for (unsigned i = 0; i < n; ++i) {
            const ViewEntry& e = v[i];
            if (!p) {
                p = &e;
                continue;
            }
            const bool ec = (e.flags & kCfCombat) != 0, pc = (p->flags & kCfCombat) != 0;
            if (ec != pc) {
                if (ec) p = &e;
                continue;
            }
            const float eh = e.hp < 0 ? 101.0f : e.hp, ph = p->hp < 0 ? 101.0f : p->hp;
            if (eh < ph || (eh == ph && e.first_ms < p->first_ms)) p = &e;
        }
        return p;
    }
};
// The marker's anchor: bt_markers draws a catcher's mark kMarkOverM above the anchor, so the anchor is set that far below the bar.
constexpr float kMarkOverM = 3.0f;  // bt_markers.cpp kCatcherOverM
inline void MarkAnchor(const ViewEntry& e, double (&out)[3]) {
    out[0] = e.pos[0], out[1] = e.pos[1];
    out[2] = static_cast<double>(e.bar_z) - kMarkOverM;
}
// The HUD line with the markers off (design 2.8): "Your partner is fighting a catcher (lion) 63%".
inline void HudText(std::uint8_t class1, float hp, char* out, std::size_t cap) {
    if (!out || !cap) return;
    if (hp >= 0) std::snprintf(out, cap, "Your partner is fighting a catcher (%s) %.0f%%", CatcherLook(class1), static_cast<double>(hp));
    else std::snprintf(out, cap, "Your partner is fighting a catcher (%s)", CatcherLook(class1));
}
// A toast again only after the health fell kToastHpStep points since the last one (one at the start).
inline bool ToastAgain(float toast_hp, float hp) { return toast_hp < 0 || (hp >= 0 && toast_hp - hp >= kToastHpStep); }

// ---------------------------------------------------------------- bt_sync 10: the test catcher (the gate's leg C)
// A Quad (the lion) d m ahead of the sender's Sam: SPAWN 100 % at 0, 60 % at 1.5 s, 20 % at 3 s, GONE at 4.5 s, each entry
// flagged kCfTest, sent at 4 Hz (each edge until a send takes it).
struct CatchTestStep {
    double at_ms;
    float hp;
    std::uint8_t edge;
};
constexpr CatchTestStep kCatchTest[] = {{0, 100, kCeSpawn}, {1500, 60, kCeNone}, {3000, 20, kCeNone}, {4500, 20, kCeGone}};
constexpr unsigned kCatchTestSteps = sizeof(kCatchTest) / sizeof(kCatchTest[0]);
constexpr std::uint8_t kCatchTestClass1 = 2;  // Quad (the lion)
constexpr float kCatchTestBarM = 7.0f;        // PF 18: the bar anchor 7 m above the root
constexpr double kCatchTestAheadM = 20.0;
inline int CatchTestAt(double t_ms) {
    int k = -1;
    for (unsigned i = 0; i < kCatchTestSteps; ++i)
        if (t_ms >= kCatchTest[i].at_ms) k = static_cast<int>(i);
    return k;
}
// The distance argument (v[1]): 0, NaN or out of 1..500 -> 20 m.
inline double CatchTestAhead(double v) { return std::isfinite(v) && v >= 1 && v <= 500 ? v : kCatchTestAheadM; }

}  // namespace st::btrules::catcher
