// BT sync B0's pure part (native/mod/bt_rules.hpp): the game's crc32c GUID hash (field and gazer ids), the order-free prints, the
// area-200 archive table, the static match, the flag text, the probe modes and the line budget. B1 (round 28 lane B1): the
// bt_sync level, the reset poll and its echo, the epochs, the NOTICED gate, the voidout names, the BTS1 codecs and the FIELD
// compare. No game.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "bt_rules.hpp"

using namespace st::btrules;

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

void TestActorIds() {
    // type 8 slot 0x123: (8 << 13) | 0x123
    const std::uint32_t id = (8u << 13) | 0x123u;
    CHECK(ActorType(id) == kGazerActorType);
    CHECK(ActorSlot(id) == 0x123u);
    CHECK(ActorType(kActorNone) == 0xFFu);
}

void TestCrc() {
    // SSE4.2 crc32 (Castagnoli, reflected 0x82F63B78), init 0, no final xor, one LE u64
    CHECK(Crc32cU64(0, 0) == 0u);
    CHECK(Crc32cU64(0, 0x0123456789ABCDEFull) == 0xE9986AA9u);
    // the bt211_01 scene's GUID (bytes as stored, little-endian Data1..3) hashes to its field id in the archive table
    const std::uint8_t g[16] = {0x79, 0xD8, 0x6B, 0xE5, 0xFC, 0x44, 0x42, 0xA2, 0xA5, 0xC3, 0xF9, 0x25, 0xB2, 0x14, 0x56, 0x18};
    CHECK(GuidHash(g) == 0x390B7DE4u);
    CHECK(FindKnownField(GuidHash(g)) != nullptr);
    CHECK(std::strcmp(FieldName(GuidHash(g)), "bt211_01") == 0);
    CHECK(Le64(g) == 0xA24244FCE56BD879ull);
    const std::uint8_t z[16] = {};
    CHECK(GuidZero(z));
    CHECK(!GuidZero(g));
}

void TestPrints() {
    // Fnv1a over the sorted u32 LE values: order-free
    CHECK(LocatorPrint({1, 2, 3}) == 0x794671B5u);
    CHECK(LocatorPrint({3, 1, 2}) == 0x794671B5u);
    CHECK(LocatorPrint({}) == kFnvSeed);
    CHECK(LocatorPrint({1, 2, 2, 3}) != LocatorPrint({1, 2, 3}));  // a duplicate shows
    CHECK(FieldsPrint({{0x390B7DE4u, 35}, {0xAF0CE561u, 54}}) == 0x62317D90u);
    CHECK(FieldsPrint({{0xAF0CE561u, 54}, {0x390B7DE4u, 35}}) == 0x62317D90u);
    CHECK(FieldsPrint({{0x390B7DE4u, 34}, {0xAF0CE561u, 54}}) != 0x62317D90u);  // a total differs
    // kills: per field the id then its kills, both sorted
    const std::uint32_t a = KillsPrint({{2, {9, 7}}, {1, {5}}});
    const std::uint32_t b = KillsPrint({{1, {5}}, {2, {7, 9}}});
    CHECK(a == b);
    CHECK(KillsPrint({{1, {5}}, {2, {7}}}) != a);
    CHECK(KillsPrint({{1, {}}}) != KillsPrint({}));  // an empty field still counts
}

void TestTable() {
    std::set<std::uint32_t> ids;
    std::set<std::string> names;
    int locators = 0;
    for (const KnownField& f : kKnownFields) {
        ids.insert(f.id);
        names.insert(f.name);
        CHECK(f.locators > 0 && f.locators < 200);
        CHECK(std::fabs(f.cx) < 4000 && std::fabs(f.cy) < 4000);
        CHECK(f.loc_print != kFnvSeed);
        locators += f.locators;
    }
    CHECK(ids.size() == sizeof(kKnownFields) / sizeof(kKnownFields[0]));
    CHECK(names.size() == ids.size());
    CHECK(ids.size() == 25);
    CHECK(locators == 761);
    const KnownField* k = FindKnownField(0x390B7DE4u);
    CHECK(k && k->locators == 35 && k->loc_print == 0xDBE6C47Cu);
    CHECK(FindKnownField(0x12345678u) == nullptr);
    CHECK(std::strcmp(FieldName(0x12345678u), "?") == 0);
}

void TestMatch() {
    FieldSeen f;
    f.id = 0x12345678u;
    CHECK(MatchStatic(f) == StaticMatch::unknown);
    f.id = 0x390B7DE4u;
    f.total = 34;
    CHECK(MatchStatic(f) == StaticMatch::total_differs);
    f.total = 35;
    f.live = 20;
    CHECK(MatchStatic(f) == StaticMatch::partial);
    f.live = 35;
    f.live_print = 0xDBE6C47Cu;
    CHECK(MatchStatic(f) == StaticMatch::all);
    f.live_print = 1;
    CHECK(MatchStatic(f) == StaticMatch::print_differs);
    CHECK(std::strlen(MatchName(StaticMatch::all)) > 0);
    // cleared as the game decides: killed == total (> 0)
    FieldSeen c;
    CHECK(!FieldCleared(c));
    c.total = 3, c.killed = 2;
    CHECK(!FieldCleared(c));
    c.killed = 3;
    CHECK(FieldCleared(c));
    // the unset total (before registration, after ResetAllGazers) is never "cleared" and is its own match
    CHECK(!TotalKnown(kTotalUnset) && !TotalKnown(0) && TotalKnown(35));
    c.total = kTotalUnset, c.killed = kTotalUnset;
    CHECK(!FieldCleared(c));
    f.total = kTotalUnset;
    CHECK(MatchStatic(f) == StaticMatch::total_unset);
    CHECK(std::strlen(MatchName(StaticMatch::total_unset)) > 0);
}

void TestFlags() {
    char b[96];
    FlagsText(0, b, sizeof(b));
    CHECK(std::strcmp(b, " none") == 0);
    FlagsText(0x50u, b, sizeof(b));
    CHECK(std::strstr(b, " react") && !std::strstr(b, "part-react"));
    FlagsText(0x10u, b, sizeof(b));
    CHECK(std::strstr(b, "part-react") != nullptr);
    FlagsText(0x4000000u | 0x2000000u | 0x400000u, b, sizeof(b));
    CHECK(std::strstr(b, "respawn") && std::strstr(b, "caught") && std::strstr(b, "no-kill-list"));
    FlagsText(0x2u, b, sizeof(b));
    CHECK(b[0] == 0);  // a bit we do not name: nothing (the hex is logged next to it)
    // round 28 P3: the spawn bit falls after ~2 s; the host 9.3 m from gazer 40AB1A93: 00090200 -> 00011200 (idle off, near on)
    FlagsText(0x01090200u, b, sizeof(b));
    CHECK(std::strstr(b, " idle") && std::strstr(b, " pending") && !std::strstr(b, " near"));
    FlagsText(0x00011200u, b, sizeof(b));
    CHECK(std::strstr(b, " near") && !std::strstr(b, " idle") && !std::strstr(b, " pending"));
    FlagsText(kFlagDead, b, sizeof(b));
    CHECK(std::strcmp(b, " dead") == 0);
    CHECK(FlagEdge(0x00090200u, 0x00011200u, kFlagNear) == 1);
    CHECK(FlagEdge(0x00090200u, 0x00011200u, kFlagIdle) == -1);
    CHECK(FlagEdge(0x01090200u, 0x00090200u, kFlagNear) == 0);
    CHECK(FlagEdge(0x00011200u, 0x00010200u, kFlagNear) == -1);
    FlagsText(0x50u, b, 4);  // a short buffer stays terminated
    CHECK(std::strlen(b) == 3);
    FlagsText(0x50u, nullptr, 0);
}

void TestModes() {
    CHECK(ProbeMode(0) == 0);
    CHECK(ProbeMode(1) == 1);
    CHECK(ProbeMode(2) == 2);
    CHECK(ProbeMode(7) == 2);
    CHECK(ProbeMode(-1) == 0);
    CHECK(ProbeMode(0.5) == 0);
    CHECK(ProbeMode(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(ProbeIntervalMs(0) == 1000.0);
    CHECK(ProbeIntervalMs(-3) == 1000.0);
    CHECK(ProbeIntervalMs(0.1) == 250.0);
    CHECK(ProbeIntervalMs(2) == 2000.0);
    CHECK(ProbeIntervalMs(100) == 30000.0);
    CHECK(ProbeIntervalMs(std::numeric_limits<double>::quiet_NaN()) == 1000.0);
}

void TestBudget() {
    LineBudget b;
    b.cap = 3;
    int took = 0;
    for (int i = 0; i < 5; ++i) took += b.Take(100.0) ? 1 : 0;
    CHECK(took == 3);
    CHECK(b.TakeDropped() == 2);
    CHECK(b.TakeDropped() == 0);
    CHECK(!b.Take(900.0));         // the same window
    CHECK(b.Take(1100.0));         // a new one
    CHECK(b.TakeDropped() == 1);
}

void TestGeometry() {
    const double a[3] = {0, 0, 0}, b[3] = {3, 4, 0};
    CHECK(std::fabs(Dist3(a, b) - 5.0) < 1e-12);
    // the scenario's spots (bt211_01): the host 8.6 m from gazer bt211_01_bt_n_mt2_01, the guest's spot (@guest goto -1046 36 0)
    // about 60 m away, and the field edge (place bt-field-mknot-s, the host's first stop) 56 m from it
    const double gazer[3] = {-986.58, 58.11, 0}, host[3] = {-978.0, 58.0, 0}, guest[3] = {-1046.0, 36.0, 0};
    const double field_edge[3] = {-1040.0, 40.0, 0};
    CHECK(Dist3(gazer, host) < 10.0);
    CHECK(Dist3(gazer, guest) > 60.0 && Dist3(gazer, guest) < 65.0);
    CHECK(Dist3(gazer, field_edge) > 50.0);
    CHECK(PosSane(b));
    const double bad[3] = {std::numeric_limits<double>::infinity(), 0, 0}, far[3] = {2e7, 0, 0};
    CHECK(!PosSane(bad));
    CHECK(!PosSane(far));
    // the stillness meter (+0x230, 1.000 at rest with no gazer) is not perception: only the near bit or the hunter area count
    Perceive p;
    CHECK(!p.any());
    p.near_n = 1;
    CHECK(p.any());
    p.near_n = 0, p.hunter = true;
    CHECK(p.any());
}

// ================================================================ B1
void TestSyncLevel() {
    CHECK(SyncLevel(0) == 0);
    CHECK(SyncLevel(1) == 1);
    CHECK(SyncLevel(2) == 2);
    CHECK(SyncLevel(9) == 2);
    CHECK(SyncLevel(0.9) == 0);
    CHECK(SyncLevel(-1) == 0);
    CHECK(SyncLevel(std::numeric_limits<double>::quiet_NaN()) == 0);
}

void TestResetPoll() {
    // round 28 P3: 31 records, 18 unset at load; dev_enemies 3 -> 31 of 31 unset, the ids the same
    const RecSummary a{31, 18, 0, 0x1234u}, b{31, 31, 0, 0x1234u};
    CHECK(LooksLikeReset(a, b));
    CHECK(!LooksLikeReset(b, b));                                           // already all unset: no new reset
    CHECK(!LooksLikeReset(a, RecSummary{31, 31, 0, 0x9999u}));              // another record set (a load)
    CHECK(!LooksLikeReset(a, RecSummary{30, 30, 0, 0x1234u}));              // a different count
    CHECK(!LooksLikeReset(a, RecSummary{31, 30, 0, 0x1234u}));              // not all unset
    CHECK(!LooksLikeReset(a, RecSummary{31, 31, 2, 0x1234u}));              // kills left: not a reset
    CHECK(!LooksLikeReset(RecSummary{1, 0, 0, 7}, RecSummary{1, 1, 0, 7}));  // one record is too little to tell
    CHECK(!LooksLikeReset(b, a));                                           // a re-registration (unset falls)
    // the echo window after an applied RESET
    CHECK(ResetEcho(1000.0, 1000.0));
    CHECK(ResetEcho(5999.0, 1000.0));
    CHECK(!ResetEcho(6001.0, 1000.0));
    CHECK(!ResetEcho(900.0, 1000.0));
    CHECK(!ResetEcho(1000.0, -1e18));
    // wrap-safe epochs
    CHECK(EpochNewer(2, 1));
    CHECK(!EpochNewer(1, 1));
    CHECK(!EpochNewer(1, 2));
    CHECK(EpochNewer(1, 0xFFFF));
    CHECK(EpochNewer(0x10, 0xFFF0));
}

void TestNoticeGate() {
    NoticeGate g;
    CHECK(g.Take(0x390B7DE4u, 0x40AB1A93u, 0.0));
    CHECK(!g.Take(0x390B7DE4u, 0x11111111u, 1000.0));   // the same field within 5 s
    CHECK(g.Take(0xAF0CE561u, 0x22222222u, 1000.0));    // another field
    CHECK(g.Take(0x390B7DE4u, 0x11111111u, 5000.0));    // the field again after 5 s, another gazer
    CHECK(!g.Take(0x390B7DE4u, 0x40AB1A93u, 10500.0));  // the first gazer within 15 s
    CHECK(g.Take(0x390B7DE4u, 0x40AB1A93u, 15000.0));
    // many keys: the round robin reuses slots, never refuses a new key
    NoticeGate h;
    int took = 0;
    for (std::uint32_t i = 1; i <= 100; ++i) took += h.Take(i, 1000 + i, 0.0) ? 1 : 0;
    CHECK(took == 100);
}

void TestVoidNames() {
    CHECK(VoidReason(true, false, false) == 13);
    CHECK(VoidReason(true, true, true) == 14);
    CHECK(VoidReason(false, false, true) == 3);
    CHECK(VoidReason(false, true, false) == 16);
    CHECK(std::strcmp(VoidReasonName(13), "GazerAnnihilation") == 0);
    CHECK(std::strcmp(VoidReasonName(kVoidReasonTest), "test") == 0);
    CHECK(std::strcmp(VoidReasonName(99), "?") == 0);
    CHECK(std::strcmp(VoidSourceName(kVoidSrcCatch), "gazer catch") == 0);
    CHECK(std::strcmp(VoidSourceName(77), "unknown caller") == 0);
    CHECK(VoidToast(kVoidSrcCatch) && VoidToast(kVoidSrcCatcher) && VoidToast(kVoidSrcB3B0) && VoidToast(kVoidSrcTest));
    CHECK(!VoidToast(kVoidSrcPending) && !VoidToast(kVoidSrcUnknown));
}

FieldMsg SampleField() {
    FieldMsg m;
    m.epoch = 1, m.applied = 0, m.fields = 31, m.unset = 18, m.cleared = 0, m.live = 35;
    m.ids = 0xEE2FFFB0u, m.kills = 0xE7FBB6B9u, m.locs = 0xDBE6C47Cu;
    m.n = 2;
    m.e[0] = {0x390B7DE4u, 35, 0, 35, 0xDBE6C47Cu};
    m.e[1] = {0xAF0CE561u, 3, 1, 54, 0x12345678u};
    return m;
}

void TestCodecs() {
    std::uint8_t b[kBtsMaxSize + 8] = {};
    BtsHeader h;
    // HELLO
    BtsHello hi;
    hi.level = 2, hi.role = 2, hi.caps = kCapField | kCapNoticed | kCapVoid | kCapResetApply;
    std::size_t n = EncodeHello(7, hi, b, sizeof(b));
    CHECK(n == kBtsHelloSize);
    CHECK(LooksLikeBts(b, n) && DecodeBtsHeader(b, n, h) && h.type == kBtsHello && h.seq == 7);
    BtsHello ho;
    CHECK(DecodeHello(b, n, ho) && ho.level == 2 && ho.role == 2 && ho.caps == 15);
    CHECK(!DecodeHello(b, n - 1, ho));
    hi.level = 0;
    CHECK(EncodeHello(7, hi, b, sizeof(b)) == 0);
    hi.level = 1, hi.role = 3;
    CHECK(EncodeHello(7, hi, b, sizeof(b)) == 0);
    // FIELD: round trip, n 0 and 6, the exact size
    const FieldMsg f = SampleField();
    n = EncodeField(9, f, b, sizeof(b));
    CHECK(n == kBtsFieldBase + 2 * kBtsFieldEntry);
    FieldMsg g;
    CHECK(DecodeField(b, n, g) && SameContent(f, g) && g.n == 2 && g.e[1].total == 54);
    CHECK(!DecodeField(b, n + 1, g));
    CHECK(!DecodeHello(b, n, ho));  // the wrong type
    FieldMsg big = f;
    big.n = kBtsFieldMax;
    for (unsigned i = 0; i < big.n; ++i) big.e[i] = {i + 1, 1, 0, 1, i};
    CHECK(EncodeField(1, big, b, sizeof(b)) == kBtsMaxSize);
    CHECK(DecodeField(b, kBtsMaxSize, g) && g.n == kBtsFieldMax);
    big.n = kBtsFieldMax + 1;
    CHECK(EncodeField(1, big, b, sizeof(b)) == 0);
    const FieldMsg empty;
    CHECK(EncodeField(1, empty, b, sizeof(b)) == kBtsFieldBase);
    CHECK(DecodeField(b, kBtsFieldBase, g) && g.n == 0);
    FieldMsg odd = f;
    odd.unset = 40;  // more unset than records
    n = EncodeField(1, odd, b, sizeof(b));
    CHECK(n > 0 && !DecodeField(b, n, g));
    CHECK(EncodeField(1, f, b, kBtsFieldBase) == 0);  // no room
    // RESET
    BtsReset r{3, 31, 0xEE2FFFB0u}, ro;
    n = EncodeReset(4, r, b, sizeof(b));
    CHECK(n == kBtsResetSize && DecodeReset(b, n, ro) && ro.epoch == 3 && ro.fields == 31 && ro.ids == 0xEE2FFFB0u);
    r.epoch = 0;
    CHECK(EncodeReset(4, r, b, sizeof(b)) == 0);
    // NOTICED: round trip, NaN refused both ways, the pad checked
    BtsNoticed no;
    no.kind = kNoticeNear, no.near_n = 1, no.field = 0x390B7DE4u, no.loc = 0x40AB1A93u;
    no.gazer[0] = -986.58f, no.gazer[1] = 58.11f, no.gazer[2] = 0;
    no.player[0] = -978.0f, no.player[1] = 58.0f, no.player[2] = 0;
    no.game_dist = 8.6f;
    n = EncodeNoticed(5, no, b, sizeof(b));
    CHECK(n == kBtsNoticedSize);
    BtsNoticed nr;
    CHECK(DecodeNoticed(b, n, nr) && nr.loc == 0x40AB1A93u && nr.field == 0x390B7DE4u && nr.gazer[0] == no.gazer[0] && nr.game_dist == no.game_dist);
    b[14] = 1;  // the pad
    CHECK(!DecodeNoticed(b, n, nr));
    no.kind = 3;
    CHECK(EncodeNoticed(5, no, b, sizeof(b)) == 0);
    no.kind = kNoticeTest;
    no.player[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK(EncodeNoticed(5, no, b, sizeof(b)) == 0);
    no.player[1] = 58.0f;
    n = EncodeNoticed(5, no, b, sizeof(b));
    CHECK(n == kBtsNoticedSize);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(b + 12 + 12 + 4, &nan, 4);  // gazer[1] on the wire
    CHECK(!DecodeNoticed(b, n, nr));
    // VOID
    BtsVoid v;
    v.reason = 13, v.source = kVoidSrcCatch, v.flags = kVoidGazer, v.pos[0] = 1, v.pos[1] = 2, v.pos[2] = 3, v.player_dist = 4, v.far_dist = 500;
    v.sender_ms = 123456;
    n = EncodeVoid(6, v, b, sizeof(b));
    CHECK(n == kBtsVoidSize);
    BtsVoid vr;
    CHECK(DecodeVoid(b, n, vr) && vr.reason == 13 && vr.source == kVoidSrcCatch && vr.flags == kVoidGazer && vr.far_dist == 500 &&
          vr.sender_ms == 123456);
    b[15] = 1;  // the pad
    CHECK(!DecodeVoid(b, n, vr));
    b[15] = 0;
    b[14] = 0x10;  // an unknown flag
    CHECK(!DecodeVoid(b, n, vr));
    v.far_dist = std::numeric_limits<float>::infinity();
    CHECK(EncodeVoid(6, v, b, sizeof(b)) == 0);
    // the header: another magic, version, pad, type; SCB1 is not BTS1
    v.far_dist = 500;
    n = EncodeVoid(6, v, b, sizeof(b));
    CHECK(DecodeBtsHeader(b, n, h) && h.seq == 6);
    b[5] = 2;
    CHECK(!DecodeBtsHeader(b, n, h));
    b[5] = 1, b[7] = 1;
    CHECK(!DecodeBtsHeader(b, n, h));
    b[7] = 0, b[4] = 9;
    CHECK(!DecodeBtsHeader(b, n, h));
    b[4] = kBtsVoid, b[3] = '2';
    CHECK(!LooksLikeBts(b, n) && !DecodeBtsHeader(b, n, h));
    const std::uint8_t scb[12] = {'S', 'C', 'B', '1', 1, 1, 0, 0, 0, 0, 0, 0};
    CHECK(!LooksLikeBts(scb, sizeof(scb)));
    CHECK(!LooksLikeBts(nullptr, 20) && !LooksLikeBts(b, 11));
}

void TestCompare() {
    const FieldMsg here = SampleField();
    FieldMsg there = SampleField();
    FieldCompare c = CompareFields(here, there);
    CHECK(c.records && c.kills && c.live_equal == 2 && c.live_differs == 0 && c.live_not_here == 0);
    CHECK(std::strcmp(EntryCompareName(here, there.e[0]), "print equal") == 0);
    const std::uint32_t k0 = c.Key(here, there);
    there.e[1].print = 1;  // the partner sees another set in that field
    c = CompareFields(here, there);
    CHECK(c.live_equal == 1 && c.live_differs == 1);
    CHECK(std::strcmp(EntryCompareName(here, there.e[1]), "print DIFFERS") == 0);
    CHECK(c.Key(here, there) != k0);
    there.e[1].id = 0x5555u;  // a field not streamed here
    c = CompareFields(here, there);
    CHECK(c.live_not_here == 1);
    CHECK(std::strcmp(EntryCompareName(here, there.e[1]), "not here") == 0);
    there = SampleField();
    there.ids = 1;
    CHECK(!CompareFields(here, there).records);
    there = SampleField();
    there.kills = 2;
    CHECK(!CompareFields(here, there).kills);
    // the unset counts and the epochs enter the key (the line logs again when a reset lands)
    there = SampleField();
    const FieldCompare same = CompareFields(here, there);
    CHECK(same.Key(here, there) == k0);
    there.unset = 31;
    CHECK(same.Key(here, there) != k0);
    there = SampleField();
    there.applied = 1;
    CHECK(same.Key(here, there) != k0);
    // SameContent: any field counts
    const FieldMsg a = SampleField();
    FieldMsg b = SampleField();
    CHECK(SameContent(a, b));
    b.e[0].killed = 1;
    CHECK(!SameContent(a, b));
    b = SampleField();
    b.locs = 0;
    CHECK(!SameContent(a, b));
    CHECK(FindEntry(a, 0x390B7DE4u) == &a.e[0] && FindEntry(a, 7) == nullptr);
}

}  // namespace

int main() {
    TestActorIds();
    TestCrc();
    TestPrints();
    TestTable();
    TestMatch();
    TestFlags();
    TestModes();
    TestBudget();
    TestGeometry();
    TestSyncLevel();
    TestResetPoll();
    TestNoticeGate();
    TestVoidNames();
    TestCodecs();
    TestCompare();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
