// Round 31 stage 8: the kill look, RESPAWN and SKY (bt_extra.hpp; the round 31 build spec, Stage 8). The game calls are those of
// bt_probe_r31.cpp (op 34's SetStartControlFlags call and op 36's stats read, with their byte checks and SEH wrappers, copied), plus
// EnableRespawnGazers FUN_1429A76D0, read from ds.exe for this stage (its message 0x58305569 to actor type 8, the handler's +0x3A8
// compare) and byte-checked here. Nothing here writes game memory except through the two game calls.
#include "bt_extra.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "MinHook.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "automation.hpp"
#include "bt_extra_rules.hpp"
#include "bt_rules.hpp"
#include "bt_sync_internal.hpp"
#include "config.hpp"
#include "log.hpp"

namespace st::bt::extra {
namespace {

namespace R = st::btrules;
namespace X = st::btrules::extra;
namespace I = st::bt::internal;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using uptr = std::uintptr_t;

// ================================================================ ds.exe DC addresses
constexpr uptr kRespawnFn = 0x1429A76D0;   // EnableRespawnGazers(scene): message 0x58305569 {scene} to every actor of type 8
constexpr uptr kSceneHashFn = 0x1429A71E0; // GetUUIDHashOfGazerScene(guid*, u32* out): crc32c over the 16 GUID bytes
constexpr uptr kBroadcastFn = 0x1422FA510, kRegGlobal = 0x147BBB8C0;
constexpr uptr kSkyFn = 0x142E1FED0, kWsGlobal = 0x147BF83B8;  // SetStartControlFlags(flag): ws+0x6518 |= 1, ws+0x651C = 0.2f
// The weather update FUN_142E1D600 counts +0x651C down (0x142E1DC38, frozen while the game is paused) and zeroes +0x6518 when it
// runs out (0x142E1DC63): stopping the calls is the restore. It also zeroes +0x6508 every update (0x142E1D66F) before setting it
// from the catcher state, a player flag and bit 0 (0x142E1D6B3), so +0x6508 read in the tick of a stop still shows 1.
constexpr uptr kWs6508 = 0x6508, kWsSky = 0x6518;
constexpr uptr kStatsGlobal = 0x147BFD960;  // FUN_142F21230 / FUN_142F21B00 add 1 to stat 0x79 / 0x7A of [it] + 0x10 (XOR-masked)
// the gazer actor and its locator / scene (bt_sync.cpp's kActor / kField checks cover these reads)
constexpr uptr kActLoc = 0x18, kActState = 0x24, kActEnt = 0x68, kLocScene = 0x80, kLocGuid = 0x88, kSceneGuid = 0x98;

// ================================================================ the byte checks (first use, game thread; logged once)
enum Group : unsigned { kGKill = 1u, kGRespawn = 2u, kGSky = 4u, kAllGroups = 7u };
struct Site {
    uptr va;
    const char* sig;
    const char* name;
    unsigned group;
};
struct Rip {
    uptr insn;
    unsigned off, len;
    uptr want;
    const char* what;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    // the stats (bt_probe_r31.cpp's kGStats, op 36): the two adds and the XOR-masked table; the kill look's "no credit" (PF 21) holds
    // for this code only
    {0x142F21230, "44 8B C2 48 83 C1 10 BA 79 00 00 00 41 B9 FF C9 9A 3B E9 29 15 00 00", "the stat 0x79 add FUN_142F21230", kGKill},
    {0x142F21B00, "44 8B C2 48 83 C1 10 BA 7A 00 00 00 41 B9 FF C9 9A 3B E9 59 0C 00 00", "the stat 0x7A add FUN_142F21B00", kGKill},
    {0x142F227AA, "4C 8D B1 F8 04 00 00", "the stat values +0x4F8 (0x142F227AA)", kGKill},
    {0x142F227F9, "8B 94 BE C8 08 00 00 33 94 AE A8 08 00 00", "the stat masks +0x8C8 / +0x8A8 (0x142F227F9)", kGKill},
    // RESPAWN: EnableRespawnGazers' prologue (bt_probe_r31.cpp's kRespawnFn; checked before this module's hook goes in), its message
    // to type 8, the gazer message handler's case and its +0x3A8 compare, GetUUIDHashOfGazerScene (the scene id = the field id)
    {kRespawnFn, "48 83 EC 58 48 8B 05 55 99 5B 02 48 33 C4 48 89 44 24 48 33 D2 48 89 4C 24 38 48 8B", "EnableRespawnGazers FUN_1429A76D0", kGRespawn},
    {0x1429A7716, "BA 08 00 00 00 48 C7 44 24 30 69 55 30 58", "its message 0x58305569 to actor type 8 (0x1429A7716)", kGRespawn},
    {0x14299F1E6, "3D 69 55 30 58 0F 84 CD 01 00 00", "the gazer message case 0x58305569 (0x14299F1E6)", kGRespawn},
    {0x14299F3BE, "48 8B 42 18 48 39 81 A8 03 00 00 0F 85 42 FD FF FF 81 89 B4 01 00 00 00 00 00 04",
     "its scene compare +0x3A8 and the respawn flag (0x14299F3BE)", kGRespawn},
    {kSceneHashFn, "48 85 D2 74 15 33 C0 F2 48 0F 38 F1 01 44 8B C0 F2 4C 0F 38 F1 41 08 44 89 02 C3", "GetUUIDHashOfGazerScene FUN_1429A71E0",
     kGRespawn},
    // SKY (bt_probe_r31.cpp's kGSky, op 34) and the bit's only reader
    {kSkyFn, "48 8B 05 E1 84 DD 04 48 85 C0 74 15 84 C9 74 11 83 88 18 65 00 00 01 C7 80 1C 65 00 00 CD CC 4C", "SetStartControlFlags FUN_142E1FED0",
     kGSky},
    {0x142E1D6B3, "F6 87 18 65 00 00 01 74 07 C6 87 08 65 00 00 01", "the +0x6518 bit 0 reader (0x142E1D6B3: +0x6508 = 1)", kGSky},
};
constexpr Rip kRips[] = {
    {0x142995F35, 3, 7, kStatsGlobal, "the stats global 0x147BFD960", kGKill},
    {0x1429A76EA, 3, 7, kRegGlobal, "EnableRespawnGazers' registry 0x147BBB8C0", kGRespawn},
    {0x1429A7729, 1, 5, kBroadcastFn, "EnableRespawnGazers' broadcast FUN_1422FA510", kGRespawn},
    {kSkyFn, 3, 7, kWsGlobal, "SetStartControlFlags' weather global", kGSky},
};
// clang-format on
const char* GroupName(unsigned g) { return g == kGKill ? "kill look" : g == kGRespawn ? "respawn" : "sky"; }

// ================================================================ state
int g_kill_look = 0, g_respawn = 0, g_sky = 0;  // session.ini (Install)
bool g_checked = false;
unsigned g_groups = 0;
std::atomic<bool> g_stopped{false};  // EmergencyStop
std::atomic<bool> g_pass{false};     // the detour straight through

// the log budget, one per part
X::LineBudget g_b_kill, g_b_resp, g_b_sky, g_b_misc;
void Line(X::LineBudget& b, double now_ms, const char* fmt, ...) {
    int dropped = 0;
    if (!b.Take(now_ms, &dropped)) return;
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (dropped) Log("[bt] extra: %s (%d lines dropped before)", buf, dropped);  // the text first: the gates match its start
    else Log("[bt] extra: %s", buf);
}
void Put(char* msg, std::size_t cap, const char* fmt, ...) {
    if (!msg || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
}
const char* YesNo(bool b) { return b ? "yes" : "no"; }

unsigned Groups() {
    if (g_checked) return g_groups;
    g_checked = true;
    unsigned groups = kAllGroups;
    int ok = 0, total = 0;
    for (const Site& s : kSites) {
        ++total;
        if (I::SiteOk(s.va, s.sig)) ++ok;
        else groups &= ~s.group, Log("[bt] extra: byte check FAILED: %s (%s off)", s.name, GroupName(s.group));
    }
    for (const Rip& r : kRips) {
        ++total;
        if (I::RipOk(r.insn, r.off, r.len, r.want)) ++ok;
        else groups &= ~r.group, Log("[bt] extra: byte check FAILED: %s (%s off)", r.what, GroupName(r.group));
    }
    // the scene reads (+0x3A8) are bt_world's (kBitScene); the census's field id is bt_sync's (its kField / kActor groups)
    if (!(btworld::Check() & btworld::kBitScene)) {
        groups &= ~kGRespawn;
        Log("[bt] extra: byte check FAILED: bt_world's +0x3A8 scene read (respawn off)");
    }
    g_groups = groups;
    Log("[bt] extra: byte checks: %d/%d ok; kill look %s, respawn %s, sky %s", ok, total, (groups & kGKill) ? "ok" : "OFF",
        (groups & kGRespawn) ? "ok" : "OFF", (groups & kGSky) ? "ok" : "OFF");
    return groups;
}
bool AnyKey() { return g_kill_look || g_respawn || g_sky; }

// ================================================================ guarded game calls (game thread; POD only inside __try)
bool SehU8(uptr fn, u8 v) noexcept {  // FUN_142E1FED0(flag) SetStartControlFlags (bt_probe_r31.cpp's SehU8)
    __try {
        reinterpret_cast<void (*)(u8)>(fn)(v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehRespawn(uptr fn, uptr scene) noexcept {  // FUN_1429A76D0(scene) EnableRespawnGazers
    __try {
        reinterpret_cast<void (*)(void*)>(fn)(reinterpret_cast<void*>(scene));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The stats (bt_probe_r31.cpp's StatRead): [0x147BFD960] + 0x10, the value at +0x4F8 + id * 4 XOR its two masks.
bool StatRead(u32 id, u32* out) noexcept {
    uptr g = 0;
    if (!I::Read(I::Addr(kStatsGlobal), g) || !g) return false;
    const uptr t = g + 0x10;
    u32 raw = 0, k1 = 0, k2 = 0;
    if (!I::Read(t + 0x4F8 + id * 4, raw) || !I::Read(t + 0x8C8 + ((id >> 3) & 7) * 4, k1) || !I::Read(t + 0x8A8 + (id & 7) * 4, k2)) return false;
    *out = raw ^ k1 ^ k2;
    return true;
}
unsigned RdB(uptr a) noexcept {
    u8 v = 0;
    return I::Read(a, v) ? v : 0xFFu;
}
u32 RdU(uptr a) noexcept {
    u32 v = 0;
    return I::Read(a, v) ? v : 0xFFFFFFFFu;
}
u32 GuidHashAt(uptr a) noexcept {  // 0: unreadable or a zero GUID
    u8 g[16];
    if (!a || !I::ReadN(a, g, sizeof(g)) || R::GuidZero(g)) return 0;
    return R::GuidHash(g);
}

// ================================================================ the kill look
X::KillWindow g_kw;
u32 g_kw_s79 = 0, g_kw_s7a = 0;
bool g_kw_sok = false;
unsigned g_kl_applied = 0, g_kl_realized = 0;
int g_kl_last_d79 = 0, g_kl_last_d7a = 0;
bool g_kl_have_d = false;

void KillWindowEnd(double now_ms, const char* why) {
    if (!g_kw.open) return;
    u32 a79 = 0, a7a = 0;
    const bool ok = g_kw_sok && StatRead(0x79, &a79) && StatRead(0x7A, &a7a);
    const int d79 = X::StatDelta(g_kw_s79, a79), d7a = X::StatDelta(g_kw_s7a, a7a);
    if (ok) g_kl_last_d79 = d79, g_kl_last_d7a = d7a, g_kl_have_d = true;
    Line(g_b_kill, now_ms, "kill look (%s): %d applied (realized %d), stats d %s%d / %d (0x79 %u -> %u, 0x7A %u -> %u)", why, g_kw.applied, g_kw.realized,
         ok ? "" : "(unreadable) ", d79, d7a, g_kw_s79, a79, g_kw_s7a, a7a);
    g_kw.Close();
}
void KillTick(double now_ms) {
    if (g_kw.Due(now_ms)) KillWindowEnd(now_ms, g_kw.applied ? "5 s after the last applied kill" : "no applied kill in 15 s");
}

// ================================================================ RESPAWN
// the detour (any thread: atomics and guarded reads only)
using RespawnFnT = u64 (*)(u64, u64, u64, u64);
RespawnFnT g_orig = nullptr;
constexpr unsigned kCallSlots = 16;
std::atomic<u64> g_call[kCallSlots];
std::atomic<unsigned> g_call_lost{0}, g_call_seen{0};
u64 RespawnDetour(u64 scene, u64 b, u64 c, u64 d) {
    if (!g_pass.load(std::memory_order_relaxed) && scene) {
        g_call_seen.fetch_add(1, std::memory_order_relaxed);
        const u32 field = GuidHashAt(static_cast<uptr>(scene) + kSceneGuid);
        const u64 v = X::PackCall(field, field != 0);
        bool put = false;
        for (unsigned i = 0; i < kCallSlots && !put; ++i) {
            u64 want = 0;
            put = g_call[i].compare_exchange_strong(want, v, std::memory_order_acq_rel);
        }
        if (!put) g_call_lost.fetch_add(1, std::memory_order_relaxed);
    }
    return g_orig(scene, b, c, d);
}
bool g_hook_in = false, g_hook_failed = false;

struct PendResp {
    R::BtsRespawn m;
    double rx_ms = 0;
    const char* last_why = nullptr;
};
PendResp g_pend[X::kRespawnPending];
unsigned g_pend_n = 0;
struct After {
    bool on = false;
    double at_ms = 0;
    u32 field = 0;
} g_after;
X::FieldRing g_sent, g_applied;
unsigned g_tx = 0, g_rx = 0, g_ok = 0, g_refused = 0, g_natural = 0, g_apply_faults = 0;
bool g_apply_dead = false;  // the apply faulted once: never called again this run
bool g_test_real = false;   // bt_sync 11 2 1: act as a real-save guest (restrictive only)

void DrainCalls() {  // drop what the detour saw (a load, the link gone)
    for (unsigned i = 0; i < kCallSlots; ++i) g_call[i].store(0, std::memory_order_relaxed);
}

// The census gazers of a field and its scene here (the first that matches), and how many carry the respawn flag.
struct FieldScene {
    uptr scene = 0;
    int gazers = 0, flagged = 0;
};
I::GzInfo g_gz[160];
FieldScene FindScene(u32 field) {
    FieldScene f;
    const int n = I::Gazers(g_gz, 160);
    for (int i = 0; i < n; ++i) {
        const I::GzInfo& z = g_gz[i];
        if (z.field != field) continue;
        ++f.gazers;
        if (z.flags & X::kFlagRespawn) ++f.flagged;
        if (f.scene) continue;
        X::SceneSeen s;
        s.field = z.field;
        if (!btworld::SceneOwner(z.actor, &s.owner)) s.owner = 0;
        uptr loc = 0;
        if (I::Read(z.actor + kActLoc, loc) && loc) I::Read(loc + kLocScene, s.loc_scene);
        s.owner_hash = GuidHashAt(s.owner ? s.owner + kSceneGuid : 0);
        if (X::SceneMatches(s, field)) f.scene = s.owner;
    }
    return f;
}

bool SendRespawn(u32 field, double now_ms, u32* seq_out) {
    R::BtsRespawn m;
    m.scene = field, m.field = field;  // GetUUIDHashOfGazerScene's value is the field id (both crc32c of the scene GUID)
    m.sender_ms = static_cast<u32>(now_ms);
    u8 b[R::kBtsRespawnSize];
    const u32 seq = I::Seq();
    const std::size_t n = R::EncodeRespawn(seq, m, b, sizeof(b));
    if (!n || !I::SendBts(b, n)) return false;
    if (seq_out) *seq_out = seq;
    ++g_tx;
    g_sent.Add(field, now_ms);
    return true;
}
// The host's respawn-on (natural: the detour; test: bt_sync 11): logged, and sent when the rules let it.
bool RespawnOn(u32 field, bool guid_ok, double now_ms, const char* how, char* why_out, std::size_t why_cap) {
    const PeerHello p = I::Peer();
    X::RespawnSendIn in;
    in.bt_respawn = g_respawn, in.role = I::SessionRole(), in.sync_level = I::SyncLevel(), in.peer = I::PeerOn();
    in.peer_role = p.role, in.peer_level = p.level, in.peer_caps = p.caps, in.checks_ok = (Groups() & kGRespawn) != 0, in.field = guid_ok ? field : 0;
    in.since_last_ms = g_sent.Since(field, now_ms);
    const char* why = X::RespawnSendWhyNot(in);
    u32 seq = 0;
    if (!why && !SendRespawn(field, now_ms, &seq)) why = "the link did not take it";
    const char* name = guid_ok ? R::FieldName(field) : "?";
    if (why) Line(g_b_resp, now_ms, "respawn on (host): field %08X (%s): %s; not sent: %s", field, how, name, why);
    else
        Line(g_b_resp, now_ms, "respawn on (host): field %08X (%s): %s; sent to the guest (seq %u; she applies: %s)", field, how, name, seq,
             (p.caps & R::kCapWorldApply) ? "her cap 0x2000 yes" : p.real_save ? "no, her own real save" : "no cap 0x2000");
    if (why_out && why_cap) Put(why_out, why_cap, "%s", why ? why : "sent");
    return !why;
}

void HostTick(const TickCtx& c) {
    if (g_respawn < 1 || c.role != 1 || c.sync_level < 2) return;
    if (!g_hook_in && !g_hook_failed && !c.loading && (Groups() & kGRespawn)) {
        char m[160] = "";
        MH_STATUS s = MH_ERROR_NOT_INITIALIZED;
        if (I::MhInit(m, sizeof(m))) {
            void* target = reinterpret_cast<void*>(I::Addr(kRespawnFn));
            s = MH_CreateHook(target, reinterpret_cast<void*>(&RespawnDetour), reinterpret_cast<void**>(&g_orig));
            if (s == MH_OK) s = MH_EnableHook(target);
        }
        g_hook_in = s == MH_OK;
        g_hook_failed = !g_hook_in;
        if (g_hook_in) Log("[bt] extra: respawn hook in (EnableRespawnGazers FUN_1429A76D0, pass-through: each call is sent as RESPAWN)");
        else Log("[bt] extra: respawn hook FAILED: %s (respawn off: nothing is sent)", m[0] ? m : MH_StatusToString(s));
    }
    if (!g_hook_in) return;
    for (unsigned i = 0; i < kCallSlots; ++i) {
        const u64 v = g_call[i].exchange(0, std::memory_order_acq_rel);
        u32 field = 0;
        bool guid_ok = false;
        if (!X::UnpackCall(v, &field, &guid_ok)) continue;
        ++g_natural;
        RespawnOn(field, guid_ok, c.now_ms, "natural", nullptr, 0);
    }
    const unsigned lost = g_call_lost.exchange(0, std::memory_order_relaxed);
    if (lost) Line(g_b_resp, c.now_ms, "respawn: %u calls lost (the 16 slots were full)", lost);
}

void GuestTick(const TickCtx& c) {
    if (g_after.on && c.now_ms >= g_after.at_ms) {
        g_after.on = false;
        if (!c.loading) {
            const FieldScene f = FindScene(g_after.field);
            Line(g_b_resp, c.now_ms, "respawn after 2 s: field %08X: the respawn flag 0x4000000 on %d of %d gazers here", g_after.field, f.flagged, f.gazers);
        }
    }
    if (!g_pend_n) return;
    unsigned keep = 0;
    for (unsigned i = 0; i < g_pend_n; ++i) {
        PendResp p = g_pend[i];
        R::RespawnIn in;
        in.bt_respawn = g_respawn, in.role = c.role, in.sandboxed = c.saves_redirected && !c.real_save && !g_test_real;
        in.age_ms = c.now_ms - p.rx_ms, in.loading = c.loading, in.not_playing = c.playing_why;
        R::KillVerdict v{R::KillCall::refuse, nullptr};
        FieldScene f;
        const bool gate_first = in.bt_respawn < 1 || in.role != 2 || !in.sandboxed;  // refused before any game read
        if (!gate_first && !c.loading && !c.playing_why) {
            f = FindScene(p.m.field);
            in.scene_here = f.scene != 0;
        }
        v = R::RespawnApplyGate(in);
        if (v.call == R::KillCall::apply) {
            if (g_apply_dead) v = {R::KillCall::refuse, "the apply faulted before (not called again)"};
            else if (!(Groups() & kGRespawn)) v = {R::KillCall::refuse, "the respawn byte checks failed here"};
            else if (g_applied.Since(p.m.field, c.now_ms) < X::kRespawnDupMs) v = {R::KillCall::refuse, "applied for this field within 10 s"};
        }
        if (v.call == R::KillCall::apply) {
            if (!SehRespawn(I::Addr(kRespawnFn), f.scene)) {
                g_apply_dead = true, ++g_apply_faults, ++g_refused;
                Line(g_b_resp, c.now_ms, "respawn apply FAULTED: field %08X (EnableRespawnGazers on scene 0x%llX; the apply is off for this run)", p.m.field,
                     static_cast<unsigned long long>(f.scene));
                continue;
            }
            ++g_ok;
            g_applied.Add(p.m.field, c.now_ms);
            g_after.on = true, g_after.at_ms = c.now_ms + 2000.0, g_after.field = p.m.field;
            Line(g_b_resp, c.now_ms, "respawn applied: field %08X (EnableRespawnGazers 0x1429A76D0 on scene 0x%llX, %s; %d gazers of it here, the respawn "
                 "flag on %d before; %.1f s after it came)", p.m.field, static_cast<unsigned long long>(f.scene), R::FieldName(p.m.field), f.gazers, f.flagged,
                 in.age_ms / 1000.0);
            continue;
        }
        if (v.call == R::KillCall::hold) {
            if (p.last_why != v.why) {
                p.last_why = v.why;
                Line(g_b_resp, c.now_ms, "respawn held: field %08X: %s", p.m.field, v.why);
            }
            g_pend[keep++] = p;
            continue;
        }
        ++g_refused;
        Line(g_b_resp, c.now_ms, "respawn not applied: field %08X: %s%s", p.m.field, v.why ? v.why : "?",
             g_test_real && c.role == 2 && c.saves_redirected ? " (bt_sync 11 2: the test's real-save stand-in)" : "");
    }
    g_pend_n = keep;
}

void RespawnDrop(double now_ms, const char* why) {
    if (g_pend_n) Line(g_b_resp, now_ms, "respawn: %u pending dropped (%s)", g_pend_n, why);
    g_pend_n = 0;
    g_after.on = false;
    g_sent.Clear();
    g_applied.Clear();
    DrainCalls();
}

// ================================================================ SKY
struct SkyState {
    bool on = false;           // calling for the partner's trouble
    bool test = false;         // bt_sync 11 1
    double test_end_ms = 0, test_start_ms = 0, test_log_ms = 0;
    double next_ms = 0;
    unsigned calls = 0, test_calls = 0;
    bool check_after = false;  // the 1 s look after a stop
    double after_ms = 0;
    unsigned total_calls = 0, faults = 0;
} g_sky_s;
bool g_sky_dead = false;  // the call faulted once

void SkyLook(double now_ms, const char* what) {
    const uptr ws = I::WeatherSys();
    Line(g_b_sky, now_ms, "%s: 6508 %u 6518 0x%X", what, ws ? RdB(ws + kWs6508) : 0xFFu, ws ? RdU(ws + kWsSky) : 0u);
}
bool SkyCall(double now_ms) {
    if (g_sky_dead) return false;
    if (!SehU8(I::Addr(kSkyFn), 1)) {
        g_sky_dead = true, ++g_sky_s.faults;
        Line(g_b_sky, now_ms, "sky call FAULTED (SetStartControlFlags FUN_142E1FED0): the sky is off for this run");
        return false;
    }
    ++g_sky_s.total_calls;
    return true;
}
void SkyStop(double now_ms, const char* why, bool look) {
    if (g_sky_s.on) {
        g_sky_s.on = false;
        Line(g_b_sky, now_ms, "sky off (%s): %u calls (the bit's 0.2 s timer clears it)", why, g_sky_s.calls);
        if (look) g_sky_s.check_after = true, g_sky_s.after_ms = now_ms + 1000.0;
    }
    if (g_sky_s.test) {
        g_sky_s.test = false;
        const uptr ws = I::WeatherSys();
        Line(g_b_sky, now_ms, "sky test done (%s): %u calls, 6508 %u 6518 0x%X", why, g_sky_s.test_calls, ws ? RdB(ws + kWs6508) : 0xFFu,
             ws ? RdU(ws + kWsSky) : 0u);
        if (look) g_sky_s.check_after = true, g_sky_s.after_ms = now_ms + 1000.0;
    }
}
void SkyTick(const TickCtx& c) {
    if (g_sky_s.check_after && c.now_ms >= g_sky_s.after_ms) {
        g_sky_s.check_after = false;
        const uptr ws = I::WeatherSys();
        u32 v = 0;
        if (ws && !c.loading && I::Read(ws + kWsSky, v)) {
            // the spec's restore is "stop calling": the game's timer clears the bit. If it did not, our own bit is taken back here
            // (bt_probe_r31.cpp's SkyEnd write) and the line says so (a gate finding).
            const bool set = (v & 1u) != 0;
            const bool cleared = set && I::Write<u32>(ws + kWsSky, v & ~1u);
            Line(g_b_sky, c.now_ms, "sky after 1 s: 6508 %u 6518 0x%X%s", RdB(ws + kWs6508), v,
                 !set ? " (bit 0 cleared by the game)" : cleared ? " (bit 0 still set: cleared by the module)" : " (bit 0 still set: the clear FAILED)");
        }
    }
    if (g_sky < 1) return;
    X::SkyIn in;
    in.bt_sky = g_sky, in.checks_ok = (Groups() & kGSky) != 0 && !g_sky_dead, in.sync_level = c.sync_level, in.peer = c.peer;
    in.partner_trouble = c.partner_trouble, in.partner_ok = c.partner_ok, in.me_ok = c.me_ok;
    in.dist_m = c.partner_ok && c.me_ok ? R::Dist3(c.me, c.partner_pos) : -1;
    in.loading = c.loading, in.not_playing = c.playing_why, in.carried = c.carried, in.real_save = c.real_save, in.stopped = g_stopped.load();
    const char* why = X::SkyWhyNot(in);
    if (!why && !g_sky_s.on) {
        g_sky_s.on = true, g_sky_s.calls = 0, g_sky_s.next_ms = 0;
        const uptr ws = I::WeatherSys();
        Line(g_b_sky, c.now_ms, "sky on: the partner's %s %.0f m away (6508 %u 6518 0x%X)", R::TroubleName(c.partner_trouble), in.dist_m,
             ws ? RdB(ws + kWs6508) : 0xFFu, ws ? RdU(ws + kWsSky) : 0u);
    } else if (why && g_sky_s.on) {
        SkyStop(c.now_ms, why, !c.loading);
    }
    // the test: the same call for its seconds, whatever the partner does (still never while loading or on a real-save guest)
    if (g_sky_s.test) {
        if (c.loading) SkyStop(c.now_ms, "a load", false);
        else if (c.now_ms >= g_sky_s.test_end_ms) SkyStop(c.now_ms, "time", true);
        else if (c.now_ms >= g_sky_s.test_log_ms) {
            g_sky_s.test_log_ms = c.now_ms + 2000.0;  // every 2 s: on, 4 looks, done and after fit one budget window
            char what[32];
            std::snprintf(what, sizeof(what), "sky +%.0f", (c.now_ms - g_sky_s.test_start_ms) / 1000.0);
            SkyLook(c.now_ms, what);
        }
    }
    if ((g_sky_s.on || g_sky_s.test) && X::SkyDue(c.now_ms, g_sky_s.next_ms)) {
        g_sky_s.next_ms = c.now_ms + X::kSkyPeriodMs;
        if (SkyCall(c.now_ms)) {
            if (g_sky_s.on) ++g_sky_s.calls;
            if (g_sky_s.test) ++g_sky_s.test_calls;
        } else {
            SkyStop(c.now_ms, "the call faulted", false);
        }
    }
}

}  // namespace

// ================================================================ the interface
void Install(const Config& config) noexcept {
    g_kill_look = config.bt_kill_look >= 1 ? 1 : 0;
    g_respawn = config.bt_respawn >= 1 ? 1 : 0;
    g_sky = config.bt_sky >= 1 ? 1 : 0;
    for (auto& s : g_call) s.store(0, std::memory_order_relaxed);
}

void OnHello(const PeerHello& peer) noexcept {
    if (g_stopped.load()) return;
    if (!AnyKey()) return;
    const unsigned g = Groups();
    const int role = I::SessionRole();
    Log("[bt] extra: partner hello: kill look %s, respawn %s (%s; the partner's cap 0x2000 %s%s), sky %s",
        !g_kill_look ? "off" : (g & kGKill) ? "on (0x5414860D)" : "OFF (byte checks)",
        !g_respawn ? "off" : !(g & kGRespawn) ? "OFF (byte checks)" : role == 1 ? "on" : role == 2 ? (I::RealSave() ? "refused here (real save)" : "on") : "off",
        role == 1 ? "the host sends" : role == 2 ? "the guest applies" : "not a two-game role", YesNo((peer.caps & R::kCapWorldApply) != 0),
        peer.real_save ? ", her own real save" : "", !g_sky ? "off" : (g & kGSky) ? "on (built off by default)" : "OFF (byte checks)");
}

void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept {
    if (type != R::kBtsRespawn || g_stopped.load()) return;
    R::BtsRespawn m;
    if (!R::DecodeRespawn(msg, n, m)) {
        Line(g_b_resp, now_ms, "a bad RESPAWN (%u B): dropped", static_cast<unsigned>(n));
        return;
    }
    ++g_rx;
    Line(g_b_resp, now_ms, "RESPAWN received: field %08X %s (scene hash %08X%s), host ms %u", m.field, R::FieldName(m.field), m.scene,
         m.scene == m.field ? "" : ", NOT the field id", m.sender_ms);
    for (unsigned i = 0; i < g_pend_n; ++i)
        if (g_pend[i].m.field == m.field) return;  // one pending per field
    if (g_pend_n >= X::kRespawnPending) {
        ++g_refused;
        Line(g_b_resp, now_ms, "respawn not applied: field %08X: %u pending already", m.field, X::kRespawnPending);
        return;
    }
    PendResp p;
    p.m = m, p.rx_ms = now_ms;
    g_pend[g_pend_n++] = p;
}

void Tick(const TickCtx& ctx) noexcept {
    if (g_stopped.load() || !AnyKey()) return;
    KillTick(ctx.now_ms);
    if (g_respawn) {
        if (ctx.role == 1) HostTick(ctx);
        else if (ctx.role == 2) GuestTick(ctx);
    }
    SkyTick(ctx);
}

void OnWorldEvent(WorldEvent e, double now_ms) noexcept {
    if (g_stopped.load() || !AnyKey()) return;
    if (e == WorldEvent::load_end) return;
    const char* why = WorldEventName(e);
    // the kill look's window: the stats of the kills before the edge are still this world's (a load: they are reloaded, unread)
    if (g_kw.open) {
        if (e == WorldEvent::load_start) {
            Line(g_b_kill, now_ms, "kill look window dropped (%s): %d applied, stats not read", why, g_kw.applied);
            g_kw.Close();
        } else {
            KillWindowEnd(now_ms, why);
        }
    }
    RespawnDrop(now_ms, why);
    SkyStop(now_ms, why, false);
    g_sky_s.check_after = false;
}

void EmergencyStop() noexcept {
    // any thread, no later Tick: only atomics here. The detour goes straight through; the sky stops being called (the game's 0.2 s
    // timer clears its bit: nothing to write); pending respawns and the kill window are never looked at again.
    g_pass.store(true);
    g_stopped.store(true);
}

std::uint16_t Caps() noexcept {
    if (g_stopped.load() || g_respawn < 1) return 0;
    if (I::SessionRole() != 2 || I::SyncLevel() < 2 || I::RealSave() || !automation::SavesRedirected()) return 0;
    return (Groups() & kGRespawn) ? static_cast<std::uint16_t>(R::kCapWorldApply) : 0;
}

void Status(char* out, std::size_t cap) noexcept {
    if (!out || !cap) return;
    if (!AnyKey()) {
        std::snprintf(out, cap, "extra: off (bt_kill_look=0 bt_respawn=0 bt_sky=0)");
        return;
    }
    const unsigned g = g_checked ? g_groups : kAllGroups;
    char kl[48], d[24] = "";
    if (g_kl_have_d) std::snprintf(d, sizeof(d), ", stats d %d/%d", g_kl_last_d79, g_kl_last_d7a);
    std::snprintf(kl, sizeof(kl), "%s", !g_kill_look ? "off" : (g & kGKill) ? "on" : "OFF");
    std::snprintf(out, cap, "extra%s: kill look %s (%u applied, %u realized%s); respawn %s (tx %u rx %u ok %u no %u nat %u%s); sky %s (%u calls)",
                  g_stopped.load() ? " (stopped)" : "", kl, g_kl_applied, g_kl_realized, d,
                  !g_respawn ? "off" : !(g & kGRespawn) ? "OFF" : g_hook_in ? "hooked" : "on", g_tx, g_rx, g_ok, g_refused, g_natural,
                  g_apply_faults ? ", FAULTED" : "", !g_sky ? "off" : (g & kGSky) && !g_sky_dead ? (g_sky_s.on ? "calling" : "on") : "OFF",
                  g_sky_s.total_calls);
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (op != 11) return Put(msg, cap, "bt sync %d: not bt_extra's op", op), false;
    if (g_stopped.load()) return Put(msg, cap, "bt sync 11: stopped (emergency stop)"), false;
    if (!automation::Enabled()) return Put(msg, cap, "bt sync 11: hidden runs only"), false;
    const int sub = X::TestSubOf(v[1]);
    const double now = I::NowMs();
    if (sub == X::kSubRealSave) {
        g_test_real = v[2] != 0;
        Line(g_b_misc, now, "test: this game %s a real-save guest for the RESPAWN gate (bt_sync 11 2 %d)", g_test_real ? "acts as" : "no longer acts as",
             g_test_real ? 1 : 0);
        return Put(msg, cap, "bt sync 11: the real-save stand-in %s", g_test_real ? "on" : "off"), true;
    }
    if (sub == X::kSubSky) {
        const double s = X::SkySeconds(v[2]);
        if (s < 0) return Put(msg, cap, "bt sync 11 1: seconds 1..120 (0 = 10)"), false;
        if (g_sky < 1) return Put(msg, cap, "bt sync 11 1: bt_sky is 0 here"), false;
        if (!(Groups() & kGSky) || g_sky_dead) return Put(msg, cap, "bt sync 11 1: the sky byte checks failed (log)"), false;
        if (I::RealSave() && !X::kSkyRealSaveOk) return Put(msg, cap, "bt sync 11 1: refused on a real-save guest"), false;
        if (I::LoadingNow()) return Put(msg, cap, "bt sync 11 1: loading"), false;
        if (g_sky_s.test) return Put(msg, cap, "bt sync 11 1: already on"), false;
        if (!I::WeatherSys()) return Put(msg, cap, "bt sync 11 1: no weather system"), false;
        g_sky_s.test = true, g_sky_s.test_calls = 0, g_sky_s.test_start_ms = now, g_sky_s.test_end_ms = now + s * 1000.0;
        g_sky_s.test_log_ms = now + 2000.0, g_sky_s.next_ms = 0, g_sky_s.check_after = false;
        char what[48];
        std::snprintf(what, sizeof(what), "sky test on for %.0f s", s);
        SkyLook(now, what);
        return Put(msg, cap, "bt sync 11 1: the sky call for %.0f s (log)", s), true;
    }
    if (sub != X::kSubRespawn) return Put(msg, cap, "bt sync 11: <sub> 0 test RESPAWN [field], 1 sky [s], 2 real-save stand-in 0|1"), false;
    u32 want = 0;
    if (!X::FieldArg(v[2], &want)) return Put(msg, cap, "bt sync 11 0: the field id is a u32 (0 = the nearest gazer's)"), false;
    if (g_respawn < 1) return Put(msg, cap, "bt sync 11 0: bt_respawn is 0 here"), false;
    if (I::SessionRole() != 1) return Put(msg, cap, "bt sync 11 0: the host's test (this game is not the host)"), false;
    if (!(Groups() & kGRespawn)) return Put(msg, cap, "bt sync 11 0: the respawn byte checks failed (log)"), false;
    if (I::LoadingNow()) return Put(msg, cap, "bt sync 11 0: loading"), false;
    if (!want) {
        double me[3];
        const bool me_ok = I::LocalPos(me);
        const int n = I::Gazers(g_gz, 160);
        double best = 1e18;
        for (int i = 0; i < n; ++i) {
            const I::GzInfo& z = g_gz[i];
            if (!z.field) continue;
            const double d = me_ok && z.pos_ok ? R::Dist3(me, z.pos) : z.pdist >= 0 ? z.pdist : 1e17;
            if (d < best) best = d, want = z.field;
        }
        if (!want) return Put(msg, cap, "bt sync 11 0: no gazer with a field here"), false;
    }
    const FieldScene f = FindScene(want);
    if (!f.scene) return Put(msg, cap, "bt sync 11 0: field %08X: no scene of it here (%d gazers)", want, f.gazers), false;
    char why[96] = "";
    const bool sent = RespawnOn(want, true, now, "test, bt_sync 11", why, sizeof(why));
    return Put(msg, cap, "bt sync 11 0: field %08X %s: %s", want, R::FieldName(want), why), sent;
}

bool KillLookOn() noexcept {
    return g_kill_look && X::KillLookReason(g_kill_look, (Groups() & kGKill) != 0, I::RealSave(), g_stopped.load()) != 0;
}

std::uint32_t KillLookReason() noexcept {
    if (!g_kill_look) return 0;
    const u32 r = X::KillLookReason(g_kill_look, (Groups() & kGKill) != 0, I::RealSave(), g_stopped.load());
    if (r && !g_kw.open) {
        const double now = I::NowMs();
        g_kw.Arm(now);
        g_kw_sok = StatRead(0x79, &g_kw_s79) && StatRead(0x7A, &g_kw_s7a);
        Line(g_b_kill, now, "kill look armed: reason 0x%08X (stats 0x79 %u 0x7A %u%s)", r, g_kw_s79, g_kw_s7a, g_kw_sok ? "" : ", unreadable");
    }
    return r;
}

void AfterUpdate(std::uintptr_t) noexcept {}

void OnGazerEvent(const btworld::Event& e, double now_ms) noexcept {
    if (g_stopped.load() || !g_kill_look || e.tag != R::kTagMirror || e.applied == btworld::kNotArmed) return;
    if (e.applied != R::kKillOk) {
        if (g_kw.open) Line(g_b_kill, now_ms, "kill look: an armed kill was not applied in the update (%d)", e.applied);
        return;
    }
    if (e.reason != X::kKillLookReason) return;  // not ours (the kill look was off when it was armed)
    std::int32_t state = -1;
    uptr ent = 0, loc = 0;
    I::Read(e.actor + kActState, state);
    I::Read(e.actor + kActEnt, ent);
    const u32 loc_hash = I::Read(e.actor + kActLoc, loc) && loc ? GuidHashAt(loc + kLocGuid) : 0;
    const bool realized = X::Realized(e.flags, state);
    ++g_kl_applied;
    if (realized) ++g_kl_realized;
    g_kw.Applied(now_ms, realized);
    Line(g_b_kill, now_ms, "kill applied: loc %08X (id %05X) reason 0x%08X killer %08X, flags 0x%X, realized %s (state %d, entity 0x%llX), death edge %s",
         loc_hash, e.id & 0x1FFFFFu, e.reason, e.killer, e.flags, YesNo(realized), state, static_cast<unsigned long long>(ent), YesNo(e.edge));
}

}  // namespace st::bt::extra
