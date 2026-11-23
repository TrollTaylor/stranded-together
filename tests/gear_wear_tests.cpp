// Gear that wears out, the pure part (native/mod/gear_wear.hpp): the rain cover's durability in percent and its log steps,
// the rain cover's resource. No game.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "gear_wear.hpp"

using namespace st::gearwear;

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

bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

void TestPercent() {
    CHECK(Near(PercentOf(3600.0f, 3600.0f), 100.0));
    CHECK(Near(PercentOf(1800.0f, 3600.0f), 50.0));
    CHECK(Near(PercentOf(3.6f, 3600.0f), 0.1, 1e-5));
    CHECK(PercentOf(0.0f, 3600.0f) == 0.0);
    CHECK(PercentOf(-5.0f, 3600.0f) == 0.0);
    CHECK(PercentOf(std::numeric_limits<float>::quiet_NaN(), 3600.0f) == 0.0);
    CHECK(PercentOf(100.0f, 0.0f) == 0.0);                                         // no maximum
    CHECK(PercentOf(100.0f, std::numeric_limits<float>::quiet_NaN()) == 0.0);
    CHECK(PercentOf(4000.0f, 3600.0f) == 100.0);                                   // over full: capped
}

void TestBucket() {
    CHECK(Bucket(100.0) == 19);
    CHECK(Bucket(99.25) == 18);
    CHECK(Bucket(0.09) == 0);
    CHECK(Bucket(0.0) == 0);
    CHECK(Bucket(-1.0) == 0);
    CHECK(Bucket(std::numeric_limits<double>::quiet_NaN()) == 0);
    // 1 % steps below 10 %, 10 % steps from 10 % up
    CHECK(Bucket(1.0) == 1);
    CHECK(Bucket(9.99) == 9);
    CHECK(Bucket(10.0) == 10);
    CHECK(Bucket(19.99) == 10);
    CHECK(Bucket(20.0) == 11);
    CHECK(Bucket(90.0) == 18);
    CHECK(Bucket(250.0) == 19);
    // monotonic: a cover wearing from full to nothing passes 20 steps (19 changes)
    int last = Bucket(100.0), changes = 0;
    for (double pc = 100.0; pc >= 0.0; pc -= 0.025) {
        const int b = Bucket(pc);
        CHECK(b <= last);
        changes += b != last;
        last = b;
    }
    CHECK(changes == 19);
    CHECK(last == 0);
}

void TestRainCover() {
    const std::uint8_t cover[16] = {0x64, 0xCE, 0xFE, 0x0D, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const std::uint8_t other[16] = {0xAD, 0x94, 0xF2, 0x6B};  // the dreamcatcher (6bf294ad)
    CHECK(IsRainCover("ds/entities/item/backpack/backpack_temp", cover));
    CHECK(!IsRainCover("ds/entities/item/backpack/backpack_temp", other));
    CHECK(!IsRainCover("ds/entities/weapons/weapon_collection", cover));
    CHECK(!IsRainCover(nullptr, cover));
    CHECK(!IsRainCover("ds/entities/item/backpack/backpack_temp", nullptr));
}

// Round 11: why the cover's entity went from a layout.
void TestCoverGone() {
    // the user's round 10 logs: no cover from the first read; the load's leftover entity went (not timefall)
    CHECK(WhyCoverGone(0, false, 0.0) == CoverGone::none_in_save);
    // no wear read yet, or the partner's gear: the general wording
    CHECK(WhyCoverGone(-1, false, 0.0) == CoverGone::unknown);
    CHECK(WhyCoverGone(-1, true, 50.0) == CoverGone::unknown);
    // worn this session: the last worn reading's durability says why (the entity may go before or after the read that sees it off)
    CHECK(WhyCoverGone(1, true, 0.4) == CoverGone::worn_out);
    CHECK(WhyCoverGone(0, true, 0.4) == CoverGone::worn_out);
    CHECK(WhyCoverGone(0, true, 1.0) == CoverGone::worn_out);
    CHECK(WhyCoverGone(0, true, 63.0) == CoverGone::taken_off);
    CHECK(WhyCoverGone(1, true, 63.0) == CoverGone::taken_off);
    // the wordings: the scenarios wait for "rain cover is gone" (timefall, the partner's); a save without one never says it
    CHECK(std::strncmp(CoverGoneText(CoverGone::worn_out), "is gone", 7) == 0);
    CHECK(std::strstr(CoverGoneText(CoverGone::worn_out), "worn out in timefall") != nullptr);
    CHECK(std::strncmp(CoverGoneText(CoverGone::taken_off), "is gone", 7) == 0);
    CHECK(std::strncmp(CoverGoneText(CoverGone::unknown), "is gone", 7) == 0);
    CHECK(std::strstr(CoverGoneText(CoverGone::none_in_save), "is gone") == nullptr);
    CHECK(std::strstr(CoverGoneText(CoverGone::none_in_save), "timefall") != nullptr);  // "not timefall"
    CHECK(std::strstr(CoverGoneText(CoverGone::none_in_save), "worn out") == nullptr);
    CHECK(std::strstr(CoverGoneText(CoverGone::none_in_save), "since the last load") != nullptr);  // per load, not per session
}
}  // namespace

int main() {
    TestPercent();
    TestBucket();
    TestRainCover();
    TestCoverGone();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
