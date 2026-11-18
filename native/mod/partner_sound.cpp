#include "partner_sound.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "audio_probe.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "partner_voice.hpp"
#include "real_body.hpp"
#include "self_voice.hpp"

namespace st::psound {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// Evidence: docs/audio-research.md sections 2, 10, 12 and 16. The Wwise API is found by export name; the export must
// be the known RVA with the known first bytes (the same checks as the audio probe's).
constexpr std::uintptr_t kPostEvent = 0x3A1A5C0, kRegister = 0x3A1CF50, kUnregister = 0x3A21640, kSetPosition = 0x3A1FA50,
                         kSetRtpc = 0x3A1FC10, kResetRtpc = 0x3A1D5B0, kStopPlaying = 0x3A20B20, kStopAll = 0x3A20820,
                         kExecAction = 0x3A17E30, kBusVolume = 0x3A1EEF0;
// The "who registers this game object" contexts (section 12.5): the SoundComponent play paths (this = the component's
// representation; +0x20 the component, its +0x48 the owner entity) and the material / physics emitter start.
constexpr std::uintptr_t kPlaySound = 0x235A460, kPlaySound2 = 0x235AAC0, kPlaySound3 = 0x23493E0, kEmitterStart = 0x1AB3CA0;
constexpr std::uintptr_t kCreateInst = 0x1DB61A0, kSetupInst = 0x1DCBC30;
constexpr std::uintptr_t kCompOwner = 0x48, kEntParent = 0x70, kObjPos = 0x20;

struct Api {
    const char* name;
    const char* export_name;
    std::uintptr_t rva;
    const char* sig;
};
const Api kApis[] = {
    {"PostEvent", "?PostEvent@SoundEngine@AK@@YAKK_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z", kPostEvent,
     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 4C 89 74 24 20 41 57 48 83"},
    {"RegisterGameObj", "?RegisterGameObj@SoundEngine@AK@@YA?AW4AKRESULT@@_K@Z", kRegister, "40 53 48 83 EC 20 48 8B D9 48 83 F9 E0 72 0B B8 1F 00 00 00"},
    {"UnregisterGameObj", "?UnregisterGameObj@SoundEngine@AK@@YA?AW4AKRESULT@@_K@Z", kUnregister,
     "40 53 48 83 EC 20 48 8B D9 48 83 F9 E0 72 0B B8 02 00 00 00"},
    {"SetPosition", "?SetPosition@SoundEngine@AK@@YA?AW4AKRESULT@@_KAEBVAkTransform@@@Z", kSetPosition, "48 83 F9 E0 72 06 B8 02 00 00 00 C3 E9"},
    {"SetRTPCValue", "?SetRTPCValue@SoundEngine@AK@@YA?AW4AKRESULT@@KM_KJW4AkCurveInterpolation@@_N@Z", kSetRtpc,
     "48 83 EC 48 0F B6 44 24 78 88 44 24 30 8B 44 24 70 89 44 24 28 44 89 4C"},
    {"ResetRTPCValue", "?ResetRTPCValue@SoundEngine@AK@@YA?AW4AKRESULT@@K_KJW4AkCurveInterpolation@@_N@Z", kResetRtpc,
     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83"},
    {"StopPlayingID", "?StopPlayingID@SoundEngine@AK@@YAXKJW4AkCurveInterpolation@@@Z", kStopPlaying,
     "85 C9 74 5C 48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 8B F8 8B F2"},
    {"StopAll", "?StopAll@SoundEngine@AK@@YAX_K@Z", kStopAll, "40 53 48 83 EC 20 48 8B D9 E8"},
    {"ExecuteActionOnEvent", "?ExecuteActionOnEvent@SoundEngine@AK@@YA?AW4AKRESULT@@KW4AkActionOnEventType@12@_KJW4AkCurveInterpolation@@K@Z", kExecAction,
     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20"},
    {"SetGameObjectOutputBusVolume", "?SetGameObjectOutputBusVolume@SoundEngine@AK@@YA?AW4AKRESULT@@_K0M@Z", kBusVolume,
     "48 89 5C 24 08 57 48 83 EC 30 0F 29 74 24 20 48 8B DA 0F 57 F6 48 8B F9"},
};
enum ApiIdx { aPost, aReg, aUnreg, aSetPos, aSetRtpc, aResetRtpc, aStop, aStopAll, aExec, aBusVolume, aCount };

// ---------------------------------------------------------------- what the receiver may post (review 2026-11-18)
// A partner's item is posted only when its event id is on the allow-list: the ids the Phase 3 runs mirrored from the
// player's own allowed objects (foley, footsteps / movement layer, the rifle; runs 20261118-0809..0822), plus every id
// this game itself posted on its own local player's allowed objects this session (learned on the sender side: both
// games play Sam's same components). Never: the voice presets, the camera's events, the fixed global posts.
constexpr std::uint32_t kSeedEvents[] = {
    0x0C7CEF35, 0x0D308F5C, 0x15DFDF91, 0x15F91DB6, 0x194419B8, 0x1B272460, 0x22174D4D, 0x24F32DF8, 0x296A7387, 0x2975C1FF, 0x38BA98A1, 0x40DA79C3,
    0x4766394C, 0x49E1E759, 0x4B4AD6BB, 0x4C41C292, 0x5BB081B1, 0x64E5ECB2, 0x7E17A383, 0x9CA13B94, 0xA232AEF1, 0xAD954B69, 0xADEF2A60, 0xBDB90313,
    0xC5DEF120, 0xC62A4A71, 0xCEA73187, 0xDFB9648A, 0xEBC2378E, 0xFCF94B42,                                              // foley
    0x04A0FA03, 0x0B509EF4, 0x0C053F73, 0x334593AE, 0x3BA60DF6, 0x3C13FF22, 0x3CA60F68, 0x3CA60F6F, 0x3D40C67B, 0x5BE9FB68,
    0x822D6D84, 0x8AF12B8F, 0x8BF12D61, 0xEAD938AE, 0xEBD93A00,                                                          // footsteps, movement
    0x6D69D583, 0xC1CFA276, 0xEE160B69,                                                                                  // weapon
};
constexpr std::uint32_t kDenyEvents[] = {
    0x3D4BF9E2, 0x1C5DF301, 0x3E51AF82, 0xA4D58CE8, 0x7B540EFF, 0xB4FECB5F, 0x537EDC0A, 0x83FBDDAA,  // Sam's voice presets, the menu figure's
    0x8118407C, 0xD44C68CF, 0xE5BEA679, 0x374FF1A2, 0x518FE01D,                                      // BB, children, radio-like voices
    0xE9C1B3E7, 0xE9C1B3E4, 0x39FE036A, 0x43582143, 0xCB8293CB, 0x0623267B,                          // the camera's SoundComponent
    0x7913AFCB, 0x0AF4925B, 0xFD842642,                                                              // fixed posts on game objects 0 / ~0
};

using CallbackFn = void (*)(int, void*);
using PostEventFn = std::uint32_t (*)(std::uint32_t, std::uint64_t, std::uint32_t, CallbackFn, void*, std::uint32_t, void*, std::uint32_t);
using RegisterFn = int (*)(std::uint64_t, const char*);
using UnregisterFn = int (*)(std::uint64_t);
using SetPositionFn = int (*)(std::uint64_t, const void*);
using SetRtpcFn = int (*)(std::uint32_t, float, std::uint64_t, std::int32_t, int, bool);
using ResetRtpcFn = int (*)(std::uint32_t, std::uint64_t, std::int32_t, int, bool);
using StopPlayingFn = void (*)(std::uint32_t, std::int32_t, int);
using StopAllFn = void (*)(std::uint64_t);
using ExecActionFn = int (*)(std::uint32_t, int, std::uint64_t, std::int32_t, int, std::uint32_t);
using Play6Fn = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using Play7Fn = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using EmitterStartFn = void (*)(void*);

constexpr int kCurveLinear = 4;      // AkCurveInterpolation_Linear
constexpr std::uint32_t kEndOfEvent = 1;  // AK_EndOfEvent

Config g_cfg;
bool g_enabled = false;
std::atomic<bool> g_on{false};
std::uintptr_t g_base = 0;
std::uintptr_t g_api[aCount] = {};
bool g_own_hooks = false;
bool g_post_hooked = false;  // a PostEvent detour (ours or the probe's) calls selfvoice::PrePost
std::atomic<bool> g_fault{false};

// ---------------------------------------------------------------- our Wwise game objects (the receiver's emitters)
// Their ids are the addresses of these bytes: unique, inside this DLL, never a Decima object, below 0xFF..E0.
constexpr unsigned kPool = 24;
alignas(16) std::uint8_t g_pool_ids[kPool][16];
inline std::uint64_t PoolId(unsigned i) { return reinterpret_cast<std::uint64_t>(&g_pool_ids[i][0]); }
inline bool IsPool(std::uint64_t obj) {
    const auto lo = reinterpret_cast<std::uint64_t>(&g_pool_ids[0][0]), hi = reinterpret_cast<std::uint64_t>(&g_pool_ids[kPool - 1][0]) + 16;
    return obj >= lo && obj < hi;
}

// ---------------------------------------------------------------- the hot ring (any thread -> game thread)
enum HKind : std::uint8_t { hPost = 1, hReg, hUnreg, hRtpc, hStop, hAction, hOwnPost, hVoicePost };
struct HRec {
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint64_t> w[6];
};
constexpr std::uint32_t kHBits = 13, kHCap = 1u << kHBits, kHMask = kHCap - 1;
HRec g_hring[kHCap];
std::atomic<std::uint32_t> g_hhead{0};
std::uint32_t g_htail = 0, g_hdropped = 0;

std::uint64_t Qpc() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<std::uint64_t>(t.QuadPart);
}
double g_qpf_ms = 1.0;  // QPC ticks per millisecond

inline std::uint64_t F2(float a, float b) {
    std::uint32_t x = 0, y = 0;
    std::memcpy(&x, &a, 4), std::memcpy(&y, &b, 4);
    return static_cast<std::uint64_t>(x) | (static_cast<std::uint64_t>(y) << 32);
}
inline float Fl(std::uint32_t bits) {
    float f = 0;
    std::memcpy(&f, &bits, 4);
    return f;
}

void HPush(HKind k, std::uint32_t id, std::uint32_t a, std::uint32_t b, std::uint64_t obj, std::uint64_t w3, std::uint64_t w4) noexcept {
    const std::uint32_t i = g_hhead.fetch_add(1, std::memory_order_relaxed);
    HRec& r = g_hring[i & kHMask];
    r.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    r.w[0].store(static_cast<std::uint64_t>(k) | (static_cast<std::uint64_t>(id) << 32), std::memory_order_relaxed);
    r.w[1].store(static_cast<std::uint64_t>(a) | (static_cast<std::uint64_t>(b) << 32), std::memory_order_relaxed);
    r.w[2].store(obj, std::memory_order_relaxed);
    r.w[3].store(w3, std::memory_order_relaxed);
    r.w[4].store(w4, std::memory_order_relaxed);
    r.w[5].store(Qpc(), std::memory_order_relaxed);
    r.seq.store(i + 1, std::memory_order_release);
}

// ---------------------------------------------------------------- end-of-event callbacks (Wwise's thread)
struct CbRec {
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint64_t> cookie{0}, obj{0}, pid_ev{0};
};
constexpr std::uint32_t kCbCap = 1024;
CbRec g_cbring[kCbCap];
std::atomic<std::uint32_t> g_cbhead{0}, g_cb_calls{0}, g_cb_unread{0};
std::uint32_t g_cbtail = 0;

void EndOfEvent(int type, void* info) {
    g_cb_calls.fetch_add(1, std::memory_order_relaxed);
    if ((static_cast<std::uint32_t>(type) & kEndOfEvent) == 0 || !info) return;
    // AkEventCallbackInfo (Wwise 2019.2): {void* pCookie; AkGameObjectID gameObjID; AkPlayingID playingID; AkUniqueID eventID}.
    // Read fault-free; the game thread checks the layout against what it posted (the object of the cookie's slot).
    std::uint64_t w[3] = {};
    if (!mem::Read(reinterpret_cast<std::uintptr_t>(info), w, sizeof(w))) {
        g_cb_unread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint32_t i = g_cbhead.fetch_add(1, std::memory_order_relaxed);
    CbRec& r = g_cbring[i % kCbCap];
    r.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    r.cookie.store(w[0], std::memory_order_relaxed);
    r.obj.store(w[1], std::memory_order_relaxed);
    r.pid_ev.store(w[2], std::memory_order_relaxed);
    r.seq.store(i + 1, std::memory_order_release);
}

// ---------------------------------------------------------------- own hooks (when the audio probe is not installed)
PostEventFn o_post = nullptr;
RegisterFn o_reg = nullptr;
UnregisterFn o_unreg = nullptr;
SetRtpcFn o_rtpc = nullptr;
StopPlayingFn o_stop = nullptr;
ExecActionFn o_exec = nullptr;
Play6Fn o_play1 = nullptr, o_play2 = nullptr;
Play7Fn o_play3 = nullptr;
EmitterStartFn o_emitter = nullptr;
thread_local std::uintptr_t t_ctx = 0;   // the component representation (or emitter) this thread is inside
thread_local bool t_ctx_emitter = false;

std::uint32_t H_PostEvent(std::uint32_t ev, std::uint64_t obj, std::uint32_t flags, CallbackFn cb, void* cookie, std::uint32_t n_ext, void* ext,
                          std::uint32_t pid) {
    selfvoice::PrePost(ev, obj, n_ext);  // self_voice silencing (before the post)
    const std::uint32_t r = o_post(ev, obj, flags, cb, cookie, n_ext, ext, pid);
    HotPost(ev, obj, n_ext, r);
    return r;
}
int H_Register(std::uint64_t obj, const char* name) {
    const int r = o_reg(obj, name);
    HotRegister(obj, t_ctx, t_ctx_emitter);
    return r;
}
int H_Unregister(std::uint64_t obj) {
    // Before the original: once it returns, another thread may register the same pooled id, and its record must come
    // after this one in the ring (review 2026-11-18).
    HotUnregister(obj);
    selfvoice::NoteUnregister(obj);
    return o_unreg(obj);
}
int H_SetRtpc(std::uint32_t id, float value, std::uint64_t obj, std::int32_t ms, int curve, bool bypass) {
    const int r = o_rtpc(id, value, obj, ms, curve, bypass);
    HotRtpc(id, value, obj, ms);
    return r;
}
void H_StopPlaying(std::uint32_t pid, std::int32_t ms, int curve) {
    o_stop(pid, ms, curve);
    HotStop(pid, ms);
}
int H_ExecAction(std::uint32_t ev, int action, std::uint64_t obj, std::int32_t ms, int curve, std::uint32_t pid) {
    const int r = o_exec(ev, action, obj, ms, curve, pid);
    HotAction(ev, action, obj, ms, pid);
    return r;
}
std::uint64_t H_Play1(void* c, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6) {
    const std::uintptr_t prev = t_ctx;
    const bool pe = t_ctx_emitter;
    t_ctx = reinterpret_cast<std::uintptr_t>(c), t_ctx_emitter = false;
    const std::uint64_t r = o_play1(c, a2, a3, a4, a5, a6);
    t_ctx = prev, t_ctx_emitter = pe;
    return r;
}
std::uint64_t H_Play2(void* c, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6) {
    const std::uintptr_t prev = t_ctx;
    const bool pe = t_ctx_emitter;
    t_ctx = reinterpret_cast<std::uintptr_t>(c), t_ctx_emitter = false;
    const std::uint64_t r = o_play2(c, a2, a3, a4, a5, a6);
    t_ctx = prev, t_ctx_emitter = pe;
    return r;
}
std::uint64_t H_Play3(void* c, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7) {
    const std::uintptr_t prev = t_ctx;
    const bool pe = t_ctx_emitter;
    t_ctx = reinterpret_cast<std::uintptr_t>(c), t_ctx_emitter = false;
    const std::uint64_t r = o_play3(c, a2, a3, a4, a5, a6, a7);
    t_ctx = prev, t_ctx_emitter = pe;
    return r;
}
// An outer component context stays (as the probe does); otherwise the emitter itself is the context.
void H_EmitterStart(void* em) {
    const std::uintptr_t prev = t_ctx;
    const bool pe = t_ctx_emitter;
    if (!prev) t_ctx = reinterpret_cast<std::uintptr_t>(em), t_ctx_emitter = true;
    o_emitter(em);
    t_ctx = prev, t_ctx_emitter = pe;
}

struct HookDef {
    const char* name;
    std::uintptr_t rva;
    int api;  // index into kApis for an export, -1 for an internal function
    const char* sig;
    void* detour;
    void** original;
};
const HookDef kHooks[] = {
    {"PostEvent", kPostEvent, aPost, nullptr, reinterpret_cast<void*>(&H_PostEvent), reinterpret_cast<void**>(&o_post)},
    {"RegisterGameObj", kRegister, aReg, nullptr, reinterpret_cast<void*>(&H_Register), reinterpret_cast<void**>(&o_reg)},
    {"UnregisterGameObj", kUnregister, aUnreg, nullptr, reinterpret_cast<void*>(&H_Unregister), reinterpret_cast<void**>(&o_unreg)},
    {"SetRTPCValue", kSetRtpc, aSetRtpc, nullptr, reinterpret_cast<void*>(&H_SetRtpc), reinterpret_cast<void**>(&o_rtpc)},
    {"StopPlayingID", kStopPlaying, aStop, nullptr, reinterpret_cast<void*>(&H_StopPlaying), reinterpret_cast<void**>(&o_stop)},
    {"ExecuteActionOnEvent", kExecAction, aExec, nullptr, reinterpret_cast<void*>(&H_ExecAction), reinterpret_cast<void**>(&o_exec)},
    {"SoundComponent play", kPlaySound, -1, "40 55 56 41 54 41 56 41 57 48 81 EC F0 00 00 00 48 8B 05", reinterpret_cast<void*>(&H_Play1),
     reinterpret_cast<void**>(&o_play1)},
    {"SoundComponent play 2", kPlaySound2, -1, "40 53 41 55 41 56 41 57 48 81 EC C8 00 00 00 48 8B 05", reinterpret_cast<void*>(&H_Play2),
     reinterpret_cast<void**>(&o_play2)},
    {"SoundComponent play 3", kPlaySound3, -1, "40 53 55 41 54 41 55 41 57 48 81 EC A0 00 00 00 48 8B 05", reinterpret_cast<void*>(&H_Play3),
     reinterpret_cast<void**>(&o_play3)},
    {"sound emitter start", kEmitterStart, -1, "4C 8B DC 53 48 83 EC 60 48 8B 05", reinterpret_cast<void*>(&H_EmitterStart),
     reinterpret_cast<void**>(&o_emitter)},
};
constexpr std::size_t kHookCount = sizeof(kHooks) / sizeof(kHooks[0]);

bool CallsTo(std::uintptr_t site, std::uint8_t op, std::uintptr_t target) {
    std::uint8_t b = 0;
    return Rd(site, b) && b == op && mem::RipTarget(site, 1, 5) == target;
}

// The internal contexts are tied to their meaning by the instance create / attach calls inside them (the return
// addresses the probe saw, run 20261118-060035) and by the emitter start's only caller.
bool InternalSitesOk() {
    const std::uintptr_t b = g_base;
    return CallsTo(b + 0x235A943, 0xE8, b + kCreateInst) && CallsTo(b + 0x235A7CD, 0xE8, b + kSetupInst) &&  // play 1
           CallsTo(b + 0x235AC9E, 0xE8, b + kSetupInst) &&                                                     // play 2
           CallsTo(b + 0x234966F, 0xE8, b + kCreateInst) &&                                                    // play 3
           CallsTo(b + 0x1AB3D8B, 0xE8, b + kCreateInst) && CallsTo(b + 0x1AB11D8, 0xE8, b + kEmitterStart);  // emitter
}

// ---------------------------------------------------------------- guarded calls into Wwise (game thread)
std::uint32_t CallPost(std::uint32_t ev, std::uint64_t obj, void* cookie) {
    __try {
        return reinterpret_cast<PostEventFn>(g_api[aPost])(ev, obj, kEndOfEvent, &EndOfEvent, cookie, 0, nullptr, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fault.store(true);
        return 0;
    }
}
int CallRegister(std::uint64_t obj) {
    __try {
        return reinterpret_cast<RegisterFn>(g_api[aReg])(obj, "StrandedTogetherPartnerSound");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fault.store(true);
        return -1;
    }
}
int CallSetPosition(std::uint64_t obj, const float (&t)[9]) {
    __try {
        return reinterpret_cast<SetPositionFn>(g_api[aSetPos])(obj, t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fault.store(true);
        return -1;
    }
}
int CallSetRtpc(std::uint32_t id, float v, std::uint64_t obj, std::int32_t ms) {
    __try {
        return reinterpret_cast<SetRtpcFn>(g_api[aSetRtpc])(id, v, obj, ms, kCurveLinear, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fault.store(true);
        return -1;
    }
}
int CallResetRtpc(std::uint32_t id, std::uint64_t obj) {
    __try {
        return reinterpret_cast<ResetRtpcFn>(g_api[aResetRtpc])(id, obj, 0, kCurveLinear, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fault.store(true);
        return -1;
    }
}
void CallStop(std::uint32_t pid, std::int32_t ms) {
    __try {
        reinterpret_cast<StopPlayingFn>(g_api[aStop])(pid, ms, kCurveLinear);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fault.store(true);
    }
}
void CallStopAll(std::uint64_t obj) {
    __try {
        reinterpret_cast<StopAllFn>(g_api[aStopAll])(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fault.store(true);
    }
}
int CallExec(std::uint32_t ev, int action, std::uint64_t obj, std::int32_t ms, std::uint32_t pid) {
    __try {
        return reinterpret_cast<ExecActionFn>(g_api[aExec])(ev, action, obj, ms, kCurveLinear, pid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fault.store(true);
        return -1;
    }
}
using BusVolumeFn = int (*)(std::uint64_t, std::uint64_t, float);
int CallBusVolume(std::uint64_t obj, float v) {
    __try {
        return reinterpret_cast<BusVolumeFn>(g_api[aBusVolume])(obj, 0, v);  // listener: game object 0 (the default listener)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fault.store(true);
        return -1;
    }
}

// ---------------------------------------------------------------- joints (both sides)
// Slot -> joint of Sam's 77-joint layout (docs/real-partner-body.md: 31 JNT_C_B_008_Head, 68 / 63 the feet, 9 / 38
// the hands, 4 the spine the backpack hangs on, 1 the hips).
constexpr unsigned kSlotJoint[wire::kSoundSlots] = {31, 68, 63, 9, 38, 4, 1};
constexpr double kSlotFallbackZ[wire::kSoundSlots] = {1.55, 0.05, 0.05, 1.0, 1.0, 1.3, 1.0};  // over the root, no joints
const char* const kSlotName[wire::kSoundSlots] = {"head", "left foot", "right foot", "right hand", "left hand", "back", "hips"};
const char* const kCatName[wire::kSoundCategories] = {"foley", "steps", "weapon", "cargo", "gear"};

struct Joints {
    bool ok = false;           // the root read
    unsigned from_model = 0;   // joints read from the model (the rest: fallback heights)
    double root[3] = {};
    double p[wire::kSoundSlots][3] = {};
};
void ReadJoints(game::Entity* e, const game::WorldTransform* root_in, Joints* out) {
    *out = Joints{};
    game::WorldTransform wt{};
    if (root_in) wt = *root_in;
    else if (!e || !game::TryGetTransform(e, &wt)) return;
    out->ok = true;
    out->root[0] = wt.pos.x, out->root[1] = wt.pos.y, out->root[2] = wt.pos.z;
    for (unsigned s = 0; s < wire::kSoundSlots; ++s) {
        double p[3];
        if (e && realbody::JointWorld(e, kSlotJoint[s], p)) {
            out->p[s][0] = p[0], out->p[s][1] = p[1], out->p[s][2] = p[2];
            ++out->from_model;
        } else {
            out->p[s][0] = out->root[0], out->p[s][1] = out->root[1], out->p[s][2] = out->root[2] + kSlotFallbackZ[s];
        }
    }
}
double Dist(const double* a, const double* b) {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// ---------------------------------------------------------------- sender state (game thread)
enum Own : std::uint8_t { oNone = 0, oComp = 1, oEmitter = 2, oDenied = 3 };
struct Label {
    std::string text;
    bool denied = false;
    std::uint32_t regs = 0, posts = 0, sent = 0;
};
std::vector<Label> g_labels;  // [0] = "not the local player's"
unsigned LabelOf(const char* text, bool denied) {
    for (unsigned i = 0; i < g_labels.size(); ++i)
        if (g_labels[i].text == text) return i;
    if (g_labels.size() >= 200) return 0;
    Label l;
    l.text = text, l.denied = denied;
    g_labels.push_back(l);
    return static_cast<unsigned>(g_labels.size() - 1);
}
struct Decision {
    Own own = oNone;
    std::uint8_t cat = 0;
    std::uint16_t label = 0;
    double t = 0;
};
std::unordered_map<std::uintptr_t, Decision> g_ctx_cache;

struct Obj {
    Own own = oNone;
    std::uint8_t cat = 0, tag = 0, slot = 6;
    std::uint16_t label = 0;
    std::uint8_t nr = 0;
    std::uint32_t rid[wire::kMaxSoundRtpcs] = {};
    float rv[wire::kMaxSoundRtpcs] = {};
    double last_post_ms = -1e9;
};
std::unordered_map<std::uint64_t, Obj> g_objs;
std::uint8_t g_next_tag = 0;

struct Out {
    wire::SoundItem it;
    double t_ms;
};
std::deque<Out> g_out;
struct RtpcUpd {
    std::uint8_t tag, slot;
    std::uint32_t id;
    float v;
    std::uint16_t ms;
};
std::vector<RtpcUpd> g_rtpc_out;
struct Repeat {
    wire::SoundItem it;
    int skip;  // frames to wait first (the original goes through g_out)
    int left;  // copies still to send
};
std::vector<Repeat> g_repeat;
struct TxPid {
    std::uint8_t tag, cat;
    double t;
};
std::unordered_map<std::uint32_t, TxPid> g_tx_pids;
std::uint16_t g_tx_seq = 0;
double g_tx_prune_ms = 0;
game::Entity* g_local = nullptr;
Joints g_lj;
double g_lj_ms = -1;

// counters (sender)
std::uint32_t g_tx_cat[wire::kSoundCategories] = {}, g_tx_frames = 0, g_tx_items = 0, g_tx_stops = 0, g_tx_actions = 0, g_tx_rtpc = 0,
              g_tx_stale = 0, g_tx_gated = 0, g_tx_emitter_far = 0, g_tx_unowned = 0, g_tx_voice = 0, g_tx_own = 0, g_tx_denied = 0,
              g_tx_nopid = 0, g_tx_overflow = 0, g_tx_far_off = 0, g_tx_regs_ctx = 0, g_tx_regs_ours = 0, g_tx_repeats = 0, g_tx_invalid = 0;
double g_tx_maxoff = 0;
const char* g_last_tx_gate = nullptr;
bool g_verbose = false;
std::uint32_t g_logged_tx = 0, g_logged_rx = 0;

bool EndsWith(const char* s, const char* suffix) {
    const std::size_t a = std::strlen(s), b = std::strlen(suffix);
    return a >= b && std::strcmp(s + a - b, suffix) == 0;
}
const char* TypeOf(std::uintptr_t p) {
    const char* n = p ? mem::RttiName(mem::RttiOf(p)) : nullptr;
    return n ? n : "?";
}

Decision Resolve(std::uintptr_t ctx, bool emitter, double now) {
    auto it = g_ctx_cache.find(ctx);
    if (it != g_ctx_cache.end() && now - it->second.t < 5000) return it->second;
    Decision d;
    d.t = now;
    const auto local = reinterpret_cast<std::uintptr_t>(g_local);
    if (emitter) {
        d.own = g_cfg.sound_steps ? oEmitter : oDenied;
        d.cat = static_cast<std::uint8_t>(wire::SoundCategory::steps);
        d.label = static_cast<std::uint16_t>(LabelOf(g_cfg.sound_steps ? "material effect emitter (footsteps, movement; position rule)"
                                                                       : "material effect emitter (sound_steps=0)",
                                                     !g_cfg.sound_steps));
    } else {
        const char* tn = TypeOf(ctx);
        const bool rep = EndsWith(tn, "Rep");
        const std::uintptr_t comp = rep ? Q(ctx + 0x20) : ctx;
        const char* ctn = TypeOf(comp);
        const std::uintptr_t owner = comp ? Q(comp + kCompOwner) : 0;
        int depth = -1;
        const char* chain[6] = {};
        std::uintptr_t e = owner;
        for (int k = 0; k < 6 && e; ++k) {
            if (e == local) {
                depth = k;
                break;
            }
            chain[k] = TypeOf(e);
            e = Q(e + kEntParent);
        }
        if (depth < 0 || !local) {
            d.own = oNone;
        } else {
            const char* deny = nullptr;
            if (std::strstr(ctn, "Talk") || std::strstr(ctn, "Voice") || std::strstr(ctn, "Echo")) deny = "voice";
            bool weapon = false, cargo = false;
            for (int k = 0; k < depth; ++k) {
                if (std::strstr(chain[k], "Camera")) deny = "camera";
                else if (std::strstr(chain[k], "Odradek")) deny = "Odradek";
                if (std::strstr(chain[k], "Weapon")) weapon = true;
                if (std::strstr(chain[k], "Baggage") || std::strstr(chain[k], "Cargo")) cargo = true;
            }
            // Allow-list (review 2026-11-18): the player's own animation foley component, and the sound components of a
            // weapon or cargo entity under him. Everything else he owns (other player components, gear, the BB pod, the cuff
            // device: plain child Entities that cannot be told apart) stays on this game.
            const bool foley = depth == 0 && std::strstr(ctn, "DSCustomSoundComponent") != nullptr;
            if (!deny && !foley && !weapon && !cargo) deny = "not on the allow-list";
            char text[160];
            if (depth == 0) std::snprintf(text, sizeof(text), "%s%s%s%s of the player", deny ? "denied (" : "", deny ? deny : "", deny ? "): " : "", ctn);
            else
                std::snprintf(text, sizeof(text), "%s%s%s%s of %s (a child entity, depth %d)", deny ? "denied (" : "", deny ? deny : "", deny ? "): " : "", ctn,
                              chain[0], depth);
            d.own = deny ? oDenied : oComp;
            d.cat = static_cast<std::uint8_t>(foley ? wire::SoundCategory::foley : weapon ? wire::SoundCategory::weapon : wire::SoundCategory::cargo);
            d.label = static_cast<std::uint16_t>(LabelOf(text, deny != nullptr));
        }
    }
    if (g_ctx_cache.size() > 4096) g_ctx_cache.clear();
    g_ctx_cache[ctx] = d;
    return d;
}

void RtpcPut(Obj& o, std::uint32_t id, float v) {
    for (unsigned k = 0; k < o.nr; ++k)
        if (o.rid[k] == id) {
            // newest last: the receiver replays in order and the 12 kept are the most recent
            for (unsigned j = k; j + 1 < o.nr; ++j) o.rid[j] = o.rid[j + 1], o.rv[j] = o.rv[j + 1];
            o.rid[o.nr - 1] = id, o.rv[o.nr - 1] = v;
            return;
        }
    if (o.nr == wire::kMaxSoundRtpcs) {
        for (unsigned j = 0; j + 1 < o.nr; ++j) o.rid[j] = o.rid[j + 1], o.rv[j] = o.rv[j + 1];
        --o.nr;
    }
    o.rid[o.nr] = id, o.rv[o.nr] = v, ++o.nr;
}

// Event ids this game posted on its own local player's allowed objects (the receiver's learned allow-list).
std::unordered_set<std::uint32_t> g_learned;
std::uint32_t g_learned_new = 0;
void Learn(std::uint32_t ev) {
    if (g_learned.size() < 4096 && g_learned.insert(ev).second) ++g_learned_new;
}
bool Denied(std::uint32_t ev) {
    for (std::uint32_t d : kDenyEvents)
        if (d == ev) return true;
    return false;
}
bool Allowed(std::uint32_t ev) {
    if (Denied(ev)) return false;
    for (std::uint32_t s : kSeedEvents)
        if (s == ev) return true;
    return g_learned.count(ev) != 0;
}

void Queue(const wire::SoundItem& it, double t) {
    if (g_out.size() >= 256) {
        g_out.pop_front();
        ++g_tx_overflow;
    }
    g_out.push_back(Out{it, t});
}

void OnPost(std::uint32_t ev, std::uint64_t obj, std::uint32_t pid, const float (&pos)[3], bool have_pos, double t_ms, double now) {
    auto it = g_objs.find(obj);
    if (it == g_objs.end() || it->second.own == oNone) {
        ++g_tx_unowned;
        return;
    }
    Obj& o = it->second;
    if (o.label < g_labels.size()) ++g_labels[o.label].posts;
    if (o.own == oDenied) {
        ++g_tx_denied;
        return;
    }
    if (g_lj_ms != now) {
        ReadJoints(g_local, nullptr, &g_lj);
        g_lj_ms = now;
    }
    if (!g_lj.ok) {
        ++g_tx_gated;
        return;
    }
    double p[3] = {pos[0], pos[1], pos[2]};
    const bool pos_ok = have_pos && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && !(p[0] == 0 && p[1] == 0 && p[2] == 0);
    if (o.own == oEmitter) {
        const double dr = pos_ok ? Dist(p, g_lj.root) : 1e9;
        const double df = pos_ok ? (std::min)(Dist(p, g_lj.p[1]), Dist(p, g_lj.p[2])) : 1e9;
        if (!(dr <= kOwnRootM || df <= kOwnFootM)) {
            ++g_tx_emitter_far;
            return;
        }
    }
    if (pid == 0) {  // the game's own post failed: nothing plays on the sender either
        ++g_tx_nopid;
        return;
    }
    Learn(ev);  // this game plays it on its own player: the partner's copy may be posted here
    if (Denied(ev)) {
        ++g_tx_denied;
        return;
    }
    if (const char* why = pvoice::LocalGate(g_local)) {
        ++g_tx_gated;
        if (why != g_last_tx_gate) g_last_tx_gate = why;
        return;
    }
    unsigned slot = 6;
    double off[3] = {0, 0, 0};
    if (pos_ok) {
        double best = 1e18;
        for (unsigned s = 0; s < wire::kSoundSlots; ++s) {
            const double d = Dist(p, g_lj.p[s]);
            if (d < best) best = d, slot = s;
        }
        for (int k = 0; k < 3; ++k) off[k] = p[k] - g_lj.p[slot][k];
        const double m = std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
        if (m > g_tx_maxoff) g_tx_maxoff = m;
        if (m > 50.0) ++g_tx_far_off;
    }
    if (!o.tag) {
        g_next_tag = static_cast<std::uint8_t>(g_next_tag == 255 ? 1 : g_next_tag + 1);
        o.tag = g_next_tag;
    }
    o.slot = static_cast<std::uint8_t>(slot);
    o.last_post_ms = now;
    // RTPC changes queued for this object are in the post's full set (they were set right before it, section 12.4)
    g_rtpc_out.erase(std::remove_if(g_rtpc_out.begin(), g_rtpc_out.end(), [&](const RtpcUpd& u) { return u.tag == o.tag; }), g_rtpc_out.end());
    wire::SoundItem item;
    item.op = static_cast<std::uint8_t>(wire::SoundOp::post);
    item.a = ev, item.b = pid;
    item.slot = static_cast<std::uint8_t>(slot), item.tag = o.tag, item.category = o.cat;
    for (int k = 0; k < 3; ++k) {
        const double cm = std::round(off[k] * 100.0);
        item.off[k] = static_cast<std::int16_t>(cm > 32000 ? 32000 : cm < -32000 ? -32000 : cm);
    }
    item.nrtpc = o.nr;
    for (unsigned k = 0; k < o.nr; ++k) item.rtpc_id[k] = o.rid[k], item.rtpc_value[k] = o.rv[k];
    Queue(item, t_ms);
    if (g_tx_pids.size() < 65536) g_tx_pids[pid] = TxPid{o.tag, o.cat, now};
    if (o.label < g_labels.size()) ++g_labels[o.label].sent;
    if (g_verbose || g_logged_tx < 30) {
        ++g_logged_tx;
        Log("[psound] mirror post %s ev 0x%08X pid %u obj 0x%llX tag %u (%s) | slot %s offset (%.2f, %.2f, %.2f) m, %.2f m from the local player's root | "
            "%u rtpc(s) | %.1f ms after the post",
            kCatName[o.cat], ev, pid, static_cast<unsigned long long>(obj), o.tag, o.label < g_labels.size() ? g_labels[o.label].text.c_str() : "?",
            kSlotName[slot], off[0], off[1], off[2], pos_ok ? Dist(p, g_lj.root) : -1.0, o.nr, now - t_ms);
    }
}

void DrainHot(double now) {
    std::uint32_t head = g_hhead.load(std::memory_order_acquire);
    const std::uint32_t dropped_before = g_hdropped;
    if (head - g_htail > kHCap) {
        g_hdropped += head - g_htail - kHCap;
        g_htail = head - kHCap;
    }
    const std::uint64_t qnow = Qpc();
    while (g_htail != head) {
        HRec& r = g_hring[g_htail & kHMask];
        const std::uint32_t s1 = r.seq.load(std::memory_order_acquire);
        if (s1 != g_htail + 1) {
            if (s1 == 0 || static_cast<std::int32_t>(s1 - (g_htail + 1)) < 0) break;  // being written: next tick
            ++g_hdropped, ++g_htail;
            continue;
        }
        std::uint64_t w[6];
        for (int k = 0; k < 6; ++k) w[k] = r.w[k].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (r.seq.load(std::memory_order_relaxed) != s1) {
            ++g_hdropped, ++g_htail;
            continue;
        }
        ++g_htail;
        const auto k = static_cast<HKind>(w[0] & 0xFF);
        const auto id = static_cast<std::uint32_t>(w[0] >> 32);
        const auto a = static_cast<std::uint32_t>(w[1]), b = static_cast<std::uint32_t>(w[1] >> 32);
        const std::uint64_t obj = w[2];
        const double t_ms = now - static_cast<double>(qnow > w[5] ? qnow - w[5] : 0) / g_qpf_ms;
        if (!g_local) continue;
        switch (k) {
        case hReg: {
            // a: 1 = registered inside a context; b: 1 = the context is a material emitter. w3 = the context.
            Obj o;
            if (a) {
                ++g_tx_regs_ctx;
                const Decision d = Resolve(static_cast<std::uintptr_t>(w[3]), b != 0, now);
                o.own = d.own, o.cat = d.cat, o.label = d.label;
                if (d.own != oNone) {
                    ++g_tx_regs_ours;
                    if (d.label < g_labels.size()) ++g_labels[d.label].regs;
                }
            }
            if (o.own == oNone) g_objs.erase(obj);  // not ours: forget whatever the pooled instance was before
            else {
                if (g_objs.size() > 4096) g_objs.clear();
                g_objs[obj] = o;
            }
            break;
        }
        case hUnreg:
            g_objs.erase(obj);
            break;
        case hRtpc: {
            auto it = g_objs.find(obj);
            if (it == g_objs.end() || (it->second.own != oComp && it->second.own != oEmitter)) break;
            Obj& o = it->second;
            const float v = Fl(a);
            if (!std::isfinite(v) || std::fabs(v) > 1e7f || id == 0) break;
            RtpcPut(o, id, v);
            // A change on a sound we mirror (its object posted in the last 3 s): the receiver's copy follows.
            if (o.tag && now - o.last_post_ms < 3000 && g_rtpc_out.size() < 64) {
                bool merged = false;
                for (RtpcUpd& u : g_rtpc_out)
                    if (u.tag == o.tag && u.id == id) u.v = v, u.ms = static_cast<std::uint16_t>((std::min)(b, 10000u)), merged = true;
                if (!merged) g_rtpc_out.push_back(RtpcUpd{o.tag, o.slot, id, v, static_cast<std::uint16_t>((std::min)(b, 10000u))});
            }
            break;
        }
        case hPost: {
            const float pos[3] = {Fl(static_cast<std::uint32_t>(w[3])), Fl(static_cast<std::uint32_t>(w[3] >> 32)), Fl(static_cast<std::uint32_t>(w[4]))};
            OnPost(id, obj, a, pos, (w[4] >> 32) != 0, t_ms, now);
            break;
        }
        case hOwnPost:
            ++g_tx_own;
            break;
        case hVoicePost: {
            auto it = g_objs.find(obj);
            if (it != g_objs.end() && it->second.own != oNone) ++g_tx_voice;
            break;
        }
        case hStop: {
            auto it = g_tx_pids.find(id);
            if (it == g_tx_pids.end()) break;
            wire::SoundItem item;
            item.op = static_cast<std::uint8_t>(wire::SoundOp::stop);
            item.b = id;
            item.fade_ms = static_cast<std::uint16_t>((std::min)(a, 10000u));
            item.category = it->second.cat;
            g_tx_pids.erase(it);
            Queue(item, t_ms);
            if (g_repeat.size() < 32) g_repeat.push_back(Repeat{item, 1, 2});  // stops are re-sent twice (an unreliable link)
            ++g_tx_stops;
            break;
        }
        case hAction: {
            auto it = g_objs.find(obj);
            if (it == g_objs.end() || (it->second.own != oComp && it->second.own != oEmitter) || !it->second.tag || a > wire::kSoundActionMax) break;
            wire::SoundItem item;
            item.op = static_cast<std::uint8_t>(wire::SoundOp::action);
            item.a = id;
            item.b = static_cast<std::uint32_t>(w[3]);
            if (item.b && !g_tx_pids.count(item.b)) item.b = 0;
            item.slot = static_cast<std::uint8_t>(a), item.tag = it->second.tag, item.category = it->second.cat;
            item.fade_ms = static_cast<std::uint16_t>((std::min)(b, 10000u));
            Queue(item, t_ms);
            ++g_tx_actions;
            break;
        }
        default:
            break;
        }
    }
    // Records were lost (the ring overflowed or a slot was overwritten): a lost registration could leave a reused pooled
    // id marked as the player's. Forget every ownership; the next registrations in a context set them again.
    if (g_hdropped != dropped_before) {
        g_objs.clear();
        g_ctx_cache.clear();
        Log("[psound] %u hot record(s) lost: object ownership forgotten (rebuilt from the next registrations)", g_hdropped - dropped_before);
    }
    // Playing ids whose stop never came are forgotten after 30 minutes (a loop longer than that never sends its stop; the
    // receiver also stops anything it played for 15 minutes). One-shots never get a stop: the map is pruned by age only.
    if (g_tx_pids.size() > 512 && now >= g_tx_prune_ms) {
        g_tx_prune_ms = now + 10000;
        for (auto it = g_tx_pids.begin(); it != g_tx_pids.end();) it = now - it->second.t > 1800000 ? g_tx_pids.erase(it) : std::next(it);
    }
}

// ---------------------------------------------------------------- receiver state (game thread)
struct In {
    wire::SoundItem it;
    double t;       // when it sounded, on the pose ring's sender axis
    double recv_ms;  // our clock when it arrived
};
std::deque<In> g_in;
struct PoolObj {
    bool registered = false;
    std::uint8_t tag = 0, slot = 6;
    double off[3] = {};
    double bound_ms = -1e9, last_use_ms = -1e9;
    int active = 0;
    std::uint8_t nr = 0;
    bool nr_overflow = false;
    std::uint32_t rid[16] = {};
};
PoolObj g_pool[kPool];
struct RxPid {
    std::uint32_t local;   // our playing id
    std::uint32_t remote;  // the sender's
    std::uint8_t pool, cat;
    double t;
};
std::unordered_map<std::uint32_t, RxPid> g_rx_pids;     // the sender's playing id -> ours
std::unordered_map<std::uint32_t, RxPid> g_local_pids;  // our playing id -> where (end-of-event)
bool g_have_rx_seq = false;
std::uint16_t g_last_rx_seq = 0;
double g_tokens[wire::kSoundSlots] = {};
double g_tokens_ms = 0;
constexpr double kRatePerSlot = 16.0, kBurstPerSlot = 12.0;  // posts per second per slot, and the burst

bool g_was_gated = false;
double g_gate_since_ms = 0;
std::uint32_t g_gate_held_at = 0, g_gate_posted_at = 0;
char g_gate_reason[112] = "";
bool g_had_partner = false;
Joints g_pj;
double g_partner_dist = -1;

std::uint32_t g_rx_frames = 0, g_rx_lost = 0, g_rx_items = 0, g_rx_posted[wire::kSoundCategories] = {}, g_rx_failed = 0, g_rx_ended[wire::kSoundCategories] = {},
              g_rx_gated = 0, g_rx_far = 0, g_rx_late = 0, g_rx_early = 0, g_rx_limited = 0, g_rx_nobody = 0, g_rx_stops = 0, g_rx_stop_unknown = 0,
              g_rx_actions = 0, g_rx_rtpc = 0, g_rx_overflow = 0, g_rx_stopall = 0, g_cb_ok = 0, g_cb_bad = 0, g_cb_unknown = 0, g_rx_rebinds = 0,
              g_rx_steals = 0, g_rx_refused = 0, g_rx_reregistered = 0, g_rx_capped = 0, g_rx_muted = 0;
std::uint32_t g_refused_logged = 0;
float g_volume = 1.0f;     // partner_volume / 100 (SetGameObjectOutputBusVolume on our objects)
double g_next_cap_ms = 0;  // the 15-minute cap check
constexpr double kMaxPlayMs = 15.0 * 60.0 * 1000.0;
double g_lat_sum = 0, g_lat_min = 1e9, g_lat_max = -1e9;
std::uint32_t g_lat_n = 0;
double g_pos_err_max = 0;  // our object vs the partner joint + offset, at the moment of the post (m)
double g_next_status_ms = 0;
std::uint64_t g_status_hash = 0;
std::uint32_t g_failed_logged = 0;

void ToWwise(const double (&p)[3], float (&t)[9]) {
    // AkTransform in this build: {front, top, position} as floats (0x141DC13F0); Decima Z-up -> Wwise Y-up: (x, z, y).
    t[0] = 0, t[1] = 0, t[2] = 1;  // front: Decima +Y
    t[3] = 0, t[4] = 1, t[5] = 0;  // top: Decima +Z
    t[6] = static_cast<float>(p[0]), t[7] = static_cast<float>(p[2]), t[8] = static_cast<float>(p[1]);
}

void PlaceAt(unsigned i, double (&out)[3]) {
    const PoolObj& po = g_pool[i];
    for (int k = 0; k < 3; ++k) out[k] = g_pj.p[po.slot][k] + po.off[k];
}

unsigned PoolFor(std::uint8_t tag, double now) {
    if (tag)
        for (unsigned i = 0; i < kPool; ++i)
            if (g_pool[i].tag == tag && now - g_pool[i].bound_ms < 60000) return i;
    // a new binding: the idle object unused the longest (else the one unused the longest)
    unsigned best = kPool;
    for (unsigned i = 0; i < kPool; ++i)
        if (g_pool[i].active <= 0 && (best == kPool || g_pool[i].last_use_ms < g_pool[best].last_use_ms)) best = i;
    if (best == kPool) {
        best = 0;
        for (unsigned i = 1; i < kPool; ++i)
            if (g_pool[i].last_use_ms < g_pool[best].last_use_ms) best = i;
        ++g_rx_steals;
    }
    PoolObj& po = g_pool[best];
    if (!po.registered) {
        po.registered = CallRegister(PoolId(best)) == 1;  // AK_Success
        if (!po.registered) Log("[psound] RegisterGameObj(our object %u) failed", best);
        else if (g_volume != 1.0f && CallBusVolume(PoolId(best), g_volume) != 1) Log("[psound] SetGameObjectOutputBusVolume(our object %u) failed", best);
    } else {
        // the previous binding's RTPCs must not leak into this one
        for (unsigned k = 0; k < po.nr; ++k) CallResetRtpc(po.rid[k], PoolId(best));
    }
    po.nr = 0;
    po.nr_overflow = false;
    po.tag = tag;
    po.bound_ms = now;
    ++g_rx_rebinds;
    return best;
}

void SetRtpcs(unsigned i, const wire::SoundItem& it, std::int32_t ms) {
    PoolObj& po = g_pool[i];
    for (unsigned k = 0; k < it.nrtpc; ++k) {
        CallSetRtpc(it.rtpc_id[k], it.rtpc_value[k], PoolId(i), ms);
        bool known = false;
        for (unsigned j = 0; j < po.nr; ++j) known |= po.rid[j] == it.rtpc_id[k];
        if (!known) {
            if (po.nr < 16) po.rid[po.nr++] = it.rtpc_id[k];
            else po.nr_overflow = true;
        }
    }
}

bool RateOk(unsigned slot, double now) {
    if (g_tokens_ms <= 0) {
        for (double& t : g_tokens) t = kBurstPerSlot;
        g_tokens_ms = now;
    }
    const double dt = (now - g_tokens_ms) / 1000.0;
    g_tokens_ms = now;
    for (double& t : g_tokens) t = (std::min)(kBurstPerSlot, t + dt * kRatePerSlot);
    if (g_tokens[slot] < 1.0) return false;
    g_tokens[slot] -= 1.0;
    return true;
}

void Execute(const In& in, double play_ms, double now) {
    const wire::SoundItem& it = in.it;
    switch (static_cast<wire::SoundOp>(it.op)) {
    case wire::SoundOp::post: {
        if (!Allowed(it.a)) {
            ++g_rx_refused;
            if (g_refused_logged++ < 10)
                Log("[psound] partner post %s ev 0x%08X refused: not on the allow-list (not seen on this game's own player, or denied)",
                    kCatName[it.category < wire::kSoundCategories ? it.category : 0], it.a);
            return;
        }
        if (g_volume <= 0.0f) {  // partner_volume=0: nothing is posted
            ++g_rx_muted;
            return;
        }
        if (!RateOk(it.slot, now)) {
            ++g_rx_limited;
            return;
        }
        const unsigned i = PoolFor(it.tag, now);
        PoolObj& po = g_pool[i];
        if (!po.registered) {
            ++g_rx_failed;
            return;
        }
        po.slot = it.slot;
        for (int k = 0; k < 3; ++k) po.off[k] = it.off[k] / 100.0;
        double p[3];
        PlaceAt(i, p);
        float t[9];
        ToWwise(p, t);
        CallSetPosition(PoolId(i), t);
        SetRtpcs(i, it, 0);
        const std::uint32_t pid = CallPost(it.a, PoolId(i), reinterpret_cast<void*>(static_cast<std::uintptr_t>(i + 1)));
        po.last_use_ms = now;
        if (!pid) {
            ++g_rx_failed;
            if (g_failed_logged++ < 10) Log("[psound] PostEvent(0x%08X) on our object %u returned 0 (the event is unknown here, or Wwise forgot the object: re-registered next time)", it.a, i);
            // Wwise may have dropped our object (not observed): register it again before its next post
            po.registered = false, po.tag = 0, po.bound_ms = -1e9;
            ++g_rx_reregistered;
            return;
        }
        ++po.active;
        const unsigned cat = it.category < wire::kSoundCategories ? it.category : 0;
        ++g_rx_posted[cat];
        // both maps lose the entry at the sound's end-of-event callback (DrainCallbacks), a stop, or StopAllOurs
        const RxPid rp{pid, it.b, static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(cat), now};
        if (g_local_pids.size() < 8192) {
            g_rx_pids[it.b] = rp;
            g_local_pids[pid] = rp;
        }
        const double late = play_ms - in.t;
        g_lat_sum += late, ++g_lat_n;
        g_lat_min = (std::min)(g_lat_min, late), g_lat_max = (std::max)(g_lat_max, late);
        const double want[3] = {g_pj.p[it.slot][0] + it.off[0] / 100.0, g_pj.p[it.slot][1] + it.off[1] / 100.0, g_pj.p[it.slot][2] + it.off[2] / 100.0};
        const double err = Dist(p, want);
        g_pos_err_max = (std::max)(g_pos_err_max, err);
        if (g_verbose || g_logged_rx < 30) {
            ++g_logged_rx;
            Log("[psound] partner post %s ev 0x%08X (their pid %u) -> pid %u on our object %u (0x%llX, tag %u) | at (%.2f, %.2f, %.2f): the partner's %s "
                "joint%s (%.2f m over the body's root) + (%.2f, %.2f, %.2f) m, %.2f m from that joint; the partner %.1f m from the local player | play time - "
                "event time %+.0f ms | %u rtpc(s)",
                kCatName[cat], it.a, it.b, pid, i, static_cast<unsigned long long>(PoolId(i)), it.tag, p[0], p[1], p[2], kSlotName[it.slot],
                g_pj.from_model ? "" : " (fallback height)", g_pj.p[it.slot][2] - g_pj.root[2], it.off[0] / 100.0, it.off[1] / 100.0, it.off[2] / 100.0,
                Dist(p, g_pj.p[it.slot]), g_partner_dist, late, it.nrtpc);
        }
        break;
    }
    case wire::SoundOp::stop: {
        auto f = g_rx_pids.find(it.b);
        if (f == g_rx_pids.end()) {
            ++g_rx_stop_unknown;  // a repeat, or a sound we did not post
            return;
        }
        CallStop(f->second.local, it.fade_ms);
        g_rx_pids.erase(f);
        ++g_rx_stops;
        break;
    }
    case wire::SoundOp::action: {
        if (!Allowed(it.a)) {
            ++g_rx_refused;
            return;
        }
        unsigned i = kPool;
        for (unsigned k = 0; k < kPool; ++k)
            if (g_pool[k].tag == it.tag && g_pool[k].registered) i = k;
        if (i == kPool) return;
        std::uint32_t pid = 0;
        if (it.b) {
            auto f = g_rx_pids.find(it.b);
            if (f == g_rx_pids.end()) return;
            pid = f->second.local;
        }
        CallExec(it.a, it.slot, PoolId(i), it.fade_ms, pid);
        ++g_rx_actions;
        break;
    }
    case wire::SoundOp::rtpc: {
        for (unsigned k = 0; k < kPool; ++k)
            if (g_pool[k].tag == it.tag && g_pool[k].registered && now - g_pool[k].bound_ms < 60000) {
                SetRtpcs(k, it, it.fade_ms);
                ++g_rx_rtpc;
                break;
            }
        break;
    }
    }
}

void StopAllOurs(const char* why) {
    unsigned n = 0;
    for (unsigned i = 0; i < kPool; ++i)
        if (g_pool[i].registered && g_pool[i].active > 0) CallStopAll(PoolId(i)), ++n;
    g_rx_stopall += n;
    g_rx_pids.clear();
    if (n) Log("[psound] stopped the partner's sounds on %u object(s): %s", n, why);
}

void DrainCallbacks() {
    for (unsigned guard = 0; guard < kCbCap; ++guard) {
        const std::uint32_t head = g_cbhead.load(std::memory_order_acquire);
        if (head - g_cbtail > kCbCap) g_cbtail = head - kCbCap;
        if (g_cbtail == head) return;
        const CbRec& r = g_cbring[g_cbtail % kCbCap];
        if (r.seq.load(std::memory_order_acquire) != g_cbtail + 1) return;
        const std::uint64_t cookie = r.cookie.load(std::memory_order_relaxed), obj = r.obj.load(std::memory_order_relaxed),
                            pe = r.pid_ev.load(std::memory_order_relaxed);
        ++g_cbtail;
        const auto pid = static_cast<std::uint32_t>(pe);
        const bool cookie_ok = cookie >= 1 && cookie <= kPool;
        if (cookie_ok && obj == PoolId(static_cast<unsigned>(cookie - 1))) ++g_cb_ok;
        else ++g_cb_bad;
        auto f = g_local_pids.find(pid);
        if (f == g_local_pids.end()) {
            ++g_cb_unknown;
            continue;
        }
        if (f->second.pool < kPool && g_pool[f->second.pool].active > 0) --g_pool[f->second.pool].active;
        ++g_rx_ended[f->second.cat < wire::kSoundCategories ? f->second.cat : 0];
        auto rx = g_rx_pids.find(f->second.remote);  // the sender's id no longer plays here: a later stop for it is a no-op
        if (rx != g_rx_pids.end() && rx->second.local == pid) g_rx_pids.erase(rx);
        g_local_pids.erase(f);
    }
}

std::string CatText(const std::uint32_t (&c)[wire::kSoundCategories]) {
    std::string s;
    char b[48];
    for (unsigned i = 0; i < wire::kSoundCategories; ++i) {
        std::snprintf(b, sizeof(b), "%s%s %u", i ? ", " : "", kCatName[i], c[i]);
        s += b;
    }
    return s;
}

void Status(const char* why) {
    std::uint32_t tx_total = 0, rx_total = 0, ended = 0;
    for (unsigned i = 0; i < wire::kSoundCategories; ++i) tx_total += g_tx_cat[i], rx_total += g_rx_posted[i], ended += g_rx_ended[i];
    Log("[psound] %s | sent: %u post(s) (%s), %u stop(s), %u action(s), %u rtpc update(s), %u repeat(s) in %u frame(s); not sent: %u while paused / menu "
        "here%s%s%s, %u emitter post(s) not near the player, %u denied, %u voice, %u unowned, %u stale, %u overflow, %u failed on the sender; own posts "
        "ignored %u; registrations in a context %u (ours %u); hot ring dropped %u; invalid items dropped %u; event ids learned here %zu",
        why, tx_total, CatText(g_tx_cat).c_str(), g_tx_stops, g_tx_actions, g_tx_rtpc, g_tx_repeats, g_tx_frames, g_tx_gated, g_last_tx_gate ? " (" : "",
        g_last_tx_gate ? g_last_tx_gate : "", g_last_tx_gate ? ")" : "", g_tx_emitter_far, g_tx_denied, g_tx_voice, g_tx_unowned, g_tx_stale, g_tx_overflow,
        g_tx_nopid, g_tx_own, g_tx_regs_ctx, g_tx_regs_ours, g_hdropped, g_tx_invalid, g_learned.size());
    Log("[psound] %s | received: %u frame(s) (%u lost), %u item(s); posted %u (%s), PostEvent failed %u; ended %u (%s); end-of-event callbacks %u "
        "(layout ok %u, bad %u, not ours %u, unreadable %u); stops %u (%u unknown / repeats), actions %u, rtpc updates %u; not posted: %u paused / menu "
        "here, %u far, %u late, %u early, %u over the rate limit, %u no partner, %u queue overflow; stop-all %u; bindings %u (stolen %u); play time - "
        "event time %s%.0f / %.0f / %.0f ms (min / avg / max of %u); our object vs the joint + offset at the post: max %.3f m; refused (allow-list) %u, "
        "learned event ids %zu, re-registered %u, capped (15 min) %u, partner_volume 0 %u, volume %.2f",
        why, g_rx_frames, g_rx_lost, g_rx_items, rx_total, CatText(g_rx_posted).c_str(), g_rx_failed, ended, CatText(g_rx_ended).c_str(),
        g_cb_calls.load(), g_cb_ok, g_cb_bad, g_cb_unknown, g_cb_unread.load(), g_rx_stops, g_rx_stop_unknown, g_rx_actions, g_rx_rtpc, g_rx_gated, g_rx_far,
        g_rx_late, g_rx_early, g_rx_limited, g_rx_nobody, g_rx_overflow, g_rx_stopall, g_rx_rebinds, g_rx_steals, g_lat_n ? "" : "-",
        g_lat_n ? g_lat_min : 0.0, g_lat_n ? g_lat_sum / g_lat_n : 0.0, g_lat_n ? g_lat_max : 0.0, g_lat_n, g_pos_err_max, g_rx_refused, g_learned.size(),
        g_rx_reregistered, g_rx_capped, g_rx_muted, static_cast<double>(g_volume));
}

void LogLabels() {
    for (const Label& l : g_labels)
        if (l.regs || l.posts) Log("[psound] owner %s: %u registration(s), %u post(s), %u sent", l.text.c_str(), l.regs, l.posts, l.sent);
}

}  // namespace

// ---------------------------------------------------------------- hot paths
bool Capturing() noexcept { return g_on.load(std::memory_order_relaxed); }

void HotPost(std::uint32_t ev, std::uint64_t obj, std::uint32_t n_ext, std::uint32_t pid) noexcept {
    if (!g_on.load(std::memory_order_relaxed)) return;
    if (IsPool(obj)) {
        HPush(hOwnPost, ev, pid, 0, obj, 0, 0);
        return;
    }
    if (obj == 0 || obj >= 0xFFFFFFFFFFFFFFE0ull) return;  // the listener / global: never a player's
    if (n_ext) {
        HPush(hVoicePost, ev, pid, 0, obj, 0, 0);
        return;
    }
    double p[3] = {};
    const bool have = mem::Read(static_cast<std::uintptr_t>(obj) + kObjPos, p, sizeof(p));
    HPush(hPost, ev, pid, 0, obj, have ? F2(static_cast<float>(p[0]), static_cast<float>(p[1])) : 0,
          have ? (static_cast<std::uint64_t>(F2(static_cast<float>(p[2]), 0.0f) & 0xFFFFFFFFull) | (1ull << 32)) : 0);
}
void HotRegister(std::uint64_t obj, std::uintptr_t ctx, bool emitter) noexcept {
    if (!g_on.load(std::memory_order_relaxed) || IsPool(obj)) return;
    HPush(hReg, 0, ctx ? 1u : 0u, emitter ? 1u : 0u, obj, ctx, 0);
}
void HotUnregister(std::uint64_t obj) noexcept {
    if (!g_on.load(std::memory_order_relaxed) || IsPool(obj)) return;
    HPush(hUnreg, 0, 0, 0, obj, 0, 0);
}
void HotRtpc(std::uint32_t id, float value, std::uint64_t obj, std::int32_t ms) noexcept {
    if (!g_on.load(std::memory_order_relaxed) || obj == 0 || obj >= 0xFFFFFFFFFFFFFFE0ull || IsPool(obj)) return;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    HPush(hRtpc, id, bits, static_cast<std::uint32_t>(ms < 0 ? 0 : ms), obj, 0, 0);
}
void HotStop(std::uint32_t pid, std::int32_t ms) noexcept {
    if (!g_on.load(std::memory_order_relaxed) || !pid) return;
    HPush(hStop, pid, static_cast<std::uint32_t>(ms < 0 ? 0 : ms), 0, 0, 0, 0);
}
void HotAction(std::uint32_t ev, std::int32_t action, std::uint64_t obj, std::int32_t ms, std::uint32_t pid) noexcept {
    if (!g_on.load(std::memory_order_relaxed) || IsPool(obj) || action < 0) return;
    HPush(hAction, ev, static_cast<std::uint32_t>(action), static_cast<std::uint32_t>(ms < 0 ? 0 : ms), obj, pid, 0);
}

// ---------------------------------------------------------------- install
void Install(const Config& cfg) noexcept {
    g_cfg = cfg;
    // self_voice silencing needs a PostEvent detour that calls selfvoice::PrePost: with sound_sync=0 only that hook (and
    // UnregisterGameObj's) is installed, and nothing is recorded or sent.
    const bool voice_only = !cfg.sound_sync && selfvoice::WantsPostHook();
    if (!cfg.sound_sync && !voice_only) return;
    g_base = mem::Base();
    g_volume = static_cast<float>((std::max)(0, (std::min)(100, cfg.partner_volume))) / 100.0f;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf_ms = static_cast<double>(f.QuadPart) / 1000.0;
    g_labels.clear();
    g_labels.push_back(Label{"not the local player's", false, 0, 0, 0});
    const HMODULE exe = GetModuleHandleW(nullptr);
    std::string bad;
    for (int i = 0; i < aCount; ++i) {
        const Api& a = kApis[i];
        const auto got = reinterpret_cast<std::uintptr_t>(GetProcAddress(exe, a.export_name));
        const std::uintptr_t want = g_base + a.rva;
        const bool hooked_by_probe = audioprobe::Installed() && audioprobe::Hooked(a.rva);
        if (!got) bad += std::string(bad.empty() ? "" : ", ") + a.name + " (export not found)";
        else if (got != want) bad += std::string(bad.empty() ? "" : ", ") + a.name + " (export moved)";
        else if (!hooked_by_probe && !mem::CodeMatches(want, a.sig)) bad += std::string(bad.empty() ? "" : ", ") + a.name + " (bytes differ)";
        else g_api[i] = want;
    }
    if (!bad.empty()) {
        Log("[psound] sound_sync=1 but NOT enabled: %s", bad.c_str());
        return;
    }
    if (voice_only) {
        if (audioprobe::Installed()) {
            g_post_hooked = audioprobe::Hooked(kPostEvent);  // the probe's detour calls selfvoice::PrePost
            Log("[psound] PostEvent for self_voice: %s", g_post_hooked ? "the audio probe's hook" : "NOT hooked by the probe");
            return;
        }
        const HookDef* need[2] = {&kHooks[0], &kHooks[2]};  // PostEvent, UnregisterGameObj
        bool ok = true;
        for (const HookDef* h : need) {
            const std::uintptr_t at = g_base + h->rva;
            const MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(at), h->detour, h->original);
            const MH_STATUS q = s == MH_OK ? MH_QueueEnableHook(reinterpret_cast<void*>(at)) : s;
            if (q != MH_OK) {
                if (s == MH_OK) MH_RemoveHook(reinterpret_cast<void*>(at));
                Log("[psound] PostEvent for self_voice NOT hooked: %s (%s)", h->name, MH_StatusToString(q));
                ok = false;
            }
        }
        if (!ok) {
            for (const HookDef* h : need) MH_RemoveHook(reinterpret_cast<void*>(g_base + h->rva));
            return;
        }
        const MH_STATUS applied = MH_ApplyQueued();
        g_post_hooked = applied == MH_OK;
        g_own_hooks = g_post_hooked;
        Log("[psound] sound_sync=0: only PostEvent and UnregisterGameObj hooked, for self_voice (%s); nothing is recorded or sent",
            g_post_hooked ? "applied" : MH_StatusToString(applied));
        return;
    }
    if (!InternalSitesOk()) {
        Log("[psound] sound_sync=1 but NOT enabled: the SoundComponent play paths / emitter start call sites differ");
        return;
    }
    if (audioprobe::Installed()) {
        // The probe owns the hooks (one per target) and forwards every call here.
        std::string missing;
        for (const HookDef& h : kHooks)
            if (!audioprobe::Hooked(h.rva)) missing += std::string(missing.empty() ? "" : ", ") + h.name;
        if (!missing.empty()) {
            Log("[psound] sound_sync=1 but NOT enabled: the audio probe owns the Wwise hooks and lacks %s", missing.c_str());
            return;
        }
        Log("[psound] partner sounds on: the audio probe's hooks (%zu targets) forward here", kHookCount);
    } else {
        unsigned queued = 0;
        std::string ok, why_not;
        for (const HookDef& h : kHooks) {
            const std::uintptr_t at = g_base + h.rva;
            const char* why = nullptr;
            if (h.api < 0 && !mem::CodeMatches(at, h.sig)) why = "bytes differ";
            if (!why) {
                const MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(at), h.detour, h.original);
                if (s != MH_OK) why = MH_StatusToString(s);
                else if (MH_QueueEnableHook(reinterpret_cast<void*>(at)) != MH_OK) why = "queue";
                else ++queued;
            }
            if (why) why_not += std::string(why_not.empty() ? "" : ", ") + h.name + " (" + why + ")";
            else ok += std::string(ok.empty() ? "" : ", ") + h.name;
        }
        if (!why_not.empty()) {
            // all or nothing: remove what was created
            for (const HookDef& h : kHooks) MH_RemoveHook(reinterpret_cast<void*>(g_base + h.rva));
            Log("[psound] sound_sync=1 but NOT enabled: %s", why_not.c_str());
            return;
        }
        const MH_STATUS applied = MH_ApplyQueued();
        if (applied != MH_OK) {
            Log("[psound] sound_sync=1 but NOT enabled: MH_ApplyQueued %s", MH_StatusToString(applied));
            return;
        }
        g_own_hooks = true;
        Log("[psound] partner sounds on: %u hooks: %s", queued, ok.c_str());
    }
    g_enabled = true;
    g_post_hooked = true;  // ours, or the probe's (it checked every target above), and both call selfvoice::PrePost
    g_on.store(true);
    Log("[psound] footsteps by position (sound_steps) %s; our Wwise objects 0x%llX..0x%llX (%u); AkTransform = {front, top, position} floats, "
        "Decima (x, y, z) -> Wwise (x, z, y); partner_volume %d%%%s",
        cfg.sound_steps ? "on (within 1.0 m of the root or 0.5 m of a foot)" : "off", static_cast<unsigned long long>(PoolId(0)),
        static_cast<unsigned long long>(PoolId(kPool - 1)), kPool, static_cast<int>(g_volume * 100.0f + 0.5f),
        g_volume <= 0.0f ? " (nothing of the partner's is posted)" : g_volume < 1.0f ? " (SetGameObjectOutputBusVolume on our objects)" : "");
}

bool Enabled() noexcept { return g_enabled && !g_fault.load(); }
bool PostHooked() noexcept { return g_post_hooked; }

// ---------------------------------------------------------------- game thread
bool TakeSend(game::Entity* local, double now, wire::SoundFrame* out) noexcept {
    if (!Enabled()) return false;
    g_local = local;
    DrainHot(now);
    if (g_out.empty() && g_rtpc_out.empty() && g_repeat.empty()) return false;
    wire::SoundFrame& f = *out;
    f.n = 0;
    f.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now));
    std::size_t size = wire::kSoundHeaderSize;
    while (!g_out.empty() && f.n < wire::kMaxSoundItems) {
        Out& o = g_out.front();
        const double age = now - o.t_ms;
        if (age > wire::kMaxSoundAgeMs) {
            ++g_tx_stale;
            g_out.pop_front();
            continue;
        }
        const std::size_t need = wire::SoundItemSize(o.it.nrtpc);
        if (size + need > wire::kMaxSoundFrameSize) break;
        o.it.age_ms = static_cast<std::uint16_t>(age < 0 ? 0 : age);
        if (!wire::SoundItemValid(o.it)) {  // one bad item must not take the whole frame (stops included) with it
            ++g_tx_invalid;
            g_out.pop_front();
            continue;
        }
        f.item[f.n++] = o.it;
        size += need;
        if (o.it.op == static_cast<std::uint8_t>(wire::SoundOp::post)) ++g_tx_cat[o.it.category < wire::kSoundCategories ? o.it.category : 0];
        g_out.pop_front();
    }
    // RTPC changes on mirrored sounds, grouped per object
    while (!g_rtpc_out.empty() && f.n < wire::kMaxSoundItems) {
        wire::SoundItem it;
        it.op = static_cast<std::uint8_t>(wire::SoundOp::rtpc);
        it.tag = g_rtpc_out.front().tag, it.slot = g_rtpc_out.front().slot;
        it.fade_ms = g_rtpc_out.front().ms;
        std::vector<RtpcUpd> rest;
        for (const RtpcUpd& u : g_rtpc_out) {
            if (u.tag == it.tag && it.nrtpc < wire::kMaxSoundRtpcs) it.rtpc_id[it.nrtpc] = u.id, it.rtpc_value[it.nrtpc] = u.v, ++it.nrtpc;
            else rest.push_back(u);
        }
        const std::size_t need = wire::SoundItemSize(it.nrtpc);
        if (size + need > wire::kMaxSoundFrameSize) break;
        g_rtpc_out.swap(rest);
        if (!wire::SoundItemValid(it)) {
            ++g_tx_invalid;
            continue;
        }
        f.item[f.n++] = it;
        size += need;
        ++g_tx_rtpc;
    }
    g_rtpc_out.clear();  // what did not fit is superseded by the next change (or the next post's full set)
    // stops again (twice after the first)
    for (auto it = g_repeat.begin(); it != g_repeat.end();) {
        if (it->skip > 0) {  // the first copy is in g_out (sent above or next time): the repeats start one frame later
            --it->skip;
            ++it;
            continue;
        }

        if (f.n >= wire::kMaxSoundItems || size + wire::kSoundItemSize > wire::kMaxSoundFrameSize) break;
        if (!wire::SoundItemValid(it->it)) {
            ++g_tx_invalid;
            it = g_repeat.erase(it);
            continue;
        }
        f.item[f.n++] = it->it;
        size += wire::kSoundItemSize;
        ++g_tx_repeats;
        it = --it->left <= 0 ? g_repeat.erase(it) : std::next(it);
    }
    if (!f.n) return false;
    f.seq = ++g_tx_seq;
    g_tx_items += f.n;
    ++g_tx_frames;
    return true;
}

void OnFrame(const wire::SoundFrame& f, double sender_axis_ms, double now) noexcept {
    if (!Enabled()) return;
    ++g_rx_frames;
    if (g_have_rx_seq) {
        const std::uint16_t gap = static_cast<std::uint16_t>(f.seq - g_last_rx_seq);
        if (gap > 1 && gap < 1000) g_rx_lost += gap - 1u;
        if (gap > 0 && gap < 32768) g_last_rx_seq = f.seq;  // a reordered (older) frame is kept by the link but does not move the newest seq
        else if (g_rx_lost) --g_rx_lost;                   // it was counted as lost when the newer one came
    } else {
        g_last_rx_seq = f.seq;
    }
    g_have_rx_seq = true;
    for (unsigned i = 0; i < f.n; ++i) {
        if (g_in.size() >= 512) {
            g_in.pop_front();
            ++g_rx_overflow;
        }
        g_in.push_back(In{f.item[i], sender_axis_ms - f.item[i].age_ms, now});
        ++g_rx_items;
    }
}

void NewSession() noexcept {
    if (!Enabled()) return;
    StopAllOurs("new link session");
    for (PoolObj& po : g_pool) po.tag = 0, po.bound_ms = -1e9;
    g_in.clear();
    g_have_rx_seq = false;
    // g_local_pids is kept: the end-of-event callbacks of the sounds just stopped still come and bring each object's
    // `active` count back down (cleared here, those objects counted as busy forever; review 2026-11-18).
}

void Tick(const Frame& fr, double now) noexcept {
    if (!g_enabled) return;
    if (g_fault.load()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            g_on.store(false);
            Log("[psound] a Wwise call faulted: partner sounds off for this session");
        }
        return;
    }
    g_local = fr.local;
    DrainHot(now);
    DrainCallbacks();
    // the partner body's joints (the receiver's emitters follow them)
    const bool partner = fr.have_root;
    if (partner) ReadJoints(fr.body, &fr.root, &g_pj);
    else g_pj = Joints{};
    double lroot[3] = {};
    game::WorldTransform lwt{};
    const bool lok = fr.local && game::TryGetTransform(fr.local, &lwt);
    if (lok) lroot[0] = lwt.pos.x, lroot[1] = lwt.pos.y, lroot[2] = lwt.pos.z;
    g_partner_dist = partner && lok ? Dist(g_pj.root, lroot) : -1;
    const char* gate = fr.local_ok ? pvoice::LocalGate(fr.local) : "no local player";
    std::uint32_t posted_now = 0;
    for (std::uint32_t c : g_rx_posted) posted_now += c;
    if (gate && !g_was_gated) {
        std::snprintf(g_gate_reason, sizeof(g_gate_reason), "%s", gate);
        g_gate_since_ms = now, g_gate_held_at = g_rx_gated, g_gate_posted_at = posted_now;
        Log("[psound] gate closed: %s (the partner's sounds are not posted)", g_gate_reason);
        StopAllOurs(gate);
    } else if (!gate && g_was_gated) {
        Log("[psound] gate open again after %.0f ms (was: %s): %u partner item(s) held back, %u posted while it was closed", now - g_gate_since_ms,
            g_gate_reason, g_rx_gated - g_gate_held_at, posted_now - g_gate_posted_at);
    }
    g_was_gated = gate != nullptr;
    if (g_had_partner && !partner) StopAllOurs("no partner body");
    g_had_partner = partner;
    // due items, in arrival order. Stops and actions are never dropped for timing, the gate or a missing partner (a
    // dropped stop would leave a loop playing): they run when due, or at once when they cannot be scheduled.
    while (!g_in.empty()) {
        In& in = g_in.front();
        const bool control = in.it.op == static_cast<std::uint8_t>(wire::SoundOp::stop) || in.it.op == static_cast<std::uint8_t>(wire::SoundOp::action);
        if (!fr.have_play || !partner) {
            if (control) {
                const In cur = in;
                g_in.pop_front();
                Execute(cur, fr.play_ms, now);
                continue;
            }
            if (now - in.recv_ms < 3000) break;  // the partner's pose may still come
            ++g_rx_nobody;
            g_in.pop_front();
            continue;
        }
        if (in.t > fr.play_ms + 12000) {  // a clock jump (the ring re-based): never wait that long (role echo plays up to 10 s late)
            const In cur = in;
            g_in.pop_front();
            if (control) Execute(cur, fr.play_ms, now);
            else ++g_rx_early;
            continue;
        }
        if (in.t > fr.play_ms) break;
        const In cur = in;
        g_in.pop_front();
        if (control) {
            Execute(cur, fr.play_ms, now);
            continue;
        }
        if (gate) {
            ++g_rx_gated;
            continue;
        }
        if (fr.play_ms - cur.t > 1500) {
            ++g_rx_late;
            continue;
        }
        if (cur.it.op == static_cast<std::uint8_t>(wire::SoundOp::post) && g_partner_dist > kMaxDistanceM) {
            ++g_rx_far;
            continue;
        }
        Execute(cur, fr.play_ms, now);
    }
    // our objects with a sound (or a recent one) follow the partner's joints
    if (partner)
        for (unsigned i = 0; i < kPool; ++i) {
            PoolObj& po = g_pool[i];
            if (!po.registered || (po.active <= 0 && now - po.last_use_ms > 2000)) continue;
            double p[3];
            PlaceAt(i, p);
            float t[9];
            ToWwise(p, t);
            CallSetPosition(PoolId(i), t);
        }
    // Nothing we post plays longer than kMaxPlayMs (a loop whose stop was lost): stopped with a short fade.
    if (now >= g_next_cap_ms) {
        g_next_cap_ms = now + 10000;
        for (const auto& kv : g_local_pids)
            if (now - kv.second.t > kMaxPlayMs && now - kv.second.t < kMaxPlayMs + 10000) {
                CallStop(kv.first, 500);
                ++g_rx_capped;
                Log("[psound] a partner sound (our pid %u) played for %.0f min with no stop: stopped", kv.first, kMaxPlayMs / 60000.0);
            }
    }
    if (now >= g_next_status_ms) {
        g_next_status_ms = now + 30000;
        std::uint64_t h = g_tx_frames * 1315423911ull + g_rx_frames * 2654435761ull + g_tx_gated + g_rx_gated * 7ull + g_cb_calls.load() * 13ull +
                          g_tx_unowned * 17ull + g_tx_own * 31ull + g_rx_refused * 37ull;
        if (h != g_status_hash) {
            g_status_hash = h;
            Status("status");
        }
    }
}

bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!g_enabled) {
        std::snprintf(msg, cap, "partner sounds off (session.ini sound_sync=1 at start)");
        return false;
    }
    switch (op) {
    case 0:
        Status("counts");
        LogLabels();
        std::snprintf(msg, cap, "counters logged");
        return true;
    case 1:
        g_verbose = v[1] != 0;
        Log("[psound] verbose %s", g_verbose ? "on: every mirrored post is logged on both sides" : "off");
        std::snprintf(msg, cap, "verbose %d", g_verbose ? 1 : 0);
        return true;
    case 2:
        StopAllOurs("debug action");
        std::snprintf(msg, cap, "stopped");
        return true;
    default:
        std::snprintf(msg, cap, "unknown op %d (0 counts, 1 0|1 verbose, 2 stop all)", op);
        return false;
    }
}

}  // namespace st::psound

namespace st::game {
bool SoundSyncCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept { return psound::DebugCommand(op, v, msg, cap); }
}  // namespace st::game
