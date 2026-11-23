// Enemy census, the pure part (native/mod/enemy_census.hpp): actor ids, the game's GUID hash-map slots, the GUID text, the
// set fingerprint and the cross-world comparison (match by GUID with a position guard). No game.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "enemy_census.hpp"

using namespace st::enemy;

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

Guid16 G(std::uint8_t a, std::uint8_t b = 0) {
    Guid16 g;
    g.b[0] = a;
    g.b[15] = b;
    return g;
}

void Put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void Put64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

void TestIds() {
    CHECK(ActorType(0x2005) == 1 && ActorSlot(0x2005) == 5);
    CHECK(ActorType(0x3FFFF) == 0x1F && ActorSlot(0x3FFFF) == 0x1FFF);
    CHECK(IsHumanEnemyId(0x2000) && IsHumanEnemyId(0x3FFF));
    CHECK(!IsHumanEnemyId(0x1FFF));   // type 0
    CHECK(!IsHumanEnemyId(0x4000));   // type 2
    CHECK(!IsHumanEnemyId(0x12000));  // type 9 (0x12000 is another actor kind)
    CHECK(!IsHumanEnemyId(0x42000));  // type 0x21
    CHECK(!IsHumanEnemyId(kActorNone));
}

void TestSlots() {
    CHECK(MapCapacityOk(1) && MapCapacityOk(256) && MapCapacityOk(kMaxMapCapacity));
    CHECK(!MapCapacityOk(0) && !MapCapacityOk(3) && !MapCapacityOk(kMaxMapCapacity * 2) && !MapCapacityOk(0xFFFFFFFFu));

    std::uint8_t k[kKindSlot] = {};
    Guid16 g;
    std::uint32_t kind = 99;
    CHECK(!DecodeKindSlot(k, &g, &kind) && kind == 99);  // empty
    Put32(k, 0x1234);                                     // a hash without the game's high bit: not a used slot
    CHECK(!DecodeKindSlot(k, &g, &kind));
    Put32(k, 0x80001234);
    for (int i = 0; i < 16; ++i) k[4 + i] = static_cast<std::uint8_t>(0xA0 + i);
    Put32(k + 0x14, 6);
    CHECK(DecodeKindSlot(k, &g, &kind) && kind == 6 && g.b[0] == 0xA0 && g.b[15] == 0xAF);

    std::uint8_t v[kValueSlot] = {};
    std::uint64_t val = 0;
    CHECK(!DecodeValueSlot(v, &g, &val));
    Put32(v, 0xC0000001);
    for (int i = 0; i < 16; ++i) v[8 + i] = static_cast<std::uint8_t>(i);
    Put64(v + 0x18, 0x00007FF612345678ull);
    CHECK(DecodeValueSlot(v, &g, &val) && val == 0x00007FF612345678ull && g.b[0] == 0 && g.b[15] == 15);
    CHECK(Le32(v) == 0xC0000001u && Le64(v + 0x18) == 0x00007FF612345678ull);
}

void TestText() {
    Guid16 g;
    for (int i = 0; i < 16; ++i) g.b[i] = static_cast<std::uint8_t>(i * 17);
    char t[33];
    GuidText(g, t);
    CHECK(std::strcmp(t, "00112233445566778899AABBCCDDEEFF") == 0);
    CHECK(Guid16{}.Zero() && !g.Zero());
    CHECK(G(1) < G(2) && !(G(2) < G(1)) && G(3) == G(3) && G(3) != G(3, 1));
}

void TestPrint() {
    // the same set in any order gives the same print; a different set does not
    std::vector<Guid16> a = {G(1), G(2), G(3)}, b = {G(3), G(1), G(2)}, c = {G(1), G(2), G(4)};
    CHECK(GuidSetPrint(a) == GuidSetPrint(b));
    CHECK(GuidSetPrint(a) != GuidSetPrint(c));
    CHECK(GuidSetPrint({}) == kFnvSeed);
    const std::uint32_t one = 1;
    CHECK(Fnv1a(kFnvSeed, &one, 4) != kFnvSeed);
}

Seen S(std::uint8_t g, std::uint32_t id, double x, double y, bool pos = true, bool realized = true, bool dead = false) {
    Seen s;
    s.guid = g ? G(g) : Guid16{};
    s.id = id;
    s.has_pos = pos;
    s.x = x, s.y = y, s.z = 100;
    s.realized = realized;
    s.dead = dead;
    return s;
}

void TestCompare() {
    // identical worlds
    std::vector<Seen> a = {S(1, 0x2001, 0, 0), S(2, 0x2002, 10, 0), S(3, 0x2003, 20, 0)};
    Comparison c = Compare(a, a, 5);
    CHECK(c.both == 3 && c.only_a == 0 && c.only_b == 0 && c.same_id == 3 && c.both_pos == 3 && c.within == 3 && c.beyond == 0 && c.max_m == 0);
    CHECK(c.both_realized == 3 && c.dead_differs == 0 && c.dup_a == 0 && c.dup_b == 0);

    // the other world: one moved 2 m, one moved 30 m, one missing, one extra, one with another id, one dead
    std::vector<Seen> b = {S(1, 0x2001, 2, 0), S(2, 0x2009, 40, 0, true, true, true), S(4, 0x2004, 0, 0)};
    c = Compare(a, b, 5);
    CHECK(c.both == 2 && c.only_a == 1 && c.only_b == 1);
    CHECK(c.same_id == 1);
    CHECK(c.both_pos == 2 && c.within == 1 && c.beyond == 1);
    CHECK(c.max_m > 29.99 && c.max_m < 30.01);
    CHECK(c.p50_m > 15.99 && c.p50_m < 16.01);  // the median of {2, 30}
    CHECK(c.dead_differs == 1);

    // actors without a GUID are never matched; a missing place counts as matched but not placed
    std::vector<Seen> d = {S(0, 0x2001, 0, 0), S(1, 0x2001, 0, 0, false, false)};
    c = Compare(a, d, 5);
    CHECK(c.both == 1 && c.only_a == 2 && c.only_b == 0 && c.both_pos == 0 && c.within == 0 && c.both_realized == 0);

    // a GUID twice in one census (should never happen; counted)
    std::vector<Seen> e = {S(1, 0x2001, 0, 0), S(1, 0x2005, 0, 0)};
    c = Compare(e, a, 5);
    CHECK(c.dup_a == 1 && c.dup_b == 0);

    // empty
    c = Compare({}, {}, 5);
    CHECK(c.both == 0 && c.only_a == 0 && c.only_b == 0 && c.p50_m == 0 && c.p95_m == 0);
    CHECK(Percentile({1, 2, 3, 4, 5}, 0.5) == 3 && Percentile({7}, 0.95) == 7);
}

}  // namespace

int main() {
    TestIds();
    TestSlots();
    TestText();
    TestPrint();
    TestCompare();
    std::printf("enemy census tests: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
