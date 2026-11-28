// Round 31 stage 7: CS: the partner's catcher state and edges (RW: crystals DROPPED) (bt_catcher.hpp).
//
// Sender (game thread, 4 Hz while the partner's hello offers kCapCatch): the catcher manager's records (cm = [0x147BEE060], via
// bt_sync's CatcherMgr: its +0x88 / +0x90 / 0x110 / +0xF0 / +0xF4 / +0 checks) read with ReadProcessMemory, diffed by
// btrules::catcher::CatchSender, sent as CATCH (14). A pass-through detour on the notify FUN_142A1C0E0(cm, id, hash, 0) (any
// thread: atomics and a lock-free ring only) adds the hash for the log and the DEFEAT / ANNIHILATE / GONE edges; it goes in on the
// first positioned record (so bt_probe 33's own hook on the same function, made before its bring, keeps working: then this module
// runs on the record diff alone). Nothing is written to the game.
//
// Receiver (game thread): CatchView; the "[bt] partner catcher:" lines; the marker at the bar anchor (bt_markers); the HUD line
// with the markers off; the rain spot through bt_hunt's SpotWant (RS: bt_rain and RainSpotGate decide; the only game write, and
// it is bt_hunt's). REWARD (15) is refused: "crystals not given: not built this round".
#include "bt_catcher.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "automation.hpp"
#include "bt_catcher_rules.hpp"
#include "bt_hunt.hpp"
#include "bt_markers.hpp"
#include "bt_rules.hpp"
#include "bt_sync_internal.hpp"
#include "config.hpp"
#include "log.hpp"

namespace st::bt::catcher {
namespace {

namespace R = btrules;
namespace C = btrules::catcher;
namespace I = st::bt::internal;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using uptr = std::uintptr_t;

void Put(char* msg, std::size_t cap, const char* fmt, ...) {
    if (!msg || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------- the byte checks (once; any thread; before the hook goes in)
// The record layout this module reads beyond bt_sync's own catcher checks, and the notify it hooks (verified in ds.exe DC; the
// entry and the class read are bt_probe_r31's kGCat sites).
constexpr uptr kNotifyFn = 0x142A1C0E0;
struct Site {
    uptr va;
    const char* sig;
    const char* name;
    bool notify;  // false: the records group (the feature), true: the notify group (the hook only)
};
// clang-format off
constexpr Site kSites[] = {
    {0x142A1C11F, "4D 8B 8F 90 00 00 00 48 8B D0 41 8B CA 49 8D 41 20 44 39 30 74 16 41 FF C0 48 FF C1 48 05 10 01 00 00",
     "the record key +0x20 (the notify's search 0x142A1C11F)", false},
    {0x142A1C18C, "44 0F B6 66 28", "the record class +0x28 (0x142A1C18C)", false},
    {0x142A87950, "C4 C1 78 10 00 C5 F8 11 41 68 C4 C1 7B 10 48 10 C5 FB 11 49 78", "the record position +0x68 (0x142A87950)", false},
    {0x142A7CDFF, "C5 F8 10 81 B0 00 00 00 C5 FB 10 89 C0 00 00 00", "the record bar +0xB0 (0x142A7CDFF)", false},
    {0x142A7CE1F, "C5 FA 11 91 F4 00 00 00 C5 F8 10 03 C5 F8 11 81 B0 00 00 00", "the record hp +0xF4 / bar store (0x142A7CE1F)", false},
    {0x142A7CE38, "C5 FA 10 44 24 28 C5 FA 11 81 00 01 00 00", "the record sub-life +0x100 (0x142A7CE38)", false},
    {kNotifyFn, "40 55 53 41 56 41 57 48 8D 6C 24 88 48 81 EC 78 01 00 00 48 8B 05 36 4F 54 02 48 33 C4 48 89 45 48 48 63 81 88 00 00 00",
     "the catcher notify FUN_142A1C0E0", true},
    {0x142A1C16F, "8B 56 24 48 8B 0D", "the notify's record id (0x142A1C16F)", true},
    {0x142A1C1F4, "81 FB 93 73 C5 2F", "the notify's GONE hash 0x2FC57393 (0x142A1C1F4)", true},
    {0x142A1C35C, "81 FB D5 9E DB 63", "the notify's SPAWN hash 0x63DB9ED5 (0x142A1C35C)", true},
};
// clang-format on
std::atomic<int> g_checked{0};  // 0 not yet, 1 running, 2 done
bool g_recs_ok = false, g_notify_ok = false;
void Check() noexcept {
    int want = 0;
    if (g_checked.load() == 2 || !g_checked.compare_exchange_strong(want, 1)) return;
    bool recs = true, notify = true;
    int ok = 0, total = 0;
    for (const Site& s : kSites) {
        ++total;
        if (I::SiteOk(s.va, s.sig)) {
            ++ok;
            continue;
        }
        (s.notify ? notify : recs) = false;
        Log("[bt] catcher: byte check FAILED: %s: %s", s.name, s.notify ? "the notify hook off (the record diff alone)" : "the catcher sync off");
    }
    g_recs_ok = recs, g_notify_ok = recs && notify;
    Log("[bt] catcher: byte checks %d/%d ok; records %s, notify %s", ok, total, recs ? "ok" : "OFF", notify ? "ok" : "OFF");
    g_checked.store(2);
}

// ---------------------------------------------------------------- the notify detour (any thread: atomics and the ring only)
using NotifyFn = u64 (*)(u64, u64, u64, u64);
NotifyFn g_orig = nullptr;
std::atomic<bool> g_pass{false};   // EmergencyStop: straight through
std::atomic<bool> g_armed{false};  // the sender runs: record the calls
std::atomic<unsigned long> g_tick_tid{0};
std::atomic<unsigned> g_calls{0}, g_lost{0};

struct Ev {
    u32 id = 0, hash = 0;
    u8 tick = 0;
};
constexpr unsigned kRing = 64;  // bt_probe_r31's Vyukov bounded MPMC ring; the Tick is the only consumer
struct Cell {
    std::atomic<unsigned> seq;
    Ev e;
};
Cell g_ring[kRing];
std::atomic<unsigned> g_enq{0};
unsigned g_deq = 0;
bool g_ring_init = false;
void RingInit() {
    if (g_ring_init) return;
    for (unsigned i = 0; i < kRing; ++i) g_ring[i].seq.store(i, std::memory_order_relaxed);
    g_ring_init = true;
}
void Push(const Ev& e) noexcept {
    unsigned pos = g_enq.load(std::memory_order_relaxed);
    for (;;) {
        Cell& c = g_ring[pos % kRing];
        const unsigned seq = c.seq.load(std::memory_order_acquire);
        const int dif = static_cast<int>(seq - pos);
        if (dif == 0) {
            if (g_enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                c.e = e;
                c.seq.store(pos + 1, std::memory_order_release);
                return;
            }
        } else if (dif < 0) {
            g_lost.fetch_add(1, std::memory_order_relaxed);
            return;
        } else {
            pos = g_enq.load(std::memory_order_relaxed);
        }
    }
}
bool Pop(Ev* out) noexcept {
    if (!g_ring_init) return false;
    Cell& c = g_ring[g_deq % kRing];
    const unsigned seq = c.seq.load(std::memory_order_acquire);
    if (static_cast<int>(seq - (g_deq + 1)) != 0) return false;
    *out = c.e;
    c.seq.store(g_deq + kRing, std::memory_order_release);
    ++g_deq;
    return true;
}
u64 NotifyDetour(u64 cm, u64 id, u64 hash, u64 d) {
    if (!g_pass.load(std::memory_order_relaxed) && g_armed.load(std::memory_order_relaxed)) {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        Ev e;
        e.id = static_cast<u32>(id), e.hash = static_cast<u32>(hash);
        e.tick = GetCurrentThreadId() == g_tick_tid.load(std::memory_order_relaxed) ? 1 : 0;
        Push(e);
    }
    return g_orig(cm, id, hash, d);
}

// ---------------------------------------------------------------- state (game thread unless marked)
int g_key = 0;          // bt_catcher
int g_crystals = 0;     // bt_crystals (RW is DROPPED: only the refusal line names it)
bool g_hook_tried = false, g_hook_in = false;
char g_hook_why[96] = "not yet (it goes in on the first catcher record)";
std::atomic<bool> g_stopped{false};

C::CatchSender g_tx;
double g_tx_next_ms = 0;
bool g_tx_on = false;  // the sender runs (the last Tick's gates)
unsigned g_sent = 0, g_sent_spawn = 0, g_sent_end = 0, g_send_fail = 0;
std::uint8_t g_rec_buf[C::kRecMax * C::kRecSize];

C::CatchView g_view;
unsigned g_rx = 0, g_rx_bad = 0, g_rx_off = 0, g_reward_refused = 0;
bool g_me_ok = false;
double g_me[3] = {0, 0, 0};
bool g_loading = false;
int g_level = 0;  // bt_sync here (the last Tick)
bool g_spot_on = false;
int g_spot_state = -1;  // the last SpotWant answer: -1 none yet, 0 refused, 1 taken (logged on a change)
double g_spot_next_ms = 0;
unsigned g_spot_refused = 0;

struct Test {
    bool on = false;
    double t0 = 0, next_ms = 0;
    int step = -1;
    std::uint8_t edge = 0;  // the edge still to go (until a send takes it)
    u32 key = 0;
    float pos[3] = {0, 0, 0};
} g_test;
unsigned g_tests = 0;

// Rate limits: at most `burst` lines per `gap_ms` window per kind; the skipped ones are counted into the next line.
struct Limit {
    double win_ms = -1e18;
    unsigned used = 0, skipped = 0;
    bool Ok(double now_ms, double gap_ms, unsigned burst) {
        if (now_ms - win_ms >= gap_ms) win_ms = now_ms, used = 0;
        if (used >= burst) {
            ++skipped;
            return false;
        }
        ++used;
        return true;
    }
    unsigned TakeSkipped() {
        const unsigned s = skipped;
        skipped = 0;
        return s;
    }
};
Limit g_l_edge, g_l_notify, g_l_send, g_l_rx, g_l_bad, g_l_spot, g_l_reward, g_l_hud;

const char* Look(u8 class1) { return R::CatcherLook(class1); }
const char* Name(u8 class1) { return R::CatcherTypeName(class1); }
double MeDist(const float* p) {
    if (!g_me_ok) return -1.0;
    const double q[3] = {p[0], p[1], p[2]};
    return R::Dist3(q, g_me);
}

// ---------------------------------------------------------------- the hook (game thread, once)
void HookIn() {
    if (g_hook_tried || g_stopped.load()) return;
    g_hook_tried = true;
    if (!g_notify_ok) {
        std::snprintf(g_hook_why, sizeof(g_hook_why), "off: its byte checks failed (or bt_probe's hook was in first)");
        Log("[bt] catcher: notify hook not in: %s; the record diff alone", g_hook_why);
        return;
    }
    char msg[128] = "";
    if (!I::MhInit(msg, sizeof(msg))) {
        std::snprintf(g_hook_why, sizeof(g_hook_why), "off: %s", msg);
        Log("[bt] catcher: notify hook not in: %s; the record diff alone", msg);
        return;
    }
    RingInit();
    void* target = reinterpret_cast<void*>(I::Addr(kNotifyFn));
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&NotifyDetour), reinterpret_cast<void**>(&g_orig));
    if (s == MH_OK) s = MH_EnableHook(target);
    g_hook_in = s == MH_OK;
    if (g_hook_in) {
        std::snprintf(g_hook_why, sizeof(g_hook_why), "in");
        Log("[bt] catcher: notify hook FUN_142A1C0E0 in (pass-through; the hash for the log, the DEFEAT / ANNIHILATE / GONE edges)");
    } else {
        std::snprintf(g_hook_why, sizeof(g_hook_why), "off: %s%s", MH_StatusToString(s),
                      s == MH_ERROR_ALREADY_CREATED ? " (bt_probe holds it)" : "");
        Log("[bt] catcher: notify hook not in: %s; the record diff alone", g_hook_why);
    }
}

// ---------------------------------------------------------------- the sender
// The records now (game thread). -1: no manager (unreadable, not in a world, or bt_sync's catcher checks failed).
int ReadRecs(C::CatchRec* out, int cap) {
    const uptr cm = I::CatcherMgr();
    if (!cm) return -1;
    std::int32_t n = 0;
    uptr data = 0;
    if (!I::Read(cm + 0x88, n) || n < 0 || n > C::kRecMax) return -1;
    if (n == 0) return 0;
    if (!I::Read(cm + 0x90, data) || !data) return -1;
    if (!I::ReadN(data, g_rec_buf, static_cast<std::size_t>(n) * C::kRecSize)) return -1;
    int k = 0;
    for (int i = 0; i < n && k < cap; ++i) out[k++] = C::ParseRec(g_rec_buf + static_cast<std::size_t>(i) * C::kRecSize);
    return k;
}

void LogEdge(const C::EdgeNote& e, double now_ms, const char* tag) {
    if (!g_l_edge.Ok(now_ms, 10000.0, 16)) return;
    const unsigned sk = g_l_edge.TakeSkipped();
    char tail[48] = "";
    if (e.edge != R::kCeSpawn) std::snprintf(tail, sizeof(tail), ", %.1f s after its spawn", e.age_ms / 1000.0);
    Log("[bt] catcher: %s key %08X %s (%s) at (%.1f %.1f %.1f), hp %.0f%% (%s%s)%s", R::CatchEdgeName(e.edge), e.key, Name(e.class1), Look(e.class1),
        e.pos[0], e.pos[1], e.pos[2], static_cast<double>(e.hp), tag, tail, sk ? " (edge lines skipped before this)" : "");
}

void DrainNotify(double now_ms, bool feed) {
    Ev e;
    while (Pop(&e)) {
        C::EdgeNote note;
        const std::uint8_t edge = feed ? g_tx.Notify(e.id, e.hash, now_ms, &note) : 0;
        if (g_l_notify.Ok(now_ms, 10000.0, 8)) {
            const unsigned sk = g_l_notify.TakeSkipped();
            Log("[bt] catcher: notify id %08X hash 0x%08X (%s, thread %s)%s%s", e.id, e.hash, C::NotifyName(e.hash), e.tick ? "tick" : "other",
                edge ? "" : feed ? ": no new edge" : ": not sending", sk ? " (notify lines skipped before this)" : "");
        }
        if (edge && C::EndEdge(edge)) LogEdge(note, now_ms, "the notify");
    }
}

void SendTick(const TickCtx& c) {
    if (c.now_ms < g_tx_next_ms) return;
    g_tx_next_ms = c.now_ms + C::kSendGapMs;
    static C::CatchRec recs[C::kRecMax];  // game thread only
    const int nr = ReadRecs(recs, C::kRecMax);
    if (nr < 0) return;  // no manager now: no diff (a GONE needs the records read)
    bool positioned = false;
    for (int i = 0; i < nr; ++i) positioned |= !recs[i].inactive && recs[i].pos_ok;
    if (positioned && !g_hook_tried) HookIn();
    C::EdgeNote notes[C::kSendTrackMax * 2];
    const unsigned ne = g_tx.Step(recs, static_cast<unsigned>(nr), c.now_ms, notes, sizeof(notes) / sizeof(notes[0]));
    for (unsigned i = 0; i < ne; ++i) LogEdge(notes[i], c.now_ms, notes[i].notify ? "the notify, then its record" : "the record diff");
    R::BtsCatch m;
    bool edges = false;
    if (!g_tx.Build(m, static_cast<u32>(c.now_ms), &edges)) return;
    std::uint8_t buf[R::kBtsCatchBase + R::kBtsCatchMax * R::kBtsCatchEntry];
    const std::size_t n = c.seq && c.send ? R::EncodeCatch(c.seq(), m, buf, sizeof(buf)) : 0;
    if (!n || !c.send(buf, n)) {
        ++g_send_fail;
        if (g_l_send.Ok(c.now_ms, 30000.0, 2)) Log("[bt] catcher: CATCH not sent (%s); %u failed so far", n ? "the link did not take it" : "encode refused", g_send_fail);
        return;
    }
    for (unsigned i = 0; i < m.n; ++i) {
        if (m.e[i].edge == R::kCeSpawn) ++g_sent_spawn;
        else if (C::EndEdge(m.e[i].edge)) ++g_sent_end;
    }
    g_tx.Sent();
    ++g_sent;
    if (edges || g_sent == 1) {
        if (g_l_send.Ok(c.now_ms, 10000.0, 6)) {
            char what[96] = "";
            std::size_t at = 0;
            for (unsigned i = 0; i < m.n && at + 24 < sizeof(what); ++i)
                at += static_cast<std::size_t>(std::snprintf(what + at, sizeof(what) - at, "%s%08X %s%s%.0f%%", i ? ", " : "", m.e[i].key,
                                                             m.e[i].edge ? R::CatchEdgeName(m.e[i].edge) : "", m.e[i].edge ? " " : "",
                                                             static_cast<double>(m.e[i].hp)));
            Log("[bt] catcher: CATCH sent (%u entries: %s); %u sent", m.n, what, g_sent);
        }
    }
}

// ---------------------------------------------------------------- the test catcher (bt_sync 10)
void TestTick(const TickCtx& c) {
    if (!g_test.on || c.now_ms < g_test.next_ms) return;
    g_test.next_ms = c.now_ms + C::kSendGapMs;
    const int k = C::CatchTestAt(c.now_ms - g_test.t0);
    if (k < 0) return;
    if (k != g_test.step) {
        g_test.step = k;
        if (C::kCatchTest[k].edge) g_test.edge = C::kCatchTest[k].edge;
    }
    R::BtsCatch m;
    m.sender_ms = static_cast<u32>(c.now_ms), m.n = 1;
    R::CatchEntry& e = m.e[0];
    e.key = g_test.key, e.class1 = C::kCatchTestClass1, e.edge = g_test.edge;
    e.flags = static_cast<u8>(R::kCfTest | (C::EndEdge(g_test.edge) ? 0 : R::kCfCombat));
    std::memcpy(e.pos, g_test.pos, sizeof(e.pos));
    e.bar_z = g_test.pos[2] + C::kCatchTestBarM;
    e.hp = C::kCatchTest[k].hp, e.sub = -1;
    std::uint8_t buf[R::kBtsCatchBase + R::kBtsCatchEntry];
    const std::size_t n = c.seq && c.send ? R::EncodeCatch(c.seq(), m, buf, sizeof(buf)) : 0;
    const bool ok = n && c.send(buf, n);
    if (ok && g_test.edge) Log("[bt] catcher: test catcher %08X: %s sent, hp %.0f%%", g_test.key, R::CatchEdgeName(g_test.edge), static_cast<double>(e.hp));
    if (ok && C::EndEdge(g_test.edge)) {
        g_test.on = false;
        Log("[bt] catcher: test catcher %08X done", g_test.key);
        return;
    }
    if (ok) g_test.edge = 0;
    if (!ok && c.now_ms - g_test.t0 > 15000.0) {
        g_test.on = false;
        Log("[bt] catcher: test catcher %08X ended: the link took nothing for 15 s", g_test.key);
    }
}

// ---------------------------------------------------------------- the receiver
void Mark(double now_ms) {
    if (g_loading || g_stopped.load() || !btmarks::On()) return;
    const C::ViewEntry* p = g_view.Primary();
    if (!p) return;
    double a[3];
    C::MarkAnchor(*p, a);
    btmarks::Catcher(a, p->hp, now_ms);
}

void Hud(const C::ViewEntry& e, double now_ms) {
    if (btmarks::On()) return;  // the marker shows it (round 30 ux 3: no text with the markers on)
    const int k = g_view.Find(e.key);
    C::ViewEntry* v = k >= 0 ? &g_view.v[k] : nullptr;
    if (!v || !C::ToastAgain(v->toast_hp, e.hp)) return;
    v->toast_hp = e.hp < 0 ? 0 : e.hp;
    char t[96];
    C::HudText(e.class1, e.hp, t, sizeof(t));
    I::Toast(now_ms, t);
    if (g_l_hud.Ok(now_ms, 10000.0, 3)) Log("[bt] catcher: HUD: %s (the markers are off; one BT toast per 8 s)", t);
}

void Forget(const char* why, double now_ms) {
    if (g_view.n) {
        Log("[bt] partner catcher: %u taken back (%s): markers and the rain spot gone", g_view.n, why);
        g_view.Clear();
        btmarks::ClearTrouble();
    }
    if (g_spot_on) {
        hunt::SpotDrop(hunt::SpotUser::catcher);
        g_spot_on = false;
        Log("[bt] catcher: rain spot dropped (%s): it dies within its life", why);
    }
    g_spot_state = -1;
    g_spot_next_ms = now_ms;
}

void OnCatch(const std::uint8_t* b, std::size_t n, double now_ms) {
    R::BtsCatch m;
    if (!R::DecodeCatch(b, n, m)) {
        ++g_rx_bad;
        if (g_l_bad.Ok(now_ms, 30000.0, 2)) Log("[bt] catcher: a bad CATCH (%zu B) dropped; %u so far", n, g_rx_bad);
        return;
    }
    ++g_rx;
    if (g_key < 1) {
        if (g_rx_off++ == 0) Log("[bt] catcher: CATCH ignored: bt_catcher=0 here");
        return;
    }
    for (unsigned i = 0; i < m.n; ++i) {
        const R::CatchEntry& c = m.e[i];
        const C::ViewOut o = g_view.Take(c, now_ms);
        const bool test = (c.flags & R::kCfTest) != 0;
        const C::ViewEntry& e = o.e;
        char hp[24] = "";
        if (e.hp >= 0) std::snprintf(hp, sizeof(hp), ", hp %.0f%%", static_cast<double>(e.hp));
        if (o.ev == C::kVeNew) {
            Log("[bt] partner catcher: %s (%s) %s at (%.1f %.1f %.1f), you %.0f m%s (key %08X%s); %s", Name(e.class1), Look(e.class1),
                c.edge == R::kCeSpawn ? "SPAWN" : "up (no SPAWN seen here)", e.pos[0], e.pos[1], e.pos[2], MeDist(e.pos), hp, e.key, test ? ", test" : "",
                btmarks::On() ? "marker at its bar" : "no marker (bt_markers=0)");
            Hud(e, now_ms);
        } else if (o.ev == C::kVeHp) {
            if (g_l_rx.Ok(now_ms, 2000.0, 4))
                Log("[bt] partner catcher: %s (%s) hp %.0f%%%s at (%.1f %.1f %.1f), you %.0f m (key %08X%s)", Name(e.class1), Look(e.class1),
                    static_cast<double>(e.hp), (c.flags & R::kCfCombat) ? "" : " (not fighting)", e.pos[0], e.pos[1], e.pos[2], MeDist(e.pos), e.key,
                    test ? ", test" : "");
            Hud(e, now_ms);
        } else if (o.ev == C::kVeEnd) {
            char age[40] = "";
            if (o.known) std::snprintf(age, sizeof(age), ", %.1f s after its first CATCH", o.age_ms / 1000.0);
            Log("[bt] partner catcher: %s (%s) %s (key %08X%s%s%s)%s", Name(e.class1), Look(e.class1), R::CatchEdgeName(c.edge), e.key, test ? ", test" : "",
                age, o.known ? "" : ", not seen here before", (c.edge != R::kCeGone) ? " [an unproven edge: no defeat was ever seen in a run]" : "");
        }
    }
    Mark(now_ms);
}

void OnReward(const std::uint8_t* b, std::size_t n, double now_ms) {
    R::BtsReward m;
    const bool ok = R::DecodeReward(b, n, m);
    ++g_reward_refused;
    if (g_l_reward.Ok(now_ms, 30000.0, 2))
        Log("[bt] catcher: crystals not given: not built this round (REWARD %s%u crystals, %s; bt_crystals=%d; %u refused)", ok ? "" : "undecodable, ",
            ok ? m.count : 0u, ok ? Look(m.class1) : "?", g_crystals, g_reward_refused);
}

void SpotTick(const TickCtx& c) {
    const C::ViewEntry* p = g_view.Primary();
    const bool want = p && c.sync_level >= 2 && !c.loading && !c.playing_why && !g_stopped.load();
    if (!want) {
        if (g_spot_on) {
            hunt::SpotDrop(hunt::SpotUser::catcher);
            g_spot_on = false;
            g_spot_state = -1;
            if (g_l_spot.Ok(c.now_ms, 10000.0, 3))
                Log("[bt] catcher: rain spot dropped (%s)", !p ? "no partner catcher" : c.playing_why ? c.playing_why : "held");
        }
        return;
    }
    if (c.now_ms < g_spot_next_ms) return;
    const double pos[3] = {p->pos[0], p->pos[1], p->pos[2]};
    const float r = R::CatcherSpotRadius(-1.0f);  // CATCH carries no radius: ActiveAreaDistance's 100 m default + 30 m
    const bool ok = hunt::SpotWant(hunt::SpotUser::catcher, pos, r, c.now_ms);
    if ((ok ? 1 : 0) != g_spot_state && g_l_spot.Ok(c.now_ms, 10000.0, 3)) {
        g_spot_state = ok ? 1 : 0;
        if (ok) Log("[bt] catcher: rain spot wanted: %s (%s) at (%.1f %.1f %.1f) r %.0f m, you %.0f m (bt_hunt's slot)", Name(p->class1), Look(p->class1), pos[0], pos[1],
                    pos[2], static_cast<double>(r), MeDist(p->pos));
        else Log("[bt] catcher: rain spot refused by bt_hunt (bt_rain=0, a real save, its gate or no slot: its status); asked again in 10 s");
    }
    if (!ok) ++g_spot_refused;
    g_spot_on = ok;
    g_spot_next_ms = c.now_ms + (ok ? 1000.0 : 10000.0);
}

}  // namespace

void Install(const Config& config) noexcept {
    g_key = R::CfgLevel(config.bt_catcher, 1);
    g_crystals = config.bt_crystals;
    Log("[bt] catcher: %s (bt_catcher=%d); crystals (RW) not built this round (bt_crystals=%d: REWARD refused)",
        g_key ? "CATCH on: the partner's catchers as markers, logs and the HUD line" : "off", g_key, g_crystals);
}

void OnHello(const PeerHello& peer) noexcept {
    if (g_key < 1) return;
    const double now = I::NowMs();
    Forget("a new partner hello", now);
    Log("[bt] catcher: the partner %s CATCH (caps 0x%04X)%s", (peer.caps & R::kCapCatch) ? "sends and takes" : "takes no", peer.caps,
        (peer.caps & R::kCapReward) ? "; it offers REWARD, refused here (not built this round)" : "");
}

void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept {
    if (g_stopped.load()) return;
    if (type == R::kBtsCatch) OnCatch(msg, n, now_ms);
    else if (type == R::kBtsReward) OnReward(msg, n, now_ms);
}

void Tick(const TickCtx& c) noexcept {
    if (g_stopped.load() || g_key < 1) return;
    g_tick_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_me_ok = c.me_ok;
    if (c.me_ok) std::memcpy(g_me, c.me, sizeof(g_me));
    g_loading = c.loading;
    g_level = c.sync_level;
    Check();
    // the sender: a two-game role, bt_sync >= 1, the partner's hello offering CATCH, a world
    const bool tx = g_recs_ok && (c.role == 1 || c.role == 2) && !c.echo && c.sync_level >= 1 && c.peer && (c.partner.caps & R::kCapCatch) && !c.loading;
    if (tx != g_tx_on) {
        g_tx_on = tx;
        g_armed.store(tx, std::memory_order_relaxed);
        if (!tx && g_tx.n) g_tx.DropEnded();
    }
    DrainNotify(c.now_ms, tx);
    if (tx) SendTick(c);
    TestTick(c);
    // the receiver
    C::ViewEntry gone[C::kViewMax];
    const unsigned ng = g_view.Expire(c.now_ms, gone, C::kViewMax);
    for (unsigned i = 0; i < ng && i < C::kViewMax; ++i)
        Log("[bt] partner catcher: %s (%s) expired (no CATCH for 3 s, key %08X): its marker goes by itself", Name(gone[i].class1), Look(gone[i].class1), gone[i].key);
    SpotTick(c);
}

void OnWorldEvent(WorldEvent e, double now_ms) noexcept {
    if (g_key < 1 || g_stopped.load()) return;
    switch (e) {
        case WorldEvent::load_start:
            if (g_tx.n) Log("[bt] catcher: load: %u tracked catchers dropped (no edge sent: the partner's view expires in 3 s)", g_tx.n);
            g_tx.Reset();
            g_tx_next_ms = 0;
            if (g_test.on) Log("[bt] catcher: test catcher %08X ended (a load)", g_test.key);
            g_test.on = false;
            Forget("a load", now_ms);
            break;
        case WorldEvent::load_end: break;
        case WorldEvent::link_lost:
        case WorldEvent::partner_gone:
            g_tx.DropEnded();
            if (g_test.on) Log("[bt] catcher: test catcher %08X ended (%s)", g_test.key, WorldEventName(e));
            g_test.on = false;
            Forget(WorldEventName(e), now_ms);
            break;
    }
}

void EmergencyStop() noexcept {
    // any thread, synchronous: the detour straight through, nothing more sent or put. Nothing was written to the game; the rain
    // spot (bt_hunt's) dies within its life by itself, and the catcher marker expires 3 s after its last refresh (bt_markers'
    // book is the game thread's: not touched from here).
    g_pass.store(true);
    g_armed.store(false);
    g_stopped.store(true);
}

std::uint16_t Caps() noexcept {
    if (g_key < 1 || g_stopped.load()) return 0;
    Check();
    return g_recs_ok ? static_cast<std::uint16_t>(R::kCapCatch) : 0;  // never kCapReward: RW is DROPPED this round
}

void Status(char* out, std::size_t cap) noexcept {
    if (!out || !cap) return;
    if (g_key < 1) return Put(out, cap, "catcher: off (bt_catcher=0)");
    if (g_stopped.load()) return Put(out, cap, "catcher: stopped (emergency stop)");
    if (g_checked.load() == 2 && !g_recs_ok) return Put(out, cap, "catcher: off (its byte checks failed: log)");
    Put(out, cap, "catcher: %s, hook %s; sent %u (spawn %u, end %u, failed %u), tracked %u, notify %u lost %u; got %u, partner catchers %u, spot %s; reward refused %u",
        g_tx_on ? "sending" : "idle", g_hook_in ? "in" : "off", g_sent, g_sent_spawn, g_sent_end, g_send_fail, g_tx.n, g_calls.load(), g_lost.load(), g_rx, g_view.n,
        g_spot_on ? "on" : "off", g_reward_refused);
}

bool Command(int, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (g_key < 1) return Put(msg, cap, "bt sync 10: bt_catcher=0 here"), false;
    if (g_stopped.load()) return Put(msg, cap, "bt sync 10: stopped (emergency stop)"), false;
    if (!automation::Enabled()) return Put(msg, cap, "bt sync 10: the test catcher is for hidden runs only"), false;
    if (I::SyncLevel() < 1) return Put(msg, cap, "bt sync 10: bt_sync=0 here"), false;
    if (!I::PeerOn()) return Put(msg, cap, "bt sync 10: no partner hello yet"), false;
    if (!(I::Peer().caps & R::kCapCatch)) return Put(msg, cap, "bt sync 10: the partner's build takes no CATCH"), false;
    if (g_test.on) return Put(msg, cap, "bt sync 10: a test catcher runs already (%08X)", g_test.key), false;
    double me[3];
    double fx = 0, fy = 0;
    const double ahead = C::CatchTestAhead(v[1]);
    if (I::SamFacing(me, &fx, &fy) && std::isfinite(fx) && std::isfinite(fy) && (fx * fx + fy * fy) > 1e-6) {
        const double l = std::sqrt(fx * fx + fy * fy);
        me[0] += fx / l * ahead, me[1] += fy / l * ahead;
    } else if (!I::LocalPos(me)) {
        return Put(msg, cap, "bt sync 10: no local player (not in a world)"), false;
    }
    g_test = Test{};
    g_test.on = true;
    g_test.t0 = I::NowMs();
    g_test.key = 0xC47E0000u | (++g_tests & 0xFFFFu);
    for (int i = 0; i < 3; ++i) g_test.pos[i] = static_cast<float>(me[i]);
    Log("[bt] catcher: test catcher %08X: Quad (lion) at (%.1f %.1f %.1f), %.0f m ahead: SPAWN 100%%, 60%% at 1.5 s, 20%% at 3 s, GONE at 4.5 s", g_test.key,
        me[0], me[1], me[2], ahead);
    return Put(msg, cap, "bt sync 10: test catcher %08X (SPAWN Quad 100 / 60 / 20 %%, then GONE over 4.5 s; log)", g_test.key), true;
}

bool OwnsCatcherMark() noexcept { return g_key >= 1 && !g_stopped.load() && g_view.n > 0; }

std::uint8_t LocalClass1() noexcept {
    for (unsigned i = 0; i < g_tx.n; ++i)
        if (!g_tx.t[i].ended && g_tx.t[i].have_pos && g_tx.t[i].class1) return g_tx.t[i].class1;
    return 0;
}

}  // namespace st::bt::catcher
