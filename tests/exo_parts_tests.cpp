// Sam's skeletons on a body that is not Sam, the pure part (native/mod/exo_parts.hpp): the mesh controller's formula with the
// self view's bit ignored, the skeleton types, the exo carrier's parts plan. No game.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "exo_parts.hpp"

using namespace st::exo;

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

void TestFormula() {
    MeshCtl m;
    CHECK(MeshAsked(m));
    m.mask = 0x20;  // reason 5: this skeleton piece is not worn
    CHECK(!MeshAsked(m));
    m.mask = 0;
    m.mask0 = kSelfViewBit;  // our self view only: what the game wants still reads
    CHECK(MeshAsked(m));
    CHECK(!MeshAsked(m, 0));  // ... unless our bit counts
    m.mask0 = kSelfViewBit | (1ull << 26);  // photo mode's own reason: hidden
    CHECK(!MeshAsked(m));
    m.mask0 = 0;
    m.root = 1;
    CHECK(!MeshAsked(m));
    m.root = 0;
    m.mask = 0x20;
    m.b = 0x20000000;  // reason 29 forced show wins over the mask
    CHECK(MeshAsked(m));
    m.b = 0;
    m.mask = 0;
    m.c = 0x20000000;  // forced hide
    CHECK(!MeshAsked(m));
    m.c = 0;
    m.cmpmode = true;
    m.cmpval = 0x10;
    m.mask = 0x10;
    CHECK(!MeshAsked(m));
    m.mask = 0;
    CHECK(MeshAsked(m));
    m.mask = 0x30;
    CHECK(MeshAsked(m));
}

void TestTypes() {
    std::uint64_t p[2];
    TypeParts(Type::speed, p);
    CHECK(Bit(p, 32) && Bit(p, 33) && Bit(p, 34) && Bit(p, 35) && !Bit(p, 36) && !Bit(p, 37) && !Bit(p, 39) && !Bit(p, 50) && !Bit(p, 51));
    CHECK(TypeOfParts(p) == 1);
    TypeParts(Type::all_terrain, p);
    CHECK(Bit(p, 32) && Bit(p, 33) && Bit(p, 34) && !Bit(p, 35) && Bit(p, 36) && !Bit(p, 37));
    CHECK(TypeOfParts(p) == 2);
    TypeParts(Type::power, p);
    CHECK(Bit(p, 32) && !Bit(p, 33) && !Bit(p, 34) && Bit(p, 37) && Bit(p, 39) && !Bit(p, 50));
    CHECK(TypeOfParts(p) == 3);
    TypeParts(Type::support, p);
    CHECK(!Bit(p, 32) && Bit(p, 50) && Bit(p, 51));
    CHECK(TypeOfParts(p) == 4);
    TypeParts(Type::none, p);
    CHECK(p[0] == 0 && p[1] == 0 && TypeOfParts(p) == 0);
    std::uint64_t odd[2] = {1ull << 35, 0};  // speed alone: no known type
    CHECK(TypeOfParts(odd) == -1);
    CHECK(std::strcmp(TypeName(Type::all_terrain), "all-terrain") == 0);
    std::int32_t idx[kExoMeshCount] = {32, 33, 34, 35, 36, 37, 39, 50, 51};
    CHECK(MapIsPorter(idx));
    idx[7] = 49;
    CHECK(!MapIsPorter(idx));
}

void TestSender() {
    std::uint64_t apply[2] = {}, vis[2] = {};
    CHECK(!SenderNamesExo(apply));
    OverrideParts(3, apply, vis);
    CHECK(SenderNamesExo(apply) && AnyExoShown(apply, vis) && TypeOfParts(vis) == 3);
    OverrideParts(0, apply, vis);
    CHECK(SenderNamesExo(apply) && !AnyExoShown(apply, vis));
    OverrideParts(-1, apply, vis);  // off: unchanged
    CHECK(SenderNamesExo(apply) && !AnyExoShown(apply, vis));
    std::uint64_t m[2];
    ExoMask(m);
    CHECK(m[0] == ((1ull << 32) | (1ull << 33) | (1ull << 34) | (1ull << 35) | (1ull << 36) | (1ull << 37) | (1ull << 39) | (1ull << 50) | (1ull << 51)) && m[1] == 0);
}

// The porter suit's runtime tree (round 7 decode of part_sam_body_suiti, runtime = decoded + 1).
void SuitTree(std::int16_t (&par)[kSuitParts]) {
    for (int i = 0; i < kSuitParts; ++i) par[i] = -1;
    // children of the container 1 (decoded parent 0 -> runtime 1)
    for (int i : {2, 3, 20, 21, 22, 23, 24, 25, 29, 30, 31, 32, 40, 42, 44, 45, 46, 47, 48, 49, 50}) par[i] = 1;
    for (int i : {4, 5, 6, 7, 8, 10, 12, 13, 16, 19}) par[i] = 3;
    par[9] = 8, par[11] = 10, par[14] = 13, par[15] = 13, par[17] = 16, par[18] = 16;
    par[26] = par[27] = par[28] = 25;
    for (int i : {33, 34, 35, 36, 37, 39}) par[i] = 32;
    par[38] = 37;
    par[41] = 40;
    par[43] = 42;
    par[51] = 50;
    for (int i = 53; i <= 89; ++i) par[i] = 52;  // the head subtree (flattened: enough for the checks)
}

void TestCarrier() {
    std::int16_t par[kSuitParts];
    SuitTree(par);
    CHECK(CarrierTreeOk(kSuitParts, par));
    CHECK(!CarrierTreeOk(106, par));
    std::int16_t bad[kSuitParts];
    std::memcpy(bad, par, sizeof(bad));
    bad[1] = 0;  // the container under the suit body: hiding the suit would hide the skeleton
    CHECK(!CarrierTreeOk(kSuitParts, bad));
    for (int t = 0; t < kTypeCount; ++t) {
        std::uint64_t vis[2];
        TypeParts(static_cast<Type>(t), vis);
        SetBit(vis, kLegOutPart, t != 0);  // the game swaps LegIn for LegOut with a skeleton
        SetBit(vis, 40, t == 0);
        for (int leg = 0; leg < 2; ++leg) {
            std::uint64_t touch[2], asked[2], shown[2];
            CarrierPlan(kSuitParts, par, vis, leg != 0, touch, asked);
            ShownFromAsked(kSuitParts, par, asked, shown);
            CHECK(Bit(touch, 0) && !Bit(asked, 0) && !Bit(shown, 0));
            CHECK(Bit(touch, 52) && !Bit(shown, 52) && !Bit(shown, 60));
            CHECK(Bit(shown, 1));
            CHECK(Bit(touch, 3) && !Bit(shown, 3) && !Bit(shown, 7) && !Bit(shown, 40) && !Bit(shown, 31) && !Bit(shown, 48));
            CHECK(!Bit(touch, 90) && !Bit(touch, 7));  // helpers and grandchildren of the container are left alone
            for (unsigned i = 0; i < kExoMeshCount; ++i) {
                const unsigned p = kExoPart[i];
                const bool parent_on = p == 32 || p == 50 ? true : p == 51 ? Bit(vis, 50) : Bit(vis, 32);
                CHECK(Bit(shown, p) == (Bit(vis, p) && parent_on));
            }
            CHECK(Bit(shown, kHeavySubPart) == (t == 3));
            CHECK(Bit(shown, kLegOutPart) == (leg && t != 0));
            // nothing outside the skeleton, the container and (leg) LegOut shows
            std::uint64_t allowed[2];
            ExoMask(allowed);
            SetBit(allowed, 1, true);
            SetBit(allowed, kHeavySubPart, true);
            if (leg) SetBit(allowed, kLegOutPart, true), SetBit(allowed, kLegOutLampPart, true);
            CHECK((shown[0] & ~allowed[0]) == 0 && (shown[1] & ~allowed[1]) == 0);
        }
    }
    // the type's pieces show exactly
    std::uint64_t vis[2], touch[2], asked[2], shown[2];
    TypeParts(Type::speed, vis);
    CarrierPlan(kSuitParts, par, vis, false, touch, asked);
    ShownFromAsked(kSuitParts, par, asked, shown);
    std::uint64_t want[2];
    TypeParts(Type::speed, want);
    SetBit(want, 1, true);
    CHECK(shown[0] == want[0] && shown[1] == want[1]);
    TypeParts(Type::support, vis);
    CarrierPlan(kSuitParts, par, vis, false, touch, asked);
    ShownFromAsked(kSuitParts, par, asked, shown);
    CHECK(Bit(shown, 50) && Bit(shown, 51) && !Bit(shown, 32) && !Bit(shown, 33));
    // a piece under a skeleton parent that is not shown is not even asked (the part setter does not look at the parent)
    std::uint64_t odd[2] = {};
    SetBit(odd, 34, true);
    SetBit(odd, 37, true);
    SetBit(odd, 51, true);
    CarrierPlan(kSuitParts, par, odd, false, touch, asked);
    CHECK(!Bit(asked, 34) && !Bit(asked, 37) && !Bit(asked, kHeavySubPart) && !Bit(asked, 51) && Bit(touch, 34) && Bit(touch, kHeavySubPart));
    TypeParts(Type::power, vis);
    CarrierPlan(kSuitParts, par, vis, false, touch, asked);
    CHECK(Bit(asked, 32) && Bit(asked, 37) && Bit(asked, kHeavySubPart) && Bit(asked, 39) && !Bit(asked, 34));
}
}  // namespace

int main() {
    TestFormula();
    TestTypes();
    TestSender();
    TestCarrier();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
