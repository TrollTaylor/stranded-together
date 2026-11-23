// Gear kept across loads and the private room (docs/real-partner-body.md section 35), the pure part: whether Sam's BB pod
// is shown while the self view hides him, and which gear items he wore before the private room are not back after it.
// No game memory, no Windows: unit-tested in tests/gear_keep_tests.cpp. The game side is partner_gear.cpp (ReadItems,
// PodTick, RoomRegainTick).
//
// The BB pod (round 11, the user's "cargo disappeared"): the game shows the pod entity from its mesh controller's slot 0
// (Sam +0xA60, the attachment update 0x1426C7760, the slot at +0x2100, its part index k at +0x2108 (1 in run
// 20261123-094135), its entity reference +0x2118, the one 0x1426A0620 answers EDSPlayerMesh 30 BBPod from). Per tick
// (0x1426C7B0B..0x1426C7BB9):
//   hidden = slot +0x310 bit 0 || !(Sam +0x88 bit 1, his own visible bit)
//            || (the map +0x38 + 8k != -1 && the ArtParts part is not shown) || +0x1220 || +0x700
//   (+0x1220 / +0x700: two fixed fields the rule reads, meaning unknown: index 119 of the forced-hide / mask arrays by
//   ReadMeshCtl's layout, past the 0x59 parts, so not mesh 30's)
//   visible = (inventory +0x4C7E bit 4 && inventory +0x4C52) ? !hidden : 1          (0x14235F640 sets entity +0x88 bit 1)
// The ArtParts part is shown as ApplyMeshControl 0x1426D9120 decides (exo_parts.hpp MeshAsked), and that test sees our
// self view's bit 63 of the global mask: under the self view the pod reads hidden. Before round 11 the pod's state from
// the last moment the self view was off stood, and that moment was almost always a load or the private room, where the
// game has it hidden: it latched hidden on both screens until the session ended.
#pragma once
#include <cstdint>

namespace st::gearkeep {

// ---------------------------------------------------------------- the BB pod
// What the game's slot-0 rule reads (part_asked: MeshAsked with our bit 63 ignored).
struct PodRead {
    bool gate_ready = true;    // inventory +0x4C7E bit 4
    bool gate_on = true;       // inventory +0x4C52 != 0
    bool slot_hidden = false;  // slot 0 +0x310 bit 0
    bool sam_visible = true;   // Sam +0x88 bit 1
    bool part_mapped = false;  // the controller's map for the slot's part k is not -1
    bool part_asked = true;    // the controller asks for part k, our self view's bit ignored
    bool forced_hide = false;  // +0x1220 != 0 (a fixed field the rule reads; not mesh 30's forced hide)
    bool mask_k = false;       // +0x700 != 0 (likewise)
};

// The pod as the game would show it without our self view.
inline bool PodAsked(const PodRead& r) {
    if (!r.gate_ready || !r.gate_on) return true;
    if (r.slot_hidden || !r.sam_visible) return false;
    if (r.part_mapped && !r.part_asked) return false;
    return !r.forced_hide && !r.mask_k;
}

// Where the pod's state comes from: its own flag (self view off), the game's rule (above), the last plain-play reading
// (the fallback), or "as normally worn" (the fallback after an off window without plain play: a load, the room); in the
// private room the fallback is hidden (BB is in the incubator there and the game hides the pod).
enum class PodSource : std::uint8_t { flag = 0, ask = 1, memo = 2, worn = 3, room = 4 };
inline const char* PodSourceName(PodSource s) {
    switch (s) {
        case PodSource::flag: return "its own flag (the self view is off)";
        case PodSource::ask: return "the game's rule for Sam's pod";
        case PodSource::memo: return "the last plain-play reading";
        case PodSource::worn: return "as worn (no plain-play reading since the self view came back)";
        case PodSource::room: return "hidden in the private room (BB is in the incubator)";
    }
    return "?";
}

struct PodTick {
    bool self_shown = false;  // our self view hides Sam now: the pod's own flag reads hidden
    bool have_ask = false;    // the game's rule was read (sites verified, `gear 30 0`)
    bool ask = false;         // PodAsked
    bool flag = false;        // the pod entity's own visible bit
    bool plain = false;       // plain play: no load or post-load setup, not the private room, a cutscene or a menu
    bool in_room = false;     // the private room's flags (current or requested)
    double now_ms = 0;
};

// A mismatch between the rule and the flag in plain play for this long (the self view off) disproves the rule for the
// session: a transition's one-tick lag never does.
inline constexpr double kPodDisproveMs = 2000;

enum class PodEvent : std::uint8_t { none = 0, mismatch_started, mismatch_ended, disproved };

struct PodState {
    bool memo = true;               // the fallback: the last plain-play reading
    bool memo_plain = false;        // memo was read in plain play since the self view went off
    bool was_self = false;
    bool disproved = false;         // the rule disagreed with the flag in plain play: the fallback for the session
    bool mismatch = false;          // the rule and the flag disagree now (self view off)
    double plain_mismatch_ms = -1;  // when the plain-play mismatch began (-1 none)
    std::uint32_t compared = 0, matched = 0, mismatched = 0;
};

struct PodOut {
    bool vis = false;
    PodSource src = PodSource::flag;
    PodEvent ev = PodEvent::none;
};

inline PodOut DecidePod(PodState& s, const PodTick& t) {
    PodOut o;
    if (!t.self_shown) {
        if (s.was_self) s.memo_plain = false;  // a new off window
        s.was_self = false;
        o.vis = t.flag;
        o.src = PodSource::flag;
        if (t.plain) s.memo = t.flag, s.memo_plain = true;
        if (t.have_ask) {
            ++s.compared;
            const bool mis = t.ask != t.flag;
            if (mis) ++s.mismatched;
            else ++s.matched;
            if (mis && !s.mismatch) o.ev = PodEvent::mismatch_started;
            if (!mis && s.mismatch) o.ev = PodEvent::mismatch_ended;
            s.mismatch = mis;
            if (!mis || !t.plain) s.plain_mismatch_ms = -1;
            else if (s.plain_mismatch_ms < 0) s.plain_mismatch_ms = t.now_ms;
            if (mis && t.plain && !s.disproved && t.now_ms - s.plain_mismatch_ms >= kPodDisproveMs) {
                s.disproved = true;
                o.ev = PodEvent::disproved;
            }
        } else if (s.mismatch) {
            s.mismatch = false;
            s.plain_mismatch_ms = -1;
        }
        return o;
    }
    if (!s.was_self) {
        // the self view came back: without a plain-play reading in the off window (a load, the room) the pod is taken as worn
        if (!s.memo_plain) s.memo = true;
        s.was_self = true;
        s.mismatch = false;
        s.plain_mismatch_ms = -1;
    }
    if (t.have_ask && !s.disproved) {
        o.vis = t.ask;
        o.src = PodSource::ask;
    } else if (t.in_room) {
        o.vis = false;
        o.src = PodSource::room;
    } else {
        o.vis = s.memo;
        o.src = s.memo_plain ? PodSource::memo : PodSource::worn;
    }
    return o;
}

// ---------------------------------------------------------------- what the private room took (the cap case)
// The items Sam wore in plain play before the private room (counted per key: gear resource ids, the BB pod, the backpack,
// the Odradek) against what he wears once he has left it and play is plain again for kRegainSettleMs (at least
// kRegainWaitMs after leaving). The game takes some off in the room (the cap with the private clothes, round 10) and
// does not always put them back; Fragile mirrors Sam, so on both screens they are gone too. The log names them.
inline constexpr double kRegainWaitMs = 10000, kRegainSettleMs = 5000;

enum class RegainEvent : std::uint8_t { none = 0, entered, report };

template <unsigned N>
struct RoomRegain {
    enum class Phase : std::uint8_t { field, in_room, left } phase = Phase::field;
    int before[N] = {};
    bool have_before = false;  // a plain-play reading before the room
    double left_ms = -1, settled_ms = -1;
};

// One read: in_room (the PrivateRoom flag, current or requested), plain (as PodTick::plain without the room), counts
// (now). On report, missing[k] = how many fewer of key k than before the room (0 or more), *n_missing their sum; before
// is then what was read before the room.
template <unsigned N>
RegainEvent RegainTick(RoomRegain<N>& s, bool in_room, bool plain, double now_ms, const int (&counts)[N], int (&missing)[N],
                       int* n_missing) {
    using P = typename RoomRegain<N>::Phase;
    if (n_missing) *n_missing = 0;
    if (in_room) {
        const bool entering = s.phase == P::field;
        s.phase = P::in_room;
        return entering ? RegainEvent::entered : RegainEvent::none;
    }
    if (s.phase == P::in_room) {
        s.phase = P::left;
        s.left_ms = now_ms;
        s.settled_ms = -1;
    }
    if (s.phase == P::left) {
        if (!plain) {
            s.settled_ms = -1;
            return RegainEvent::none;
        }
        if (s.settled_ms < 0) s.settled_ms = now_ms;
        if (now_ms - s.left_ms < kRegainWaitMs || now_ms - s.settled_ms < kRegainSettleMs) return RegainEvent::none;
        int sum = 0;
        for (unsigned k = 0; k < N; ++k) {
            const int d = s.have_before && s.before[k] > counts[k] ? s.before[k] - counts[k] : 0;
            missing[k] = d;
            sum += d;
        }
        if (n_missing) *n_missing = sum;
        s.phase = P::field;
        return RegainEvent::report;  // before stays until the next plain read replaces it (the caller logs it first)
    }
    if (plain) {
        for (unsigned k = 0; k < N; ++k) s.before[k] = counts[k];
        s.have_before = true;
    }
    return RegainEvent::none;
}

}  // namespace st::gearkeep
