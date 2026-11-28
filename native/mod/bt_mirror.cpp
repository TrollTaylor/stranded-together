// Round 31 stage 5: GM, the gazer mirror (realized copies, 25-60 m) (bt_mirror.hpp; the round 31 build spec, Stage 5). The game
// calls and their byte checks are bt_probe_r31.cpp's ops 20 (the agitation read), 22 (MirrorAfter: the re-check, the writes, the
// write-back and its read-back) and 23 (the yaw through game::TrySetTransform), copied here as they were proven (rerun 2,
// 20261127-230816); this file never calls into the probe.
#include "bt_mirror.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "MinHook.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "automation.hpp"
#include "bt_mirror_rules.hpp"
#include "bt_rules.hpp"
#include "bt_sync_internal.hpp"
#include "config.hpp"
#include "game_actions.hpp"
#include "game_api.hpp"
#include "link.hpp"
#include "log.hpp"
#include "partner_source.hpp"

namespace st::bt::mirror {
namespace {

namespace R = st::btrules;
namespace M = st::btrules::mirror;
namespace I = st::bt::internal;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using uptr = std::uintptr_t;

// ================================================================ ds.exe DC addresses and offsets (bt_probe_r31.cpp's gz map)
constexpr uptr kGzActionFn = 0x1429C49D0, kGzMsgFn = 0x14299EFB0;  // the drop detours
constexpr uptr kActVt10Slot = 0x143DA5778, kActVt10Fn = 0x1422F8AA0;  // the gazer actor's vt+0x10 (the transform copy, op 23)
constexpr uptr kActId = 0x08, kActState = 0x24, kActRot = 0x40, kActEntRef = 0x68, kActVis = 0xB0, kActHealth = 0x180;
constexpr uptr kActMode = 0x1A8, kActFlags = 0x1B4, kActAction = 0x200, kActDist = 0x204, kActBlend = 0x20C, kActKind = 0x214;
constexpr uptr kCompBack = 0x110, kComp80 = 0x80, kComp7C = 0x7C, kCompGraph = 0x160, kCompVars = 0x198, kHmArea = 0x140;
constexpr u32 kFlagCaught = 0x2000000u;

// ================================================================ the byte checks (once, game thread)
enum Group : unsigned { kGCore = 1u, kGDrops = 2u };
struct Site {
    uptr va;
    const char* sig;
    const char* name;
    unsigned group;
};
// clang-format off
constexpr Site kSites[] = {
    // core: the agitation pair and its graph var (op 20 / 22)
    {0x1429D66C1, "C5 FA 10 47 7C", "the agitation comp+0x7C (0x1429D66C1)", kGCore},
    {0x1429D66E1, "44 38 AF 80 00 00 00", "the agitation comp+0x80 (0x1429D66E1)", kGCore},
    {0x1429D66EA, "48 8B 97 60 01 00 00", "the graph comp+0x160 (0x1429D66EA)", kGCore},
    {0x1429D66D6, "4C 8B 87 98 01 00 00", "the graph vars comp+0x198 (0x1429D66D6)", kGCore},
    {0x1429D66F6, "48 63 82 90 00 00 00", "the agitation index [+0x160]+0x90 (0x1429D66F6)", kGCore},
    {0x1429D6701, "49 8B 40 58 48 03 C9 C5 F8 2F C7 C5 FA 10 54 C8 20", "the agitation value [[+0x198]+0x58]+idx*0x30+0x20 (0x1429D6701)", kGCore},
    // drops: the AI action and message handlers and the cases they drop (op 21 / 22)
    {kGzActionFn, "4C 8B DC 56 57 48 81 EC B8 01 00 00 48 8B 05 4D C6 59 02 48 33 C4 48 89", "the AI action FUN_1429C49D0", kGDrops},
    {0x1429C4BC4, "3D 1D 4B 7E 5A", "the catch action case 0x5A7E4B1D (0x1429C4BC4)", kGDrops},
    {0x1429C4BCF, "3D CB E6 21 62", "the FOUND action case 0x6221E6CB (0x1429C4BCF)", kGDrops},
    {kGzMsgFn, "40 57 48 83 EC 60 48 8B 05 73 20 5C 02 48 33 C4 48 89 44 24 50 8B 42 10 48 8B F9", "the actor message FUN_14299EFB0", kGDrops},
    {0x14299F207, "3D 3B FB 2D 68 0F 84 BE 00 00 00", "the caught message case 0x682DFB3B (0x14299F207)", kGDrops},
};
// clang-format on

int g_key = 0;                       // bt_gazer_mirror (Install)
bool g_checked = false;              // game thread
unsigned g_groups = 0;
bool g_off = false;                  // core failed (or the gazer update hook refused): the feature is off, one log line
const char* g_off_why = nullptr;
std::atomic<bool> g_pass{false};     // EmergencyStop: every detour and AfterUpdate straight through
std::atomic<bool> g_dead{false};     // EmergencyStop ran: nothing more, ever
bool g_force_turn = false;           // bt_sync 13 1 (hidden runs): mirror as a real-save guest

u64 Q(uptr a) noexcept {
    u64 v = 0;
    return a && I::Read(a, v) ? v : 0;
}
float RdF(uptr a, float def = -1.0f) noexcept {
    float f = 0;
    return a && I::Read(a, f) && std::isfinite(f) ? f : def;
}
u32 RdU(uptr a, u32 def = 0xFFFFFFFFu) noexcept {
    u32 v = 0;
    return a && I::Read(a, v) ? v : def;
}
int RdI(uptr a, int def = -1) noexcept {
    std::int32_t v = 0;
    return a && I::Read(a, v) ? v : def;
}
unsigned RdB(uptr a, unsigned def = 0xFFu) noexcept {
    u8 v = 0;
    return a && I::Read(a, v) ? v : def;
}
unsigned long long H(uptr a) noexcept { return static_cast<unsigned long long>(a); }

unsigned Check() {
    if (g_checked) return g_groups;
    g_checked = true;
    unsigned groups = kGCore | kGDrops;
    int ok = 0, total = 0;
    for (const Site& s : kSites) {
        ++total;
        if (I::SiteOk(s.va, s.sig)) ++ok;
        else {
            if (groups & s.group)
                Log("[bt] mirror: byte check FAILED: %s: %s", s.name,
                    s.group == kGCore ? "the gazer mirror off" : "the drops off (every mirror turns only)");
            groups &= ~s.group;
        }
    }
    ++total;  // the actor's vt+0x10 (FUN_1422F8AA0: the transform copy into actor +0x28 / +0x40; op 23 read it back)
    if (Q(I::Addr(kActVt10Slot)) == I::Addr(kActVt10Fn)) ++ok;
    else {
        if (groups & kGCore) Log("[bt] mirror: byte check FAILED: the gazer actor's vt+0x10 0x143DA5778: the gazer mirror off");
        groups &= ~kGCore;
    }
    g_groups = groups;
    Log("[bt] mirror: byte checks %d/%d ok; core %s, drops %s", ok, total, (groups & kGCore) ? "ok" : "OFF", (groups & kGDrops) ? "ok" : "OFF");
    if (!(groups & kGCore)) g_off = true, g_off_why = "its byte checks failed";
    return groups;
}

// ================================================================ the per-mirror atomics (the detours and AfterUpdate read them)
// One record per mirror (index i, the same in g_m). Tick (the game thread) sets comp / ent / id / kind / agit, then actor (release);
// AfterUpdate (the game thread, inside the gazer update) and the drop detours (any thread) read them. The drop pair (d_*) outlives
// the writes through the write-back check; the restore set (r_*) is what EmergencyStop writes back, synchronously.
struct Slot {
    std::atomic<uptr> actor{0}, comp{0}, ent{0};  // the writes (actor 0: none)
    std::atomic<u32> id{0};
    std::atomic<int> kind{-1};                    // the kind byte to write, -1 none
    std::atomic<bool> agit{false};                // the agitation pair this frame
    std::atomic<int> lost{0};                     // AfterUpdate's stale code (the next Tick releases the mirror)
    std::atomic<unsigned> writes{0};
    std::atomic<uptr> d_actor{0}, d_comp{0};      // the drops (0: none)
    std::atomic<u32> d_id{0};
    std::atomic<bool> r_due{false};               // the agitation pair was written: EmergencyStop writes the own pair back
    std::atomic<uptr> r_actor{0}, r_comp{0}, r_ent{0};
    std::atomic<u32> r_id{0}, r_own7C{0};
    std::atomic<unsigned> r_own80{0};
};
Slot g_slot[M::kMax];
std::atomic<unsigned> g_drop_found{0}, g_drop_catch{0}, g_drop_caught{0};

// The re-check on values read now (guarded reads; a freed block reads garbage, never faults). 0 = still good.
int StaleNow(uptr actor, uptr comp, uptr ent, u32 want_id) noexcept {
    u32 id = 0;
    const bool id_ok = actor && I::Read(actor + kActId, id);
    const int state = RdI(actor + kActState);
    const uptr ref = Q(actor + kActEntRef);
    const uptr ent_now = ref > 0x20 ? ref - 0x20 : 0;
    return M::StaleCheck(id_ok, id, want_id, state, ent_now, ent, comp ? Q(comp + kCompBack) : 0, actor);
}

// ================================================================ the drop detours (any thread: atomics and guarded reads only)
using FnU = u64 (*)(u64, u64, u64, u64);
FnU g_o_action = nullptr, g_o_msg = nullptr;
bool g_drops_tried = false, g_drops_in = false;  // game thread

// DSGazerComponent MsgDsAiAction FUN_1429C49D0(comp, msg): the action hash at msg+0x10. Dropped only for a mirrored component whose
// back pointer is still the mirrored actor with the mirrored id (a freed component's address reused by another gazer passes).
u64 ActionDetour(u64 comp, u64 msg, u64 c, u64 d) {
    if (!g_pass.load(std::memory_order_relaxed) && comp) {
        for (Slot& s : g_slot) {
            if (s.d_comp.load(std::memory_order_acquire) != comp) continue;
            u32 hash = 0, id = 0;
            const uptr actor = s.d_actor.load(std::memory_order_relaxed), back = Q(static_cast<uptr>(comp) + kCompBack);
            if (I::Read(static_cast<uptr>(msg) + 0x10, hash) && M::DropAction(hash) && actor && back > 0xE8 && back - 0xE8 == actor &&
                I::Read(actor + kActId, id) && id == s.d_id.load(std::memory_order_relaxed)) {
                (hash == R::kGzActionFound ? g_drop_found : g_drop_catch).fetch_add(1, std::memory_order_relaxed);
                return 0;  // dropped (a void handler)
            }
            break;
        }
    }
    return g_o_action(comp, msg, c, d);
}
// The gazer actor's message handler FUN_14299EFB0(actor, msg): the message hash at msg+0x10.
u64 MsgDetour(u64 actor, u64 msg, u64 c, u64 d) {
    if (!g_pass.load(std::memory_order_relaxed) && actor) {
        for (Slot& s : g_slot) {
            if (s.d_actor.load(std::memory_order_acquire) != actor) continue;
            u32 hash = 0, id = 0;
            if (I::Read(static_cast<uptr>(msg) + 0x10, hash) && M::DropMessage(hash) && I::Read(static_cast<uptr>(actor) + kActId, id) &&
                id == s.d_id.load(std::memory_order_relaxed)) {
                g_drop_caught.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            break;
        }
    }
    return g_o_msg(actor, msg, c, d);
}

// Game thread, once: both drop hooks in, or neither (then every mirror turns only).
bool DropsReady() {
    if (g_drops_tried) return g_drops_in;
    g_drops_tried = true;
    if (!(Check() & kGDrops)) return false;
    char msg[160] = "";
    if (!I::MhInit(msg, sizeof(msg))) {
        Log("[bt] mirror: drop hooks not in (MinHook: %s): every mirror turns only", msg);
        return false;
    }
    // review (crash 1): the byte check again, right here (Check ran once, at the first Tick: a bt_probe 16 / 21 / 22 hook made since
    // then changed these bytes)
    for (const Site& st : kSites) {
        if (st.group != kGDrops || I::SiteOk(st.va, st.sig)) continue;
        Log("[bt] mirror: byte check FAILED at the hook: %s: drop hooks not in, every mirror turns only", st.name);
        return false;
    }
    void* ta = reinterpret_cast<void*>(I::Addr(kGzActionFn));
    void* tm = reinterpret_cast<void*>(I::Addr(kGzMsgFn));
    // review (crash 1): a hook is removed only when this module created it (MH_ERROR_ALREADY_CREATED: bt_probe holds that target;
    // removing it would free the probe's trampoline under the probe)
    auto hook = [](void* t, void* detour, FnU* orig, const char* name, const char* tail) -> bool {
        MH_STATUS s = MH_CreateHook(t, detour, reinterpret_cast<void**>(orig));
        if (s != MH_OK) {
            Log("[bt] mirror: drop hook %s not in (%s%s): %s", name, MH_StatusToString(s),
                s == MH_ERROR_ALREADY_CREATED ? ", bt_probe holds it: left alone" : "", tail);
            return false;
        }
        s = MH_EnableHook(t);
        if (s != MH_OK) {
            MH_RemoveHook(t);  // created just now by this module
            Log("[bt] mirror: drop hook %s not in (%s): %s", name, MH_StatusToString(s), tail);
            return false;
        }
        return true;
    };
    if (!hook(ta, reinterpret_cast<void*>(&ActionDetour), &g_o_action, "FUN_1429C49D0", "every mirror turns only")) return false;
    if (!hook(tm, reinterpret_cast<void*>(&MsgDetour), &g_o_msg, "FUN_14299EFB0", "both out, every mirror turns only")) {
        MH_DisableHook(ta);  // this module's own hook on ta (created and enabled above)
        MH_RemoveHook(ta);
        return false;
    }
    Log("[bt] mirror: drop hooks in: FUN_1429C49D0 (FOUND 0x6221E6CB, catch 0x5A7E4B1D) and FUN_14299EFB0 (caught 0x682DFB3B), dropped for "
        "mirrored gazers only (0x68A02F9F and the rest pass)");
    return g_drops_in = true;
}

// ================================================================ the game-thread state
// One record per mirror (index i, the same as g_slot[i]): a mirror (on) or, after its release, its write-back check (back).
struct Mir {
    bool used = false;   // the record is taken: a mirror or its write-back check
    bool on = false;     // mirroring
    bool back = false;   // released: the agitation write-back is read back once a second (the drops stay on)
    u32 loc = 0, field = 0, id = 0;
    uptr actor = 0, comp = 0, ent = 0;
    bool turn_only = false;
    const char* turn_why = nullptr;
    unsigned own80 = 0;
    float own7C = 0;
    bool agit_now = false, agit_written = false;
    unsigned kind = 0;  // the partner's kind byte (last applied)
    float trig = -1;
    double start_ms = 0, next_line_ms = 0, yaw_ms = 0, back_ms = 0;
    int back_tries = 0;
    unsigned yaw_writes = 0, yaw_fails = 0;
    bool turn_logged = false, gameover = false;
};
Mir g_m[M::kMax];

struct Rx {  // the latest GZST2 entry per field and locator
    bool used = false;
    R::Gz2Entry e;
    double ms = 0;
};
constexpr unsigned kRxCap = 16;
Rx g_rx[kRxCap];

struct HeldLine {  // the "held" lines, per locator (a change of reason, or every 10 s)
    u32 loc = 0;
    const char* why = nullptr;
    double ms = -1e18;
};
HeldLine g_held[8];

struct Sent {  // the sender's last bits per locator (the edges), and when an edge went up (kept 1 s)
    u32 loc = 0;
    u8 bits = 0, edges = 0;
    double edge_ms = -1e18, seen_ms = -1e18;
    bool used = false;
    bool sending = false;  // in the last GZST2 (the "sending" / "not sent" lines on a change)
};
Sent g_sent[16];

int g_role = 0, g_level = 0;  // config (Install): 1 host, 2 guest; bt_sync
bool g_peer = false;
R::LineBudget g_budget;  // every line of this module: at most 16 a second
unsigned g_tx = 0, g_rx_n = 0, g_rx_bad = 0, g_started = 0, g_refused = 0;
double g_gate_ms = -1e18, g_drop_line_ms = 0;
unsigned g_drop_logged = 0;
u32 g_last_loc = 0;
const char* g_last_why = nullptr;
constexpr int kGzCap = 160;
I::GzInfo g_gz[kGzCap];

template <class... A>
void Lg(const char* fmt, A... a) {
    if (g_budget.Take(I::NowMs())) Log(fmt, a...);
}

float Nan() noexcept { return std::numeric_limits<float>::quiet_NaN(); }
uptr GzEntity(uptr actor) noexcept {
    const uptr ref = Q(actor + kActEntRef);
    return ref > 0x20 ? ref - 0x20 : 0;
}
uptr GzComp(uptr ent) noexcept {
    return ent ? partner::FindComponentByName(reinterpret_cast<game::Entity*>(ent), "DSGazerComponent") : 0;
}
// bt_probe op 20's read (review semantics 3): [comp+0x160]+0x90 is an int32 INDEX (movsxd at 0x1429D66F6); the value is the float
// [[comp+0x198]+0x58] + idx*0x30 + 0x20 (0x1429D66D6, 0x1429D6701). -1.0f when the index is outside 0..255 or unreadable.
float GzAgit(uptr comp, int* idx) noexcept {
    *idx = -1;
    const uptr graph = comp ? Q(comp + kCompGraph) : 0;
    std::int32_t i = -1;
    if (!graph || !I::Read(graph + 0x90, i)) return -1.0f;
    *idx = i;
    if (i < 0 || i > 255) return -1.0f;
    const uptr vars = Q(comp + kCompVars);
    const uptr data = vars ? Q(vars + 0x58) : 0;
    float v = -1.0f;
    if (!data || !I::Read(data + static_cast<uptr>(i) * 0x30 + 0x20, v) || !std::isfinite(v)) return -1.0f;
    return v;
}
u64 GameState() noexcept {
    u64 st = 0;
    u32 load = 0;
    return game::ReadStopGates(&st, &load) ? st : ~0ull;
}
bool GameOver(u64 st) noexcept { return st != ~0ull && (st & (1ull << 13)); }
uptr HmArea() noexcept {
    const uptr hm = I::HunterMgr();
    return hm ? Q(hm + kHmArea) : 0;
}
const char* YesNo(bool b) noexcept { return b ? "yes" : "no"; }

void ClearDrops(Slot& s) noexcept {
    s.d_actor.store(0, std::memory_order_release);
    s.d_comp.store(0, std::memory_order_release);
    s.d_id.store(0);
}
bool WritePair(uptr comp, unsigned v80, float v7C) noexcept {
    const bool a = I::Write<u8>(comp + kComp80, static_cast<u8>(v80));
    const bool b = I::Write<float>(comp + kComp7C, v7C);
    return a && b;
}
void SyncAfter() noexcept {
    bool any = false;
    for (const Slot& s : g_slot) any = any || s.actor.load() != 0;
    btworld::SetAfterUpdate(btworld::kAfterMirror, any ? &AfterUpdate : nullptr);
}
unsigned UsedN() noexcept {
    unsigned n = 0;
    for (const Mir& m : g_m) n += m.used ? 1u : 0u;
    return n;
}
int FindRec(u32 field, u32 loc) noexcept {
    for (unsigned i = 0; i < M::kMax; ++i)
        if (g_m[i].used && g_m[i].loc == loc && g_m[i].field == field) return static_cast<int>(i);
    return -1;
}
int FreeRec() noexcept {
    for (unsigned i = 0; i < M::kMax; ++i)
        if (!g_m[i].used) return static_cast<int>(i);
    return -1;
}
Rx* FindRx(u32 field, u32 loc) noexcept {
    for (Rx& r : g_rx)
        if (r.used && r.e.loc == loc && r.e.field == field) return &r;
    return nullptr;
}
const I::GzInfo* FindGz(int n, u32 field, u32 loc) noexcept {
    for (int i = 0; i < n; ++i)
        if (g_gz[i].loc == loc && g_gz[i].field == field) return &g_gz[i];
    return nullptr;
}

// ================================================================ the release, the write-back and its read-back (op 22's)
enum EndMode { kEndCheck, kEndNow, kEndNoWrite };  // write back + check later; write back, drops off now; no write (a load)
void End(unsigned i, const char* why, EndMode mode, double now) {
    Mir& m = g_m[i];
    Slot& s = g_slot[i];
    if (!m.used) return;
    if (m.back) {  // a write-back check pending
        if (mode == kEndCheck) return;  // it ends on its own (the drops stay on until the pair reads back)
        m.back = false, m.used = false;
        ClearDrops(s);
        Lg("[bt] mirror: back loc %08X: not checked (%s)", m.loc, why);
        return;
    }
    if (!m.on) return void(m.used = false);
    m.on = false;
    s.actor.store(0, std::memory_order_release);  // no more writes (AfterUpdate and this run on the game thread)
    s.agit.store(false), s.kind.store(-1);
    SyncAfter();
    const int w = mode == kEndNoWrite ? -1 : StaleNow(m.actor, m.comp, m.ent, m.id);
    bool wrote = false;
    if (m.agit_written && w == 0) wrote = WritePair(m.comp, m.own80, m.own7C);
    s.r_due.store(false);
    const bool check = mode == kEndCheck && m.agit_written && w == 0;
    if (check) m.back = true, m.back_tries = 0, m.back_ms = now + 1000.0;  // the drops stay on
    else m.used = false, ClearDrops(s);
    if (GameOver(GameState())) m.gameover = true;
    const char* pair = !m.agit_written ? "the pair never written"
                       : wrote         ? "own pair written back"
                       : w < 0         ? "not written back (a load frees the gazer)"
                       : w > 0         ? M::StaleText(w)
                                       : "the write-back FAILED";
    Lg("[bt] mirror: off: loc %08X (%s): %.0f s, writes %u, yaw writes %u, drops %u/%u/%u, GameOver %s; %s (own +0x80 %u +0x7C %.2f)", m.loc, why,
       (now - m.start_ms) / 1000.0, s.writes.load(), m.yaw_writes, g_drop_found.load(), g_drop_catch.load(), g_drop_caught.load(), YesNo(m.gameover),
       pair, m.own80, m.own7C);
    if (m.agit_written && !check) Lg("[bt] mirror: back loc %08X: not checked (%s)", m.loc, w > 0 ? M::StaleText(w) : why);
}
// Once a second after the release, up to 5 s: did +0x80 / +0x7C go back to the gazer's own values? The drops end here.
void Back(unsigned i, double now) {
    Mir& m = g_m[i];
    if (!m.back || now < m.back_ms) return;
    const int w = StaleNow(m.actor, m.comp, m.ent, m.id);
    const unsigned b80 = w ? 0xFFu : RdB(m.comp + kComp80);
    const float b7C = w ? -1.0f : RdF(m.comp + kComp7C);
    const bool ok = !w && M::BackOk(m.own80, m.own7C, b80, b7C);
    ++m.back_tries;
    if (!w && !ok && m.back_tries < M::kBackTries) return void(m.back_ms = now + 1000.0);
    m.back = false, m.used = false;
    ClearDrops(g_slot[i]);
    Lg("[bt] mirror: back loc %08X after %d s: +0x80 %u +0x7C %.2f vs own %u %.2f: %s", m.loc, m.back_tries, b80, b7C, m.own80, m.own7C,
       w ? M::StaleText(w) : ok ? "back yes" : "back NO");
}
unsigned EndAll(const char* why, EndMode mode, double now) {
    unsigned n = 0;
    for (unsigned i = 0; i < M::kMax; ++i)
        if (g_m[i].used) ++n, End(i, why, mode, now);
    return n;
}

// ================================================================ the start and the writes
// What a mirror writes from now on (WritesFor). The pair off again: the gazer's own values written back on a re-checked component.
void ApplyWrites(unsigned i, int role, const R::Gz2Entry& e) {
    Mir& m = g_m[i];
    Slot& s = g_slot[i];
    const M::Writes w = M::WritesFor(m.turn_only, role, e.agit);
    s.kind.store(w.kind ? static_cast<int>(e.kind) : -1);
    m.kind = e.kind;
    if (w.agit && !m.agit_now) {
        s.r_due.store(true);  // EmergencyStop writes the own pair back from here on
        s.agit.store(true);
        m.agit_now = true, m.agit_written = true;
    } else if (!w.agit && m.agit_now) {
        s.agit.store(false);
        m.agit_now = false;
        const int st = StaleNow(m.actor, m.comp, m.ent, m.id);
        const bool wrote = !st && WritePair(m.comp, m.own80, m.own7C);
        s.r_due.store(false);
        Lg("[bt] mirror: calm loc %08X: the partner's agitation %.2f: %s", m.loc, e.agit / 255.0, wrote ? "own pair written back" : st ? M::StaleText(st) : "the write-back FAILED");
    }
}

bool Start(const TickCtx& c, const I::GzInfo& z, const R::Gz2Entry& e, uptr ent, uptr comp, bool turn_only, const char* turn_why, double now) {
    const int at = FreeRec();
    if (at < 0) return false;
    const unsigned i = static_cast<unsigned>(at);
    auto refuse = [&](const char* why) {
        ++g_refused;
        g_last_loc = z.loc, g_last_why = why;
        Lg("[bt] mirror: refused: loc %08X: %s", z.loc, why);
        return false;
    };
    if (!turn_only) {
        char msg[160] = "";
        if (!I::InstallGz(msg, sizeof(msg))) {
            g_off = true, g_off_why = "bt_world's gazer update hook is not in";
            Lg("[bt] mirror: the gazer update hook is not in (%s): the gazer mirror off", msg);
            return false;
        }
    }
    if (const int w = StaleNow(z.actor, comp, ent, z.id)) return refuse(M::StaleText(w));
    u8 own80 = 0;
    float own7C = 0;
    if (!I::Read(comp + kComp80, own80) || !I::Read(comp + kComp7C, own7C) || !std::isfinite(own7C)) return refuse("its +0x80 / +0x7C are unreadable");
    Mir& m = g_m[i];
    m = Mir{};
    m.used = true, m.on = true, m.loc = z.loc, m.field = z.field, m.id = z.id, m.actor = z.actor, m.comp = comp, m.ent = ent;
    m.turn_only = turn_only, m.turn_why = turn_why, m.own80 = own80, m.own7C = own7C, m.trig = z.trig;
    m.start_ms = now, m.next_line_ms = now;
    Slot& s = g_slot[i];
    s.actor.store(0), s.lost.store(0), s.writes.store(0), s.kind.store(-1), s.agit.store(false), s.r_due.store(false);
    s.id.store(z.id), s.ent.store(ent), s.comp.store(comp);
    u32 bits7C = 0;
    std::memcpy(&bits7C, &own7C, sizeof(bits7C));
    s.r_id.store(z.id), s.r_ent.store(ent), s.r_comp.store(comp), s.r_own80.store(own80), s.r_own7C.store(bits7C), s.r_actor.store(z.actor);
    if (!turn_only) {
        s.d_id.store(z.id), s.d_comp.store(comp);
        s.d_actor.store(z.actor, std::memory_order_release);  // the drops on
        s.actor.store(z.actor, std::memory_order_release);    // the writes on (last)
        SyncAfter();
    }
    ApplyWrites(i, c.role, e);
    ++g_started;
    char tail[96] = "";
    if (turn_only && !c.real_save && !g_force_turn) std::snprintf(tail, sizeof(tail), " (turn only: %s)", turn_why ? turn_why : "?");
    else if (turn_only) std::snprintf(tail, sizeof(tail), "%s", c.real_save ? " (real save: turn only)" : " (real save: turn only) (rehearsed: bt_sync 13 1)");
    Lg("[bt] mirror: on: loc %08X kind %u agit %.2f (field %08X %s), you %.1f m from it (trigger %.1f m), own +0x80 %u +0x7C %.2f%s", z.loc,
       static_cast<unsigned>(e.kind), e.agit / 255.0, z.field, R::FieldName(z.field), z.pdist, z.trig, static_cast<unsigned>(own80), own7C, tail);
    return true;
}

// ================================================================ the yaw (op 23's call: game::TrySetTransform, the game thread)
void StaleEnd(unsigned i, int w, double now) {
    char why[128];
    std::snprintf(why, sizeof(why), "not realized here any more: %s", M::StaleText(w));
    End(i, why, kEndCheck, now);
}
void Yaw(unsigned i, const TickCtx& c, double now) {
    Mir& m = g_m[i];
    if (!m.on || !c.partner_ok || c.loading || c.playing_why) return;
    if (m.yaw_ms > 0 && now - m.yaw_ms < M::kYawMinMs) return;
    if (const int w = StaleNow(m.actor, m.comp, m.ent, m.id)) return StaleEnd(i, w, now);
    game::Entity* ent = reinterpret_cast<game::Entity*>(m.ent);
    game::WorldTransform wt{};
    if (!game::TryGetTransform(ent, &wt)) {
        ++m.yaw_fails, m.yaw_ms = now;
        return;
    }
    const double from[3] = {wt.pos.x, wt.pos.y, wt.pos.z};
    double want = 0;
    if (!M::YawToward(from, c.partner_pos, &want)) return void(m.yaw_ms = now);
    const double cur = M::RowYaw(wt.rot.m), lim = R::kGzYawRateRad * M::YawDt(now, m.yaw_ms);
    const double step = std::clamp(M::YawDiff(cur, want), -lim, lim);
    m.yaw_ms = now;
    if (std::fabs(step) < M::kYawEpsRad) return;
    M::RotateRows(wt.rot.m, step);
    if (!game::TrySetTransform(ent, wt)) return void(++m.yaw_fails);
    ++m.yaw_writes;
    if (m.turn_logged) return;
    m.turn_logged = true;
    const double dx = c.partner_pos[0] - from[0], dy = c.partner_pos[1] - from[1];
    Lg("[bt] mirror: turn loc %08X: yaw %.1f -> %.1f (toward the partner's body at %.1f, %.1f m away; at most 90 deg/s)", m.loc, cur * M::kDeg,
       (cur + step) * M::kDeg, want * M::kDeg, std::sqrt(dx * dx + dy * dy));
}

// ================================================================ the lines
void Line(unsigned i, double now) {
    Mir& m = g_m[i];
    if (!m.on || now < m.next_line_ms) return;
    m.next_line_ms = M::NextLineMs(m.start_ms, now);
    const uptr a = m.actor;
    const int w = StaleNow(a, m.comp, m.ent, m.id);
    int gi = -1;
    const float agit = w ? -1.0f : GzAgit(m.comp, &gi);
    const u64 st = GameState();
    if (GameOver(st)) m.gameover = true;
    Lg("[bt] mirror: +%.0f loc %08X: flags 0x%X kind %u agit %.3f (idx %d) (+0x80 %u +0x7C %.1f) vis %.2f dist %.1f, drops %u/%u/%u, writes %u, "
       "yaw writes %u, hm140 0x%llX, state %d, game 0x%llX%s",
       (now - m.start_ms) / 1000.0, m.loc, RdU(a + kActFlags), RdB(a + kActKind), agit, gi, w ? 0xFFu : RdB(m.comp + kComp80),
       w ? -1.0f : RdF(m.comp + kComp7C), RdF(a + kActVis), RdF(a + kActDist), g_drop_found.load(), g_drop_catch.load(), g_drop_caught.load(),
       g_slot[i].writes.load(), m.yaw_writes, H(HmArea()), RdI(a + kActState), static_cast<unsigned long long>(st), m.gameover ? ", GameOver seen" : "");
}
void DropLine(double now) {
    const unsigned f = g_drop_found.load(), k = g_drop_catch.load(), c = g_drop_caught.load();
    if (f + k + c == g_drop_logged || now < g_drop_line_ms) return;
    g_drop_logged = f + k + c, g_drop_line_ms = now + 10000.0;
    Lg("[bt] mirror: dropped FOUND %u, catch %u, caught %u (mirrored gazers only)", f, k, c);
}
void NoteHeld(u32 loc, const char* why, double now) {
    HeldLine* h = nullptr;
    for (HeldLine& x : g_held)
        if (x.loc == loc) h = &x;
    if (!h)
        for (HeldLine& x : g_held)
            if (!h || x.ms < h->ms) h = &x;
    if (h->loc == loc && h->why == why && now - h->ms < 10000.0) return;
    h->loc = loc, h->why = why, h->ms = now;
    Lg("[bt] mirror: held: loc %08X %s", loc, why);
}

// ================================================================ the sender (reads only)
Sent* SentFor(u32 loc, bool insert, double now) {
    Sent* old = nullptr;
    for (Sent& s : g_sent) {
        if (s.used && s.loc == loc) return &s;
        if (!old || !s.used || (old->used && s.seen_ms < old->seen_ms)) old = &s;
    }
    if (!insert || !old) return nullptr;
    *old = Sent{};
    old->used = true, old->loc = loc, old->seen_ms = now;
    return old;
}
void Send(const TickCtx& c, double now, int n) {
    if ((c.role != 1 && c.role != 2) || c.sync_level < 1 || !c.peer || !(c.partner.caps & R::kCapGzst2) || c.loading || !c.send || !c.seq) return;
    int idx[kGzCap];
    u8 eff[kGzCap];
    int k = 0;
    for (int j = 0; j < n; ++j) {
        const I::GzInfo& z = g_gz[j];
        const u8 raw = R::GzBits(z.flags, z.state);
        // the reach (bt_mirror_rules.hpp SenderInReach): a NEAR bit left set after Sam went away is not sent
        double own = -1;
        if (z.pos_ok && c.me_ok) {
            const double dx = z.pos[0] - c.me[0], dy = z.pos[1] - c.me[1], dz = z.pos[2] - c.me[2];
            own = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        const bool reach = M::SenderInReach(own, z.pdist, z.trig);
        const u8 bits = eff[j] = M::SenderBits(raw, reach);
        const bool wants = M::SenderWants(bits);
        if (Sent* s = SentFor(z.loc, wants || M::SenderWants(raw), now)) {
            const u8 e = M::SenderEdges(s->bits, bits);
            if (e) s->edges = static_cast<u8>(s->edges | e), s->edge_ms = now;
            if (now - s->edge_ms > 1000.0) s->edges = 0;
            s->bits = bits, s->seen_ms = now;
            const bool sending = wants && z.pos_ok;
            if (sending != s->sending) {
                s->sending = sending;
                if (sending)
                    Lg("[bt] mirror: sending loc %08X (field %08X %s): flags 0x%X, you %.1f m from it (game %.1f m, reach %.1f m)", z.loc, z.field,
                       R::FieldName(z.field), z.flags, own, z.pdist, M::SenderReachM(z.trig));
                else
                    Lg("[bt] mirror: not sent: loc %08X: %s (flags 0x%X, you %.1f m from it, game %.1f m, reach %.1f m)", z.loc,
                       !reach && M::SenderWants(raw) ? "out of reach (its sensing bit is still set)" : !z.pos_ok ? "its position is unreadable" : "it no longer senses you",
                       z.flags, own, z.pdist, M::SenderReachM(z.trig));
            }
        }
        if (wants && z.pos_ok) idx[k++] = j;
    }
    if (!k) return;
    std::sort(idx, idx + k, [](int a, int b) {
        const float da = g_gz[a].pdist >= 0 ? g_gz[a].pdist : 1e9f, db = g_gz[b].pdist >= 0 ? g_gz[b].pdist : 1e9f;
        return da < db;
    });
    R::BtsGzst2 msg;
    msg.sender_ms = static_cast<u32>(static_cast<u64>(now));
    for (int t = 0; t < k && msg.n < R::kBtsGzst2Max; ++t) {
        const I::GzInfo& z = g_gz[idx[t]];
        const uptr a = z.actor;
        R::Gz2Entry& e = msg.e[msg.n];
        e = R::Gz2Entry{};
        e.field = z.field, e.loc = z.loc, e.state = static_cast<u8>(std::clamp(z.state, 0, 255));
        e.mode = static_cast<u8>(RdB(a + kActMode, 0)), e.bits = eff[idx[t]], e.kind = static_cast<u8>(RdB(a + kActKind, 0));
        e.action = RdU(a + kActAction, 0), e.blend = R::Unit8(RdF(a + kActBlend, 0));
        const uptr ent = z.state == 2 ? GzEntity(a) : 0;
        const uptr comp = ent ? GzComp(ent) : 0;
        int gi = -1;
        const float graph = comp ? GzAgit(comp, &gi) : -1.0f;
        e.agit = R::Unit8(M::SenderAgit(e.bits, graph >= 0, graph));
        const Sent* s = SentFor(z.loc, false, now);
        e.edges = s ? s->edges : 0;
        for (int q = 0; q < 3; ++q) e.pos[q] = static_cast<float>(z.pos[q]);
        game::WorldTransform wt{};
        double yaw = 0;
        if (ent && game::TryGetTransform(reinterpret_cast<game::Entity*>(ent), &wt)) yaw = M::RowYaw(wt.rot.m);
        else yaw = std::atan2(static_cast<double>(RdF(a + kActRot + 16, 0)), static_cast<double>(RdF(a + kActRot + 12, 0)));
        e.yaw = R::YawCdeg(yaw), e.dist_dm = R::DistDm(z.pdist);
        if (R::Gz2EntryValid(e)) ++msg.n;
    }
    if (!msg.n) return;
    u8 buf[R::kBtsMaxSize];
    const std::size_t len = R::EncodeGzst2(c.seq(), msg, buf, sizeof(buf));
    if (len && c.send(buf, len)) ++g_tx;
}

// ================================================================ the gate (4 Hz)
M::GateIn Base(const TickCtx& c, double now, const Rx* rx, bool real) {
    M::GateIn in;
    in.key = g_key, in.sync_level = c.sync_level, in.emergency = g_pass.load();
    in.partner_bits = rx ? rx->e.bits : 0, in.since_ms = rx ? now - rx->ms : 1e18;
    in.loading = c.loading, in.not_playing = c.playing_why, in.real_save = real, in.mirrored_n = UsedN();
    return in;
}
void Here(M::GateIn& in, const I::GzInfo& z) {
    in.resolved = true, in.state = z.state;
    in.dead = M::DeadHere(z.flags, RdF(z.actor + kActHealth, Nan()));
    in.caught_here = (z.flags & kFlagCaught) != 0;
    in.local_dist_m = z.pdist, in.trigger_m = z.trig;
}
void GatePass(const TickCtx& c, double now) {
    const int got = I::Gazers(g_gz, kGzCap);
    const int n = got < 0 ? 0 : got;
    Send(c, now, n);
    const bool real = c.real_save || g_force_turn;
    for (unsigned i = 0; i < M::kMax; ++i) {  // the mirrored
        Mir& m = g_m[i];
        if (!m.on) continue;
        const Rx* rx = FindRx(m.field, m.loc);
        const I::GzInfo* z = FindGz(n, m.field, m.loc);
        M::GateIn in = Base(c, now, rx, real);
        in.mirrored = true, in.drops_ok = g_drops_in;
        if (z && z->actor == m.actor && z->id == m.id) Here(in, *z), in.comp_ok = StaleNow(m.actor, m.comp, m.ent, m.id) == 0;
        const M::GateOut o = M::Gate(in);
        if (o.call != R::GzCall::mirror) {
            End(i, o.why ? o.why : "the gate", kEndCheck, now);
            continue;
        }
        if (o.turn_only != m.turn_only) {
            End(i, o.turn_only ? "turn only from now on (the bt_sync 13 rehearsal)" : "writes allowed from now on", kEndCheck, now);
            continue;
        }
        if (rx) ApplyWrites(i, c.role, rx->e);
    }
    for (Rx& r : g_rx) {  // the candidates
        if (!r.used || FindRec(r.e.field, r.e.loc) >= 0) continue;
        if (now - r.ms > 10000.0) {
            r.used = false;
            continue;
        }
        const I::GzInfo* z = FindGz(n, r.e.field, r.e.loc);
        M::GateIn in = Base(c, now, &r, real);
        in.drops_ok = g_drops_in || (!g_drops_tried && !real);  // before the first try: the hooks go in on the first mirror
        uptr ent = 0, comp = 0;
        if (z) {
            Here(in, *z);
            if (z->state == 2) ent = GzEntity(z->actor), comp = GzComp(ent);
            in.comp_ok = comp != 0;
        }
        const M::GateOut o = M::Gate(in);
        if (o.call == R::GzCall::mirror && z) {
            bool turn = real;
            const char* why = c.real_save ? "real save" : g_force_turn ? "the bt_sync 13 rehearsal" : nullptr;
            if (!turn && !DropsReady()) turn = true, why = "the drop hooks are not in";
            Start(c, *z, r.e, ent, comp, turn, why, now);
        } else if (o.held) {
            NoteHeld(r.e.loc, o.why, now);
        } else {
            g_last_loc = r.e.loc, g_last_why = o.why;
        }
    }
}

void ResetTables() {
    for (Rx& r : g_rx) r = Rx{};
    for (Sent& s : g_sent) s = Sent{};
    for (HeldLine& h : g_held) h = HeldLine{};
}

}  // namespace

// ================================================================ the interface
void Install(const Config& config) noexcept {
    g_key = config.bt_gazer_mirror >= 1 ? 1 : 0;
    g_role = config.role == Role::host ? 1 : config.role == Role::guest ? 2 : 0;
    g_level = config.bt_sync;
    if (g_key < 1) return;
    if (!g_role || g_level < 1) {
        g_key = 0;
        Log("[bt] mirror: off (bt_gazer_mirror=1, but not a two-game session or bt_sync=0)");
        return;
    }
    Log("[bt] mirror: GM on (bt_gazer_mirror=1, bt_sync %d): GZST2 sent and taken once the partner's hello offers cap 0x800; mirrors%s", g_level,
        g_level >= 2 ? " realized copies 25-60 m from this game's player" : " nothing (bt_sync 1 applies nothing)");
}

void OnHello(const PeerHello& peer) noexcept {
    if (g_key < 1 || g_dead.load()) return;
    g_peer = true;
    Lg("[bt] mirror: partner hello: %s", (peer.caps & R::kCapGzst2) ? "GZST2 offered (cap 0x800): this game's sensing gazers go out at 4 Hz"
                                                                    : "no GZST2 (cap 0x800 missing): nothing sent, nothing mirrored");
}

void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept {
    if (g_key < 1 || g_dead.load() || type != R::kBtsGzst2) return;
    R::BtsGzst2 m;
    if (!R::DecodeGzst2(msg, n, m)) return void(++g_rx_bad);
    ++g_rx_n;
    for (unsigned i = 0; i < m.n; ++i) {
        const R::Gz2Entry& e = m.e[i];
        Rx* r = FindRx(e.field, e.loc);
        for (Rx& x : g_rx)  // a free entry, else the oldest one no record uses (16 entries, at most 6 records)
            if (!r && !x.used) r = &x;
        if (!r)
            for (Rx& x : g_rx)
                if (FindRec(x.e.field, x.e.loc) < 0 && (!r || x.ms < r->ms)) r = &x;
        if (!r) continue;
        r->used = true, r->e = e, r->ms = now_ms;
    }
}

void Tick(const TickCtx& c) noexcept {
    if (g_key < 1 || g_dead.load() || (c.role != 1 && c.role != 2)) return;
    const double now = c.now_ms;
    if (!g_checked) Check();
    for (unsigned i = 0; i < M::kMax; ++i) {
        Mir& m = g_m[i];
        if (m.on) {
            if (const int w = g_slot[i].lost.exchange(0)) {
                StaleEnd(i, w, now);
            } else if (g_off) {
                End(i, g_off_why ? g_off_why : "the gazer mirror is off", kEndCheck, now);
            } else {
                Yaw(i, c, now);
                Line(i, now);
            }
        }
        if (m.back) Back(i, now);
    }
    DropLine(now);
    if (now - g_gate_ms < M::kGateMs) return;
    g_gate_ms = now;
    if (!g_off) GatePass(c, now);
}

void OnWorldEvent(WorldEvent e, double now_ms) noexcept {
    if (g_key < 1 || g_dead.load()) return;
    switch (e) {
        case WorldEvent::load_start: {
            const unsigned n = EndAll("a load", kEndNoWrite, now_ms);
            ResetTables();
            if (n) Lg("[bt] mirror: load: %u mirrors taken back (nothing written: the load frees the gazers)", n);
            break;
        }
        case WorldEvent::link_lost:
        case WorldEvent::partner_gone: {
            const char* why = e == WorldEvent::link_lost ? "link lost" : "the partner is gone";
            const unsigned n = EndAll(why, kEndCheck, now_ms);
            ResetTables();
            if (e == WorldEvent::link_lost) g_peer = false;
            if (n) Lg("[bt] mirror: %s: %u mirrors taken back", why, n);
            break;
        }
        case WorldEvent::load_end: break;
    }
}

void OnGazerEvent(const btworld::Event& e, double now_ms) noexcept {
    if (g_key < 1 || g_dead.load() || !e.edge) return;
    for (unsigned i = 0; i < M::kMax; ++i)
        if (g_m[i].on && g_m[i].actor == e.actor && g_m[i].id == e.id) End(i, "dead here", kEndCheck, now_ms);
}

// Inside bt_world's detour, the game thread, every gazer updated: the re-check, then kind and the agitation pair. No logging.
void AfterUpdate(std::uintptr_t actor) noexcept {
    if (!actor || g_pass.load(std::memory_order_relaxed)) return;
    for (Slot& s : g_slot) {
        if (s.actor.load(std::memory_order_acquire) != actor) continue;
        const uptr comp = s.comp.load(std::memory_order_relaxed);
        if (const int w = StaleNow(actor, comp, s.ent.load(std::memory_order_relaxed), s.id.load(std::memory_order_relaxed))) {
            s.actor.store(0), s.agit.store(false), s.r_due.store(false);  // no more writes: the component may be freed
            ClearDrops(s);                                                 // the drops for that actor end
            s.lost.store(w);                                               // the next Tick releases the mirror
            return;
        }
        const int k = s.kind.load(std::memory_order_relaxed);
        if (k >= 0) I::Write<u8>(actor + kActKind, static_cast<u8>(k));
        if (s.agit.load(std::memory_order_relaxed)) WritePair(comp, M::kAgitOwn80, M::kAgitOwn7C);
        s.writes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

// Any thread, synchronously (no later Tick comes): the detours straight through, the saved pairs written back on re-checked components.
void EmergencyStop() noexcept {
    g_pass.store(true);
    g_dead.store(true);
    for (Slot& s : g_slot) s.actor.store(0), s.agit.store(false), s.kind.store(-1);
    btworld::SetAfterUpdate(btworld::kAfterMirror, nullptr);
    unsigned back = 0, stale = 0;
    for (Slot& s : g_slot) {
        if (s.r_due.exchange(false)) {
            const uptr comp = s.r_comp.load();
            float own7C = 0;
            const u32 bits = s.r_own7C.load();
            std::memcpy(&own7C, &bits, sizeof(own7C));
            if (!StaleNow(s.r_actor.load(), comp, s.r_ent.load(), s.r_id.load()) && WritePair(comp, s.r_own80.load(), own7C)) ++back;
            else ++stale;
        }
        ClearDrops(s);
    }
    if (g_key >= 1) Log("[bt] mirror: emergency stop: detours through, %u agitation pairs written back (%u stale, not written)", back, stale);
}

std::uint16_t Caps() noexcept {
    if (g_key < 1 || g_dead.load() || I::Stopped() || g_off) return 0;
    return (Check() & kGCore) ? static_cast<std::uint16_t>(R::kCapGzst2) : 0;
}

void Status(char* out, std::size_t cap) noexcept {
    if (!out || !cap) return;
    if (g_key < 1) return void(std::snprintf(out, cap, "mirror: off (bt_gazer_mirror=0, or not a two-game session)"));
    unsigned on = 0, back = 0, turn = 0;
    for (const Mir& m : g_m) on += m.on ? 1u : 0u, back += m.back ? 1u : 0u, turn += (m.on && m.turn_only) ? 1u : 0u;
    std::snprintf(out, cap, "mirror: %s%s; on %u (turn only %u) back %u; got %u bad %u, sent %u, started %u refused %u, drops %u/%u/%u; last %08X %s",
                  g_off ? "OFF: " : "", g_off ? (g_off_why ? g_off_why : "?") : (g_drops_in ? "drops in" : "drops not in"), on, turn, back, g_rx_n,
                  g_rx_bad, g_tx, g_started, g_refused, g_drop_found.load(), g_drop_catch.load(), g_drop_caught.load(), g_last_loc,
                  g_last_why ? g_last_why : "-");
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    auto put = [&](const char* text) {
        if (msg && cap) std::snprintf(msg, cap, "%s", text);
    };
    if (op != 13) return put("bt sync: not bt_mirror's op"), false;
    if (g_dead.load()) return put("bt sync 13: stopped (emergency stop)"), false;
    if (!automation::Enabled()) return put("bt sync 13: hidden runs only"), false;
    if (g_key < 1) return put("bt sync 13: off (bt_gazer_mirror=0, or not a two-game session)"), false;
    const double m = v[1];
    if (m == 0) {
        char line[kStatusCap];
        Status(line, sizeof(line));
        Lg("[bt] mirror: status (bt_sync 13): %s", line);
        return put("bt sync 13: status (log)"), true;
    }
    if (m == 1 || m == 2) {
        g_force_turn = m == 1;
        Lg("[bt] mirror: test: %s", g_force_turn ? "this game mirrors as a real-save guest would (the yaw only) until bt_sync 13 2" : "back to normal");
        return put(g_force_turn ? "bt sync 13: turn only (as a real-save guest)" : "bt sync 13: normal"), true;
    }
    return put("bt sync 13: 0 status, 1 turn only (as a real-save guest), 2 normal"), false;
}

}  // namespace st::bt::mirror
