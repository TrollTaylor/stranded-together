// Round 31 stage 2: WX: the host's forecast on a sandboxed guest (bt_wx.hpp; the build spec, Stage 2). The game-call code, the
// byte checks and the SEH wrappers are bt_probe_r31.cpp's (ops 15, 16 and 18), copied: the job and pre-job detours, the row
// write before the job (probe 18's WxApplyPre / WxApplyPost), the region and raw-rain calls. Changed from the probe as the spec
// says: the restore writes back the guest's own saved rows (the probe's regeneration restore failed, PF 3), the window comes from
// the host's slot phase, every masked region is written, and only the parts of a row that differ are written.
#include "bt_wx.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bt_rules.hpp"
#include "bt_sync_internal.hpp"
#include "bt_wx_rules.hpp"
#include "config.hpp"
#include "log.hpp"

namespace st::bt::wx {
namespace {

namespace R = btrules;
namespace W = btrules::wx;
namespace I = bt::internal;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using uptr = std::uintptr_t;

// ================================================================ ds.exe DC (bt_probe_r31.cpp's weather addresses)
constexpr uptr kWsGlobal = 0x147BF83B8, kJobFn = 0x142E183F0, kPreFn = 0x142E17CF0, kRegionFn = 0x142E0C6B0, kRainRawFn = 0x142E0C3F0;
constexpr uptr kMgr238 = 0x238;

struct Site {
    uptr va;
    const char* sig;
    const char* name;
};
// The probe's kGWx sites (the clamped rain is not called here), byte for byte; its RIP check of the weather global below.
constexpr Site kSites[] = {
    {kJobFn, "48 8B C4 53 55 56 41 54 41 55 48 81 EC 30 01 00 00 0F B6 99 68 69 00 00 48 8B F1", "the forecast job FUN_142E183F0"},
    {0x142E045A0, "C5 FA 10 89 90 69 00 00 E9 43 3E 01 00", "the job's jmp (dt = ws+0x6990) at 0x142E045A0"},
    {0x142E0C4D6, "4C 63 81 A0 64 00 00", "the rain's slot read +0x64A0 (0x142E0C4D6)"},
    {0x142E19BA3, "C6 86 15 65 00 00 00", "the instant-grid flag +0x6515 (0x142E19BA3)"},
    {0x142E1FE79, "66 3B F0 75 29 48 63 8B A0 64 00 00 48 C1 E1 06 48 83 C1 18 40 0F B6 C5 48 03 C8 40 0F B6 C7 48 6B D1 5C 89 04 1A",
     "the weather rows (0x142E1FE79)"},
    {kRegionFn, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30", "the region at a point FUN_142E0C6B0"},
    {kRainRawFn, "48 83 EC 48 C5 FB 10 42 10 C5 FB 5C 49 18 C5 FB 10 52 08 C5 EB 5C 41 10", "the raw rain FUN_142E0C3F0"},
};
// The pre-job: its own check (failed: no pre-job hook; the restore then comes from the job detour, or the Tick / EmergencyStop
// while the job is idle).
constexpr Site kPreSite = {kPreFn, "48 89 5C 24 18 55 56 57 48 83 EC 60 48 8B D9 C5 F8 29 74 24 50 48 8B 0D 7C F1 DD 04", "the pre-job FUN_142E17CF0"};

void Put(char* out, std::size_t cap, const char* fmt, ...) {
    if (!out || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(out, cap, fmt, ap);
    va_end(ap);
}

// ================================================================ config and the byte checks (game thread)
int g_key = 0;      // bt_weather (Install)
int g_checks = -1;  // -1 not run, 0 failed (WX off), 1 ok
bool g_pre_ok = false;

bool Check() noexcept {
    if (g_checks >= 0) return g_checks == 1;
    int ok = 0, total = 0;
    bool all = true;
    for (const Site& s : kSites) {
        ++total;
        if (I::SiteOk(s.va, s.sig)) ++ok;
        else all = false, Log("[bt] wx: r31 byte check FAILED: wx (%s): wx off", s.name);
    }
    ++total;
    if (I::RipOk(0x142E1FE34, 3, 7, kWsGlobal)) ++ok;
    else all = false, Log("[bt] wx: r31 byte check FAILED: wx (the weather global 0x147BF83B8, its load at 0x142E1FE34): wx off");
    ++total;
    g_pre_ok = I::SiteOk(kPreSite.va, kPreSite.sig);
    if (g_pre_ok) ++ok;
    else Log("[bt] wx: byte check failed: %s: no pre-job hook (the restore comes from the job detour, else from the Tick while the job is idle)", kPreSite.name);
    g_checks = all ? 1 : 0;
    Log("[bt] wx: byte checks %d/%d ok; wx %s, the pre-job %s", ok, total, all ? "ok" : "OFF", g_pre_ok ? "ok" : "off");
    return all;
}

// ================================================================ guarded game calls (game thread; POD only inside __try)
bool SehRainRaw(uptr ws, const double* p, float* out) noexcept {
    __try {
        *out = reinterpret_cast<float (*)(void*, const double*)>(I::Addr(kRainRawFn))(reinterpret_cast<void*>(ws), p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehRegion(uptr ws, const double* p, int* out) noexcept {
    __try {
        *out = reinterpret_cast<int (*)(void*, const double*)>(I::Addr(kRegionFn))(reinterpret_cast<void*>(ws), p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ================================================================ the detours (any thread: atomics, seqlocks, guarded memory only)
enum Mode : int { kModeIdle = 0, kModeApply = 1, kModeRestore = 2 };

using FnF = void (*)(void*, float, u64, u64);
FnF g_job_orig = nullptr, g_pre_orig = nullptr;
std::atomic<bool> g_job_in{false}, g_pre_in{false};
std::atomic<bool> g_pass{false};       // EmergencyStop: the job detour straight through (the pre-job still writes a pending restore)
std::atomic<bool> g_stopped{false};    // EmergencyStop ran
std::atomic<bool> g_busy{false};       // one detour at a time touches the saved rows and the block (a try-lock: never waits)
std::atomic<bool> g_may_write{false};  // a sandboxed guest at level 2 (set once at the hook install; never on a real save)
std::atomic<bool> g_instant{false};    // the next apply also sets the instant grid +0x6515 = 1 (once)
std::atomic<bool> g_have_saved{false}; // g_saved.have, for the game thread
std::atomic<int> g_mode{kModeIdle};
std::atomic<int> g_sam_region{-1};  // the Tick's region at Sam (the saved / restored eff0 of the log lines)
std::atomic<const char*> g_restore_why{nullptr};
// counters (the detours count, the Tick logs)
std::atomic<u32> g_c_applies{0}, g_c_writes{0}, g_c_wfail{0}, g_c_skips{0}, g_c_saves{0}, g_c_restores{0}, g_c_dropped{0}, g_c_swaps{0},
    g_c_rolls{0}, g_c_instants{0}, g_c_pre{0}, g_c_busy{0};
std::atomic<u32> g_applied_gen{0};
std::atomic<int> g_saved_slot{0}, g_saved_n{0}, g_swap_from{0}, g_swap_to{0};
std::atomic<unsigned> g_restored_eff0{0xFFu};
std::atomic<u64> g_saved_eff0s[W::kWxEff0Words] = {};  // the saved eff[0] by region (W::WxSavedEff0s), for the saved line
std::atomic<u64> g_own_next_ms{0};
W::WxSeqlock<W::kSnapWords> g_own;   // this game's own rows (the job detour writes, the Tick reads)
W::WxSeqlock<W::kSnapWords> g_snap;  // the host's snapshot to apply (the Tick writes, the job detour reads)
W::WxSaved g_saved;                  // under g_busy
u8 g_block[W::kWxBlock];             // under g_busy

struct Busy {
    bool got;
    Busy() noexcept : got(!g_busy.exchange(true, std::memory_order_acquire)) {
        if (!got) g_c_busy.fetch_add(1, std::memory_order_relaxed);
    }
    ~Busy() {
        if (got) g_busy.store(false, std::memory_order_release);
    }
    Busy(const Busy&) = delete;
    Busy& operator=(const Busy&) = delete;
};

uptr RowAt(uptr ws, int slot, int r) noexcept { return ws + W::RowOffset(slot, r); }
bool JobIdleNow(uptr ws) noexcept {
    uptr job = 0;
    u32 st = 1;
    return ws && I::Read(ws + W::kWsJob, job) && job && I::Read(job + W::kJobState, st) && W::JobIdle(st);
}

// The saved rows into ws (under g_busy; the caller checked ws == g_saved.ws).
void WriteBack(uptr ws) noexcept {
    u32 bad = 0;
    for (unsigned r = 0; r < R::kWxRegions; ++r)
        if (g_saved.mask >> r & 1u)
            if (!I::WriteN(RowAt(ws, g_saved.slot, static_cast<int>(r)) + W::kRowKeep, g_saved.rows[r], W::kRowSaved)) ++bad;
    if (bad) g_c_wfail.fetch_add(bad, std::memory_order_relaxed);
}
void DropSaved() noexcept {
    g_saved.Drop();
    g_have_saved.store(false, std::memory_order_release);
}
// The restore (under g_busy): the own rows back when they came from this weather system, else dropped (a load replaced it).
void DoRestore(uptr ws) noexcept {
    if (g_saved.have) {
        if (g_saved.ws == ws) {
            g_restored_eff0.store(g_saved.Eff0(g_sam_region.load(std::memory_order_relaxed)), std::memory_order_relaxed);
            WriteBack(ws);
            g_c_restores.fetch_add(1, std::memory_order_release);
        } else {
            g_c_dropped.fetch_add(1, std::memory_order_release);
        }
        DropSaved();
    }
    int want = kModeRestore;
    g_mode.compare_exchange_strong(want, kModeIdle, std::memory_order_acq_rel);
}
bool Skip() noexcept {
    g_c_skips.fetch_add(1, std::memory_order_relaxed);
    return false;
}
// The apply (under g_busy, before the job): probe 18's write for every masked region, from the host's snapshot. Nothing is
// written before the own rows of this slot are saved; a slot change writes the old slot's back first.
bool DoApply(uptr ws, int* slot_out, u64* mask_out) noexcept {
    u64 v[W::kSnapWords];
    if (!g_snap.Read(v)) return Skip();
    const int sslot = static_cast<int>(static_cast<u32>(v[0]));
    const u32 gen = static_cast<u32>(v[0] >> 32);
    const u64 smask = v[1];
    float p0 = 0, anchor = 0;
    W::UnpackF2(v[2], p0, anchor);
    const bool anchored = (v[3] & 1u) != 0;
    std::int32_t slot = 0;
    u64 mask = 0;
    float clock = 0;
    if (!I::Read(ws + W::kWsSlot, slot) || !I::Read(ws + W::kWsMask, mask) || !I::Read(ws + W::kWsClock, clock) || !std::isfinite(clock) ||
        slot < 1 || slot > 3 || slot != sslot || mask != smask || !mask)
        return Skip();
    if (g_saved.have && g_saved.ws != ws) DropSaved(), g_c_dropped.fetch_add(1, std::memory_order_release);
    if (g_saved.have && g_saved.slot != slot) {
        g_swap_from.store(g_saved.slot, std::memory_order_relaxed), g_swap_to.store(slot, std::memory_order_relaxed);
        WriteBack(ws);
        DropSaved();
        g_c_swaps.fetch_add(1, std::memory_order_release);
    }
    if (!I::ReadN(RowAt(ws, slot, 0), g_block, sizeof(g_block))) return Skip();
    if (!g_saved.have) {
        if (!g_saved.Save(g_block, ws, slot, mask)) return Skip();
        u64 e8[W::kWxEff0Words];
        W::WxSavedEff0s(g_saved, e8);
        for (std::size_t i = 0; i < W::kWxEff0Words; ++i) g_saved_eff0s[i].store(e8[i], std::memory_order_relaxed);
        g_saved_slot.store(slot, std::memory_order_relaxed), g_saved_n.store(static_cast<int>(R::Popcount64(mask)), std::memory_order_relaxed);
        g_have_saved.store(true, std::memory_order_release);
        g_c_saves.fetch_add(1, std::memory_order_release);
    }
    const float win0 = W::WxWindowStart(p0, anchor, anchored, clock);
    u8 eff[R::kWxSlots];
    u8 img[W::kRowSaved];
    u32 writes = 0, bad = 0;
    for (unsigned r = 0; r < R::kWxRegions; ++r) {
        if (!(mask >> r & 1u)) continue;
        R::UnpackRow(v[W::kSnapHead + r], eff);
        const unsigned parts = W::WxRowImage(g_block + r * W::kRowSize + W::kRowKeep, eff, win0, img);
        const uptr at = RowAt(ws, slot, static_cast<int>(r)) + W::kRowKeep;
        if (parts & W::kPartHead) ++writes, bad += I::WriteN(at + W::kHeadAt, img + W::kHeadAt, W::kHeadLen) ? 0u : 1u;
        if (parts & W::kPartTail) ++writes, bad += I::WriteN(at + W::kTailAt, img + W::kTailAt, W::kTailLen) ? 0u : 1u;
    }
    if (writes) g_c_writes.fetch_add(writes, std::memory_order_relaxed);
    if (bad) g_c_wfail.fetch_add(bad, std::memory_order_relaxed);
    if (g_instant.exchange(false, std::memory_order_acq_rel)) {
        if (I::Write<u8>(ws + W::kWsInstant, 1)) g_c_instants.fetch_add(1, std::memory_order_relaxed);
        else g_c_wfail.fetch_add(1, std::memory_order_relaxed);
    }
    g_applied_gen.store(gen, std::memory_order_relaxed);
    g_c_applies.fetch_add(1, std::memory_order_release);
    *slot_out = slot, *mask_out = mask;
    return true;
}
// After the job in an applied run: a shift's random roll in this same run (bit 0) undone (probe 18's WxApplyPost).
void PostApply(uptr ws, int slot, u64 mask) noexcept {
    if (!I::ReadN(RowAt(ws, slot, 0), g_block, sizeof(g_block))) return;
    for (unsigned r = 0; r < R::kWxRegions; ++r) {
        if (!(mask >> r & 1u)) continue;
        u32 bits = 0;
        std::memcpy(&bits, g_block + r * W::kRowSize + W::kRowBits, 4);
        if (W::WxClearRoll(bits) && I::Write<u32>(RowAt(ws, slot, static_cast<int>(r)) + W::kRowBits, bits))
            g_c_rolls.fetch_add(1, std::memory_order_relaxed);
    }
}
// This game's own effective rows into g_own (after the job, at most every 250 ms).
void SnapOwn(uptr ws) noexcept {
    const u64 t = GetTickCount64();
    if (t < g_own_next_ms.load(std::memory_order_relaxed)) return;
    g_own_next_ms.store(t + 250, std::memory_order_relaxed);
    u64 v[W::kSnapWords] = {};
    std::int32_t slot = 0;
    u64 mask = 0;
    float clock = 0, last = 0;
    bool ok = I::Read(ws + W::kWsSlot, slot) && I::Read(ws + W::kWsMask, mask) && I::Read(ws + W::kWsClock, clock) && I::Read(ws + W::kWsLast, last) &&
              slot >= 1 && slot <= 3 && mask && std::isfinite(clock) && I::ReadN(RowAt(ws, slot, 0), g_block, sizeof(g_block));
    for (unsigned r = 0; ok && r < R::kWxRegions; ++r)
        if (mask >> r & 1u) ok = R::PackRow(g_block + r * W::kRowSize + W::kRowEff, &v[W::kSnapHead + r]);
    v[0] = static_cast<u64>(static_cast<u32>(slot)) | (ok ? (1ull << 32) : 0ull);
    v[1] = mask;
    v[2] = W::PackF2(clock, last);
    v[3] = ws;
    v[4] = t;
    g_own.Write(v);
}

void JobDetour(void* self, float dt, u64 r8, u64 r9) {
    if (g_pass.load(std::memory_order_relaxed) || !self) return g_job_orig(self, dt, r8, r9);
    const uptr ws = reinterpret_cast<uptr>(self);
    bool wrote = false;
    int slot = 0;
    u64 mask = 0;
    {
        Busy b;
        if (b.got) {
            const int mode = g_mode.load(std::memory_order_acquire);
            if (mode == kModeRestore) DoRestore(ws);
            else if (mode == kModeApply && g_may_write.load(std::memory_order_relaxed)) wrote = DoApply(ws, &slot, &mask);
        }
    }
    g_job_orig(self, dt, r8, r9);
    if (g_pass.load(std::memory_order_relaxed)) return;
    Busy b;
    if (!b.got) return;
    if (wrote) PostApply(ws, slot, mask);
    SnapOwn(ws);
}
// The pre-job runs on the game thread right before the job is queued (PF 1: the job state "done" at every entry). A pending
// restore is written here, once, also in pass-through (EmergencyStop), and only while the job is idle or done.
void PreDetour(void* self, float dt, u64 r8, u64 r9) {
    if (self && g_mode.load(std::memory_order_acquire) == kModeRestore) {
        const uptr ws = reinterpret_cast<uptr>(self);
        if (JobIdleNow(ws)) {
            Busy b;
            if (b.got && g_mode.load(std::memory_order_acquire) == kModeRestore) DoRestore(ws), g_c_pre.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_pre_orig(self, dt, r8, r9);
}

// A restore written from here and now (the Tick when no detour wrote it, or EmergencyStop without the pre-job hook): only while
// the job is idle or done, and only into the weather system the rows came from. wait: up to 100 ms for the job and the lock.
bool DirectRestore(bool wait) noexcept {
    uptr ws = 0;
    if (!I::Read(I::Addr(kWsGlobal), ws) || !ws) return false;
    for (int i = 0; i < (wait ? 100 : 1); ++i) {
        if (JobIdleNow(ws)) {
            Busy b;
            if (b.got) {
                if (g_mode.load(std::memory_order_acquire) == kModeRestore) DoRestore(ws);
                return true;
            }
        }
        if (wait) Sleep(1);
    }
    return false;
}

// ================================================================ the game thread's state
struct Own {
    bool ok = false;
    int slot = 0;
    u64 mask = 0;
    float clock = 0, last = 0;
    u64 rows[R::kWxRegions] = {};
    R::WxSchedule sched;
};
bool ReadOwn(Own& o) noexcept {
    u64 v[W::kSnapWords];
    if (!g_own.Read(v)) return false;
    if (!(v[0] >> 32 & 1u) || GetTickCount64() - v[4] > static_cast<u64>(W::kWxOwnStaleMs)) return false;
    o.slot = static_cast<int>(static_cast<u32>(v[0]));
    o.mask = v[1];
    W::UnpackF2(v[2], o.clock, o.last);
    for (unsigned r = 0; r < R::kWxRegions; ++r) o.rows[r] = v[W::kSnapHead + r];
    o.ok = W::WxFromRegions(o.slot, o.mask, o.rows, o.sched);
    return o.ok;
}

bool g_hooks_tried = false;
double g_now = 0;
// here (every 2 s): the region and the raw rain at Sam
double g_here_next = 0;
bool g_here_ok = false;
int g_region_here = -1;
float g_rain_here = 0;
// the beats
double g_beat_next = 0;
R::BtsWxBeat g_peer_beat;
bool g_have_peer_beat = false;
double g_peer_beat_ms = -1e18;
// the host
u32 g_gen = 0;
bool g_have_last = false, g_new_gen = false, g_send_now = false;
int g_last_slot = 0;
u64 g_last_mask = 0;
u32 g_last_crc = 0;
double g_last_send_ms = -1e18;
// the guest
R::WxAssembler g_asm;
double g_fresh_ms = -1e18;
float g_p0 = 0, g_anchor = 0;
bool g_anchored = false, g_pub_dirty = false, g_rearm = true, g_first_pending = false;
u32 g_first_req_applies = 0, g_pub_gen = 0, g_pub_crc = 0, g_logged_gen = 0;
int g_pub_slot = 0, g_last_own_slot = 0;
double g_hold_until = -1e18, g_restore_req_ms = 0;
const char* g_last_why = nullptr;
const char* g_verdict = "none yet";
// the compare
double g_cmp_last = -1e18;
int g_cmp_last_k = -3;  // the last line's key (Compare): the schedule verdict and the two rain bands
// the counters
unsigned g_tx_rows = 0, g_tx_beats = 0, g_rx_rows = 0, g_rx_beats = 0, g_rx_bad = 0, g_rx_crc = 0, g_rx_restarted = 0;
u32 g_seen_saves = 0, g_seen_restores = 0, g_seen_dropped = 0, g_seen_swaps = 0, g_seen_wfail = 0, g_seen_applies = 0;
bool g_saved_waiting = false;  // a save not logged yet: its line waits for the region at Sam (Drain)
double g_saved_seen_ms = 0;
W::WxRate g_r_sent, g_r_why, g_r_wfail, g_r_rx, g_r_bad;

const char* RoleText(int role) { return role == 1 ? "host" : role == 2 ? "guest" : "other"; }
void Eff0Text(unsigned e, char (&out)[8]) {
    if (e <= 3) std::snprintf(out, sizeof(out), "%u", e);
    else std::snprintf(out, sizeof(out), "?");
}

void Here(const TickCtx& c) noexcept {
    if (c.now_ms < g_here_next) return;
    g_here_next = c.now_ms + W::kWxBeatMs;
    g_here_ok = false;
    if (c.loading || !c.me_ok) return;
    const uptr ws = I::WeatherSys();
    if (!ws) return;
    int region = -1;
    float raw = 0;
    const bool rok = SehRegion(ws, c.me, &region) && region >= 0 && region < static_cast<int>(R::kWxRegions);
    const bool aok = SehRainRaw(ws, c.me, &raw) && std::isfinite(raw);
    g_region_here = rok ? region : -1;
    g_sam_region.store(g_region_here, std::memory_order_relaxed);
    g_rain_here = aok ? raw : 0.0f;
    g_here_ok = rok && aok;
}

void Beat(const TickCtx& c, const Own& own) noexcept {
    if (c.now_ms < g_beat_next || !c.send || !c.seq) return;
    g_beat_next = c.now_ms + W::kWxBeatMs;
    R::BtsWxBeat b;
    b.slot = static_cast<u8>(own.ok ? own.slot : 0);
    b.state = W::WxBeatStateOf(c.loading, c.playing_why == nullptr);
    b.gen = c.role == 1 ? g_gen : 0;
    b.clock = own.ok ? own.clock : 0.0f;
    b.crc = own.ok ? own.sched.crc : 0;
    b.rain = g_here_ok ? g_rain_here : 0.0f;
    b.region = g_here_ok ? static_cast<u8>(g_region_here) : R::kWxUnknown;
    b.eff0 = own.ok && g_here_ok ? W::WxEff0(own.rows, own.mask, g_region_here) : R::kWxUnknown;
    b.sender_ms = static_cast<u32>(static_cast<u64>(c.now_ms));
    u8 buf[R::kBtsWxBeatSize];
    const std::size_t n = R::EncodeWxBeat(c.seq(), b, buf, sizeof(buf));
    if (n && c.send(buf, n)) ++g_tx_beats;
}

void HostTick(const TickCtx& c, const Own& own) noexcept {
    if (c.loading || !own.ok || !c.send || !c.seq) return;
    const R::WxSchedule& s0 = own.sched;
    if (!g_have_last || s0.slot != g_last_slot || s0.mask != g_last_mask || s0.crc != g_last_crc) {
        ++g_gen;
        g_have_last = true, g_new_gen = true;
        g_last_slot = s0.slot, g_last_mask = s0.mask, g_last_crc = s0.crc;
    }
    if (!g_send_now && !W::WxSendDue(g_new_gen, c.now_ms, g_last_send_ms)) return;
    R::WxSchedule s = s0;
    s.gen = g_gen;
    s.clock = W::WxPhase(own.clock, own.last);  // the slot phase (the build spec's change 2), not the clock
    R::BtsWxRow ch[R::kWxChunksMax];
    const unsigned k = R::WxChunks(s, ch, R::kWxChunksMax);
    if (!k) return;
    g_last_send_ms = c.now_ms;
    for (unsigned i = 0; i < k; ++i) {
        u8 buf[R::kBtsMaxSize];
        const std::size_t n = R::EncodeWxRow(c.seq(), ch[i], buf, sizeof(buf));
        if (!n || !c.send(buf, n)) return;  // again at the next resend
        ++g_tx_rows;
    }
    if (g_new_gen && g_r_sent.Allow(c.now_ms, 5000.0))
        Log("[bt] wx sent: slot %d gen %u, %u regions (%u messages), crc %08X (phase %.1f s)%s", static_cast<int>(s.slot), s.gen, s.n, k, s.crc, s.clock,
            g_r_sent.skipped ? " (and earlier gens)" : "");
    g_r_sent.TakeSkipped();
    g_new_gen = false, g_send_now = false;
}

void Publish() noexcept {
    u64 v[W::kSnapWords] = {};
    const R::WxSchedule& d = g_asm.done;
    u64 rows[R::kWxRegions];
    if (!W::WxRowsByRegion(d, rows)) return;
    v[0] = static_cast<u64>(d.slot) | static_cast<u64>(d.gen) << 32;
    v[1] = d.mask;
    v[2] = W::PackF2(g_p0, g_anchor);
    v[3] = g_anchored ? 1u : 0u;
    v[4] = d.crc;
    for (unsigned r = 0; r < R::kWxRegions; ++r) v[W::kSnapHead + r] = rows[r];
    g_snap.Write(v);
    g_pub_gen = d.gen, g_pub_crc = d.crc, g_pub_slot = d.slot;
    g_pub_dirty = false;
}

void RequestRestore(const char* why, double now) noexcept {
    if (g_mode.load(std::memory_order_acquire) == kModeRestore) return;  // the first why stands
    if (!g_have_saved.load(std::memory_order_acquire)) {
        g_mode.store(kModeIdle, std::memory_order_release);
        return;
    }
    g_restore_why.store(why, std::memory_order_relaxed);
    g_restore_req_ms = now;
    g_mode.store(kModeRestore, std::memory_order_release);
}

void NotApplied(const R::WxApplyIn& in, const char* why, double now) noexcept {
    if (!why || why == g_last_why || !in.link) return;
    const bool real = std::strstr(why, "real save") != nullptr, cmp = std::strstr(why, "compare only") != nullptr;
    if (!in.have_snapshot && !real && !cmp) return;  // nothing from the host yet: nothing to refuse
    if (!g_r_why.Allow(now, 10000.0)) return;
    g_last_why = why;
    Log("[bt] wx: not applied: %s", why);
}

void Compare(const TickCtx& c, const Own& own, bool link) noexcept {
    if (!link) return;
    const bool have = g_asm.have_done && own.ok;
    const int k = have ? R::ScheduleDiffRegions(g_asm.done, own.sched) : -1;
    const bool hb = g_have_peer_beat && c.now_ms - g_peer_beat_ms <= R::kWxStaleMs;
    // what a new line is due on: the schedule's verdict and the rain band at each Sam
    const int key = (k + 2) * 16 + W::WxRainBand(hb, g_peer_beat.rain) * 4 + W::WxRainBand(g_here_ok, g_rain_here);
    if (!W::WxCompareDue(key != g_cmp_last_k, c.now_ms, g_cmp_last)) return;
    g_cmp_last = c.now_ms, g_cmp_last_k = key;
    char sched[64];
    if (k == 0) Put(sched, sizeof(sched), "equal");
    else if (k > 0) Put(sched, sizeof(sched), "DIFFER (%d regions)", k);
    else Put(sched, sizeof(sched), "unknown (%s)", own.ok ? "no complete host snapshot yet" : "this game's own rows unread");
    const int region_host = hb && g_peer_beat.region != R::kWxUnknown ? g_peer_beat.region : -1;
    const int eff_host = hb && g_peer_beat.eff0 != R::kWxUnknown ? g_peer_beat.eff0 : -1;
    const int eff_here = own.ok && g_here_ok ? static_cast<int>(W::WxEff0(own.rows, own.mask, g_region_here)) : -1;
    unsigned m238 = 0xFFu;
    if (const uptr mgr = I::GazerMgr()) {
        u8 b = 0;
        if (I::Read(mgr + kMgr238, b)) m238 = b;
    }
    Log("[bt] wx compare: schedule %s; rain host %.3f at host, here %.3f at Sam; region host %d here %d; eff0 host %d here %d; mgr238 here %d "
        "(gen %u, %s)",
        sched, hb ? static_cast<double>(g_peer_beat.rain) : 99.0, g_here_ok ? static_cast<double>(g_rain_here) : 99.0, region_host,
        g_here_ok ? g_region_here : -1, eff_host, eff_here == 0xFF ? -1 : eff_here, m238 == 0xFFu ? -1 : static_cast<int>(m238),
        g_asm.have_done ? g_asm.done.gen : 0u, g_verdict);
}

void GuestTick(const TickCtx& c, const Own& own, bool partner) noexcept {
    if (own.ok && own.slot != g_last_own_slot) {
        if (g_last_own_slot) g_rearm = true;  // a new slot: its first apply is instant again
        g_last_own_slot = own.slot;
    }
    R::WxApplyIn in;
    in.bt_weather = W::WxLevel(g_key, c.sync_level);
    in.role = 2;
    in.sandboxed = c.saves_redirected && !c.real_save;
    in.link = partner && c.partner.role == 1 && (c.partner.caps & R::kCapWx) != 0;
    in.loading = c.loading;
    in.have_snapshot = g_asm.have_done;
    in.slot_here = own.ok ? own.slot : 0;
    in.slot_host = g_asm.have_done ? g_asm.done.slot : 0;
    in.age_ms = c.now_ms - g_fresh_ms;
    in.mask_equal = own.ok && g_asm.have_done && own.mask == g_asm.done.mask;
    in.applied = g_have_saved.load(std::memory_order_acquire);
    in.rearm = g_rearm;
    R::WxVerdict v = W::WxGate(in);
    if (v.call == R::WxCall::apply && c.now_ms < g_hold_until) v = R::WxVerdict{R::WxCall::hold, "partner gone (held for 10 s)", false};
    if (v.call == R::WxCall::apply && !g_may_write.load(std::memory_order_relaxed))
        v = R::WxVerdict{in.applied ? R::WxCall::restore : R::WxCall::hold, "no write path here (not a sandboxed level-2 guest at the hook install)", false};
    g_verdict = v.call == R::WxCall::apply ? "applying" : v.why ? v.why : R::WxCallName(v.call);
    const int mode = g_mode.load(std::memory_order_acquire);
    switch (v.call) {
        case R::WxCall::apply:
            if (mode == kModeRestore) break;  // the pending restore first; the next apply saves the rows again
            if (g_pub_dirty || g_pub_gen != g_asm.done.gen) Publish();
            if ((v.instant || g_rearm) && !g_first_pending) {
                g_instant.store(true, std::memory_order_release);
                g_first_pending = true, g_rearm = false;
                g_first_req_applies = g_c_applies.load(std::memory_order_acquire);
            }
            g_mode.store(kModeApply, std::memory_order_release);
            g_last_why = nullptr;
            break;
        case R::WxCall::restore: RequestRestore(v.why, c.now_ms); break;
        case R::WxCall::hold:
            if (mode == kModeApply) g_mode.store(kModeIdle, std::memory_order_release);
            NotApplied(in, v.why, c.now_ms);
            break;
    }
    // a restore no detour wrote (the job did not run): from here, while the job is idle or done
    if (g_mode.load(std::memory_order_acquire) == kModeRestore && c.now_ms - g_restore_req_ms > W::kWxTickRestoreMs && !c.loading) {
        if (DirectRestore(false)) g_restore_req_ms = c.now_ms;
    }
    Compare(c, own, in.link);
}

// The detours' counters into log lines (game thread).
void Drain(double now) noexcept {
    char e[8];
    const u32 saves = g_c_saves.load(std::memory_order_acquire);
    if (saves != g_seen_saves) {
        if (!g_saved_waiting) g_saved_waiting = true, g_saved_seen_ms = now;
        // the saved eff0 at Sam's region: the line waits for the region (the first apply can come on the load's tail, before it is
        // read: gate batch 20261128-02), at most W::kWxSavedLineWaitMs, and comes before a line that ends these rows
        const bool flush = g_c_restores.load(std::memory_order_acquire) != g_seen_restores ||
                           g_c_dropped.load(std::memory_order_acquire) != g_seen_dropped || g_c_swaps.load(std::memory_order_acquire) != g_seen_swaps;
        const int region = g_sam_region.load(std::memory_order_relaxed);
        if (W::WxSavedLineDue(region, now, g_saved_seen_ms, flush)) {
            g_seen_saves = saves, g_saved_waiting = false;
            u64 e8[W::kWxEff0Words];
            for (std::size_t i = 0; i < W::kWxEff0Words; ++i) e8[i] = g_saved_eff0s[i].load(std::memory_order_relaxed);
            Eff0Text(W::WxEff0At(e8, region), e);
            Log("[bt] wx: own rows saved before the first write: slot %d, %d regions (eff0 %s at Sam's region %d)", g_saved_slot.load(), g_saved_n.load(), e,
                region);
        }
    }
    const u32 swaps = g_c_swaps.load(std::memory_order_acquire);
    if (swaps != g_seen_swaps) {
        g_seen_swaps = swaps;
        Log("[bt] wx: slot change: slot %d's own rows written back before slot %d's are saved", g_swap_from.load(), g_swap_to.load());
    }
    const u32 restores = g_c_restores.load(std::memory_order_acquire);
    if (restores != g_seen_restores) {
        g_seen_restores = restores;
        Eff0Text(g_restored_eff0.load(), e);
        const char* why = g_restore_why.load();
        Log("[bt] wx restored: %s (own rows written back: eff0 %s)", why ? why : "?", e);
        g_rearm = true, g_first_pending = false;
        g_instant.store(false, std::memory_order_release);
    }
    const u32 dropped = g_c_dropped.load(std::memory_order_acquire);
    if (dropped != g_seen_dropped) {
        g_seen_dropped = dropped;
        Log("[bt] wx: own rows dropped, nothing written back: the weather system was replaced (a load)");
        g_rearm = true, g_first_pending = false;
    }
    const u32 applies = g_c_applies.load(std::memory_order_acquire);
    if (applies != g_seen_applies) {
        g_seen_applies = applies;
        const u32 gen = g_applied_gen.load(std::memory_order_relaxed);
        if (g_first_pending && applies != g_first_req_applies) {
            g_first_pending = false;
            g_logged_gen = gen;
            Log("[bt] wx applied: slot %d gen %u crc %08X (first: %s)", g_pub_slot, gen, g_pub_crc, g_c_instants.load() ? "instant" : "the instant flag failed");
        } else if (gen != g_logged_gen) {
            g_logged_gen = gen;
            Log("[bt] wx applied: gen %u", gen);
        }
    }
    const u32 wfail = g_c_wfail.load(std::memory_order_relaxed);
    if (wfail != g_seen_wfail && g_r_wfail.Allow(now, 30000.0)) {
        Log("[bt] wx: %u guarded writes failed (%u so far)", wfail - g_seen_wfail, wfail);
        g_seen_wfail = wfail;
    }
}

void InstallHooks(const TickCtx& c) noexcept {
    g_hooks_tried = true;
    if (!Check()) return;
    char msg[160] = {};
    if (!I::MhInit(msg, sizeof(msg))) {
        Log("[bt] wx: off: MinHook: %s", msg);
        return;
    }
    // a sandboxed level-2 guest is the only role with a write path (WxGate decides each tick; this is the second lock)
    g_may_write.store(c.role == 2 && W::WxLevel(g_key, c.sync_level) >= 2 && c.saves_redirected && !c.real_save, std::memory_order_release);
    auto hook = [](uptr va, const char* sig, void* detour, FnF* orig, const char* name) -> bool {
        if (!I::SiteOk(va, sig)) {  // another hook got there since the check (the stage-0 probe's own on this function)
            Log("[bt] wx: r31 byte check FAILED: wx (%s, at the hook): wx off", name);
            return false;
        }
        void* target = reinterpret_cast<void*>(I::Addr(va));
        MH_STATUS s = MH_CreateHook(target, detour, reinterpret_cast<void**>(orig));
        if (s == MH_OK) {
            s = MH_EnableHook(target);
            if (s != MH_OK) MH_RemoveHook(target);
        }
        if (s != MH_OK) Log("[bt] wx: the hook on %s failed: %s", name, MH_StatusToString(s));
        return s == MH_OK;
    };
    if (!hook(kJobFn, kSites[0].sig, reinterpret_cast<void*>(&JobDetour), &g_job_orig, kSites[0].name)) {
        Log("[bt] wx: off: the forecast job is not hooked");
        g_may_write.store(false);
        return;
    }
    g_job_in.store(true);
    if (g_pre_ok && hook(kPreFn, kPreSite.sig, reinterpret_cast<void*>(&PreDetour), &g_pre_orig, kPreSite.name)) g_pre_in.store(true);
    Log("[bt] wx: hooks in: the forecast job FUN_142E183F0 (pass-through until a write is due), the pre-job FUN_142E17CF0 %s; %s", g_pre_in.load() ? "in" : "OUT",
        c.role == 1                ? "the host sends its rows"
        : g_may_write.load()       ? "this sandboxed guest applies the host's rows (bt_weather 2)"
        : c.real_save              ? "this guest plays on her own real save: compared only, never written"
                                   : "this guest compares only");
}

}  // namespace

void Install(const Config& config) noexcept {
    g_key = R::CfgLevel(static_cast<double>(config.bt_weather), 2);
    const int role = I::SessionRole(), sync = I::SyncLevel();
    if ((role != 1 && role != 2) || sync <= 0) return;  // solo, echo, bt_sync 0: WX is a two-game feature
    if (!g_key) return Log("[bt] wx: off (bt_weather=0)");
    Log("[bt] wx: bt_weather=%d as the %s: %s; waits for the partner's hello", g_key, RoleText(role),
        role == 1                                       ? "the forecast rows sent (WXROW / WXBEAT)"
        : W::WxLevel(g_key, sync) < 2                   ? "the host's forecast compared only"
        : I::RealSave()                                 ? "compared only: this game plays on her own real save (host_save=0), never written"
                                                        : "the host's forecast applied here (a sandboxed save)");
}

void OnHello(const PeerHello& peer) noexcept {
    if (g_key <= 0 || g_stopped.load()) return;
    g_asm.Reset();
    g_fresh_ms = -1e18;
    g_rearm = true, g_first_pending = false, g_pub_dirty = true;
    g_send_now = true;
    g_last_why = nullptr;
    g_have_peer_beat = false;
    g_cmp_last = -1e18, g_cmp_last_k = -3;
    const int role = I::SessionRole();
    const bool wx = W::WxPartner(role, peer.role, peer.caps);
    Log("[bt] wx: partner hello (caps 0x%X): %s", peer.caps,
        !wx         ? "no WX there (an older build or bt_weather=0): nothing sent"
        : role == 1 ? ((peer.caps & R::kCapWxApply) ? "WX on; the guest applies the rows" : "WX on; the guest compares only")
                    : "WX on; the host sends its rows");
}

void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept {
    if (g_key <= 0 || g_stopped.load()) return;
    const int role = I::SessionRole();
    if (type == R::kBtsWxBeat) {
        R::BtsWxBeat b;
        if (!R::DecodeWxBeat(msg, n, b)) {
            ++g_rx_bad;
            return;
        }
        ++g_rx_beats;
        g_peer_beat = b, g_have_peer_beat = true, g_peer_beat_ms = now_ms;
        // the host is alive in the same area: the snapshot it sent still holds (a newer gen comes as chunks)
        if (role == 2 && g_asm.have_done && b.state != R::kWxLoading && b.slot == g_asm.done.slot) g_fresh_ms = now_ms;
        return;
    }
    if (type != R::kBtsWxRow || role != 2) return;
    R::BtsWxRow m;
    if (!R::DecodeWxRow(msg, n, m)) {
        ++g_rx_bad;
        if (g_r_bad.Allow(now_ms, 30000.0)) Log("[bt] wx: a bad WXROW (%u so far)", g_rx_bad);
        return;
    }
    ++g_rx_rows;
    const R::WxPut p = g_asm.Take(m);
    const bool done_gen = g_asm.have_done && m.gen == g_asm.done.gen;
    if (p == R::WxPut::complete || (p == R::WxPut::duplicate && m.first == 0)) {
        // the phase anchored to this game's own clock now (a resend re-anchors it: the clocks drift)
        Own own;
        g_p0 = m.clock;
        g_anchored = ReadOwn(own);
        g_anchor = g_anchored ? own.clock : 0.0f;
        g_pub_dirty = true;
    }
    if (done_gen && (p == R::WxPut::complete || p == R::WxPut::duplicate)) g_fresh_ms = now_ms;
    if (p == R::WxPut::complete && g_r_rx.Allow(now_ms, 5000.0))
        Log("[bt] wx: the host's forecast in: slot %d gen %u crc %08X, %u regions, phase %.1f s", static_cast<int>(g_asm.done.slot), g_asm.done.gen, g_asm.done.crc,
            g_asm.done.n, static_cast<double>(m.clock));
    if (p == R::WxPut::crc_mismatch) ++g_rx_crc;
    if (p == R::WxPut::restarted) ++g_rx_restarted;
    if ((p == R::WxPut::crc_mismatch || p == R::WxPut::refused) && g_r_bad.Allow(now_ms, 30000.0))
        Log("[bt] wx: a WXROW chunk %s (gen %u; crc fails %u)", R::WxPutName(p), m.gen, g_rx_crc);
}

void Tick(const TickCtx& c) noexcept {
    if (g_key <= 0 || g_stopped.load()) return;
    if (c.role != 1 && c.role != 2) return;
    g_now = c.now_ms;
    const bool partner = c.link_up && c.peer && W::WxPartner(c.role, c.partner.role, c.partner.caps);
    if (!g_hooks_tried) {
        if (!partner || W::WxLevel(g_key, c.sync_level) <= 0) return;
        InstallHooks(c);
    }
    if (!g_job_in.load()) return;
    Own own;
    ReadOwn(own);
    Here(c);
    if (partner) Beat(c, own);
    if (c.role == 1) {
        if (partner) HostTick(c, own);
    } else {
        GuestTick(c, own, partner);
    }
    Drain(c.now_ms);
}

void OnWorldEvent(WorldEvent e, double now_ms) noexcept {
    if (g_key <= 0 || g_stopped.load()) return;
    switch (e) {
        case WorldEvent::load_start:
            RequestRestore("load start", now_ms);
            g_rearm = true, g_first_pending = false;
            break;
        case WorldEvent::load_end: g_rearm = true, g_first_pending = false; break;
        case WorldEvent::link_lost:
            RequestRestore("link lost", now_ms);
            g_asm.Reset();
            g_fresh_ms = -1e18;
            g_rearm = true, g_first_pending = false, g_send_now = true;
            break;
        case WorldEvent::partner_gone:
            RequestRestore("partner gone", now_ms);
            g_hold_until = now_ms + W::kWxGoneHoldMs;
            g_rearm = true, g_first_pending = false;
            break;
    }
}

void EmergencyStop() noexcept {
    g_pass.store(true);
    g_stopped.store(true);
    g_may_write.store(false);
    g_instant.store(false);
    // review (crash 2): a first apply already in flight on the job thread (past its g_may_write check, before it saved the rows)
    // finishes first: g_busy taken and given back, for up to 100 ms. After that a job sees g_may_write false (the store above
    // comes before this release), so g_have_saved below is final. Not taken in 100 ms: the restore path below (a restore with
    // nothing saved writes nothing).
    bool settled = false;
    for (int i = 0; i < 100 && !settled; ++i) {
        if (!g_busy.exchange(true, std::memory_order_acquire)) {
            g_busy.store(false, std::memory_order_release);
            settled = true;
        } else {
            Sleep(1);
        }
    }
    if (settled && !g_have_saved.load(std::memory_order_acquire)) {
        g_mode.store(kModeIdle);
        return;
    }
    // the guest's own rows back: by the pre-job detour (the game thread, the job idle) right before the next job, or here and now
    if (g_mode.load() != kModeRestore) g_restore_why.store("EmergencyStop");
    g_mode.store(kModeRestore, std::memory_order_release);
    if (g_pre_in.load()) {
        Log("[bt] wx: EmergencyStop: the detours pass through; the guest's own rows are written back by the pre-job before the next job");
        return;
    }
    const bool ok = DirectRestore(true);
    char e[8];
    Eff0Text(g_restored_eff0.load(), e);
    if (ok && g_c_restores.load() != g_seen_restores) Log("[bt] wx restored: EmergencyStop (own rows written back: eff0 %s)", e);
    else if (!ok) Log("[bt] wx: EmergencyStop: the own rows NOT written back (the job kept running for 100 ms); they stay the host's until a load");
}

std::uint16_t Caps() noexcept {
    if (g_key <= 0 || g_stopped.load()) return 0;
    const int role = I::SessionRole(), sync = I::SyncLevel();
    if ((role != 1 && role != 2) || sync <= 0) return 0;
    return W::WxCaps(role, g_key, sync, I::RealSave(), Check());
}

void Status(char* out, std::size_t cap) noexcept {
    if (!out || !cap) return;
    const int role = I::SessionRole();
    if (g_key <= 0) return Put(out, cap, "wx: off (bt_weather=0)");
    if (!g_job_in.load())
        return Put(out, cap, "wx: bt_weather=%d, not hooked (%s)", g_key,
                   g_checks == 0 ? "byte checks failed" : g_hooks_tried ? "the hook failed" : "no WX partner yet");
    if (role == 1)
        return Put(out, cap, "wx: host bt_weather=%d gen %u, sent %u rows %u beats, got %u beats%s", g_key, g_gen, g_tx_rows, g_tx_beats, g_rx_beats,
                   g_pre_in.load() ? "" : " (no pre-job hook)");
    Put(out, cap, "wx: guest bt_weather=%d %s; got %u rows %u beats (crc %u bad %u); gen %u; applies %u writes %u skips %u; saved %s; restores %u",
        g_key, g_verdict, g_rx_rows, g_rx_beats, g_rx_crc, g_rx_bad, g_asm.have_done ? g_asm.done.gen : 0u, g_c_applies.load(), g_c_writes.load(),
        g_c_skips.load(), g_have_saved.load() ? "yes" : "no", g_c_restores.load());
}

}  // namespace st::bt::wx
