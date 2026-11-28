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
    CHECK(KillGuard(2, 10, 10, 0x1, true) == kKillOk && KillGuard(0, 10, 10, 0x1, true) == kKillOk && KillGuard(2, 10, 10, 0x9, true) == kKillFlags);
    CHECK(KillGuard(1, 10, 10, 0, true) == kKillState && KillGuard(2, 10, 10, 0x1) == kKillState && KillGuard(2, 0, 10, 0x1, true) == kKillHealth);
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
    t.kind = kTrPulled;  // round 31: sent
    CHECK(EncodeTrouble(5, t, b, sizeof(b)) == kBtsTroubleSize);
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
    CHECK(GzBits(0, 2) == 0 && GzBits(kFlagRealized, 2) == 0 && GzBits(kFlagDead, 0) == kGzDead);  // round 31: 0x1 realized, 0x8 dead
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
    // the player id 0 (round 31 stage 0): killer 0 is not a player kill; the stats reasons still relay with killer 0
    CHECK(KillRelay(0x1234, 0, true, 0, kTagNone, false, 1e9, &cause) != nullptr && cause == 0);
    CHECK(KillRelay(kKillRainGone, 0, true, 0, kTagNone, false, 1e9, &cause) != nullptr);
    CHECK(KillRelay(kKillStatsB, 0, true, 0, kTagNone, false, 1e9, &cause) == nullptr && cause == kCauseStats);
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
    CHECK(SyncOp(0) == 0 && SyncOp(1) == 1 && SyncOp(2) == -1 && SyncOp(5) == 5 && SyncOp(7) == 7 && SyncOp(8) == 8 && SyncOp(11) == 11 && SyncOp(12) == 12 && SyncOp(13) == 13 && SyncOp(14) == -1 && SyncOp(1.5) == -1 &&
          SyncOp(-1) == -1 && SyncOp(std::numeric_limits<double>::quiet_NaN()) == -1);
    CHECK(TestStepAt(-1) == -1 && TestStepAt(0) == 0 && TestStepAt(1499) == 0 && TestStepAt(1500) == 1 && TestStepAt(4600) == 3 &&
          TestStepAt(9000) == 4);
    static_assert(kTestSteps[4].kind == kTrClear && kTestSteps[2].hp == 60, "the test sequence");
}

// ================================================================ round 31 stage 1
std::uint32_t FBitsOf(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    return u;
}
std::uint64_t DBitsOf(double d) {
    std::uint64_t u = 0;
    std::memcpy(&u, &d, 8);
    return u;
}

void TestR31Forecast() {
    // the type table, byte for byte as ds.exe DC holds it at 0x144C77ED0 (read from the binary on disk with tools/re)
    static const char* const kTableHex[kForecastTypeCount] = {
        "000000009a99193e0000403fcdcccc3d", "000000006666663fcdcccc3d00000000", "000000000000003f6666e63ecdcc4c3d",
        "00000000000000000000803f00000000", "000000000000803f0000000000000000", "0000000000000000000000000000803f",
        "0000803f000000000000000000000000", "00000000cdcc4c3ecdcc4c3f00000000", "000000000000003f0000003f00000000",
        "000000009a99193e0000403fcdcccc3d", "000000000000003f6666e63ecdcc4c3d", "000000006666663fcdcccc3d00000000",
        "00000000cdcc4c3ecdcc4c3f00000000", "000000000000003f0000003f00000000"};
    for (int t = 0; t < kForecastTypeCount; ++t) {
        std::uint8_t want[16] = {}, have[16] = {};
        for (int i = 0; i < 16; ++i) {
            unsigned v = 0;
            std::sscanf(kTableHex[t] + 2 * i, "%2x", &v);
            want[i] = static_cast<std::uint8_t>(v);
        }
        std::memcpy(have, &kForecastTable[t], 16);
        CHECK(std::memcmp(want, have, 16) == 0);
    }
    static_assert(sizeof(ForecastProb) == 16, "the table's row");
    // the job's constants, bit for bit (0x143E0997C, 0x143C07E04, 0x143DB7388 / B0 / C0 / B8 / A0 / E0, 0x143B8F2C0, 0x143B87F90)
    CHECK(FBitsOf(3633.0f) == 0x45631000u && FBitsOf(0.0078125f) == 0x3C000000u && FBitsOf(0.5f) == 0x3F000000u);
    CHECK(DBitsOf(2.067390879775102) == 0x40008A043AC5763Cull && DBitsOf(12.451168662908248) == 0x4028E6FF9438578Aull);
    CHECK(DBitsOf(53.392573080032136) == 0x404AB23FD5AE0396ull && DBitsOf(38.026512460676564) == 0x40430364C2A3C56Bull);
    CHECK(DBitsOf(6.182785114200511) == 0x4018BB2C055EBF4Eull && DBitsOf(43758.5453) == 0x40E55DD17318FC50ull);
    CHECK(DBitsOf(0.5) == 0x3FE0000000000000ull);
    // an independent numpy model of the same instruction sequence (float32 / float64 casts), not the game: catches a cast drift
    struct Ref {
        float t;
        int r, type;
        std::uint32_t n_bits;
        std::uint8_t want;
    };
    const Ref refs[] = {{0.0f, 0, 0, 0x3eb39800u, 2},      {600.0f, 1, 0, 0x3df20000u, 2},     {1234.5f, 17, 2, 0x3f7d0000u, 1},
                        {36000.0f, 63, 0, 0x3eec0000u, 2}, {18000.0f, 5, 1, 0x3d600000u, 2},   {250000.0f, 40, 7, 0x3eaa0000u, 2},
                        {99999.0f, 33, 8, 0x3f500000u, 1}, {4200.0f, 12, 2, 0x3e420000u, 2},   {700000.0f, 63, 0, 0x3edd0000u, 2},
                        {1500000.0f, 2, 2, 0x3f240000u, 1}};
    for (const Ref& r : refs) {
        CHECK(FBitsOf(ForecastNoise(r.t, r.r)) == r.n_bits);
        CHECK(ForecastSlot(r.t, r.r, r.type) == r.want);
    }
    // probe 17's golden vectors (the game's own bytes; the sentinel row until the main session fills them in)
    for (const ForecastGolden& g : kForecastGolden)
        if (g.want != kForecastNone) CHECK(ForecastSlot(g.t, g.region, g.type) == g.want);
    // the "Only" types ignore the hash; 14 (Invalid) and negatives are no type
    int counts[4] = {};
    for (int r = 0; r < 64; ++r)
        for (int k = 0; k < 200; ++k) {
            const float t = 600.0f * static_cast<float>(k) + 37.0f;
            const float n = ForecastNoise(t, r);
            CHECK(n >= 0.0f && n < 1.0f);
            CHECK(ForecastSlot(t, r, 3) == 2 && ForecastSlot(t, r, 4) == 1 && ForecastSlot(t, r, 5) == 3 && ForecastSlot(t, r, 6) == 0);
            CHECK(ForecastSlot(t, r, 9) == ForecastSlot(t, r, 0) && ForecastSlot(t, r, 11) == ForecastSlot(t, r, 1));
            ++counts[ForecastSlot(t, r, 0)];
        }
    // Rainy {0, .15, .75, .10}: the hash spreads (12800 slots)
    CHECK(counts[0] == 0);
    CHECK(counts[1] > 1280 && counts[1] < 2560 && counts[2] > 8320 && counts[2] < 10880 && counts[3] > 640 && counts[3] < 1920);
    CHECK(ForecastSlot(0, 0, 14) == kForecastNone && ForecastSlot(0, 0, -1) == kForecastNone);
    // the slot rounding: floor(T / 600 + 0.5), so T within +-300 s of a slot boundary hashes the same
    CHECK(ForecastNoise(6000.0f, 9) == ForecastNoise(6250.0f, 9) && ForecastNoise(6000.0f, 9) == ForecastNoise(5701.0f, 9));
    CHECK(ForecastNoise(6000.0f, 9) != ForecastNoise(6000.0f, 10) || ForecastNoise(6600.0f, 9) != ForecastNoise(6600.0f, 10));
    // the thresholds: sunny >= (rain + cloudy) + heavy; cloudy >= heavy + rain; heavy < heavy; NaN falls to heavy
    const ForecastProb& rainy = kForecastTable[0];
    CHECK(ForecastByte(0.999f, rainy) == 1 && ForecastByte(0.85f, rainy) == 1 && ForecastByte(0.8499f, rainy) == 2 && ForecastByte(0.1f, rainy) == 2 &&
          ForecastByte(0.0999f, rainy) == 3 && ForecastByte(1.0f, rainy) == 0);  // sunny needs n >= 1.0: never from the hash
    CHECK(ForecastByte(std::numeric_limits<float>::quiet_NaN(), rainy) == 3);
    CHECK(ForecastByte(0.95f, kForecastTable[1]) == 1 && ForecastByte(0.1f, kForecastTable[1]) == 1 && ForecastByte(0.0999f, kForecastTable[1]) == 2 &&
          ForecastByte(0.0f, kForecastTable[1]) == 2);  // heavy 0: n < 0 never
    CHECK(ForecastByte(0.0f, kForecastTable[6]) == 0 && ForecastByte(0.5f, kForecastTable[2]) == 1 && ForecastByte(0.4999f, kForecastTable[2]) == 2 &&
          ForecastByte(0.0499f, kForecastTable[2]) == 3);
    CHECK(ForecastSlotTime(100.0f, 0) == 100.0f && ForecastSlotTime(100.0f, 3) == 1900.0f);
    // review (rules 1): the base index truncates (T - clock) / len in float; a clock where T rounds down lands two steps in one index
    CHECK(ForecastSlotIndex(100.0f, 100.0f) == 0 && ForecastSlotIndex(100.0f, 1900.0f) == 3 && ForecastSlotIndex(100.0f, 1899.0f) == 2);
    CHECK(ForecastSlotIndex(0.0f, std::numeric_limits<float>::quiet_NaN()) == -1);
    int st[kWxSlots];
    float tt[kWxSlots];
    CHECK(ForecastReplay(600.0f, 600.0f + 32.0f * 600.0f, st, tt) == 32);
    bool straight = true;
    for (unsigned i = 0; i < kWxSlots; ++i) straight = straight && st[i] == static_cast<int>(i) && tt[i] == ForecastSlotTime(600.0f, static_cast<int>(i));
    CHECK(straight);
    const float ck = 119739.039f;  // float32 model: steps 18 and 19 both land in index 18, 19..30 hold steps 20..31, 31 is not written
    CHECK(ForecastSlotIndex(ck, ForecastSlotTime(ck, 19)) == 18);
    CHECK(ForecastReplay(ck, ck + 32.0f * 600.0f, st, tt) == 32);
    CHECK(st[17] == 17 && st[18] == 19 && st[19] == 20 && st[30] == 31 && st[31] == -1 && tt[18] == ForecastSlotTime(ck, 19));
    CHECK(ForecastReplay(100.0f, 100.0f, st, tt) == 0 && st[0] == -1);  // end <= clock: nothing generated
}

void TestR31Schedule() {
    static std::uint8_t eff[kWxRegions][kWxSlots], back[kWxRegions][kWxSlots];
    for (unsigned r = 0; r < kWxRegions; ++r)
        for (unsigned i = 0; i < kWxSlots; ++i) eff[r][i] = static_cast<std::uint8_t>((r * 7 + i * 3) % 4);
    std::uint64_t row = 0;
    CHECK(PackRow(eff[5], &row));
    std::uint8_t one[kWxSlots] = {};
    UnpackRow(row, one);
    CHECK(std::memcmp(one, eff[5], kWxSlots) == 0);
    one[0] = 3;
    CHECK(PackRow(one, &row) && (row & 3u) == 3u);  // slot 0 in the low bits
    one[31] = 2;
    CHECK(PackRow(one, &row) && (row >> 62) == 2u);
    one[7] = 4;
    CHECK(!PackRow(one, &row));  // only 0..3
    CHECK(!PackRow(nullptr, &row));
    WxSchedule s;
    s.slot = 2, s.gen = 7, s.clock = 12345.5f;
    CHECK(PackSchedule(~0ull, eff, s) && s.n == 64);
    CHECK(s.crc == ScheduleCrc(s.mask, s.rows, s.n));
    CHECK(UnpackSchedule(s, back) && std::memcmp(back, eff, sizeof(eff)) == 0);
    // a sparse mask: regions in ascending order, the others zero
    WxSchedule p;
    CHECK(PackSchedule(0x8000000000000005ull, eff, p) && p.n == 3);
    CHECK(UnpackSchedule(p, back));
    CHECK(std::memcmp(back[0], eff[0], kWxSlots) == 0 && std::memcmp(back[2], eff[2], kWxSlots) == 0 && std::memcmp(back[63], eff[63], kWxSlots) == 0);
    CHECK(back[1][0] == 0 && back[1][1] == 0 && back[62][5] == 0);
    p.n = 2;
    CHECK(!UnpackSchedule(p, back));  // n is not popcount(mask)
    // the crc sees one 2-bit slot and the mask
    WxSchedule q = s;
    q.rows[40] ^= 1ull << 20;
    CHECK(ScheduleCrc(q.mask, q.rows, q.n) != s.crc);
    CHECK(ScheduleCrc(s.mask & ~1ull, s.rows, s.n) != s.crc);
    CHECK(ScheduleDiffRegions(s, s) == 0 && ScheduleDiffRegions(s, q) == 1);
    WxSchedule m2;
    CHECK(PackSchedule(~0ull & ~(1ull << 9), eff, m2));
    CHECK(ScheduleDiffRegions(s, m2) == 1);  // region 9 in only one mask; the rest equal
    CHECK(Popcount64(0) == 0 && Popcount64(~0ull) == 64 && Popcount64(0x8000000000000005ull) == 3);
}

void TestR31WxRow() {
    static std::uint8_t eff[kWxRegions][kWxSlots];
    for (unsigned r = 0; r < kWxRegions; ++r)
        for (unsigned i = 0; i < kWxSlots; ++i) eff[r][i] = static_cast<std::uint8_t>((r + i) % 4);
    WxSchedule s;
    s.slot = 2, s.gen = 41, s.clock = 9000.25f;
    CHECK(PackSchedule(~0ull, eff, s));
    BtsWxRow c[kWxChunksMax];
    CHECK(WxChunks(s, c, kWxChunksMax) == 3);
    CHECK(c[0].first == 0 && c[0].n == 25 && c[0].flags == 0 && c[1].first == 25 && c[1].n == 25 && c[2].first == 50 && c[2].n == 14 &&
          c[2].flags == kWxrLast && c[2].total == 64);
    CHECK(WxChunks(s, c, 2) == 0);  // not enough room
    std::uint8_t b[kBtsMaxSize + 8] = {};
    BtsHeader h;
    std::size_t n = EncodeWxRow(9, c[0], b, sizeof(b));
    CHECK(n == 236 && n <= kBtsMaxSize);
    CHECK(DecodeBtsHeader(b, n, h) && h.type == kBtsWxRow && h.n == 25 && h.seq == 9);
    BtsWxRow d;
    CHECK(DecodeWxRow(b, n, d) && d.slot == 2 && d.gen == 41 && d.clock == 9000.25f && d.crc == s.crc && d.mask == ~0ull && d.n == 25 &&
          d.rows[24] == s.rows[24]);
    CHECK(!DecodeWxRow(b, n - 1, d));
    CHECK(EncodeWxRow(9, c[2], b, sizeof(b)) == 36 + 14 * 8);
    CHECK(EncodeWxRow(9, c[0], b, 235) == 0);
    // refusals (each from a valid chunk)
    BtsWxRow x = c[0];
    x.slot = 0;
    CHECK(EncodeWxRow(1, x, b, sizeof(b)) == 0);
    x = c[0], x.slot = 4;
    CHECK(EncodeWxRow(1, x, b, sizeof(b)) == 0);
    x = c[0], x.total = 63;  // not popcount(mask)
    CHECK(EncodeWxRow(1, x, b, sizeof(b)) == 0);
    x = c[0], x.flags = kWxrLast;  // the last flag on a middle chunk
    CHECK(EncodeWxRow(1, x, b, sizeof(b)) == 0);
    x = c[2], x.flags = 0;  // the last chunk without it
    CHECK(EncodeWxRow(1, x, b, sizeof(b)) == 0);
    x = c[0], x.flags = 2;
    CHECK(EncodeWxRow(1, x, b, sizeof(b)) == 0);
    x = c[2], x.first = 51;  // past the total
    CHECK(EncodeWxRow(1, x, b, sizeof(b)) == 0);
    x = c[0], x.n = 0;
    CHECK(EncodeWxRow(1, x, b, sizeof(b)) == 0);
    x = c[0], x.n = 26;
    CHECK(EncodeWxRow(1, x, b, sizeof(b)) == 0);
    x = c[0], x.clock = std::numeric_limits<float>::infinity();
    CHECK(EncodeWxRow(1, x, b, sizeof(b)) == 0);
    // the decoder's own checks on the wire
    n = EncodeWxRow(9, c[1], b, sizeof(b));
    CHECK(DecodeWxRow(b, n, d));
    b[15] = 1;  // flags: the last flag on a middle chunk
    CHECK(!DecodeWxRow(b, n, d));
    b[15] = 0, b[12] = 0;  // slot 0
    CHECK(!DecodeWxRow(b, n, d));
    b[12] = 2, b[5] = 2;  // version
    CHECK(!DecodeWxRow(b, n, d));
    b[5] = 1, b[6] = 0;  // n 0
    CHECK(!DecodeBtsHeader(b, 36, h));
    // a bad snapshot gives no chunks
    WxSchedule bad = s;
    bad.slot = 0;
    CHECK(WxChunks(bad, c, kWxChunksMax) == 0);
    bad = s, bad.n = 63;
    CHECK(WxChunks(bad, c, kWxChunksMax) == 0);
}

void TestR31Assembler() {
    static std::uint8_t eff[kWxRegions][kWxSlots];
    for (unsigned r = 0; r < kWxRegions; ++r)
        for (unsigned i = 0; i < kWxSlots; ++i) eff[r][i] = static_cast<std::uint8_t>((r * 3 + i) % 4);
    WxSchedule s;
    s.slot = 2, s.gen = 10, s.clock = 600.0f;
    CHECK(PackSchedule(~0ull, eff, s));
    BtsWxRow c[3];
    CHECK(WxChunks(s, c, 3) == 3);
    WxAssembler a;
    // in order
    CHECK(a.Take(c[0]) == WxPut::partial && a.Take(c[1]) == WxPut::partial && !a.have_done);
    CHECK(a.Take(c[2]) == WxPut::complete && a.have_done && a.done.gen == 10 && a.done.crc == s.crc && a.done.n == 64 && a.done.rows[63] == s.rows[63]);
    CHECK(a.Take(c[1]) == WxPut::duplicate);
    // the next gen out of order, the previous one kept until it is whole
    WxSchedule s2 = s;
    s2.gen = 11, s2.rows[3] ^= 1;
    s2.crc = ScheduleCrc(s2.mask, s2.rows, s2.n);
    BtsWxRow c2[3];
    CHECK(WxChunks(s2, c2, 3) == 3);
    CHECK(a.Take(c2[2]) == WxPut::partial && a.done.gen == 10);
    CHECK(a.Take(c2[0]) == WxPut::partial && a.done.gen == 10);
    CHECK(a.Take(c2[1]) == WxPut::complete && a.done.gen == 11 && a.done.rows[3] == s2.rows[3]);
    // an older gen is stale
    CHECK(a.Take(c[0]) == WxPut::stale);
    // a missing chunk: never complete, the last whole one stays
    WxSchedule s3 = s2;
    s3.gen = 12, s3.rows[60] ^= 2;
    s3.crc = ScheduleCrc(s3.mask, s3.rows, s3.n);
    BtsWxRow c3[3];
    CHECK(WxChunks(s3, c3, 3) == 3);
    CHECK(a.Take(c3[0]) == WxPut::partial && a.Take(c3[1]) == WxPut::partial && a.done.gen == 11);
    // a newer gen drops that partial set
    WxSchedule s4 = s3;
    s4.gen = 13;
    BtsWxRow c4[3];
    CHECK(WxChunks(s4, c4, 3) == 3);
    CHECK(a.Take(c4[0]) == WxPut::partial && a.part.gen == 13);
    CHECK(a.Take(c3[2]) == WxPut::stale);  // the dropped gen's last chunk
    CHECK(a.Take(c4[1]) == WxPut::partial && a.Take(c4[2]) == WxPut::complete && a.done.gen == 13);
    // a crc mismatch drops the set and keeps the last whole one
    WxSchedule s5 = s4;
    s5.gen = 14;  // crc of s4's rows, but one row differs below
    BtsWxRow c5[3];
    CHECK(WxChunks(s5, c5, 3) == 3);
    c5[1].rows[0] ^= 1;
    CHECK(a.Take(c5[0]) == WxPut::partial && a.Take(c5[1]) == WxPut::partial);
    CHECK(a.Take(c5[2]) == WxPut::crc_mismatch && a.done.gen == 13 && a.crc_fails == 1 && !a.have_part);
    // the same gen with another header restarts from that chunk
    WxSchedule s6 = s4;
    s6.gen = 15;
    BtsWxRow c6[3];
    CHECK(WxChunks(s6, c6, 3) == 3);
    CHECK(a.Take(c6[0]) == WxPut::partial);
    BtsWxRow odd = c6[1];
    odd.crc ^= 1;
    CHECK(a.Take(odd) == WxPut::restarted && a.part.crc == odd.crc);
    CHECK(a.Take(c6[0]) == WxPut::restarted);  // back to the real header: restart again
    CHECK(a.Take(c6[1]) == WxPut::partial && a.Take(c6[2]) == WxPut::complete && a.done.gen == 15);
    // a bad chunk is refused; a small mask is one chunk
    BtsWxRow badc = c6[0];
    badc.slot = 9;
    CHECK(a.Take(badc) == WxPut::refused);
    WxSchedule small;
    small.slot = 1, small.gen = 16;
    CHECK(PackSchedule(0x3ull, eff, small));
    BtsWxRow cs[3];
    CHECK(WxChunks(small, cs, 3) == 1 && cs[0].flags == kWxrLast);
    CHECK(a.Take(cs[0]) == WxPut::complete && a.done.n == 2 && a.done.slot == 1);
    // the gen wraps
    WxAssembler w;
    small.gen = 0xFFFFFFFFu;
    CHECK(WxChunks(small, cs, 3) == 1 && w.Take(cs[0]) == WxPut::complete);
    small.gen = 0;
    CHECK(WxChunks(small, cs, 3) == 1 && w.Take(cs[0]) == WxPut::complete && w.done.gen == 0);
    CHECK(std::strcmp(WxPutName(WxPut::crc_mismatch), "crc mismatch (dropped)") == 0);
    // review (rules 3): the same row twice with two values (same gen and header): the partial set restarts from the newer chunk only
    WxSchedule s7 = s;
    s7.gen = 20;
    BtsWxRow c7[3];
    CHECK(WxChunks(s7, c7, 3) == 3 && c7[0].first == 0 && c7[0].n > 3);
    WxAssembler b;
    CHECK(b.Take(c7[0]) == WxPut::partial && b.Take(c7[1]) == WxPut::partial);
    BtsWxRow dup = c7[0];
    dup.rows[3] ^= 1;
    std::uint64_t dup_bits = 0;
    for (unsigned j = 0; j < dup.n; ++j) dup_bits |= 1ull << (dup.first + j);
    CHECK(b.Take(dup) == WxPut::restarted && b.got == dup_bits && b.part.rows[3] == dup.rows[3] && b.part.gen == 20 && !b.have_done);
    CHECK(b.Take(c7[2]) == WxPut::partial && !b.have_done);  // c7[1]'s rows were dropped with the restart: not complete yet
    CHECK(b.Take(c7[1]) == WxPut::crc_mismatch && !b.have_done && b.crc_fails == 1);  // whole again, but row 3 is the odd one: the crc decides
}

void TestR31WxBeatAndGate() {
    std::uint8_t b[kBtsMaxSize + 8] = {};
    BtsWxBeat m;
    m.slot = 2, m.state = kWxPaused, m.gen = 5, m.clock = 1200.5f, m.crc = 0xABCDEF01u, m.rain = -1.25f, m.region = 17, m.eff0 = 2, m.sender_ms = 777;
    std::size_t n = EncodeWxBeat(3, m, b, sizeof(b));
    CHECK(n == kBtsWxBeatSize && n == 40);
    BtsWxBeat d;
    CHECK(DecodeWxBeat(b, n, d) && d.slot == 2 && d.state == 1 && d.gen == 5 && d.clock == 1200.5f && d.crc == 0xABCDEF01u && d.rain == -1.25f &&
          d.region == 17 && d.eff0 == 2 && d.sender_ms == 777);
    CHECK(!DecodeWxBeat(b, n - 1, d));
    b[14] = 1;  // pad
    CHECK(!DecodeWxBeat(b, n, d));
    b[14] = 0, b[34] = 1;  // the second pad
    CHECK(!DecodeWxBeat(b, n, d));
    b[34] = 0, b[13] = 3;  // state 3
    CHECK(!DecodeWxBeat(b, n, d));
    b[13] = 0, b[32] = 64;  // region 64
    CHECK(!DecodeWxBeat(b, n, d));
    b[32] = kWxUnknown, b[33] = kWxUnknown;  // both unknown is fine
    CHECK(DecodeWxBeat(b, n, d) && d.region == kWxUnknown && d.eff0 == kWxUnknown);
    b[33] = 4;
    CHECK(!DecodeWxBeat(b, n, d));
    BtsWxBeat x = m;
    x.slot = 4;
    CHECK(EncodeWxBeat(1, x, b, sizeof(b)) == 0);
    x = m, x.rain = std::numeric_limits<float>::quiet_NaN();
    CHECK(EncodeWxBeat(1, x, b, sizeof(b)) == 0);
    x = m, x.slot = 0;  // a beat may say "not in a forecast area"
    CHECK(EncodeWxBeat(1, x, b, sizeof(b)) == 40);

    // WxApplyGate
    WxApplyIn in;
    in.bt_weather = 2, in.role = 2, in.sandboxed = true, in.link = true, in.have_snapshot = true, in.slot_here = 2, in.slot_host = 2,
    in.age_ms = 1500, in.mask_equal = true;
    WxVerdict v = WxApplyGate(in);
    CHECK(v.call == WxCall::apply && !v.why && v.instant);  // the first apply is instant
    in.applied = true;
    v = WxApplyGate(in);
    CHECK(v.call == WxCall::apply && !v.instant);
    in.rearm = true;  // a new slot, a session start, after a load
    CHECK(WxApplyGate(in).instant);
    in.rearm = false;
    WxApplyIn x2 = in;
    // the real-save rows: never a write, applied or not
    x2.sandboxed = false;
    CHECK(WxApplyGate(x2).call == WxCall::restore && std::strstr(WxApplyGate(x2).why, "real save (host_save=0)"));
    x2.applied = false;
    CHECK(WxApplyGate(x2).call == WxCall::hold && std::strcmp(WxApplyGate(x2).why, "this game plays on its own real save (host_save=0)") == 0);
    x2.bt_weather = 1;  // compare only: the bt_weather reason comes first
    CHECK(WxApplyGate(x2).call == WxCall::hold && std::strstr(WxApplyGate(x2).why, "compare only"));
    // each failing rule: restore while applied, hold otherwise
    struct Row {
        void (*set)(WxApplyIn&);
        const char* why;
    };
    const Row rows[] = {
        {[](WxApplyIn& i) { i.emergency = true; }, "EmergencyStop"},
        {[](WxApplyIn& i) { i.role = 1; }, "not the guest"},
        {[](WxApplyIn& i) { i.role = 0; }, "not the guest"},
        {[](WxApplyIn& i) { i.bt_weather = 1; }, "compare only"},
        {[](WxApplyIn& i) { i.bt_weather = 0; }, "compare only"},
        {[](WxApplyIn& i) { i.link = false; }, "link lost"},
        {[](WxApplyIn& i) { i.have_snapshot = false; }, "no complete snapshot"},
        {[](WxApplyIn& i) { i.slot_host = 3; }, "another area"},
        {[](WxApplyIn& i) { i.slot_host = 0, i.slot_here = 0; }, "another area"},
        {[](WxApplyIn& i) { i.age_ms = 10001; }, "stale"},
        {[](WxApplyIn& i) { i.age_ms = std::numeric_limits<double>::quiet_NaN(); }, "stale"},
        {[](WxApplyIn& i) { i.mask_equal = false; }, "mask differs"},
    };
    for (const Row& r : rows) {
        WxApplyIn y = in;
        r.set(y);
        CHECK(WxApplyGate(y).call == WxCall::restore && std::strstr(WxApplyGate(y).why, r.why));
        y.applied = false;
        CHECK(WxApplyGate(y).call == WxCall::hold && std::strstr(WxApplyGate(y).why, r.why));
    }
    WxApplyIn y = in;
    y.age_ms = 10000;  // 10 s exactly is still fresh
    CHECK(WxApplyGate(y).call == WxCall::apply);
    y = in, y.loading = true;  // a load holds, even while applied (the job is not touched; re-armed after)
    CHECK(WxApplyGate(y).call == WxCall::hold && std::strcmp(WxApplyGate(y).why, "a load") == 0);
    CHECK(std::strcmp(WxCallName(WxCall::restore), "restore") == 0);
}

void TestR31Hunt() {
    std::uint8_t b[kBtsMaxSize + 8] = {};
    BtsHunt m;
    m.kind = kHuntUp, m.flags = kHfGold | kHfPulled | kHfDeep, m.episode = 4, m.center[0] = -1046, m.center[1] = 36, m.center[2] = 120,
    m.radius = 12, m.hands = 3, m.members = 7, m.level = 1.1f, m.strength = 0.4f, m.dir[0] = 0.6f, m.dir[1] = -0.8f, m.sender_ms = 4242;
    std::size_t n = EncodeHunt(2, m, b, sizeof(b));
    CHECK(n == kBtsHuntSize && n == 56);
    BtsHunt d;
    CHECK(DecodeHunt(b, n, d) && d.kind == kHuntUp && d.flags == 0x15 && d.episode == 4 && d.center[0] == -1046 && d.radius == 12 && d.hands == 3 &&
          d.members == 7 && d.level == 1.1f && d.strength == 0.4f && d.dir[1] == -0.8f && d.sender_ms == 4242);
    CHECK(!DecodeHunt(b, n + 1, d));
    b[34] = 1;  // the pad after hands / members
    CHECK(!DecodeHunt(b, n, d));
    b[34] = 0, b[13] = 0x20;  // an unknown flag
    CHECK(!DecodeHunt(b, n, d));
    b[13] = 0, b[12] = 3;  // kind 3
    CHECK(!DecodeHunt(b, n, d));
    BtsHunt x = m;
    x.episode = 0;
    CHECK(EncodeHunt(1, x, b, sizeof(b)) == 0);
    x = m, x.radius = -1;
    CHECK(EncodeHunt(1, x, b, sizeof(b)) == 0);
    x = m, x.radius = 1001;
    CHECK(EncodeHunt(1, x, b, sizeof(b)) == 0);
    x = m, x.level = std::numeric_limits<float>::infinity();
    CHECK(EncodeHunt(1, x, b, sizeof(b)) == 0);
    x = m, x.center[2] = std::numeric_limits<float>::quiet_NaN();
    CHECK(EncodeHunt(1, x, b, sizeof(b)) == 0);
    CHECK(EncodeHunt(1, m, b, 55) == 0);

    // the sender's episode machine
    HuntTracker t;
    HuntIn in;
    BtsHunt o;
    CHECK(t.Step(in, 0, &o) == 0);  // no CLEAR before anything
    in.up = true, in.center[0] = 10, in.radius = 12, in.hands = 300, in.members = 5, in.gold = true;
    CHECK(t.Step(in, 1000, &o) == kHuntUp && o.episode == 1 && o.flags == kHfGold && o.hands == 255 && o.radius == 12);
    CHECK(t.Step(in, 1100, &o) == 0);            // 4 Hz
    CHECK(t.Step(in, 1250, &o) == kHuntUp);      // 250 ms later
    in.pulled = true, in.level = 0.4f;           // a flag edge goes at once
    CHECK(t.Step(in, 1300, &o) == kHuntUp && (o.flags & kHfPulled) && !(o.flags & kHfDeep));
    in.level = 1.1f;                             // the deep flag at 1.0
    CHECK(t.Step(in, 1310, &o) == kHuntUp && (o.flags & kHfDeep));
    in.up = false;                               // pulled alone keeps it active
    CHECK(t.Step(in, 1600, &o) == kHuntUp && o.episode == 1);
    in = HuntIn{};
    CHECK(t.Step(in, 1700, &o) == kHuntClear && o.episode == 1 && o.center[0] == 10 && o.hands == 0 && o.level == 0);
    CHECK(HuntValid(o));
    CHECK(t.Step(in, 1800, &o) == 0);            // CLEAR once
    in.up = true, in.test = true;
    CHECK(t.Step(in, 2000, &o) == kHuntUp && o.episode == 2 && (o.flags & kHfTest));
    in.up = false;
    CHECK(t.Step(in, 2100, &o) == kHuntClear && o.flags == kHfTest);  // the test flag rides on the CLEAR
    t.episode = 0xFFFF;
    in.up = true;
    CHECK(t.Step(in, 3000, &o) == kHuntUp && o.episode == 1);  // never 0
    in.radius = std::numeric_limits<float>::quiet_NaN();
    in.center[1] = std::numeric_limits<float>::infinity();
    CHECK(t.Step(in, 3300, &o) == kHuntUp && o.radius == 0 && o.center[1] == 0 && HuntValid(o));

    // the receiver's view
    HuntView v;
    BtsHunt up = m;
    up.flags = 0, up.episode = 7;
    HuntView::Out r = v.Take(up, 0);
    CHECK(r.up && !r.pulled && !r.clear && v.up);
    r = v.Take(up, 250);
    CHECK(!r.up && !r.pulled);
    up.flags = kHfPulled;
    r = v.Take(up, 500);
    CHECK(r.pulled && !r.up);
    r = v.Take(up, 750);
    CHECK(!r.pulled);
    up.flags = kHfPulled | kHfDeep;
    CHECK(v.Take(up, 1000).pulled);  // PULLED level >= 1.0 logs again
    up.flags = kHfDeep;              // deep without pulled is not a PULLED line
    CHECK(!v.Take(up, 1100).pulled);
    BtsHunt clr = up;
    clr.kind = kHuntClear;
    r = v.Take(clr, 5000);
    CHECK(r.clear && std::fabs(r.secs - 5.0) < 1e-9 && !v.up);
    CHECK(!v.Take(clr, 5100).clear);  // once
    up.episode = 8, up.flags = 0;
    CHECK(v.Take(up, 6000).up);
    CHECK(!v.Expire(9000) && v.Expire(9001) && !v.up && !v.Expire(9500));
    CHECK(v.SinceMs(9500) == 3500);
    up.episode = 9;
    CHECK(v.Take(up, 10000).up);
    up.episode = 10;  // a new episode without a CLEAR between
    CHECK(v.Take(up, 10250).up);
}

void TestR31Trouble() {
    std::uint8_t b[kBtsMaxSize + 8] = {};
    BtsTrouble t;
    t.kind = kTrPulled, t.episode = 2, t.hp = -1;
    const std::size_t n = EncodeTrouble(5, t, b, sizeof(b));
    CHECK(n == kBtsTroubleSize);  // round 31: PULLED is sent
    BtsTrouble u;
    CHECK(DecodeTrouble(b, n, u) && u.kind == kTrPulled);
    t.kind = 0;
    CHECK(EncodeTrouble(5, t, b, sizeof(b)) == 0);
    t.kind = 6;
    CHECK(EncodeTrouble(5, t, b, sizeof(b)) == 0);
    // CATCHER > CAUGHT > PULLED > HUNTED
    TroubleTracker tr;
    TroubleIn in;
    float p = 0;
    in.hunted = true;
    CHECK(tr.Step(in, 0, &p) == kTrHunted);
    in.pulled = true;
    CHECK(tr.Step(in, 100, &p) == kTrPulled && tr.episode == 1);
    in.caught = true;
    CHECK(tr.Step(in, 200, &p) == kTrCaught);
    in.catcher = true, in.catcher_hp = 50;
    CHECK(tr.Step(in, 300, &p) == kTrCatcher);
    in.catcher = false, in.caught = false;
    CHECK(tr.Step(in, 400, &p) == kTrPulled);
    in.pulled = false;
    CHECK(tr.Step(in, 500, &p) == kTrHunted);
    in = TroubleIn{};
    in.pulled = true;  // pulled alone (no hunter area read) is still trouble
    CHECK(tr.Step(in, 600, &p) == kTrPulled);
    in = TroubleIn{};
    CHECK(tr.Step(in, 700, &p) == kTrClear);
    // the receiver: PULLED logs, never toasts
    TroubleView v;
    BtsTrouble m;
    m.kind = kTrPulled, m.episode = 3;
    TroubleView::Out o = v.Take(m, 0);
    CHECK(o.log && !o.toast);
}

void TestR31RainSpot() {
    RainSpotIn in;
    in.bt_rain = 1, in.source_live = true, in.radius = 50, in.my_dist_m = 500;
    CHECK(RainSpotGate(in).call == SpotCall::make && !RainSpotGate(in).why);
    RainSpotIn x = in;
    x.bt_rain = 0;
    CHECK(RainSpotGate(x).call == SpotCall::drop);
    x = in, x.source_live = false;
    CHECK(RainSpotGate(x).call == SpotCall::drop && std::strstr(RainSpotGate(x).why, "source ended"));
    x = in, x.radius = 0;
    CHECK(RainSpotGate(x).call == SpotCall::drop);
    x = in, x.radius = std::numeric_limits<float>::quiet_NaN();
    CHECK(RainSpotGate(x).call == SpotCall::drop);
    x = in, x.radius = 401;
    CHECK(RainSpotGate(x).call == SpotCall::drop);
    x = in, x.gazer_near = true;
    CHECK(RainSpotGate(x).call == SpotCall::drop && std::strstr(RainSpotGate(x).why, "150 m"));
    x = in, x.not_playing = "a menu";
    CHECK(RainSpotGate(x).call == SpotCall::hold && std::strcmp(RainSpotGate(x).why, "a menu") == 0);
    x = in, x.slots_used = 8;
    CHECK(RainSpotGate(x).call == SpotCall::hold);
    x.has_slot = true;  // its own slot: refreshed
    CHECK(RainSpotGate(x).call == SpotCall::make);
    // a sandboxed game feels it even at the spot; review (safety 1): a real-save guest makes none at any distance (a rain spot is
    // timefall there: her cargo, vehicles and structures in it are world state in her save)
    x = in, x.my_dist_m = 0;
    CHECK(RainSpotGate(x).call == SpotCall::make);
    x.carried = true;
    CHECK(RainSpotGate(x).call == SpotCall::make);
    for (const double d : {500.0, 160.01, 160.0, 10.0, -1.0}) {
        x = in, x.real_save = true, x.my_dist_m = d;
        CHECK(RainSpotGate(x).call == SpotCall::drop && std::strcmp(RainSpotGate(x).why, kSpotRealSaveWhy) == 0);
        x.carried = true, x.has_slot = true;
        CHECK(RainSpotGate(x).call == SpotCall::drop);
    }
    x = in, x.real_save = true, x.not_playing = "a menu";  // dropped, not held (the slot is taken back)
    CHECK(RainSpotGate(x).call == SpotCall::drop && std::strstr(RainSpotGate(x).why, "timefall there"));
    x = in, x.real_save = true, x.bt_rain = 0;
    CHECK(RainSpotGate(x).call == SpotCall::drop && std::strstr(RainSpotGate(x).why, "bt_rain"));
    CHECK(CatcherSpotRadius(100) == 130 && CatcherSpotRadius(0) == 130 && CatcherSpotRadius(150) == 180);
    // review (rules 2): a spot lives on until its life runs out after the last refresh, so on a real save its life is short and the
    // margin covers the distance Sam can travel in it: still more than r + 20 m away when it is gone
    CHECK(RainSpotLife(false) == 10.0f && RainSpotLife(true) == 3.0f && RainSpotLife(true) > kSpotRefreshS);
    CHECK(kSpotRealMarginM == 110.0 && kSpotRealMarginM - kSpotRealSpeedMps * RainSpotLife(true) >= kSpotRealClearM);
    {
        // review (safety 1): the margin is a second lock only now: even just beyond it a real-save guest makes no spot
        RainSpotIn d = in;
        d.real_save = true, d.my_dist_m = 50.0 + kSpotRealMarginM + 0.01;
        CHECK(RainSpotGate(d).call == SpotCall::drop);
        const double at_expiry = d.my_dist_m - kSpotRealSpeedMps * RainSpotLife(true);
        CHECK(at_expiry > 50.0 + kSpotRealClearM);
    }
    CHECK(DrySpotAllowed(false, 1) && !DrySpotAllowed(true, 1) && !DrySpotAllowed(false, 0));
}

void TestR31TarFeel() {
    TarFeelIn in;
    in.bt_tar = 1, in.hunt_up = true, in.since_hunt_ms = 250, in.hm_ready = true, in.dist_m = 8, in.count = 2;
    CHECK(TarFeelGate(in).call == TarCall::in && !TarFeelGate(in).why);
    struct Row {
        void (*set)(TarFeelIn&);
        TarCall call;
        const char* why;
    };
    const Row rows[] = {
        {[](TarFeelIn& i) { i.emergency = true; }, TarCall::out, "EmergencyStop"},
        {[](TarFeelIn& i) { i.bt_tar = 0; }, TarCall::out, "bt_tar is 0"},
        {[](TarFeelIn& i) { i.hunt_up = false; }, TarCall::out, "CLEAR"},
        {[](TarFeelIn& i) { i.since_hunt_ms = 3001; }, TarCall::out, "3 s"},
        {[](TarFeelIn& i) { i.loading = true; }, TarCall::out, "a load"},
        {[](TarFeelIn& i) { i.carried = true; }, TarCall::out, "passenger"},
        {[](TarFeelIn& i) { i.real_save = true; }, TarCall::out, "probe 26"},
        {[](TarFeelIn& i) { i.hm_ready = false; }, TarCall::out, "+0x138"},
        {[](TarFeelIn& i) { i.dist_m = 300.5; }, TarCall::out, "300 m"},
        {[](TarFeelIn& i) { i.dist_m = -1; }, TarCall::out, "300 m"},
        {[](TarFeelIn& i) { i.not_playing = "a cutscene"; }, TarCall::hold, "a cutscene"},
        {[](TarFeelIn& i) { i.count = 10; }, TarCall::hold, "list full (10)"},
    };
    for (const Row& r : rows) {
        TarFeelIn y = in;
        r.set(y);
        CHECK(TarFeelGate(y).call == r.call && std::strstr(TarFeelGate(y).why, r.why));
    }
    TarFeelIn y = in;
    y.count = 10, y.ours_in = true;  // ours is one of the 10: kept
    CHECK(TarFeelGate(y).call == TarCall::in);
    y = in, y.real_save = true, y.real_save_ok = true;  // after probe 26 passes
    CHECK(TarFeelGate(y).call == TarCall::in);
    y = in, y.since_hunt_ms = 3000, y.dist_m = 300;
    CHECK(TarFeelGate(y).call == TarCall::in);
}

void TestR31Gzst2() {
    std::uint8_t b[kBtsMaxSize + 8] = {};
    BtsGzst2 g;
    g.sender_ms = 99;
    std::size_t n = EncodeGzst2(4, g, b, sizeof(b));
    CHECK(n == kBtsGzst2Base);
    BtsGzst2 d;
    CHECK(DecodeGzst2(b, n, d) && d.n == 0 && d.sender_ms == 99);
    g.n = kBtsGzst2Max;
    for (unsigned i = 0; i < g.n; ++i) {
        Gz2Entry& e = g.e[i];
        e.field = 0x390B7DE4u, e.loc = 0x40AB1A93u + i, e.state = 0, e.mode = 3, e.bits = kGzNear | kGzReact, e.kind = static_cast<std::uint8_t>(i),
        e.action = 0x6221E6CBu, e.blend = 128, e.agit = 200, e.edges = kGeFound, e.pos[0] = -978, e.pos[1] = 58, e.pos[2] = 10,
        e.yaw = static_cast<std::int16_t>(-17999 + static_cast<int>(i)), e.dist_dm = 93;
    }
    n = EncodeGzst2(4, g, b, sizeof(b));
    CHECK(n == 232 && n <= kBtsMaxSize);
    CHECK(DecodeGzst2(b, n, d) && d.n == 6 && d.e[5].loc == 0x40AB1A98u && d.e[5].kind == 5 && d.e[0].yaw == -17999 && d.e[2].action == 0x6221E6CBu &&
          d.e[1].blend == 128 && d.e[1].agit == 200 && d.e[1].edges == kGeFound && d.e[3].dist_dm == 93 && d.e[4].pos[0] == -978);
    CHECK(!DecodeGzst2(b, n - 1, d));
    b[16 + 19] = 1;  // the first entry's pad
    CHECK(!DecodeGzst2(b, n, d));
    b[16 + 19] = 0, b[16 + 18] = 4;  // an unknown edge
    CHECK(!DecodeGzst2(b, n, d));
    b[16 + 18] = 0, b[16 + 10] = 0x40;  // an unknown bit
    CHECK(!DecodeGzst2(b, n, d));
    BtsGzst2 x = g;
    x.n = 7;
    CHECK(EncodeGzst2(1, x, b, sizeof(b)) == 0);
    x = g, x.e[2].yaw = 18001;
    CHECK(EncodeGzst2(1, x, b, sizeof(b)) == 0);
    x = g, x.e[2].pos[1] = std::numeric_limits<float>::infinity();
    CHECK(EncodeGzst2(1, x, b, sizeof(b)) == 0);
    CHECK(EncodeGzst2(1, g, b, 231) == 0);
    // the packers
    CHECK(Unit8(0) == 0 && Unit8(1) == 255 && Unit8(2) == 255 && Unit8(0.5f) == 128 && Unit8(-1) == 0 && Unit8(std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(YawCdeg(0) == 0 && YawCdeg(3.141592653589793) == 18000 && YawCdeg(-1.5707963267948966) == -9000 && YawCdeg(2 * 3.141592653589793) == 0);
    CHECK(YawCdeg(4.71238898038469) == -9000 && YawCdeg(std::numeric_limits<double>::infinity()) == 0);
    CHECK(DistDm(9.3) == 93 && DistDm(-1) == kGzDistUnknown && DistDm(1e6) == 0xFFFE && DistDm(std::numeric_limits<double>::quiet_NaN()) == kGzDistUnknown);
    CHECK(GzMirrorDrops(0x6221E6CB) && GzMirrorDrops(0x5A7E4B1D) && GzMirrorDrops(0x682DFB3B) && !GzMirrorDrops(0x7989539C) && !GzMirrorDrops(kGzActionNever));
}

void TestR31GzMirror() {
    GzMirrorIn in;
    in.bt_gazer_mirror = 1, in.partner_bits = kGzNear, in.since_gzst2_ms = 250, in.resolved = true, in.local_dist_m = 66, in.trigger_m = 10;
    CHECK(GzMirrorGate(in).call == GzCall::mirror && !GzMirrorGate(in).why);
    // hysteresis: on above trigger + 5, released below trigger + 2
    GzMirrorIn x = in;
    x.local_dist_m = 15;
    CHECK(GzMirrorGate(x).call == GzCall::none);
    x.local_dist_m = 15.01;
    CHECK(GzMirrorGate(x).call == GzCall::mirror);
    x.mirrored = true, x.mirrored_n = 1, x.local_dist_m = 13;  // inside the band: kept
    CHECK(GzMirrorGate(x).call == GzCall::mirror);
    x.local_dist_m = 12;
    CHECK(GzMirrorGate(x).call == GzCall::mirror);
    x.local_dist_m = 11.99;
    CHECK(GzMirrorGate(x).call == GzCall::release && std::strstr(GzMirrorGate(x).why, "+ 2 m"));
    x.mirrored = false, x.local_dist_m = 13;  // inside the band, not mirrored: stays off
    CHECK(GzMirrorGate(x).call == GzCall::none);
    // at most 6
    x = in, x.mirrored_n = 6;
    CHECK(GzMirrorGate(x).call == GzCall::none);
    x.mirrored = true;  // one of the 6 is kept
    CHECK(GzMirrorGate(x).call == GzCall::mirror);
    struct Row {
        void (*set)(GzMirrorIn&);
        const char* why;
    };
    const Row rows[] = {
        {[](GzMirrorIn& i) { i.emergency = true; }, "EmergencyStop"},
        {[](GzMirrorIn& i) { i.bt_gazer_mirror = 0; }, "bt_gazer_mirror is 0"},
        {[](GzMirrorIn& i) { i.loading = true; }, "a load"},
        {[](GzMirrorIn& i) { i.since_gzst2_ms = 2001; }, "no GZST2 for 2 s"},
        {[](GzMirrorIn& i) { i.partner_bits = 0; }, "no longer senses"},
        {[](GzMirrorIn& i) { i.partner_bits = kGzRealized; }, "no longer senses"},
        {[](GzMirrorIn& i) { i.partner_bits = kGzNear | kGzDead; }, "no longer senses"},
        {[](GzMirrorIn& i) { i.resolved = false; }, "streamed here"},
        {[](GzMirrorIn& i) { i.not_playing = "a menu"; }, "a menu"},
        {[](GzMirrorIn& i) { i.state = 2; }, "state not 0"},
        {[](GzMirrorIn& i) { i.dead = true; }, "dead here"},
        {[](GzMirrorIn& i) { i.caught_here = true; }, "holds this game's player"},
        {[](GzMirrorIn& i) { i.local_dist_m = -1; }, "unknown"},
        {[](GzMirrorIn& i) { i.trigger_m = -1; }, "unknown"},
    };
    for (const Row& r : rows) {
        GzMirrorIn y = in;
        r.set(y);
        CHECK(GzMirrorGate(y).call == GzCall::none && std::strstr(GzMirrorGate(y).why, r.why));
        y.mirrored = true, y.mirrored_n = 1;
        CHECK(GzMirrorGate(y).call == GzCall::release && std::strstr(GzMirrorGate(y).why, r.why));
    }
    x = in, x.partner_bits = kGzReact;
    CHECK(GzMirrorGate(x).call == GzCall::mirror);
    x.partner_bits = kGzCaught;
    CHECK(GzMirrorGate(x).call == GzCall::mirror);
    // the yaw step: 90 deg/s, the short way round
    const double pi = 3.141592653589793;
    CHECK(std::fabs(YawStep(0, pi / 2, 0.5) - pi / 4) < 1e-12);
    CHECK(std::fabs(YawStep(0, 0.1, 0.5) - 0.1) < 1e-12);
    CHECK(std::fabs(YawStep(pi - 0.1, -pi + 0.1, 1.0) - (pi + 0.1)) < 1e-9);  // across +-pi: +0.2 rad, not -6.08
    CHECK(YawStep(1.0, 2.0, 0) == 1.0 && YawStep(1.0, std::numeric_limits<double>::quiet_NaN(), 1) == 1.0);
}

void TestR31Void2() {
    std::uint8_t b[kBtsMaxSize + 8] = {};
    BtsVoid2 m;
    m.source = kVoidSrcCatch, m.reason = 13, m.flags = kV2Gazer | kV2Catcher | kV2CraterWanted, m.class1 = 2, m.pos[0] = -1046.123456789,
    m.pos[1] = 36.5, m.pos[2] = 120.25, m.crater_r = 107, m.player_dist = 3.5f, m.sender_ms = 11, m.void_id = 22, m.world = 0x12345678u;
    std::size_t n = EncodeVoid2(8, m, b, sizeof(b));
    CHECK(n == kBtsVoid2Size && n == 60);
    BtsVoid2 d;
    CHECK(DecodeVoid2(b, n, d) && d.source == kVoidSrcCatch && d.reason == 13 && d.flags == 0x31 && d.class1 == 2 && d.pos[0] == -1046.123456789 &&
          d.pos[2] == 120.25 && d.crater_r == 107 && d.player_dist == 3.5f && d.sender_ms == 11 && d.void_id == 22 && d.world == 0x12345678u);
    CHECK(!DecodeVoid2(b, n - 1, d));
    b[14] = 0x40;  // an unknown flag
    CHECK(!DecodeVoid2(b, n, d));
    b[14] = 0, b[15] = 9;  // class + 1 above 8
    CHECK(!DecodeVoid2(b, n, d));
    BtsVoid2 x = m;
    x.pos[1] = 2e7;  // not sane
    CHECK(EncodeVoid2(1, x, b, sizeof(b)) == 0);
    x = m, x.pos[0] = std::numeric_limits<double>::quiet_NaN();
    CHECK(EncodeVoid2(1, x, b, sizeof(b)) == 0);
    x = m, x.crater_r = -1;
    CHECK(EncodeVoid2(1, x, b, sizeof(b)) == 0);
    x = m, x.player_dist = std::numeric_limits<float>::infinity();
    CHECK(EncodeVoid2(1, x, b, sizeof(b)) == 0);
    CHECK(EncodeVoid2(1, m, b, 59) == 0);
    CHECK(std::strcmp(CatcherTypeName(2), "Quad") == 0 && std::strcmp(CatcherLook(2), "lion") == 0 && std::strcmp(CatcherLook(1), "tar-man") == 0 &&
          std::strcmp(CatcherLook(0), "?") == 0 && std::strcmp(CatcherTypeName(7), "Reserve") == 0 && std::strcmp(CatcherTypeName(9), "?") == 0);

    // BlastGate: every save (a real-save guest only from 300 m: see below)
    BlastIn in;
    in.bt_void = 1, in.pos_ok = true, in.age_ms = 100, in.has_resource = true, in.dist_m = 600, in.far_m = 100;
    BlastVerdict v = BlastGate(in);
    CHECK(v.call == BlastCall::play && v.far_res && !v.why);
    in.dist_m = 99.9;
    CHECK(BlastGate(in).call == BlastCall::play && !BlastGate(in).far_res);
    in.dist_m = -1;  // unknown: the near resource
    CHECK(!BlastGate(in).far_res);
    in.dist_m = 100, in.far_m = 0;  // an unread far distance: the default 100
    CHECK(BlastGate(in).far_res);
    in.far_m = 100;
    BlastIn x2 = in;
    x2.bt_void = 0;
    CHECK(BlastGate(x2).call == BlastCall::drop);
    x2 = in, x2.emergency = true;
    CHECK(BlastGate(x2).call == BlastCall::drop);
    x2 = in, x2.pos_ok = false;
    CHECK(BlastGate(x2).call == BlastCall::drop);
    x2 = in, x2.loading = true;
    CHECK(BlastGate(x2).call == BlastCall::drop && std::strstr(BlastGate(x2).why, "load"));
    x2 = in, x2.age_ms = 10001;
    CHECK(BlastGate(x2).call == BlastCall::drop && std::strstr(BlastGate(x2).why, "10 s"));
    x2 = in, x2.not_playing = "a cutscene";
    CHECK(BlastGate(x2).call == BlastCall::hold);
    x2 = in, x2.blast_active = true;
    CHECK(BlastGate(x2).call == BlastCall::hold && std::strstr(BlastGate(x2).why, "another"));
    x2 = in, x2.has_resource = false;
    CHECK(BlastGate(x2).call == BlastCall::hold);
    // review (stage 6 VB): a real-save guest drops blasts nearer than 300 m (the near blast is untested) or at an unknown distance
    x2 = in, x2.real_save = true, x2.dist_m = 299.9;
    CHECK(BlastGate(x2).call == BlastCall::drop && std::strstr(BlastGate(x2).why, "300 m"));
    x2.dist_m = -1;
    CHECK(BlastGate(x2).call == BlastCall::drop);
    x2.dist_m = 0;
    CHECK(BlastGate(x2).call == BlastCall::drop);
    x2.dist_m = 300;
    CHECK(BlastGate(x2).call == BlastCall::play && BlastGate(x2).far_res);
    x2 = in, x2.dist_m = 30;  // a sandboxed game still plays the near blast
    CHECK(BlastGate(x2).call == BlastCall::play && !BlastGate(x2).far_res);
    // FlashFade: full within FadeStart, none beyond FadeStart + 450
    CHECK(FlashFade(0) == 1.0f && FlashFade(150) == 1.0f && FlashFade(600) == 0.0f && FlashFade(1000) == 0.0f);
    CHECK(std::fabs(FlashFade(375) - 0.5f) < 1e-6f && std::fabs(FlashFade(300) - (1.0f - 150.0f / 450.0f)) < 1e-6f);
    CHECK(FlashFade(300, 300) == 1.0f && FlashFade(200, 100, 0) == 0.0f && FlashFade(std::numeric_limits<double>::quiet_NaN()) == 1.0f);
}

void TestR31Crater() {
    CraterApplyIn in;
    in.bt_void = 2, in.role = 2, in.sandboxed = true, in.flags = kV2Catcher | kV2CraterWanted, in.age_ms = 100;
    CHECK(CraterApplyGate(in).call == KillCall::apply && !CraterApplyGate(in).why);
    CraterApplyIn x = in;
    x.sandboxed = false;  // the real-save row
    CHECK(CraterApplyGate(x).call == KillCall::refuse && std::strstr(CraterApplyGate(x).why, "own real save (host_save=0): no crater record"));
    x = in, x.bt_void = 1;
    CHECK(CraterApplyGate(x).call == KillCall::refuse && std::strstr(CraterApplyGate(x).why, "blast only"));
    x = in, x.flags = kV2Catcher;
    CHECK(CraterApplyGate(x).call == KillCall::refuse && std::strstr(CraterApplyGate(x).why, "no crater"));
    x = in, x.flags = kV2Gazer | kV2CraterWanted;  // a gazer voidout: the blast only until probe 35
    CHECK(CraterApplyGate(x).call == KillCall::refuse && std::strstr(CraterApplyGate(x).why, "probe 35"));
    x.gazer_crater_ok = true;
    CHECK(CraterApplyGate(x).call == KillCall::apply);
    x = in, x.role = 0;
    CHECK(CraterApplyGate(x).call == KillCall::refuse);
    x = in, x.age_ms = 60001;
    CHECK(CraterApplyGate(x).call == KillCall::refuse);
    x = in, x.loading = true;
    CHECK(CraterApplyGate(x).call == KillCall::refuse && std::strstr(CraterApplyGate(x).why, "load"));
    x = in, x.not_playing = "Sam asleep";
    CHECK(CraterApplyGate(x).call == KillCall::hold && std::strcmp(CraterApplyGate(x).why, "Sam asleep") == 0);
    x = in, x.carried = true;
    CHECK(CraterApplyGate(x).call == KillCall::hold);
    // the host: a guest's crater only from a sandboxed guest in the same world with the same records
    x = in, x.role = 1, x.sandboxed = false;
    CHECK(CraterApplyGate(x).call == KillCall::refuse && std::strstr(CraterApplyGate(x).why, "her own real save"));
    x.sender_sandboxed = true;
    CHECK(CraterApplyGate(x).call == KillCall::refuse && std::strstr(CraterApplyGate(x).why, "another world"));
    x.world_equal = true;
    CHECK(CraterApplyGate(x).call == KillCall::refuse && std::strstr(CraterApplyGate(x).why, "records differ"));
    x.records_equal = true;
    CHECK(CraterApplyGate(x).call == KillCall::apply);
    // radius and IsDisableCrater
    CHECK(CraterRadius(0) == 107 && CraterRadius(40) == 40 && CraterRadius(std::numeric_limits<float>::quiet_NaN()) == 107 && CraterRadius(2000) == 107);
    CHECK(CraterFlags(116, -1, 107) == 1 && CraterFlags(117.5, -1, 107) == 0 && CraterFlags(-1, 50, 40) == 1 && CraterFlags(-1, -1, 40) == 0 &&
          CraterFlags(60, 60, 40) == 0);
}

void TestR31Catch() {
    std::uint8_t b[kBtsMaxSize + 8] = {};
    BtsCatch c;
    c.sender_ms = 31;
    c.n = kBtsCatchMax;
    for (unsigned i = 0; i < c.n; ++i) {
        CatchEntry& e = c.e[i];
        e.key = 0x4000u + i, e.class1 = 2, e.flags = kCfCombat | kCfBoss, e.edge = static_cast<std::uint8_t>(i), e.pos[0] = -864, e.pos[1] = -5,
        e.pos[2] = 30, e.bar_z = 42, e.hp = 63, e.sub = 100;
    }
    std::size_t n = EncodeCatch(6, c, b, sizeof(b));
    CHECK(n == 16 + 4 * 32 && n <= kBtsMaxSize);
    BtsCatch d;
    CHECK(DecodeCatch(b, n, d) && d.n == 4 && d.sender_ms == 31 && d.e[3].key == 0x4003u && d.e[3].edge == kCeAnnihilate && d.e[1].class1 == 2 &&
          d.e[1].flags == 5 && d.e[2].bar_z == 42 && d.e[2].hp == 63 && d.e[0].sub == 100 && d.e[0].pos[0] == -864);
    CHECK(!DecodeCatch(b, n - 1, d));
    b[16 + 7] = 1;  // pad
    CHECK(!DecodeCatch(b, n, d));
    b[16 + 7] = 0, b[16 + 6] = 5;  // an unknown edge
    CHECK(!DecodeCatch(b, n, d));
    b[16 + 6] = 0, b[16 + 5] = 0x20;  // an unknown flag
    CHECK(!DecodeCatch(b, n, d));
    b[16 + 5] = 0, b[16 + 4] = 9;  // class + 1 above 8
    CHECK(!DecodeCatch(b, n, d));
    BtsCatch x = c;
    x.n = 5;
    CHECK(EncodeCatch(1, x, b, sizeof(b)) == 0);
    x = c, x.e[1].hp = std::numeric_limits<float>::quiet_NaN();
    CHECK(EncodeCatch(1, x, b, sizeof(b)) == 0);
    x.n = 0;
    CHECK(EncodeCatch(1, x, b, sizeof(b)) == kBtsCatchBase);
    CHECK(std::strcmp(CatchEdgeName(kCeDefeat), "DEFEAT") == 0 && std::strcmp(CatchEdgeName(kCeGone), "GONE") == 0);
    CHECK(CatchEdgeOfNotify(0x63DB9ED5) == kCeSpawn && CatchEdgeOfNotify(0x4C8F5A9D) == kCeDefeat && CatchEdgeOfNotify(0x033DB880) == kCeAnnihilate &&
          CatchEdgeOfNotify(1) == kCeNone);

    // CatcherEdges: the record diff alone (no hook)
    CatcherEdges ce;
    CatcherEdgeOut out[8];
    CatcherSeen r[2] = {{0x10, 2, false}, {0x20, 3, true}};
    CHECK(ce.Step(r, 2, out, 8) == 1 && out[0].key == 0x10 && out[0].edge == kCeSpawn && out[0].class1 == 2);  // the inactive one is no spawn
    CHECK(ce.Step(r, 2, out, 8) == 0);
    r[0].inactive = true;  // +0xF0 rising with no defeat: GONE
    CHECK(ce.Step(r, 2, out, 8) == 1 && out[0].edge == kCeGone && out[0].key == 0x10 && ce.n == 0);
    CHECK(ce.Step(r, 2, out, 8) == 0);
    // with the hook: SPAWN from the notify, then the record (no second spawn), DEFEAT, then +0xF0 (no GONE)
    CHECK(ce.Notify(0x30, kCatchNotifySpawn, 2) == kCeSpawn && ce.Notify(0x30, kCatchNotifySpawn, 2) == kCeNone);
    CatcherSeen s1[1] = {{0x30, 2, false}};
    CHECK(ce.Step(s1, 1, out, 8) == 0);
    CHECK(ce.Notify(0x30, kCatchNotifyDefeat, 2) == kCeDefeat && ce.Notify(0x30, kCatchNotifyAnnihilate, 2) == kCeNone);
    s1[0].inactive = true;
    CHECK(ce.Step(s1, 1, out, 8) == 0 && ce.n == 0);
    // a record that vanishes after being seen: GONE at once
    CatcherSeen s2[1] = {{0x40, 1, false}};
    CHECK(ce.Step(s2, 1, out, 8) == 1);
    CHECK(ce.Step(nullptr, 0, out, 8) == 1 && out[0].edge == kCeGone && out[0].key == 0x40);
    // a spawn notify whose record never shows: GONE after 4 record steps
    CHECK(ce.Notify(0x50, kCatchNotifySpawn, 4) == kCeSpawn);
    CHECK(ce.Step(nullptr, 0, out, 8) == 0 && ce.Step(nullptr, 0, out, 8) == 0 && ce.Step(nullptr, 0, out, 8) == 0);
    CHECK(ce.Step(nullptr, 0, out, 8) == 1 && out[0].edge == kCeGone && out[0].class1 == 4 && ce.n == 0);
    // an annihilate for a key never seen is still sent, once
    CHECK(ce.Notify(0x60, kCatchNotifyAnnihilate, 1) == kCeAnnihilate && ce.Notify(0x60, kCatchNotifyDefeat, 1) == kCeNone);
    CHECK(ce.Notify(0x61, 0x12345678u, 1) == kCeNone);
    // the cap
    CatcherEdges full;
    CatcherSeen many[10];
    for (unsigned i = 0; i < 10; ++i) many[i] = CatcherSeen{0x100u + i, 1, false};
    CHECK(full.Step(many, 10, out, 8) == 8 && full.n == kCatchTrackMax);
}

void TestR31Reward() {
    std::uint8_t b[kBtsMaxSize + 8] = {};
    BtsReward m;
    m.kind = kRewardCrystals, m.class1 = 2, m.count = 6, m.key = 0x4001, m.episode = 3, m.sender_ms = 50;
    std::size_t n = EncodeReward(7, m, b, sizeof(b));
    CHECK(n == kBtsRewardSize && n == 32);
    BtsReward d;
    CHECK(DecodeReward(b, n, d) && d.kind == 1 && d.class1 == 2 && d.count == 6 && d.key == 0x4001 && d.episode == 3 && d.sender_ms == 50);
    CHECK(!DecodeReward(b, n + 1, d));
    b[14] = 1;  // pad
    CHECK(!DecodeReward(b, n, d));
    b[14] = 0, b[12] = 2;  // kind 2
    CHECK(!DecodeReward(b, n, d));
    BtsReward x = m;
    x.count = 0;
    CHECK(EncodeReward(1, x, b, sizeof(b)) == 0);
    x.count = kRewardCountMax + 1;
    CHECK(EncodeReward(1, x, b, sizeof(b)) == 0);
    x = m, x.class1 = 9;
    CHECK(EncodeReward(1, x, b, sizeof(b)) == 0);

    RewardIn in;
    in.bt_crystals = 1, in.role = 2, in.sandboxed = true, in.kind = kRewardCrystals, in.count = 6, in.age_ms = 10, in.have = 100, in.max = 9999;
    RewardVerdict v = RewardGate(in);
    CHECK(v.call == KillCall::apply && v.amount == 6 && !v.why);
    RewardIn y = in;
    y.sandboxed = false;  // the real-save row
    CHECK(RewardGate(y).call == KillCall::refuse && std::strstr(RewardGate(y).why, "real save") == RewardGate(y).why && RewardGate(y).amount == 0);
    y = in, y.role = 1, y.sandboxed = false;  // the host gives on its own save
    CHECK(RewardGate(y).call == KillCall::apply);
    y = in, y.role = 0;
    CHECK(RewardGate(y).call == KillCall::refuse);
    y = in, y.bt_crystals = 0;
    CHECK(RewardGate(y).call == KillCall::refuse);
    y = in, y.kind = 2;
    CHECK(RewardGate(y).call == KillCall::refuse);
    y = in, y.already = true;
    CHECK(RewardGate(y).call == KillCall::refuse && std::strstr(RewardGate(y).why, "already"));
    y = in, y.count = 0;
    CHECK(RewardGate(y).call == KillCall::refuse);
    y = in, y.age_ms = 60001;
    CHECK(RewardGate(y).call == KillCall::refuse);
    y = in, y.loading = true;
    CHECK(RewardGate(y).call == KillCall::refuse);
    y = in, y.not_playing = "a menu";
    CHECK(RewardGate(y).call == KillCall::hold);
    y = in, y.max = -1;
    CHECK(RewardGate(y).call == KillCall::hold);
    y = in, y.have = -1;
    CHECK(RewardGate(y).call == KillCall::hold);
    y = in, y.have = 9999;
    CHECK(RewardGate(y).call == KillCall::refuse && std::strstr(RewardGate(y).why, "cap"));
    y = in, y.have = 9996;  // capped
    CHECK(RewardGate(y).call == KillCall::apply && RewardGate(y).amount == 3);
    RewardRing ring;
    CHECK(!ring.Has(1, 1));
    ring.Add(1, 1);
    CHECK(ring.Has(1, 1) && !ring.Has(1, 2) && !ring.Has(2, 1));
    for (unsigned i = 0; i < 16; ++i) ring.Add(100 + i, 0);
    CHECK(!ring.Has(1, 1) && ring.Has(115, 0) && ring.Has(100, 0));
}

void TestR31Respawn() {
    std::uint8_t b[kBtsMaxSize + 8] = {};
    BtsRespawn m;
    m.scene = 0xDEADBEEFu, m.field = 0x390B7DE4u, m.sender_ms = 9;
    const std::size_t n = EncodeRespawn(1, m, b, sizeof(b));
    CHECK(n == kBtsRespawnSize && n == 24);
    BtsRespawn d;
    CHECK(DecodeRespawn(b, n, d) && d.scene == 0xDEADBEEFu && d.field == 0x390B7DE4u && d.sender_ms == 9);
    CHECK(!DecodeRespawn(b, n - 1, d));
    m.scene = 0;
    CHECK(EncodeRespawn(1, m, b, sizeof(b)) == 0);
    RespawnIn in;
    in.bt_respawn = 1, in.role = 2, in.sandboxed = true, in.scene_here = true, in.age_ms = 5;
    CHECK(RespawnApplyGate(in).call == KillCall::apply);
    RespawnIn x = in;
    x.sandboxed = false;  // the real-save row
    CHECK(RespawnApplyGate(x).call == KillCall::refuse && std::strstr(RespawnApplyGate(x).why, "no respawn"));
    x = in, x.role = 1;
    CHECK(RespawnApplyGate(x).call == KillCall::refuse);
    x = in, x.bt_respawn = 0;
    CHECK(RespawnApplyGate(x).call == KillCall::refuse);
    x = in, x.age_ms = 60001;
    CHECK(RespawnApplyGate(x).call == KillCall::refuse);
    x = in, x.loading = true;
    CHECK(RespawnApplyGate(x).call == KillCall::refuse);
    x = in, x.not_playing = "a load screen";
    CHECK(RespawnApplyGate(x).call == KillCall::hold);
    x = in, x.scene_here = false;
    CHECK(RespawnApplyGate(x).call == KillCall::hold);
}

void TestR31Wire() {
    // every round-31 type's exact size in the header decoder, and the caps fill the u16
    static_assert(kBtsWxRowBase == kBtsHeader + 24 && kBtsWxBeatSize == kBtsHeader + 28 && kBtsHuntSize == kBtsHeader + 44 &&
                      kBtsGzst2Base == kBtsHeader + 4 && kBtsVoid2Size == kBtsHeader + 48 && kBtsCatchBase == kBtsHeader + 4 &&
                      kBtsRewardSize == kBtsHeader + 20 && kBtsRespawnSize == kBtsHeader + 12,
                  "round 31 sizes");
    static_assert(kBtsWxRowBase + kBtsWxRowMax * kBtsWxRowEntry == 236 && kBtsGzst2Base + kBtsGzst2Max * kBtsGzst2Entry == 232 && kBtsMaxSize == 240,
                  "the biggest");
    static_assert((kCapField | kCapNoticed | kCapVoid | kCapResetApply | kCapTrouble | kCapGzst | kCapKill | kCapKillApply | kCapWx | kCapWxApply |
                   kCapHunt | kCapGzst2 | kCapVoid2 | kCapWorldApply | kCapCatch | kCapReward) == 0xFFFF,
                  "the u16 caps are full");
    static_assert((kCapWx ^ kCapWxApply ^ kCapHunt ^ kCapGzst2 ^ kCapVoid2 ^ kCapWorldApply ^ kCapCatch ^ kCapReward) == 0xFF00, "one bit each");
    std::uint8_t b[256] = {'B', 'T', 'S', '1', 0, 1, 0, 0};
    BtsHeader h;
    const struct {
        std::uint8_t type, n;
        std::size_t size;
        bool ok;
    } rows[] = {
        {kBtsWxRow, 1, 44, true},     {kBtsWxRow, 25, 236, true}, {kBtsWxRow, 26, 244, false}, {kBtsWxRow, 0, 36, false}, {kBtsWxRow, 2, 44, false},
        {kBtsWxBeat, 0, 40, true},    {kBtsWxBeat, 1, 40, false}, {kBtsWxBeat, 0, 41, false},  {kBtsHunt, 0, 56, true},   {kBtsHunt, 0, 55, false},
        {kBtsGzst2, 0, 16, true},     {kBtsGzst2, 6, 232, true},  {kBtsGzst2, 7, 268, false},  {kBtsVoid2, 0, 60, true},  {kBtsVoid2, 0, 44, false},
        {kBtsCatch, 4, 144, true},    {kBtsCatch, 5, 176, false}, {kBtsReward, 0, 32, true},   {kBtsReward, 0, 31, false},
        {kBtsRespawn, 0, 24, true},   {kBtsRespawn, 1, 24, false}, {17, 0, 24, false},
    };
    for (const auto& r : rows) {
        b[4] = r.type, b[6] = r.n;
        CHECK(DecodeBtsHeader(b, r.size, h) == r.ok);
    }
    // a decoder of one type refuses another type's message of the same size
    BtsRespawn rs;
    rs.scene = 1;
    std::uint8_t m[64] = {};
    const std::size_t n = EncodeRespawn(1, rs, m, sizeof(m));
    BtsReward rw;
    BtsWxBeat wb;
    CHECK(n == 24 && !DecodeReward(m, n, rw) && !DecodeWxBeat(m, n, wb));
}

void TestR31Config() {
    CHECK(CfgLevel(0, 2) == 0 && CfgLevel(1, 2) == 1 && CfgLevel(2, 2) == 2 && CfgLevel(7, 2) == 2 && CfgLevel(1.9, 2) == 1 && CfgLevel(-1, 2) == 0);
    CHECK(CfgLevel(std::numeric_limits<double>::quiet_NaN(), 2) == 0 && CfgLevel(1, 1) == 1 && CfgLevel(2, 1) == 1 &&
          CfgLevel(std::numeric_limits<double>::infinity(), 1) == 1);
    CHECK(FlashStartM(150) == 150 && FlashStartM(0) == 0 && FlashStartM(-5) == 150 && FlashStartM(9999) == 5000 &&
          FlashStartM(std::numeric_limits<double>::quiet_NaN()) == 150 && FlashStartM(std::numeric_limits<double>::infinity()) == 150);
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
    TestR31Forecast();
    TestR31Schedule();
    TestR31WxRow();
    TestR31Assembler();
    TestR31WxBeatAndGate();
    TestR31Hunt();
    TestR31Trouble();
    TestR31RainSpot();
    TestR31TarFeel();
    TestR31Gzst2();
    TestR31GzMirror();
    TestR31Void2();
    TestR31Crater();
    TestR31Catch();
    TestR31Reward();
    TestR31Respawn();
    TestR31Wire();
    TestR31Config();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
