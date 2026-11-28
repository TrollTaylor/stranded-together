// Round 31 stage 4: TF: the partner's hunt felt here as tar (list 0) (bt_tar.hpp; design 2.4; build spec stage 4; probe-findings 13).
// While the partner's HUNT is UP and its centre is within 300 m of this game's Sam, one record of ours (g_rec, in this DLL) sits
// in the hunter manager's list 0 (hm+0xC0 count, hm+0xD0 ptr[10], under the SRW lock hm+0x130): the player locomotion then gives
// Sam the tar feel (+0x844) inside it. Nothing else: no hunt (hm+0x140), no hands, no pull, no gazer fact (list 1 is never used).
// The list edits are probe 26's (bt_probe_r31.cpp SehTarList: add at the end if count < 10, remove by pointer search with the
// later entries moved down), done here with the guarded reads and writes (ReadProcessMemory / WriteProcessMemory) while the lock
// is held inside SEH; no game call is made while the lock is held. The record is taken out on: CLEAR, 3 s without HUNT, a load,
// a ride (Sam carried), the link lost, the partner gone, a real-save guest, EmergencyStop (synchronous, any thread) or bt_tar=0.
#include "bt_tar.hpp"

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
#include "bt_rules.hpp"
#include "bt_sync_internal.hpp"
#include "bt_tar_rules.hpp"
#include "config.hpp"
#include "game_actions.hpp"
#include "game_api.hpp"
#include "log.hpp"

namespace st::bt::tar {
namespace {

namespace R = btrules;
namespace T = btrules::tar;
namespace I = st::bt::internal;
using uptr = std::uintptr_t;
using u64 = std::uint64_t;

// ---- the hunter manager hm = [0x147BECBD0] (I::HunterMgr: bt_sync's kHunter group) and Sam's tar level (probe 26's offsets)
constexpr uptr kHmMeshN = 0xC0, kHmMesh = 0xD0, kHmLock = 0x130, kHmRes = 0x138, kHmArea = 0x140;
constexpr uptr kSamParam = 0x54C0, kParTarSm = 0x844, kParTar = 0x848;
// ---- the baggage manager (shot_sync.cpp's `shots 6` reads): DSBaggage2 (0x240 B) at +0x20 + index * 0x240
constexpr uptr kBagMgrGlobal = 0x147BF4968, kBagAddTemp = 0x142CD1380;
constexpr uptr kBagArray = 0x20, kBagStride = 0x240, kBagInUse = 0x21, kBagLife = 0x34, kBagPos = 0x38, kBagHolder = 0x90, kBagEntityIdx = 0xD8;
constexpr unsigned kBagMax = 8000, kBagChunk = 64;

// The byte checks: bt_probe_r31.cpp's tar group (kGTar) as it is. One failed site turns the tar feel off for the session.
struct Site {
    uptr va;
    const char* sig;
    const char* name;
};
constexpr Site kTarSites[] = {
    {0x1429BEEB4, "8B 83 C0 00 00 00 83 F8 0A 74 0E 48 89 AC C3 D0 00 00 00 FF 83 C0 00 00", "the tar list add hm+0xC0 / +0xD0 (0x1429BEEB4)"},
    {0x14280AFC2, "C5 FA 10 87 48 08 00 00 C4 C1 7A 5F CB C5 FA 11", "the tar max write +0x848 (cleared each update; 0x14280AFC2)"},
    {0x14280AFEF, "3A 5F 50 74 60 88 5F 50 84 DB", "the tar list non-empty byte +0x50 (0x14280AFEF)"},
    {0x14280AEC4, "C5 7A 10 2D C4 D0 37 01 C5 7A 10 1D C0 D0 37 01", "the tar inside test 0.5 / 1.0 loads (0x14280AEC4)"},
    {0x14280B054, "C5 FA 10 87 44 08 00 00 C5 FA 10 8F 48 08 00 00", "the tar smoothed +0x844 read (0x14280B054)"},
    {0x14280B0C7, "44 89 A7 48 08 00 00", "the tar max +0x848 clear (0x14280B0C7)"},
    {0x1429BE1F0, "40 55 48 83 EC 40 48 89 5C 24 58 48 8D A9 30 01 00 00 48 89 74 24 60 48", "the tar record reader FUN_1429BE1F0"},
    {0x1426EC610, "48 85 C9 0F 84 A0 00 00 00 57 48 83 EC 30 C5 F8 29 74 24 20 48 8B F9 48", "the in-tar call FUN_1426EC610"},
};
// shot_sync.cpp's AddBaggageToTemporaryList check and its RIP load of the baggage manager global (the cargo read only).
constexpr const char* kBagAddSig =
    "40 53 48 83 EC 20 48 8B 1D DB 35 F2 04 81 E1 FF FF 1F 00 8B C1 81 F9 FF FF 1F 00 74 30 25 FF 1F 00 00 48 8D 04 C0 48 C1 E0 06 "
    "80 7C 18 41 00 74 1C 48 8D 53 20 48 03 D0 74 13 48 8D 8B A8 DD 46 00";

// The record both lists read (probe 26): f64 pos[3], f32 radius (+0x18), f32 height (+0x1C); 0x20 bytes, aligned 16.
struct alignas(16) TarRec {
    double pos[3];
    float radius, height;
};
static_assert(sizeof(TarRec) == 0x20, "the tar record is 0x20 bytes");
TarRec g_rec{};
uptr RecPtr() noexcept { return reinterpret_cast<uptr>(&g_rec); }

int g_key = 0;                     // bt_tar (Install)
std::atomic<bool> g_stopped{false};  // EmergencyStop ran: nothing goes in again
std::atomic<bool> g_in{false};       // our record is in g_hm's list (as far as this module knows)
std::atomic<uptr> g_hm{0};           // the manager it went into
int g_checks = 0;                    // the tar sites: 0 not run, 1 ok, -1 failed (the feel off)
int g_cargo_checks = 0;              // the baggage manager: 0 not run, 1 ok, -1 failed (cargo d n/a)
bool g_dead = false;                 // a list edit faulted: the feel off for the session

R::HuntView g_view;           // the partner's HUNT (or the test's), as bt_hunt's view takes it
bool g_view_test = false;     // the last HUNT taken was bt_sync 12's
bool g_sim_real = false;      // bt_sync 12 0 1: this game counts as a real-save guest for the gate
bool g_me_ok = false;
double g_me[3] = {0, 0, 0};

struct Test {
    bool on = false;
    double start_ms = 0, next_ms = 0;
    float center[3] = {0, 0, 0};
    std::uint16_t episode = T::kTestEpisode0;
} g_test;

// The record's episode: what the record-out line reports (the gate's evidence).
struct Episode {
    std::uint16_t episode = 0;
    bool test = false;
    double in_ms = 0, sample_ms = 0, check_ms = 0, line_ms = 0;
    bool inside = false, logged_in = false;
    float max_in = -1, max_out = -1;
    int n_in = 0, n_out = 0;
    float health0 = -1;
    u64 game0 = ~0ull;
    T::CargoSnap cargo0;
    const char* cargo_na = nullptr;
    // gate run 20261128-020913: the cargo read across an episode in which a dev_goto moved Sam is not the tar's alone; the
    // record-out line says how far Sam went (the sum of the sampled steps) so a clean read (Sam still) can be told apart
    double moved_m = 0, last[3] = {0, 0, 0};
    bool have_last = false;
} g_ep;
// The real-save refusal of one episode (logged at its start and its end, with the cargo read across it).
struct Refusal {
    bool on = false;
    std::uint16_t episode = 0;
    double start_ms = 0;
    T::CargoSnap cargo0;
    const char* cargo_na = nullptr;
} g_ref;

double g_readd_ms = 0;
T::Why g_hold_logged = T::Why::none;  // the hold reason last logged (once per episode and reason)
std::uint16_t g_hold_episode = 0;
unsigned g_adds = 0, g_outs = 0, g_refresh = 0, g_rx = 0, g_rx_bad = 0, g_rx_test_skip = 0, g_faults = 0, g_game_outs = 0;
T::LineLimit g_lim_edge, g_lim_line, g_lim_misc;

const char* YesNo(bool b) noexcept { return b ? "yes" : "no"; }
unsigned long long H(uptr a) noexcept { return static_cast<unsigned long long>(a); }

// Every line starts "[bt] tar: " and goes through its channel's rate limit.
void Line(T::LineLimit& lim, double now_ms, const char* fmt, ...) {
    int dropped = 0;
    if (!lim.Allow(now_ms, &dropped)) return;
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (dropped) Log("[bt] tar: %s (%d earlier lines dropped by the rate limit)", buf, dropped);
    else Log("[bt] tar: %s", buf);
}
void Put(char* msg, std::size_t cap, const char* fmt, ...) {
    if (!msg || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
}

// ---- byte checks (game thread, once)
bool Checks(double now_ms) {
    if (g_checks) return g_checks > 0;
    int ok = 0;
    const int n = static_cast<int>(sizeof(kTarSites) / sizeof(kTarSites[0]));
    for (const Site& s : kTarSites) {
        if (I::SiteOk(s.va, s.sig)) ++ok;
        else Line(g_lim_misc, now_ms, "byte check FAILED: %s", s.name);
    }
    g_checks = ok == n ? 1 : -1;
    g_cargo_checks = I::SiteOk(kBagAddTemp, kBagAddSig) && I::RipOk(kBagAddTemp + 6, 3, 7, kBagMgrGlobal) ? 1 : -1;
    if (g_cargo_checks < 0) Line(g_lim_misc, now_ms, "byte check FAILED: the baggage manager global in AddBaggageToTemporaryList (the cargo reads say n/a; the feel is unaffected)");
    if (g_checks > 0) Line(g_lim_misc, now_ms, "byte checks %d/%d ok (list 0 hm+0xC0 / +0xD0 under hm+0x130; cargo read %s)", ok, n, g_cargo_checks > 0 ? "ok" : "off");
    else Line(g_lim_misc, now_ms, "off: %d/%d byte checks ok (the tar feel stays off this session)", ok, n);
    return g_checks > 0;
}

// ---- guarded reads (any thread)
bool HmSane(uptr hm) noexcept {  // the list, the lock and +0x138 readable, the count 0..10
    if (!hm) return false;
    std::uint8_t b[kHmRes + 8 - kHmMeshN];
    if (!I::ReadN(hm + kHmMeshN, b, sizeof(b))) return false;
    std::int32_t n = -1;
    std::memcpy(&n, b, sizeof(n));
    return n >= 0 && n <= R::kTarListMax;
}
bool HmReady(uptr hm) noexcept {
    uptr r = 0;
    return hm && I::Read(hm + kHmRes, r) && r;
}
int HmCount(uptr hm) noexcept {
    std::int32_t n = -1;
    return hm && I::Read(hm + kHmMeshN, n) ? n : -1;
}
uptr HmArea(uptr hm) noexcept {
    uptr a = 0;
    return hm && I::Read(hm + kHmArea, a) ? a : 0;
}
int HasOurs(uptr hm) noexcept {  // 1 our pointer is in the list, 0 not, -1 unreadable
    std::int32_t n = -1;
    uptr a[R::kTarListMax] = {};
    if (!hm || !I::Read(hm + kHmMeshN, n) || !I::ReadN(hm + kHmMesh, a, sizeof(a))) return -1;
    return T::ListHas(n, a, RecPtr());
}
// [Sam+0x54C0]: +0x844 the smoothed tar level (read this one), +0x848 the per-update max (cleared every update).
float SamTar(float* raw848) noexcept {
    game::Entity* e = nullptr;
    uptr p = 0;
    if (!game::TryGetLocalPlayer(&e) || !e || !I::Read(reinterpret_cast<uptr>(e) + kSamParam, p) || !p) {
        if (raw848) *raw848 = -1;
        return -1;
    }
    float v = -1, r = -1;
    if (!I::Read(p + kParTarSm, v) || !std::isfinite(v)) v = -1;
    if (raw848 && (!I::Read(p + kParTar, r) || !std::isfinite(r))) r = -1;
    if (raw848) *raw848 = r;
    return v;
}
float Health() noexcept {
    game::Diagnostics d;
    game::ReadDiagnostics(&d);
    return d.health_valid ? d.health : -1.0f;
}
u64 GameState() noexcept {
    u64 st = 0;
    std::uint32_t load = 0;
    return game::ReadStopGates(&st, &load) ? st : ~0ull;
}
// The pieces Sam carries now (the gate's cargo read). na: why there is no read.
T::CargoSnap CargoRead(const char** na) noexcept {
    T::CargoSnap s;
    *na = nullptr;
    if (g_cargo_checks <= 0) return *na = "the baggage manager's byte check", s;
    if (!g_me_ok) return *na = "no local Sam", s;
    uptr mgr = 0;
    if (!I::Read(I::Addr(kBagMgrGlobal), mgr) || !mgr) return *na = "no baggage manager", s;
    static std::uint8_t buf[kBagChunk * kBagStride];
    for (unsigned base = 0; base < kBagMax; base += kBagChunk) {
        const unsigned cnt = kBagMax - base < kBagChunk ? kBagMax - base : kBagChunk;
        if (!I::ReadN(mgr + kBagArray + kBagStride * base, buf, kBagStride * cnt)) continue;
        for (unsigned j = 0; j < cnt; ++j) {
            const std::uint8_t* b = buf + kBagStride * j;
            if (!b[kBagInUse]) continue;
            double p[3];
            std::int32_t eidx = 0;
            std::uint64_t holder = 0;
            float life = 0;
            std::memcpy(p, b + kBagPos, sizeof(p));
            std::memcpy(&eidx, b + kBagEntityIdx, sizeof(eidx));
            std::memcpy(&holder, b + kBagHolder, sizeof(holder));
            std::memcpy(&life, b + kBagLife, sizeof(life));
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
            if (T::CarriedPiece(b[kBagInUse], eidx, holder, R::Dist3(p, g_me))) s.Add(base + j, life);
        }
    }
    s.ok = true;
    return s;
}

// ---- the list edit (game thread; EmergencyStop: any thread, try_only). Probe 26's SehTarList with the guarded reads and writes:
// the count and the 10 pointers read, edited (bt_tar_rules.hpp ListAdd / ListRemove) and written back while hm+0x130 is held
// exclusively. No game call while it is held. 1 done, 2 already in (add), 0 not (full / not found / stopped), -1 faulted or
// unreadable, -2 the lock was busy (try_only).
int ListEditLocked(uptr hm, bool in, int* count_out) noexcept {
    std::int32_t n = -1;
    uptr a[R::kTarListMax] = {};
    if (!I::Read(hm + kHmMeshN, n) || !I::ReadN(hm + kHmMesh, a, sizeof(a))) return -1;
    int r = 0;
    if (in) {
        const int has = T::ListHas(n, a, RecPtr());
        r = has == 1 ? 2 : has < 0 || g_stopped.load() ? 0 : T::ListAdd(&n, a, RecPtr());
    } else {
        r = T::ListRemove(&n, a, RecPtr());
    }
    if (r == 1 && (!I::WriteN(hm + kHmMesh, a, sizeof(a)) || !I::Write<std::int32_t>(hm + kHmMeshN, n))) r = -1;
    *count_out = n;
    return r;
}
int TarListEdit(uptr hm, bool in, int* count_out, bool try_only) noexcept {
    int rc = -1;
    __try {
        SRWLOCK* lock = reinterpret_cast<SRWLOCK*>(hm + kHmLock);
        if (try_only) {
            if (!TryAcquireSRWLockExclusive(lock)) return -2;
        } else {
            AcquireSRWLockExclusive(lock);
        }
        __try {
            rc = ListEditLocked(hm, in, count_out);
        } __finally {
            ReleaseSRWLockExclusive(lock);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rc = -1;
    }
    return rc;
}
// Our record's new centre / radius while it is in the list (our own memory, written under the same lock the reader takes).
int TarRecSet(uptr hm, const TarRec& want) noexcept {
    int rc = -1;
    __try {
        SRWLOCK* lock = reinterpret_cast<SRWLOCK*>(hm + kHmLock);
        AcquireSRWLockExclusive(lock);
        __try {
            g_rec = want;
            rc = 1;
        } __finally {
            ReleaseSRWLockExclusive(lock);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rc = -1;
    }
    return rc;
}

const char* EditText(int rc) noexcept {
    switch (rc) {
        case 1: return "yes";
        case 0: return "no (not in the list)";
        case -2: return "no (the lock stayed busy; the record moved 100 km below the ground instead)";
        case -3: return "not touched (the hunter manager changed; the record moved 100 km below the ground)";
        default: return "FAULTED (the record moved 100 km below the ground)";
    }
}

// ---- the record in / out (game thread)
void FmtPos(char* out, std::size_t cap, const double* p) { std::snprintf(out, cap, "%.1f %.1f %.1f", p[0], p[1], p[2]); }

void RecordOut(T::Why why, double now_ms) {
    if (!g_in.load()) return;
    const uptr hm = g_hm.load(), cur = I::HunterMgr();
    int n = -1, rc = -3;
    // The manager it went into, or (during a load's teardown, HunterMgr() 0) that list while it still holds our pointer.
    if (hm && (cur == hm || (cur == 0 && HmSane(hm) && HasOurs(hm) == 1)) && HmSane(hm)) rc = TarListEdit(hm, false, &n, false);
    if (rc != 1 && rc != 0) g_rec.pos[2] = T::kNeutralZ;  // our own memory: never "inside" again, wherever the pointer is
    if (rc == -1) ++g_faults;
    g_in.store(false);
    ++g_outs;
    if (!T::EndsEpisode(why)) g_readd_ms = now_ms + T::kReaddMs;
    float raw = -1;
    const float tar = SamTar(&raw);
    const T::CargoSnap after = g_ep.cargo_na ? T::CargoSnap{} : CargoRead(&g_ep.cargo_na);
    const T::CargoDelta cd = T::CargoDiff(g_ep.cargo0, after);
    char cargo[96], worn[96];
    T::CargoText(cd, g_ep.cargo_na, cargo, sizeof(cargo));
    T::CargoWornText(cd, worn, sizeof(worn));
    Line(g_lim_edge, now_ms,
         "record out (%s): removed %s, count %d; %.1f s in; +0x844 max inside %.3f (%d samples), outside %.3f (%d samples), now %.3f; hm140 0x%llX; "
         "health %.1f -> %.1f; game 0x%llX -> 0x%llX; %s (episode %u%s)%s; Sam moved %.1f m while it was in",
         T::WhyText(why), EditText(rc), n, (now_ms - g_ep.in_ms) / 1000.0, g_ep.max_in, g_ep.n_in, g_ep.max_out, g_ep.n_out, tar,
         H(HmArea(cur ? cur : hm)), g_ep.health0, Health(), static_cast<unsigned long long>(g_ep.game0),
         static_cast<unsigned long long>(GameState()), cargo, g_ep.episode, g_ep.test ? ", test" : "", worn, g_ep.moved_m);
    if (rc == -1) {
        g_dead = true;
        Line(g_lim_misc, now_ms, "off: the list edit faulted (the tar feel stays off this session)");
    }
}

bool RecordIn(uptr hm, const double (&pos)[3], float radius, double now_ms) {
    if (!HmSane(hm)) {
        Line(g_lim_edge, now_ms, "not added: the hunter manager's list is unreadable (count %d)", HmCount(hm));
        g_readd_ms = now_ms + T::kReaddMs;
        return false;
    }
    if (HasOurs(hm) != 1) {  // not in any list: our memory is ours to write
        std::memcpy(g_rec.pos, pos, sizeof(g_rec.pos));
        g_rec.radius = radius, g_rec.height = T::kHeightM;
    }
    g_hm.store(hm);
    int n = -1;
    const int rc = TarListEdit(hm, true, &n, false);
    if (rc != 1 && rc != 2) {
        if (rc < 0) {
            ++g_faults, g_dead = true;
            Line(g_lim_edge, now_ms, "not added (FAULTED, count %d): the tar feel stays off this session", n);
        } else {
            Line(g_lim_edge, now_ms, "list full (10): not added");
        }
        g_readd_ms = now_ms + T::kReaddMs;
        return false;
    }
    g_in.store(true);
    ++g_adds;
    g_ep = Episode{};
    g_ep.episode = g_view.episode, g_ep.test = g_view_test;
    g_ep.in_ms = g_ep.sample_ms = now_ms, g_ep.check_ms = now_ms + T::kCheckMs, g_ep.line_ms = now_ms + T::kLineMs;
    g_ep.health0 = Health(), g_ep.game0 = GameState();
    g_ep.cargo0 = CargoRead(&g_ep.cargo_na);
    char p[64];
    FmtPos(p, sizeof(p), g_rec.pos);
    char cargo[48];
    if (g_ep.cargo_na) std::snprintf(cargo, sizeof(cargo), "cargo n/a");
    else std::snprintf(cargo, sizeof(cargo), "cargo %d pieces", g_ep.cargo0.n);
    Line(g_lim_edge, now_ms, "record in (hm+0xD0 %d/10) at (%s) r %.1f m h %.1f, Sam %.1f m from its centre (episode %u%s%s); health %.1f, %s", n, p,
         g_rec.radius, g_rec.height, g_me_ok ? R::Dist3(g_me, g_rec.pos) : -1.0, g_ep.episode, g_ep.test ? ", test" : "", rc == 2 ? ", already listed" : "",
         g_ep.health0, cargo);
    return true;
}

void Refresh(const double (&pos)[3], float radius, double now_ms) {
    if (!T::NeedsRefresh(g_rec.pos, g_rec.radius, pos, radius)) return;
    TarRec want = g_rec;
    std::memcpy(want.pos, pos, sizeof(want.pos));
    want.radius = radius;
    if (TarRecSet(g_hm.load(), want) == 1) {
        ++g_refresh;
        char p[64];
        FmtPos(p, sizeof(p), g_rec.pos);
        Line(g_lim_line, now_ms, "record moved to (%s) r %.1f m", p, g_rec.radius);
    } else {
        ++g_faults;
        RecordOut(T::Why::hm_changed, now_ms);
    }
}

// While the record is in: the samples, the read-back and the periodic line.
void InTick(double now_ms) {
    const uptr hm = g_hm.load(), cur = I::HunterMgr();
    if (cur != hm) return RecordOut(T::Why::hm_changed, now_ms);
    if (now_ms >= g_ep.check_ms) {
        g_ep.check_ms = now_ms + T::kCheckMs;
        if (HasOurs(hm) == 0) {  // the game rewrote its list without our record
            g_in.store(false);
            ++g_game_outs;
            g_readd_ms = now_ms + T::kReaddMs;
            Line(g_lim_edge, now_ms, "record out (%s): count %d (in again in 1 s if the gate lets it)", T::WhyText(T::Why::gone_from_list), HmCount(hm));
            return;
        }
    }
    if (now_ms < g_ep.sample_ms) return;
    g_ep.sample_ms = now_ms + T::kSampleMs;
    float raw = -1;
    const float tar = SamTar(&raw);
    const T::Inside in = T::InsideTest(g_me, g_rec.pos, g_rec.radius, g_rec.height);
    if (g_me_ok) {
        if (g_ep.have_last) g_ep.moved_m += R::Dist3(g_me, g_ep.last);
        std::memcpy(g_ep.last, g_me, sizeof(g_ep.last));
        g_ep.have_last = true;
    }
    if (g_me_ok && tar >= 0) {
        float& mx = in.in ? g_ep.max_in : g_ep.max_out;
        if (tar > mx) mx = tar;
        ++(in.in ? g_ep.n_in : g_ep.n_out);
    }
    if (g_me_ok && in.in != g_ep.inside) {
        g_ep.inside = in.in;
        if (!in.in) g_ep.logged_in = false;  // a new entry logs again
    }
    if (g_me_ok && in.in && !g_ep.logged_in && tar >= T::kInLevel) {
        g_ep.logged_in = true;
        Line(g_lim_edge, now_ms, "Sam in it (+0x844 %.3f after %.1f s; 2D %.1f m vs %.1f, z %.1f vs top %.1f)", tar, (now_ms - g_ep.in_ms) / 1000.0, in.d2,
             in.rin, g_me[2], in.top);
    }
    if (now_ms >= g_ep.line_ms) {
        g_ep.line_ms = now_ms + (now_ms - g_ep.in_ms < 20000.0 ? T::kLineMs : 30000.0);
        // the cargo so far (gate run 20261128-020913): when the wear comes (at a move, or with +0x844) shows on these lines
        char cargo[96] = "cargo n/a", worn[96] = "";
        if (!g_ep.cargo_na) {
            const char* na = nullptr;
            const T::CargoSnap now_c = CargoRead(&na);
            const T::CargoDelta cd = T::CargoDiff(g_ep.cargo0, na ? T::CargoSnap{} : now_c);
            T::CargoText(cd, na, cargo, sizeof(cargo));
            T::CargoWornText(cd, worn, sizeof(worn));
        }
        Line(g_lim_line, now_ms, "+%.1f s: Sam inside %s (2D %.1f m vs %.1f, z %.1f vs top %.1f), +0x844 %.3f (+0x848 %.3f), count %d, hm140 0x%llX, health %.1f, game 0x%llX; "
             "so far %s%s, Sam moved %.1f m",
             (now_ms - g_ep.in_ms) / 1000.0, g_me_ok ? YesNo(in.in) : "unknown", in.d2, in.rin, g_me_ok ? g_me[2] : 0.0, in.top, tar, raw, HmCount(hm),
             H(HmArea(hm)), Health(), static_cast<unsigned long long>(GameState()), cargo, worn, g_ep.moved_m);
    }
}

// The real-save refusal: one line at the episode's start and one at its end (the cargo read across it, without a record).
void RefusalStart(double now_ms) {
    g_ref = Refusal{};
    g_ref.on = true, g_ref.episode = g_view.episode, g_ref.start_ms = now_ms;
    g_ref.cargo0 = CargoRead(&g_ref.cargo_na);
    double c[3];
    float r = 0;
    const bool ok = T::RecordFrom(g_view.last, c, r);
    char p[64];
    if (ok) FmtPos(p, sizeof(p), c);
    else std::snprintf(p, sizeof(p), "?");
    Line(g_lim_edge, now_ms, "not felt: %s%s: the partner's tar at (%s) r %.1f m is %.1f m from Sam (episode %u%s); no record goes in (kRealSaveOk false)",
         T::WhyText(T::Why::real_save), g_sim_real ? " (bt_sync 12 0 1: the test's real-save gate on a sandboxed save)" : "", p, r,
         ok && g_me_ok ? R::Dist3(g_me, c) : -1.0, g_ref.episode, g_view_test ? ", test" : "");
}
void RefusalEnd(T::Why why, double now_ms) {
    if (!g_ref.on) return;
    g_ref.on = false;
    const T::CargoSnap after = g_ref.cargo_na ? T::CargoSnap{} : CargoRead(&g_ref.cargo_na);
    const T::CargoDelta cd = T::CargoDiff(g_ref.cargo0, after);
    char cargo[96], worn[96];
    T::CargoText(cd, g_ref.cargo_na, cargo, sizeof(cargo));
    T::CargoWornText(cd, worn, sizeof(worn));
    Line(g_lim_edge, now_ms, "not felt (episode %u, %.1f s, ended: %s): %s%s", g_ref.episode, (now_ms - g_ref.start_ms) / 1000.0, T::WhyText(why), cargo, worn);
}

// ---- the HUNT feed (game thread): the partner's (OnMsg) and bt_sync 12's test (through the codec, as the partner's would be)
void Take(const R::BtsHunt& m, double now_ms, bool test) {
    g_view.Take(m, now_ms);
    g_view_test = test;
}
bool FeedTest(std::uint8_t kind, double now_ms) {
    R::BtsHunt m;
    m.kind = kind, m.flags = R::kHfTest, m.episode = g_test.episode;
    std::memcpy(m.center, g_test.center, sizeof(m.center));
    m.radius = T::kTestRadiusM;
    m.sender_ms = static_cast<std::uint32_t>(now_ms);
    std::uint8_t b[R::kBtsHuntSize];
    const std::size_t n = R::EncodeHunt(0, m, b, sizeof(b));
    R::BtsHunt d;
    if (!n || !R::DecodeHunt(b, n, d)) return false;
    Take(d, now_ms, true);
    return true;
}
void TestTick(double now_ms) {
    if (!g_test.on) return;
    if (now_ms - g_test.start_ms >= T::kTestMs) {
        g_test.on = false;
        FeedTest(R::kHuntClear, now_ms);
        Line(g_lim_misc, now_ms, "test HUNT (bt_sync 12): CLEAR after %.0f s (episode %u)", T::kTestMs / 1000.0, g_test.episode);
        return;
    }
    if (now_ms < g_test.next_ms) return;
    g_test.next_ms = now_ms + T::kTestSendMs;
    if (!FeedTest(R::kHuntUp, now_ms)) {
        g_test.on = false;
        Line(g_lim_misc, now_ms, "test HUNT (bt_sync 12): the codec refused it (ended)");
    }
}

bool PartnerSourceOk(const TickCtx& c) {
    return (c.role == 1 || c.role == 2) && !c.echo && c.sync_level >= 2 && c.peer && (c.partner.caps & R::kCapHunt) != 0;
}

void HoldLine(T::Why why, const char* not_playing, double now_ms) {
    if (g_in.load()) return;  // a hold keeps the record as it is: only a refused add is logged
    if (g_hold_episode == g_view.episode && g_hold_logged == why) return;
    g_hold_episode = g_view.episode, g_hold_logged = why;
    if (why == T::Why::list_full) Line(g_lim_edge, now_ms, "list full (10): not added");
    else Line(g_lim_edge, now_ms, "record held out (%s)", not_playing ? not_playing : T::WhyText(why));
}

}  // namespace

void Install(const Config& config) noexcept {
    g_key = config.bt_tar;
    if (g_key >= 1)
        Log("[bt] tar: bt_tar=%d: the partner's hunt (HUNT) felt here as tar, list 0 (a real-save guest: %s)", g_key,
            T::kRealSaveOk ? "felt" : "refused until the gate's cargo read");
}

void OnHello(const PeerHello& peer) noexcept {
    if (g_key < 1 || g_stopped.load()) return;
    const double now = I::NowMs();
    const bool can = (peer.caps & R::kCapHunt) && peer.level >= 2;
    Line(g_lim_misc, now, "the partner's hello: bt_sync %u, role %u, HUNT cap %s (the tar feel %s)", peer.level, peer.role,
         YesNo((peer.caps & R::kCapHunt) != 0), can ? "can run here" : "off for this partner");
    // gate run 20261128-011702 (two-games-bt-trouble-tar): the checks ran only on the first HUNT, so a guest that had no HUNT yet
    // had no checks line. They run here once (code bytes only, the game thread), as soon as a partner can send a HUNT; the echo's
    // bt_sync 12 test still runs them on its first tick (Tick: Checks).
    if (can && !g_dead) Checks(now);
}

void OnMsg(std::uint8_t type, const std::uint8_t* msg, std::size_t n, double now_ms) noexcept {
    if (type != R::kBtsHunt || g_key < 1 || g_stopped.load()) return;
    ++g_rx;
    if (g_test.on) {  // bt_sync 12's test owns the view while it runs
        ++g_rx_test_skip;
        return;
    }
    R::BtsHunt m;
    if (!R::DecodeHunt(msg, n, m)) {
        ++g_rx_bad;
        return;
    }
    Take(m, now_ms, false);
}

void Tick(const TickCtx& c) noexcept {
    if (g_stopped.load()) return;
    const double now = c.now_ms;
    g_me_ok = c.me_ok;
    std::memcpy(g_me, c.me, sizeof(g_me));
    if (g_key < 1) {
        if (g_in.load()) RecordOut(T::Why::off, now);
        return;
    }
    TestTick(now);
    const bool active = g_view.up || g_in.load() || g_ref.on;
    if (!active) return;
    if (g_dead || !Checks(now)) {
        if (g_in.load()) RecordOut(T::Why::off, now);
        g_view.Expire(now);
        return;
    }
    const uptr hm = I::HunterMgr();
    R::TarFeelIn in;
    in.bt_tar = g_key;
    in.hunt_up = g_view.up;
    in.since_hunt_ms = g_view.SinceMs(now);
    in.loading = c.loading;
    in.not_playing = c.loading ? nullptr : c.playing_why;
    in.carried = c.carried;
    in.real_save = c.real_save || g_sim_real;
    in.real_save_ok = T::kRealSaveOk;
    in.hm_ready = HmReady(hm);
    in.count = HmCount(hm);
    in.ours_in = g_in.load();
    double pos[3] = {0, 0, 0};
    float radius = 0;
    const bool have = T::RecordFrom(g_view.last, pos, radius);
    in.dist_m = have && c.me_ok ? R::Dist3(c.me, pos) : -1;
    const T::FeelVerdict v = T::FeelGate(in, g_view_test || PartnerSourceOk(c));
    if (v.why == T::Why::real_save) {
        if (!g_ref.on || g_ref.episode != g_view.episode) {
            RefusalEnd(T::Why::cleared, now);
            RefusalStart(now);
        }
    } else if (g_ref.on) {
        RefusalEnd(v.why == T::Why::none ? T::Why::cleared : v.why, now);
    }
    switch (v.call) {
        case R::TarCall::in:
            if (!g_in.load()) {
                if (now >= g_readd_ms && have) RecordIn(hm, pos, radius, now);
            } else if (have) {
                Refresh(pos, radius, now);
            }
            break;
        case R::TarCall::out:
            if (g_in.load()) RecordOut(v.why, now);
            break;
        case R::TarCall::hold: HoldLine(v.why, v.not_playing, now); break;
    }
    if (g_in.load()) InTick(now);
    g_view.Expire(now);
}

void OnWorldEvent(WorldEvent e, double now_ms) noexcept {
    if (g_key < 1 || g_stopped.load()) return;
    switch (e) {
        case WorldEvent::load_start:
            RecordOut(T::Why::load, now_ms);
            RefusalEnd(T::Why::load, now_ms);
            break;
        case WorldEvent::link_lost:
        case WorldEvent::partner_gone: {
            const T::Why why = e == WorldEvent::link_lost ? T::Why::link_lost : T::Why::partner_gone;
            if (!g_view_test) {
                RecordOut(why, now_ms);
                RefusalEnd(why, now_ms);
                g_view = R::HuntView{};
            }
            break;
        }
        case WorldEvent::load_end: break;
    }
}

// Any thread, synchronous (dllmain's emergency stop after a tick fault: no later Tick). Our record first goes 100 km below the
// ground (our own memory: never "inside" again), then out of the list under hm+0x130 inside SEH (try-locked, 50 ms at most: this
// thread may be the one that holds it).
void EmergencyStop() noexcept {
    if (g_stopped.exchange(true)) return;
    if (!g_in.load()) return;
    g_rec.pos[2] = T::kNeutralZ;
    const uptr hm = g_hm.load();
    int n = -1, rc = -3;
    if (HmSane(hm)) {
        rc = -2;
        for (int i = 0; i < 50 && rc == -2; ++i) {
            rc = TarListEdit(hm, false, &n, true);
            if (rc == -2) Sleep(1);
        }
    }
    g_in.store(false);
    Log("[bt] tar: record out (EmergencyStop): removed %s, count %d", EditText(rc), n);
}

std::uint16_t Caps() noexcept { return 0; }  // no wire of its own (HUNT's centre and radius)

void Status(char* out, std::size_t cap) noexcept {
    if (!out || !cap) return;
    if (g_key < 1) {
        std::snprintf(out, cap, "tar: off (bt_tar=%d)", g_key);
        return;
    }
    std::snprintf(out, cap, "tar: bt_tar=%d%s, record %s, in %u out %u (game %u) moved %u, faults %u; hunt rx %u (bad %u, skipped %u), view %s ep %u%s; checks %s%s",
                  g_key, g_stopped.load() ? " (stopped)" : g_dead ? " (off: a fault)" : "", g_in.load() ? "IN" : "out", g_adds, g_outs, g_game_outs, g_refresh,
                  g_faults, g_rx, g_rx_bad, g_rx_test_skip, g_view.up ? "UP" : "down", g_view.episode, g_view_test ? " test" : "",
                  g_checks > 0 ? "ok" : g_checks < 0 ? "FAILED" : "not run", g_sim_real ? "; real-save test on" : "");
}

bool Command(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (op != 12) return Put(msg, cap, "bt sync %d: not a tar op", op), false;
    if (g_stopped.load()) return Put(msg, cap, "bt sync 12: stopped (emergency stop)"), false;
    if (!automation::Enabled()) return Put(msg, cap, "bt sync 12: the test HUNT is for hidden runs only"), false;
    if (g_key < 1) return Put(msg, cap, "bt sync 12: bt_tar is 0 here"), false;
    const double now = I::NowMs();
    switch (T::TestArgs(v[1], v[2])) {
        case T::TestMode::bad:
            return Put(msg, cap, "bt sync 12: [x y] a test HUNT (0 0: 8 m ahead of Sam), 0 1 / 0 2 the real-save gate on / off, 0 3 end the test"), false;
        case T::TestMode::real_on:
        case T::TestMode::real_off:
            g_sim_real = T::TestArgs(v[1], v[2]) == T::TestMode::real_on;
            Line(g_lim_misc, now, "bt_sync 12: this game counts as a real-save guest for the tar gate: %s (a sandboxed save: the refusal path only)", g_sim_real ? "on" : "off");
            return Put(msg, cap, "bt sync 12: the real-save gate %s", g_sim_real ? "on" : "off"), true;
        case T::TestMode::stop:
            if (!g_test.on) return Put(msg, cap, "bt sync 12: no test HUNT is up"), false;
            g_test.on = false;
            FeedTest(R::kHuntClear, now);
            Line(g_lim_misc, now, "test HUNT (bt_sync 12): CLEAR (ended by bt_sync 12 0 3; episode %u)", g_test.episode);
            return Put(msg, cap, "bt sync 12: test HUNT cleared"), true;
        case T::TestMode::ahead:
        case T::TestMode::at: break;
    }
    if (g_test.on) return Put(msg, cap, "bt sync 12: the last test HUNT is still up (20 s; 0 3 ends it)"), false;
    if (g_view.up && !g_view_test) return Put(msg, cap, "bt sync 12: the partner's HUNT is up (not mixed with a test)"), false;
    double me[3], fx = 0, fy = 0, c[3];
    if (!I::SamFacing(me, &fx, &fy)) return Put(msg, cap, "bt sync 12: no local player transform"), false;
    if (T::TestArgs(v[1], v[2]) == T::TestMode::ahead) {
        if (!R::AheadOf(me, fx, fy, T::kTestAheadM, c)) return Put(msg, cap, "bt sync 12: no facing"), false;
    } else {
        c[0] = v[1], c[1] = v[2], c[2] = me[2];
    }
    g_test.on = true, g_test.start_ms = now, g_test.next_ms = now;
    if (++g_test.episode < T::kTestEpisode0) g_test.episode = T::kTestEpisode0;
    for (int i = 0; i < 3; ++i) g_test.center[i] = static_cast<float>(c[i]);
    TestTick(now);
    char p[64];
    FmtPos(p, sizeof(p), c);
    Line(g_lim_misc, now, "test HUNT (bt_sync 12): UP at (%s) r %.1f m for %.0f s (episode %u), Sam %.1f m from it", p, T::kTestRadiusM, T::kTestMs / 1000.0,
         g_test.episode, R::Dist3(me, c));
    return Put(msg, cap, "bt sync 12: test HUNT at (%s) r %.0f m for %.0f s (log)", p, T::kTestRadiusM, T::kTestMs / 1000.0), true;
}

}  // namespace st::bt::tar
