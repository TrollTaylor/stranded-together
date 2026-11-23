// The self view in the private room, the pure part (native/mod/room_outfit_rules.hpp): the room_outfit setting, Sam's
// body variant -> her look, the sticky room rule, when her body may change, the clothes-change window and the CRC-32C
// of the game's named flags. No game.
#include <cstdint>
#include <cstdio>

#include "room_outfit_rules.hpp"

using namespace st::roomoutfit;

namespace {
int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

void TestChoice() {
    Choice c = Choice::suit;
    CHECK(ParseChoice("black", &c) && c == Choice::black);
    CHECK(ParseChoice("suit", &c) && c == Choice::suit);
    CHECK(ParseChoice("fragile", &c) && c == Choice::black);
    CHECK(ParseChoice("1", &c) && c == Choice::black);
    CHECK(ParseChoice("porter", &c) && c == Choice::suit);
    CHECK(ParseChoice("off", &c) && c == Choice::suit);
    CHECK(ParseChoice("0", &c) && c == Choice::suit);
    c = Choice::black;
    CHECK(!ParseChoice("leather", &c) && c == Choice::black);
    CHECK(!ParseChoice("", &c));
    CHECK(!ParseChoice(nullptr, &c));
    CHECK(std::strcmp(ChoiceName(Choice::black), "black") == 0 && std::strcmp(ChoiceName(Choice::suit), "suit") == 0);
    CHECK(RoomBodyFor(kBodyFragileSuit, Choice::black) == kBodyFragile);
    CHECK(RoomBodyFor(kBodyFragileSuit, Choice::suit) == 0);
    CHECK(RoomBodyFor(0, Choice::black) == 0);             // Sam
    CHECK(RoomBodyFor(kBodyFragile, Choice::black) == 0);  // already her black suit
}

void TestVariant() {
    CHECK(!ClothesBusy(0, 0, 0));
    CHECK(ClothesBusy(0, 0, 1));   // wanted, the stream not started yet
    CHECK(ClothesBusy(1, 0, 1));
    CHECK(ClothesBusy(2, 0, 1));
    CHECK(ClothesBusy(3, 1, 1));   // the paint wait: +0x74 is already the new one
    CHECK(EffectiveVariant(0, 1, 1) == 1);
    CHECK(EffectiveVariant(0, 1, 2) == 1);
    CHECK(EffectiveVariant(1, 1, 3) == 1);
    CHECK(EffectiveVariant(0, 0, 0) == 0);
    CHECK(EffectiveVariant(0, 1, 0) == 1);
    CHECK(EffectiveVariant(0, 1, 7) == kVariantUnknown);
    CHECK(EffectiveVariant(0, 1, -1) == kVariantUnknown);
    CHECK(LookForVariant(1) == Look::room && LookForVariant(5) == Look::room);
    CHECK(LookForVariant(0) == Look::base && LookForVariant(2) == Look::base && LookForVariant(3) == Look::base && LookForVariant(6) == Look::base);
    CHECK(LookForVariant(4) == Look::keep && LookForVariant(7) == Look::keep && LookForVariant(0xFF) == Look::keep);
}

LookInput In(bool room, std::uint8_t cur, std::uint8_t wanted, int state, std::uint8_t current = kBodyFragileSuit) {
    LookInput in;
    in.base_id = kBodyFragileSuit;
    in.in_private_room = room;
    in.variant_ok = true;
    in.cur = cur, in.wanted = wanted, in.state = state;
    in.current_id = current;
    return in;
}

void TestWanted() {
    // The field: the suit.
    CHECK(WantedBodyId(In(false, 0, 0, 0)) == kBodyFragileSuit);
    // The bed scene before the room load (PrivateRoom set, still the porter suit): the room look, swapped when hidden.
    CHECK(WantedBodyId(In(true, 0, 0, 0)) == kBodyFragile);
    // In the room in his room clothes.
    CHECK(WantedBodyId(In(true, 1, 1, 0, kBodyFragile)) == kBodyFragile);
    // The uniform menu's preview (variant 0 in the room): sticky, no swap.
    CHECK(WantedBodyId(In(true, 1, 0, 1, kBodyFragile)) == kBodyFragile);
    CHECK(WantedBodyId(In(true, 0, 0, 0, kBodyFragile)) == kBodyFragile);
    // Leaving: the flag cleared in the exit load while he still wears the room clothes, then the switch to the suit.
    CHECK(WantedBodyId(In(false, 1, 1, 0, kBodyFragile)) == kBodyFragile);
    CHECK(WantedBodyId(In(false, 1, 0, 1, kBodyFragile)) == kBodyFragileSuit);
    CHECK(WantedBodyId(In(false, 0, 0, 3, kBodyFragile)) == kBodyFragileSuit);
    // A switch to the room clothes in the field (the test request): the room look from its start.
    CHECK(WantedBodyId(In(false, 0, 1, 0)) == kBodyFragile);
    CHECK(WantedBodyId(In(false, 0, 1, 2)) == kBodyFragile);
    // Casual wear: the room look; naked / skeleton only / unknown: whatever is worn.
    CHECK(WantedBodyId(In(false, 5, 5, 0)) == kBodyFragile);
    CHECK(WantedBodyId(In(false, 4, 4, 0)) == kBodyFragileSuit);
    CHECK(WantedBodyId(In(false, 4, 4, 0, kBodyFragile)) == kBodyFragile);
    CHECK(WantedBodyId(In(false, 7, 7, 0, 0)) == kBodyFragileSuit);  // nothing worn yet: the base
    // The variant unreadable: the flag alone decides, else what is worn.
    LookInput u = In(false, 1, 1, 0, kBodyFragile);
    u.variant_ok = false;
    CHECK(WantedBodyId(u) == kBodyFragile);
    u.current_id = kBodyFragileSuit;
    CHECK(WantedBodyId(u) == kBodyFragileSuit);
    u.in_private_room = true;
    CHECK(WantedBodyId(u) == kBodyFragile);
    // room_outfit=suit: never the black suit.
    LookInput s = In(true, 1, 1, 0);
    s.choice = Choice::suit;
    CHECK(WantedBodyId(s) == kBodyFragileSuit);
    // Another character (Sam, the host): no room body.
    LookInput sam = In(true, 1, 1, 0, 0);
    sam.base_id = 0;
    CHECK(WantedBodyId(sam) == 0);
    // self_view 18: off / force room (even with room_outfit=suit) / force base.
    LookInput f = In(false, 0, 0, 0);
    f.force = Force::room;
    CHECK(WantedBodyId(f) == kBodyFragile);
    f.choice = Choice::suit;
    CHECK(WantedBodyId(f) == kBodyFragile);
    f = In(true, 1, 1, 0);
    f.force = Force::base;
    CHECK(WantedBodyId(f) == kBodyFragileSuit);
    f.force = Force::off;
    CHECK(WantedBodyId(f) == kBodyFragileSuit);
    LookInput fs = sam;
    fs.force = Force::room;
    CHECK(WantedBodyId(fs) == 0);  // Sam has no room body even when forced
}

void TestSwap() {
    CHECK(SwapDecision(false, 0, true, false) == Swap::hidden);  // not shown: at once, even in a cutscene
    CHECK(SwapDecision(false, 0, false, false) == Swap::hidden);
    CHECK(SwapDecision(true, 10000, true, false) == Swap::wait);  // a cutscene, a menu: never visibly
    CHECK(SwapDecision(true, 10000, true, true) == Swap::wait);
    CHECK(SwapDecision(true, 0, false, false) == Swap::wait);
    CHECK(SwapDecision(true, kVisibleSwapAfterMs - 1, false, false) == Swap::wait);
    CHECK(SwapDecision(true, kVisibleSwapAfterMs, false, false) == Swap::visible);
    CHECK(SwapDecision(true, 0, false, true) == Swap::visible);
}

void TestClothes() {
    ClothesWatch w;
    CHECK(!ClothesHold(w, 0));
    CHECK(ClothesStep(w, true, 0, 0, 0, 100) == ClothesEvent::none);
    CHECK(ClothesStep(w, true, 0, 0, 1, 200) == ClothesEvent::start);
    CHECK(w.busy && w.from_variant == 0 && w.to_variant == 1 && w.from_ms == 200);
    CHECK(ClothesHold(w, 210));
    CHECK(ClothesStep(w, true, 0, 0, 1, 210) == ClothesEvent::none);
    CHECK(ClothesStep(w, true, 1, 0, 1, 220) == ClothesEvent::step);
    CHECK(ClothesStep(w, true, 2, 0, 1, 900) == ClothesEvent::step);
    CHECK(ClothesStep(w, true, 3, 1, 1, 910) == ClothesEvent::step);
    CHECK(ClothesStep(w, true, 3, 1, 1, 1500) == ClothesEvent::none);
    CHECK(ClothesStep(w, true, 0, 1, 1, 2200) == ClothesEvent::end);
    CHECK(!w.busy && w.last_ms == 2000 && w.idle_from_ms == 2200);
    CHECK(ClothesHold(w, 2200 + kClothesIdleMs - 1));
    CHECK(!ClothesHold(w, 2200 + kClothesIdleMs));
    // The wardrobe: two switches back to back (1 -> 0, then 0 -> 1): the hold covers the gap.
    CHECK(ClothesStep(w, true, 0, 1, 0, 2300) == ClothesEvent::start);
    CHECK(ClothesStep(w, true, 0, 0, 0, 4000) == ClothesEvent::end);
    CHECK(ClothesStep(w, true, 0, 0, 1, 4200) == ClothesEvent::start);
    CHECK(ClothesHold(w, 4200));
    // Unreadable (the rule switched off, the player gone): a running switch ends.
    CHECK(ClothesStep(w, false, 0, 0, 0, 5000) == ClothesEvent::end);
    CHECK(!w.busy && w.state == -1);
    CHECK(ClothesStep(w, false, 1, 0, 1, 5100) == ClothesEvent::none);
    // The state 3 -> 0 "mismatch" path (wanted changed during the wait): busy again at once.
    ClothesWatch m;
    CHECK(ClothesStep(m, true, 3, 1, 1, 0) == ClothesEvent::start);
    CHECK(ClothesStep(m, true, 0, 1, 0, 10) == ClothesEvent::step && m.to_variant == 0);
}

void TestRules() {
    static_assert(kDefaultRules == 5, "the load rule and the clothes rule");
    static_assert((kDefaultRules & kRuleHold) == 0, "the close-up hold is retired");
    int rules = kDefaultRules;
    CHECK((rules & kRuleLoad) && (rules & kRuleClothes) && !(rules & kRuleHold));
    // the clothes rule's byte checks failed: the hold and the stall guard come back (v0.25, not v0.24's black screen)
    CHECK(EffectiveRules(kDefaultRules, true) == kDefaultRules);
    CHECK(EffectiveRules(kDefaultRules, false) == (kDefaultRules | kRuleHold));
    CHECK(EffectiveRules(kRuleClothes, false) == (kRuleClothes | kRuleHold));
    // the clothes rule not asked for (3 v0.25, 1, 0 v0.24): left as it is
    CHECK(EffectiveRules(kRuleLoad | kRuleHold, false) == (kRuleLoad | kRuleHold));
    CHECK(EffectiveRules(kRuleLoad, false) == kRuleLoad);
    CHECK(EffectiveRules(0, false) == 0);
    CHECK(EffectiveRules(kRuleLoad | kRuleHold | kRuleClothes, false) == (kRuleLoad | kRuleHold | kRuleClothes));
}

void TestCrc() {
    // The standard CRC-32C check value (init and final xor ~0): "123456789" -> 0xE3069283.
    CHECK(~Crc32c("123456789", 0xFFFFFFFFu) == 0xE3069283u);
    // The game's form (from 0, no final xor): the empty string is 0; one byte 0x01 is the table's entry 1.
    CHECK(Crc32c("") == 0);
    CHECK(Crc32c(nullptr) == 0);
    CHECK(Crc32c("\x01") == 0xF26B8303u);
}
}  // namespace

int main() {
    TestChoice();
    TestVariant();
    TestWanted();
    TestSwap();
    TestClothes();
    TestRules();
    TestCrc();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
