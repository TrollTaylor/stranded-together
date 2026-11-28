// Round 31 stage 2 (bt_wx): this feature's own pure logic (no game, no Windows), tested by tests/bt_wx_tests.cpp. The
// shared codecs (WXROW / WXBEAT), the chunk assembler and WxApplyGate stay in bt_rules.hpp (included); what bt_wx adds is here:
// the slot phase and the row window, the row image the guest writes, the saved own rows and their write-back, the seqlock the
// detours share with the game thread, the caps, the gate's real-save order and the log pacing.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "bt_rules.hpp"

namespace st::btrules::wx {

constexpr int kStage = 2;

// ---------------------------------------------------------------- the weather system's layout (ds.exe DC; the build spec, Stage 2)
// A row is ws + 0x8A0 + (slot * 0x40 + r) * 0x5C: +0x00 type (never written), +0x04 / +0x08 the window, +0x0C base[32],
// +0x2C eff[32], +0x4C / +0x50 / +0x54 forced, +0x58 bits. One slot's 64 rows are contiguous (kWxBlock bytes).
constexpr std::size_t kRowSize = 0x5C, kRowWin0 = 0x04, kRowWin1 = 0x08, kRowBase = 0x0C, kRowEff = 0x2C, kRowForced = 0x4C, kRowBits = 0x58;
constexpr std::size_t kRowKeep = 0x04;                  // the saved part starts after the type
constexpr std::size_t kRowSaved = kRowSize - kRowKeep;  // 0x58: +0x04..+0x5B, everything but the type
constexpr std::size_t kWxBlock = kWxRegions * kRowSize; // 0x1700: one slot's rows
constexpr std::uintptr_t kWsMask = 0x48, kWsRows = 0x8A0, kWsSlot = 0x64A0, kWsClock = 0x64A8, kWsLast = 0x64AC, kWsInstant = 0x6515,
                         kWsJob = 0x6998, kJobState = 0x28;
constexpr std::uint32_t kJobDone = 0x7111BEEFu;  // [[ws+0x6998]+0x28]: 0 idle or this (done) = the job is not running
constexpr std::uint32_t kBitsRoll = 1u, kBitsOwn = 5u;  // +0x58: bit 0 the shift's random roll, bits 0 and 2 its own cancel / force
constexpr float kSlotLen = kForecastSlotLen;           // 600 s
constexpr float kWindowLen = 32.0f * kForecastSlotLen;  // 32 slots
inline std::size_t RowOffset(int slot, int r) {
    return kWsRows + (static_cast<std::size_t>(slot) * 0x40 + static_cast<std::size_t>(r)) * kRowSize;
}
inline bool JobIdle(std::uint32_t st) { return st == 0 || st == kJobDone; }

// ---------------------------------------------------------------- the slot phase (WXROW's clock field) and the row window
// The host's phase: how far into its current slot it is, host clock - host last (ws+0x64A8 - ws+0x64AC), clamped to 0..600 s
// (a non-finite or negative value is 0). The clocks drift between games (PF 5 read 3), so the phase travels, not the clock.
inline float WxPhase(float clock, float last) {
    if (!std::isfinite(clock) || !std::isfinite(last)) return 0.0f;
    const float p = clock - last;
    return !(p > 0.0f) ? 0.0f : p > kSlotLen ? kSlotLen : p;
}
// The guest's window start: its own clock minus the phase now. The phase came with the snapshot (p0) when the guest's own clock
// read `anchor`; it grows with the guest's clock since then, at most to 600 s (a clock that went back, or no anchor, adds 0).
// row+0x04 = this, row+0x08 = this + 32 * 600: the whole window ahead, so the guest's job neither shifts nor regenerates the row.
inline float WxPhaseNow(float p0, float anchor, bool anchored, float clock) {
    float p = std::isfinite(p0) ? p0 : 0.0f;
    if (anchored && std::isfinite(anchor) && std::isfinite(clock) && clock > anchor) p += clock - anchor;
    return !(p > 0.0f) ? 0.0f : p > kSlotLen ? kSlotLen : p;
}
inline float WxWindowStart(float p0, float anchor, bool anchored, float clock) { return clock - WxPhaseNow(p0, anchor, anchored, clock); }

// ---------------------------------------------------------------- the row image the guest writes (before the job runs)
// `cur` is the row's saved part as read now (kRowSaved bytes from +0x04); `eff` the host's 32 effective values (0..3). The image:
// the window from win0, base[32] = the host's eff, forced = 0, bits &= ~5 (bit 1, its own dry spots, stays); eff (+0x2C) as read.
// Returns which of the two written parts differ from `cur`: kPartHead (+0x04..+0x2B: the window and base) and kPartTail
// (+0x4C..+0x5B: forced and bits). The module writes only those parts, so a steady apply writes nothing.
constexpr unsigned kPartHead = 1u, kPartTail = 2u;
constexpr std::size_t kHeadAt = kRowWin0 - kRowKeep, kHeadLen = kRowEff - kRowWin0;      // 0x00, 0x28
constexpr std::size_t kTailAt = kRowForced - kRowKeep, kTailLen = kRowSize - kRowForced;  // 0x48, 0x10
inline unsigned WxRowImage(const std::uint8_t* cur, const std::uint8_t* eff, float win0, std::uint8_t* out) {
    if (!cur || !eff || !out) return 0;
    std::memcpy(out, cur, kRowSaved);
    const float win[2] = {win0, win0 + kWindowLen};
    std::memcpy(out + (kRowWin0 - kRowKeep), win, sizeof(win));
    for (unsigned i = 0; i < kWxSlots; ++i) out[kRowBase - kRowKeep + i] = static_cast<std::uint8_t>(eff[i] & 3u);
    std::memset(out + (kRowForced - kRowKeep), 0, 12);
    std::uint32_t bits = 0;
    std::memcpy(&bits, out + (kRowBits - kRowKeep), 4);
    bits &= ~kBitsOwn;
    std::memcpy(out + (kRowBits - kRowKeep), &bits, 4);
    unsigned parts = 0;
    if (std::memcmp(out + kHeadAt, cur + kHeadAt, kHeadLen) != 0) parts |= kPartHead;
    if (std::memcmp(out + kTailAt, cur + kTailAt, kTailLen) != 0) parts |= kPartTail;
    return parts;
}
// After the job ran in an applied run: bit 0 (a shift's random roll in that same run) off again. True when it was on.
inline bool WxClearRoll(std::uint32_t& bits) {
    if (!(bits & kBitsRoll)) return false;
    bits &= ~kBitsRoll;
    return true;
}

// ---------------------------------------------------------------- the guest's own rows (saved before the first write)
// One slot's masked rows, +0x04..+0x5B each, copied once before the first write for that slot and written back to restore (the
// build spec's change 1: the design's regeneration restore left eff0 at the written value, PF 3). `ws` is the weather system they
// came from: a restore into another one (a load replaced it) writes nothing and drops them.
struct WxSaved {
    bool have = false;
    int slot = 0;
    std::uint64_t mask = 0;
    std::uintptr_t ws = 0;
    std::uint8_t rows[kWxRegions][kRowSaved] = {};
    // block: the slot's kWxBlock bytes as read now (row r at r * kRowSize).
    bool Save(const std::uint8_t* block, std::uintptr_t from_ws, int at_slot, std::uint64_t at_mask) {
        if (!block || at_slot < 1 || at_slot > 3 || !at_mask) return false;
        for (unsigned r = 0; r < kWxRegions; ++r)
            if (at_mask >> r & 1u) std::memcpy(rows[r], block + r * kRowSize + kRowKeep, kRowSaved);
        have = true, slot = at_slot, mask = at_mask, ws = from_ws;
        return true;
    }
    // The saved eff[0] of region r (0xFF: none saved there).
    std::uint8_t Eff0(int r) const {
        if (!have || r < 0 || r >= static_cast<int>(kWxRegions) || !(mask >> r & 1u)) return kWxUnknown;
        return rows[r][kRowEff - kRowKeep];
    }
    void Drop() { have = false, slot = 0, mask = 0, ws = 0; }
};
// The write-back into a slot block in memory (the tests' model of WriteN(row + 0x04, rows[r], 0x58) per masked row).
inline bool WxWriteBack(const WxSaved& s, std::uint8_t* block) {
    if (!s.have || !block) return false;
    for (unsigned r = 0; r < kWxRegions; ++r)
        if (s.mask >> r & 1u) std::memcpy(block + r * kRowSize + kRowKeep, s.rows[r], kRowSaved);
    return true;
}

// ---------------------------------------------------------------- the packed rows by region index
// A snapshot keeps its rows in mask order (WxSchedule); the detours want them by region index (zero outside the mask).
inline bool WxRowsByRegion(const WxSchedule& s, std::uint64_t (&out)[kWxRegions]) {
    for (auto& v : out) v = 0;
    if (s.n != Popcount64(s.mask) || s.n > kWxRegions) return false;
    unsigned k = 0;
    for (unsigned r = 0; r < kWxRegions; ++r)
        if (s.mask >> r & 1u) out[r] = s.rows[k++];
    return true;
}
// A game's own slot block into a schedule (eff +0x2C of each masked row, packed). False: a value above 3 or a bad slot / mask.
inline bool WxPackBlock(const std::uint8_t* block, int slot, std::uint64_t mask, WxSchedule& out) {
    if (!block || slot < 1 || slot > 3 || !mask) return false;
    out = WxSchedule{};
    out.slot = static_cast<std::uint8_t>(slot), out.mask = mask;
    for (unsigned r = 0; r < kWxRegions; ++r) {
        if (!(mask >> r & 1u)) continue;
        if (!PackRow(block + r * kRowSize + kRowEff, &out.rows[out.n])) return false;
        ++out.n;
    }
    out.crc = ScheduleCrc(out.mask, out.rows, out.n);
    return true;
}
// The other way: a by-region packed set into a schedule (mask order, crc). False: a bad slot or an empty mask.
inline bool WxFromRegions(int slot, std::uint64_t mask, const std::uint64_t (&rows)[kWxRegions], WxSchedule& out) {
    if (slot < 1 || slot > 3 || !mask) return false;
    out = WxSchedule{};
    out.slot = static_cast<std::uint8_t>(slot), out.mask = mask;
    for (unsigned r = 0; r < kWxRegions; ++r)
        if (mask >> r & 1u) out.rows[out.n++] = rows[r];
    out.crc = ScheduleCrc(out.mask, out.rows, out.n);
    return true;
}
// Region r's eff[0] in a by-region packed set (0xFF outside the mask).
inline std::uint8_t WxEff0(const std::uint64_t (&rows)[kWxRegions], std::uint64_t mask, int r) {
    if (r < 0 || r >= static_cast<int>(kWxRegions) || !(mask >> r & 1u)) return kWxUnknown;
    return static_cast<std::uint8_t>(rows[r] & 3u);
}

// ---------------------------------------------------------------- the saved line's eff0 at Sam (gate batch 20261128-02)
// The first apply can come before this game has read the region at Sam: on the load's tail (the guest's player set up, not
// placed yet) run 20261128-020121 saved at region -1 and logged "eff0 ?", which leaves leg 3 no E to compare. The detour keeps
// the saved eff[0] of every region (one byte each, 0xFF outside the mask, 8 per word) so the Tick names the value at Sam once
// the region is read. The line waits for that at most kWxSavedLineWaitMs (then it says "?" at region -1), and a restore, a
// drop or a slot change logs it at once (the saved line always comes before the line that ends those rows). Log only: what
// is saved and written back does not change.
constexpr std::size_t kWxEff0Words = kWxRegions / 8;
inline void WxSavedEff0s(const WxSaved& s, std::uint64_t (&out)[kWxEff0Words]) {
    for (auto& w : out) w = 0;
    for (unsigned r = 0; r < kWxRegions; ++r) out[r / 8] |= static_cast<std::uint64_t>(s.Eff0(static_cast<int>(r))) << (r % 8 * 8);
}
inline std::uint8_t WxEff0At(const std::uint64_t (&in)[kWxEff0Words], int r) {
    if (r < 0 || r >= static_cast<int>(kWxRegions)) return kWxUnknown;
    return static_cast<std::uint8_t>(in[r / 8] >> (r % 8 * 8));
}
constexpr double kWxSavedLineWaitMs = 10000.0;
inline bool WxSavedLineDue(int region, double now, double first_seen, bool flush) {
    return flush || (region >= 0 && region < static_cast<int>(kWxRegions)) || now - first_seen >= kWxSavedLineWaitMs;
}

// ---------------------------------------------------------------- the seqlock (one writer, any number of readers; atomics only)
// The detours and the game thread share two of these: the game's own rows (written by the job detour, read by the Tick) and the
// host's snapshot to apply (written by the Tick, read by the job detour). No allocation, no lock, no wait: a reader that loses the
// race gives up after a few tries (the detour then skips that run).
template <std::size_t N>
struct WxSeqlock {
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint64_t> w[N] = {};
    void Write(const std::uint64_t* v) noexcept {
        const std::uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < N; ++i) w[i].store(v[i], std::memory_order_relaxed);
        seq.store(s + 2, std::memory_order_release);
    }
    // False: nothing written yet, or a writer kept it busy for `tries` reads.
    bool Read(std::uint64_t* v, int tries = 4) const noexcept {
        for (int t = 0; t < tries; ++t) {
            const std::uint32_t s0 = seq.load(std::memory_order_acquire);
            if (s0 == 0 || (s0 & 1u)) continue;
            for (std::size_t i = 0; i < N; ++i) v[i] = w[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s0) return true;
        }
        return false;
    }
    std::uint32_t Version() const noexcept { return seq.load(std::memory_order_acquire); }
};
// Word layouts (u64 each).
// The own rows: [0] slot (i32, low) | 1 << 32 valid; [1] mask; [2] clock bits | last bits << 32; [3] ws; [4] taken at (ms, the
// detour's GetTickCount64); [5 + r] region r's packed eff.
// The snapshot to apply: [0] slot | gen << 32; [1] mask; [2] p0 bits | anchor bits << 32; [3] 1 = anchored; [4] crc; [5 + r] rows.
constexpr std::size_t kSnapHead = 5, kSnapWords = kSnapHead + kWxRegions;
inline std::uint64_t PackF2(float a, float b) {
    std::uint32_t x = 0, y = 0;
    std::memcpy(&x, &a, 4), std::memcpy(&y, &b, 4);
    return static_cast<std::uint64_t>(x) | static_cast<std::uint64_t>(y) << 32;
}
inline void UnpackF2(std::uint64_t v, float& a, float& b) {
    const std::uint32_t x = static_cast<std::uint32_t>(v), y = static_cast<std::uint32_t>(v >> 32);
    std::memcpy(&a, &x, 4), std::memcpy(&b, &y, 4);
}

// ---------------------------------------------------------------- the levels, the caps and the gate
// bt_weather as it acts here: 2 (apply) needs bt_sync 2 as well; bt_sync 1 compares only; bt_sync 0 is off.
inline int WxLevel(int bt_weather, int sync_level) {
    if (bt_weather <= 0 || sync_level <= 0) return 0;
    return bt_weather >= 2 && sync_level >= 2 ? 2 : 1;
}
// The hello's caps from this game. kCapWx: this game sends its forecast (the host: WXROW and WXBEAT; a guest: its WXBEAT, so the
// host sends it the rows to compare); kCapWxApply: a sandboxed guest (not on its real save) at level 2 applies them.
inline std::uint16_t WxCaps(int role, int bt_weather, int sync_level, bool real_save, bool checks_ok) {
    const int lv = WxLevel(bt_weather, sync_level);
    if (!lv || !checks_ok || (role != 1 && role != 2)) return 0;
    if (role == 1) return kCapWx;
    return static_cast<std::uint16_t>(kCapWx | (lv >= 2 && !real_save ? kCapWxApply : 0));
}
// The partner is worth talking to: its hello names WX and it plays the other role.
inline bool WxPartner(int my_role, int peer_role, std::uint16_t peer_caps) {
    return (my_role == 1 || my_role == 2) && peer_role != my_role && (peer_role == 1 || peer_role == 2) && (peer_caps & (kCapWx | kCapWxApply)) != 0;
}
// WxApplyGate with the real-save rule first: a guest on her own real save (host_save=0) is never written, whatever bt_weather
// says, and her log names that reason (the build spec's leg 4), not "compare only".
inline WxVerdict WxGate(const WxApplyIn& in) {
    if (!in.emergency && in.role == 2 && !in.sandboxed)
        return WxVerdict{in.applied ? WxCall::restore : WxCall::hold, "this game plays on its own real save (host_save=0)", false};
    return WxApplyGate(in);
}

// ---------------------------------------------------------------- pacing
// The host: a new gen goes at once (at most once a second); the current one again every 10 s (it refreshes the guest's phase and
// mends a lost chunk). The beat every 2 s. The compare every 30 s, and on a change (at most every 5 s).
constexpr double kWxGenGapMs = 1000.0, kWxResendMs = 10000.0, kWxBeatMs = 2000.0, kWxCompareMs = 30000.0, kWxCompareChangeMs = 5000.0;
constexpr double kWxOwnStaleMs = 5000.0;          // the own rows older than this (the job stopped): not used
constexpr double kWxGoneHoldMs = 10000.0;         // after a "partner gone" restore: no new apply for this long
constexpr double kWxTickRestoreMs = 3000.0;       // a restore no detour wrote for this long: the Tick writes it (job idle only)
inline bool WxSendDue(bool new_gen, double now, double last_send) {
    const double since = now - last_send;
    return new_gen ? since >= kWxGenGapMs : since >= kWxResendMs;
}
inline bool WxCompareDue(bool changed, double now, double last) {
    const double since = now - last;
    return since >= kWxCompareMs || (changed && since >= kWxCompareChangeMs);
}
// One log line's pace: at most one per `every` ms; the skipped ones are counted and named in the next line.
struct WxRate {
    double last = -1e18;
    unsigned skipped = 0;
    bool Allow(double now, double every) {
        if (now - last >= every) {
            last = now;
            return true;
        }
        ++skipped;
        return false;
    }
    unsigned TakeSkipped() {
        const unsigned s = skipped;
        skipped = 0;
        return s;
    }
};

// The compare's rain band at one game's Sam (a band change logs a compare line at once, at most every 5 s, so the gate's lines
// come when the rain turns, not only every 30 s): 0 below -0.5 (the timefall proof), 1 from -0.5 to 0, 2 above 0, 3 not read.
inline int WxRainBand(bool ok, float raw) {
    if (!ok || !std::isfinite(raw)) return 3;
    return raw < -0.5f ? 0 : raw > 0.0f ? 2 : 1;
}

// The WXBEAT state from this game's play state.
inline std::uint8_t WxBeatStateOf(bool loading, bool playing) { return loading ? kWxLoading : playing ? kWxPlaying : kWxPaused; }

}  // namespace st::btrules::wx
