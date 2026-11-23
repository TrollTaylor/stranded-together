// Sam's skeletons on a body that is not Sam, the pure part (native/mod/exo_parts.hpp): the mesh controller's formula with the
// self view's bit ignored, the skeleton types, the exo carrier's parts plan, the rain hood on the carrier. No game.
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
// Section 37: the hood on the carrier. A head subtree with nesting: 53 (porter on) with a lamp child 90, 55 (hair) with a
// child 56, the otter hood 69 under a holder 60, the oxi switch 84 with children 85 / 86.
void HoodTree(std::int16_t (&par)[kSuitParts]) {
    SuitTree(par);
    par[56] = 55;
    par[60] = 52;
    par[69] = 60;
    par[70] = 60;
    par[85] = par[86] = 84;
    par[90] = 53;
}

void TestHoodRules() {
    CHECK(HoodPhaseOf(0) == HoodPhase::down && HoodPhaseOf(1) == HoodPhase::down && HoodPhaseOf(2) == HoodPhase::down);
    CHECK(HoodPhaseOf(3) == HoodPhase::raising && HoodPhaseOf(4) == HoodPhase::raising && HoodPhaseOf(5) == HoodPhase::raising);
    CHECK(HoodPhaseOf(6) == HoodPhase::up && HoodPhaseOf(7) == HoodPhase::up && HoodPhaseOf(8) == HoodPhase::up && HoodPhaseOf(10) == HoodPhase::up);
    CHECK(HoodPhaseOf(9) == HoodPhase::lowering && HoodPhaseOf(11) == HoodPhase::lowering && HoodPhaseOf(12) == HoodPhase::lowering);
    CHECK(HoodPhaseOf(13) == HoodPhase::down && HoodPhaseOf(-1) == HoodPhase::down);
    CHECK(std::strcmp(HoodPhaseName(HoodPhase::lowering), "lowering") == 0 && std::strcmp(HoodTypeName(1), "oxi") == 0 && std::strcmp(HoodTypeName(2), "sea otter") == 0 &&
          std::strcmp(HoodTypeName(5), "none") == 0);
    CHECK(HoodAnimSlot(0) == 2 && HoodAnimSlot(1) == 5 && HoodAnimSlot(2) == 4 && HoodAnimSlot(3) == -1);
    // only the three hood slots reach the game's calls (0x1422C80B0 refuses 9 alone)
    for (int t = 0; t < kHoodTypes; ++t) CHECK(HoodSlotValid(HoodAnimSlot(t)));
    CHECK(!HoodSlotValid(-1) && !HoodSlotValid(255) && !HoodSlotValid(0) && !HoodSlotValid(3) && !HoodSlotValid(9));
    CHECK(HoodClipIndex(0, false, kHoodClipOpen) == 0 && HoodClipIndex(0, false, kHoodClipCloseLoop) == 3 && HoodClipIndex(0, true, kHoodClipOpen) == 4 &&
          HoodClipIndex(0, true, kHoodClipCloseLoop) == 7 && HoodClipIndex(1, false, kHoodClipOpenLoop) == 9 && HoodClipIndex(1, true, kHoodClipClose) == 10 &&
          HoodClipIndex(2, false, kHoodClipCloseLoop) == 15 && HoodClipIndex(3, false, 0) == -1 && HoodClipIndex(0, false, 4) == -1);
    CHECK(kHoodPart[0][0] == 54 && kHoodPart[0][1] == 53 && kHoodPart[1][0] == 71 && kHoodPart[2][0] == 69 && kHoodPart[2][2] == 83 && kHoodPart[1][2] == 84);
    CHECK(HoodHidesHat(HoodPhase::up) && !HoodHidesHat(HoodPhase::raising) && !HoodHidesHat(HoodPhase::lowering) && !HoodHidesHat(HoodPhase::down));
    std::int16_t par[kSuitParts];
    HoodTree(par);
    CHECK(CarrierTreeOk(kSuitParts, par));
    CHECK(HoodPartOk(kSuitParts, par, 53) && HoodPartOk(kSuitParts, par, 69) && HoodPartOk(kSuitParts, par, 85));
    CHECK(!HoodPartOk(kSuitParts, par, 52) && !HoodPartOk(kSuitParts, par, 32) && !HoodPartOk(kSuitParts, par, 0) && !HoodPartOk(kSuitParts, par, -1) &&
          !HoodPartOk(kSuitParts, par, 200) && !HoodPartOk(kSuitParts, nullptr, 53));
}

void TestCarrierHood() {
    std::int16_t par[kSuitParts];
    HoodTree(par);
    std::uint64_t vis[2], touch[2], asked[2], shown[2];
    TypeParts(Type::none, vis);
    // no hood: the head container off, nothing under it touched
    CarrierPlan(kSuitParts, par, vis, false, -1, touch, asked);
    ShownFromAsked(kSuitParts, par, asked, shown);
    CHECK(Bit(touch, 52) && !Bit(asked, 52) && !Bit(touch, 53) && !Bit(shown, 53) && !Bit(shown, 69));
    // the porter hood (53, directly under the head): the head on, 53 on, its siblings off, its lamp left alone
    CarrierPlan(kSuitParts, par, vis, false, 53, touch, asked);
    ShownFromAsked(kSuitParts, par, asked, shown);
    CHECK(Bit(asked, 52) && Bit(asked, 53) && Bit(shown, 53) && Bit(shown, 52));
    CHECK(Bit(touch, 55) && !Bit(asked, 55) && Bit(touch, 54) && !Bit(asked, 54) && Bit(touch, 60) && !Bit(asked, 60) && Bit(touch, 68) && !Bit(asked, 68));
    CHECK(!Bit(touch, 90) && !Bit(touch, 56) && !Bit(touch, 69));  // the hood's own child; grandchildren behind an off parent
    CHECK(!Bit(shown, 55) && !Bit(shown, 56) && !Bit(shown, 69) && !Bit(shown, 0));
    // with a skeleton too: both
    TypeParts(Type::speed, vis);
    CarrierPlan(kSuitParts, par, vis, false, 53, touch, asked);
    ShownFromAsked(kSuitParts, par, asked, shown);
    CHECK(Bit(shown, 53) && Bit(shown, 32) && Bit(shown, 35) && !Bit(shown, 37));
    // without the hood, the plan is the old one
    std::uint64_t t0[2], a0[2];
    CarrierPlan(kSuitParts, par, vis, false, touch, asked);
    CarrierPlan(kSuitParts, par, vis, false, -1, t0, a0);
    CHECK(touch[0] == t0[0] && touch[1] == t0[1] && asked[0] == a0[0] && asked[1] == a0[1]);
    // the otter hood (69) under a holder (60): the way down asked, the holder's other child (70) off
    TypeParts(Type::none, vis);
    CarrierPlan(kSuitParts, par, vis, false, 69, touch, asked);
    ShownFromAsked(kSuitParts, par, asked, shown);
    CHECK(Bit(shown, 69) && Bit(asked, 60) && Bit(touch, 70) && !Bit(asked, 70) && Bit(touch, 53) && !Bit(asked, 53) && !Bit(shown, 53));
    // only the container and the way down to the hood show
    for (unsigned i = 0; i < static_cast<unsigned>(kSuitParts); ++i)
        if (Bit(shown, i)) CHECK(i == 1 || i == 52 || i == 60 || i == 69);
    // the oxi switch (84): its children (85, 86) left as they are
    CarrierPlan(kSuitParts, par, vis, false, 84, touch, asked);
    CHECK(Bit(asked, 84) && !Bit(touch, 85) && !Bit(touch, 86) && Bit(touch, 71) && !Bit(asked, 71));
    // a part that is not under the head: no hood
    CarrierPlan(kSuitParts, par, vis, false, 32, touch, asked);
    CHECK(!Bit(asked, 52) && !Bit(asked, 32));
}

void TestHoodAnim() {
    // the timed swap (no unfold): the raised hood exactly while the sender's is up
    HoodAnim a;
    CHECK(HoodAnimTick(a, -1, HoodPhase::up, false, false, 0) == 0 && a.show == HoodShow::none);
    CHECK(HoodAnimTick(a, 1, HoodPhase::raising, false, false, 10) == 0 && a.show == HoodShow::none);
    CHECK(HoodAnimTick(a, 1, HoodPhase::up, false, false, 20) == 0 && a.show == HoodShow::on);
    CHECK(HoodAnimTick(a, 1, HoodPhase::lowering, false, false, 30) == 0 && a.show == HoodShow::none);
    CHECK(HoodAnimTick(a, 1, HoodPhase::down, false, false, 40) == 0 && a.show == HoodShow::none);
    // the unfold: enable + opening, the open loop the next tick, wait for the slot, then the raised hood
    a = HoodAnim{};
    CHECK(HoodAnimTick(a, 1, HoodPhase::down, true, true, 0) == 0 && a.show == HoodShow::none);
    CHECK(HoodAnimTick(a, 1, HoodPhase::raising, true, true, 100) == (kHoodEnable | kHoodPlayOpen) && a.show == HoodShow::sw && a.mode == 1);
    CHECK(HoodAnimTick(a, 1, HoodPhase::raising, true, true, 116) == kHoodPlayOpenLoop && a.show == HoodShow::sw);
    CHECK(HoodAnimTick(a, 1, HoodPhase::raising, true, true, 150) == 0 && a.show == HoodShow::sw);  // "done" within 100 ms: not yet
    CHECK(HoodAnimTick(a, 1, HoodPhase::raising, true, false, 900) == 0 && a.show == HoodShow::sw);
    CHECK(HoodAnimTick(a, 1, HoodPhase::raising, true, true, 1100) == kHoodDisable && a.show == HoodShow::on && a.mode == 0);
    CHECK(HoodAnimTick(a, 1, HoodPhase::raising, true, true, 1116) == 0 && a.show == HoodShow::on);  // not again while still raising
    CHECK(HoodAnimTick(a, 1, HoodPhase::up, true, true, 1200) == 0 && a.show == HoodShow::on);
    // lowering: enable + closing, the close loop, then nothing shown
    CHECK(HoodAnimTick(a, 1, HoodPhase::lowering, true, true, 2000) == (kHoodEnable | kHoodPlayClose) && a.show == HoodShow::sw && a.mode == 2);
    CHECK(HoodAnimTick(a, 1, HoodPhase::lowering, true, false, 2016) == kHoodPlayCloseLoop);
    CHECK(HoodAnimTick(a, 1, HoodPhase::lowering, true, false, 4000) == 0 && a.show == HoodShow::sw);
    CHECK(HoodAnimTick(a, 1, HoodPhase::lowering, true, false, 5100) == kHoodDisable && a.show == HoodShow::none);  // 3 s at most
    CHECK(HoodAnimTick(a, 1, HoodPhase::down, true, false, 5200) == 0 && a.show == HoodShow::none);
    // the instant path (a load): up without raising shows the hood at once, no clip
    CHECK(HoodAnimTick(a, 1, HoodPhase::up, true, false, 6000) == 0 && a.show == HoodShow::on);
    // the game resets the hood (down) while it unfolds: off at once
    a = HoodAnim{};
    HoodAnimTick(a, 0, HoodPhase::raising, true, false, 0);
    HoodAnimTick(a, 0, HoodPhase::raising, true, false, 16);
    CHECK(HoodAnimTick(a, 0, HoodPhase::down, true, false, 30) == kHoodDisable && a.show == HoodShow::none && a.mode == 0);
    // the type changes while it unfolds: the slot off, then the new type from scratch
    HoodAnimTick(a, 0, HoodPhase::raising, true, false, 100);
    CHECK(a.mode == 1);
    CHECK(HoodAnimTick(a, 2, HoodPhase::raising, true, false, 120) == (kHoodDisable | kHoodEnable | kHoodPlayOpen) && a.type == 2 && a.mode == 1);
    // lowering starts while it still unfolds: it finishes (the hood up), then closes
    a = HoodAnim{};
    HoodAnimTick(a, 1, HoodPhase::raising, true, false, 0);
    HoodAnimTick(a, 1, HoodPhase::raising, true, false, 16);
    CHECK(HoodAnimTick(a, 1, HoodPhase::lowering, true, true, 600) == kHoodDisable && a.show == HoodShow::on);
    CHECK(HoodAnimTick(a, 1, HoodPhase::lowering, true, true, 616) == (kHoodEnable | kHoodPlayClose) && a.show == HoodShow::sw);
    // the unfold turned off mid-way: the slot off, the timed swap
    CHECK(HoodAnimTick(a, 1, HoodPhase::lowering, false, true, 700) == kHoodDisable && a.show == HoodShow::none && a.mode == 0);
    // lowering without the hood shown: nothing to close
    a = HoodAnim{};
    CHECK(HoodAnimTick(a, 1, HoodPhase::lowering, true, true, 0) == 0 && a.show == HoodShow::none);
}
}  // namespace

int main() {
    TestFormula();
    TestTypes();
    TestSender();
    TestCarrier();
    TestHoodRules();
    TestCarrierHood();
    TestHoodAnim();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
