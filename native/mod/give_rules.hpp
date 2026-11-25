// Dev give (docs/debug-actions.md "Dev give"), the pure part: which baggage the dev tools may give Sam, the groups the menu's
// buttons give, the refusals and the weight budget. No game memory, no Windows: unit-tested in tests/give_rules_tests.cpp. The
// game side is dev_tools.cpp (Op::give: the game's own CreateAndAddBaggageToPlayer, byte-checked, on the game thread).
//
// Every weapon, grenade, tool, equipment piece, pair of boots and blood bag in DS is a baggage (DSBaggage2) named by a u32
// baggage NameCode. The codes, names and weights below are the game's own data (round 22 re-items: the catalogues weapon /
// equipment / item and their _1p5 files joined to the baggage lists and the English names; scratchpad items_table.txt).
// Only codes on this list are ever given (an allow list): the catalogue's "special" entries (vehicles, the floating
// carrier, pouches, backpack covers, batteries: unlocks and upgrades, not carried baggage), the NonBaggage things (Strand,
// Urination, Odradek, Harmonica), the story items (the Revolver: CanRequest False) and anything else are refused.
// Giving a baggage does not unlock its recipe [inferred, round 22 re-items 4]: the progression stays as it was. The tools
// run only on a sandboxed save (devrules::Tool::world), so a gift never reaches a real save.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace st::giverules {

struct Entry {
    std::uint32_t code;  // the baggage NameCode
    const char* name;    // English, from the game's localisation
    float kg;            // the catalogue's weight
    bool late;           // its recipe has an unlock (FriendShip level, MissionClear, DeliveryOfLostBaggage) in the normal game
};

// clang-format off
inline constexpr Entry kAllowed[] = {
    // weapons (catalogues weapon, weapon_1p5)
    {1060671497u, "Handgun (Lv. 1)", 1.1f, false},
    {1297289994u, "Handgun (Lv. 2)", 1.3f, true},
    {1577312510u, "Handgun (HG Custom)", 1.5f, true},
    {429488097u, "Anti-BT Handgun (Lv. 1)", 1.2f, false},
    {1811084514u, "Anti-BT Handgun (Lv. 2)", 1.4f, true},
    {1793345639u, "Assault Rifle (Lv. 1)", 3.3f, false},
    {412076900u, "Assault Rifle (Lv. 2)", 3.6f, false},
    {277446946u, "Assault Rifle (Lv. 3)", 5.1f, true},
    {1279635343u, "Assault Rifle (HG Custom)", 6.6f, true},
    {1200245701u, "Non-Lethal Assault Rifle (Lv. 1)", 3.3f, false},
    {199178384u, "Non-Lethal Assault Rifle (Lv. 2)", 3.6f, false},
    {1658978849u, "Non-Lethal Assault Rifle (Lv. 3)", 5.1f, true},
    {2041887635u, "Non-Lethal Assault Rifle (HG Custom)", 6.6f, true},
    {17195103u, "Shotgun (Lv. 1)", 3.6f, false},
    {1936581468u, "Shotgun (Lv. 2)", 3.9f, true},
    {410886375u, "Shotgun (HG Custom)", 5.4f, true},
    {1043213452u, "Riot Shotgun (Lv. 1)", 3.6f, false},
    {763289464u, "Riot Shotgun (Lv. 2)", 3.9f, true},
    {1595235451u, "Riot Shotgun (HG Custom)", 5.4f, true},
    {2018536597u, "Bola Gun (Lv. 1)", 3.5f, false},
    {641945777u, "Bola Gun (Lv. 2)", 2.9f, true},
    {1779835876u, "Grenade Launcher", 6.1f, false},
    {2034683920u, "Remote-Detonation Grenade Launcher", 6.4f, false},
    {1152486986u, "Quadruple Rocket Launcher", 19.2f, false},
    {920259913u, "Multi-Rocket Launcher", 11.3f, true},
    {446870126u, "Sticky Gun", 3.1f, true},
    {1643247843u, "Maser Gun", 2.8f, true},
    // grenades and throwables
    {796556776u, "Grenade", 2.0f, false},
    {1706476718u, "Smoke Grenade (Lv. 1)", 2.5f, false},
    {400377773u, "Smoke Grenade (Lv. 2)", 2.7f, true},
    {1994798938u, "Stun Bomb", 1.5f, false},
    {687147902u, "Remote Stun Bomb", 1.7f, true},
    {1354087217u, "Hematic Grenade (Lv. 1)", 2.5f, false},
    {584988722u, "Hematic Grenade (Lv. 2)", 2.7f, false},
    {908617418u, "Custom Hematic Grenade", 2.2f, false},
    {75489242u, "EX Grenade No. 0", 0.6f, false},
    {388967470u, "EX Grenade No. 1", 0.6f, false},
    {1884694227u, "EX Grenade No. 1+", 0.6f, false},
    {37594576u, "EX Grenade No. 2", 0.6f, false},
    {445344244u, "Smoke-Emitting Decoy Cargo", 3.0f, false},
    {1312886047u, "Vog-Emitting Decoy Cargo", 3.0f, false},
    // tools
    {1458972140u, "Ladder", 5.0f, false},
    {1575196008u, "Custom Chiral Ladder", 1.1f, false},
    {614311663u, "Climbing Anchor (Lv. 1)", 5.0f, false},
    {2056166091u, "Climbing Anchor (Lv. 2)", 7.1f, false},
    {1006809425u, "Custom Chiral Climbing Anchor", 1.1f, false},
    {386219546u, "PCC (Lv. 1)", 5.0f, false},
    {1701756185u, "PCC (Lv. 2)", 5.1f, false},
    {64582358u, "Container Repair Spray", 1.0f, true},
    // equipment (catalogues equipment, equipment_1p5)
    {1690305677u, "Power Skeleton (Lv. 1)", 10.0f, false},
    {2005989241u, "Power Skeleton (Lv. 2)", 8.0f, true},
    {593138578u, "Power Skeleton (Lv. 3)", 6.0f, true},
    {100365434u, "All-terrain Skeleton (Lv. 1)", 8.0f, false},
    {805986406u, "All-terrain Skeleton (Lv. 2)", 6.0f, true},
    {1847155778u, "All-terrain Skeleton (Lv. 3)", 4.0f, true},
    {380340110u, "Speed Skeleton (Lv. 1)", 6.0f, false},
    {1362170001u, "Speed Skeleton (Lv. 2)", 4.0f, true},
    {1113711461u, "Speed Skeleton (Lv. 3)", 2.0f, true},
    {1988603691u, "Support Skeleton", 5.5f, true},
    {374991885u, "Armor Plate (Lv. 1)", 2.5f, false},
    {1681026830u, "Armor Plate (Lv. 2)", 2.5f, true},
    {2002951418u, "Armor Plate (Lv. 3)", 1.2f, true},
    {84484089u, "Armor Plate (Lv. 4)", 0.6f, true},
    {582894740u, "Power Gloves", 2.0f, false},
    {928696810u, "Gravity Gloves", 2.0f, true},
    {1586862296u, "Silver Hand", 2.7f, true},
    {1356187543u, "Thermal Pad", 2.5f, false},
    {1116965094u, "Oxygen Mask", 1.0f, false},
    // items (catalogues item, item_1p5)
    {336212924u, "Blood Bag", 1.0f, false},
    {1717678271u, "High-capacity Blood Bag", 1.0f, false},
    {1077034196u, "Bridges Boots (Lv. 1)", 0.4f, true},
    {844742615u, "Bridges Boots (Lv. 2)", 0.4f, false},
    {554257443u, "Bridges Boots (Lv. 3)", 0.4f, false},
    {1966162763u, "Chiral Boots", 0.2f, false},
    {351819839u, "Porter Boots", 0.5f, false},
    {1398984480u, "MULE Boots", 0.5f, false},
    {225571588u, "Terrorist Boots", 0.5f, false},
    {2082492767u, "Omnireflector Boots", 0.5f, false},
    {1221065158u, "Spare Magazine", 0.3f, false},
};

// The catalogue's "special" entries' baggage codes (vehicles, the floating carrier, pouches, backpack covers, batteries, coins,
// talismans): unlocks, upgrades or vehicles, never given (a clearer refusal than "not on the list").
inline constexpr std::uint32_t kSpecial[] = {
    24850722u, 81573226u, 86069083u, 103447673u, 114946861u, 119926219u, 159025865u, 192817241u, 204470733u, 251102758u,
    316254549u, 344421820u, 360353677u, 361292814u, 361300185u, 377739052u, 395069086u, 450368190u, 492117585u, 544740549u,
    577011637u, 641363663u, 710092681u, 742070455u, 775928359u, 835119042u, 858318641u, 868369746u, 896179515u, 899455419u,
    970555571u, 972144638u, 992658036u, 1033708112u, 1067024576u, 1094935602u, 1173637509u, 1191255608u, 1229369562u, 1236241431u,
    1268399357u, 1269509767u, 1377015750u, 1414655436u, 1414689634u, 1448530418u, 1477031664u, 1480107146u, 1510806624u, 1524902061u,
    1548994697u, 1622531067u, 1649952166u, 1655798123u, 1709688221u, 1742898138u, 1742956301u, 1756651536u, 1803184305u, 1827544681u,
    1860877561u, 1897128220u, 1912266193u, 1930904460u, 1950482298u, 1957851182u, 2065102951u, 2090599717u, 2101879950u, 2135721502u,
};
inline constexpr std::uint32_t kRevolver = 149232072u;  // the story's revolver (CanRequest False)

// ---------------------------------------------------------------- groups (the Dev tools page's buttons; dev_give <group>)
struct Item {
    std::uint32_t code;
    int count;
};
inline constexpr Item kWeapons[] = {  // 29.7 kg: one of each gun at its best plain level (no HG custom)
    {1297289994u, 1}, {1811084514u, 1}, {277446946u, 1}, {1658978849u, 1}, {1936581468u, 1}, {763289464u, 1}, {641945777u, 1}, {1779835876u, 1},
};
inline constexpr Item kThrowables[] = {  // 9.5 kg
    {796556776u, 1}, {584988722u, 1}, {400377773u, 1}, {1994798938u, 1}, {37594576u, 1},
};
inline constexpr Item kTools[] = {  // 19.2 kg
    {1458972140u, 1}, {2056166091u, 1}, {1701756185u, 1}, {64582358u, 2},
};
inline constexpr Item kEquipment[] = {  // 8.6 kg: one skeleton (they share the suit slot)
    {593138578u, 1}, {84484089u, 1}, {582894740u, 1},
};
inline constexpr Item kSupplies[] = {  // 3.3 kg
    {336212924u, 2}, {554257443u, 1}, {1221065158u, 3},
};
inline constexpr Item kLate[] = {  // 28.6 kg: the late-game unlocks and the heavy launcher
    {1577312510u, 1}, {446870126u, 1}, {1643247843u, 1}, {928696810u, 1}, {1152486986u, 1},
};
inline constexpr Item kCustom[] = {  // 24.0 kg: the HG custom long guns
    {1279635343u, 1}, {2041887635u, 1}, {410886375u, 1}, {1595235451u, 1},
};
// clang-format on

struct Group {
    int id;
    const char* name;
    const Item* items;
    std::size_t n;
};
inline constexpr Group kGroups[] = {
    {1, "weapons", kWeapons, sizeof(kWeapons) / sizeof(kWeapons[0])},
    {2, "throwables", kThrowables, sizeof(kThrowables) / sizeof(kThrowables[0])},
    {3, "tools", kTools, sizeof(kTools) / sizeof(kTools[0])},
    {4, "equipment", kEquipment, sizeof(kEquipment) / sizeof(kEquipment[0])},
    {5, "supplies", kSupplies, sizeof(kSupplies) / sizeof(kSupplies[0])},
    {6, "late", kLate, sizeof(kLate) / sizeof(kLate[0])},
    {7, "custom", kCustom, sizeof(kCustom) / sizeof(kCustom[0])},
};
inline constexpr int kGroupCount = static_cast<int>(sizeof(kGroups) / sizeof(kGroups[0]));

inline const Entry* Find(std::uint32_t code) noexcept {
    for (const Entry& e : kAllowed)
        if (e.code == code) return &e;
    return nullptr;
}
inline const Group* FindGroup(int id) noexcept {
    for (const Group& g : kGroups)
        if (g.id == id) return &g;
    return nullptr;
}
inline bool IsSpecial(std::uint32_t code) noexcept {
    for (std::uint32_t c : kSpecial)
        if (c == code) return true;
    return false;
}

inline constexpr const char* kNotListed = "not on the verified list of weapons, tools, equipment and items (docs/debug-actions.md \"Dev give\")";
inline constexpr const char* kSpecialReason = "a catalogue special entry (a vehicle, an upgrade or an unlock): never given";
inline constexpr const char* kStoryReason = "a story item (the Revolver): never given";

// nullptr when the code may be given, else why not.
inline const char* RefuseCode(std::uint32_t code) noexcept {
    if (code == 0) return "no baggage code";
    if (Find(code)) return nullptr;
    if (IsSpecial(code)) return kSpecialReason;
    if (code == kRevolver) return kStoryReason;
    return kNotListed;
}

// ---------------------------------------------------------------- the plan of one press
inline constexpr int kMaxPlan = 16;     // requests in one press
inline constexpr int kMaxCount = 5;     // dev_give 0 <code> <count>
inline constexpr double kDefaultMaxKg = 30.0;
inline constexpr int kDefaultPerFrame = 4;

struct Plan {
    const char* name = "";              // the group's name, or the single entry's
    std::uint32_t codes[kMaxPlan] = {};  // one request each
    int n = 0;
    double kg = 0;
    int skipped = 0;                    // over the weight budget
    char skipped_names[160] = {};       // "Quadruple Rocket Launcher (19.2 kg), ..."
};

// group 1..7: that group; group 0: `code` (count 1..kMaxCount, 0 = 1). Entries that do not fit in max_kg are skipped (the
// rest still go). nullptr = a plan (maybe empty when everything was over the budget), else why the press is refused.
inline const char* BuildPlan(int group, double code, double count, double max_kg, Plan* out) noexcept {
    if (!out) return "no plan";
    *out = Plan{};
    if (!(max_kg > 0)) max_kg = kDefaultMaxKg;
    Item one[1] = {};
    const Item* items = nullptr;
    std::size_t n = 0;
    if (group == 0) {
        if (!(code >= 1 && code <= 4294967295.0) || code != static_cast<double>(static_cast<std::uint32_t>(code))) return "dev_give 0 <baggage code> [count]";
        const std::uint32_t c = static_cast<std::uint32_t>(code);
        if (const char* why = RefuseCode(c)) return why;
        const int k = count >= 1 && count <= kMaxCount ? static_cast<int>(count) : count == 0 ? 1 : -1;
        if (k < 1) return "count 1..5";
        one[0] = {c, k};
        items = one, n = 1;
        out->name = Find(c)->name;
    } else {
        const Group* g = FindGroup(group);
        if (!g) return "dev_give 1 weapons, 2 throwables, 3 tools, 4 equipment, 5 supplies, 6 late, 7 custom, 0 <code> [count]";
        items = g->items, n = g->n;
        out->name = g->name;
    }
    std::size_t sk = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const Entry* e = Find(items[i].code);
        if (!e) return kNotListed;  // a group names only listed codes (tests)
        for (int k = 0; k < items[i].count; ++k) {
            if (out->n >= kMaxPlan || out->kg + e->kg > max_kg + 1e-6) {
                ++out->skipped;
                if (sk < sizeof(out->skipped_names) - 1) {
                    const int w = std::snprintf(out->skipped_names + sk, sizeof(out->skipped_names) - sk, "%s%s (%.1f kg)", sk ? ", " : "", e->name,
                                                static_cast<double>(e->kg));
                    if (w > 0) sk = (std::min)(sizeof(out->skipped_names) - 1, sk + static_cast<std::size_t>(w));
                }
                continue;
            }
            out->codes[out->n++] = e->code;
            out->kg += e->kg;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- session.ini (config.cpp)
// dev_give_max_kg: 1..200 kg per press.
inline double MaxKg(const std::string& v, double keep) {
    char* end = nullptr;
    const double r = std::strtod(v.c_str(), &end);
    if (v.empty() || !end || *end) return keep;
    return r >= 1.0 && r <= 200.0 ? r : keep;
}
// dev_give_per_frame: 1..16 requests a game tick.
inline int PerFrame(const std::string& v, int keep) {
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || !end || *end) return keep;
    return n >= 1 && n <= 16 ? static_cast<int>(n) : keep;
}

// dev_give_slot: 0..31, the request's u8 (+0x10 of the 0x90 request): the consumer 0x142C95530 compares it with a holder's
// kind byte (holder +1) and remaps 12 to 1 [observed]. 0 made the baggages at the world origin with no holder, +0x21 1 (run
// 20261125-125658-dev-give-solo); 1 put them on Sam: +0x21 2, a holder, 1.3-2.6 m from him, his carried slots 9 -> 17 -> 25
// (run 20261125-130213) [observed]. Default 1.
inline constexpr int kDefaultSlot = 1;
inline int Slot(const std::string& v, int keep) {
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || !end || *end) return keep;
    return n >= 0 && n <= 31 ? static_cast<int>(n) : keep;
}

}  // namespace st::giverules
