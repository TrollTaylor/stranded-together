// The carried cargo on the other screen, the pure part (native/mod/cargo_rules.hpp): which carried pieces are sent with which
// model byte, and what the receiver draws for it (docs/real-partner-body.md section 39). No game.
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "cargo_rules.hpp"

using namespace st::cargorules;

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

void TestModels() {
    // v0.7's mapping, unchanged
    CHECK(ModelFromCase(0, 0, 0) == 1);
    CHECK(ModelFromCase(1, 0, 2) == 6);
    CHECK(ModelFromCase(0, 0, 5) == 3);
    CHECK(ModelFromCase(0, 1, 0) == 13);
    CHECK(ModelFromCase(1, 2, 0) == 15);
    CHECK(ModelFromCase(1, 2, 1) == 16);
    CHECK(ModelFromCase(1, 2, 3) == 17);
    CHECK(ModelFromCase(0, 9, 3) == 21);
    CHECK(ModelFromCase(0, 12, 0) == 14);
    CHECK(ModelFromCase(0, 17, 0) == 27);
    CHECK(ModelFromCase(0, 18, 0) == 30);
    CHECK(ModelFromCase(1, 4, 0) == kNone);
    CHECK(DeliveryCase(0) && DeliveryCase(1) && DeliveryCase(9) && DeliveryCase(13) && DeliveryCase(17) && DeliveryCase(18));
    CHECK(!DeliveryCase(2) && !DeliveryCase(4) && !DeliveryCase(12) && !DeliveryCase(10));
}

void TestSend() {
    // the user's complete-100 waist: slot type 4 a weapon case (case 2, volume 0), slot type 5 the ladder (case 12)
    CHECK(SendModel(4, 1, 2, 0, true) == (kExact | 15));
    CHECK(SendModel(5, 1, 12, 0, true) == (kExact | 14));
    CHECK(SendModel(5, 1, 2, 1, true) == (kExact | 16));
    CHECK(SendModel(4, 1, 2, 2, true) == (kExact | 17));
    // off: the waist is not sent (v0.28)
    CHECK(SendModel(4, 1, 2, 0, false) == kNone);
    CHECK(SendModel(5, 1, 12, 0, false) == kNone);
    // the same pieces on the back or the arms: not sent (the game keeps them in the backpack's pouches)
    CHECK(SendModel(1, 1, 2, 0, true) == kNone);
    CHECK(SendModel(2, 1, 4, 0, true) == kNone);
    CHECK(SendModel(1, 1, 4, 0, true) == kNone);
    // equipment at the hip: no model known, not sent
    CHECK(SendModel(4, 1, 4, 0, true) == kNone);
    // deliveries: their cutscene prop model, anywhere, with the setting on or off
    CHECK(SendModel(1, 0, 0, 1, true) == 2);
    CHECK(SendModel(4, 0, 0, 1, true) == 2);
    CHECK(SendModel(6, 0, 9, 0, false) == 18);
    CHECK(SendModel(1, 0, 1, 0, true) == 13);
}

void TestReceive() {
    CHECK(ExactModel(kExact | 15) == 15);
    CHECK(ExactModel(kExact | 14) == 14);
    CHECK(ExactModel(kExact | 0) == 0);
    CHECK(ExactModel(kExact | 30) == 30);
    CHECK(ExactModel(kExact | 31) == -1);
    CHECK(ExactModel(0xFE) == -1);
    CHECK(ExactModel(kNone) == -1);
    CHECK(ExactModel(15) == -1);
    CHECK(ExactModel(0) == -1);
    CHECK(ExactModel(30) == -1);
    // every sent waist value round-trips, and is >= 31 (an older receiver's "no prop")
    for (std::uint8_t k : {std::uint8_t{2}, std::uint8_t{12}})
        for (std::uint8_t v = 0; v < 6; ++v) {
            const std::uint8_t m = SendModel(4, 1, k, v, true);
            CHECK(m != kNone && m >= 31);
            CHECK(ExactModel(m) == WaistModel(k, v));
        }
    // what the receiver draws (PropForKind): an exact model only with waist_cargo on; v0.7's cutscene cases and the
    // unknown case unchanged either way; any other value nothing
    CHECK(ReceiverDraw(kExact | 15, true) == Draw::exact);
    CHECK(ReceiverDraw(kExact | 14, true) == Draw::exact);
    CHECK(ReceiverDraw(kExact | 15, false) == Draw::none);
    CHECK(ReceiverDraw(kExact | 14, false) == Draw::none);
    for (bool on : {false, true}) {
        CHECK(ReceiverDraw(0, on) == Draw::cutscene);
        CHECK(ReceiverDraw(15, on) == Draw::cutscene);
        CHECK(ReceiverDraw(30, on) == Draw::cutscene);
        CHECK(ReceiverDraw(kNone, on) == Draw::unknown);
        CHECK(ReceiverDraw(31, on) == Draw::none);
        CHECK(ReceiverDraw(0x7F, on) == Draw::none);
        CHECK(ReceiverDraw(kExact | 31, on) == Draw::none);
        CHECK(ReceiverDraw(0xFE, on) == Draw::none);
    }
    // a sender's waist piece is drawn only by a receiver with waist_cargo on; a delivery by any
    CHECK(ReceiverDraw(SendModel(5, 0, 12, 0, true), true) == Draw::exact);
    CHECK(ReceiverDraw(SendModel(5, 0, 12, 0, true), false) == Draw::none);
    CHECK(ReceiverDraw(SendModel(5, 0, 0, 1, true), false) == Draw::cutscene);
}

void TestNames() {
    CHECK(std::strcmp(ModelName(15), "weapon case S (wsm0)") == 0);
    CHECK(std::strcmp(ModelName(14), "folding ladder (flad)") == 0);
    CHECK(std::strcmp(ModelName(99), "?") == 0);
    CHECK(IsWaist(4) && IsWaist(5) && !IsWaist(1) && !IsWaist(6));
}
}  // namespace

int main() {
    TestModels();
    TestSend();
    TestReceive();
    TestNames();
    std::printf("cargo_rules_tests: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
