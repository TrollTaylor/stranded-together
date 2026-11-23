#include "face_sync.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "face_rules.hpp"
#include "log.hpp"
#include "partner_source.hpp"

namespace st::facesync {
namespace {

namespace mem = partner::mem;
namespace fr = facerules;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs; Ghidra, round 11)
constexpr std::uintptr_t kPlayFacial = 0x22CC710;  // DSFacialRigComponent::PlayFacial(comp, anim, start, blend, blink, saccade)
constexpr std::uintptr_t kRepRtti = 0x4AE7600;     // DSFacialRigComponentRep (its GetRTTI 0x1422CF770)
// DSFacialRigComponent: +0x40 its rep; +0x48 its entity (EntityComponent, as every component the mod checks).
constexpr std::uintptr_t kCompRep = 0x40, kCompOwner = 0x48;
// DSFacialRigComponentRep: the clip slots the game's own getter reads (0x1422CF730): +0x300 (the rig path of
// 0x1422D1340), else +0xF0; [+0x2E8]+0x80 the clip blink needs (0x1422CEAE0); +0x573 flags (0x10 blink, 0x08 saccade,
// 0x1422D1510); +0x534 the blink count (0..5, one per blink) and +0x538 the time since the last one (0x1422CEAE0).
constexpr std::uintptr_t kRepSlotRig = 0x300, kRepSlotMesh = 0xF0, kRepPlayer = 0x2E8, kPlayerClip = 0x80;
constexpr std::uintptr_t kRepFlags = 0x573, kRepBlinkN = 0x534, kRepBlinkT = 0x538;
// DSFacialRigManagerComponent (face_rules.hpp).
constexpr std::uintptr_t kMgrRes = 0x30, kMgrEnt = 0x48, kMgrOverride = 0x50, kMgrPhoto = 0x5C, kMgrOvBlink = 0x5E, kMgrOvSacc = 0x5F;
constexpr std::uintptr_t kMgrEventKey = 0x60, kMgrState = 0x64, kMgrFlags = 0x68;
constexpr std::uintptr_t kResMgrRes = 0x20, kResPhotoN = 0x28, kResPhotoData = 0x30;
constexpr std::uintptr_t kMresEventN = 0x20, kMresEventData = 0x28, kMresStateN = 0x30, kMresStateData = 0x38;
constexpr std::uintptr_t kEventStride = 0x28, kStateStride = 0x18;
constexpr std::uintptr_t kClipRefs = 0x40;  // SkeletonAnimationResource: PlayFacial keeps a clip only when this is non-zero
constexpr unsigned kPhotoGameN = 18;        // 0x142740E10: index < 0x12

struct Site {
    const char* name;
    std::uintptr_t rva;
    const char* sig;
};
// Every byte the reads and the call rely on: PlayFacial and its lambda (the flag bits), the rig's clip slots, the
// manager's state / event / photo / override code (the table layout and the flags above), the blink update (the meter).
constexpr Site kSites[] = {
    {"PlayFacial", 0x22CC710,
     "40 53 48 81 EC 80 00 00 00 48 8B 05 10 49 C9 02 48 33 C4 48 89 44 24 70 48 8B D9 48 89 54 24 50 48 8D 4C 24 58 C5 FA 11 54 24 68 "
     "C5 FA 11 5C 24 60 E8 8A 23 10 00 C7 44 24 48 00 00 00 00 48 8D 05 BB 4D 00 00 48 89 44 24 40 4C 8D 4C 24 50 C5 F8 10 44 24 40 48 "
     "8D 84 24 B8 00 00 00 33 D2 48"},
    {"PlayFacial (tail)", 0x22CC770,
     "89 44 24 38 4C 8D 44 24 40 48 8D 84 24 B0 00 00 00 48 8B CB 48 89 44 24 30 48 8D 44 24 60 48 89 44 24 28 48 8D 44 24 68 48 89 44 24 "
     "20 C5 F9 7F 44 24 40 E8 98 DC FF FF 48 8D 4C 24 58 E8 EE 2C 10 00 48 8B 4C 24 70 48 33 CC E8 21 6B 83 01 48 81 C4 80 00 00 00 5B C3"},
    {"PlayFacial's lambda", 0x22D1510,
     "40 53 48 83 EC 20 48 8B D9 E8 22 FE FF FF 80 A3 73 05 00 00 EF 0F B6 54 24 50 0F B6 44 24 58 C0 E2 04 0A 93 73 05 00 00 80 E2 F7 C0 "
     "E0 03 0A D0 88 93 73 05 00 00 48 83 C4 20 5B C3"},
    {"the rig's clip slots", 0x22CF730,
     "48 83 B9 00 03 00 00 00 74 09 C5 FA 10 81 0C 03 00 00 C3 48 83 B9 F0 00 00 00 00 74 09 C5 FA 10 81 FC 00 00 00 C3"},
    {"the manager's state apply", 0x22D2D70,
     "48 83 EC 38 0F B6 41 68 4C 8B C9 A8 02 75 59 A8 01 75 55 8B 51 64 85 D2 79 05 33 D2 89 51 64 48 8B 41 30 48 8B 48 20 48 63 C2 48 8B "
     "51 38 49 8B 49 48 4C 8D 04 40 42 0F B6 44 C2 10 C4 A1 7A 10 5C C2 0C C4 A1 7A 10 54 C2 08 48 8B 89 80 01 00 00 88 44 24 28 42 0F B6 "
     "44 C2 11 4A 8B 14 C2 88"},
    {"the manager's state apply (tail)", 0x22D2DD0, "44 24 20 E8 F8 1A FE FF 48 83 C4 38 C3"},
    {"the manager's event clips", 0x22D2A50,
     "48 89 74 24 18 57 48 83 EC 30 F6 41 68 02 41 0F B6 F0 8B FA 0F 85 D7 00 00 00 48 8B 41 30 4C 8B 58 20 49 63 43 20 85 C0 0F 8E C3 00 "
     "00 00 45 33 D2 48 89 5C 24 48 33 D2 48 8B D8 0F 1F 44 00 00 4D 8B 43 28 42 39 7C 02 20 75 1F 0F B6 41 68 45 33 C9 F2 4E 0F 38 F1 4C "
     "02 08 40 84 F6 74 72 0C"},
    {"the manager's event clips (call)", 0x22D2AB0,
     "01 88 41 68 44 39 49 60 75 1C 49 FF C2 48 83 C2 28 4C 3B D3 7C CA 48 8B 5C 24 48 48 8B 74 24 50 48 83 C4 30 5F C3 44 89 49 60 42 0F "
     "B6 44 02 19 48 8B 49 48 C4 A1 7A 10 5C 02 14 C4 A1 7A 10 54 02 10 88 44 24 28 42 0F B6 44 02 18 48 8B 89 80 01 00 00 4A 8B 54 02 08 "
     "88 44 24 20 E8 BF 1D FE"},
    {"the photo-mode clips", 0x2740E10, "80 FA 12 7C 03 32 C0 C3 48 8B 41 30 48 0F BE D2 48 8B 40 30 48 8B 14 D0 E9"},
    {"the manager's override", 0x22D2F10,
     "40 53 57 48 83 EC 58 0F B6 41 68 48 8B FA 24 FE C5 F8 29 74 24 40 0C 02 C5 F8 29 7C 24 30 88 41 68 48 8B D9 C7 41 60 00 00 00 00 C5 "
     "F8 28 FB C5 F8 28 F2 48 85 D2 74 0A B8 01 00 00 00 F0 0F C1 42 18 48 8B 49 50 48 85 C9 74 05 E8 F0 2E 72 FF 44 0F B6 84 24 98 00 00 "
     "00 48 8B D7 0F B6 84 24"},
    {"the manager's override (flags)", 0x22D2F70, "90 00 00 00 48 89 7B 50 48 8B 4B 48 44 88 43 5F 88 43 5E"},
};
// The blink meter only (not needed for the face): the blink update's reads.
constexpr Site kBlinkSite = {"the blink update", 0x22CEAE0,
                             "48 8B 81 E8 02 00 00 4C 8D 89 73 05 00 00 C5 F8 28 E9 48 8B D1 48 83 B8 80 00 00 00 00 74 08 41 0F B6 01 A8 10"};

bool g_checked = false, g_ok = false, g_blink_ok = false, g_off = false;
char g_why[200] = "not checked";
int g_mode = 3;  // `self_view 20`: bit 1 self, bit 2 partner

bool CheckSites() {
    if (g_checked) return g_ok;
    g_checked = true;
    const std::uintptr_t b = mem::Base();
    for (const Site& s : kSites)
        if (!mem::CodeMatches(b + s.rva, s.sig)) {
            std::snprintf(g_why, sizeof(g_why), "%s at ds.exe+0x%llX does not match", s.name, static_cast<unsigned long long>(s.rva));
            Log("[face] Fragile's face sync off: %s (another game version?)", g_why);
            return g_ok = false;
        }
    g_blink_ok = mem::CodeMatches(b + kBlinkSite.rva, kBlinkSite.sig);
    std::snprintf(g_why, sizeof(g_why), "ok");
    Log("[face] sites verified: PlayFacial 0x%llX, its lambda, the rig's clip slots, the manager's state / event / photo / override code "
        "(%u checks); blink meter %s",
        static_cast<unsigned long long>(b + kPlayFacial), static_cast<unsigned>(sizeof(kSites) / sizeof(kSites[0])),
        g_blink_ok ? "on" : "off (the blink update does not match)");
    return g_ok = true;
}

std::uint8_t B(std::uintptr_t a) {
    std::uint8_t v = 0;
    Rd(a, v);
    return v;
}
std::int32_t I32(std::uintptr_t a, std::int32_t def = 0) {
    std::int32_t v = def;
    if (!Rd(a, v)) return def;
    return v;
}
float F32(std::uintptr_t a, float def) {
    float v = def;
    if (!Rd(a, v) || !(v >= 0.0f && v < 100.0f)) return def;
    return v;
}

// ---------------------------------------------------------------- clips
// A SkeletonAnimationResource the rig keeps (RTTI and the field PlayFacial checks); a small cache of the ones seen,
// a ring (the oldest goes first), emptied whenever Sam's tables are read again (a new manager or resource).
constexpr unsigned kClipCache = 64;
std::uintptr_t g_clip_ok[kClipCache] = {};
unsigned g_clip_ok_n = 0, g_clip_ok_next = 0;
void ClipCacheClear() { g_clip_ok_n = g_clip_ok_next = 0; }
bool ClipOk(std::uintptr_t a) {
    if (!a) return false;
    for (unsigned i = 0; i < g_clip_ok_n; ++i)
        if (g_clip_ok[i] == a) return true;
    if (!mem::DerivesFrom(mem::RttiOf(a), "SkeletonAnimationResource") || I32(a + kClipRefs) == 0) return false;
    g_clip_ok[g_clip_ok_next] = a;
    g_clip_ok_next = (g_clip_ok_next + 1) % kClipCache;
    if (g_clip_ok_n < kClipCache) ++g_clip_ok_n;
    return true;
}

struct Clip {
    std::uintptr_t anim = 0;
    float start = 0, blend = 0.3f;
    bool blink = false, saccade = false;  // PlayFacial's 5th / 6th arguments
};

// Sam's facial manager and its tables (the game's resource data: read once per manager).
struct Tables {
    std::uintptr_t sam = 0, mgr = 0, res = 0, mres = 0;
    bool ok = false;
    unsigned ns = 0, ne = 0, np = 0;
    Clip st[fr::kStateSlots];
    Clip ev[fr::kEventSlots];
    std::uint32_t ev_key[fr::kEventSlots] = {};
    std::uintptr_t ph[fr::kPhotoSlots] = {};
    std::uintptr_t skel = 0;  // the rig skeleton of state clip 0 (all of Sam's face clips)
} g_t;
std::uintptr_t g_t_failed_sam = 0;  // a Sam whose tables did not read (logged once; tried again every 5 s)
double g_t_failed_ms = 0;
double g_now_ms = 0;

bool ReadTables(std::uintptr_t sam) {
    if (g_t.ok && g_t.sam == sam) {
        // the same manager still (a new one when the player entity is rebuilt)
        if (Q(g_t.mgr + kMgrEnt) == sam && Q(g_t.mgr + kMgrRes) == g_t.res) return true;
    }
    if (g_t_failed_sam == sam && g_now_ms - g_t_failed_ms < 5000) return false;
    ClipCacheClear();  // clips of an older manager's resource are not trusted again
    Tables t;
    t.sam = sam;
    char why[120] = "";
    t.mgr = partner::FindComponentByName(reinterpret_cast<game::Entity*>(sam), "DSFacialRigManagerComponent");
    if (!t.mgr) std::snprintf(why, sizeof(why), "Sam has no DSFacialRigManagerComponent");
    else if (Q(t.mgr + kMgrEnt) != sam) std::snprintf(why, sizeof(why), "the manager's entity is not Sam");
    else if (!(t.res = Q(t.mgr + kMgrRes)) || !(t.mres = Q(t.res + kResMgrRes))) std::snprintf(why, sizeof(why), "the manager's resource is unreadable");
    if (!why[0]) {
        const std::int32_t ns = I32(t.mres + kMresStateN, -1), ne = I32(t.mres + kMresEventN, -1), np = I32(t.res + kResPhotoN, -1);
        const std::uintptr_t sd = Q(t.mres + kMresStateData), ed = Q(t.mres + kMresEventData), pd = Q(t.res + kResPhotoData);
        if (ns <= 0 || ns > static_cast<std::int32_t>(fr::kStateSlots) || !sd) std::snprintf(why, sizeof(why), "%d states", ns);
        else if (ne < 0 || ne > static_cast<std::int32_t>(fr::kEventSlots) || (ne && !ed)) std::snprintf(why, sizeof(why), "%d event clips", ne);
        else {
            t.ns = static_cast<unsigned>(ns), t.ne = static_cast<unsigned>(ne);
            for (unsigned i = 0; i < t.ns; ++i) {
                const std::uintptr_t e = sd + kStateStride * i;
                Clip& c = t.st[i];
                c.anim = Q(e);
                c.start = F32(e + 8, 0);
                c.blend = F32(e + 0xC, 0.3f);
                c.saccade = B(e + 0x10) != 0;  // 6th argument (0x1422D2D70)
                c.blink = B(e + 0x11) != 0;    // 5th
            }
            for (unsigned i = 0; i < t.ne; ++i) {
                const std::uintptr_t e = ed + kEventStride * i;
                Clip& c = t.ev[i];
                c.anim = Q(e + 8);
                c.start = F32(e + 0x10, 0);
                c.blend = F32(e + 0x14, 0.3f);
                c.blink = B(e + 0x18) != 0;    // 5th (0x1422D2A50)
                c.saccade = B(e + 0x19) != 0;  // 6th
                t.ev_key[i] = fr::Crc32cU64(0, c.anim);
            }
            // The photo array: the game reads 18 entries without a check; ours: its count, at most 32.
            if (np >= static_cast<std::int32_t>(kPhotoGameN) && np <= static_cast<std::int32_t>(fr::kPhotoSlots) && pd) {
                t.np = static_cast<unsigned>(np);
                for (unsigned i = 0; i < t.np; ++i) t.ph[i] = Q(pd + 8 * i);
            }
            unsigned bad = 0;
            for (unsigned i = 0; i < t.ns; ++i) bad += t.st[i].anim && !ClipOk(t.st[i].anim);
            for (unsigned i = 0; i < t.ne; ++i) bad += t.ev[i].anim && !ClipOk(t.ev[i].anim);
            t.skel = t.st[0].anim ? Q(t.st[0].anim + 0x38) : 0;
            if (bad) std::snprintf(why, sizeof(why), "%u table clips are not SkeletonAnimationResources", bad);
        }
    }
    if (why[0]) {
        const bool again = g_t_failed_sam == sam;
        g_t_failed_sam = sam;
        g_t_failed_ms = g_now_ms;
        g_t = Tables{};
        if (!again) Log("[face] Sam's facial tables are not readable: %s (tried again every 5 s)", why);
        return false;
    }
    t.ok = true;
    g_t = t;
    g_t_failed_sam = 0;
    Log("[face] Sam's facial manager %p (%s): %u states, %u event clips, %u photo-mode clips, rig skeleton %p", reinterpret_cast<void*>(t.mgr),
        mem::RttiName(mem::RttiOf(t.mgr)) ? mem::RttiName(mem::RttiOf(t.mgr)) : "?", t.ns, t.ne, t.np, reinterpret_cast<void*>(t.skel));
    return true;
}

// The clip's place in Sam's tables.
fr::Table Lookup(std::uintptr_t anim, unsigned* index) {
    for (unsigned i = 0; i < g_t.ns; ++i)
        if (g_t.st[i].anim == anim) return *index = i, fr::Table::state;
    for (unsigned i = 0; i < g_t.ne; ++i)
        if (g_t.ev[i].anim == anim) return *index = i, fr::Table::event;
    for (unsigned i = 0; i < g_t.np; ++i)
        if (g_t.ph[i] == anim) return *index = i, fr::Table::photo;
    return *index = 0, fr::Table::unknown;
}

// ---------------------------------------------------------------- rigs
struct Rig {
    std::uintptr_t e = 0, comp = 0, rep = 0;
    std::uintptr_t comp_vt = 0;  // the component's vtable when it was found (checked every tick with its entity)
    bool rtti_checked = false;  // the rep's own RTTI was decodable and right
    bool fresh = false;         // found (again) since the driver last looked: its face has been given nothing yet
};

std::uintptr_t RigClip(std::uintptr_t rep) {
    std::uintptr_t a = Q(rep + kRepSlotRig);
    return a ? a : Q(rep + kRepSlotMesh);
}
bool RigHolds(std::uintptr_t rep, std::uintptr_t clip) {
    return clip && (Q(rep + kRepSlotRig) == clip || Q(rep + kRepSlotMesh) == clip || Q(Q(rep + kRepPlayer) + kPlayerClip) == clip);
}

// The entity's DSFacialRigComponent and its rep (re-found when the entity changes, or when the kept component no
// longer belongs to it: a body torn down and respawned at the same address; the rep re-read every tick).
bool FindRig(std::uintptr_t e, Rig& r, const char* who) {
    if (!e) return r = Rig{}, false;
    if (r.e == e && r.comp && (Q(r.comp + kCompOwner) != e || Q(r.comp) != r.comp_vt)) {
        Log("[face] %s %p: its DSFacialRigComponent %p is no longer its own: found again", who, reinterpret_cast<void*>(e),
            reinterpret_cast<void*>(r.comp));
        r.comp = 0;
    }
    if (r.e != e || !r.comp) {
        r = Rig{};
        r.e = e;
        r.comp = partner::FindComponentByName(reinterpret_cast<game::Entity*>(e), "DSFacialRigComponent");
        if (!r.comp || Q(r.comp + kCompOwner) != e) {
            static unsigned logs = 0;
            if (++logs <= 10) Log("[face] %s %p has no DSFacialRigComponent of its own", who, reinterpret_cast<void*>(e));
            r.comp = 0;
            return false;
        }
        r.comp_vt = Q(r.comp);
        r.fresh = true;
    }
    const std::uintptr_t rep = Q(r.comp + kCompRep);
    if (!rep) return false;  // not built yet (PlayFacial would do nothing)
    if (rep != r.rep) {
        r.rep = rep;
        const std::uintptr_t rt = mem::RttiOf(rep);
        r.rtti_checked = rt == mem::Base() + kRepRtti;
        if (rt && !r.rtti_checked) {
            Log("[face] %s's facial rig rep %p is a %s, not a DSFacialRigComponentRep: not used", who, reinterpret_cast<void*>(rep),
                mem::RttiName(rt) ? mem::RttiName(rt) : "?");
            r.comp = 0;
            return false;
        }
        Log("[face] %s %p: DSFacialRigComponent %p, rep %p (%s), clip now %p, flags 0x%02X", who, reinterpret_cast<void*>(e),
            reinterpret_cast<void*>(r.comp), reinterpret_cast<void*>(rep), r.rtti_checked ? "RTTI checked" : "RTTI not decodable, the component's checked",
            reinterpret_cast<void*>(RigClip(rep)), B(rep + kRepFlags));
    }
    return true;
}

// ---------------------------------------------------------------- Sam's wanted clip
struct Want {
    bool ok = false;
    fr::Source src = fr::Source::state;
    fr::Table table = fr::Table::unknown;
    unsigned index = 0;
    Clip clip;
};

Rig g_sam_rig;

bool SamWant(std::uintptr_t sam, Want* w) {
    *w = Want{};
    if (!ReadTables(sam)) return false;
    const std::uintptr_t m = g_t.mgr;
    const std::uint8_t flags = B(m + kMgrFlags);
    const bool photo = B(m + kMgrPhoto) != 0;
    w->src = fr::PickSource(flags, photo);
    if (w->src == fr::Source::event) {
        const std::uint32_t key = static_cast<std::uint32_t>(I32(m + kMgrEventKey));
        for (unsigned i = 0; i < g_t.ne; ++i)
            if (g_t.ev_key[i] == key) {
                w->table = fr::Table::event, w->index = i, w->clip = g_t.ev[i];
                break;
            }
        if (w->table != fr::Table::event) w->src = fr::Source::state;  // as the game's Restore (0x1422D2C00)
    }
    if (w->src == fr::Source::state) {
        const int i = fr::StateIndex(I32(m + kMgrState), static_cast<int>(g_t.ns));
        if (i < 0) return false;
        w->table = fr::Table::state, w->index = static_cast<unsigned>(i), w->clip = g_t.st[i];
    } else if (w->src == fr::Source::override_ || w->src == fr::Source::rig) {
        if (w->src == fr::Source::override_) {
            w->clip.anim = Q(m + kMgrOverride);
            w->clip.blink = B(m + kMgrOvBlink) != 0;
            w->clip.saccade = B(m + kMgrOvSacc) != 0;
        } else {
            if (!FindRig(sam, g_sam_rig, "Sam")) return false;
            w->clip.anim = RigClip(g_sam_rig.rep);
            const std::uint8_t f = B(g_sam_rig.rep + kRepFlags);
            w->clip.blink = (f & 0x10) != 0;
            w->clip.saccade = (f & 0x08) != 0;
        }
        if (!ClipOk(w->clip.anim)) return false;
        w->table = Lookup(w->clip.anim, &w->index);
    }
    w->ok = w->clip.anim != 0;
    return w->ok;
}

// ---------------------------------------------------------------- one target (her self view, the partner body)
struct Target {
    const char* name;
    Rig rig;
    fr::Played played;
    std::uint32_t plays = 0, retries = 0, took = 0, faults = 0, waits = 0;
    std::uint8_t blink_n = 0xFF;
    std::uint32_t blinks = 0;
    double active_ms = 0;  // time with a rig to drive in the stats window
    unsigned play_logs = 0;
};
Target g_self{"self"}, g_partner{"partner"};

using PlayFn = void (*)(void* comp, void* anim, float start, float blend, bool blink, bool saccade);
bool CallPlay(std::uintptr_t comp, const Clip& c) {
    const auto fn = reinterpret_cast<PlayFn>(mem::Base() + kPlayFacial);
    __try {
        fn(reinterpret_cast<void*>(comp), reinterpret_cast<void*>(c.anim), c.start, c.blend, c.blink, c.saccade);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* ClipText(fr::Table t, unsigned i, char* out, std::size_t cap) {
    if (t == fr::Table::unknown) std::snprintf(out, cap, "a clip in no table");
    else std::snprintf(out, cap, "%s %u", fr::TableName(t), i);
    return out;
}

// The body went away (or another came): nothing kept from it, so no call ever reaches a component it owned.
void ResetTarget(Target& tg) {
    tg.rig = Rig{};
    tg.played = fr::Played{};
    tg.blink_n = 0xFF;
}

void Drive(Target& tg, std::uintptr_t e, std::uint32_t epoch, const char* key, const Clip& c, fr::Table t, unsigned idx, double now_ms, double dt) {
    if (!e) return ResetTarget(tg);
    if (tg.rig.e != e) ResetTarget(tg);
    if (!FindRig(e, tg.rig, tg.name)) return;
    if (tg.rig.fresh) {  // a component found again on the same entity: give it the clip anew
        tg.rig.fresh = false;
        tg.played = fr::Played{};
        tg.blink_n = 0xFF;
    }
    tg.active_ms += dt;
    const std::uintptr_t rep = tg.rig.rep;
    if (g_blink_ok) {
        const std::uint8_t n = B(rep + kRepBlinkN);
        if (tg.blink_n != 0xFF && n != tg.blink_n) ++tg.blinks;
        tg.blink_n = n;
    }
    const bool holds = RigHolds(rep, c.anim);
    if (fr::NoteTook(tg.played, holds)) {
        ++tg.took;
        if (tg.play_logs <= 20)
            Log("[face] %s: the clip is on the face (rig clip %p, flags 0x%02X: blink %s, saccade %s)", tg.name,
                reinterpret_cast<void*>(RigClip(rep)), B(rep + kRepFlags), (B(rep + kRepFlags) & 0x10) ? "on" : "off",
                (B(rep + kRepFlags) & 0x08) ? "on" : "off");
    }
    const std::uint8_t flags = fr::MakeFlags(c.blink, c.saccade);
    const fr::Act act = fr::Decide(tg.played, tg.rig.comp, epoch, c.anim, flags, holds, now_ms);
    if (act == fr::Act::wait) ++tg.waits;
    if (act != fr::Act::play && act != fr::Act::retry) return;
    if (!CallPlay(tg.rig.comp, c)) {
        ++tg.faults;
        g_off = true;
        Log("[face] PlayFacial faulted on the %s body '%s' (rig %p, clip %p): face sync off for this session", tg.name, key,
            reinterpret_cast<void*>(tg.rig.comp), reinterpret_cast<void*>(c.anim));
        return;
    }
    const bool retry = act == fr::Act::retry;
    fr::NotePlayed(tg.played, tg.rig.comp, epoch, c.anim, flags, now_ms, retry);
    ++tg.plays;
    if (retry) ++tg.retries;
    if (++tg.play_logs <= 20 || retry) {
        char ct[48];
        Log("[face] %s: played %s on '%s' (clip %p, start %.2f, blend %.2f, blink %d, saccade %d)%s (play %u)", tg.name,
            ClipText(t, idx, ct, sizeof(ct)), key, reinterpret_cast<void*>(c.anim), c.start, c.blend, c.blink ? 1 : 0, c.saccade ? 1 : 0,
            retry ? " again: the last call did not land in her rig" : "", tg.plays);
    }
}

// ---------------------------------------------------------------- the partner's face (received)
struct Rx {
    bool have = false;
    std::uint8_t id = fr::kIdNone, flags = 0;
    std::uint32_t frames = 0, changes = 0, unresolved = 0;
    unsigned logs = 0;
} g_rx;

bool Resolve(std::uint8_t id, std::uint8_t flags, Clip* c, fr::Table* t, unsigned* idx) {
    *t = fr::SplitId(id, idx);
    if (*t == fr::Table::state && *idx < g_t.ns) *c = g_t.st[*idx];
    else if (*t == fr::Table::event && *idx < g_t.ne) *c = g_t.ev[*idx];
    else if (*t == fr::Table::photo && *idx < g_t.np && ClipOk(g_t.ph[*idx])) *c = Clip{g_t.ph[*idx], 0, 0.3f, false, false};
    else return false;
    c->blink = (flags & fr::kFlagBlink) != 0;
    c->saccade = (flags & fr::kFlagSaccade) != 0;
    return c->anim != 0;
}

// ---------------------------------------------------------------- Sam while hidden, and the stats
struct SamMeter {
    std::uint32_t ticks = 0, follows = 0, advanced = 0, blinks = 0;
    std::uint8_t blink_n = 0xFF;
    float blink_t = -1;
} g_sm;
fr::Source g_last_src = fr::Source::state;
std::uintptr_t g_last_want = 0;
std::uint32_t g_want_changes = 0;
double g_last_tick_ms = 0, g_next_stats_ms = 0;

void SamMeterTick(std::uintptr_t sam, const Want& w) {
    if (!FindRig(sam, g_sam_rig, "Sam")) return;
    ++g_sm.ticks;
    if (RigHolds(g_sam_rig.rep, w.clip.anim)) ++g_sm.follows;
    if (g_blink_ok) {
        const std::uint8_t n = B(g_sam_rig.rep + kRepBlinkN);
        float t = -1;
        Rd(g_sam_rig.rep + kRepBlinkT, t);
        if ((g_sm.blink_n != 0xFF && n != g_sm.blink_n) || (g_sm.blink_t >= 0 && t != g_sm.blink_t)) ++g_sm.advanced;
        if (g_sm.blink_n != 0xFF && n != g_sm.blink_n) ++g_sm.blinks;
        g_sm.blink_n = n;
        g_sm.blink_t = t;
    }
}

void Stats(double now_ms) {
    if (now_ms < g_next_stats_ms) return;
    const bool any = g_self.active_ms > 0 || g_partner.active_ms > 0 || g_rx.frames;
    g_next_stats_ms = now_ms + 10000;
    if (!any) return;
    if (g_self.active_ms > 0) {
        Log("[face] self: plays %u, took %u, retries %u, waits %u, faults %u; her blinks %u in %.1f s shown; Sam's clip changes %u",
            g_self.plays, g_self.took, g_self.retries, g_self.waits, g_self.faults, g_self.blinks, g_self.active_ms / 1000.0, g_want_changes);
        if (g_sm.ticks)
            Log("[face] Sam hidden by reason 63 (%u ticks): his rig holds the manager's clip in %u (%.0f %%), his blink timer moved in %u, his "
                "blinks %u%s",
                g_sm.ticks, g_sm.follows, 100.0 * g_sm.follows / g_sm.ticks, g_sm.advanced, g_sm.blinks, g_blink_ok ? "" : " (blink meter off)");
    }
    if (g_partner.active_ms > 0 || g_rx.frames)
        Log("[face] partner: rx frames %u (id changes %u, last id %u flags %u, unresolved %u); plays %u, took %u, retries %u, faults %u; its blinks "
            "%u in %.1f s",
            g_rx.frames, g_rx.changes, g_rx.id, g_rx.flags, g_rx.unresolved, g_partner.plays, g_partner.took, g_partner.retries, g_partner.faults,
            g_partner.blinks, g_partner.active_ms / 1000.0);
    g_self.active_ms = g_partner.active_ms = 0;
    g_self.blinks = g_partner.blinks = 0;
    g_sm = SamMeter{};
    g_rx.frames = 0;
}

}  // namespace

void Tick(const TickIn& in, double now_ms) noexcept {
    const double dt = g_last_tick_ms > 0 && now_ms > g_last_tick_ms && now_ms - g_last_tick_ms < 1000 ? now_ms - g_last_tick_ms : 0;
    g_last_tick_ms = now_ms;
    g_now_ms = now_ms;
    // A body not given this tick (gone, unshown, respawning) keeps nothing: its rig is found again when it is back.
    if (!in.self_body || g_off || !(g_mode & 1)) ResetTarget(g_self);
    if (!in.partner_body || g_off || !(g_mode & 2)) ResetTarget(g_partner);
    if (!in.sam) g_sam_rig = Rig{};
    if (g_off || !g_mode || !in.sam) return Stats(now_ms);
    if (!in.self_body && !in.partner_body) return Stats(now_ms);
    if (!CheckSites()) return;
    Want w;
    const bool have = SamWant(in.sam, &w);
    if (have && (w.clip.anim != g_last_want || w.src != g_last_src)) {
        ++g_want_changes;
        if (g_want_changes <= 30) {
            char ct[48];
            Log("[face] Sam's face: %s (%s, clip %p, blink %d, saccade %d)%s", ClipText(w.table, w.index, ct, sizeof(ct)), fr::SourceName(w.src),
                reinterpret_cast<void*>(w.clip.anim), w.clip.blink ? 1 : 0, w.clip.saccade ? 1 : 0, in.self_body ? "; Sam hidden (self view)" : "");
        }
        g_last_want = w.clip.anim;
        g_last_src = w.src;
    }
    if ((g_mode & 1) && in.self_body && have) {
        Drive(g_self, in.self_body, in.self_epoch, in.self_key, w.clip, w.table, w.index, now_ms, dt);
        SamMeterTick(in.sam, w);
    }
    if ((g_mode & 2) && in.partner_body && g_rx.have && g_t.ok && !g_off) {
        Clip c;
        fr::Table t = fr::Table::none;
        unsigned idx = 0;
        if (Resolve(g_rx.id, g_rx.flags, &c, &t, &idx)) Drive(g_partner, in.partner_body, in.partner_epoch, in.partner_key, c, t, idx, now_ms, dt);
        else if (g_rx.id != fr::kIdNone) ++g_rx.unresolved;
    }
    Stats(now_ms);
}

bool OwnFace(std::uintptr_t sam, wire::Face* out) noexcept {
    if (!out || g_off || !(g_mode & 2) || !sam || !CheckSites()) return false;
    Want w;
    if (!SamWant(sam, &w)) return false;
    out->id = fr::MakeId(w.table, w.index);
    out->flags = fr::MakeFlags(w.clip.blink, w.clip.saccade);
    return true;
}

void SetPartnerFace(const wire::Face* f) noexcept {
    if (!f) {
        g_rx = Rx{};
        g_partner.played = fr::Played{};
        return;
    }
    ++g_rx.frames;
    const std::uint8_t id = fr::KeepId(g_rx.have ? g_rx.id : fr::kIdNone, f->id);
    const std::uint8_t flags = static_cast<std::uint8_t>(f->flags & fr::kFlagsKnown);
    if (!g_rx.have || id != g_rx.id || flags != g_rx.flags) {
        ++g_rx.changes;
        if (++g_rx.logs <= 30) {
            unsigned i = 0;
            const fr::Table t = fr::SplitId(id, &i);
            Log("[face] rx: the partner's face now id %u (%s %u), flags %u%s", id, fr::TableName(t), i, flags,
                f->id == fr::kIdUnknown ? " (the sender's clip is in no table: the last one kept)" : "");
        }
    }
    g_rx.have = true;
    g_rx.id = id;
    g_rx.flags = flags;
}

bool SetMode(int mask, char* msg, std::size_t cap) noexcept {
    g_mode = mask & 3;
    std::snprintf(msg, cap, "face sync: her self view %s, the partner body %s (mask %d)%s", (g_mode & 1) ? "on" : "off", (g_mode & 2) ? "on" : "off",
                  g_mode, g_off ? "; OFF after a fault" : "");
    Log("[face] %s", msg);
    return true;
}

void Status(char* msg, std::size_t cap) noexcept {
    std::snprintf(msg, cap,
                  "face sync mask %d%s, sites %s; Sam's tables %s (%u states, %u events, %u photo); self plays %u took %u faults %u; partner rx id %u "
                  "flags %u, plays %u took %u faults %u",
                  g_mode, g_off ? " (OFF after a fault)" : "", g_checked ? g_why : "not checked yet", g_t.ok ? "read" : "not read", g_t.ns, g_t.ne,
                  g_t.np, g_self.plays, g_self.took, g_self.faults, g_rx.id, g_rx.flags, g_partner.plays, g_partner.took, g_partner.faults);
    Log("[face] %s", msg);
}

}  // namespace st::facesync
