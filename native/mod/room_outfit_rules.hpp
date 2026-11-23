// The self view in the private room (docs/real-partner-body.md section 34), the pure part: when the game is changing
// Sam's clothes (his body variant switch), which body the guest's Fragile wears (her porter suit, or her black suit in
// the private room) and when a change of body may happen. No game memory, no Windows: unit-tested in
// tests/room_outfit_tests.cpp.
//
// Sam's look is one of 8 body variants (DSPlayerStreamingControlComponentResource.Variants): 0 suit (the porter suit),
// 1 private (the private room's clothes), 2 suit_mule, 3 suit_cdt, 4 naked, 5 wear (casual), 6 suit_beach,
// 7 skeleton_only. His streaming control (player +0x54F8, update 0x1427CAAE0) keeps:
//   +0x70 the switch state: 0 idle, 1 streaming the new variant, 2 apply (+0x74 = +0x75, the model and the inventory
//         swapped), 3 wait (1 s, then until his paint effect reports ready, or 120 s);
//   +0x74 the current variant, +0x75 the wanted one (state 0 rewrites it every tick from the request +0x76, or from a
//         node-graph variable when +0x76 is 8), +0x76 the request.
// The paint wait never ends while Sam is hidden (reason 63): the paint effect's init only progresses while he is drawn,
// and its own 60 s fallback releases it. That was the private room's 60 s black screen (round 10). So: while the game
// is changing his clothes, the self view shows Sam (the game's own fade covers it), and the body comes back after
// 0.5 s of quiet.
#pragma once
#include <cstdint>
#include <cstring>

namespace st::roomoutfit {

// ---------------------------------------------------------------- the setting (session.ini room_outfit)
// black (default): Fragile wears her black suit (demo_fgl_new, body 3 "fragile") in the private room; suit: her porter
// suit there too (the v0.25 look).
enum class Choice : std::uint8_t { black = 0, suit = 1 };

// "black" / "fragile" / "1" and "suit" / "porter" / "off" / "0". False for anything else.
inline bool ParseChoice(const char* s, Choice* out) {
    if (!s || !out) return false;
    if (std::strcmp(s, "black") == 0 || std::strcmp(s, "fragile") == 0 || std::strcmp(s, "1") == 0) return *out = Choice::black, true;
    if (std::strcmp(s, "suit") == 0 || std::strcmp(s, "porter") == 0 || std::strcmp(s, "off") == 0 || std::strcmp(s, "0") == 0)
        return *out = Choice::suit, true;
    return false;
}
inline const char* ChoiceName(Choice c) { return c == Choice::black ? "black" : "suit"; }

// Body table ids (body_table.cpp).
inline constexpr std::uint8_t kBodyFragile = 3;       // demo_fgl_new: her black Fragile Express suit
inline constexpr std::uint8_t kBodyFragileSuit = 16;  // demo_fgl_body_suiti: the porter suit

// The room body for a base body, 0 when it has none (only her porter suit has one).
inline std::uint8_t RoomBodyFor(std::uint8_t base_id, Choice c) {
    return base_id == kBodyFragileSuit && c == Choice::black ? kBodyFragile : 0;
}

// ---------------------------------------------------------------- Sam's body variant
inline constexpr std::uint8_t kVariantSuit = 0, kVariantPrivate = 1, kVariantWear = 5, kVariantGraph = 8;
inline constexpr std::uint8_t kVariantUnknown = 0xFF;
inline constexpr int kStateIdle = 0, kStateStream = 1, kStateApply = 2, kStateWait = 3;

// The game is changing Sam's clothes: a switch runs (+0x70 != 0) or one is wanted (+0x75 != +0x74).
inline bool ClothesBusy(int state, std::uint8_t cur, std::uint8_t wanted) { return state != kStateIdle || wanted != cur; }

// The variant Sam is on his way to: the wanted one until the switch applied it (state 3 has +0x74 = the new one).
inline std::uint8_t EffectiveVariant(std::uint8_t cur, std::uint8_t wanted, int state) {
    if (state < kStateIdle || state > kStateWait) return kVariantUnknown;
    return state == kStateWait ? cur : wanted;
}

enum class Look : std::uint8_t { base = 0, room = 1, keep = 2 };
// Outside the room: the private clothes and the casual wear are the room look; the suits are the base look; naked
// (the onsen), skeleton only (init) and unknown keep whatever is worn.
inline Look LookForVariant(std::uint8_t v) {
    switch (v) {
        case kVariantPrivate:
        case kVariantWear: return Look::room;
        case kVariantSuit:
        case 2:
        case 3:
        case 6: return Look::base;
        default: return Look::keep;
    }
}

// `self_view 18 <mode>` (tests): 0 never the room body, 1 the rule (default), 2 always the room body, 3 always the base.
enum class Force : std::uint8_t { off = 0, rule = 1, room = 2, base = 3 };

struct LookInput {
    std::uint8_t base_id = 0;           // the configured character (my_character)
    Choice choice = Choice::black;      // session.ini room_outfit
    Force force = Force::rule;
    bool in_private_room = false;       // the DSGameState flag PrivateRoom (0x8), current or requested
    bool variant_ok = false;            // the variant fields were read (byte checks passed, the player checked out)
    std::uint8_t cur = kVariantUnknown, wanted = kVariantUnknown;
    int state = -1;
    std::uint8_t current_id = 0;        // the body worn now (0: none yet)
};

// The body id the self view (and STJ1 body_id) wants now. Sticky in the room: the uniform menu's preview switches Sam
// to the porter suit (variant 0) while PrivateRoom stays set, and that must not swap her body twice.
inline std::uint8_t WantedBodyId(const LookInput& in) {
    const std::uint8_t room = RoomBodyFor(in.base_id, in.force == Force::room ? Choice::black : in.choice);
    if (!room || in.force == Force::off || in.force == Force::base) return in.base_id;
    if (in.force == Force::room || in.in_private_room) return room;
    const Look l = in.variant_ok ? LookForVariant(EffectiveVariant(in.cur, in.wanted, in.state)) : Look::keep;
    if (l == Look::room) return room;
    if (l == Look::base) return in.base_id;
    return in.current_id == room ? room : in.base_id;
}

// ---------------------------------------------------------------- when her body may change
inline constexpr double kVisibleSwapAfterMs = 3000;  // a wanted change that found no hidden moment this long: done visibly
enum class Swap : std::uint8_t { hidden = 0, wait = 1, visible = 2 };
// `shown`: the self body is shown now (a change then shows Sam for about 0.12 s); `blocked`: a cutscene, a menu, the
// pause menu or the subspace (never a visible change there); `forced`: self_view 18 2 / 3 (tests: at once).
inline Swap SwapDecision(bool shown, double differs_for_ms, bool blocked, bool forced) {
    if (!shown) return Swap::hidden;
    if (blocked) return Swap::wait;
    return forced || differs_for_ms >= kVisibleSwapAfterMs ? Swap::visible : Swap::wait;
}

// ---------------------------------------------------------------- the clothes-change window
inline constexpr double kClothesIdleMs = 500;  // the body is shown again only after this long without a switch

struct ClothesWatch {
    bool busy = false;
    double from_ms = 0;               // the switch started
    double idle_from_ms = -1e18;      // the last switch ended
    std::uint8_t from_variant = kVariantUnknown, to_variant = kVariantUnknown;
    int state = -1;                   // the last state seen while busy (a step is logged on change)
    double last_ms = 0;               // the last switch's duration (ms)
};
enum class ClothesEvent : std::uint8_t { none = 0, start = 1, step = 2, end = 3 };

// One tick. `read_ok` false (unreadable, the rule off): a running switch ends there.
inline ClothesEvent ClothesStep(ClothesWatch& w, bool read_ok, int state, std::uint8_t cur, std::uint8_t wanted, double now_ms) {
    const bool b = read_ok && ClothesBusy(state, cur, wanted);
    if (b && !w.busy) {
        w.busy = true;
        w.from_ms = now_ms;
        w.from_variant = cur;
        w.to_variant = wanted;
        w.state = state;
        return ClothesEvent::start;
    }
    if (!b && w.busy) {
        w.busy = false;
        w.idle_from_ms = now_ms;
        w.last_ms = now_ms - w.from_ms;
        w.state = read_ok ? state : -1;
        return ClothesEvent::end;
    }
    if (b && state != w.state) {
        w.state = state;
        if (wanted != w.to_variant) w.to_variant = wanted;
        return ClothesEvent::step;
    }
    return ClothesEvent::none;
}
// The body stays off: a switch runs, or one ended less than kClothesIdleMs ago.
inline bool ClothesHold(const ClothesWatch& w, double now_ms) { return w.busy || now_ms - w.idle_from_ms < kClothesIdleMs; }

// ---------------------------------------------------------------- `self_view 17 <mask>` (before / after runs)
// bit 1 the load rule and the fresh show (section 33), bit 2 the private room's close-up hold and the stall guard
// (section 33, retired in round 11: off by default), bit 4 the clothes rule (section 34).
inline constexpr int kRuleLoad = 1, kRuleHold = 2, kRuleClothes = 4;
inline constexpr int kDefaultRules = kRuleLoad | kRuleClothes;

// The rules in force: when the clothes rule is asked for but its byte checks failed (another game build), the close-up
// hold and the stall guard come back, so the private room falls back to v0.25's behaviour, never to v0.24's 60 s black
// screen.
inline int EffectiveRules(int rules, bool clothes_available) {
    return (rules & kRuleClothes) && !clothes_available ? rules | kRuleHold : rules;
}

// ---------------------------------------------------------------- DSPlayerLoadingModel (diagnostics)
// The game's named-flag table (0x1422AF7B0) keys a name by its CRC-32C (the SSE4.2 crc32 instruction from 0, no final
// xor) with bit 31 set.
inline std::uint32_t Crc32c(const char* s, std::uint32_t init = 0) {
    std::uint32_t c = init;
    for (; s && *s; ++s) {
        c ^= static_cast<std::uint8_t>(*s);
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
    }
    return c;
}

}  // namespace st::roomoutfit
