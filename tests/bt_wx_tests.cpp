// Round 31 stage 2 (bt_wx): the pure part's checks (native/mod/bt_wx_rules.hpp; the shared codecs in bt_rules.hpp). No game.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bt_wx_rules.hpp"

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

static_assert(st::btrules::wx::kStage == 2, "the stage");

namespace R = st::btrules;
namespace W = st::btrules::wx;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

static_assert(W::kRowSaved == 0x58 && W::kWxBlock == 0x1700, "the row and block sizes");
static_assert(W::kHeadAt + W::kHeadLen == W::kRowEff - W::kRowKeep && W::kTailAt + W::kTailLen == W::kRowSaved, "the two written parts");

// The shared BTS1 header round trip.
void TestHello() {
    R::BtsHello h;
    h.level = 2, h.role = 1, h.caps = R::kCapField;
    std::uint8_t b[R::kBtsHelloSize] = {};
    const std::size_t n = R::EncodeHello(1, h, b, sizeof(b));
    R::BtsHello o;
    CHECK(n == R::kBtsHelloSize && R::DecodeHello(b, n, o) && o.caps == h.caps && o.level == 2 && o.role == 1);
}

void TestLayout() {
    CHECK(W::RowOffset(2, 43) == 0x8A0 + (2 * 0x40 + 43) * 0x5C);
    CHECK(W::RowOffset(1, 0) + W::kWxBlock == W::RowOffset(1, 63) + W::kRowSize);
    CHECK(W::JobIdle(0) && W::JobIdle(0x7111BEEFu) && !W::JobIdle(0x2000BEEFu) && !W::JobIdle(1));
    CHECK(W::WxBeatStateOf(true, true) == R::kWxLoading && W::WxBeatStateOf(false, true) == R::kWxPlaying && W::WxBeatStateOf(false, false) == R::kWxPaused);
}

void TestPhase() {
    // PF 5 read 3: the host's clock 430390, last 430354: 36 s into its slot
    CHECK(W::WxPhase(430390.0f, 430354.0f) == 36.0f);
    CHECK(W::WxPhase(100.0f, 200.0f) == 0.0f);        // last ahead of the clock: 0
    CHECK(W::WxPhase(5000.0f, 100.0f) == 600.0f);     // at most one slot
    CHECK(W::WxPhase(NAN, 1.0f) == 0.0f && W::WxPhase(1.0f, INFINITY) == 0.0f);
    // the guest's phase grows with her own clock from the anchor
    CHECK(W::WxPhaseNow(36.0f, 1000.0f, true, 1010.0f) == 46.0f);
    CHECK(W::WxPhaseNow(36.0f, 1000.0f, true, 990.0f) == 36.0f);   // her clock went back (a load): no growth
    CHECK(W::WxPhaseNow(36.0f, 1000.0f, false, 1500.0f) == 36.0f); // no anchor: the phase as sent
    CHECK(W::WxPhaseNow(500.0f, 1000.0f, true, 1200.0f) == 600.0f); // never past the slot
    CHECK(W::WxPhaseNow(NAN, 0.0f, false, 10.0f) == 0.0f);
    // her window start: her own clock minus the phase (not the host's clock: PF 5's drift 28 vs 60 s)
    CHECK(W::WxWindowStart(36.0f, 430421.0f, true, 430431.0f) == 430431.0f - 46.0f);
    // the window start stays put while the phase grows (a fixed slot boundary), until the slot is full
    const float a = W::WxWindowStart(10.0f, 2000.0f, true, 2000.0f), b = W::WxWindowStart(10.0f, 2000.0f, true, 2300.0f);
    CHECK(a == b && a == 1990.0f);
    CHECK(W::WxWindowStart(10.0f, 2000.0f, true, 3000.0f) == 2400.0f);  // phase 600: it slides with the clock
}

// A fake row's saved part (+0x04..+0x5B).
void FillRow(u8* p, int seed) {
    for (std::size_t i = 0; i < W::kRowSaved; ++i) p[i] = static_cast<u8>(seed * 31 + static_cast<int>(i) * 7);
    for (unsigned i = 0; i < R::kWxSlots; ++i) p[W::kRowEff - W::kRowKeep + i] = static_cast<u8>((seed + static_cast<int>(i)) & 3);
}

void TestRowImage() {
    u8 cur[W::kRowSaved], img[W::kRowSaved], eff[R::kWxSlots];
    FillRow(cur, 5);
    const u32 bits_in = 0x7u;
    std::memcpy(cur + (W::kRowBits - W::kRowKeep), &bits_in, 4);
    for (unsigned i = 0; i < R::kWxSlots; ++i) eff[i] = static_cast<u8>(i % 4);
    const unsigned parts = W::WxRowImage(cur, eff, 1000.0f, img);
    CHECK(parts == (W::kPartHead | W::kPartTail));
    float win[2];
    std::memcpy(win, img + (W::kRowWin0 - W::kRowKeep), 8);
    CHECK(win[0] == 1000.0f && win[1] == 1000.0f + 32.0f * 600.0f);
    bool base_ok = true;
    for (unsigned i = 0; i < R::kWxSlots; ++i) base_ok &= img[W::kRowBase - W::kRowKeep + i] == eff[i];
    CHECK(base_ok);
    CHECK(std::memcmp(img + (W::kRowEff - W::kRowKeep), cur + (W::kRowEff - W::kRowKeep), 32) == 0);  // eff: the job's, untouched
    u32 forced[3] = {1, 1, 1}, bits = 0;
    std::memcpy(forced, img + (W::kRowForced - W::kRowKeep), 12);
    std::memcpy(&bits, img + (W::kRowBits - W::kRowKeep), 4);
    CHECK(forced[0] == 0 && forced[1] == 0 && forced[2] == 0);
    CHECK(bits == 0x2u);  // bits 0 and 2 off; bit 1 (her own dry spots) kept
    // a steady apply writes nothing
    u8 again[W::kRowSaved];
    CHECK(W::WxRowImage(img, eff, 1000.0f, again) == 0 && std::memcmp(again, img, W::kRowSaved) == 0);
    // only the window moved: the head only
    CHECK(W::WxRowImage(img, eff, 1001.0f, again) == W::kPartHead);
    // only a forced value came back (the game's story rain): the tail only
    std::memcpy(again, img, W::kRowSaved);
    const u32 one = 1;
    std::memcpy(again + (W::kRowForced - W::kRowKeep) + 4, &one, 4);
    u8 third[W::kRowSaved];
    CHECK(W::WxRowImage(again, eff, 1000.0f, third) == W::kPartTail);
    // values above 3 in the host's eff are masked to 2 bits
    u8 big[R::kWxSlots];
    std::memset(big, 7, sizeof(big));
    W::WxRowImage(cur, big, 0.0f, img);
    CHECK(img[W::kRowBase - W::kRowKeep] == 3);
    CHECK(W::WxRowImage(nullptr, eff, 0.0f, img) == 0);
    u32 b = 3;
    CHECK(W::WxClearRoll(b) && b == 2 && !W::WxClearRoll(b) && b == 2);
}

// The restore model: save the own rows, write the images, write them back: the block is byte-for-byte what it was.
void TestSaveRestore() {
    static u8 block[W::kWxBlock], orig[W::kWxBlock];
    for (unsigned r = 0; r < R::kWxRegions; ++r) {
        u8* row = block + r * W::kRowSize;
        const u32 type = 100 + r;
        std::memcpy(row, &type, 4);
        FillRow(row + W::kRowKeep, static_cast<int>(r));
    }
    std::memcpy(orig, block, sizeof(block));
    const u64 mask = 0xFFFFFFFFFFF7FFFFull;  // PF 5's mask: region 19 out
    static W::WxSaved s;
    CHECK(!s.Save(block, 0x1000, 0, mask) && !s.Save(block, 0x1000, 2, 0) && !s.have);
    CHECK(s.Save(block, 0x1000, 2, mask) && s.have && s.slot == 2 && s.mask == mask && s.ws == 0x1000);
    CHECK(s.Eff0(43) == block[43 * W::kRowSize + W::kRowEff] && s.Eff0(19) == R::kWxUnknown && s.Eff0(-1) == R::kWxUnknown && s.Eff0(64) == R::kWxUnknown);
    // the apply: every masked row gets the host's eff (all 3)
    u8 eff[R::kWxSlots];
    std::memset(eff, 3, sizeof(eff));
    u8 img[W::kRowSaved];
    for (unsigned r = 0; r < R::kWxRegions; ++r) {
        if (!(mask >> r & 1u)) continue;
        u8* p = block + r * W::kRowSize + W::kRowKeep;
        const unsigned parts = W::WxRowImage(p, eff, 5000.0f, img);
        if (parts & W::kPartHead) std::memcpy(p + W::kHeadAt, img + W::kHeadAt, W::kHeadLen);
        if (parts & W::kPartTail) std::memcpy(p + W::kTailAt, img + W::kTailAt, W::kTailLen);
        p[W::kRowEff - W::kRowKeep] = 3;  // the job's own eff follows (PF 2: eff0 3)
    }
    CHECK(std::memcmp(block, orig, sizeof(block)) != 0);
    bool types = true, out = true;
    for (unsigned r = 0; r < R::kWxRegions; ++r) {
        types &= std::memcmp(block + r * W::kRowSize, orig + r * W::kRowSize, 4) == 0;
        if (!(mask >> r & 1u)) out &= std::memcmp(block + r * W::kRowSize, orig + r * W::kRowSize, W::kRowSize) == 0;
    }
    CHECK(types && out);  // the type is never written; a region outside the mask is never written
    CHECK(block[43 * W::kRowSize + W::kRowEff] == 3);
    // the restore (the build spec's change 1): her own rows back, eff0 included (PF 3's failed restore kept the written value)
    CHECK(W::WxWriteBack(s, block));
    CHECK(std::memcmp(block, orig, sizeof(block)) == 0);
    s.Drop();
    CHECK(!s.have && !W::WxWriteBack(s, block));
}

void TestPackAndRegions() {
    static u8 block[W::kWxBlock];
    u8 eff[R::kWxRegions][R::kWxSlots] = {};
    for (unsigned r = 0; r < R::kWxRegions; ++r)
        for (unsigned i = 0; i < R::kWxSlots; ++i) {
            eff[r][i] = static_cast<u8>((r * 3 + i) & 3);
            block[r * W::kRowSize + W::kRowEff + i] = eff[r][i];
        }
    const u64 mask = 0xFFFFFFFFFFF7FFFFull;
    R::WxSchedule a, b;
    CHECK(W::WxPackBlock(block, 2, mask, a) && R::PackSchedule(mask, eff, b));
    CHECK(a.n == 63 && a.crc == b.crc && R::ScheduleDiffRegions(a, b) == 0 && a.slot == 2);
    CHECK(!W::WxPackBlock(block, 4, mask, a) && !W::WxPackBlock(block, 1, 0, a));
    block[5 * W::kRowSize + W::kRowEff] = 4;  // not a weather value
    CHECK(!W::WxPackBlock(block, 1, mask, a));
    block[5 * W::kRowSize + W::kRowEff] = eff[5][0];
    // by region and back
    u64 rows[R::kWxRegions];
    CHECK(W::WxRowsByRegion(b, rows) && rows[19] == 0);
    R::WxSchedule c;
    CHECK(W::WxFromRegions(2, mask, rows, c) && c.crc == b.crc && c.n == b.n && R::ScheduleDiffRegions(b, c) == 0);
    CHECK(!W::WxFromRegions(0, mask, rows, c) && !W::WxFromRegions(1, 0, rows, c));
    CHECK(W::WxEff0(rows, mask, 43) == eff[43][0] && W::WxEff0(rows, mask, 19) == R::kWxUnknown && W::WxEff0(rows, mask, -1) == R::kWxUnknown);
    R::WxSchedule bad = b;
    bad.n = 5;
    CHECK(!W::WxRowsByRegion(bad, rows));
}

void TestSeqlock() {
    static W::WxSeqlock<W::kSnapWords> q;
    u64 v[W::kSnapWords], o[W::kSnapWords];
    CHECK(!q.Read(o) && q.Version() == 0);  // nothing written yet
    for (std::size_t i = 0; i < W::kSnapWords; ++i) v[i] = 0x1111111111111111ull * (i + 1);
    q.Write(v);
    CHECK(q.Version() == 2 && q.Read(o) && std::memcmp(v, o, sizeof(v)) == 0);
    v[7] = 42;
    q.Write(v);
    CHECK(q.Version() == 4 && q.Read(o) && o[7] == 42);
    q.seq.store(5);  // a writer in the middle: the reader gives up
    CHECK(!q.Read(o, 3));
    float x = 0, y = 0;
    W::UnpackF2(W::PackF2(430354.0f, -1.5f), x, y);
    CHECK(x == 430354.0f && y == -1.5f);
}

void TestCapsAndGate() {
    CHECK(W::WxLevel(0, 2) == 0 && W::WxLevel(2, 0) == 0 && W::WxLevel(1, 2) == 1 && W::WxLevel(2, 1) == 1 && W::WxLevel(2, 2) == 2);
    CHECK(W::WxCaps(1, 1, 2, false, true) == R::kCapWx);
    CHECK(W::WxCaps(1, 2, 2, false, true) == R::kCapWx);  // the host never applies
    CHECK(W::WxCaps(2, 2, 2, false, true) == (R::kCapWx | R::kCapWxApply));
    CHECK(W::WxCaps(2, 2, 2, true, true) == R::kCapWx);   // a real-save guest: compare only
    CHECK(W::WxCaps(2, 1, 2, false, true) == R::kCapWx);  // the built default: compare only
    CHECK(W::WxCaps(2, 2, 1, false, true) == R::kCapWx);  // bt_sync 1: compare only
    CHECK(W::WxCaps(2, 2, 2, false, false) == 0 && W::WxCaps(0, 2, 2, false, true) == 0 && W::WxCaps(1, 0, 2, false, true) == 0);
    CHECK(W::WxPartner(1, 2, R::kCapWx) && W::WxPartner(2, 1, R::kCapWx) && W::WxPartner(1, 2, R::kCapWxApply));
    CHECK(!W::WxPartner(1, 1, R::kCapWx) && !W::WxPartner(2, 1, R::kCapField) && !W::WxPartner(0, 1, R::kCapWx) && !W::WxPartner(1, 0, R::kCapWx));
    R::WxApplyIn in;
    in.bt_weather = 2, in.role = 2, in.sandboxed = true, in.link = true, in.have_snapshot = true, in.slot_here = 2, in.slot_host = 2,
    in.age_ms = 1000, in.mask_equal = true;
    R::WxVerdict v = W::WxGate(in);
    CHECK(v.call == R::WxCall::apply && v.instant);
    // a real-save guest: her reason, also at bt_weather 1 (WxApplyGate alone says "compare only" first)
    R::WxApplyIn rs = in;
    rs.sandboxed = false, rs.bt_weather = 1;
    v = W::WxGate(rs);
    CHECK(v.call == R::WxCall::hold && std::strcmp(v.why, "this game plays on its own real save (host_save=0)") == 0);
    CHECK(std::strstr(R::WxApplyGate(rs).why, "compare only"));
    rs.applied = true;  // (never on a real save; the rule still restores)
    CHECK(W::WxGate(rs).call == R::WxCall::restore);
    R::WxApplyIn em = rs;
    em.emergency = true;
    CHECK(std::strcmp(W::WxGate(em).why, "EmergencyStop") == 0);
    // the host: never applies
    R::WxApplyIn host = in;
    host.role = 1, host.sandboxed = false;
    CHECK(W::WxGate(host).call == R::WxCall::hold);
    // a sandboxed guest: WxApplyGate's own rules; "link lost" is the leg-3 pass text
    R::WxApplyIn ll = in;
    ll.applied = true, ll.link = false;
    v = W::WxGate(ll);
    CHECK(v.call == R::WxCall::restore && std::strcmp(v.why, "link lost") == 0);
    R::WxApplyIn old = in;
    old.applied = true, old.age_ms = 10001;
    CHECK(W::WxGate(old).call == R::WxCall::restore);
    R::WxApplyIn ld = in;
    ld.applied = true, ld.loading = true;
    CHECK(W::WxGate(ld).call == R::WxCall::hold);
    R::WxApplyIn again = in;
    again.applied = true;
    CHECK(W::WxGate(again).call == R::WxCall::apply && !W::WxGate(again).instant);
    again.rearm = true;
    CHECK(W::WxGate(again).instant);
}

void TestPacing() {
    CHECK(W::WxSendDue(true, 1000.0, 0.0) && !W::WxSendDue(true, 500.0, 0.0));
    CHECK(!W::WxSendDue(false, 9999.0, 0.0) && W::WxSendDue(false, 10000.0, 0.0));
    CHECK(W::WxCompareDue(false, 30000.0, 0.0) && !W::WxCompareDue(false, 29999.0, 0.0));
    CHECK(W::WxCompareDue(true, 5000.0, 0.0) && !W::WxCompareDue(true, 4999.0, 0.0));
    W::WxRate r;
    CHECK(r.Allow(0.0, 1000.0) && !r.Allow(500.0, 1000.0) && !r.Allow(900.0, 1000.0) && r.Allow(1000.0, 1000.0));
    CHECK(r.TakeSkipped() == 2 && r.TakeSkipped() == 0);
    CHECK(W::WxRainBand(true, -0.8f) == 0 && W::WxRainBand(true, -0.5f) == 1 && W::WxRainBand(true, 0.0f) == 1 && W::WxRainBand(true, 0.3f) == 2);
    CHECK(W::WxRainBand(false, -0.8f) == 3 && W::WxRainBand(true, NAN) == 3);
}

// End to end: the host's block -> WXROW chunks on the wire (the phase in the clock field) -> the guest's assembler -> the
// rows by region the detour writes -> the guest's base[32] equals the host's eff for every masked region.
void TestEndToEnd() {
    static u8 host[W::kWxBlock], guest[W::kWxBlock];
    for (unsigned r = 0; r < R::kWxRegions; ++r) {
        for (unsigned i = 0; i < R::kWxSlots; ++i) host[r * W::kRowSize + W::kRowEff + i] = static_cast<u8>((r + 2 * i) & 3);
        FillRow(guest + r * W::kRowSize + W::kRowKeep, static_cast<int>(r) + 9);
    }
    const u64 mask = 0xFFFFFFFFFFF7FFFFull;
    R::WxSchedule s;
    CHECK(W::WxPackBlock(host, 2, mask, s));
    s.gen = 7;
    s.clock = W::WxPhase(430430.0f, 430420.0f);  // 10 s into the host's slot
    R::BtsWxRow ch[R::kWxChunksMax];
    const unsigned k = R::WxChunks(s, ch, R::kWxChunksMax);
    CHECK(k == 3);
    R::WxAssembler a;
    R::WxPut last = R::WxPut::refused;
    float phase = -1;
    for (unsigned i = 0; i < k; ++i) {
        u8 buf[R::kBtsMaxSize];
        const std::size_t n = R::EncodeWxRow(100 + i, ch[i], buf, sizeof(buf));
        R::BtsWxRow m;
        CHECK(n == R::kBtsWxRowBase + ch[i].n * R::kBtsWxRowEntry && R::DecodeWxRow(buf, n, m));
        last = a.Take(m);
        phase = m.clock;
    }
    CHECK(last == R::WxPut::complete && a.have_done && a.done.gen == 7 && a.done.crc == s.crc && phase == 10.0f);
    // a resend of the same gen is a duplicate (the module re-anchors the phase from its first chunk)
    CHECK(a.Take(ch[0]) == R::WxPut::duplicate);
    u64 rows[R::kWxRegions];
    CHECK(W::WxRowsByRegion(a.done, rows));
    const float win0 = W::WxWindowStart(phase, 430440.0f, true, 430445.0f);  // her own clock, not the host's
    CHECK(win0 == 430445.0f - 15.0f);
    bool all = true;
    u8 eff[R::kWxSlots], img[W::kRowSaved];
    for (unsigned r = 0; r < R::kWxRegions; ++r) {
        if (!(mask >> r & 1u)) continue;
        R::UnpackRow(rows[r], eff);
        u8* p = guest + r * W::kRowSize + W::kRowKeep;
        W::WxRowImage(p, eff, win0, img);
        std::memcpy(p, img, W::kRowSaved);
        all &= std::memcmp(guest + r * W::kRowSize + W::kRowBase, host + r * W::kRowSize + W::kRowEff, R::kWxSlots) == 0;
    }
    CHECK(all);
}

// The saved line's eff0 at Sam (gate batch 20261128-02: the first save came before the region at Sam was read).
void TestSavedLine() {
    u8 block[W::kWxBlock] = {};
    const u64 mask = 0x7FFFFFFFFFFFFFFFull;  // 63 regions, region 63 outside
    for (unsigned r = 0; r < R::kWxRegions; ++r) block[r * W::kRowSize + W::kRowEff] = static_cast<u8>(r % 4);
    block[43 * W::kRowSize + W::kRowEff] = 1;
    W::WxSaved s;
    CHECK(s.Save(block, 0x1000, 2, mask));
    u64 e8[W::kWxEff0Words];
    W::WxSavedEff0s(s, e8);
    bool same = true;
    for (int r = -2; r < 70; ++r) same &= W::WxEff0At(e8, r) == s.Eff0(r);
    CHECK(same);
    CHECK(W::WxEff0At(e8, 43) == 1 && W::WxEff0At(e8, 2) == 2 && W::WxEff0At(e8, 63) == R::kWxUnknown && W::WxEff0At(e8, -1) == R::kWxUnknown);
    // nothing saved: all unknown
    W::WxSaved none;
    W::WxSavedEff0s(none, e8);
    CHECK(W::WxEff0At(e8, 0) == R::kWxUnknown && W::WxEff0At(e8, 43) == R::kWxUnknown);
    // the line: at once with a region read, else waits up to 10 s; a restore / drop / slot change flushes it
    CHECK(W::WxSavedLineDue(43, 1000.0, 1000.0, false));
    CHECK(W::WxSavedLineDue(0, 1000.0, 1000.0, false));
    CHECK(!W::WxSavedLineDue(-1, 1000.0, 1000.0, false));
    CHECK(!W::WxSavedLineDue(64, 1000.0, 1000.0, false));
    CHECK(!W::WxSavedLineDue(-1, 10999.0, 1000.0, false));
    CHECK(W::WxSavedLineDue(-1, 11000.0, 1000.0, false));
    CHECK(W::WxSavedLineDue(-1, 1000.0, 1000.0, true));
}

int main() {
    TestHello();
    TestLayout();
    TestPhase();
    TestRowImage();
    TestSaveRestore();
    TestPackAndRegions();
    TestSeqlock();
    TestCapsAndGate();
    TestPacing();
    TestEndToEnd();
    TestSavedLine();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
