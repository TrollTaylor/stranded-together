// Round 31 stage 3: HUNT, PULLED's test path, RS (rain spots) and the markers glue (bt_hunt.hpp; the round 31 build spec, Stage 3).
// The game calls and reads are bt_probe_r31.cpp's (ops 19, 24, 25), copied with their byte checks and SEH wrappers:
//   the hunter manager hm = [0x147BECBD0] (bt_sync's kHunter group): +0x140 the hunter area (up), +0x50 / +0x58 the crowd list
//   (comp = entry - 0x230, local when comp+0x48 == area - 0x20), comp+0x260 f64 centre[3], comp+0x278 f32 radius, comp+0x2B0
//   members, hm+0x244 gold, hm+0x00 hands, +0x04 strength, +0x08 / +0x0C dir, +0x10 level, +0x14 grab (the last five read and
//   logged; PULLED is sent only from bt_sync 8);
//   the rain spot FUN_142E14F60(ws, const double pos[3], f32 radius, f32 life, owner, u8 0) (kind 2; it takes the weather lock
//   +0x69A8 itself in FUN_142E15DF0 and only compares the owner, which is one of 8 static keys here: probe 19's guard page was
//   never touched), the spot list ws+0x90 count / +0x98 entries of 0x30 (+0x18 life, +0x20 owner), the raw rain at a point
//   FUN_142E0C3F0(ws, const double pos[3]) (leg D's evidence only).
#include "bt_hunt.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "automation.hpp"
#include "bt_hunt_rules.hpp"
#include "bt_markers.hpp"
#include "bt_rules.hpp"
#include "bt_sync_internal.hpp"
#include "config.hpp"
#include "log.hpp"

namespace st::bt::hunt {
namespace {

namespace R = btrules;
namespace H = btrules::hunt;
namespace I = st::bt::internal;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using uptr = std::uintptr_t;

// ================================================================ addresses (ds.exe DC; bt_probe_r31.cpp's, verified there)
constexpr uptr kHunterTick = 0x1429E1370, kAreaReader = 0x1427AD2F0, kHandsAgg = 0x1429E15A0, kCrowdPosSite = 0x1429CD5C2;
constexpr uptr kSpotRainFn = 0x142E14F60, kSpotAdd = 0x142E15DF0, kSpotRadiusSite = 0x1429D9B9C, kSpotCall = 0x1429D9BB8;
constexpr uptr kRainRawFn = 0x142E0C3F0;
constexpr uptr kHmHands = 0x00, kHmStrength = 0x04, kHmDir = 0x08, kHmLevel = 0x10, kHmGrab = 0x14, kHmCrowdN = 0x50, kHmCrowds = 0x58;
constexpr uptr kHmArea = 0x140, kHmGold = 0x244;
constexpr uptr kCrowdArea = 0x48, kCrowdPos = 0x260, kCrowdRadius = 0x278, kCrowdMembers = 0x2B0;
constexpr int kCrowdMax = 16;
constexpr uptr kWsSpotN = 0x90, kWsSpots = 0x98, kSpotSize = 0x30, kSpotLife = 0x18, kSpotOwner = 0x20;
constexpr int kSpotListMax = 64;
constexpr double kReadMs = 50.0;  // the sender's hunter-manager read (20 Hz, so HuntTracker's 250 ms keeps about 4 Hz; edges at once)
constexpr int kGzCap = 512;

// ================================================================ byte checks (on first use; a failed group turns off its part)
enum Group : unsigned { kGHunt = 1u, kGSpot = 2u, kGRaw = 4u };
struct Site {
    uptr va;
    const char* sig;
    const char* name;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    {kHunterTick, "4C 8B DC 53 48 81 EC 90 00 00 00 C5 F8 29 74 24 70 48 8B D9 49 89 6B 08 33 ED 49 89 73 10 48 8D B1 70 01 00 00",
     "the hunter tick FUN_1429E1370", kGHunt},
    {kAreaReader, "48 89 5C 24 08 57 48 83 EC 20 48 8D 99 28 01 00 00 48 8B F9 48 8B CB FF", "the +0x140 reader FUN_1427AD2F0", kGHunt},
    {kHandsAgg, "48 8B 0F 8B 01 4C 8D 71 18 01 03", "the hands aggregate hm+0x00 (0x1429E15A0)", kGHunt},
    {kCrowdPosSite, "4C 8D B7 60 02 00 00", "the crowd centre +0x260 in FUN_1429CCF10 (0x1429CD5C2)", kGHunt},
    {kSpotRainFn, "48 83 EC 48 0F B6 44 24 78 C5 FA 10 05 97 A3 D7 00 88 44 24 38 48 8B 44", "the rain spot FUN_142E14F60 (kind 2)", kGSpot},
    {kSpotAdd, "48 89 5C 24 10 48 89 74 24 18 57 48 81 EC 80 00 00 00 48 8D 99 A8 69 00", "the spot add FUN_142E15DF0 (lock +0x69A8)", kGSpot},
    {kSpotRadiusSite, "C5 FA 10 15 BC C4 1F 01", "the hunter area's spot radius (0x1429D9B9C)", kGSpot},
    {kRainRawFn, "48 83 EC 48 C5 FB 10 42 10 C5 FB 5C 49 18 C5 FB 10 52 08 C5 EB 5C 41 10", "the raw rain FUN_142E0C3F0", kGRaw},
};
// clang-format on

bool g_checked = false;
unsigned g_groups = 0;

void Fail(unsigned& groups, unsigned g, const char* what) {
    const char* name = g == kGHunt ? "hunt" : g == kGSpot ? "spot" : "rain read";
    const char* off = g == kGHunt ? "HUNT not sent from this game (received HUNT still logged and marked)"
                    : g == kGSpot ? "rain spots off" : "the rain spot check lines off";
    if (groups & g) Log("[bt] hunt: byte check FAILED: %s (%s): %s", name, what, off);
    groups &= ~g;
}

unsigned Check() {
    if (g_checked) return g_groups;
    g_checked = true;
    unsigned groups = kGHunt | kGSpot | kGRaw;
    int ok = 0, total = 0;
    for (const Site& s : kSites) {
        ++total;
        bool good = I::SiteOk(s.va, s.sig);
        if (!good && s.va == kHunterTick) {  // an entry another module may have detoured: MinHook's jmp in place of the prologue
            u8 b0 = 0;
            good = I::Read(I::Addr(s.va), b0) && b0 == 0xE9 && I::SiteOk(kAreaReader, kSites[1].sig);
            if (good) Log("[bt] hunt: byte check: the hunter tick's entry carries a detour's jmp (another module): its body not compared");
        }
        if (good) ++ok;
        else Fail(groups, s.group, s.name);
    }
    ++total;
    if (I::RipOk(kSpotCall, 1, 5, kSpotRainFn)) ++ok;
    else Fail(groups, kGSpot, "the hunter area's call to FUN_142E14F60 (0x1429D9BB8)");
    ++total;
    if (I::HunterMgr() || I::RipOk(0x1429C4ED8, 3, 7, 0x147BECBD0)) ++ok;  // bt_sync's kHunter group (the manager may be null at the title)
    else Fail(groups, kGHunt, "the hunter manager global (bt_sync's hunter group)");
    g_groups = groups;
    Log("[bt] hunt: byte checks: %d/%d ok; hunt %s, spot %s, rain read %s", ok, total, (groups & kGHunt) ? "ok" : "OFF", (groups & kGSpot) ? "ok" : "OFF",
        (groups & kGRaw) ? "ok" : "OFF");
    return groups;
}

// ================================================================ SEH wrappers (bt_probe_r31.cpp's, as they are)
// FUN_142E14F60 (kind 2, rain) (ws, pos, radius, life, owner, oneshot): it goes to FUN_142E15DF0, which takes the weather lock
// +0x69A8 itself and only compares the owner (cmp [spot+0x20], owner)
bool SehSpot(uptr fn, uptr ws, const double* p, float radius, float life, void* owner) noexcept {
    __try {
        reinterpret_cast<void (*)(void*, const double*, float, float, void*, u8)>(fn)(reinterpret_cast<void*>(ws), p, radius, life, owner, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehRain(uptr fn, uptr ws, const double* p, float* out) noexcept {
    __try {
        *out = reinterpret_cast<float (*)(void*, const double*)>(fn)(reinterpret_cast<void*>(ws), p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ================================================================ state (game thread unless marked)
std::atomic<bool> g_stop{false};  // EmergencyStop (any thread): nothing is called, sent or refreshed again
int g_rain = 0;                   // bt_rain from session.ini
int g_level_cfg = 0;              // bt_sync from session.ini
int g_role_cfg = 0;               // 1 host, 2 guest, 0 another
bool g_on = false;                // a two-game session with bt_sync >= 1
bool g_spot_dead = false;         // the spot call faulted once: no more spots this run
bool g_rehearse = false;          // bt_sync 8 -1 1 (hidden runs): the real-save gate here
R::LineBudget g_budget{1000.0, 12};       // the ordinary lines
R::LineBudget g_budget_edge{1000.0, 30};  // the edge lines (each once per edge; the cap only stops a runaway)

// the last tick's view, for OnMsg / SpotWant / Command
bool g_peer = false, g_me_ok = false, g_real_save = false;
double g_me[3] = {0, 0, 0};
std::uint8_t g_partner_trouble = 0;

void Line(double now_ms, bool force, const char* fmt, ...) {
    if (!(force ? g_budget_edge : g_budget).Take(now_ms)) return;
    char b[640];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    Log("[bt] hunt: %s", b);
}

// ---- the sender
struct Sender {
    R::HuntTracker tr;
    double read_ms = -1e18;
    bool nat_up = false;       // the last read: hm+0x140 set here
    bool pull_logged = false;  // the natural pull line, once per episode
    bool crowd_logged = false; // "no local crowd", once per episode
    std::uint16_t logged_episode = 0;
    std::uint8_t logged_flags = 0xFF;
    unsigned tx_up = 0, tx_clear = 0, unsent = 0;
    const char* why = "not started";
} g_tx;
struct TestHunt {
    bool on = false;
    double t0 = 0, dur_ms = 0;
    double pos[3] = {0, 0, 0};
    float dir[2] = {0, 0};
} g_test;

// ---- the receiver
struct Receiver {
    R::HuntView view;
    unsigned rx = 0, bad = 0;
} g_rx;

// ---- the spots
H::SpotBook g_book;
alignas(16) unsigned char g_owner[R::kSpotSlots][16];  // the 8 owner keys: static addresses in this DLL, never dereferenced by the game
void* Owner(unsigned i) { return &g_owner[i][0]; }
unsigned g_made = 0, g_refreshed = 0, g_held = 0, g_gone = 0, g_stale = 0;
I::GzInfo g_gz[kGzCap];

const char* UserName(std::uint8_t u) { return u == static_cast<std::uint8_t>(SpotUser::hunt) ? "hunt" : u == static_cast<std::uint8_t>(SpotUser::catcher) ? "catcher" : "?"; }

// nullptr: spots can be booked here now; else why not.
const char* SpotsOff() {
    if (g_stop.load()) return "stopped (emergency stop)";
    if (g_rain < 1) return "bt_rain is 0 here";
    if (!g_on) return "not a two-game session";
    if (I::SyncLevel() < 2) return "bt_sync is not 2 here (bt_sync 1 sends but applies nothing)";
    if (!I::PeerOn()) return "no partner relay";
    if (g_spot_dead) return "the spot call faulted earlier (rain spots off for this run)";
    if (!(Check() & kGSpot)) return "the spot byte checks failed";
    // review (safety 1): no rain spot on a real-save guest (timefall there is world state in her save); no slot is booked
    if (g_real_save) return R::kSpotRealSaveWhy;
    if (g_rehearse)
        return "this game plays on its own real save (host_save=0): a rain spot is timefall there (bt_sync 8 -1 1: the test's real-save stand-in)";
    return nullptr;
}
bool RealSaveGate() { return g_real_save || g_rehearse; }

// The spot list: is slot i's owner listed (and its life), how many of ours are listed. False: no list readable.
struct SpotList {
    bool listed[R::kSpotSlots] = {};
    float life[R::kSpotSlots] = {};
    int ours = 0, total = -1;
};
bool ReadSpots(uptr ws, SpotList& out) {
    if (!ws) return false;
    std::int32_t n = 0;
    uptr sp = 0;
    if (!I::Read(ws + kWsSpotN, n) || !I::Read(ws + kWsSpots, sp)) return false;
    if (n < 0 || n > 4096) return false;
    out.total = n;
    for (int i = 0; sp && i < n && i < kSpotListMax; ++i) {
        u64 owner = 0;
        if (!I::Read(sp + static_cast<uptr>(i) * kSpotSize + kSpotOwner, owner)) return false;
        for (unsigned k = 0; k < R::kSpotSlots; ++k)
            if (owner == reinterpret_cast<u64>(Owner(k))) {
                out.listed[k] = true, ++out.ours;
                float life = -1;
                I::Read(sp + static_cast<uptr>(i) * kSpotSize + kSpotLife, life);
                out.life[k] = life;
            }
    }
    return true;
}

// The nearest ARMED gazer streamed here to pos (horizontal m), -1 none / unknown.
double NearestArmed(const double* pos) {
    const int n = I::Gazers(g_gz, kGzCap);
    double best = -1;
    for (int i = 0; i < n; ++i) {
        const I::GzInfo& z = g_gz[i];
        if (!z.pos_ok || !(z.flags & H::kGzArmed)) continue;
        const double d = H::Dist2D(z.pos, pos);
        if (best < 0 || d < best) best = d;
    }
    return best;
}

void RawCheck(unsigned i, const char* tag, double now_ms) {
    const H::SpotSlot& x = g_book.s[i];
    const uptr ws = I::WeatherSys();
    float raw = 0;
    const bool ok = (Check() & kGRaw) && ws && SehRain(I::Addr(kRainRawFn), ws, x.pos, &raw);
    if (ok) Line(now_ms, true, "rain spot check %s: raw %.3f at the spot (%s slot %u, r %.0f m)", tag, static_cast<double>(raw), UserName(x.user), i,
                 static_cast<double>(x.radius));
    else Line(now_ms, true, "rain spot check %s: raw unreadable at the spot (%s slot %u)", tag, UserName(x.user), i);
}

// Every live slot let go at once (the link, the partner, the relay off): no refresh again; each spot dies within its life by itself.
// A slot that made a spot drains and stays watched (the spot list is only read) until the game drops it, so "rain spot gone" still
// comes (gate run 20261128-011308: a "partner gone" dropped the watch and the gone line never came). A load forgets the drains too
// (forget: the world is rebuilt).
void LetGoAll(const char* why, double now_ms, bool forget) {
    const unsigned live = g_book.Count(H::SlotState::live), drain = g_book.Count(H::SlotState::draining);
    if (forget) {
        g_book.Clear();
        if (live || drain)
            Line(now_ms, true, "%s: %u rain spots let go, %u no longer watched (each dies within its life: 10 s, 3 s on a real save)", why, live, drain);
        return;
    }
    const unsigned n = g_book.LetGo(now_ms);
    if (n)
        Line(now_ms, true, "%s: %u rain spots let go, %u watched until gone (each dies within its life: 10 s, 3 s on a real save)", why, n,
             g_book.Count(H::SlotState::draining));
}

// ================================================================ the sender
bool ReadNatural(R::HuntIn& in, const TickCtx& c) {
    const uptr hm = I::HunterMgr();
    if (!hm) return false;
    u64 area = 0;
    if (!I::Read(hm + kHmArea, area)) return false;
    in.up = area != 0;
    if (!in.up) return true;
    std::int32_t hands = 0;
    float strength = 0, d0 = 0, d1 = 0, level = 0;
    u8 grab = 0, gold = 0;
    I::Read(hm + kHmHands, hands);
    I::Read(hm + kHmStrength, strength);
    I::Read(hm + kHmDir, d0);
    I::Read(hm + kHmDir + 4, d1);
    I::Read(hm + kHmLevel, level);
    I::Read(hm + kHmGrab, grab);
    I::Read(hm + kHmGold, gold);
    in.gold = gold != 0, in.hands = hands, in.level = level, in.strength = strength, in.dir[0] = d0, in.dir[1] = d1;
    in.pulled = false;  // build spec stage 3: PULLED only from bt_sync 8
    if (H::NaturalPullSeen(hands, grab) && !g_tx.pull_logged) {
        g_tx.pull_logged = true;
        Line(c.now_ms, true, "natural pull read here: hands %d grab %u level %.2f strength %.2f dir (%.2f %.2f) (not sent as PULLED: unverified, build spec "
                             "stage 3)",
             hands, grab, static_cast<double>(level), static_cast<double>(strength), static_cast<double>(d0), static_cast<double>(d1));
    }
    std::int32_t n = 0;
    uptr list = 0;
    bool found = false;
    if (I::Read(hm + kHmCrowdN, n) && I::Read(hm + kHmCrowds, list) && list)
        for (int i = 0; i < n && i < kCrowdMax && !found; ++i) {
            uptr ent = 0;
            if (!I::Read(list + static_cast<uptr>(i) * 8, ent) || ent < H::kCrowdEntOff) continue;
            const uptr comp = ent - static_cast<uptr>(H::kCrowdEntOff);
            u64 c48 = 0;
            if (!I::Read(comp + kCrowdArea, c48) || !H::CrowdIsLocal(c48, area)) continue;
            double p[3] = {0, 0, 0};
            float r = 0;
            std::int32_t members = 0;
            if (!I::ReadN(comp + kCrowdPos, p, sizeof(p)) || !R::PosSane(p) || !I::Read(comp + kCrowdRadius, r) || !std::isfinite(r)) continue;
            I::Read(comp + kCrowdMembers, members);
            for (int k = 0; k < 3; ++k) in.center[k] = static_cast<float>(p[k]);
            in.radius = r, in.members = members;
            found = true;
        }
    if (!found) {
        for (int k = 0; k < 3; ++k) in.center[k] = c.me_ok ? static_cast<float>(c.me[k]) : 0.0f;
        in.radius = 0, in.members = 0;
        if (!g_tx.crowd_logged) {
            g_tx.crowd_logged = true;
            Line(c.now_ms, false, "the hunter area is up but no local crowd was read (%d listed): HUNT carries Sam's position, radius 0", n);
        }
    }
    return true;
}

void SendHunt(const R::BtsHunt& m0, double now_ms, SendFn send, std::uint32_t seq, const char* note) {
    R::BtsHunt m = m0;
    m.sender_ms = static_cast<std::uint32_t>(static_cast<std::int64_t>(now_ms) & 0xFFFFFFFFll);
    std::uint8_t b[R::kBtsHuntSize];
    const std::size_t n = R::EncodeHunt(seq, m, b, sizeof(b));
    const bool sent = n && send && send(b, n);
    if (!sent) ++g_tx.unsent;
    else if (m.kind == R::kHuntUp) ++g_tx.tx_up;
    else ++g_tx.tx_clear;
    const bool test = (m.flags & R::kHfTest) != 0;
    if (m.kind == R::kHuntUp && sent && m.episode == g_tx.logged_episode && m.flags == g_tx.logged_flags) return;  // the 4 Hz keepalive
    g_tx.logged_episode = m.episode, g_tx.logged_flags = m.kind == R::kHuntUp ? m.flags : 0xFF;
    if (m.kind == R::kHuntClear) {
        Line(now_ms, true, "HUNT %s%s: CLEAR (episode %u, %.1f s)%s%s (seq %u)", sent ? "sent" : "NOT sent", test ? " (test)" : "", m.episode,
             (now_ms - g_tx.tr.start_ms) / 1000.0, note ? "; " : "", note ? note : "", seq);
        return;
    }
    Line(now_ms, true, "HUNT %s%s: UP (episode %u) at (%.1f %.1f %.1f) r %.0f m, hands %u, members %u, level %.2f, strength %.2f%s%s%s (seq %u)",
         sent ? "sent" : "NOT sent", test ? " (test)" : "", m.episode, static_cast<double>(m.center[0]), static_cast<double>(m.center[1]),
         static_cast<double>(m.center[2]), static_cast<double>(m.radius), m.hands, m.members, static_cast<double>(m.level), static_cast<double>(m.strength),
         (m.flags & R::kHfGold) ? ", gold" : "", (m.flags & R::kHfPulled) ? ", PULLED" : "", (m.flags & R::kHfDeep) ? " (deep)" : "", seq);
}

// The tracker's CLEAR now if it is active (a load, the gate closed, the test done); sent only while the relay is on.
void SenderClear(double now_ms, bool can_send, SendFn send, std::uint32_t seq, const char* note) {
    if (!g_tx.tr.active) return;
    R::HuntIn none;
    R::BtsHunt m;
    if (g_tx.tr.Step(none, now_ms, &m) == R::kHuntClear) {
        if (can_send) SendHunt(m, now_ms, send, seq, note);
        else Line(now_ms, false, "HUNT episode %u ended unsent (%s)", m.episode, note ? note : "the relay is off");
    }
    g_tx.pull_logged = g_tx.crowd_logged = false;
}

void SenderTick(const TickCtx& c) {
    if (c.now_ms - g_tx.read_ms < kReadMs) return;
    g_tx.read_ms = c.now_ms;
    R::HuntIn in;
    bool test = false, read_ok = false;
    if (g_test.on) {
        const H::TestFrame f = H::TestHuntAt(c.now_ms - g_test.t0, g_test.dur_ms);
        if (!f.up) {
            g_test.on = false;
            Line(c.now_ms, true, "test HUNT done after %.0f s", g_test.dur_ms / 1000.0);
        } else {
            test = read_ok = true;
            in.up = true, in.test = true, in.pulled = f.pulled, in.level = f.level, in.strength = f.strength, in.hands = f.hands;
            in.members = H::kTestMembers, in.radius = H::kTestRadiusM;
            for (int k = 0; k < 3; ++k) in.center[k] = static_cast<float>(g_test.pos[k]);
            if (f.pulled) in.dir[0] = g_test.dir[0], in.dir[1] = g_test.dir[1];
        }
    }
    if (!test) {
        read_ok = (Check() & kGHunt) && ReadNatural(in, c);
        g_tx.nat_up = read_ok && in.up;
        if (!read_ok) in = R::HuntIn{};
    }
    H::SendIn g;
    g.role = c.role, g.sync_level = c.sync_level, g.peer = c.peer, g.partner_caps = c.partner.caps;
    g.reads_ok = (Check() & kGHunt) != 0, g.test = test, g.loading = c.loading;
    const char* why = H::SendGate(g);
    g_tx.why = why ? why : (read_ok ? "on" : "the hunter manager unreadable");
    const bool can_send = c.peer && c.send && c.seq;
    if (why) {
        SenderClear(c.now_ms, can_send && c.sync_level >= 1, c.send, can_send ? c.seq() : 0, why);
        return;
    }
    R::BtsHunt m;
    const std::uint8_t kind = g_tx.tr.Step(in, c.now_ms, &m);
    if (!kind) return;
    SendHunt(m, c.now_ms, c.send, c.seq(), nullptr);
    if (kind == R::kHuntClear) g_tx.pull_logged = g_tx.crowd_logged = false;
}

// ================================================================ the spots (game thread)
void EvalSlot(unsigned i, const TickCtx& c) {
    H::SpotSlot& x = g_book.s[i];
    const bool live = g_book.SourceLive(i, c.now_ms);
    x.eval_ms = c.now_ms;
    R::SpotVerdict v;
    R::RainSpotIn in;
    in.bt_rain = g_rain, in.source_live = live, in.real_save = RealSaveGate(), in.not_playing = c.playing_why, in.carried = c.carried;
    in.my_dist_m = c.me_ok ? H::Dist2D(c.me, x.pos) : -1.0;
    in.radius = x.radius;
    const unsigned used = g_book.Count(H::SlotState::live);
    in.slots_used = used ? used - 1 : 0, in.has_slot = true;
    const char* off = SpotsOff();
    if (off) v = R::SpotVerdict{R::SpotCall::drop, off};
    else if (!live) v = R::SpotVerdict{R::SpotCall::drop, x.end_why ? x.end_why : "its source ended"};
    else {
        v = R::RainSpotGate(in);  // gazer_near false here: the ARMED rule below holds instead of dropping (it would be re-booked at 4 Hz)
        if (v.call == R::SpotCall::make && H::SpotGazerBlock(x.made_once || g_book.OwnSpotCovers(i, x.pos), NearestArmed(x.pos)))
            v = R::SpotVerdict{R::SpotCall::hold, "an ARMED gazer streamed here is within 150 m (this game rains there itself)"};
    }
    const uptr ws = v.call == R::SpotCall::make ? I::WeatherSys() : 0;
    if (v.call == R::SpotCall::make && !ws) v = R::SpotVerdict{R::SpotCall::hold, "no weather system (no world?)"};
    switch (v.call) {
        case R::SpotCall::make: {
            if (!x.made_once) RawCheck(i, "-0 s", c.now_ms);
            const float life = R::RainSpotLife(in.real_save);
            if (!SehSpot(I::Addr(kSpotRainFn), ws, x.pos, x.radius, life, Owner(i))) {
                g_spot_dead = true;
                Line(c.now_ms, true, "rain spot call FAULTED (%s slot %u): rain spots off for this run", UserName(x.user), i);
                g_book.Release(i, c.now_ms);
                return;
            }
            ++x.refreshes, ++g_refreshed;
            x.made_ms = c.now_ms;
            if (!x.made_once || x.held)
                Line(c.now_ms, true, "rain spot %s: %s at (%.1f %.1f %.1f) r %.0f m, you %.0f m (slot %u, life %.0f s%s)", x.made_once ? "on again" : "on",
                     UserName(x.user), x.pos[0], x.pos[1], x.pos[2], static_cast<double>(x.radius), in.my_dist_m, i, static_cast<double>(life),
                     in.real_save ? (g_real_save ? ", real save" : ", real-save rehearsal") : "");
            if (!x.made_once) x.made_once = true, x.first_ms = c.now_ms, x.checks = 1, ++g_made;
            x.held = false, x.hold_why = nullptr;
            break;
        }
        case R::SpotCall::hold:
            if (x.hold_why != v.why) {
                x.hold_why = v.why;
                ++g_held;
                Line(c.now_ms, true, "rain spot held: %s (%s slot %u, you %.0f m, r %.0f m%s)", v.why, UserName(x.user), i, in.my_dist_m,
                     static_cast<double>(x.radius), x.made_once ? "; the spot dies within its life" : "");
            }
            x.held = true;
            break;
        case R::SpotCall::drop:
            Line(c.now_ms, true, "rain spot off: %s (%s slot %u, %d refreshes%s)", v.why, UserName(x.user), i, x.refreshes,
                 x.made_once ? "; watching the list for its end" : "; it never made a spot");
            g_book.Release(i, c.now_ms);
            break;
    }
}

void DrainSlot(unsigned i, double now_ms) {
    H::SpotSlot& x = g_book.s[i];
    if (now_ms - x.poll_ms < H::kGonePollMs) return;
    x.poll_ms = now_ms;
    SpotList l;
    const bool read = ReadSpots(I::WeatherSys(), l);
    if (!read) {
        if (now_ms - x.drop_ms > H::kGoneWaitMs) {
            Line(now_ms, true, "rain spot gone unverified: slot %u (the spot list was unreadable for 15 s)", i);
            g_book.Free(i);
        }
        return;
    }
    switch (H::DrainVerdict(now_ms, x.drop_ms, l.listed[i])) {
        case H::Drain::gone:
            ++g_gone;
            Line(now_ms, true, "rain spot gone: slot %u (spots %d) %.1f s after its last refresh (%s)", i, l.ours,
                 x.made_ms > -1e17 ? (now_ms - x.made_ms) / 1000.0 : -1.0, UserName(x.user));
            g_book.Free(i);
            break;
        case H::Drain::stale:
            ++g_stale;
            Line(now_ms, true, "rain spot still listed 15 s after its last refresh: slot %u (life %.1f, spots %d): no longer watched", i,
                 static_cast<double>(l.life[i]), l.ours);
            g_book.Free(i);
            break;
        case H::Drain::wait: break;
    }
}

void SpotTick(const TickCtx& c) {
    for (unsigned i = 0; i < R::kSpotSlots; ++i) {
        H::SpotSlot& x = g_book.s[i];
        if (x.st == H::SlotState::live) {
            if (g_book.Due(i, c.now_ms) || !g_book.SourceLive(i, c.now_ms)) EvalSlot(i, c);
            if (x.st == H::SlotState::live && x.made_once && x.checks == 1 && c.now_ms - x.first_ms >= H::kCheckLateMs) {
                x.checks = 2;
                RawCheck(i, "+10 s", c.now_ms);
            }
        } else if (x.st == H::SlotState::draining) {
            DrainSlot(i, c.now_ms);
        }
    }
}

// ================================================================ the receiver
void ViewReset() { g_rx.view = R::HuntView{}; }

}  // namespace

void Install(const Config& config) noexcept {
    g_rain = R::CfgLevel(config.bt_rain, 1);
    g_level_cfg = config.bt_sync;
    g_role_cfg = config.role == Role::host ? 1 : config.role == Role::guest ? 2 : 0;
    g_on = g_role_cfg != 0 && g_level_cfg >= 1;
    if (!g_on) return;
    Log("[bt] hunt: HUNT on (bt_sync %d; sent and taken once the partner's hello offers cap 0x400); rain spots %s (bt_rain=%d)", g_level_cfg,
        g_rain < 1 ? "off" : g_level_cfg < 2 ? "off (bt_sync 1 applies nothing)" : "on", g_rain);
}

void OnHello(const PeerHello& peer) noexcept {
    if (!g_on || g_stop.load()) return;
    Log("[bt] hunt: partner hello: HUNT %s; rain spots here %s", (peer.caps & R::kCapHunt) ? "both ways" : "received only (the partner takes none)",
        g_rain < 1 ? "off (bt_rain=0)" : g_level_cfg < 2 ? "off (bt_sync 1)" : "on");
}

void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept {
    if (!g_on || g_stop.load() || type != R::kBtsHunt) return;
    R::BtsHunt m;
    if (!R::DecodeHunt(msg, n, m)) {
        ++g_rx.bad;
        Line(now_ms, false, "a bad HUNT (%zu B) dropped", n);
        return;
    }
    ++g_rx.rx;
    const R::HuntView::Out o = g_rx.view.Take(m, now_ms);
    const bool test = (m.flags & R::kHfTest) != 0, gold = (m.flags & R::kHfGold) != 0;
    const double c[3] = {m.center[0], m.center[1], m.center[2]};
    const double you = g_me_ok ? H::Dist2D(g_me, c) : -1.0;
    if (m.kind == R::kHuntUp) {
        btmarks::Tar(c, gold, test, now_ms, H::TarRingRadius(m.radius, (m.flags & R::kHfPulled) != 0, m.level));  // the ring on the partner body when fresh, else at the centre (refreshed at 4 Hz)
        const char* off = SpotsOff();
        int slot = -1;
        if (!off) {
            slot = g_book.Want(static_cast<std::uint8_t>(SpotUser::hunt), c, R::kSpotHuntRadiusM, now_ms);
            if (slot < 0) off = "all 8 spot slots are in use";
        }
        if (o.up) {
            char spot[192];  // review (safety 1): the real-save refusal text is long
            if (off) std::snprintf(spot, sizeof(spot), "rain spot: none (%s)", off);
            else std::snprintf(spot, sizeof(spot), "rain spot: slot %d r %.0f m", slot, static_cast<double>(R::kSpotHuntRadiusM));
            Line(now_ms, true, "partner hunt UP at (%.1f %.1f %.1f) r %.0f m, hands %u, members %u, level %.2f (episode %u%s%s%s), you %.0f m; %s", c[0], c[1],
                 c[2], static_cast<double>(m.radius), m.hands, m.members, static_cast<double>(m.level), m.episode, gold ? ", gold" : "",
                 (m.flags & R::kHfVehicle) ? ", vehicle" : "", test ? ", test" : "", you, spot);
        }
        if (o.pulled)
            Line(now_ms, true, "partner PULLED level %.2f (hands %u, strength %.2f, dir (%.2f %.2f), episode %u%s%s)", static_cast<double>(m.level), m.hands,
                 static_cast<double>(m.strength), static_cast<double>(m.dir[0]), static_cast<double>(m.dir[1]), m.episode,
                 (m.flags & R::kHfDeep) ? ", deep" : "", test ? ", test" : "");
        return;
    }
    // CLEAR
    if (o.clear) Line(now_ms, true, "partner hunt CLEAR (episode %u, %.1f s%s)", m.episode, o.secs, test ? ", test" : "");
    g_book.End(static_cast<std::uint8_t>(SpotUser::hunt), "the hunt ended");
    if (!g_partner_trouble) btmarks::ClearTrouble();  // no TROUBLE keeps the ring (a TROUBLE CLEAR clears it too)
}

void Tick(const TickCtx& c) noexcept {
    if (!g_on || g_stop.load()) return;
    g_real_save = c.real_save, g_partner_trouble = c.partner_trouble;
    g_me_ok = c.me_ok;
    if (c.me_ok) std::memcpy(g_me, c.me, sizeof(g_me));
    const bool was_peer = g_peer;
    g_peer = c.peer;
    if (was_peer && !c.peer) {
        LetGoAll("the relay is off", c.now_ms, false);
        ViewReset();
    }
    if (c.role == 0 || c.sync_level < 1) return;
    SenderTick(c);
    if (g_rx.view.Expire(c.now_ms)) {
        Line(c.now_ms, true, "partner hunt: no HUNT for 3 s (no CLEAR came): the hunt is taken as ended");
        g_book.End(static_cast<std::uint8_t>(SpotUser::hunt), "no HUNT for 3 s");
    }
    SpotTick(c);
}

void OnWorldEvent(WorldEvent e, double now_ms) noexcept {
    if (!g_on || g_stop.load()) return;
    switch (e) {
        case WorldEvent::load_start:
            SenderClear(now_ms, g_peer && I::PeerOn(), &I::SendBts, I::Seq(), "a load here");
            if (g_test.on) g_test.on = false, Line(now_ms, true, "test HUNT ended by a load");
            LetGoAll("load", now_ms, true);
            ViewReset();
            break;
        case WorldEvent::load_end: break;
        case WorldEvent::link_lost:
            if (g_tx.tr.active) {  // nobody to tell: the next UP is a new episode (the counter goes on)
                const std::uint16_t ep = g_tx.tr.episode;
                g_tx.tr = R::HuntTracker{};
                g_tx.tr.episode = ep;
                g_tx.pull_logged = g_tx.crowd_logged = false;
            }
            if (g_test.on) g_test.on = false, Line(now_ms, true, "test HUNT ended (link lost)");
            LetGoAll("link lost", now_ms, false);
            ViewReset();
            break;
        case WorldEvent::partner_gone:
            // The spots (this module's only game records) are let go. The sender and the view stay: sending writes nothing here and
            // stops with the relay (SendGate), and a partner really gone sends no HUNT, so the view expires in 3 s (Expire) and any
            // slot booked again drops. Gate run 20261128-011308: a "partner gone" from a beacon 5 s old fired between two UPs 125 ms
            // apart (guest 54.406); ending the sender there would cut a test HUNT short with no CLEAR.
            LetGoAll("partner gone", now_ms, false);
            break;
    }
}

void EmergencyStop() noexcept {
    // any thread, synchronous, no later Tick: nothing to restore in the game (a spot is never refreshed again and dies within its
    // life by itself: 10 s; a real-save guest makes none; nothing else here writes the game); PulledNow and SpotWant answer false
    // from now on
    g_stop.store(true);
}

std::uint16_t Caps() noexcept { return g_on && !g_stop.load() ? static_cast<std::uint16_t>(R::kCapHunt) : 0; }

void Status(char* out, std::size_t cap) noexcept {
    if (!out || !cap) return;
    if (!g_on) {
        std::snprintf(out, cap, "hunt: off (not a two-game session or bt_sync=0; bt_rain=%d)", g_rain);
        return;
    }
    const char* spots = SpotsOff();
    std::snprintf(out, cap, "hunt: send %s (ep %u, up %u clear %u unsent %u%s); got %u bad %u, partner %s; spots %s: live %u drain %u, made %u held %u gone %u",
                  g_tx.why, g_tx.tr.episode, g_tx.tx_up, g_tx.tx_clear, g_tx.unsent, g_test.on ? ", TEST" : "", g_rx.rx, g_rx.bad,
                  g_rx.view.up ? "UP" : "none", spots ? "off" : (RealSaveGate() ? "on (real-save gate)" : "on"), g_book.Count(H::SlotState::live),
                  g_book.Count(H::SlotState::draining), g_made, g_held, g_gone);
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    auto put = [&](const char* text) {
        if (msg && cap) std::snprintf(msg, cap, "%s", text);
    };
    if (op != 8) return put("bt sync: not bt_hunt's op"), false;
    if (g_stop.load()) return put("bt sync 8: stopped (emergency stop)"), false;
    if (!automation::Enabled()) return put("bt sync 8: the test HUNT is for hidden runs only"), false;
    if (!g_on) return put("bt sync 8: off (not a two-game session, or bt_sync=0)"), false;
    const H::Op8Args a = H::ParseOp8(v[1], v[2]);
    const double now = I::NowMs();
    if (a.kind == H::Op8::bad) return put("bt sync 8: [secs 8..120] the test HUNT, or -1 1|0 the real-save rehearsal of the rain spots here"), false;
    if (a.kind == H::Op8::rehearse) {
        g_rehearse = a.on;
        Line(now, true, "real-save rehearsal %s: this game's rain spots %s (hidden run)", a.on ? "on" : "off",
             a.on ? "follow a real save's gate (none made: a rain spot is timefall there)" : "follow this game's own save again");
        if (msg && cap) std::snprintf(msg, cap, "bt sync 8: real-save rehearsal %s (log)", a.on ? "on" : "off");
        return true;
    }
    if (g_test.on) return put("bt sync 8: a test HUNT is running"), false;
    if (!I::PeerOn()) return put("bt sync 8: no partner hello yet"), false;
    if (!(I::Peer().caps & R::kCapHunt)) return put("bt sync 8: the partner's build takes no HUNT (cap 0x400)"), false;
    if (I::LoadingNow()) return put("bt sync 8: a load"), false;
    if (g_tx.nat_up || g_tx.tr.active) return put("bt sync 8: a real hunt is up here (its HUNT goes as it is)"), false;
    double me[3] = {0, 0, 0}, fx = 0, fy = 0;
    if (!I::SamFacing(me, &fx, &fy)) return put("bt sync 8: no local player transform"), false;
    g_test = TestHunt{};
    g_test.on = true, g_test.t0 = now, g_test.dur_ms = a.secs * 1000.0;
    std::memcpy(g_test.pos, me, sizeof(g_test.pos));
    g_test.dir[0] = static_cast<float>(fx), g_test.dir[1] = static_cast<float>(fy);
    g_tx.read_ms = -1e18;  // the UP goes at the next tick
    Line(now, true, "test HUNT: episode %u at (%.1f %.1f %.1f): UP r %.0f m, PULLED level %.2f from %.0f s to %.0f s, CLEAR at %.0f s", g_tx.tr.episode + 1u,
         me[0], me[1], me[2], static_cast<double>(H::kTestRadiusM), static_cast<double>(H::kTestLevel), H::kTestPullFromMs / 1000.0, H::kTestPullToMs / 1000.0,
         a.secs);
    if (msg && cap) std::snprintf(msg, cap, "bt sync 8: test HUNT for %.0f s (log)", a.secs);
    return true;
}

bool PulledNow() noexcept {
    if (!g_on || g_stop.load() || !g_test.on) return false;
    return H::TestHuntAt(I::NowMs() - g_test.t0, g_test.dur_ms).pulled;
}

bool SpotWant(SpotUser who, const double (&pos)[3], float radius_m, double now_ms) noexcept {
    if (!g_on || SpotsOff()) return false;
    if (!R::PosSane(pos) || !(radius_m > 0) || !(radius_m <= static_cast<float>(R::kSpotRadiusMaxM))) return false;
    return g_book.Want(static_cast<std::uint8_t>(who), pos, radius_m, now_ms) >= 0;
}

void SpotDrop(SpotUser who) noexcept {
    if (!g_on || g_stop.load()) return;
    g_book.End(static_cast<std::uint8_t>(who), who == SpotUser::catcher ? "the catcher ended" : "the hunt ended");
}

}  // namespace st::bt::hunt
