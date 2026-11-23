#include "fragile_textures.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

#include "MinHook.h"
#include "automation.hpp"
#include "coop_session.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "suit_colour.hpp"
#include "tex_blob.hpp"
#include "tex_hash.hpp"
#include "tex_slots.hpp"
#include "texture_looks.hpp"

namespace st::ftex {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;
namespace tx = st::tex;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// v0.23's (suit_colour.cpp, byte-checked by suitcol::Available, which this module requires):
constexpr std::uintptr_t kCore = 0x237CA20;     // texture-set override core (drawable, String*, TextureSet*, Array*, mesh)
constexpr std::uintptr_t kUndo = 0x237A960;     // (drawable, u64* handle)
constexpr std::uintptr_t kRefresh = 0x2378C20;  // (drawable, bool)
constexpr std::uintptr_t kLoader = 0x1D24460;   // (HwTexture** out, reader, u32* dims)
constexpr std::uintptr_t kFactory = 0x4F6D4F8;
constexpr std::uintptr_t kFactoryVtbl = 0x3B9B8C8;
constexpr std::uintptr_t kHwTexVtbl = 0x3B9B488;
constexpr std::uintptr_t kTexVtbl = 0x3BC7C50;
constexpr std::uintptr_t kEmptyString = 0x4562CA0;
// New (docs/real-partner-body.md section 29.3):
// The override table's rebuild 0x141DF4720(table, nodes): table+0 = nodes; every entry whose hash is not the free
// sentinel gets its slots collected again (0x141D884A0 + 0x141C97290); then every kind-5 (texture) entry pushes
// entry+0x30's hardware texture into ALL its slots (0x14198A320 -> slot->vtbl[+0x68]). Detoured: our entries are hidden
// from it (hash = sentinel) and rebuilt by us with only the slots that hold one of her suit's own textures.
constexpr std::uintptr_t kRebuild = 0x1DF4720;
constexpr std::uintptr_t kRebuildNodes = 0x1DF4740;     // movsxd rbx,[rcx+0x10]; xor r13d,r13d; mov [rcx],rdx
constexpr std::uintptr_t kRebuildSkip1 = 0x1DF47C0;     // the collect loop's sentinel compare (rip -> kSentinel)
constexpr std::uintptr_t kRebuildSkip2 = 0x1DF4830;     // the push loop's sentinel compare
constexpr std::uintptr_t kRebuildKind5 = 0x1DF4845;     // kind 5: +0x30 Texture*, +0x38 swizzle
constexpr std::uintptr_t kSentinel = 0x4563070;         // the free entry's hash (u32)
constexpr std::uintptr_t kCreateEntry = 0x1DF4040;      // the core's entry create: collects with 0x141C974F0
constexpr std::uintptr_t kCreateCollectCall = 0x1DF40D2;
constexpr std::uintptr_t kCollect = 0x1C974F0;          // (nodes, hash, entry+8): the slots with that binding name
constexpr std::uintptr_t kPush = 0x1DF56A0;             // (entry+8, hw, swizzle): every slot's vtbl[+0x68]
constexpr std::uintptr_t kSwizzle = 0x1D25E50;          // (set entry, u16* out, type): the swizzle from the packing
// The renderer and its upload ring: 0x141967665 mov rcx,[R]; ...; add rcx,0xB48; cmp r8,[rcx+8] (the ring size).
constexpr std::uintptr_t kRingCmp = 0x1967665;
constexpr std::uintptr_t kRenderer = 0x4F6D430;
constexpr std::uintptr_t kRingSize = 0xB50;
// The Texture destructor 0x141D24160 releases its hardware texture: when (hw+0x38 & 0x28) == 0x28 the GUID cache entry
// (0x14196F480(R+0x2CDF8, hw), the call at 0x141D241BC), then the reference at hw+8 (the last one: hw->vtbl[0](hw, 1)).
constexpr std::uintptr_t kTexRelease = 0x1D241A6;
constexpr std::uintptr_t kTexReleaseCall = 0x1D241BC;
constexpr std::uintptr_t kCacheRelease = 0x196F480;
constexpr std::uintptr_t kGuidCache = 0x2CDF8;
constexpr std::uintptr_t kHwDtor = 0x195E740;

constexpr std::uintptr_t kDrMeshes = 0x98;
constexpr std::uintptr_t kDrOverrides = 0x1E8;

// HwTexture fields (0x14197BE60 / 0x141D24460 / 0x141967390).
constexpr std::uintptr_t kHwFmt = 0x31, kHwDims = 0x34, kHwMips = 0x38, kHwStream = 0x70, kHwGuid = 0x78, kHwBusy = 0x98;

// ---------------------------------------------------------------- state
struct State {
    bool checked = false, ok = false, off = false;
    char why[200] = "not checked";
    Settings cfg;
    bool configured = false;
    bool hooked = false;
    bool user_off = false;    // tex 4 0
    DWORD game_tid = 0;
    std::uint64_t ring = 0;
    bool ring_logged = false;
    int forced_colour = -1;
    int forced_boots = -1;    // tex 10
    int pattern = 0;          // tex 9: 0 off, 1 suit checker, 2 + boots
} g;

bool Check(const char* name, std::uintptr_t rva, const char* sig) {
    if (mem::CodeMatches(mem::Base() + rva, sig)) return true;
    std::snprintf(g.why, sizeof(g.why), "%s at ds.exe+0x%llX does not match", name, static_cast<unsigned long long>(rva));
    return false;
}

void Fault(const char* what) {
    g.off = true;
    Log("[tex] %s FAULTED: Fragile's textures off for this session", what);
}

// ---------------------------------------------------------------- the rule GUIDs (content GUIDs of the slots)
std::uint8_t g_guid[tx::kSlotCount][16];
bool g_guid_ok = false;
void InitGuids() {
    if (g_guid_ok) return;
    bool ok = true;
    for (std::size_t i = 0; i < tx::kSlotCount; ++i) ok = tx::GuidBytes(tx::kSlots[i].content_guid, g_guid[i]) && ok;
    g_guid_ok = ok;
}
// -1 none, else the slot index whose original texture this hardware texture is (its GUID at +0x78).
int SlotOfHw(std::uintptr_t hw) {
    if (!hw) return -1;
    std::uintptr_t vt = 0;
    if (!Rd(hw, vt) || vt != mem::Base() + kHwTexVtbl) return -1;
    std::uint8_t gd[16];
    if (!mem::Read(hw + kHwGuid, gd, 16)) return -1;
    for (std::size_t i = 0; i < tx::kSlotCount; ++i)
        if (std::memcmp(gd, g_guid[i], 16) == 0) return static_cast<int>(i);
    return -1;
}
bool IsOurHw(std::uintptr_t hw) {
    std::uint8_t gd[4];
    return hw && mem::Read(hw + kHwGuid, gd, 4) && gd[0] == 'S' && gd[1] == 'T' && gd[2] == 'T' && gd[3] == 'X';
}

// ---------------------------------------------------------------- the filtered registry
// One override entry of ours: the detour rebuilds it with only the slots its rule keeps.
struct Reg {
    bool used = false;
    std::uintptr_t table = 0, drawable = 0, tex = 0;
    int index = -1;
    std::uint32_t hash = 0;
    std::uint16_t swz = 0;
    int rule = 0;            // 0 suit, 1 boots
    unsigned seen = 0;       // rebuilds of it done by the detour
    unsigned kept = 0, dropped = 0;
};
constexpr int kRegMax = 64;
Reg g_reg[kRegMax];
SRWLOCK g_reg_lock = SRWLOCK_INIT;
std::atomic<int> g_any{0};
std::atomic<int> g_filter{0};       // 0 product, 1 keep all (tests: shows the bleed), 2 log only (keep all + dump)
std::atomic<int> g_probe{0};        // rebuilds still to dump
std::atomic<unsigned> g_rebuilds{0}, g_kept{0}, g_dropped{0}, g_faults{0}, g_detour_calls{0}, g_detour_logged{0};
std::atomic<bool> g_detour_off{false};
using RebuildFn = void (*)(std::uintptr_t, std::uintptr_t);
RebuildFn g_orig_rebuild = nullptr;

// SEH-wrapped game calls used inside the detour (no C++ objects here).
bool SehCollect(std::uintptr_t nodes, std::uint32_t hash, std::uintptr_t entry8) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, std::uint32_t, std::uintptr_t)>(mem::Base() + kCollect)(nodes, hash, entry8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehPush(std::uintptr_t entry8, std::uintptr_t hw, std::uint16_t swz) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t, std::uint16_t)>(mem::Base() + kPush)(entry8, hw, swz);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The slot (pair[0], class vtable 0x143B9B7E8) keeps its shader resources in up to three copy-on-write blocks
// (slot+0x10/+0x18/+0x20, tagged pointers); its vtbl[+0x68] (0x14198A380) finds the block of a binding (pair[1], a packed
// u64) with 0x14196E390 and writes the 0x20-byte record {hw, swizzle, ...} at
//   block + 0x10 + u16[layout + ((l1>>24&3) + (l1&0xF) + 2)*6 + ((l1>>4&0xF) + (l0>>14&7))*4 + idx*10 + 4] + (binding>>45 & 0xFFFF)
// with layout = *(*(slot+8)+0x38) & ~0xF (l0, l1 its first two u32) and idx = binding>>38 & 0x7F. Read here without a call
// (fault-free reads; byte-checked in Available), so the rebuild can see which texture a slot holds before ours goes in.
constexpr std::uintptr_t kSlotVtbl = 0x3B9B7E8;
constexpr std::uintptr_t kSlotSet = 0x198A380;       // the slot's vtbl[+0x68]: the record write
constexpr std::uintptr_t kSlotSetLayout = 0x198A3FD;  // its record address computation
constexpr std::uintptr_t kSlotBlockOf = 0x196E390;    // the block of a binding (read only)

std::uintptr_t SlotHw(std::uintptr_t slot, std::uint64_t binding) {
    if (!slot || (binding & 0xF) == 8) return 0;
    std::uintptr_t vt = 0;
    if (!Rd(slot, vt) || vt != mem::Base() + kSlotVtbl) return 0;
    const std::uintptr_t info = Q(slot + 8);
    const std::uintptr_t lay = info ? (Q(info + 0x38) & ~static_cast<std::uintptr_t>(0xF)) : 0;
    std::uint32_t l0 = 0, l1 = 0;
    if (!lay || !Rd(lay, l0) || !Rd(lay + 4, l1)) return 0;
    const std::uint32_t idx = static_cast<std::uint32_t>(binding >> 38) & 0x7Fu;
    const std::uintptr_t ranges = lay + ((((l1 >> 24) & 3u) + 2u) * 6u + ((l0 >> 14) & 7u) * 4u);
    const std::uintptr_t multi = lay + (((l1 & 0xFu) + ((l1 >> 24) & 3u) + 2u) * 6u + ((l0 >> 14) & 7u) * 4u);
    std::uintptr_t block = 0;
    for (int i = 0; i < 3 && !block; ++i) {
        const std::uintptr_t c = Q(slot + 0x10 + 8ull * i);
        if (!(c & ~static_cast<std::uintptr_t>(0xF))) continue;
        std::uint32_t d = 0;
        if (!Rd(ranges + (c & 0xF) * 6u, d)) return 0;
        const std::uint32_t lo = (d >> 17) & 0x7Fu, cnt = d >> 24;
        bool hit = false;
        if (!((d >> 16) & 1u)) {
            hit = lo <= idx && idx < lo + cnt;
        } else {
            for (std::uint32_t k = 0; k < cnt && !hit; ++k) {
                std::uint32_t e = 0;
                if (!Rd(multi + (lo + k) * 4u, e)) return 0;
                const std::uint32_t a = (e >> 7) & 0xFFu;
                hit = a <= idx && idx < a + ((e >> 15) & 0xFFu);
            }
        }
        if (hit) block = c & ~static_cast<std::uintptr_t>(0xF);
    }
    if (!block) return 0;
    std::uint16_t off = 0;
    const std::uintptr_t at = lay + ((((l1 >> 24) & 3u) + (l1 & 0xFu) + 2u) * 6u + (((l1 >> 4) & 0xFu) + ((l0 >> 14) & 7u)) * 4u) + idx * 10u + 4u;
    if (!Rd(at, off)) return 0;
    return Q(block + 0x10 + off + ((binding >> 45) & 0xFFFFu));
}

const char* HwName(std::uintptr_t hw, char* buf, std::size_t cap) {
    if (!hw) return "none";
    const int s = SlotOfHw(hw);
    if (s >= 0) return tx::kSlots[s].id;
    if (IsOurHw(hw)) return "OURS";
    std::uint8_t gd[4] = {};
    std::uintptr_t vt = 0;
    Rd(hw, vt);
    if (vt != mem::Base() + kHwTexVtbl) return "not-a-texture";
    mem::Read(hw + kHwGuid, gd, 4);
    std::snprintf(buf, cap, "other(%02x%02x%02x%02x)", gd[3], gd[2], gd[1], gd[0]);
    return buf;
}

void DumpEntry(const char* tag, std::uintptr_t entry, unsigned max_pairs) {
    std::int32_t n = 0;
    Rd(entry + 8, n);
    const std::uintptr_t pairs = Q(entry + 0x10);
    std::uint32_t hash = 0, kind = 0;
    Rd(entry, hash);
    Rd(entry + 0x18, kind);
    char line[460];
    int k = std::snprintf(line, sizeof(line), "[tex] probe %s entry %08X (kind %u, %d slots, Texture %p):", tag, hash, kind, n,
                          reinterpret_cast<void*>(Q(entry + 0x30)));
    for (int i = 0; i < n && i < static_cast<int>(max_pairs) && pairs && k < static_cast<int>(sizeof(line)) - 40; ++i) {
        const std::uintptr_t p0 = Q(pairs + 16ull * i);
        const std::uint64_t p1 = Q(pairs + 16ull * i + 8);
        char nb[32];
        k += std::snprintf(line + k, sizeof(line) - k, " %d:%s/%04llx", i, HwName(SlotHw(p0, p1), nb, sizeof(nb)), static_cast<unsigned long long>(p1 >> 48));
    }
    Log("%s", line);
}

bool Keeps(int rule, std::uintptr_t slot, std::uint64_t binding) {
    const int mode = g_filter.load(std::memory_order_relaxed);
    if (mode != 0) return true;
    const int s = SlotOfHw(SlotHw(slot, binding));
    if (s < 0) return false;
    return rule == (tx::kSlots[s].group == tx::SlotGroup::suit ? 0 : 1);
}

// After the original: our entry gets its slots again, only those its rule keeps, and our texture in them. No C++ objects.
bool RebuildOurs(const Reg& m, std::uintptr_t nodes, bool probe, unsigned* kept, unsigned* dropped) {
    const std::uintptr_t entry = Q(m.table + 0x18) + static_cast<std::uintptr_t>(m.index) * 0x40;
    *reinterpret_cast<volatile std::uint32_t*>(entry) = m.hash;
    *reinterpret_cast<volatile std::int32_t*>(entry + 8) = 0;
    if (!nodes) return true;
    if (!SehCollect(nodes, m.hash, entry + 8)) return false;
    if (probe) DumpEntry("collected", entry, 24);
    std::int32_t n = *reinterpret_cast<volatile std::int32_t*>(entry + 8);
    std::uint64_t* pairs = reinterpret_cast<std::uint64_t*>(Q(entry + 0x10));
    int w = 0;
    for (int i = 0; i < n && pairs; ++i) {
        if (Keeps(m.rule, static_cast<std::uintptr_t>(pairs[2 * i]), pairs[2 * i + 1])) {
            if (w != i) {
                pairs[2 * w] = pairs[2 * i];
                pairs[2 * w + 1] = pairs[2 * i + 1];
            }
            ++w;
            ++*kept;
        } else {
            ++*dropped;
        }
    }
    *reinterpret_cast<volatile std::int32_t*>(entry + 8) = w;
    *reinterpret_cast<volatile std::uintptr_t*>(entry + 0x30) = m.tex;
    *reinterpret_cast<volatile std::uint16_t*>(entry + 0x38) = m.swz;
    const std::uintptr_t hw = Q(m.tex + 0x20);
    if (!SehPush(entry + 8, hw, m.swz)) return false;
    if (probe) DumpEntry("ours", entry, 24);
    return true;
}

void DetourRebuild(std::uintptr_t table, std::uintptr_t nodes) {
    if (!g_any.load(std::memory_order_acquire) || g_detour_off.load(std::memory_order_relaxed)) return g_orig_rebuild(table, nodes);
    const unsigned call = g_detour_calls.fetch_add(1, std::memory_order_relaxed);
    Reg mine[24];
    int idx[24];
    int nm = 0;
    AcquireSRWLockShared(&g_reg_lock);
    for (int i = 0; i < kRegMax && nm < 24; ++i)
        if (g_reg[i].used && g_reg[i].table == table) {
            mine[nm] = g_reg[i];
            idx[nm++] = i;
        }
    ReleaseSRWLockShared(&g_reg_lock);
    if (!nm) return g_orig_rebuild(table, nodes);
    const DWORD tid = GetCurrentThreadId();
    if (g_detour_logged.load(std::memory_order_relaxed) < 8) {
        g_detour_logged.fetch_add(1, std::memory_order_relaxed);
        Log("[tex] detour: rebuild of table %p (nodes %p) on thread %lu (%s), %d entr%s of ours (call #%u)", reinterpret_cast<void*>(table),
            reinterpret_cast<void*>(nodes), tid, tid == g.game_tid ? "the game thread" : "NOT the game thread", nm, nm == 1 ? "y" : "ies", call);
    }
    const std::uint32_t sentinel = *reinterpret_cast<const std::uint32_t*>(mem::Base() + kSentinel);
    std::int32_t count = 0;
    Rd(table + 0x10, count);
    const std::uintptr_t entries = Q(table + 0x18);
    bool valid[24] = {};
    for (int j = 0; j < nm; ++j) {
        const Reg& m = mine[j];
        if (m.index < 0 || m.index >= count || !entries) continue;
        const std::uintptr_t entry = entries + static_cast<std::uintptr_t>(m.index) * 0x40;
        std::uint32_t hash = 0, kind = 0;
        Rd(entry, hash);
        Rd(entry + 0x18, kind);
        const std::uintptr_t t = Q(entry + 0x30);
        if (hash != m.hash || kind != 5 || (t != 0 && t != m.tex)) continue;
        valid[j] = true;
        *reinterpret_cast<volatile std::uint32_t*>(entry) = sentinel;  // the original neither collects nor pushes it
    }
    g_orig_rebuild(table, nodes);
    const bool probe = g_probe.load(std::memory_order_relaxed) > 0;
    unsigned kept = 0, dropped = 0;
    for (int j = 0; j < nm; ++j) {
        if (!valid[j]) continue;
        if (!RebuildOurs(mine[j], nodes, probe, &kept, &dropped)) {
            g_detour_off.store(true);
            g_faults.fetch_add(1);
            Log("[tex] detour: the rebuild of our entry FAULTED: pass-through from now on, Fragile's textures off");
            g.off = true;
            break;
        }
    }
    if (probe) g_probe.fetch_sub(1);
    g_rebuilds.fetch_add(1, std::memory_order_relaxed);
    g_kept.fetch_add(kept, std::memory_order_relaxed);
    g_dropped.fetch_add(dropped, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_reg_lock);
    for (int j = 0; j < nm; ++j) {
        Reg& r = g_reg[idx[j]];
        if (r.used && r.table == table && valid[j]) {
            ++r.seen;
            r.kept = kept;
            r.dropped = dropped;
        }
    }
    ReleaseSRWLockExclusive(&g_reg_lock);
}

bool InstallDetour() {
    if (g.hooked) return true;
    void* target = reinterpret_cast<void*>(mem::Base() + kRebuild);
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&DetourRebuild), reinterpret_cast<void**>(&g_orig_rebuild));
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        std::snprintf(g.why, sizeof(g.why), "the rebuild detour could not be installed (%s)", MH_StatusToString(s));
        Log("[tex] off: %s", g.why);
        return false;
    }
    g.hooked = true;
    Log("[tex] detour on the override rebuild (ds.exe+0x%llX) installed", static_cast<unsigned long long>(kRebuild));
    return true;
}

// ---------------------------------------------------------------- the binary reader (as v0.23's)
struct Reader {
    void* const* vtbl;
    std::uint32_t r08;
    std::uint32_t big_endian;
    std::uint32_t state;
    std::uint32_t r14;
    std::uintptr_t spare[6];
    const std::uint8_t* data;
    std::uint32_t size, pos;
    bool overrun;
    unsigned odd;
};
using RSlot = std::uintptr_t (*)(Reader*, std::uintptr_t, std::uintptr_t, std::uintptr_t);
std::uintptr_t RDtor(Reader* r, std::uintptr_t, std::uintptr_t, std::uintptr_t) { return reinterpret_cast<std::uintptr_t>(r); }
std::uintptr_t RRead(Reader* r, std::uintptr_t dst, std::uintptr_t n, std::uintptr_t) {
    const std::uint32_t want = static_cast<std::uint32_t>(n);
    std::uint8_t* out = reinterpret_cast<std::uint8_t*>(dst);
    const std::uint32_t have = r->pos < r->size ? r->size - r->pos : 0;
    const std::uint32_t k = want < have ? want : have;
    if (k) std::memcpy(out, r->data + r->pos, k);
    if (k < want) {
        std::memset(out + k, 0, want - k);
        r->overrun = true;
    }
    r->pos += k;
    return want;
}
std::uintptr_t RSkip(Reader* r, std::uintptr_t n, std::uintptr_t, std::uintptr_t) {
    const std::int32_t k = static_cast<std::int32_t>(n);
    const std::int64_t p = static_cast<std::int64_t>(r->pos) + k;
    r->pos = p < 0 ? 0 : p > r->size ? r->size : static_cast<std::uint32_t>(p);
    return 1;
}
std::uintptr_t RTell(Reader* r, std::uintptr_t, std::uintptr_t, std::uintptr_t) { return r->pos; }
template <int N>
std::uintptr_t ROdd(Reader* r, std::uintptr_t, std::uintptr_t, std::uintptr_t) {
    r->odd |= 1u << N;
    return 0;
}
RSlot const kReaderVtbl[16] = {&RDtor,    &ROdd<1>,  &RRead,    &RSkip,    &ROdd<4>,  &RTell,    &ROdd<6>,  &ROdd<7>,
                               &ROdd<8>,  &ROdd<9>,  &ROdd<10>, &ROdd<11>, &ROdd<12>, &ROdd<13>, &ROdd<14>, &ROdd<15>};

bool SehLoader(std::uintptr_t* hw_out, Reader* r, std::uint32_t* dims) {
    __try {
        reinterpret_cast<std::uintptr_t* (*)(std::uintptr_t*, Reader*, std::uint32_t*)>(mem::Base() + kLoader)(hw_out, r, dims);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- our textures (clones), kept for the session
struct TexObj {
    alignas(8) std::uint8_t b[0x30];
};
struct Clone {
    bool used = false;
    std::uint8_t id[32] = {};
    int slot = -1;
    TexObj obj{};
    std::uintptr_t hw = 0;
    double made_ms = 0, up_ms = -1;
    bool failed = false;
    double used_ms = 0;  // the last tick a body wanted or wore it
};
constexpr int kCloneMax = 48;
Clone g_clones[kCloneMax];
unsigned g_made = 0, g_make_failed = 0;
double g_last_make_ms = -1e9;

Clone* FindClone(const std::uint8_t id[32]) {
    for (Clone& c : g_clones)
        if (c.used && std::memcmp(c.id, id, 32) == 0) return &c;
    return nullptr;
}

std::uint64_t RingSize() {
    const std::uintptr_t r = Q(mem::Base() + kRenderer);
    return r ? Q(r + kRingSize) : 0;
}

// The largest mip's footprint in the upload ring (row pitch aligned to 256, offset to 512).
std::uint64_t Footprint(const tx::Slot& s) {
    const std::uint64_t blocks_w = (s.w + 3u) / 4u, blocks_h = (s.h + 3u) / 4u;
    const std::uint64_t pitch = ((blocks_w * static_cast<std::uint64_t>(tx::BlockBytes(s.clone_fmt))) + 255u) & ~255ull;
    return ((pitch * blocks_h) + 511u) & ~511ull;
}

// Game thread: one texture from a blob through the game's loader. Logged.
Clone* Make(const std::uint8_t id[32], int slot, const std::vector<std::uint8_t>& blob, double now_ms) {
    const tx::Slot& s = tx::kSlots[slot];
    const std::uintptr_t b = mem::Base();
    const std::uintptr_t r = Q(b + kRenderer);
    if (!r) return nullptr;  // no renderer yet
    const std::uint64_t ring = Q(r + kRingSize);
    if (!g.ring_logged || ring != g.ring) {
        g.ring_logged = true;
        g.ring = ring;
        Log("[tex] upload ring %llu bytes (largest mip footprint %llu)", static_cast<unsigned long long>(ring),
            static_cast<unsigned long long>(Footprint(s)));
    }
    if (ring < (1ull << 20) || ring > (4ull << 30) || ring < Footprint(s)) {
        if (!g.off) Log("[tex] off: upload ring %llu < %llu bytes", static_cast<unsigned long long>(ring), static_cast<unsigned long long>(Footprint(s)));
        g.off = true;
        return nullptr;
    }
    const std::uintptr_t fac = Q(b + kFactory);
    if (!fac || Q(fac) != b + kFactoryVtbl) {
        Log("[tex] the texture factory %p is not the expected class: no texture made", reinterpret_cast<void*>(fac));
        ++g_make_failed;
        return nullptr;
    }
    Clone* c = nullptr;
    for (Clone& k : g_clones)
        if (!k.used) {
            c = &k;
            break;
        }
    if (!c) {
        if (g_made >= kCloneMax) Log("[tex] the session's texture budget (%d) is used up: no more textures", kCloneMax);
        return nullptr;
    }
    std::string why;
    if (!tx::ValidateBlob(s, blob.data(), blob.size(), nullptr, &why)) {
        Log("[tex] %s blob refused before the loader: %s", s.id, why.c_str());
        ++g_make_failed;
        return nullptr;
    }
    Reader rd{};
    rd.vtbl = reinterpret_cast<void* const*>(kReaderVtbl);
    rd.state = 2;
    rd.data = blob.data();
    rd.size = static_cast<std::uint32_t>(blob.size());
    TexObj& o = c->obj;
    std::memset(o.b, 0, sizeof(o.b));
    std::uintptr_t hw = 0;
    const LARGE_INTEGER t0 = [] {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t;
    }();
    if (!SehLoader(&hw, &rd, reinterpret_cast<std::uint32_t*>(o.b + 0x28))) {
        Fault("the game's texture loader");
        return nullptr;
    }
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
    c->used = true;  // the slot keeps whatever the loader gave (we own a reference we never drop in this build)
    std::memcpy(c->id, id, 32);
    c->slot = slot;
    c->made_ms = now_ms;
    ++g_made;
    std::uintptr_t hwvt = 0;
    Rd(hw, hwvt);
    std::uint8_t fmt = 0;
    std::uint32_t dims = 0, mips = 0;
    std::uintptr_t stream = 0;
    std::uint8_t gd[16] = {};
    if (hw) {
        Rd(hw + kHwFmt, fmt);
        Rd(hw + kHwDims, dims);
        Rd(hw + kHwMips, mips);
        Rd(hw + kHwStream, stream);
        mem::Read(hw + kHwGuid, gd, 16);
    }
    const bool good = hw && !rd.overrun && !rd.odd && rd.pos == rd.size && hwvt == b + kHwTexVtbl && stream == 0 &&
                      std::memcmp(gd, blob.data() + 0x10, 16) == 0;
    Log("[tex] clone %s %s: hw %p (vt +0x%llX) fmt %u dims 0x%08X mips 0x%08X stream %p guid %s; reader %u/%u odd 0x%X; loader %.1f ms%s", s.id,
        tx::Hex(id, 4).c_str(), reinterpret_cast<void*>(hw), static_cast<unsigned long long>(hwvt > b ? hwvt - b : 0), fmt, dims, mips,
        reinterpret_cast<void*>(stream), std::memcmp(gd, blob.data() + 0x10, 16) == 0 ? "ours" : "OTHER", rd.pos, rd.size, rd.odd, ms,
        good ? "" : ": NOT USED");
    if (!good) {
        c->failed = true;
        ++g_make_failed;
        return nullptr;
    }
    const std::uintptr_t vt = b + kTexVtbl;
    std::memcpy(o.b, &vt, 8);
    std::memcpy(o.b + 0x08, blob.data() + 0x10, 16);
    const std::uint32_t refs = 0x40000000u;
    std::memcpy(o.b + 0x18, &refs, 4);
    std::memcpy(o.b + 0x20, &hw, 8);
    c->hw = hw;
    return c;
}

// The upload is done (the game clears hw+0x98 when its queued copy completed) or 3 s passed.
bool Uploaded(Clone& c, double now_ms) {
    if (c.up_ms >= 0) return true;
    std::uint8_t busy = 1;
    Rd(c.hw + kHwBusy, busy);
    if (busy != 1 || now_ms - c.made_ms > 3000) {
        c.up_ms = now_ms;
        Log("[tex] clone %s %s: hw %p uploaded in %.0f ms%s", tx::kSlots[c.slot].id, tx::Hex(c.id, 4).c_str(), reinterpret_cast<void*>(c.hw),
            now_ms - c.made_ms, busy == 1 ? " (TIMEOUT: still marked busy, bound anyway)" : "");
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- the override core (as v0.23)
struct SetEntry {
    std::uint8_t pack[4];
    std::uint32_t pad;
    std::uintptr_t tex;
    std::uint8_t flags;
    std::uint8_t pad2[7];
};
static_assert(sizeof(SetEntry) == 0x18, "TextureSet entry");
struct SetDesc {
    std::uint32_t type;
    std::uint32_t pad;
    std::uintptr_t path;
    std::uint8_t active;
    std::uint8_t gamma;
    std::uint8_t rest[0x40 - 0x12];
};
static_assert(sizeof(SetDesc) == 0x40, "TextureSet desc");
struct FakeSet {
    std::uint8_t head[0x20];
    std::int32_t entries_n, entries_cap;
    SetEntry* entries;
    std::uint8_t mid[0x08];
    std::int32_t descs_n, descs_cap;
    SetDesc* descs;
    std::uint8_t tail[0x10];
};
struct NodeString {
    std::uint32_t refs = 0x40000000u;
    std::uint32_t crc = 0xFFFFFFFFu;
    std::uint32_t len = 25;
    std::uint32_t pad = 0;
    char text[32] = "inSampler_Main_TextureSet";
} g_node_str;
const char* g_node = g_node_str.text;
struct GameArray {
    std::int32_t n = 0, cap = 0;
    std::uint64_t* data = nullptr;
} g_arr;

bool SehCore(std::uintptr_t drawable, FakeSet* set, int mesh) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, const char**, FakeSet*, GameArray*, int)>(mem::Base() + kCore)(drawable, &g_node, set, &g_arr, mesh);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehUndo(std::uintptr_t drawable, std::uint64_t* h) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, std::uint64_t*)>(mem::Base() + kUndo)(drawable, h);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehRefresh(std::uintptr_t drawable) {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, char)>(mem::Base() + kRefresh)(drawable, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehSwizzle(SetEntry* e, std::uint16_t* out, std::uint32_t type) {
    __try {
        const std::uint16_t* p =
            reinterpret_cast<std::uint16_t* (*)(SetEntry*, std::uint16_t*, std::uint32_t)>(mem::Base() + kSwizzle)(e, out, type);
        *out = p ? *p : *out;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uintptr_t Drawable(std::uintptr_t e, int* meshes) {
    *meshes = 0;
    const std::uintptr_t a = Q(e + 0xB8);
    const std::uintptr_t bb = a ? Q(a + 0x40) : 0;
    const std::uintptr_t d = bb ? Q(bb + 0x28) : 0;
    if (!d) return 0;
    const std::uintptr_t m = Q(d + kDrMeshes);
    std::int32_t n = 0;
    if (!m || !Rd(m + 0x40, n) || n <= 0 || n > 256 || !Q(m + 0x48)) return 0;
    *meshes = n;
    return d;
}

// The override table of a handle (its table id at +2) in the drawable's sorted list, or 0.
std::uintptr_t TableOf(std::uintptr_t dr, std::uint64_t h) {
    const std::int16_t key = static_cast<std::int16_t>((h >> 16) & 0xFFFF);
    std::int32_t n = 0;
    Rd(dr + kDrOverrides, n);
    const std::uintptr_t arr = Q(dr + kDrOverrides + 8);
    for (int i = 0; i < n && i < 256 && arr; ++i) {
        std::int32_t k = 0;
        Rd(arr + 16ull * i, k);
        if (k == key) return Q(arr + 16ull * i + 8);
    }
    return 0;
}

void Unregister(Dress& d) {
    AcquireSRWLockExclusive(&g_reg_lock);
    for (unsigned i = 0; i < d.nreg; ++i)
        if (d.reg[i] >= 0 && d.reg[i] < kRegMax) g_reg[d.reg[i]] = Reg{};
    int any = 0;
    for (const Reg& r : g_reg) any += r.used ? 1 : 0;
    g_any.store(any, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_reg_lock);
    d.nreg = 0;
}

bool LookIdZero(const std::uint8_t id[32]) {
    for (int i = 0; i < 32; ++i)
        if (id[i]) return false;
    return true;
}

}  // namespace

// ================================================================ public
bool Available() noexcept {
    if (g.checked) return g.ok && !g.off;
    g.checked = true;
    InitGuids();
    bool ok = g_guid_ok;
    if (!ok) std::snprintf(g.why, sizeof(g.why), "the slot table's GUIDs do not parse");
    if (ok && !suitcol::Available()) {
        std::snprintf(g.why, sizeof(g.why), "v0.23's override / loader checks failed: %s", suitcol::Why());
        ok = false;
    }
    const std::uintptr_t b = mem::Base();
    ok = ok && Check("override rebuild", kRebuild, "48 89 5C 24 20 56 57 41 55 41 56 41 57 48 83 EC 60 48 8B 05") &&
         Check("rebuild: table+0 = nodes", kRebuildNodes, "48 63 59 10 45 33 ED 48 89 11 4C 8B F1") &&
         Check("rebuild: collect loop sentinel", kRebuildSkip1, "49 8B 4E 18 8B 05 ?? ?? ?? ?? 39 04 39 74 21") &&
         Check("rebuild: push loop sentinel", kRebuildSkip2, "49 8B 4E 18 8B 05 ?? ?? ?? ?? 49 03 CC 39 01 0F 84") &&
         Check("rebuild: kind 5 texture push", kRebuildKind5, "8B 71 18 83 FE 05 75 51 48 8B 69 30 0F B7 59 38") &&
         Check("override entry create", kCreateEntry, "48 89 5C 24 10 48 89 6C 24 18 56 41 54 41 55 41 56 41 57 48 83 EC 20") &&
         Check("entry create's collect call", kCreateCollectCall, "E8") &&
         Check("slot collect", kCollect, "40 55 56 57 41 56 48 81 EC F8 00 00 00 48 8B 05") &&
         Check("slot push", kPush, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 79 08") &&
         Check("swizzle from packing", kSwizzle, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B7 05") &&
         Check("upload ring size compare", kRingCmp, "48 8B 0D ?? ?? ?? ?? 4C 8B 45 F8 48 81 C1 48 0B 00 00 4C 3B 41 08") &&
         Check("slot record write", kSlotSet, "48 89 5C 24 10 57 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 68 48 8B DA") &&
         Check("slot record write: block-of call", kSlotSet + 0x4F, "E8") &&
         Check("slot record address", kSlotSetLayout,
               "4C 8B 00 49 83 E0 F0 41 8B 48 04 8B D1 41 8B 00 C1 E8 0E 83 E0 07 C1 EA 04 83 E2 0F 03 D0 8B C1 C1 E8 18 83 E1 0F 83 E0 03 "
               "83 C1 02 03 C1 8D 0C 40 48 8B C3 48 C1 E8 20 03 C9 C1 E8 06 83 E0 7F 8D 14 91 48 8D 0C 80 49 8D 04 10 0F B7 54 48 04 33 C9 "
               "48 8B 44 24 58 48 8B 00 48 83 E0 F0 48 83 C0 10 48 03 C2 74 0A 48 C1 EB 2D 0F B7 CB 48 03 C8") &&
         Check("slot block of a binding", kSlotBlockOf, "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 48 89 54 24 10 57 41 54 41 55 41 56 41 57 48 83 EC") &&
         Check("slot block ranges", kSlotBlockOf + 0xD6,
               "4C 8B 5E 38 45 8B CE 4D 8B C3 41 C1 E9 06 49 83 E0 F0 41 83 E1 7F 41 0F B6 40 07 41 8B 08 83 E0 03 83 C0 02 C1 E9 0E 83 E1 07 "
               "8D 04 40 03 C0 8D 14 88 40 0F B6 C7 83 E0 0F 48 8D 0C 40 49 8D 04 10 8B 14 48 8B F2 C1 EE 11 8B C2 40 80 E6 7F C1 E8 18 C1 EA 10 F6 C2 01");
    ok = ok && Check("Texture release (GUID cache)", kTexRelease, "48 85 D2 74 16 8B 42 38 83 E0 28 3C 28 75 0C 48 81 C1 F8 CD 02 00 E8") &&
         Check("GUID cache release", kCacheRelease, "48 85 D2 74 5D 48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 89 5C 24 30 48") &&
         Check("2D texture deleting destructor", kHwDtor, "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8");
    std::uintptr_t hw_dtor = 0;
    if (ok && (mem::RipTarget(b + kTexReleaseCall, 1, 5) != b + kCacheRelease || !Rd(b + kHwTexVtbl, hw_dtor) || hw_dtor != b + kHwDtor)) {
        std::snprintf(g.why, sizeof(g.why), "the Texture release does not lead where expected");
        ok = false;
    }
    std::uintptr_t slot_f68 = 0;
    if (ok && (!Rd(b + kSlotVtbl + 0x68, slot_f68) || slot_f68 != b + kSlotSet || mem::RipTarget(b + kSlotSet + 0x4F, 1, 5) != b + kSlotBlockOf)) {
        std::snprintf(g.why, sizeof(g.why), "the slot class's record write is not where expected");
        ok = false;
    }
    if (ok && (mem::RipTarget(b + kRebuildSkip1 + 4, 2, 6) != b + kSentinel || mem::RipTarget(b + kRebuildSkip2 + 4, 2, 6) != b + kSentinel ||
               mem::RipTarget(b + kCreateCollectCall, 1, 5) != b + kCollect || mem::RipTarget(b + kRingCmp, 3, 7) != b + kRenderer)) {
        std::snprintf(g.why, sizeof(g.why), "the rebuild's sentinel / collect / ring references do not lead where expected");
        ok = false;
    }
    if (ok && g.configured && g.cfg.on && !InstallDetour()) ok = false;
    if (ok) std::snprintf(g.why, sizeof(g.why), "ok");
    Log("[tex] Fragile's textures %s%s", ok ? "available (override rebuild, slot collect / push, loader and ring verified)" : "unavailable: ",
        ok ? "" : g.why);
    g.ok = ok;
    return ok;
}

const char* Why() noexcept { return g.off ? "off after a fault or a refused guard" : g.why; }

void NoteGameThread() noexcept { g.game_tid = GetCurrentThreadId(); }

namespace {
bool SehRelease(std::uintptr_t r, std::uintptr_t hw) {
    __try {
        const std::uint32_t m = *reinterpret_cast<const volatile std::uint32_t*>(hw + kHwMips);
        if ((m & 0x28u) == 0x28u) reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(mem::Base() + kCacheRelease)(r + kGuidCache, hw);
        if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(hw + 8)) == 0) {
            const std::uintptr_t vt = *reinterpret_cast<const std::uintptr_t*>(hw);
            reinterpret_cast<void (*)(std::uintptr_t, int)>(*reinterpret_cast<const std::uintptr_t*>(vt))(hw, 1);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
unsigned g_freed = 0;
bool g_force_free = false;

// One texture of ours released the way the game releases a Texture's (game thread). Nothing binds it any more: our
// overrides that used it were undone (their entries no longer match the detour), the bodies that wore it are gone.
void Release(Clone& c, const char* why) {
    const std::uintptr_t r = Q(mem::Base() + kRenderer);
    if (!r || !c.hw) return;
    const std::uintptr_t hw = c.hw;
    if (!SehRelease(r, hw)) return Fault("the texture release");
    ++g_freed;
    Log("[tex] clone %s %s: hw %p released (%s; %u released this session)", c.slot >= 0 ? tx::kSlots[c.slot].id : "?", tx::Hex(c.id, 4).c_str(),
        reinterpret_cast<void*>(hw), why, g_freed);
    std::uintptr_t zero = 0;
    std::memcpy(c.obj.b + 0x20, &zero, 8);
    c = Clone{};
}
}  // namespace

void FreeTick(double now_ms, bool world_ready) noexcept {
    if (!g.checked || !g.ok || g.off || !world_ready) return;
    const bool force = g_force_free;
    g_force_free = false;
    if (!g.cfg.free_unused && !force) return;
    for (Clone& c : g_clones) {
        if (!c.used || !c.hw || c.up_ms < 0) continue;
        if (force ? now_ms - c.used_ms > 600 : (now_ms - c.used_ms > 5000 && now_ms - c.made_ms > 5000)) {
            Release(c, force ? "tex 6" : "unused for 5 s");
            if (!force) return;  // one per tick
        }
    }
}

void Configure(const Settings& s) noexcept {
    g.cfg = s;
    g.configured = true;
    looks::Configure(s);
    Log("[tex] Fragile's textures %s (session.ini fragile_textures=%d), folder %s, hot reload %d, send %d, receive %d, ready-made colours %d", s.on ? "on" : "off",
        s.on ? 1 : 0, s.dir.empty() ? "- (none)" : "set", s.hot_reload ? 1 : 0, s.send ? 1 : 0, s.receive ? 1 : 0, s.defaults ? 1 : 0);
}

bool Enabled() noexcept { return g.configured && g.cfg.on && !g.user_off && Available(); }

int ForcedColour() noexcept { return g.forced_colour; }
int ForcedBoots() noexcept { return g.forced_boots; }
bool PatternOn() noexcept { return g.pattern != 0; }

Stats GetStats() noexcept {
    Stats s;
    s.rebuilds = g_rebuilds.load();
    s.kept = g_kept.load();
    s.dropped = g_dropped.load();
    s.faults = g_faults.load();
    s.made = g_made;
    s.make_failed = g_make_failed;
    s.built = looks::BuiltCount();
    return s;
}

void Forget(Dress& d) noexcept {
    Unregister(d);
    const char* who = d.who;
    d = Dress{};
    d.who = who;
}

void Undo(Dress& d, const char* why) noexcept {
    Unregister(d);
    d.nbound = 0;
    if (!d.nh) {
        d.bound = false;
        return;
    }
    int meshes = 0;
    const std::uintptr_t dr = d.e ? Drawable(d.e, &meshes) : 0;
    if (!dr || dr != d.drawable) {
        Log("[tex] body %s %p: %u override(s) not undone (%s): the drawable is gone or changed", d.who, reinterpret_cast<void*>(d.e), d.nh, why);
        d.nh = 0;
        d.bound = false;
        std::memset(d.applied, 0, 32);
        return;
    }
    for (unsigned i = 0; i < d.nh; ++i)
        if (!SehUndo(dr, &d.handles[i])) return Fault("the override undo");
    if (!SehRefresh(dr)) return Fault("the override refresh");
    ++d.undos;
    Log("[tex] body %s %p: look %s taken off (%u override(s) undone, %s)", d.who, reinterpret_cast<void*>(d.e), tx::Hex(d.applied, 4).c_str(), d.nh, why);
    d.nh = 0;
    d.bound = false;
    std::memset(d.applied, 0, 32);
}

void Keep(Dress& d, std::uintptr_t e, double now_ms) noexcept {
    if (!e || d.e != e) return;
    for (unsigned i = 0; i < d.nbound; ++i)
        if (d.bound_clones[i]) static_cast<Clone*>(d.bound_clones[i])->used_ms = now_ms;
}

bool Apply(Dress& d, std::uintptr_t e, const Look& look, double now_ms) noexcept {
    if (!e) return false;
    if (d.e != e) {
        Forget(d);
        d.e = e;
    }
    if (!Enabled() || g.off) {
        if (d.nh) Undo(d, "feature off");
        return false;
    }
    for (unsigned i = 0; i < d.nbound; ++i)
        if (d.bound_clones[i]) static_cast<Clone*>(d.bound_clones[i])->used_ms = now_ms;
    for (std::size_t i = 0; i < look.slots.size(); ++i)
        if (Clone* w = FindClone(look.blob_ids.data() + 32 * i)) w->used_ms = now_ms;
    const bool want_empty = look.Empty();
    int meshes = 0;
    const std::uintptr_t dr = Drawable(e, &meshes);
    if (d.nh && dr != d.drawable) {
        Log("[tex] body %s %p: its drawable changed (%p -> %p): the overrides went with the old one", d.who, reinterpret_cast<void*>(e),
            reinterpret_cast<void*>(d.drawable), reinterpret_cast<void*>(dr));
        Unregister(d);
        d.nh = 0;
        d.bound = false;
        std::memset(d.applied, 0, 32);
    }
    // a bound look waits for its first rebuild (1 s): no rebuild means no textures of ours, so take it off again
    if (d.bound && d.waiting_rebuild) {
        unsigned seen = 0, kept = 0, dropped = 0;
        AcquireSRWLockShared(&g_reg_lock);
        for (unsigned i = 0; i < d.nreg; ++i) {
            const Reg& r = g_reg[d.reg[i]];
            if (r.used && r.seen) {
                ++seen;
                kept += r.kept;
                dropped += r.dropped;
            }
        }
        ReleaseSRWLockShared(&g_reg_lock);
        if (seen == d.nreg && d.nreg) {
            d.waiting_rebuild = false;
            Log("[tex] body %s %p: look %s bound (%u entr%s), rebuild kept %u / dropped %u slot(s) in %.0f ms", d.who, reinterpret_cast<void*>(e),
                tx::Hex(d.applied, 4).c_str(), d.nreg, d.nreg == 1 ? "y" : "ies", kept, dropped, now_ms - d.bound_ms);
        } else if (now_ms - d.bound_ms > 1000) {
            Log("[tex] body %s %p: no rebuild of its override table within 1 s (%u of %u entries): look taken off", d.who, reinterpret_cast<void*>(e), seen,
                d.nreg);
            Undo(d, "no rebuild");
            return false;
        }
    }
    if (now_ms < d.next_ms) return d.nh != 0 || !want_empty;
    d.next_ms = now_ms + 250;
    std::uint8_t want[32] = {};
    if (!want_empty) std::memcpy(want, look.id, 32);
    if (!d.pending_set || std::memcmp(want, d.pending, 32) != 0) {
        std::memcpy(d.pending, want, 32);
        d.pending_set = true;
        d.pending_ms = now_ms;
    }
    const bool same = d.bound ? std::memcmp(want, d.applied, 32) == 0 : want_empty;
    if (same || now_ms - d.pending_ms < 500) return d.nh != 0 || !want_empty;
    if (want_empty) {
        if (d.nh) Undo(d, "her own look");
        return false;
    }
    if (!dr) {
        if (d.refused_logged++ == 0) Log("[tex] body %s %p: no model drawable yet", d.who, reinterpret_cast<void*>(e));
        return true;
    }
    // every texture of the look made and uploaded first (the old look stays meanwhile); one made per call
    Clone* cl[8] = {};
    const std::size_t n = look.slots.size() < 8 ? look.slots.size() : 8;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* id = look.blob_ids.data() + 32 * i;
        Clone* c = FindClone(id);
        if (!c) {
            if (now_ms - g_last_make_ms < 250) return true;
            std::vector<std::uint8_t> bytes;
            if (!BlobBytes(id, &bytes)) return true;  // not here yet (the link or the worker brings it)
            g_last_make_ms = now_ms;
            if (!Make(id, look.slots[i], bytes, now_ms)) return d.nh != 0;
            return true;
        }
        if (c->failed || !c->hw) return d.nh != 0;
        if (!Uploaded(*c, now_ms)) return true;
        cl[i] = c;
    }
    if (d.nh) {
        Undo(d, "look change");
        return true;
    }
    // bind: per slot, per map of its packing, per mesh: an entry with a null Texture (the core binds nothing), then ours
    // registered for the detour; the refresh makes the drawable rebuild, the detour pushes ours into the kept slots.
    const std::uintptr_t empty_path = mem::Base() + kEmptyString + 0x10;
    struct Pending {
        std::uint64_t h;
        int slot;
        std::uint32_t type;
        Clone* c;
    } pend[32];
    unsigned np = 0;
    d.drawable = dr;
    for (std::size_t i = 0; i < n; ++i) {
        const tx::Slot& s = tx::kSlots[look.slots[i]];
        for (const tx::PackedMap& m : tx::MapsOf(s.packing)) {
            SetEntry en{};
            for (int k = 0; k < 4; ++k) en.pack[k] = static_cast<std::uint8_t>(s.packing >> (8 * k));
            en.tex = 0;
            en.flags = 1;
            SetDesc ds{};
            ds.type = m.type;
            ds.path = empty_path;
            ds.active = 1;
            ds.gamma = m.type == tx::kTypeColor ? 1 : 0;
            FakeSet set{};
            set.entries_n = set.entries_cap = 1;
            set.entries = &en;
            set.descs_n = set.descs_cap = 1;
            set.descs = &ds;
            for (int mesh = 0; mesh < meshes; ++mesh) {
                g_arr.n = 0;
                if (!SehCore(dr, &set, mesh)) {
                    Fault("the texture-set override core");
                    return false;
                }
                for (int k = 0; k < g_arr.n && g_arr.data; ++k) {
                    if (np < 32 && d.nh < 32) {
                        pend[np++] = Pending{g_arr.data[k], look.slots[i], m.type, cl[i]};
                        d.handles[d.nh++] = g_arr.data[k];
                    }
                }
                g_arr.n = 0;
            }
        }
    }
    AcquireSRWLockExclusive(&g_reg_lock);
    unsigned nreg = 0;
    for (unsigned i = 0; i < np && nreg < 24; ++i) {  // every registered entry is tracked by the body (Unregister)
        const std::uintptr_t table = TableOf(dr, pend[i].h);
        const int index = static_cast<std::int16_t>(pend[i].h & 0xFFFF);
        std::int32_t count = 0;
        if (!table || !Rd(table + 0x10, count) || index < 0 || index >= count) continue;
        const std::uintptr_t entry = Q(table + 0x18) + static_cast<std::uintptr_t>(index) * 0x40;
        std::uint32_t hash = 0;
        Rd(entry, hash);
        SetEntry real{};
        const tx::Slot& s = tx::kSlots[pend[i].slot];
        for (int k = 0; k < 4; ++k) real.pack[k] = static_cast<std::uint8_t>(s.packing >> (8 * k));
        real.tex = reinterpret_cast<std::uintptr_t>(&pend[i].c->obj);
        real.flags = 1;
        std::uint16_t swz = 0;
        if (!SehSwizzle(&real, &swz, pend[i].type)) {
            ReleaseSRWLockExclusive(&g_reg_lock);
            Fault("the swizzle from the packing");
            return false;
        }
        for (int r = 0; r < kRegMax; ++r)
            if (!g_reg[r].used) {
                Reg& x = g_reg[r];
                x = Reg{};
                x.used = true;
                x.table = table;
                x.drawable = dr;
                x.tex = real.tex;
                x.index = index;
                x.hash = hash;
                x.swz = swz;
                x.rule = s.group == tx::SlotGroup::suit ? 0 : 1;
                if (nreg < 24) d.reg[nreg++] = r;
                break;
            }
    }
    d.nreg = nreg;
    int any = 0;
    for (const Reg& r : g_reg) any += r.used ? 1 : 0;
    g_any.store(any, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_reg_lock);
    if (!SehRefresh(dr)) {
        Fault("the override refresh");
        return false;
    }
    ++d.applies;
    d.nbound = 0;
    for (std::size_t i = 0; i < n && i < 8; ++i) d.bound_clones[d.nbound++] = cl[i];
    d.bound = true;
    d.waiting_rebuild = true;
    d.bound_ms = now_ms;
    std::memcpy(d.applied, want, 32);
    char names[64] = {};
    int k = 0;
    for (std::size_t i = 0; i < n; ++i) k += std::snprintf(names + k, sizeof(names) - k, "%s%s", i ? " " : "", tx::kSlots[look.slots[i]].id);
    Log("[tex] body %s %p: look %s (colour %d) binding %s: %u override(s), %u filtered entr%s, drawable %p (%d mesh%s)", d.who, reinterpret_cast<void*>(e),
        tx::Hex(want, 4).c_str(), look.colour, names, d.nh, nreg, nreg == 1 ? "y" : "ies", reinterpret_cast<void*>(dr), meshes, meshes == 1 ? "" : "es");
    return true;
}

const char* Describe(const Dress& d, char* buf, std::size_t cap) noexcept {
    std::snprintf(buf, cap, "%s %p look %s%s overrides %u entries %u applies %u undos %u", d.who, reinterpret_cast<void*>(d.e),
                  d.bound ? tx::Hex(d.applied, 4).c_str() : "-", d.waiting_rebuild ? " (waiting for a rebuild)" : "", d.nh, d.nreg, d.applies, d.undos);
    return buf;
}

unsigned StoreEvicted() noexcept;  // the blob store (below)

bool DebugCommand(int mode, double a, double b2, std::uintptr_t e_self, std::uintptr_t e_slot, char* msg, std::size_t cap) noexcept {
    const bool hidden = automation::Enabled();
    switch (mode) {
    case 0: {
        const Stats s = GetStats();
        char lk[400];
        looks::Describe(lk, sizeof(lk));
        std::snprintf(msg, cap, "%s; ring %llu; rebuilds %u kept %u dropped %u faults %u; clones %u (failed %u); filter %d; blobs in RAM %.1f MB (%u let go); %s",
                      Available() ? (g.off ? "OFF" : "available") : Why(), static_cast<unsigned long long>(RingSize()), s.rebuilds, s.kept, s.dropped, s.faults,
                      s.made, s.make_failed, g_filter.load(), StoreRamBytes() / 1048576.0, StoreEvicted(), lk);
        Log("[tex] status: %s", msg);
        for (const Clone& c : g_clones)
            if (c.used)
                Log("[tex]   clone %s %s hw %p uploaded %s", c.slot >= 0 ? tx::kSlots[c.slot].id : "?", tx::Hex(c.id, 4).c_str(), reinterpret_cast<void*>(c.hw),
                    c.up_ms >= 0 ? "yes" : "no");
        AcquireSRWLockShared(&g_reg_lock);
        for (const Reg& r : g_reg)
            if (r.used)
                Log("[tex]   entry table %p #%d hash %08X rule %s swizzle %04X: %u rebuild(s), kept %u dropped %u", reinterpret_cast<void*>(r.table), r.index, r.hash,
                    r.rule ? "boots" : "suit", r.swz, r.seen, r.kept, r.dropped);
        ReleaseSRWLockShared(&g_reg_lock);
        return true;
    }
    case 1:
        looks::Rescan();
        std::snprintf(msg, cap, "rescan requested");
        return true;
    case 2:
        if (!hidden) return std::snprintf(msg, cap, "refused: hidden test runs only"), false;
        g_probe.store(a > 0 ? static_cast<int>(a) : 2);
        std::snprintf(msg, cap, "probe: the next %d rebuild(s) of our tables are dumped", g_probe.load());
        return true;
    case 3:
        if (!hidden) return std::snprintf(msg, cap, "refused: hidden test runs only"), false;
        g.forced_colour = a >= 0 && a <= 16 ? static_cast<int>(a) : -1;
        std::snprintf(msg, cap, "colour %d for our own Fragile's textures (-1 follow)", g.forced_colour);
        return true;
    case 4:
        g.user_off = a <= 0;
        std::snprintf(msg, cap, "Fragile's textures %s", g.user_off ? "off (undone at the next step)" : "on");
        return true;
    case 5:
        if (!hidden && a != 0) return std::snprintf(msg, cap, "refused: hidden test runs only"), false;
        g_filter.store(a >= 0 && a <= 2 ? static_cast<int>(a) : 0);
        std::snprintf(msg, cap, "filter mode %d (0 product, 1 keep all, 2 keep all + log)", g_filter.load());
        return true;
    case 6:
        if (!hidden) return std::snprintf(msg, cap, "refused: hidden test runs only"), false;
        g_force_free = true;
        std::snprintf(msg, cap, "every texture of ours unused for 0.6 s is released at the next tick");
        return true;
    case 7:
        looks::Resend();
        std::snprintf(msg, cap, "look resend / partner cache drop requested");
        return true;
    case 10:
        if (!hidden) return std::snprintf(msg, cap, "refused: hidden test runs only"), false;
        g.forced_boots = a >= 0 && a <= 10 ? static_cast<int>(a) : -1;
        std::snprintf(msg, cap, "boot type %d for our own Fragile's boots pictures (-1 follow our Sam's)", g.forced_boots);
        return true;
    case 11:
        if (!hidden) return std::snprintf(msg, cap, "refused: hidden test runs only"), false;
        looks::SetVariant(static_cast<int>(a));
        std::snprintf(msg, cap, "ready-made gold / silver: metal variant %d (0 the product); the looks are rebuilt", looks::Variant());
        return true;
    case 12:
        if (!hidden) return std::snprintf(msg, cap, "refused: hidden test runs only"), false;
        looks::SetMakeMode(static_cast<int>(a));
        std::snprintf(msg, cap, "host: a guest's ready-made colour is %s", looks::MakeMode() == 1   ? "always sent over the link (tex 12 1)"
                                                                          : looks::MakeMode() == 2 ? "refused as if the originals differed (tex 12 2)"
                                                                          : looks::MakeMode() == 3 ? "made here, each make 2 s slower (tex 12 3)"
                                                                          : looks::MakeMode() == 4 ? "made here; the next make fails as out of memory (tex 12 4)"
                                                                                                   : "made here when the originals match (tex 12 0)");
        return true;
    case 9:
        if (!hidden) return std::snprintf(msg, cap, "refused: hidden test runs only"), false;
        g.pattern = a > 0 ? static_cast<int>(a) : 0;
        looks::SetPattern(g.pattern);
        std::snprintf(msg, cap, "test pattern %d on our own Fragile bodies", g.pattern);
        return true;
    default:
        break;
    }
    (void)b2;
    (void)e_self;
    (void)e_slot;
    std::snprintf(msg, cap, "unknown tex mode %d", mode);
    return false;
}

// ---------------------------------------------------------------- the blob store (built here, received over the link)
// RAM is bounded (kStoreRamMax, least recently used out first): a blob with a disk file (the built cache, the partner
// cache) keeps only its entry and is read again (and validated against its id) when needed; one without a file stays.
namespace {
struct StoreEntry {
    int slot = -1;
    std::vector<std::uint8_t> bytes;  // empty: let go of (read again from `path`)
    std::wstring path;
    std::uint64_t used = 0;           // g_store_clock at the last store / read
};
SRWLOCK g_store_lock = SRWLOCK_INIT;
std::map<std::string, StoreEntry> g_store;  // key: blob id hex
std::size_t g_store_ram = 0;
std::uint64_t g_store_clock = 0;
unsigned g_store_evicted = 0;

bool FileThere(const std::wstring& p) {
    const DWORD a = p.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadBlobFile(const std::wstring& path, std::vector<std::uint8_t>& out) {
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= (16ll << 20);
    if (ok) {
        out.resize(static_cast<std::size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

// g_store_lock held exclusively: lets go of the least recently used RAM copies that have a file until RAM fits.
void TrimStoreLocked() {
    while (g_store_ram > kStoreRamMax) {
        StoreEntry* oldest = nullptr;
        for (auto& kv : g_store)
            if (!kv.second.bytes.empty() && !kv.second.path.empty() && (!oldest || kv.second.used < oldest->used)) oldest = &kv.second;
        if (!oldest) break;  // only blobs without a file are left: they stay
        g_store_ram -= oldest->bytes.size();
        std::vector<std::uint8_t>().swap(oldest->bytes);
        ++g_store_evicted;
    }
}
}  // namespace

bool BlobBytes(const std::uint8_t id[32], std::vector<std::uint8_t>* out) noexcept {
    bool locked = false;
    try {
        const std::string key = tx::Hex(id, 32);
        std::wstring path;
        int slot = -1;
        AcquireSRWLockExclusive(&g_store_lock);
        locked = true;
        auto it = g_store.find(key);
        if (it != g_store.end()) {
            if (!it->second.bytes.empty()) {
                it->second.used = ++g_store_clock;
                *out = it->second.bytes;
                locked = false;
                ReleaseSRWLockExclusive(&g_store_lock);
                return true;
            }
            path = it->second.path;
            slot = it->second.slot;
        }
        locked = false;
        ReleaseSRWLockExclusive(&g_store_lock);
        if (path.empty() || slot < 0 || slot >= static_cast<int>(tx::kSlotCount)) return false;
        // let go of earlier: read again from its file (outside the lock), validated against its id
        std::vector<std::uint8_t> b;
        tx::Sha256 want{};
        std::memcpy(want.data(), id, 32);
        std::string why;
        const bool ok = ReadBlobFile(path, b) && tx::ValidateBlob(tx::kSlots[slot], b.data(), b.size(), &want, &why);
        if (!ok) {
            Log("[tex] blob %s: its file is gone or changed (%s): it is built or received again", tx::Hex(id, 4).c_str(), why.empty() ? "not readable" : why.c_str());
            AcquireSRWLockExclusive(&g_store_lock);
            auto jt = g_store.find(key);
            if (jt != g_store.end() && jt->second.bytes.empty()) g_store.erase(jt);
            ReleaseSRWLockExclusive(&g_store_lock);
            return false;
        }
        *out = b;
        StoreBlob(id, slot, std::move(b), path);
        return true;
    } catch (...) {
        if (locked) ReleaseSRWLockExclusive(&g_store_lock);
        Log("[tex] a blob could not be read: out of memory");
        return false;
    }
}

void StoreBlob(const std::uint8_t id[32], int slot, std::vector<std::uint8_t>&& bytes, const std::wstring& path) noexcept {
    bool locked = false;
    try {
        const std::string key = tx::Hex(id, 32);
        AcquireSRWLockExclusive(&g_store_lock);
        locked = true;
        StoreEntry& e = g_store[key];
        g_store_ram -= e.bytes.size();
        e.slot = slot;
        e.bytes = std::move(bytes);
        if (!path.empty()) e.path = path;
        e.used = ++g_store_clock;
        g_store_ram += e.bytes.size();
        TrimStoreLocked();
        locked = false;
        ReleaseSRWLockExclusive(&g_store_lock);
    } catch (...) {
        if (locked) ReleaseSRWLockExclusive(&g_store_lock);
        Log("[tex] a blob could not be stored: out of memory (dropped)");
    }
}

void StoreReceived(const std::uint8_t id[32], int slot, std::vector<std::uint8_t>&& bytes, const std::wstring& path) noexcept {
    StoreBlob(id, slot, std::move(bytes), path);
}

bool HaveBlob(const std::uint8_t id[32]) noexcept {
    bool locked = false;
    try {
        const std::string key = tx::Hex(id, 32);
        std::wstring path;
        AcquireSRWLockShared(&g_store_lock);
        locked = true;
        auto it = g_store.find(key);
        const bool ok = it != g_store.end() && !it->second.bytes.empty();
        if (it != g_store.end() && !ok) path = it->second.path;
        locked = false;
        ReleaseSRWLockShared(&g_store_lock);
        return ok || FileThere(path);
    } catch (...) {
        if (locked) ReleaseSRWLockShared(&g_store_lock);
        return false;
    }
}

unsigned DropRam(const std::vector<const std::uint8_t*>& ids) noexcept {
    unsigned n = 0;
    AcquireSRWLockExclusive(&g_store_lock);
    for (const std::uint8_t* id : ids) {
        auto it = g_store.find(tx::Hex(id, 32));
        if (it == g_store.end() || it->second.bytes.empty() || it->second.path.empty()) continue;
        g_store_ram -= it->second.bytes.size();
        std::vector<std::uint8_t>().swap(it->second.bytes);
        ++n;
    }
    ReleaseSRWLockExclusive(&g_store_lock);
    return n;
}

unsigned StoreEvicted() noexcept { return g_store_evicted; }

std::size_t StoreRamBytes() noexcept {
    AcquireSRWLockShared(&g_store_lock);
    const std::size_t n = g_store_ram;
    ReleaseSRWLockShared(&g_store_lock);
    return n;
}

}  // namespace st::ftex
