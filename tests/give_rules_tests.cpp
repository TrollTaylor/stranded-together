// Dev give's pure part (native/mod/give_rules.hpp): the allow list, the groups, the refusals, the weight budget, the
// session.ini values. No game.
#include <cstdio>
#include <cstring>

#include "dev_rules.hpp"
#include "give_rules.hpp"

using namespace st::giverules;

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

void TestLists() {
    // no code twice, none special, none the revolver, sane weights and names
    const std::size_t n = sizeof(kAllowed) / sizeof(kAllowed[0]);
    static_assert(sizeof(kAllowed) / sizeof(kAllowed[0]) == 79, "the allow list");
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(kAllowed[i].code != 0);
        CHECK(kAllowed[i].kg > 0.0f && kAllowed[i].kg < 25.0f);
        CHECK(kAllowed[i].name && kAllowed[i].name[0]);
        CHECK(!IsSpecial(kAllowed[i].code));
        CHECK(kAllowed[i].code != kRevolver);
        for (std::size_t j = i + 1; j < n; ++j) CHECK(kAllowed[i].code != kAllowed[j].code);
    }
    for (std::size_t i = 0; i < sizeof(kSpecial) / sizeof(kSpecial[0]); ++i)
        for (std::size_t j = i + 1; j < sizeof(kSpecial) / sizeof(kSpecial[0]); ++j) CHECK(kSpecial[i] != kSpecial[j]);
    // every group names listed codes only and fits the default budget (nothing skipped at 30 kg)
    static_assert(kGroupCount == 7, "the groups");
    for (const Group& g : kGroups) {
        CHECK(FindGroup(g.id) == &g);
        double kg = 0;
        int requests = 0;
        for (std::size_t i = 0; i < g.n; ++i) {
            const Entry* e = Find(g.items[i].code);
            CHECK(e != nullptr);
            CHECK(g.items[i].count >= 1 && g.items[i].count <= kMaxCount);
            if (e) kg += static_cast<double>(e->kg) * g.items[i].count;
            requests += g.items[i].count;
        }
        CHECK(kg <= kDefaultMaxKg);
        CHECK(requests <= kMaxPlan);
        Plan p;
        CHECK(BuildPlan(g.id, 0, 0, kDefaultMaxKg, &p) == nullptr);
        CHECK(p.n == requests);
        CHECK(p.skipped == 0);
        CHECK(std::strcmp(p.name, g.name) == 0);
    }
    CHECK(FindGroup(0) == nullptr && FindGroup(8) == nullptr);
    // the weapons group: the eight guns, 29.7 kg
    Plan w;
    CHECK(BuildPlan(1, 0, 0, 30, &w) == nullptr);
    CHECK(w.n == 8);
    CHECK(w.kg > 29.6 && w.kg < 29.8);
    CHECK(w.codes[0] == 1297289994u && w.codes[7] == 1779835876u);
    // the supplies group: counts expand (2 blood bags, 1 boots, 3 magazines)
    Plan s;
    CHECK(BuildPlan(5, 0, 0, 30, &s) == nullptr);
    CHECK(s.n == 6);
    CHECK(s.codes[0] == 336212924u && s.codes[1] == 336212924u && s.codes[5] == 1221065158u);
}

void TestRefusals() {
    CHECK(RefuseCode(0) != nullptr);
    CHECK(RefuseCode(1297289994u) == nullptr);                              // Handgun Lv. 2
    CHECK(std::strcmp(RefuseCode(2090599717u), kSpecialReason) == 0);       // Floating Carrier (Lv. 1)
    CHECK(std::strcmp(RefuseCode(492117585u), kSpecialReason) == 0);        // Floating Carrier (Lv. 2)
    CHECK(std::strcmp(RefuseCode(kRevolver), kStoryReason) == 0);
    CHECK(std::strcmp(RefuseCode(12345u), kNotListed) == 0);
    // the Strand, Urination, Odradek ids are not baggage codes: refused as unlisted
    CHECK(RefuseCode(41u) != nullptr && RefuseCode(58u) != nullptr);
    Plan p;
    CHECK(BuildPlan(0, 2090599717.0, 1, 30, &p) == kSpecialReason);
    CHECK(BuildPlan(0, 149232072.0, 1, 30, &p) == kStoryReason);
    CHECK(BuildPlan(0, 0, 1, 30, &p) != nullptr);
    CHECK(BuildPlan(0, -5, 1, 30, &p) != nullptr);
    CHECK(BuildPlan(0, 1.5, 1, 30, &p) != nullptr);
    CHECK(BuildPlan(0, 5e9, 1, 30, &p) != nullptr);
    CHECK(BuildPlan(0, 336212924.0, 6, 30, &p) != nullptr);   // count > 5
    CHECK(BuildPlan(0, 336212924.0, -1, 30, &p) != nullptr);
    CHECK(BuildPlan(9, 0, 0, 30, &p) != nullptr);
    CHECK(BuildPlan(-1, 0, 0, 30, &p) != nullptr);
    CHECK(BuildPlan(1, 0, 0, 30, nullptr) != nullptr);
    // one code, count 0 = 1, count 3
    CHECK(BuildPlan(0, 336212924.0, 0, 30, &p) == nullptr);
    CHECK(p.n == 1 && p.codes[0] == 336212924u && std::strcmp(p.name, "Blood Bag") == 0);
    CHECK(BuildPlan(0, 336212924.0, 3, 30, &p) == nullptr);
    CHECK(p.n == 3);
}

void TestBudget() {
    // 10 kg: the weapons group gives what fits in order and names the rest
    Plan p;
    CHECK(BuildPlan(1, 0, 0, 10, &p) == nullptr);
    CHECK(p.kg <= 10.0);
    CHECK(p.n + p.skipped == 8);
    CHECK(p.skipped > 0);
    CHECK(std::strstr(p.skipped_names, "Grenade Launcher (6.1 kg)") != nullptr);
    // the rocket launcher alone is over 10 kg: an empty plan, not a refusal
    CHECK(BuildPlan(0, 1152486986.0, 1, 10, &p) == nullptr);
    CHECK(p.n == 0 && p.skipped == 1);
    // a non-positive budget is the default
    CHECK(BuildPlan(6, 0, 0, 0, &p) == nullptr);
    CHECK(p.skipped == 0 && p.n == 5);
    // the skipped names never overflow
    CHECK(BuildPlan(7, 0, 0, 1, &p) == nullptr);
    CHECK(p.n == 0 && p.skipped == 4 && std::strlen(p.skipped_names) < sizeof(p.skipped_names));
}

void TestConfig() {
    CHECK(MaxKg("40", 30) == 40);
    CHECK(MaxKg("0.5", 30) == 30);
    CHECK(MaxKg("500", 30) == 30);
    CHECK(MaxKg("x", 30) == 30);
    CHECK(MaxKg("", 30) == 30);
    CHECK(PerFrame("2", 4) == 2);
    CHECK(PerFrame("0", 4) == 4);
    CHECK(PerFrame("17", 4) == 4);
    CHECK(Slot("1", 0) == 1);
    static_assert(kDefaultSlot == 1, "Sam's own inventory");
    CHECK(Slot("0", 1) == 0);
    CHECK(Slot("31", 0) == 31);
    CHECK(Slot("32", 1) == 1);
    CHECK(Slot("-1", 1) == 1);
    CHECK(Slot("x", 1) == 1);
    CHECK(PerFrame("3x", 4) == 4);
}

void TestGate() {
    // dev_give is a world tool: the real-save rule (dev_rules.hpp) refuses it on a real save, the guest's copy is local only
    using namespace st::devrules;
    Session s;
    s.enabled = true;
    s.saves_redirected = false;
    CHECK(!Decide(Tool::world, s).ok());
    s.saves_redirected = true;
    CHECK(Decide(Tool::world, s).route == Route::local);
    s.role = 2, s.host_save = true;
    CHECK(Decide(Tool::world, s).route == Route::local_only);
    s.role = 1;
    CHECK(Decide(Tool::world, s).route == Route::local_only);
    s.saves_redirected = false;
    CHECK(!Decide(Tool::world, s).ok());
}
}  // namespace

int main() {
    TestLists();
    TestRefusals();
    TestBudget();
    TestConfig();
    TestGate();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
