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
    // round 30: renamed, the wire numbers kept
    static_assert(kVoidSrcScripted == 1 && kVoidSrcCatch == 2 && kVoidSrcCorpseNecrosis == 3 && kVoidSrcMuleCorpse == 4 && kVoidSrcTest == 0xFF,
                  "the wire numbers");
    CHECK(std::strcmp(VoidSourceName(kVoidSrcScripted), "scripted / timed") == 0);
    CHECK(std::strcmp(VoidSourceName(kVoidSrcCorpseNecrosis), "corpse necrosis (manager, 3 s)") == 0);
    CHECK(std::strcmp(VoidSourceName(kVoidSrcMuleCorpse), "MULE corpse (necrosis)") == 0);
    CHECK(std::strcmp(VoidSourceName(kVoidSrcTest), "test") == 0);
}

// Round 30: the partner VOID's verdict (a toast only with the partner's GameOver within 5 s).
void TestVoidVerdict() {
    CHECK(VoidVerdict(true, 1000, -1, 1000) == VoidCall::test);       // a test: at once, whatever the beacon
    CHECK(VoidVerdict(true, 1000, 1500, 9000) == VoidCall::test);
    CHECK(VoidVerdict(false, 1000, -1, 1000) == VoidCall::wait);      // nothing yet: wait
    CHECK(VoidVerdict(false, 1000, -1, 5999) == VoidCall::wait);
    CHECK(VoidVerdict(false, 1000, -1, 6000) == VoidCall::log_only);  // 5 s and no GameOver: log only
    CHECK(VoidVerdict(false, 1000, 3000, 3000) == VoidCall::partner);  // GameOver 2 s after
    CHECK(VoidVerdict(false, 10000, 6000, 10000) == VoidCall::partner);  // GameOver 4 s before (the beacon came first)
    CHECK(VoidVerdict(false, 10000, 5000, 10000) == VoidCall::partner);  // exactly 5 s before
    CHECK(VoidVerdict(false, 10000, 4000, 10000) == VoidCall::wait);    // an old GameOver: not this VOID's
    CHECK(VoidVerdict(false, 10000, 4000, 15000) == VoidCall::log_only);
    CHECK(VoidVerdict(false, 1000, 6500, 7000) == VoidCall::log_only);  // a GameOver 5.5 s after: too late
    static_assert(kBeaconDownDead == 1 && kVoidDownWindowMs == 5000.0, "the window");
    CHECK(std::strcmp(VoidCallName(VoidCall::partner), "partner") == 0 && std::strcmp(VoidCallName(VoidCall::log_only), "log only") == 0);
    // round 30 review: no down channel (enemy_sync / hits / restrain relay off on either side): at once, round 28's rule by source
    CHECK(VoidVerdict(false, 1000, -1, 1000, false, kVoidSrcCatch) == VoidCall::no_channel_toast);
    CHECK(VoidVerdict(false, 1000, -1, 1000, false, kVoidSrcScripted) == VoidCall::no_channel_toast);
    CHECK(VoidVerdict(false, 1000, -1, 1000, false, kVoidSrcCorpseNecrosis) == VoidCall::no_channel_log);
    CHECK(VoidVerdict(false, 1000, -1, 1000, false, kVoidSrcMuleCorpse) == VoidCall::no_channel_log);
    CHECK(VoidVerdict(false, 1000, -1, 1000, false, kVoidSrcUnknown) == VoidCall::no_channel_log);
    CHECK(VoidVerdict(true, 1000, -1, 1000, false, kVoidSrcCorpseNecrosis) == VoidCall::test);       // a test still a test
    CHECK(VoidVerdict(false, 1000, 3000, 3000, false, kVoidSrcCorpseNecrosis) == VoidCall::partner);  // a GameOver seen still wins
    CHECK(VoidVerdict(false, 1000, -1, 1000, true, kVoidSrcCatch) == VoidCall::wait);                 // with the channel: wait as before
    CHECK(VoidMayBePlayer(kVoidSrcCatch) && VoidMayBePlayer(kVoidSrcScripted) && !VoidMayBePlayer(kVoidSrcMuleCorpse) && !VoidMayBePlayer(kVoidSrcTest));
}

// Round 30 BT S0: the probe ops and their pure gates.
void TestProbeOps() {
    CHECK(ProbeOp(0) == 0 && ProbeOp(1) == 1 && ProbeOp(2) == 2);
    CHECK(ProbeOp(7) == 2 && ProbeOp(-1) == 0 && ProbeOp(8) == 2);  // ProbeMode's clamp, as before
    CHECK(ProbeOp(9) == kOpThreads && ProbeOp(10) == kOpKill && ProbeOp(11) == kOpCrater && ProbeOp(12) == kOpWeather);
    CHECK(ProbeOp(13) == kOpUnbuilt && ProbeOp(14) == kOpHunter);
    CHECK(ProbeOp(9.5) == 2 && ProbeOp(15) == 2 && ProbeOp(std::nan("")) == 0);

    std::uint32_t loc = 1;
    CHECK(LocArg(0x40AB, 0x1A93, &loc) && loc == 0x40AB1A93u);
    CHECK(LocArg(0, 0, &loc) && loc == 0);
    CHECK(LocArg(0x40AB1A93 & 0x3FFFFFFF, 0, &loc) && loc == (0x40AB1A93u & 0x3FFFFFFFu));  // a whole hash under 1e9
    CHECK(!LocArg(0x10000, 1, &loc));       // a whole hash with a low half
    CHECK(!LocArg(1, 0x10000, &loc));       // a low half too big
    CHECK(!LocArg(-1, 0, &loc) && !LocArg(1.5, 0, &loc) && !LocArg(std::nan(""), 0, &loc) && !LocArg(1, 1, nullptr));

    CHECK(KillGuard(0, 10, 10, 0x1000) == kKillOk);
    CHECK(KillGuard(2, 10, 10, 0) == kKillState);
    CHECK(KillGuard(0, 0, 10, 0) == kKillHealth && KillGuard(0, std::nanf(""), 10, 0) == kKillHealth);
    CHECK(KillGuard(0, 10, 0, 0) == kKillPrev);
    CHECK(KillGuard(0, 10, 10, 0x1) == kKillFlags && KillGuard(0, 10, 10, 0x8) == kKillFlags);
    CHECK(std::strcmp(KillGuardText(kKillOk), "ok") == 0 && std::strcmp(KillGuardText(99), "?") == 0);
    static_assert(kKillRainGone == 0x01322187u && kKillCrater == 0x5414860Du, "the kill reasons");

    CHECK(CraterGate(true, true, 250, 200) == nullptr);
    CHECK(CraterGate(true, true, 200, 200) == nullptr);
    CHECK(CraterGate(false, true, 250, 200) != nullptr);
    CHECK(CraterGate(true, false, 250, 200) != nullptr);
    CHECK(CraterGate(true, true, 150, 200) != nullptr);
    CHECK(CraterGate(true, true, 250, -1) != nullptr && CraterGate(true, true, 250, std::nan("")) != nullptr);

    const double me[3] = {10, 20, 5};
    double out[3] = {0, 0, 0};
    CHECK(AheadOf(me, 0, 2, 30, out) && out[0] == 10 && out[1] == 50 && out[2] == 5);
    CHECK(AheadOf(me, 3, 4, 10, out) && std::fabs(out[0] - 16) < 1e-9 && std::fabs(out[1] - 28) < 1e-9);
    CHECK(!AheadOf(me, 0, 0, 10, out) && !AheadOf(nullptr, 1, 0, 10, out));
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
    CHECK(EncodeField(1, big, b, sizeof(b)) == kBtsFieldBase + kBtsFieldMax * kBtsFieldEntry);
    CHECK(DecodeField(b, kBtsFieldBase + kBtsFieldMax * kBtsFieldEntry, g) && g.n == kBtsFieldMax);
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

// Round 30 BT S1 / S2: the TROUBLE / GZST / KILL codecs, the caps, the size bound.
void TestS1S2Codecs() {
    static_assert(kBtsMaxSize == 240, "GZST's most");
    static_assert(kBtsTroubleSize == kBtsHeader + 32 && kBtsKillSize == kBtsHeader + 36 && kBtsGzstBase == kBtsHeader + 4, "sizes");
    static_assert((kCapTrouble | kCapGzst | kCapKill | kCapKillApply) == 0xF0, "caps");
    std::uint8_t b[kBtsMaxSize + 8] = {};
    BtsHeader h;
    // TROUBLE
    BtsTrouble t;
    t.kind = kTrCatcher, t.flags = kTrfGold | kTrfTest, t.episode = 3, t.pos[0] = 1.5f, t.pos[1] = -2, t.pos[2] = 30, t.hp = 60, t.field = 0x390B7DE4,
    t.loc = 0x40AB1A93;
    std::size_t n = EncodeTrouble(5, t, b, sizeof(b));
    CHECK(n == kBtsTroubleSize);
    CHECK(DecodeBtsHeader(b, n, h) && h.type == kBtsTrouble && h.seq == 5);
    BtsTrouble u;
    CHECK(DecodeTrouble(b, n, u) && u.kind == kTrCatcher && u.flags == 9 && u.episode == 3 && u.hp == 60 && u.pos[2] == 30 && u.loc == 0x40AB1A93);
    CHECK(!DecodeTrouble(b, n - 1, u));
    b[13] = 0x10;  // an unknown flag
    CHECK(!DecodeTrouble(b, n, u));
    t.kind = kTrPulled;  // never sent
    CHECK(EncodeTrouble(5, t, b, sizeof(b)) == 0);
    t.kind = 9;
    CHECK(EncodeTrouble(5, t, b, sizeof(b)) == 0);
    t.kind = kTrHunted, t.hp = std::numeric_limits<float>::quiet_NaN();
    CHECK(EncodeTrouble(5, t, b, sizeof(b)) == 0);
    t.hp = -1, t.flags = 0x10;
    CHECK(EncodeTrouble(5, t, b, sizeof(b)) == 0);
    t.flags = 0;
    CHECK(EncodeTrouble(5, t, b, kBtsTroubleSize - 1) == 0);
    CHECK(std::strcmp(TroubleName(kTrCatcher), "CATCHER") == 0 && std::strcmp(TroubleName(kTrClear), "CLEAR") == 0);
    // GZST: n 0, 1, 8 (the biggest message), 9 refused
    BtsGzst g;
    g.sender_ms = 1234;
    n = EncodeGzst(6, g, b, sizeof(b));
    CHECK(n == kBtsGzstBase);
    BtsGzst gg;
    CHECK(DecodeGzst(b, n, gg) && gg.n == 0 && gg.sender_ms == 1234);
    g.n = kBtsGzstMax;
    for (unsigned i = 0; i < g.n; ++i) {
        g.e[i].field = 0x390B7DE4, g.e[i].loc = 0x1000 + i, g.e[i].state = 0, g.e[i].mode = 1, g.e[i].bits = kGzNear | kGzReact;
        g.e[i].pos[0] = static_cast<float>(i), g.e[i].approach = 0.5f;
    }
    n = EncodeGzst(6, g, b, sizeof(b));
    CHECK(n == kBtsMaxSize);
    CHECK(DecodeBtsHeader(b, n, h) && h.type == kBtsGzst && h.n == kBtsGzstMax);
    CHECK(DecodeGzst(b, n, gg) && gg.n == 8 && gg.e[7].loc == 0x1007 && gg.e[7].bits == 5 && gg.e[7].pos[0] == 7 && gg.e[3].mode == 1);
    CHECK(!DecodeGzst(b, n + 1, gg));
    b[kBtsGzstBase + 11] = 1;  // the first entry's pad
    CHECK(!DecodeGzst(b, n, gg));
    g.n = kBtsGzstMax + 1;
    CHECK(EncodeGzst(6, g, b, sizeof(b)) == 0);
    g.n = 1, g.e[0].bits = 0x40;
    CHECK(EncodeGzst(6, g, b, sizeof(b)) == 0);
    // KILL
    BtsKill k;
    k.field = 0x390B7DE4, k.loc = 0x40AB1A93, k.reason = kKillStatsA, k.world = 0xABCD, k.cause = kCauseStats, k.flags = kKfSandboxed | kKfTest;
    k.pos[0] = -978, k.pos[1] = 58, k.pos[2] = 1, k.sender_ms = 99;
    n = EncodeKill(7, k, b, sizeof(b));
    CHECK(n == kBtsKillSize);
    BtsKill kk;
    CHECK(DecodeKill(b, n, kk) && kk.loc == 0x40AB1A93 && kk.reason == kKillStatsA && kk.world == 0xABCD && kk.cause == kCauseStats &&
          kk.flags == 0x81 && kk.pos[0] == -978 && kk.sender_ms == 99);
    CHECK(!DecodeKill(b, n - 1, kk));
    b[kBtsHeader + 18] = 1;  // the pad
    CHECK(!DecodeKill(b, n, kk));
    k.loc = 0;
    CHECK(EncodeKill(7, k, b, sizeof(b)) == 0);
    k.loc = 1, k.cause = 0;
    CHECK(EncodeKill(7, k, b, sizeof(b)) == 0);
    k.cause = kCausePlayer, k.flags = 0x02;
    CHECK(EncodeKill(7, k, b, sizeof(b)) == 0);
    // an older build's decoder knew types 1..5 only; ours still refuses type 9
    k.flags = 0;
    n = EncodeKill(7, k, b, sizeof(b));
    b[4] = 9;
    CHECK(!DecodeBtsHeader(b, n, h));
}

void TestGzBits() {
    CHECK(GzBits(0x1000, 0) == kGzNear);
    CHECK(GzBits(0x50, 0) == kGzReact && GzBits(0x10, 0) == 0);
    CHECK(GzBits(0x2000000 | 0x4, 0) == (kGzCaught | kGzRealized));
    CHECK(GzBits(0, 2) == kGzDead && GzBits(1, 0) == kGzDead);
    CHECK(GzInteresting(kGzNear) && GzInteresting(kGzTest) && !GzInteresting(0) && !GzInteresting(kGzNear | kGzDead) && !GzInteresting(kGzRealized));
    char s[96];
    GzBitsText(kGzNear | kGzCaught, s, sizeof(s));
    CHECK(std::strcmp(s, " near caught") == 0);
    GzBitsText(0, s, sizeof(s));
    CHECK(std::strstr(s, "ended") != nullptr);
}

void TestTrouble() {
    TroubleTracker tr;
    TroubleIn in;
    float p = 0;
    CHECK(tr.Step(in, 0, &p) == 0 && p == -1);  // nothing, no CLEAR before any trouble
    in.hunted = true;
    CHECK(tr.Step(in, 1000, &p) == kTrHunted && tr.episode == 1);
    CHECK(tr.Step(in, 1500, &p) == 0);           // no change, no keepalive yet
    CHECK(tr.Step(in, 2000, &p) == kTrHunted);   // the keepalive
    in.catcher = true, in.catcher_hp = 800;
    CHECK(tr.Step(in, 2100, &p) == kTrCatcher && p == 100 && tr.episode == 1);
    in.catcher_hp = 480;
    CHECK(tr.Step(in, 2300, &p) == 0);           // 2 Hz at most
    CHECK(tr.Step(in, 2700, &p) == kTrCatcher && std::fabs(p - 60) < 0.01f);
    CHECK(tr.Step(in, 3300, &p) == 0);           // same health, keepalive at 1 s
    CHECK(tr.Step(in, 3700, &p) == kTrCatcher);
    in.catcher = false, in.caught = true;
    CHECK(tr.Step(in, 3800, &p) == kTrCaught && p == -1);
    in = TroubleIn{};
    CHECK(tr.Step(in, 3900, &p) == kTrClear);
    CHECK(tr.Step(in, 5000, &p) == 0);            // CLEAR once
    in.hunted = true;
    CHECK(tr.Step(in, 6000, &p) == kTrHunted && tr.episode == 2);
    tr.episode = 0xFFFF;
    in = TroubleIn{};
    tr.Step(in, 7000, &p);
    in.caught = true;
    CHECK(tr.Step(in, 8000, &p) == kTrCaught && tr.episode == 1);  // never 0
    in.catcher = true, in.catcher_hp = std::numeric_limits<float>::quiet_NaN();
    CHECK(tr.Step(in, 9000, &p) == kTrCatcher && p == 0);
    // the receiver
    TroubleView v;
    BtsTrouble m;
    m.kind = kTrHunted, m.episode = 4;
    TroubleView::Out o = v.Take(m, 0);
    CHECK(o.log && !o.toast);
    o = v.Take(m, 900);
    CHECK(!o.log && !o.toast);
    m.kind = kTrCatcher, m.hp = 100;
    o = v.Take(m, 1500);
    CHECK(o.log && o.toast);
    m.hp = 95;
    o = v.Take(m, 2000);
    CHECK(!o.log && !o.toast);  // under 10 points
    m.hp = 60;
    o = v.Take(m, 3000);
    CHECK(o.log && !o.toast);   // one toast per episode
    m.kind = kTrCaught, m.hp = -1;
    o = v.Take(m, 3500);
    CHECK(o.log && !o.toast);
    CHECK(!v.Expire(6000) && v.Expire(6600) && !v.Expire(7000));
    m.kind = kTrClear;
    o = v.Take(m, 7000);
    CHECK(!o.log);  // already expired
    m.kind = kTrCaught, m.episode = 5;
    o = v.Take(m, 8000);
    CHECK(o.log && o.toast);  // a new episode
    m.kind = kTrClear;
    o = v.Take(m, 8100);
    CHECK(o.log);
}

void TestMarkers() {
    MarkerBook bk;
    MarkEntry e;
    for (unsigned i = 0; i < kMarkCap; ++i) {
        e.key = GazerMarkKey(i + 1), e.kind = kMarkGazer, e.seen_ms = 100.0 * i;
        bk.Put(e);
    }
    CHECK(bk.n == kMarkCap);
    e.key = kMarkKeyTar, e.kind = kMarkTar, e.seen_ms = 1000;
    bk.Put(e);  // past the cap: replaces the stalest (key 1)
    CHECK(bk.n == kMarkCap);
    bool has1 = false, tar = false;
    for (unsigned i = 0; i < bk.n; ++i) has1 |= bk.e[i].key == 1, tar |= bk.e[i].key == kMarkKeyTar;
    CHECK(!has1 && tar);
    e.seen_ms = 5000;
    bk.Put(e);  // a refresh, not a new entry
    CHECK(bk.n == kMarkCap);
    bk.Expire(3650);  // 3 s after: keys 2..8 at 100..700 ms are gone at > 3000 ms
    CHECK(bk.n == 2);
    bk.Expire(7000);
    CHECK(bk.n == 1 && bk.e[0].key == kMarkKeyTar);
    bk.Expire(8001);
    CHECK(bk.n == 0);
    e.key = 5, e.kind = kMarkGazer, e.seen_ms = 0;
    bk.Put(e);
    e.key = kMarkKeyCatcher, e.kind = kMarkCatcher;
    bk.Put(e);
    bk.Drop(5);
    CHECK(bk.n == 1 && bk.e[0].kind == kMarkCatcher);
    bk.DropKind(kMarkCatcher);
    CHECK(bk.n == 0);
}

void TestKillRules() {
    std::uint8_t cause = 9;
    // the relay rule
    CHECK(KillRelay(kKillStatsA, 0, false, 0, kTagNone, false, 1e9, &cause) == nullptr && cause == kCauseStats);
    CHECK(KillRelay(kKillStatsB, 0, false, 0, kTagNone, false, -1, &cause) == nullptr && cause == kCauseStats);
    CHECK(KillRelay(0x1234, 0x20005, true, 0x20005, kTagNone, false, 1e9, &cause) == nullptr && cause == kCausePlayer);
    CHECK(KillRelay(0x1234, 0x20005, false, 0x20005, kTagNone, false, 1e9, &cause) != nullptr && cause == 0);  // no player id
    CHECK(KillRelay(0x1234, 0x1000E, true, 0x20005, kTagNone, false, 1e9, &cause) != nullptr);
    CHECK(KillRelay(kKillRainGone, 0x20005, true, 0x20005, kTagNone, false, 1e9, &cause) != nullptr);  // the reason wins
    CHECK(KillRelay(kKillCrater, 0, true, 0x20005, kTagNone, false, 1e9, &cause) != nullptr);
    CHECK(KillRelay(kKillStatsA, 0, true, 1, kTagNone, false, 4999, &cause) != nullptr);  // after a reset
    CHECK(KillRelay(kKillStatsA, 0, true, 1, kTagNone, true, 1e9, &cause) != nullptr);    // an echo
    CHECK(KillRelay(kKillStatsA, 0, true, 1, kTagMirror, false, 1e9, &cause) != nullptr);
    CHECK(KillRelay(kKillStatsA, 0, true, 1, kTagProbe, false, 1e9, &cause) != nullptr);
    CHECK(KillRelay(kKillRainGone, 0, false, 0, kTagTest, false, 0, &cause) == nullptr && cause == kCauseTest);
    CHECK(KillRelay(kKillRainGone, 0, false, 0, kTagTest, true, 0, &cause) != nullptr);  // the test is not sent back either
    // the gate: the guest
    KillApplyIn in;
    in.bt_kill = 2, in.role = 2, in.sandboxed = true, in.streamed = true, in.dist_m = 66;
    CHECK(KillApplyGate(in).call == KillCall::apply);
    KillApplyIn x = in;
    x.bt_kill = 1;
    CHECK(KillApplyGate(x).call == KillCall::refuse);
    x = in, x.sandboxed = false;
    CHECK(KillApplyGate(x).call == KillCall::refuse && std::strstr(KillApplyGate(x).why, "real save"));
    x = in, x.role = 0;
    CHECK(KillApplyGate(x).call == KillCall::refuse);
    x = in, x.not_playing = "loading";
    CHECK(KillApplyGate(x).call == KillCall::hold && std::strcmp(KillApplyGate(x).why, "loading") == 0);
    x.age_ms = 60001;
    CHECK(KillApplyGate(x).call == KillCall::refuse);
    x = in, x.carried = true;
    CHECK(KillApplyGate(x).call == KillCall::hold);
    x = in, x.bt_allowed = 0;
    CHECK(KillApplyGate(x).call == KillCall::hold);
    x = in, x.bt_allowed = -1;
    CHECK(KillApplyGate(x).call == KillCall::hold);
    x = in, x.streamed = false;
    CHECK(KillApplyGate(x).call == KillCall::hold);
    x = in, x.scene_owner = true;  // round 30: +0x3A8 is the locator's scene, set on every placed gazer: logged only, never a refusal
    CHECK(KillApplyGate(x).call == KillCall::apply);
    x = in, x.guard = kKillFlags;
    CHECK(KillApplyGate(x).call == KillCall::refuse);
    x = in, x.guard = kKillHealth;
    CHECK(KillApplyGate(x).call == KillCall::refuse);
    x = in, x.guard = kKillState;
    CHECK(KillApplyGate(x).call == KillCall::hold);
    x = in, x.guard = kKillPrev;
    CHECK(KillApplyGate(x).call == KillCall::hold);
    x = in, x.dist_m = 14.9;
    CHECK(KillApplyGate(x).call == KillCall::hold);
    x = in, x.dist_m = 15;
    CHECK(KillApplyGate(x).call == KillCall::apply);
    x = in, x.dist_m = -1;
    CHECK(KillApplyGate(x).call == KillCall::hold);
    // the host: only from a sandboxed guest in the same world with the same records
    x = in, x.role = 1, x.sandboxed = false;
    CHECK(KillApplyGate(x).call == KillCall::refuse);
    x.sender_sandboxed = true;
    CHECK(KillApplyGate(x).call == KillCall::refuse);
    x.world_equal = true;
    CHECK(KillApplyGate(x).call == KillCall::refuse);
    x.records_equal = true;
    CHECK(KillApplyGate(x).call == KillCall::apply);
    // the echo ring
    EchoRing er;
    CHECK(!er.Has(0x40AB1A93, 0));
    er.Add(0x40AB1A93, 1000);
    CHECK(er.Has(0x40AB1A93, 1000) && er.Has(0x40AB1A93, 11000) && !er.Has(0x40AB1A93, 11001) && !er.Has(0, 1000) && !er.Has(5, 1000));
    for (unsigned i = 0; i < 16; ++i) er.Add(100 + i, 2000);
    CHECK(!er.Has(0x40AB1A93, 2000));  // pushed out
    // the op and the test sequence
    CHECK(SyncOp(0) == 0 && SyncOp(1) == 1 && SyncOp(2) == -1 && SyncOp(5) == 5 && SyncOp(7) == 7 && SyncOp(8) == -1 && SyncOp(1.5) == -1 &&
          SyncOp(-1) == -1 && SyncOp(std::numeric_limits<double>::quiet_NaN()) == -1);
    CHECK(TestStepAt(-1) == -1 && TestStepAt(0) == 0 && TestStepAt(1499) == 0 && TestStepAt(1500) == 1 && TestStepAt(4600) == 3 &&
          TestStepAt(9000) == 4);
    static_assert(kTestSteps[4].kind == kTrClear && kTestSteps[2].hp == 60, "the test sequence");
}

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
    TestVoidVerdict();
    TestProbeOps();
    TestCodecs();
    TestCompare();
    TestS1S2Codecs();
    TestGzBits();
    TestTrouble();
    TestMarkers();
    TestKillRules();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
