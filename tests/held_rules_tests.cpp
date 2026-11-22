// Held items on the other screen, the pure part (native/mod/held_rules.hpp): which weapon resources are sent and drawn as a
// held clone in each held_items mode, which component resources a clone keeps, and the receiver's prop keys. No game.
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "held_rules.hpp"

using namespace st::heldrules;

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

void TestTools() {
    // the cores the user's logs and the round 6 census name
    CHECK(IsToolPath("ds/entities/weapons/ladder"));
    CHECK(IsToolPath("ds/entities/weapons/builder"));
    CHECK(IsToolPath("ds/entities/weapons/pile"));
    CHECK(IsToolPath("ds/entities/weapons/pile_lv2"));
    CHECK(!IsToolPath("ds/entities/weapons/assault_rifle"));
    CHECK(!IsToolPath("ds/entities/weapons/coating_spray"));
    CHECK(!IsToolPath("ds/entities/weapons/ladder_extra"));
    CHECK(!IsToolPath("ds/entities/item/ladder"));
    CHECK(!IsToolPath(nullptr));
    CHECK(!IsToolPath(""));
}

void TestWanted() {
    const char* ladder = "ds/entities/weapons/ladder";
    const char* spray = "ds/entities/weapons/coating_spray";
    const char* bola = "ds/entities/weapons/bola_gun";
    // off: never (v0.23)
    CHECK(!Wanted(Mode::off, ladder, false) && !Wanted(Mode::off, bola, false));
    // tools: the three construction tools only
    CHECK(Wanted(Mode::tools, ladder, false) && !Wanted(Mode::tools, spray, false) && !Wanted(Mode::tools, bola, false));
    // all: every weapon resource
    CHECK(Wanted(Mode::all, ladder, false) && Wanted(Mode::all, spray, false) && Wanted(Mode::all, bola, false));
    // a weapon with a cutscene prop keeps it, in every mode
    CHECK(!Wanted(Mode::all, "ds/entities/weapons/assault_rifle", true));
    // nothing outside ds/entities/weapons/, and not the folder itself
    CHECK(!Wanted(Mode::all, "ds/entities/item/backpack/backpack_temp", false));
    CHECK(!Wanted(Mode::all, "ds/entities/weapons/", false));
    CHECK(!Wanted(Mode::all, nullptr, false));
}

void TestComponents() {
    for (const char* k : {"SkinnedModelResource", "ArtPartsDataResource", "ArtPartsVariationResource", "ShaderVariableComponentResource",
                          "StencilBufferComponentResource", "ExternalAnimationComponentResource", "DSSimpleAnimationComponentResource",
                          "AttachToHelperMoverResource"})
        CHECK(KeepComponent(k) == 1);
    for (const char* k : {"SoundComponentResource", "DSBuildCheckEffectComponentResource", "DSWeaponComponentResource", "AIBodyResource",
                          "SkinnedModel", "", "PhysicsCollisionResource"})
        CHECK(KeepComponent(k) == 0);
    CHECK(KeepComponent(nullptr) == 0);
}

void TestKeys() {
    constexpr unsigned kMax = 64;
    constexpr int kBase = 17 + 64;  // the receiver's first held key (kPropCount + kMaxGearRes)
    // a mapped weapon: its cutscene prop, in every mode
    CHECK(WeaponKey(Mode::all, "ds/entities/weapons/assault_rifle", 9, 3, kMax, kBase) == 9);
    CHECK(WeaponKey(Mode::off, "ds/entities/weapons/assault_rifle", 9, 3, kMax, kBase) == 9);
    // the ladder: a held clone keyed by its resource id
    CHECK(WeaponKey(Mode::all, "ds/entities/weapons/ladder", -1, 5, kMax, kBase) == kBase + 5);
    CHECK(WeaponKey(Mode::tools, "ds/entities/weapons/ladder", -1, 63, kMax, kBase) == kBase + 63);
    // off, not wanted, or a resource id out of range: nothing
    CHECK(WeaponKey(Mode::off, "ds/entities/weapons/ladder", -1, 5, kMax, kBase) == -1);
    CHECK(WeaponKey(Mode::tools, "ds/entities/weapons/bola_gun", -1, 5, kMax, kBase) == -1);
    CHECK(WeaponKey(Mode::all, "ds/entities/weapons/ladder", -1, 64, kMax, kBase) == -1);
    CHECK(WeaponKey(Mode::all, "ds/entities/weapons/ladder", -1, 255, kMax, kBase) == -1);
}

void TestNames() {
    CHECK(std::strcmp(ShortName("ds/entities/weapons/ladder"), "ladder") == 0);
    CHECK(std::strcmp(ShortName("ladder"), "ladder") == 0);
    CHECK(std::strcmp(ShortName(nullptr), "?") == 0);
}
}  // namespace

int main() {
    TestTools();
    TestWanted();
    TestComponents();
    TestKeys();
    TestNames();
    std::printf("held_rules_tests: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
