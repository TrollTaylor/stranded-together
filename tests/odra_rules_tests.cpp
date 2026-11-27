// Round 28 P4 / O3 (native/mod/odra_rules.hpp): the Odradek scan edge, the local trigger and the replayed / partner scan rules.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

#include "odra_rules.hpp"

using namespace st::odrarules;

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

bool Near(double a, double b, double e = 1e-5) { return a - b < e && b - a < e; }

void TestEdge() {
    CHECK(ScanEdge(3, 4));
    for (int a = -1; a <= 6; ++a)
        for (int b = -1; b <= 6; ++b)
            if (!(a == 3 && b == 4)) CHECK(!ScanEdge(a, b));
}

void TestSurvey() {
    CHECK(!SurveyActive(0, 1));  // the ctor's idle (dword 0x10100)
    CHECK(SurveyActive(1, 0));   // FUN_1433123C0 wrote word 0x0001
    CHECK(!SurveyActive(1, 1));  // finished
    CHECK(!SurveyActive(0, 0));
}

void TestBodyReady() {
    // mask 0x6408D3C2: bits 1 6 7 8 9 12 14 15 19 26 29 30
    const int ready[] = {1, 6, 7, 8, 9, 12, 14, 15, 19, 26, 29, 30};
    for (int s = 0; s <= 0x1E; ++s) {
        bool want = false;
        for (int r : ready) want = want || r == s;
        CHECK(BodyReady(4, static_cast<std::uint8_t>(s)) == want);
        CHECK(!BodyReady(0, static_cast<std::uint8_t>(s)));  // +0x3E4 bit 2 missing
    }
    CHECK(!BodyReady(4, 0x1F));
    CHECK(!BodyReady(4, 0xFF));
    CHECK(BodyReady(0xFFFFFFFFu, 1));
}

TriggerIn GoodTrigger() {
    TriggerIn t;
    t.state = 0, t.arm_ref = t.body_ref = t.activated = t.body_ready = t.wave = true;
    return t;
}

void TestTrigger() {
    TriggerIn t = GoodTrigger();
    CHECK(ScanRequest(t) == TriggerVerdict::accept);
    t.state = 5;
    CHECK(ScanRequest(t) == TriggerVerdict::accept);  // the machine consumes the bit in 5 too
    for (int s : {1, 2, 3, 4, 6, -1}) {
        t.state = s;
        CHECK(ScanRequest(t) == TriggerVerdict::busy);
    }
    t = GoodTrigger(), t.arm_ref = false;
    CHECK(ScanRequest(t) == TriggerVerdict::no_odradek);
    t = GoodTrigger(), t.body_ref = false;
    CHECK(ScanRequest(t) == TriggerVerdict::no_odradek);
    t = GoodTrigger(), t.activated = false;
    CHECK(ScanRequest(t) == TriggerVerdict::accept);  // +0x4C7F bit 0 is a one-shot request, 0 at idle: not a gate (round 28 p4 run)
    t = GoodTrigger(), t.wave = false;
    CHECK(ScanRequest(t) == TriggerVerdict::no_wave);
    t = GoodTrigger(), t.body_ready = false;
    CHECK(ScanRequest(t) == TriggerVerdict::not_ready);
    t = GoodTrigger(), t.state = 2, t.wave = false;
    CHECK(ScanRequest(t) == TriggerVerdict::busy);  // busy wins
    for (int v = 0; v <= 4; ++v) CHECK(std::strlen(TriggerName(static_cast<TriggerVerdict>(v))) > 0);
}

ScanIn GoodScan() {
    ScanIn s;
    s.state = 0, s.b244 = 0, s.b245 = 1, s.wave = true, s.comp = true;
    s.origin[0] = 100, s.origin[1] = -50, s.origin[2] = 300;
    s.dist_m = 20, s.since_last_ms = 1e9;
    return s;
}

void TestSeq() {
    CHECK(SeqCompare(1, 0) == 1);
    CHECK(SeqCompare(0, 1) == -1);
    CHECK(SeqCompare(5, 5) == 0);
    CHECK(SeqCompare(0, 0xFFFF) == 1);     // wraparound: 0 follows 65535
    CHECK(SeqCompare(3, 0xFFFE) == 5);
    CHECK(SeqCompare(0xFFFF, 0) == -1);
    CHECK(SeqCompare(0x8000, 0) < 0);      // exactly half the range: treated as older
    CHECK(SeqCompare(0x7FFF, 0) > 0);
}

void TestAccept() {
    ScanIn s = GoodScan();
    CHECK(AcceptScan(s) == ScanVerdict::accept);
    // busy: every local state but idle (O3: 1..5)
    for (int k = 1; k <= 6; ++k) {
        s = GoodScan(), s.state = k;
        CHECK(AcceptScan(s) == ScanVerdict::busy_local_scan);
    }
    s = GoodScan(), s.b244 = 1, s.b245 = 0;
    CHECK(AcceptScan(s) == ScanVerdict::busy_survey);
    s = GoodScan(), s.b244 = 1, s.b245 = 1;  // finished: may start again
    CHECK(AcceptScan(s) == ScanVerdict::accept);
    // the cap
    s = GoodScan(), s.dist_m = kMaxRangeM;
    CHECK(AcceptScan(s) == ScanVerdict::accept);
    s.dist_m = kMaxRangeM + 0.01;
    CHECK(AcceptScan(s) == ScanVerdict::too_far);
    // the rate limit
    s = GoodScan(), s.since_last_ms = 1499;
    CHECK(AcceptScan(s) == ScanVerdict::rate_limited);
    s.since_last_ms = 1500;
    CHECK(AcceptScan(s) == ScanVerdict::accept);
    // seq
    s = GoodScan(), s.have_last_seq = true, s.last_seq = 10, s.seq = 10;
    CHECK(AcceptScan(s) == ScanVerdict::duplicate);
    s.seq = 9;
    CHECK(AcceptScan(s) == ScanVerdict::stale);
    s.seq = 11;
    CHECK(AcceptScan(s) == ScanVerdict::accept);
    s.last_seq = 0xFFFF, s.seq = 0;
    CHECK(AcceptScan(s) == ScanVerdict::accept);
    s.have_last_seq = false, s.last_seq = 10, s.seq = 10;  // no previous one: any seq
    CHECK(AcceptScan(s) == ScanVerdict::accept);
    // the origin
    s = GoodScan(), s.origin[1] = std::numeric_limits<double>::quiet_NaN();
    CHECK(AcceptScan(s) == ScanVerdict::bad_origin);
    s = GoodScan(), s.origin[2] = std::numeric_limits<double>::infinity();
    CHECK(AcceptScan(s) == ScanVerdict::bad_origin);
    s = GoodScan(), s.dist_m = std::numeric_limits<double>::quiet_NaN();
    CHECK(AcceptScan(s) == ScanVerdict::bad_origin);
    // the wave and the component
    s = GoodScan(), s.wave = false;
    CHECK(AcceptScan(s) == ScanVerdict::no_wave);
    s = GoodScan(), s.comp = false;
    CHECK(AcceptScan(s) == ScanVerdict::no_comp);
    // order: a duplicate is refused before busy (a resend is not a new scan)
    s = GoodScan(), s.state = 3, s.have_last_seq = true, s.seq = s.last_seq = 4;
    CHECK(AcceptScan(s) == ScanVerdict::duplicate);
    // busy before too far / rate limited
    s = GoodScan(), s.state = 4, s.dist_m = 1000, s.since_last_ms = 0;
    CHECK(AcceptScan(s) == ScanVerdict::busy_local_scan);
    for (int v = 0; v <= 9; ++v) CHECK(std::strlen(ScanName(static_cast<ScanVerdict>(v))) > 0);
}

void TestDir() {
    float d[4];
    SurveyDir(3, 4, d);
    CHECK(Near(d[0], 0.6) && Near(d[1], 0.8) && d[2] == 0.0f && d[3] == 0.0f);
    SurveyDir(0, 0, d);
    CHECK(d[0] == 0.0f && d[1] == 1.0f && d[2] == 0.0f && d[3] == 0.0f);
    SurveyDir(std::numeric_limits<float>::quiet_NaN(), 1, d);
    CHECK(d[0] == 0.0f && d[1] == 1.0f);
    SurveyDir(-2, 0, d);
    CHECK(Near(d[0], -1) && Near(d[1], 0));
}

// ---- O3: SOD1, the admission, the detour's decision, the queue, the transform
void TestSodHello() {
    std::uint8_t b[16] = {};
    CHECK(EncodeSodHello(kCapOdradekScan, b, sizeof(b)) == kSodHelloSize);
    CHECK(std::memcmp(b, "SOD1", 4) == 0 && b[4] == 1 && b[5] == kSodHello && b[6] == 1 && b[7] == 0);
    CHECK(LooksLikeSod(b, kSodHelloSize));
    std::uint8_t caps = 0xFF;
    CHECK(DecodeSodHello(b, kSodHelloSize, &caps) && caps == kCapOdradekScan);
    CHECK(EncodeSodHello(0, b, sizeof(b)) == kSodHelloSize && DecodeSodHello(b, kSodHelloSize, &caps) && caps == 0);
    // unknown caps bits from a newer build: masked, not refused
    b[6] = 0xF1;
    CHECK(DecodeSodHello(b, kSodHelloSize, &caps) && caps == kCapOdradekScan);
    b[6] = 1;
    CHECK(!DecodeSodHello(b, kSodHelloSize - 1, &caps));
    CHECK(!DecodeSodHello(b, kSodHelloSize + 1, &caps));
    b[7] = 1;
    CHECK(!DecodeSodHello(b, kSodHelloSize, &caps));
    b[7] = 0, b[4] = 2;
    CHECK(!DecodeSodHello(b, kSodHelloSize, &caps));
    b[4] = 1, b[5] = kSodScan;
    CHECK(!DecodeSodHello(b, kSodHelloSize, &caps));
    b[5] = kSodHello, b[0] = 'X';
    CHECK(!DecodeSodHello(b, kSodHelloSize, &caps) && !LooksLikeSod(b, kSodHelloSize));
    CHECK(EncodeSodHello(1, b, kSodHelloSize - 1) == 0 && EncodeSodHello(1, nullptr, 64) == 0);
    CHECK(!LooksLikeSod(nullptr, 8));
    const std::uint8_t scb[] = {'S', 'C', 'B', '1', 1, 2};
    CHECK(!LooksLikeSod(scb, sizeof(scb)));
}

SodScan GoodSod() {
    SodScan s;
    s.seq = 7;
    s.origin[0] = -1234.5, s.origin[1] = 2345.25, s.origin[2] = 210.125;
    s.dir[0] = 0.6f, s.dir[1] = 0.8f;
    return s;
}

void TestSodScan() {
    std::uint8_t b[64] = {};
    SodScan s = GoodSod(), r;
    CHECK(EncodeSodScan(s, b, sizeof(b)) == kSodScanSize);
    CHECK(LooksLikeSod(b, kSodScanSize) && b[5] == kSodScan && b[6] == 7 && b[7] == 0 && b[9] == 0);
    CHECK(DecodeSodScan(b, kSodScanSize, r));
    CHECK(r.seq == 7 && r.flags == 0 && r.origin[0] == -1234.5 && r.origin[1] == 2345.25 && r.origin[2] == 210.125 && r.world == 0);
    // the world fingerprint rides at 42
    s.world = 0xA1B2C3D4u;
    CHECK(EncodeSodScan(s, b, sizeof(b)) == kSodScanSize && b[42] == 0xD4 && b[45] == 0xA1);
    CHECK(DecodeSodScan(b, kSodScanSize, r) && r.world == 0xA1B2C3D4u);
    static_assert(kSodScanSize == 46 && kSodScanSize <= 64, "within link::kOdradekMax and SyncTick's buffer");
    CHECK(Near(r.dir[0], 0.6) && Near(r.dir[1], 0.8));
    // the direction is normalized on encode
    s.dir[0] = 3, s.dir[1] = 4;
    CHECK(EncodeSodScan(s, b, sizeof(b)) == kSodScanSize && DecodeSodScan(b, kSodScanSize, r) && Near(r.dir[0], 0.6) && Near(r.dir[1], 0.8));
    // seq wraps; test flag kept
    s = GoodSod(), s.seq = 0xFFFF, s.flags = kSodFlagTest;
    CHECK(EncodeSodScan(s, b, sizeof(b)) == kSodScanSize && DecodeSodScan(b, kSodScanSize, r) && r.seq == 0xFFFF && r.flags == kSodFlagTest);
    // encode refuses: no room, bad origin, degenerate dir, unknown flags
    s = GoodSod();
    CHECK(EncodeSodScan(s, b, kSodScanSize - 1) == 0);
    s.origin[1] = std::numeric_limits<double>::quiet_NaN();
    CHECK(EncodeSodScan(s, b, sizeof(b)) == 0);
    s = GoodSod(), s.origin[0] = s.origin[1] = s.origin[2] = 0;
    CHECK(EncodeSodScan(s, b, sizeof(b)) == 0);
    s = GoodSod(), s.dir[0] = s.dir[1] = 0;
    CHECK(EncodeSodScan(s, b, sizeof(b)) == 0);
    s = GoodSod(), s.flags = 0x80;
    CHECK(EncodeSodScan(s, b, sizeof(b)) == 0);
    // decode refuses: size, version, type, reserved, unknown flags, NaN, zero origin, non-unit dir
    s = GoodSod();
    CHECK(EncodeSodScan(s, b, sizeof(b)) == kSodScanSize);
    CHECK(!DecodeSodScan(b, kSodScanSize - 1, r) && !DecodeSodScan(b, kSodScanSize + 1, r));
    std::uint8_t c[64];
    auto mut = [&](int at, std::uint8_t v) {
        std::memcpy(c, b, sizeof(c));
        c[at] = v;
        return DecodeSodScan(c, kSodScanSize, r);
    };
    CHECK(!mut(4, 2));
    CHECK(!mut(5, kSodHello));
    CHECK(!mut(9, 1));
    CHECK(!mut(8, 2));
    CHECK(!mut(0, 'X'));
    std::memcpy(c, b, sizeof(c));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::memcpy(c + 18, &nan, 8);
    CHECK(!DecodeSodScan(c, kSodScanSize, r));
    std::memcpy(c, b, sizeof(c));
    const double z = 0;
    for (int i = 0; i < 3; ++i) std::memcpy(c + 10 + 8 * i, &z, 8);
    CHECK(!DecodeSodScan(c, kSodScanSize, r));
    std::memcpy(c, b, sizeof(c));
    const float two = 2.0f;
    std::memcpy(c + 34, &two, 4);
    CHECK(!DecodeSodScan(c, kSodScanSize, r));
    std::memcpy(c, b, sizeof(c));
    const float inf = std::numeric_limits<float>::infinity();
    std::memcpy(c + 38, &inf, 4);
    CHECK(!DecodeSodScan(c, kSodScanSize, r));
    // a HELLO is not a SCAN and the other way round
    std::uint8_t h[8];
    CHECK(EncodeSodHello(1, h, sizeof(h)) == kSodHelloSize && !DecodeSodScan(h, kSodHelloSize, r));
    std::uint8_t caps = 0;
    CHECK(!DecodeSodHello(b, kSodScanSize, &caps));
    // origins
    double o1[3] = {1, 2, 3}, o2[3] = {0, 0, 0}, o3[3] = {2e7, 0, 0};
    CHECK(OriginOk(o1) && !OriginOk(o2) && !OriginOk(o3));
}

AdmitIn GoodAdmit() {
    AdmitIn a;
    a.on = true, a.peer_cap = true, a.have_player = true, a.dist_m = 120;
    a.have_last = true, a.seq = 5, a.last_seq = 4;
    return a;
}

void TestAdmit() {
    AdmitIn a = GoodAdmit();
    CHECK(AdmitRemote(a) == AdmitVerdict::queue);
    a.have_last = false;
    CHECK(AdmitRemote(a) == AdmitVerdict::queue);
    a = GoodAdmit(), a.on = false;
    CHECK(AdmitRemote(a) == AdmitVerdict::off);
    a = GoodAdmit(), a.peer_cap = false;
    CHECK(AdmitRemote(a) == AdmitVerdict::no_hello);
    a.test = true;  // odra 5: no partner needed
    CHECK(AdmitRemote(a) == AdmitVerdict::queue);
    a = GoodAdmit(), a.seq = 4;
    CHECK(AdmitRemote(a) == AdmitVerdict::duplicate);
    a.seq = 3;
    CHECK(AdmitRemote(a) == AdmitVerdict::stale);
    a.seq = 0, a.last_seq = 0xFFFF;  // wrap: newer
    CHECK(AdmitRemote(a) == AdmitVerdict::queue);
    a = GoodAdmit(), a.test = true, a.seq = 1, a.last_seq = 1;  // a test scan has no seq history
    CHECK(AdmitRemote(a) == AdmitVerdict::queue);
    a = GoodAdmit(), a.have_player = false;
    CHECK(AdmitRemote(a) == AdmitVerdict::no_player);
    a = GoodAdmit(), a.dist_m = kRemoteMaxRangeM + 0.5;
    CHECK(AdmitRemote(a) == AdmitVerdict::too_far);
    a.dist_m = kRemoteMaxRangeM;
    CHECK(AdmitRemote(a) == AdmitVerdict::queue);
    a.dist_m = std::numeric_limits<double>::quiet_NaN();
    CHECK(AdmitRemote(a) == AdmitVerdict::too_far);
    // the world: refused only when both fingerprints are known and differ
    a = GoodAdmit(), a.world = 0x1111, a.our_world = 0x2222;
    CHECK(AdmitRemote(a) == AdmitVerdict::other_world);
    a.our_world = 0x1111;
    CHECK(AdmitRemote(a) == AdmitVerdict::queue);
    a.world = 0;
    CHECK(AdmitRemote(a) == AdmitVerdict::queue);
    a.world = 0x1111, a.our_world = 0;
    CHECK(AdmitRemote(a) == AdmitVerdict::queue);
    // a pending scan re-checked: a load, another world, no player, out of range
    KeepIn k;
    k.world_at = k.world_now = 0x1111, k.have_player = true, k.dist_m = 100;
    CHECK(KeepPending(k) == KeepVerdict::keep);
    k.loading = true;
    CHECK(KeepPending(k) == KeepVerdict::loading);
    k.loading = false, k.world_now = 0x2222;
    CHECK(KeepPending(k) == KeepVerdict::world_changed);
    k.world_now = 0x1111, k.have_player = false;
    CHECK(KeepPending(k) == KeepVerdict::no_player);
    k.have_player = true, k.dist_m = kRemoteMaxRangeM + 1;
    CHECK(KeepPending(k) == KeepVerdict::too_far);
    // off wins over everything; no_hello before seq
    a = GoodAdmit(), a.on = false, a.peer_cap = false, a.seq = 1;
    CHECK(AdmitRemote(a) == AdmitVerdict::off);
    a.on = true;
    CHECK(AdmitRemote(a) == AdmitVerdict::no_hello);
    for (int v = 0; v <= 6; ++v) CHECK(std::strlen(AdmitName(static_cast<AdmitVerdict>(v))) > 1);
}

void TestRemoteStart() {
    StartIn s;
    s.ok = true, s.state = 0, s.wave = true, s.wave_done = true;
    CHECK(RemoteStart(s) == StartVerdict::start_full);
    s.state = 5;
    CHECK(RemoteStart(s) == StartVerdict::start_visual);
    for (int st = 1; st <= 4; ++st) {
        s.state = st;
        CHECK(RemoteStart(s) == StartVerdict::wait_local_scan);
        CHECK(Waits(RemoteStart(s)) && !Started(RemoteStart(s)));
    }
    s.state = 6;
    CHECK(RemoteStart(s) == StartVerdict::bad_state && !Waits(RemoteStart(s)));
    s.state = -1;
    CHECK(RemoteStart(s) == StartVerdict::bad_state);
    s.state = 0, s.wave = false;
    CHECK(RemoteStart(s) == StartVerdict::wait_no_wave);
    s.wave = true, s.wave_done = false;
    CHECK(RemoteStart(s) == StartVerdict::wait_wave);
    s.wave_done = true, s.survey_active = true;
    CHECK(RemoteStart(s) == StartVerdict::wait_survey);
    s.survey_active = false, s.ok = false;
    CHECK(RemoteStart(s) == StartVerdict::wait_no_wave);
    // a local scan in progress wins over the wave / survey reasons
    s.ok = true, s.state = 3, s.wave_done = false, s.survey_active = true;
    CHECK(RemoteStart(s) == StartVerdict::wait_local_scan);
    // the steps
    CHECK(StartSteps(StartVerdict::start_full, kStartAll) == kStartAll);
    CHECK(StartSteps(StartVerdict::start_visual, kStartAll) == (kStartAll & ~kStartSystem));
    CHECK(!(StartSteps(StartVerdict::start_visual, kStartAll) & kStartSystem));
    CHECK(StartSteps(StartVerdict::start_full, kStartWave | 0x100) == kStartWave);
    CHECK(StartSteps(StartVerdict::wait_wave, kStartAll) == 0 && StartSteps(StartVerdict::bad_state, kStartAll) == 0);
    static_assert(kStartAll == (kStartWave | kStartEmitDir | kStartSlot | kStartSensor | kStartFront | kStartSystem | kStartSurvey), "start steps");
    for (int v = 0; v <= 6; ++v) CHECK(std::strlen(StartName(static_cast<StartVerdict>(v))) > 1);
}

void TestQueue() {
    CHECK(Supersedes(false, false, 6, 5));
    CHECK(!Supersedes(false, false, 5, 5));
    CHECK(!Supersedes(false, false, 4, 5));
    CHECK(Supersedes(false, false, 0, 0xFFFF));
    CHECK(Supersedes(true, false, 1, 9) && Supersedes(false, true, 1, 9));
    CHECK(!PendingExpired(0) && !PendingExpired(kPendingTtlMs) && PendingExpired(kPendingTtlMs + 1));
    CHECK(PendingExpired(std::numeric_limits<double>::quiet_NaN()));
}

void TestRot() {
    float m[3][3];
    RotFromDir(0.6f, 0.8f, m);
    // forward is the second row, right = forward x up, all unit and orthogonal
    CHECK(Near(m[1][0], 0.6) && Near(m[1][1], 0.8) && m[1][2] == 0.0f);
    CHECK(Near(m[0][0], 0.8) && Near(m[0][1], -0.6) && m[2][2] == 1.0f);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const double d = m[i][0] * m[j][0] + m[i][1] * m[j][1] + m[i][2] * m[j][2];
            CHECK(Near(d, i == j ? 1.0 : 0.0));
        }
    // right-handed: right x forward = up
    CHECK(Near(m[0][0] * m[1][1] - m[0][1] * m[1][0], 1.0));
    float d[2];
    CHECK(NormDir2(0, -2, d) && Near(d[0], 0) && Near(d[1], -1));
    CHECK(!NormDir2(0, 0, d) && !NormDir2(std::numeric_limits<float>::quiet_NaN(), 1, d));
}

void TestSendGates() {
    SendIn s;
    CHECK(!MaySendHello(s) && !MaySendScan(s));
    s.on = true, s.connected = true, s.channel_ok = true;
    CHECK(MaySendHello(s) && !MaySendScan(s));
    s.peer_cap = true;
    CHECK(MaySendScan(s));
    s.channel_ok = false;
    CHECK(!MaySendHello(s) && !MaySendScan(s));
    // key off: nothing, unless a HELLO with the bit went out (the caps 0 that follows a fault)
    s = SendIn{}, s.connected = true, s.channel_ok = true, s.peer_cap = true;
    CHECK(!MaySendHello(s) && !MaySendScan(s));
    s.announced = true;
    CHECK(MaySendHello(s) && !MaySendScan(s));
    s.connected = false;
    CHECK(!MaySendHello(s));
}
}  // namespace

int main() {
    TestEdge();
    TestSurvey();
    TestBodyReady();
    TestTrigger();
    TestSeq();
    TestAccept();
    TestDir();
    TestSodHello();
    TestSodScan();
    TestAdmit();
    TestRemoteStart();
    TestQueue();
    TestRot();
    TestSendGates();
    std::printf("odra_rules: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
