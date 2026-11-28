// Round 31 stage 7 (bt_catcher): the pure part's checks (native/mod/bt_catcher_rules.hpp; the shared codecs in bt_rules.hpp). No game.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bt_catcher_rules.hpp"

namespace {
int g_checks = 0, g_fail = 0;
}  // namespace
#define CHECK(c)                                                     \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static_assert(st::btrules::catcher::kStage == 7, "the stage");

namespace R = st::btrules;
namespace C = st::btrules::catcher;

// The shared BTS1 header round trip.
void TestHello() {
    R::BtsHello h;
    h.level = 2, h.role = 1, h.caps = R::kCapField | R::kCapCatch;
    std::uint8_t b[R::kBtsHelloSize] = {};
    const std::size_t n = R::EncodeHello(1, h, b, sizeof(b));
    R::BtsHello o;
    CHECK(n == R::kBtsHelloSize && R::DecodeHello(b, n, o) && o.caps == h.caps && o.level == 2 && o.role == 1);
}

C::CatchRec Rec(std::uint32_t key, std::uint8_t class1, double x, float hp, bool inactive = false) {
    C::CatchRec r;
    r.key = key, r.class1 = class1, r.entity = true, r.inactive = inactive;
    r.pos[0] = x, r.pos[1] = -36, r.pos[2] = 535;
    r.bar[0] = x, r.bar[1] = -34, r.bar[2] = 542;
    r.pos_ok = C::RecPosOk(r.pos);
    r.hp = hp, r.sub = 0;
    return r;
}

// The record bytes as PF 18's dump read them (b2:732): type 1 (Quad), id E001, at (-896.7 -36.1 535.0), bar z 542.0, hp 100.
void TestParse() {
    std::uint8_t r[C::kRecSize] = {};
    const std::uint64_t ent = 0x75BEE246C20ull;
    const std::uint32_t id = 0xE001;
    const double pos[3] = {-896.7, -36.1, 535.0}, bar[3] = {-896.8, -33.7, 542.0};
    const float hp = 100.0f, sub = 0.0f;
    std::memcpy(r + 0x00, &ent, 8);
    std::memcpy(r + 0x20, &id, 4);
    r[0x28] = 1;
    std::memcpy(r + 0x68, pos, sizeof(pos));
    std::memcpy(r + 0xB0, bar, sizeof(bar));
    std::memcpy(r + 0xF4, &hp, 4);
    std::memcpy(r + 0x100, &sub, 4);
    C::CatchRec c = C::ParseRec(r);
    CHECK(c.key == 0xE001 && c.class1 == 2 && c.entity && !c.inactive && c.pos_ok && c.hp == 100.0f && c.sub == 0.0f);
    CHECK(std::strcmp(R::CatcherTypeName(c.class1), "Quad") == 0 && std::strcmp(R::CatcherLook(c.class1), "lion") == 0);
    CHECK(std::fabs(C::BarZ(c.pos, c.bar) - 542.0f) < 0.01f);
    r[0xF0] = 1;
    CHECK(C::ParseRec(r).inactive);
    // a new record before the game sets it (b2:734): (0 0 0) is no position; the bar falls back to the root
    std::uint8_t z[C::kRecSize] = {};
    std::memcpy(z + 0x20, &id, 4);
    CHECK(!C::ParseRec(z).pos_ok && !C::ParseRec(z).entity);
    const double far_bar[3] = {1, 1, 900};
    CHECK(C::BarZ(pos, far_bar) == 535.0f);
    // a class past 7 is unknown; a NaN hp reads -1
    r[0x28] = 9;
    const float nan = std::nanf("");
    std::memcpy(r + 0xF4, &nan, 4);
    c = C::ParseRec(r);
    CHECK(c.class1 == 0 && c.hp == -1.0f);
}

void TestNotify() {
    CHECK(C::EdgeOfNotify(0x63DB9ED5) == R::kCeSpawn && C::EdgeOfNotify(0x2FC57393) == R::kCeGone);
    CHECK(C::EdgeOfNotify(0x4C8F5A9D) == R::kCeDefeat && C::EdgeOfNotify(0x033DB880) == R::kCeAnnihilate);
    CHECK(C::EdgeOfNotify(0x60ABEA4D) == R::kCeNone && std::strcmp(C::NotifyName(0x60ABEA4D), "other") == 0);
    CHECK(std::strcmp(C::NotifyName(0x2FC57393), "GONE") == 0);
    CHECK(C::Pct(60, 100) == 60.0f && C::Pct(120, 100) == 100.0f && C::Pct(5, 0) == -1.0f && C::Pct(-1, 100) == -1.0f);
}

// The diff: SPAWN for a positioned active key, a message, GONE when it goes, nothing after.
void TestSenderDiff() {
    C::CatchSender s;
    C::EdgeNote ev[8];
    C::CatchRec recs[3] = {Rec(0xE001, 2, -896, 100), Rec(0xE002, 2, 0, 100), Rec(0xE003, 2, -890, 100)};
    recs[1].pos[0] = recs[1].pos[1] = recs[1].pos[2] = 0, recs[1].pos_ok = false;  // not placed yet
    unsigned k = s.Step(recs, 3, 1000, ev, 8);
    CHECK(k == 2 && ev[0].edge == R::kCeSpawn && ev[0].key == 0xE001 && ev[1].key == 0xE003 && !ev[0].notify);
    CHECK(s.n == 2 && s.Live() == 2);
    R::BtsCatch m;
    bool edges = false;
    CHECK(s.Build(m, 1000, &edges) == 2 && edges && m.e[0].edge == R::kCeSpawn && m.e[0].class1 == 2 && (m.e[0].flags & R::kCfCombat));
    CHECK(m.e[0].hp == 100.0f && std::fabs(m.e[0].bar_z - 542.0f) < 0.01f && m.e[0].sub == -1.0f);
    // the wire round trip of what Build made
    std::uint8_t b[R::kBtsCatchBase + R::kBtsCatchMax * R::kBtsCatchEntry];
    const std::size_t n = R::EncodeCatch(7, m, b, sizeof(b));
    R::BtsCatch o;
    CHECK(n == R::kBtsCatchBase + 2 * R::kBtsCatchEntry && R::DecodeCatch(b, n, o) && o.n == 2 && o.e[1].key == 0xE003 && o.e[0].edge == R::kCeSpawn);
    // not sent: the edges stay pending
    CHECK(s.Build(m, 1250, &edges) == 2 && edges);
    s.Sent();
    CHECK(s.Build(m, 1500, &edges) == 2 && !edges && m.e[0].edge == R::kCeNone);
    s.Sent();
    // the third record gets placed: its SPAWN now; the health moves (percent of the most seen)
    recs[1] = Rec(0xE002, 2, -897, 100);
    recs[0].hp = 63;
    k = s.Step(recs, 3, 1750, ev, 8);
    CHECK(k == 1 && ev[0].key == 0xE002 && ev[0].edge == R::kCeSpawn);
    CHECK(s.Build(m, 1750, &edges) == 3 && edges && m.e[0].key == 0xE002);
    bool saw63 = false;
    for (unsigned i = 0; i < m.n; ++i) saw63 |= m.e[i].key == 0xE001 && std::fabs(m.e[i].hp - 63.0f) < 0.01f;
    CHECK(saw63);
    s.Sent();
    // E001 goes inactive (+0xF0), E003 vanishes: two GONE; sent once, then the tracks are gone
    recs[0].inactive = true;
    C::CatchRec left[2] = {recs[0], recs[1]};
    k = s.Step(left, 2, 14000, ev, 8);
    CHECK(k == 2 && ev[0].edge == R::kCeGone && ev[1].edge == R::kCeGone && std::fabs(ev[0].age_ms - 13000.0) < 1);
    CHECK(s.Build(m, 14000, &edges) == 3 && edges);
    unsigned gone = 0;
    for (unsigned i = 0; i < m.n; ++i) gone += m.e[i].edge == R::kCeGone && !(m.e[i].flags & R::kCfCombat) ? 1u : 0u;
    CHECK(gone == 2);
    s.Sent();
    CHECK(s.n == 1 && s.t[0].key == 0xE002);
    k = s.Step(left, 2, 14250, ev, 8);
    CHECK(k == 0);
    // the last one goes: GONE once, then nothing to send
    k = s.Step(nullptr, 0, 15000, ev, 8);
    CHECK(k == 1 && ev[0].key == 0xE002);
    CHECK(s.Build(m, 15000, &edges) == 1 && m.e[0].edge == R::kCeGone);
    s.Sent();
    CHECK(s.n == 0 && s.Build(m, 15250, &edges) == 0);
}

// The notify: SPAWN before its record, a DEFEAT (no second GONE from the diff), GONE 0x2FC57393, unknown keys and repeats.
void TestSenderNotify() {
    C::CatchSender s;
    C::EdgeNote ev[8], note;
    CHECK(s.Notify(0xE001, 0x63DB9ED5, 100, &note) == R::kCeSpawn && s.n == 1);
    CHECK(s.Notify(0xE001, 0x63DB9ED5, 120, &note) == R::kCeNone);  // a second SPAWN
    R::BtsCatch m;
    bool edges = false;
    CHECK(s.Build(m, 100, &edges) == 0);  // no position yet: nothing to say
    C::CatchRec r = Rec(0xE001, 2, -896, 100);
    unsigned k = s.Step(&r, 1, 400, ev, 8);
    CHECK(k == 1 && ev[0].edge == R::kCeSpawn && ev[0].notify && ev[0].class1 == 2);
    CHECK(s.Build(m, 400, &edges) == 1 && edges && m.e[0].edge == R::kCeSpawn);
    s.Sent();
    // DEFEAT from the notify; the record then going is not a second edge
    CHECK(s.Notify(0xE001, 0x4C8F5A9D, 5000, &note) == R::kCeDefeat && note.edge == R::kCeDefeat && std::fabs(note.age_ms - 4900) < 1);
    CHECK(s.Notify(0xE001, 0x2FC57393, 5100, &note) == R::kCeNone);  // after the end
    k = s.Step(nullptr, 0, 5250, ev, 8);
    CHECK(k == 0);
    CHECK(s.Build(m, 5250, &edges) == 1 && m.e[0].edge == R::kCeDefeat && (m.e[0].flags & R::kCfDefeated) && !(m.e[0].flags & R::kCfCombat));
    s.Sent();
    CHECK(s.n == 0);
    // an end for a key never told: nothing
    CHECK(s.Notify(0xE009, 0x2FC57393, 6000, &note) == R::kCeNone && s.n == 0);
    // GONE 0x2FC57393 for a told key (b2:818)
    C::CatchRec r2 = Rec(0xE002, 2, -896, 100);
    s.Step(&r2, 1, 7000, ev, 8);
    s.Build(m, 7000, &edges);
    s.Sent();
    CHECK(s.Notify(0xE002, 0x2FC57393, 19000, &note) == R::kCeGone && s.t[0].ended);
    CHECK(s.Build(m, 19000, &edges) == 1 && m.e[0].edge == R::kCeGone);
    s.Sent();
    // a SPAWN notify whose record never shows: dropped silently after kCatchMissSteps passes, never sent
    CHECK(s.Notify(0xE00A, 0x63DB9ED5, 20000, &note) == R::kCeSpawn);
    for (unsigned i = 0; i < R::kCatchMissSteps; ++i) s.Step(nullptr, 0, 20000 + 250.0 * i, ev, 8);
    CHECK(s.n == 0);
    // an unknown hash changes nothing; the annihilate edge sets its flag
    CHECK(s.Notify(0xE00B, 0x60ABEA4D, 21000, &note) == R::kCeNone && s.n == 0);
    C::CatchRec r3 = Rec(0xE00B, 3, -896, 100);
    s.Step(&r3, 1, 21000, ev, 8);
    s.Build(m, 21000, &edges);
    s.Sent();
    CHECK(s.Notify(0xE00B, 0x033DB880, 22000, &note) == R::kCeAnnihilate);
    CHECK(s.Build(m, 22000, &edges) == 1 && (m.e[0].flags & R::kCfAnnihilating) && m.e[0].edge == R::kCeAnnihilate);
    // the partner gone before it went: DropEnded
    s.DropEnded();
    CHECK(s.n == 0);
}

// Five live catchers: four per message, edges first, the rest by turns; the cap of eight tracks.
void TestSenderMany() {
    C::CatchSender s;
    C::EdgeNote ev[16];
    C::CatchRec recs[10];
    for (int i = 0; i < 10; ++i) recs[i] = Rec(0xE100u + static_cast<std::uint32_t>(i), 2, -800 - i, 100);
    const unsigned k = s.Step(recs, 10, 0, ev, 16);
    CHECK(k == C::kSendTrackMax && s.n == C::kSendTrackMax);
    R::BtsCatch m;
    bool edges = false;
    unsigned told = 0;
    for (int round = 0; round < 2; ++round) {
        CHECK(s.Build(m, 0, &edges) == R::kBtsCatchMax && edges);
        s.Sent();
        told += m.n;
    }
    CHECK(told == 8);
    std::uint32_t seen = 0;
    for (int round = 0; round < 2; ++round) {
        CHECK(s.Build(m, 0, &edges) == R::kBtsCatchMax && !edges);
        for (unsigned i = 0; i < m.n; ++i) seen |= 1u << (m.e[i].key - 0xE100u);
        s.Sent();
    }
    CHECK(seen != 0);
    s.Reset();
    CHECK(s.n == 0 && s.Live() == 0);
}

R::CatchEntry Ent(std::uint32_t key, float hp, std::uint8_t edge, std::uint8_t flags = R::kCfCombat) {
    R::CatchEntry e;
    e.key = key, e.class1 = 2, e.flags = flags, e.edge = edge;
    e.pos[0] = -896, e.pos[1] = -36, e.pos[2] = 535;
    e.bar_z = 542, e.hp = hp, e.sub = -1;
    return e;
}

// The receiver: the test sequence (SPAWN 100, 60, 20, GONE) as leg C sees it.
void TestView() {
    C::CatchView v;
    C::ViewOut o = v.Take(Ent(0xC47E0001, 100, R::kCeSpawn, R::kCfCombat | R::kCfTest), 0);
    CHECK(o.ev == C::kVeNew && o.edge == R::kCeSpawn && v.n == 1);
    o = v.Take(Ent(0xC47E0001, 100, R::kCeNone), 250);
    CHECK(o.ev == C::kVeNone);
    o = v.Take(Ent(0xC47E0001, 95, R::kCeNone), 500);
    CHECK(o.ev == C::kVeNone);  // under 10 points
    o = v.Take(Ent(0xC47E0001, 60, R::kCeNone), 1500);
    CHECK(o.ev == C::kVeHp && o.e.hp == 60.0f);
    o = v.Take(Ent(0xC47E0001, 20, R::kCeNone), 3000);
    CHECK(o.ev == C::kVeHp);
    const C::ViewEntry* p = v.Primary();
    CHECK(p && p->key == 0xC47E0001);
    double a[3];
    C::MarkAnchor(*p, a);
    CHECK(a[0] == -896 && std::fabs(a[2] - (542 - C::kMarkOverM)) < 0.01);
    o = v.Take(Ent(0xC47E0001, 20, R::kCeGone, R::kCfTest), 4500);
    CHECK(o.ev == C::kVeEnd && o.known && o.edge == R::kCeGone && std::fabs(o.age_ms - 4500) < 1 && v.n == 0);
    // an end for a key not seen here
    o = v.Take(Ent(0xE00F, 50, R::kCeDefeat), 5000);
    CHECK(o.ev == C::kVeEnd && !o.known && v.n == 0);
    // a key without a SPAWN (joined late): new, edge none
    o = v.Take(Ent(0xE010, 80, R::kCeNone), 6000);
    CHECK(o.ev == C::kVeNew && o.edge == R::kCeNone);
    // the primary: the fighting one first, then the lowest health
    v.Take(Ent(0xE011, 30, R::kCeNone, 0), 6000);
    v.Take(Ent(0xE012, 50, R::kCeNone), 6000);
    p = v.Primary();
    CHECK(p && p->key == 0xE012);
    // expiry: no CATCH for 3 s
    v.Take(Ent(0xE012, 50, R::kCeNone), 8000);
    C::ViewEntry gone[8];
    const unsigned k = v.Expire(9100, gone, 8);
    CHECK(k == 2 && v.n == 1 && v.v[0].key == 0xE012);
    CHECK(v.Expire(11100, gone, 8) == 1 && v.n == 0);
    // the cap: the stalest replaced
    for (unsigned i = 0; i < C::kViewMax + 1; ++i) v.Take(Ent(0xE200 + i, 100, R::kCeSpawn), 100.0 * i);
    CHECK(v.n == C::kViewMax && v.Find(0xE200) < 0 && v.Find(0xE200 + C::kViewMax) >= 0);
}

void TestHudAndTest() {
    char t[96];
    C::HudText(2, 63.0f, t, sizeof(t));
    CHECK(std::strcmp(t, "Your partner is fighting a catcher (lion) 63%") == 0);
    C::HudText(1, -1.0f, t, sizeof(t));
    CHECK(std::strcmp(t, "Your partner is fighting a catcher (tar-man)") == 0);
    CHECK(C::ToastAgain(-1, 100) && !C::ToastAgain(100, 85) && C::ToastAgain(100, 80) && !C::ToastAgain(100, -1));
    CHECK(C::CatchTestAt(-1) == -1 && C::CatchTestAt(0) == 0 && C::CatchTestAt(1499) == 0 && C::CatchTestAt(1500) == 1);
    CHECK(C::CatchTestAt(3000) == 2 && C::CatchTestAt(4500) == 3 && C::CatchTestAt(99999) == 3);
    CHECK(C::kCatchTest[0].edge == R::kCeSpawn && C::kCatchTest[0].hp == 100 && C::kCatchTest[1].hp == 60 && C::kCatchTest[2].hp == 20);
    CHECK(C::kCatchTest[3].edge == R::kCeGone && std::strcmp(R::CatcherTypeName(C::kCatchTestClass1), "Quad") == 0);
    CHECK(C::CatchTestAhead(0) == 20 && C::CatchTestAhead(50) == 50 && C::CatchTestAhead(std::nan("")) == 20 && C::CatchTestAhead(900) == 20);
    // the test entry as Command / TestTick builds it encodes (flags test + combat)
    R::BtsCatch m;
    m.n = 1;
    m.e[0] = Ent(0xC47E0001, 100, R::kCeSpawn, R::kCfTest | R::kCfCombat);
    std::uint8_t b[R::kBtsCatchBase + R::kBtsCatchEntry];
    R::BtsCatch o;
    const std::size_t n = R::EncodeCatch(1, m, b, sizeof(b));
    CHECK(n == sizeof(b) && R::DecodeCatch(b, n, o) && (o.e[0].flags & R::kCfTest) && o.e[0].hp == 100.0f);
    // the receiver's spot radius with no radius on the wire: 130 m (the gate's "r 130 m")
    CHECK(R::CatcherSpotRadius(-1.0f) == 130.0f);
    // REWARD stays a decoded codec only (RW is DROPPED): a good one decodes
    R::BtsReward rw;
    rw.kind = R::kRewardCrystals, rw.class1 = 2, rw.count = 12, rw.key = 0xE001, rw.episode = 1;
    std::uint8_t rb[R::kBtsRewardSize];
    R::BtsReward ro;
    CHECK(R::EncodeReward(1, rw, rb, sizeof(rb)) == R::kBtsRewardSize && R::DecodeReward(rb, sizeof(rb), ro) && ro.count == 12);
}

int main() {
    TestHello();
    TestParse();
    TestNotify();
    TestSenderDiff();
    TestSenderNotify();
    TestSenderMany();
    TestView();
    TestHudAndTest();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
