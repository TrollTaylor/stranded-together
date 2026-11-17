// Game-side debug actions and diagnostics. See game_actions.hpp for the rules
// and docs/debug-actions.md for the evidence behind every address.
#include "game_actions.hpp"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game_api.hpp"
#include "anim_census.hpp"
#include "holo_catalog.hpp"
#include "real_body.hpp"
#include "log.hpp"

namespace st::game {
namespace {

// ---------------------------------------------------------------- ds.exe RVAs (image base 0x140000000)
// Globals (each one is confirmed by the byte checks below: the RIP-relative
// displacement that loads it is part of the checked bytes).
constexpr std::uintptr_t kGameModuleGlobal = 0x7BE6318;    // GameModule*
constexpr std::uintptr_t kGameStateGlobal = 0x7BF6E88;     // DSGameState*
constexpr std::uintptr_t kPlayerSystemGlobal = 0x7BE7980;  // DSPlayerSystem*
constexpr std::uintptr_t kAreaChangeGlobal = 0x7AE90E8;    // area-change state (GetCurrentArea fast path)
constexpr std::uintptr_t kHealGateGlobal = 0x7C0AFF8;      // read unchecked by the heal gate 0x1426A05F0

// Functions
constexpr std::uintptr_t kPlaceOn = 0x2634F00;                  // PlayerGame vt+0xA8 (Player*, const WorldTransform*, int mode)
constexpr std::uintptr_t kGetPlayer = 0x27115B0;                // DSPlayerEntity vt+0xD8: [e+0x560]
constexpr std::uintptr_t kGetHealth = 0x27105E0;                // DSPlayerEntity vt+0x78
constexpr std::uintptr_t kGetMaxHealth = 0x2710F60;             // DSPlayerEntity vt+0x80
constexpr std::uintptr_t kOnHeal = 0x2713AE0;                   // DSPlayerEntity vt+0x98
constexpr std::uintptr_t kExportedHeal = 0x2361BD0;             // Entity_ExportedHeal(Entity*, float)
constexpr std::uintptr_t kExportedIsBoundToSequence = 0x34FD620;  // ControlledEntity_ExportedIsBoundToSequence(Entity*)
constexpr std::uintptr_t kSetTimeOfDay = 0x3631F90;             // GameWorldTimeState::SetTimeOfDay(tod, hours, transition)
constexpr std::uintptr_t kEchoPlay = 0x2BBF3A0;                 // DSEchoComponent::Play(comp, idx, tableSel, delay)

// Vtables / RTTI
constexpr std::uintptr_t kPlayerGameVtable = 0x3D5C860;
constexpr std::uintptr_t kTimeStateVtable = 0x3F19A00;
constexpr std::uintptr_t kTimeStateGetRtti = 0x362C400;  // vt[0]: lea rax,[GameWorldTimeState RTTI]; ret
constexpr std::uintptr_t kTimeStateRtti = 0x4E6B540;
constexpr std::uintptr_t kEchoComponentRtti = 0x4C23EC0;
constexpr std::uintptr_t kEchoResourceRtti = 0x4C23E00;
constexpr std::uintptr_t kParamComponentRtti = 0x4BC89D0;

// Offsets
constexpr std::uintptr_t kEntParent = 0x70;
constexpr std::uintptr_t kEntFlags = 0x88;
constexpr std::uintptr_t kEntComponents = 0x90;  // i32 count, data ptr at +0x98
constexpr std::uintptr_t kEntPos = 0xC8;         // 3 doubles
constexpr std::uintptr_t kEntRot = 0xE0;         // 3x3 floats
constexpr std::uintptr_t kSamPlayer = 0x560;
constexpr std::uintptr_t kSamPopGuard = 0x678;   // byte; the game's own Pop refuses while set
constexpr std::uintptr_t kSamParamComponent = 0x54C0;
constexpr std::uintptr_t kPlayerEntity = 0x70;
constexpr std::uintptr_t kPlayerSnapPending = 0x16C;  // i32, 1 = mode-2 ground snap pending
constexpr std::uintptr_t kGmTimeState = 0x58;
constexpr std::uintptr_t kGmLoadMask = 0x44C;
constexpr std::uintptr_t kGmLoadReason = 0x450;
constexpr std::uintptr_t kGsFlags = 0x08;
constexpr std::uintptr_t kGsRequested = 0x10;
constexpr std::uintptr_t kGsPhase = 0x5E0;
constexpr std::uintptr_t kPsPushedTransform = 0x206D0;  // bit 0
constexpr std::uintptr_t kAreaActive = 0xA8;
constexpr std::uintptr_t kAreaCurrent = 0x48;
constexpr std::uintptr_t kTodHours = 0x20;
constexpr std::uintptr_t kTodDay = 0x28;
constexpr std::uintptr_t kTodCycle = 0x2C;
constexpr std::uintptr_t kPcHealthObject = 0x2E0;
constexpr std::uintptr_t kHealth = 0xC0;
constexpr std::uintptr_t kHealthMax = 0xC4;
constexpr std::uintptr_t kPcSafeFlags = 0x7F73;  // 0x20: A valid, 0x40: B valid
constexpr std::uintptr_t kPcSafeA = 0x81F0;      // pos (3 doubles), rot (3x3 floats) at +0x18 = 0x8208
constexpr std::uintptr_t kPcSafeB = 0x8230;      // rot at 0x8248
constexpr std::uintptr_t kEchoOwner = 0x48;
constexpr std::uintptr_t kEchoResource = 0x30;
constexpr std::uintptr_t kEchoState = 0x64;
constexpr std::uintptr_t kEchoResCount = 0x20;  // Array<Ref<SentenceResource>> EchoSentenceResources
constexpr std::uintptr_t kEchoResData = 0x28;
constexpr std::uint64_t kFlagAsleep = 0x100;

// ---------------------------------------------------------------- byte checks
// Exact ds.exe DC bytes (PE timestamp 1706443442) at each known RVA, taken
// from the binary with tools/re/dsre.py. Every global and field offset this
// file uses appears inside one of these ranges.
struct Check {
    const char* name;
    std::uintptr_t rva;
    const char* hex;
    std::uint32_t features;  // disabled when the bytes differ
};
constexpr std::uint32_t kNeedsGuards = kFeatTeleport | kFeatUnstuck | kFeatHeal;
const Check kChecks[] = {
    {"PlayerGame::PlaceOn", 0x2634F00,
     "48895C24205556574883EC40488B051DC192024833C44889442438488B7970418BD8488BEA488BF14885FF747F488D0514EF6C01C644242800"
     "4533C04889442420488D542420C644243001488BCFC744243400000000E8357AD2FF488D4C2420E8FBB33BFF85DB784383FB017E2283FB0275"
     "394533C941B001488BD5488BCFE88C50D2FFC7866C01000001000000EB1C410F94C0488BD54533C9488BCFE86E50D2FFC7866C010000000000"
     "00488B4C24384833CCE827E34C01488B5C24784883C4405F5E5DC3",
     kFeatTeleport | kFeatUnstuck},
    {"DSPlayerEntity::GetPlayer", 0x27115B0, "488B8160050000C3", kFeatTeleport | kFeatUnstuck},
    // DSPlayerSystem global, +0x206D0 bit 0, Sam+0x678
    {"DSPlayerSystem::PopWorldTransform guards", 0x26E9953,
     "488B0526E04F054885C00F84E2000000F680D0060200010F84D500000048895C2470E8B6521500488BD84885C00F84BA00000080B87806000000",
     kNeedsGuards},
    // GameModule global, +0x44C
    {"GameModule suspend check", 0x26A05A6, "488B056B5D5405488BD983B84C04000000",
     kFeatModule | kNeedsGuards | kFeatEcho | kFeatTime},
    // DSGameState global, +8 / +0x10, the stopping-event mask
    {"IsStoppingGameEvent", 0x2DD3760,
     "4883EC28488B051D37E2044885C0742D48B97F7FEC7F404F1800488548087516488548107510488B",
     kFeatState | kNeedsGuards | kFeatEcho},
    {"DSGameState setup phase", 0x2E0E950, "83B9E0050000020F94C0C3", kFeatState | kNeedsGuards},
    {"ControlledEntity_ExportedIsBoundToSequence", 0x34FD620, "4885C9750332C0C3E9D340E8FE", kNeedsGuards},
    {"Entity::IsBoundToSequence", 0x2381700,
     "48895C2410574883EC20488D9988020000488BF9488BCBFF155B8E7B0185C07509488BCBFF155E8E7B01488D8F90000000488D1588647E02E8"
     "2313F3FF4885C075054032FFEB094883785000400F95C74885DB7409488BCBFF15128E7B01488B5C2438400FB6C74883C4205FC3",
     kNeedsGuards},
    // area-change global, +0xA8 active, +0x48 current area
    {"GameModule::GetCurrentArea", 0x262A864, "488B057DE84B054885C0741280B8A80000000074090FB74048", kFeatArea},
    // GameModule+0x58 time state, call to SetTimeOfDay, +0x2C EnableDayNightCycle
    {"SetTimeOfDay event", 0x35720FC,
     "488B051542670448895C245048897C2458488B7E38488B5858807F3800742AC5F057C9C5F22A4F30C5F857C0C5FA2A4734C5FA5E1D576D6100"
     "C5E258C9C5F828D6488BCBE84BFE0B00807B2C00740A807B2D00",
     kFeatTime},
    {"GameWorldTimeState::SetTimeOfDay", 0x3631F90,
     // whole function: the transition path, then the instant path we use
     // (transition 0: writes +0x20 hours, +0x28 day on rollover, clears +0x30)
     "C5FA105920C5F857C0C5F82FD0C5F828E17633C5F82FCB7206C5F25CC3EB10C5FA1005D1345800C5FA5CCBC5F258C4C5FA5905CD6E5500C5FA"
     "114130C5FA5EC2C5FA114134C3C5F82FE37252C5DA5CC3C5FA5905AC6E5500C5FA5E0DA46E5500C5FA100590345800C5F258D3C5F82FD0C5FA"
     "115120727D8B4128C5FA100D72555800C5EA58D1FFC0C5F82FD073F4C5FA115120894128C7413000000000C3C5FA102D52345800C5D25CC3C5"
     "FA58CCC5F2590D4E6E5500C5F25E05466E5500C5FA58DBC5F82FDDC5FA11592072278B4128C5FA10051C5558000F1F840000000000C5E258D8"
     "FFC0C5F82FDD73F4C5FA115920894128C7413000000000C3",
     kFeatTime},
    // Sam+0x54C0 -> +0x2E0 -> health +0xC0 / max +0xC4
    {"DSPlayerEntity::GetHealth", 0x27105E0,
     "488B81C05400004885C07438488B80E0020000C5FA1090C40000004885C0750DC5FA100D8C794701C5EA5DC1C3C5FA1088C0000000C5F857C0"
     "C5F82FC1770AC5EA5DC1C3E9E7D6C3FFC3",
     kFeatHealth | kFeatHeal},
    {"DSPlayerEntity::GetMaxHealth", 0x2710F60, "488B81C05400004885C07410488B80E0020000C5FA1080C4000000C3",
     kFeatHealth | kFeatHeal},
    {"Entity_ExportedHeal", 0x2361BD0, "E93BA7FEFF", kFeatHeal},
    {"Entity::Heal", 0x234C310,
     "4885C90F844E0100004C8BDC534881EC20010000C4C178297BD8488B05FF4CC1024833C448898424F0000000C4C1782973E8488BD9C5C857F6"
     "C5F82FF1C5F828",
     kFeatHeal},
    {"DSPlayerEntity::OnHeal", 0x2713AE0,
     "4883EC38488B81C05400004885C07433C5FA100A41B895010000488D0D4FD63D0248894C24284533C9488B88E00200006644894424204533C0"
     "E8A2E8FEFF4883C438C34883C438E914B8C3FF",
     kFeatHeal},
    // OnHeal's target 0x1427023C0 loads the DSPlayerSystem global and calls the
    // gate 0x1426A05F0, which dereferences it (+0x206D1) and [0x147C0AFF8]
    // (+0x8170C) without null checks: HealLocalPlayer checks both first.
    {"DSPlayerEntity heal body", 0x27023C0,
     "48895C24084889742410574883EC50488BD9C5F8297C2430488B0DA1554E05418BF9C57829442420418BF0C57828C1E8FCE1F9FF",
     kFeatHeal},
    {"DSPlayerSystem heal gate", 0x26A05F0, "F681D1060200017517488B05F8A956058B880C170800D1E9F6C1017503", kFeatHeal},
    // DSPlayerParamComponent safe transforms: +0x7F73 flags, +0x81F0 / +0x8230
    {"DSPlayerParamComponent safe transform A", 0x27A4A40,
     "4883EC58488B81C00200004C8D42184532C9C5F81080C8000000C5FB1088D80000000FB681737F0000C5F8110424C5FB114C2410A840743FC5"
     "F8108130820000C5F81102C5FB108940820000488D8148820000C5FB114A104C3BC07410C5FC1000C4C17C11008B4020418940200FB681737F"
     "000041B101A8200F84F9000000",
     kFeatUnstuck},
    {"DSPlayerParamComponent safe transform B", 0x27A4B79,
     "C5F81081F0810000C5F81102C5FB1089008200004881C108820000C5FB114A104C3BC17410C5FC1001C4C17C11008B412041894020",
     kFeatUnstuck},
    {"DSPlayerSystem return-to-safe warp", 0x26EA537, "488B88C05400004885C97469488D542440E8F3A40B00", kFeatUnstuck},
    {"DSEchoComponent::Play", 0x2BBF3A0,
     "48895C240848896C24104889742418574883EC30488D5950C5F829742420488BF9410FB6F0488BCB8BEAC5F828F3E84D4BE4FE488B05AE7A03"
     "0548B97F7FEC7F404F1800488548087513488BC8E84EB8200084C07417488B058B7A03058B400848C1E819A80175054032FFEB21806768FE40"
     "087768C5FA117758896F60C7476401000000C7475C0000000040B7014885DB7408488BCBE8F545D8FE488B5C2440400FB6C7488B6C2448488B"
     "742450C5F8287424204883C4305FC3",
     kFeatEcho},
    // Play calls this: GameModule global (must be non-null) -> 0x1426303B0
    {"GameModule loading check", 0x2DCAC40, "488B0DD1B6E104E9645786FF", kFeatEcho},
    // DSEchoComponent+0x30 resource, +0x60 idx, +0x68 table bit; resource +0x20/+0x28 and +0x30/+0x38
    {"DSEchoComponent::Update sentence pick", 0x2BB705F,
     "488B573033F6F647680174133B42307C058977608BC64863C8488B4238EB113B42207C058977608BC64863C8", kFeatEcho},
};

const char* FeatureName(std::uint32_t f) {
    switch (f) {
        case kFeatTeleport: return "teleport";
        case kFeatUnstuck: return "unstuck";
        case kFeatHealth: return "health";
        case kFeatHeal: return "heal";
        case kFeatTime: return "time of day";
        case kFeatArea: return "area";
        case kFeatState: return "game state";
        case kFeatModule: return "load mask";
        case kFeatEcho: return "voice ping";
        default: return "?";
    }
}

// ---------------------------------------------------------------- fault-free memory
struct Range {
    std::uintptr_t lo = 0, hi = 0;
    bool Has(std::uintptr_t a) const { return a >= lo && a < hi; }
};
Range g_image, g_text;

void InitRanges() {
    if (g_image.lo) return;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    g_image = {base, base + nt->OptionalHeader.SizeOfImage};
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (std::memcmp(s->Name, ".text", 6) == 0)
            g_text = {base + s->VirtualAddress, base + s->VirtualAddress + s->Misc.VirtualSize};
}

std::uintptr_t Base() {
    InitRanges();
    return g_image.lo;
}

// ReadProcessMemory on our own process fails cleanly on a bad page instead of faulting.
bool Read(std::uintptr_t addr, void* out, std::size_t n) {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), out, n, &got) && got == n;
}
template <class T>
bool Rd(std::uintptr_t addr, T& out) {
    return Read(addr, &out, sizeof(T));
}
std::uintptr_t Q(std::uintptr_t addr) {
    std::uintptr_t v = 0;
    return Rd(addr, v) ? v : 0;
}
bool Write(std::uintptr_t addr, const void* in, std::size_t n) {
    if (addr < 0x10000) return false;
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(addr), in, n, &put) && put == n;
}

// Decima GetRTTI getters: lea rax,[rip+X]; ret  /  mov rax,[rip+X]; ret
std::uintptr_t DecodeGetter(std::uintptr_t fn) {
    if (!g_text.Has(fn)) return 0;
    std::uint8_t b[8];
    if (!Read(fn, b, sizeof(b)) || b[0] != 0x48 || b[2] != 0x05 || b[7] != 0xC3) return 0;
    std::int32_t rel = 0;
    std::memcpy(&rel, b + 3, 4);
    const std::uintptr_t x = fn + 7 + static_cast<std::intptr_t>(rel);
    if (b[1] == 0x8D) return g_image.Has(x) ? x : 0;
    if (b[1] == 0x8B) {
        const std::uintptr_t v = Q(x);
        return g_image.Has(v) ? v : 0;
    }
    return 0;
}

std::uintptr_t RttiOf(std::uintptr_t obj) {
    const std::uintptr_t vt = Q(obj);
    if (!g_image.Has(vt)) return 0;
    return DecodeGetter(Q(vt));
}

bool RttiNamed(std::uintptr_t rtti, const char* name) {
    const std::uintptr_t p = Q(rtti + 0x40);
    if (!g_image.Has(p)) return false;
    char buf[64] = {};
    const std::size_t n = std::strlen(name) + 1;
    return n <= sizeof(buf) && Read(p, buf, n) && std::memcmp(buf, name, n) == 0;
}

bool BytesMatch(std::uintptr_t at, const char* hex) {
    std::uint8_t want[512], got[512];
    std::size_t n = 0;
    for (const char* s = hex; s[0] && s[1] && n < sizeof(want); s += 2) {
        char pair[3] = {s[0], s[1], 0};
        want[n++] = static_cast<std::uint8_t>(std::strtoul(pair, nullptr, 16));
    }
    return n && Read(at, got, n) && std::memcmp(want, got, n) == 0;
}

void Put(char* out, std::size_t cap, const char* fmt, ...) {
    if (!out || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(out, cap, fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------- resolve
bool g_resolved = false;
std::uint32_t g_features = 0;
char g_disabled[160] = {};
const char* g_feature_reason[9] = {};  // first failed check per feature bit

void EnsureResolved() {
    if (g_resolved) return;
    g_resolved = true;
    const std::uintptr_t base = Base();
    std::uint32_t features = kFeatAll;
    auto fail = [&](std::uint32_t f, const char* why) {
        for (int b = 0; b < 9; ++b)
            if ((f & (1u << b)) && (features & (1u << b))) g_feature_reason[b] = why;
        features &= ~f;
    };
    int ok = 0, total = 0;
    for (const Check& c : kChecks) {
        ++total;
        if (BytesMatch(base + c.rva, c.hex)) {
            ++ok;
            continue;
        }
        Log("[game] actions: %s at ds.exe+0x%llX: bytes differ (unsupported ds.exe build?)", c.name,
            static_cast<unsigned long long>(c.rva));
        fail(c.features, c.name);
    }
    // Data checks: vtable slots and RTTI descriptors.
    ++total;
    if (Q(base + kPlayerGameVtable + 0xA8) == base + kPlaceOn) ++ok;
    else {
        Log("[game] actions: PlayerGame vtable +0xA8 is not PlaceOn");
        fail(kFeatTeleport | kFeatUnstuck, "PlayerGame vtable");
    }
    ++total;
    if (Q(base + kTimeStateVtable) == base + kTimeStateGetRtti && DecodeGetter(base + kTimeStateGetRtti) == base + kTimeStateRtti &&
        RttiNamed(base + kTimeStateRtti, "GameWorldTimeState"))
        ++ok;
    else {
        Log("[game] actions: GameWorldTimeState vtable/RTTI mismatch");
        fail(kFeatTime, "GameWorldTimeState RTTI");
    }
    ++total;
    if (RttiNamed(base + kEchoComponentRtti, "DSEchoComponent") && RttiNamed(base + kEchoResourceRtti, "DSEchoComponentResource"))
        ++ok;
    else {
        Log("[game] actions: DSEchoComponent RTTI mismatch");
        fail(kFeatEcho, "DSEchoComponent RTTI");
    }
    ++total;
    if (RttiNamed(base + kParamComponentRtti, "DSPlayerParamComponent")) ++ok;
    else {
        Log("[game] actions: DSPlayerParamComponent RTTI mismatch");
        fail(kFeatUnstuck, "DSPlayerParamComponent RTTI");
    }
    if (!game::Get().GetLocalPlayerEntity) fail(kFeatTeleport | kFeatUnstuck | kFeatHealth | kFeatHeal, "GetLocalDSPlayerEntity");
    g_features = features;

    char on[160] = {}, off[160] = {};
    std::size_t non = 0, noff = 0;
    g_disabled[0] = 0;
    std::size_t nd = 0;
    for (int b = 0; b < 9; ++b) {
        const std::uint32_t f = 1u << b;
        if (features & f) {
            non += static_cast<std::size_t>(std::snprintf(on + non, sizeof(on) - non, "%s%s", non ? ", " : "", FeatureName(f)));
            if (non >= sizeof(on)) non = sizeof(on) - 1;
        } else {
            noff += static_cast<std::size_t>(std::snprintf(off + noff, sizeof(off) - noff, "%s%s", noff ? ", " : "", FeatureName(f)));
            if (noff >= sizeof(off)) noff = sizeof(off) - 1;
            nd += static_cast<std::size_t>(std::snprintf(g_disabled + nd, sizeof(g_disabled) - nd, "%s%s: %s", nd ? "; " : "",
                                                         FeatureName(f), g_feature_reason[b] ? g_feature_reason[b] : "?"));
            if (nd >= sizeof(g_disabled)) nd = sizeof(g_disabled) - 1;
        }
    }
    Log("[game] actions: %d/%d byte checks ok; enabled: %s; DISABLED: %s", ok, total, non ? on : "none", noff ? off : "none");
}

bool Has(std::uint32_t f, char* msg, std::size_t cap) {
    EnsureResolved();
    if (g_features & f) return true;
    int b = 0;
    while (b < 9 && !(f & (1u << b))) ++b;
    Put(msg, cap, "%s is disabled: %s did not match this ds.exe", FeatureName(f),
        b < 9 && g_feature_reason[b] ? g_feature_reason[b] : "a byte check");
    return false;
}

// ---------------------------------------------------------------- guarded calls (last resort only;
// everything is validated first because the crash reporter fires even on a caught fault)
bool CallPlaceOn(std::uintptr_t player, const WorldTransform* wt, int mode) {
    using Fn = void (*)(void*, const WorldTransform*, int);
    __try {
        reinterpret_cast<Fn>(Base() + kPlaceOn)(reinterpret_cast<void*>(player), wt, mode);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallIsBoundToSequence(std::uintptr_t e, bool* out) {
    using Fn = bool (*)(void*);
    __try {
        *out = reinterpret_cast<Fn>(Base() + kExportedIsBoundToSequence)(reinterpret_cast<void*>(e));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallHeal(std::uintptr_t e, float amount) {
    using Fn = void (*)(void*, float);
    __try {
        reinterpret_cast<Fn>(Base() + kExportedHeal)(reinterpret_cast<void*>(e), amount);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallSetTimeOfDay(std::uintptr_t tod, float hours) {
    using Fn = void (*)(void*, float, float);
    __try {
        reinterpret_cast<Fn>(Base() + kSetTimeOfDay)(reinterpret_cast<void*>(tod), hours, 0.0f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallEchoPlay(std::uintptr_t comp, int idx, float delay, bool* accepted) {
    using Fn = bool (*)(void*, int, bool, float);
    __try {
        *accepted = reinterpret_cast<Fn>(Base() + kEchoPlay)(reinterpret_cast<void*>(comp), idx, false, delay);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- reads
struct World {
    std::uintptr_t gm = 0, gs = 0, ps = 0;
};
World ReadWorld() {
    const std::uintptr_t base = Base();
    return {Q(base + kGameModuleGlobal), Q(base + kGameStateGlobal), Q(base + kPlayerSystemGlobal)};
}

struct Sam {
    std::uintptr_t e = 0, vt = 0;
    std::uint64_t flags = 0;
    bool wt_ok = false;
    WorldTransform wt{};
};

// Decima keeps a transform as 3 doubles followed by a 3x3 float matrix (0x3C bytes).
bool ReadPosRot(std::uintptr_t at, WorldTransform* wt) {
    return Read(at, &wt->pos, sizeof(wt->pos)) && Read(at + 0x18, &wt->rot, sizeof(wt->rot));
}
bool ReadTransform(std::uintptr_t e, WorldTransform* wt) {
    static_assert(kEntRot - kEntPos == 0x18, "entity transform layout");
    return ReadPosRot(e + kEntPos, wt);
}

bool ReadSam(Sam* s) {
    *s = Sam{};
    Entity* e = nullptr;
    if (!TryGetLocalPlayer(&e) || !e) return false;
    s->e = reinterpret_cast<std::uintptr_t>(e);
    s->vt = Q(s->e);
    if (!g_image.Has(s->vt) || !Rd(s->e + kEntFlags, s->flags)) return false;
    s->wt_ok = ReadTransform(s->e, &s->wt);
    return true;
}

bool SamSlot(const Sam& s, std::uintptr_t slot, std::uintptr_t rva) { return Q(s.vt + slot) == Base() + rva; }

bool ReadHealth(const Sam& s, float* health, float* max) {
    if (!SamSlot(s, 0x78, kGetHealth) || !SamSlot(s, 0x80, kGetMaxHealth)) return false;
    const std::uintptr_t pc = Q(s.e + kSamParamComponent);
    const std::uintptr_t h = pc ? Q(pc + kPcHealthObject) : 0;
    float v = 0, m = 0;
    if (!h || !Rd(h + kHealth, v) || !Rd(h + kHealthMax, m)) return false;
    if (!(v == v) || !(m == m)) return false;
    *health = v < 0 ? 0 : (v > m ? m : v);  // the getter clamps the same way
    *max = m;
    return true;
}

std::uintptr_t TimeState() {
    const std::uintptr_t gm = Q(Base() + kGameModuleGlobal);
    const std::uintptr_t tod = gm ? Q(gm + kGmTimeState) : 0;
    return tod && Q(tod) == Base() + kTimeStateVtable ? tod : 0;
}

// ---------------------------------------------------------------- preconditions
enum Need : unsigned { kNeedPlacement = 1, kNeedSequenceCall = 2 };
char g_block[96];

// nullptr = ok, otherwise why not. kNeedPlacement adds the teleport-only
// checks (no parent, Pop guard, Player object, no pending snap/pushed transform).
const char* Blocked(const Sam& s, unsigned need, std::uintptr_t* player_out) {
    if (player_out) *player_out = 0;
    if (!s.e) return "no local player (title screen or loading)";
    if (s.flags & kFlagAsleep) return "Sam is asleep (loading, cutscene, menu or knot space)";
    const World w = ReadWorld();
    if (!w.gm) return "no GameModule";
    std::uint32_t mask = 0;
    if (!Rd(w.gm + kGmLoadMask, mask)) return "GameModule unreadable";
    if (mask) {
        char d[64];
        DescribeLoadMask(mask, d, sizeof(d));
        std::snprintf(g_block, sizeof(g_block), "the game is loading/suspended (0x%X %s)", mask, d);
        return g_block;
    }
    if (!w.gs) return "no DSGameState";
    std::uint64_t f = 0, r = 0;
    std::int32_t phase = 0;
    if (!Rd(w.gs + kGsFlags, f) || !Rd(w.gs + kGsRequested, r) || !Rd(w.gs + kGsPhase, phase)) return "DSGameState unreadable";
    if ((f | r) & kStoppingGameEventMask) {
        char d[64];
        DescribeGameStateFlags((f | r) & kStoppingGameEventMask, d, sizeof(d));
        std::snprintf(g_block, sizeof(g_block), "game state: %s", d);
        return g_block;
    }
    if (phase == 2) return "post-load setup phase";
    if (need & kNeedPlacement) {
        if (!SamSlot(s, 0xD8, kGetPlayer)) return "Sam's vtable is not DSPlayerEntity's";
        if (Q(s.e + kEntParent)) return "Sam is attached to a parent entity (vehicle?)";
        std::uint8_t guard = 1;
        if (!Rd(s.e + kSamPopGuard, guard) || guard) return "Sam+0x678 is set (the game's own warp refuses too)";
        const std::uintptr_t p = Q(s.e + kSamPlayer);
        if (!p) return "Sam has no Player";
        if (Q(p) != Base() + kPlayerGameVtable) return "Sam's Player is not a PlayerGame";
        if (Q(p + kPlayerEntity) != s.e) return "the Player does not control Sam";
        std::int32_t snap = 0;
        if (!Rd(p + kPlayerSnapPending, snap)) return "Player unreadable";
        if (snap == 1) return "a ground snap is still pending";
        if (!w.ps) return "no DSPlayerSystem";
        std::uint8_t pushed = 0;
        if (!Rd(w.ps + kPsPushedTransform, pushed)) return "DSPlayerSystem unreadable";
        if (pushed & 1) return "a pushed player transform is pending (the game would warp Sam back)";
        if (player_out) *player_out = p;
    }
    if (need & kNeedSequenceCall) {
        bool bound = true;
        if (!CallIsBoundToSequence(s.e, &bound)) return "sequence check faulted";
        if (bound) return "Sam is bound to a sequence (scripted scene)";
    }
    return nullptr;
}

// ---------------------------------------------------------------- teleport + verification
double g_now_ms = 0;
ULONGLONG g_last_teleport_tick = 0;
char g_last_teleport[160] = {};

struct Verify {
    bool active = false;
    int stage = 0;
    double t0 = 0;
    double target[3] = {};
    int mode = 0;
    std::uintptr_t sam = 0;
    char what[24] = {};
};
Verify g_verify;

bool Place(const WorldTransform& target, TeleportMode mode, const char* what, char* msg, std::size_t cap) {
    const int m = static_cast<int>(mode);
    if (m != 1 && m != 2) {
        Put(msg, cap, "unknown teleport mode %d", m);
        return false;
    }
    if (!TeleportTargetSane(target.pos.x, target.pos.y, target.pos.z)) {
        Put(msg, cap, "target (%.2f, %.2f, %.2f) is not finite or out of range", target.pos.x, target.pos.y, target.pos.z);
        return false;
    }
    const ULONGLONG tick = GetTickCount64();
    if (g_last_teleport_tick && tick - g_last_teleport_tick < static_cast<ULONGLONG>(kTeleportMinIntervalMs)) {
        Put(msg, cap, "too soon after the last teleport (max 2 per second)");
        return false;
    }
    Sam s;
    ReadSam(&s);
    std::uintptr_t player = 0;
    if (const char* why = Blocked(s, kNeedPlacement | kNeedSequenceCall, &player)) {
        Put(msg, cap, "%s refused: %s", what, why);
        return false;
    }
    WorldTransform wt = target;
    const WorldTransform from = s.wt;
    g_last_teleport_tick = tick;
    if (!CallPlaceOn(player, &wt, m)) {
        Log("[game] teleport: PlaceOn FAULTED (player %p); teleport disabled for this run", reinterpret_cast<void*>(player));
        g_features &= ~(kFeatTeleport | kFeatUnstuck);
        g_feature_reason[0] = g_feature_reason[1] = "PlaceOn faulted";
        Put(msg, cap, "%s: PlaceOn faulted; teleport disabled", what);
        return false;
    }
    WorldTransform now{};
    const bool now_ok = ReadTransform(s.e, &now);
    Log("[game] teleport (%s): PlaceOn(mode %d) to (%.2f, %.2f, %.2f) from (%.2f, %.2f, %.2f); right after: (%.2f, %.2f, %.2f)%s",
        what, m, target.pos.x, target.pos.y, target.pos.z, from.pos.x, from.pos.y, from.pos.z, now.pos.x, now.pos.y, now.pos.z,
        now_ok ? "" : " (unreadable)");
    g_verify = Verify{};
    g_verify.active = true;
    g_verify.t0 = g_now_ms;
    g_verify.target[0] = target.pos.x, g_verify.target[1] = target.pos.y, g_verify.target[2] = target.pos.z;
    g_verify.mode = m;
    g_verify.sam = s.e;
    std::snprintf(g_verify.what, sizeof(g_verify.what), "%s", what);
    std::snprintf(g_last_teleport, sizeof(g_last_teleport), "%s (%.1f, %.1f, %.1f) mode %d: verifying...", what, target.pos.x,
                  target.pos.y, target.pos.z, m);
    if (m == 2)
        Put(msg, cap, "%s: placed at (%.2f, %.2f), ground snap next frame", what, target.pos.x, target.pos.y);
    else
        Put(msg, cap, "%s: placed at (%.2f, %.2f, %.2f)", what, now.pos.x, now.pos.y, now.pos.z);
    return true;
}

void RunVerify() {
    if (!g_verify.active) return;
    const double due = g_verify.t0 + (g_verify.stage == 0 ? 500.0 : 2000.0);
    if (g_now_ms < due) return;
    Sam s;
    ReadSam(&s);
    const World w = ReadWorld();
    std::uint32_t mask = 0;
    if (w.gm) Rd(w.gm + kGmLoadMask, mask);
    const char* label = g_verify.stage == 0 ? "+0.5 s" : "+2 s";
    if (!s.e || !s.wt_ok) {
        Log("[game] teleport check %s: no local player readable (load mask 0x%X)", label, mask);
        if (g_verify.stage == 1)
            std::snprintf(g_last_teleport, sizeof(g_last_teleport), "%s: Sam not readable 2 s later", g_verify.what);
    } else {
        const double dx = s.wt.pos.x - g_verify.target[0], dy = s.wt.pos.y - g_verify.target[1];
        const double dh = std::sqrt(dx * dx + dy * dy);
        const double dz = s.wt.pos.z - g_verify.target[2];
        const bool ok = dh < 3.0 && (g_verify.mode == 2 || std::fabs(dz) < 5.0);
        Log("[game] teleport check %s: Sam%s at (%.2f, %.2f, %.2f), %.2f m from the target horizontally, dz %.2f; flags=0x%llX "
            "load mask=0x%X -> %s",
            label, s.e == g_verify.sam ? "" : " (NEW ENTITY)", s.wt.pos.x, s.wt.pos.y, s.wt.pos.z, dh, dz,
            static_cast<unsigned long long>(s.flags), mask, ok ? "ok" : "NOT AT TARGET");
        if (g_verify.stage == 1)
            std::snprintf(g_last_teleport, sizeof(g_last_teleport), "%s mode %d: %s, %.1f m off horizontally, dz %.1f (z %.1f)",
                          g_verify.what, g_verify.mode, ok ? "verified" : "NOT at target", dh, dz, s.wt.pos.z);
    }
    if (++g_verify.stage > 1) g_verify.active = false;
}

// Echo line rotation for the voice ping.
int g_ping_next = 0;

}  // namespace

// ================================================================== public
bool TeleportLocalPlayer(double x, double y, double z, TeleportMode mode, char* msg, std::size_t cap) noexcept {
    char local[160];
    if (!msg) msg = local, cap = sizeof(local);
    if (!Has(kFeatTeleport, msg, cap)) {
        Log("[game] teleport to (%.2f, %.2f, %.2f) refused: %s", x, y, z, msg);
        return false;
    }
    Sam s;
    ReadSam(&s);
    WorldTransform wt{};
    if (s.wt_ok) wt.rot = s.wt.rot;  // keep Sam's facing
    else wt.rot = RotMatrix{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    wt.pos = {x, y, z};
    const bool ok = Place(wt, mode, mode == TeleportMode::snap_to_ground ? "snap" : "teleport", msg, cap);
    if (!ok) Log("[game] %s", msg);
    return ok;
}

bool Unstuck(char* msg, std::size_t cap) noexcept {
    if (!Has(kFeatUnstuck, msg, cap)) return false;
    Sam s;
    if (!ReadSam(&s)) {
        Put(msg, cap, "no local player");
        return false;
    }
    const std::uintptr_t pc = Q(s.e + kSamParamComponent);
    if (!pc || RttiOf(pc) != Base() + kParamComponentRtti) {
        Put(msg, cap, "Sam+0x54C0 is not a DSPlayerParamComponent");
        return false;
    }
    std::uint8_t valid = 0;
    if (!Rd(pc + kPcSafeFlags, valid)) {
        Put(msg, cap, "DSPlayerParamComponent unreadable");
        return false;
    }
    WorldTransform a{}, b{};
    const bool has_a = (valid & 0x20) && ReadPosRot(pc + kPcSafeA, &a);
    const bool has_b = (valid & 0x40) && ReadPosRot(pc + kPcSafeB, &b);
    const auto dist2 = [&](const WorldTransform& t) {
        const double dx = t.pos.x - s.wt.pos.x, dy = t.pos.y - s.wt.pos.y, dz = t.pos.z - s.wt.pos.z;
        return dx * dx + dy * dy + dz * dz;
    };
    // Same choice as the game's picker (0x1427A4A40): B unless A is valid and nearer.
    const WorldTransform* pick = nullptr;
    if (has_b) pick = &b;
    if (has_a && (!pick || dist2(a) < dist2(b))) pick = &a;
    if (!pick) {
        Put(msg, cap, "the game has no safe position recorded (flags 0x%02X)", valid);
        return false;
    }
    Log("[game] unstuck: safe transforms A %s (%.2f, %.2f, %.2f) B %s (%.2f, %.2f, %.2f); using %s", has_a ? "valid" : "-",
        a.pos.x, a.pos.y, a.pos.z, has_b ? "valid" : "-", b.pos.x, b.pos.y, b.pos.z, pick == &a ? "A" : "B");
    const bool ok = Place(*pick, TeleportMode::place, "unstuck", msg, cap);
    if (!ok) Log("[game] %s", msg);
    return ok;
}

bool HealLocalPlayer(float amount, char* msg, std::size_t cap) noexcept {
    if (!Has(kFeatHeal, msg, cap)) return false;
    Sam s;
    if (!ReadSam(&s)) {
        Put(msg, cap, "no local player");
        return false;
    }
    if (!SamSlot(s, 0x98, kOnHeal)) {
        Put(msg, cap, "Sam's vtable is not DSPlayerEntity's");
        return false;
    }
    float h0 = 0, max = 0;
    if (!ReadHealth(s, &h0, &max)) {
        Put(msg, cap, "Sam's health is unreadable");
        return false;
    }
    if (const char* why = Blocked(s, kNeedSequenceCall, nullptr)) {
        Put(msg, cap, "heal refused: %s", why);
        return false;
    }
    if (!(amount > 0)) amount = max - h0;
    if (!(amount > 0.01f)) {
        Put(msg, cap, "already at full health (%.1f / %.1f)", h0, max);
        return true;
    }
    if (amount > 100000.0f) amount = 100000.0f;
    // The heal gate (0x1426A05F0) dereferences both of these without a null check.
    if (!ReadWorld().ps || !Q(Base() + kHealGateGlobal)) {
        Put(msg, cap, "heal refused: DSPlayerSystem / game globals not ready");
        return false;
    }
    if (!CallHeal(s.e, amount)) {
        g_features &= ~kFeatHeal;
        g_feature_reason[3] = "Heal faulted";
        Put(msg, cap, "Entity_ExportedHeal faulted; heal disabled");
        return false;
    }
    float h1 = h0, m1 = max;
    ReadHealth(s, &h1, &m1);
    Put(msg, cap, "health %.1f -> %.1f / %.1f%s", h0, h1, m1, h1 > h0 ? "" : " (unchanged: the game refused?)");
    return h1 > h0;
}

bool SetTimeOfDay(float hours, char* msg, std::size_t cap) noexcept {
    if (!Has(kFeatTime, msg, cap)) return false;
    if (!(hours >= 0.0f && hours < 24.0f)) {
        Put(msg, cap, "time %.2f is outside 0..24", static_cast<double>(hours));
        return false;
    }
    const std::uintptr_t tod = TimeState();
    if (!tod) {
        Put(msg, cap, "no GameWorldTimeState (title screen or loading)");
        return false;
    }
    float before = 0, after = 0;
    Rd(tod + kTodHours, before);
    if (!CallSetTimeOfDay(tod, hours)) {
        g_features &= ~kFeatTime;
        g_feature_reason[4] = "SetTimeOfDay faulted";
        Put(msg, cap, "SetTimeOfDay faulted; disabled");
        return false;
    }
    Rd(tod + kTodHours, after);
    char a[8], b[8];
    FormatClock(before, a, sizeof(a));
    FormatClock(after, b, sizeof(b));
    const bool ok = std::fabs(after - hours) < 0.05f;
    Put(msg, cap, "time of day %s -> %s%s", a, b, ok ? "" : " (did not take)");
    return ok;
}

bool SetDayNightCycle(bool enabled, char* msg, std::size_t cap) noexcept {
    if (!Has(kFeatTime, msg, cap)) return false;
    const std::uintptr_t tod = TimeState();
    if (!tod) {
        Put(msg, cap, "no GameWorldTimeState (title screen or loading)");
        return false;
    }
    std::uint8_t before = 0, after = 0;
    const std::uint8_t v = enabled ? 1 : 0;
    if (!Rd(tod + kTodCycle, before) || !Write(tod + kTodCycle, &v, 1) || !Rd(tod + kTodCycle, after)) {
        Put(msg, cap, "could not write EnableDayNightCycle");
        return false;
    }
    Put(msg, cap, "day/night cycle %s -> %s%s", before ? "on" : "off", after ? "on" : "off",
        after ? "" : " (time frozen)");
    return after == v;
}

bool PartnerVoicePing(std::uintptr_t e, int index, char* msg, std::size_t cap) noexcept {
    if (!Has(kFeatEcho, msg, cap)) return false;
    if (!e) {
        Put(msg, cap, "no live partner");
        return false;
    }
    const std::uintptr_t base = Base();
    std::uint64_t flags = 0;
    std::int32_t n = 0;
    if (!g_image.Has(Q(e)) || !Rd(e + kEntFlags, flags) || !Rd(e + kEntComponents, n) || n < 0 || n > 512) {
        Put(msg, cap, "partner entity unreadable");
        return false;
    }
    const std::uintptr_t list = Q(e + kEntComponents + 8);
    std::uintptr_t comp = 0;
    for (std::int32_t i = 0; i < n && !comp; ++i) {
        const std::uintptr_t c = Q(list + 8 * static_cast<std::uintptr_t>(i));
        if (c && RttiOf(c) == base + kEchoComponentRtti) comp = c;
    }
    if (!comp) {
        Put(msg, cap, "the partner has no DSEchoComponent (%d components)", n);
        return false;
    }
    if (Q(comp + kEchoOwner) != e) {
        Put(msg, cap, "echo component owner mismatch");
        return false;
    }
    const std::uintptr_t res = Q(comp + kEchoResource);
    if (!res || RttiOf(res) != base + kEchoResourceRtti) {
        Put(msg, cap, "echo component has no DSEchoComponentResource");
        return false;
    }
    std::int32_t count = 0;
    if (!Rd(res + kEchoResCount, count) || count <= 0 || count > 4096) {
        Put(msg, cap, "the echo resource has no voice lines (count %d)", count);
        return false;
    }
    const std::uintptr_t data = Q(res + kEchoResData);
    const int idx = index >= 0 ? index % count : (g_ping_next++ % count);
    if (!data || !Q(data + 8 * static_cast<std::uintptr_t>(idx))) {
        Put(msg, cap, "voice line %d of %d is missing", idx, count);
        return false;
    }
    const World w = ReadWorld();
    if (!w.gs || !w.gm) {  // Play dereferences both without a null check
        Put(msg, cap, "no game state / GameModule yet");
        return false;
    }
    std::int32_t state = -1;
    Rd(comp + kEchoState, state);
    if (state < 0 || state > 3) {
        Put(msg, cap, "echo component state %d unexpected", state);
        return false;
    }
    bool accepted = false;
    if (!CallEchoPlay(comp, idx, 0.05f, &accepted)) {
        g_features &= ~kFeatEcho;
        g_feature_reason[8] = "Play faulted";
        Put(msg, cap, "DSEchoComponent::Play faulted; disabled");
        return false;
    }
    std::uint64_t g = 0;
    Rd(w.gs + kGsFlags, g);
    Log("[game] voice ping: Play(comp %p, line %d/%d) -> %d (state was %d, partner flags 0x%llX, game state 0x%llX)",
        reinterpret_cast<void*>(comp), idx, count, accepted ? 1 : 0, state, static_cast<unsigned long long>(flags),
        static_cast<unsigned long long>(g));
    if (!accepted) {
        char d[64];
        DescribeGameStateFlags(g & kStoppingGameEventMask, d, sizeof(d));
        Put(msg, cap, "the game refused the voice line (state %s)", d);
        return false;
    }
    Put(msg, cap, "voice line %d of %d queued%s", idx + 1, count, (flags & kFlagAsleep) ? " (partner asleep: may not play)" : "");
    return true;
}

void ActionsTick(double now_ms) noexcept {
    g_now_ms = now_ms;
    EnsureResolved();
    RunVerify();
    catalog::Tick(now_ms);
    census::Tick(now_ms);
}

bool StartHoloCatalog(int start, int count, double distance_m, char* msg, std::size_t cap) noexcept {
    return catalog::Start(start, count, distance_m, msg, cap);
}

bool StartClipSurvey(char* msg, std::size_t cap) noexcept { return catalog::StartClipSurvey(msg, cap); }

bool StartAnimCensus(double radius_m, int tag, int scan, char* msg, std::size_t cap) noexcept {
    return census::StartAnimCensus(radius_m, tag, scan, msg, cap);
}
bool PoseDump(int tag, int frames, double delay_ms, char* msg, std::size_t cap) noexcept {
    return census::PoseDump(tag, frames, delay_ms, msg, cap);
}
bool StartPoseScan(int tag, char* msg, std::size_t cap) noexcept { return census::StartPoseScan(tag, msg, cap); }
bool LookAtBody(int index, double dist_m, char* msg, std::size_t cap) noexcept { return census::LookAt(index, dist_m, msg, cap); }
bool RealBodyCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    return realbody::DebugCommand(op, v, msg, cap);
}

void ReadDiagnostics(Diagnostics* d) noexcept {
    *d = Diagnostics{};
    EnsureResolved();
    d->resolved = true;
    d->features = g_features;
    std::snprintf(d->disabled, sizeof(d->disabled), "%s", g_disabled);
    std::snprintf(d->last_teleport, sizeof(d->last_teleport), "%s", g_last_teleport);
    const World w = ReadWorld();
    if ((g_features & kFeatState) && w.gs)
        d->state_valid = Rd(w.gs + kGsFlags, d->state_flags) && Rd(w.gs + kGsRequested, d->state_requested) &&
                         Rd(w.gs + kGsPhase, d->state_phase);
    if ((g_features & kFeatModule) && w.gm)
        d->module_valid = Rd(w.gm + kGmLoadMask, d->load_mask) && Rd(w.gm + kGmLoadReason, d->load_reason);
    if ((g_features & kNeedsGuards) && w.ps) {
        std::uint8_t b = 0;
        d->player_system_valid = Rd(w.ps + kPsPushedTransform, b);
        d->pushed_transform = (b & 1) != 0;
    }
    if (g_features & kFeatArea) {
        const std::uintptr_t a = Q(Base() + kAreaChangeGlobal);
        std::uint8_t active = 0;
        if (a && Rd(a + kAreaActive, active) && active) d->area_valid = Rd(a + kAreaCurrent, d->area);
    }
    if (g_features & kFeatTime) {
        if (const std::uintptr_t tod = TimeState()) {
            std::uint8_t cycle = 0;
            d->tod_valid = Rd(tod + kTodHours, d->time_of_day) && Rd(tod + kTodDay, d->day) && Rd(tod + kTodCycle, cycle);
            d->day_night_cycle = cycle != 0;
        }
    }
    Sam s;
    const bool have_sam = (g_features & (kFeatHealth | kFeatTeleport)) && ReadSam(&s);
    if ((g_features & kFeatHealth) && have_sam) d->health_valid = ReadHealth(s, &d->health, &d->health_max);
    if (!(g_features & kFeatTeleport)) {
        std::snprintf(d->teleport_block, sizeof(d->teleport_block), "disabled (%s)",
                      g_feature_reason[0] ? g_feature_reason[0] : "byte check");
    } else if (const char* why = Blocked(s, kNeedPlacement, nullptr)) {
        std::snprintf(d->teleport_block, sizeof(d->teleport_block), "%s", why);
    } else {
        d->teleport_ready = true;
    }
}

}  // namespace st::game
