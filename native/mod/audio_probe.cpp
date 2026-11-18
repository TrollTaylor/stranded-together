#include "audio_probe.hpp"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "game_actions.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "partner_sound.hpp"
#include "self_voice.hpp"

namespace st::audioprobe {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// Evidence: docs/audio-research.md sections 2, 3 and 10. Every target is byte-checked before its hook.
// The Wwise API is found by export name first; the export must be the known RVA with the known bytes.
constexpr std::uintptr_t kPostEvent = 0x3A1A5C0, kRegister = 0x3A1CF50, kUnregister = 0x3A21640, kSetPosition = 0x3A1FA50,
                         kSetSwitch = 0x3A202F0, kSetRtpc = 0x3A1FC10, kStopPlaying = 0x3A20B20;
constexpr std::uintptr_t kEnqueue = 0x26A6570;        // voice manager enqueue (player + 0x4500)
constexpr std::uintptr_t kEnqueueCall = 0x26A6381;    // in 0x1426A6330 (the locked enqueue): call kEnqueue
constexpr std::uintptr_t kTalk = 0x22D4F70;           // DSTalkComponent MsgDSStartTalk handler body
constexpr std::uintptr_t kTalkThunk = 0x22D6AA0;      // jmp kTalk (the handler table's entry)
constexpr std::uintptr_t kTalkTableEntry = 0x4AE8568; // DSTalkComponent's handler table: MsgDSStartTalk -> kTalkThunk
constexpr std::uintptr_t kPlaySentence = 0x22D3FE0;   // DSTalkComponent::PlaySentence (vtable +0x68)
constexpr std::uintptr_t kTalkVtable = 0x3CF65E0;
constexpr std::uintptr_t kAnim = 0x2D83630;           // DSCustomSoundComponent::OnAnimationSoundEvent body
constexpr std::uintptr_t kAnimThunk = 0x2D98D60;      // jmp kAnim
// Test situations (op 2): what PlayPlayerVoice 0x1426E98A0 does, from its call at 0x1426E9918.
constexpr std::uintptr_t kPlayVoiceBody = 0x26E98A0, kPlayVoiceCall = 0x26E9918;
constexpr std::uintptr_t kToVoiceMgr = 0x27352E0;      // add rcx,0x4500; jmp 0x1426A6330
constexpr std::uintptr_t kVoiceCheck = 0x2717F30;      // add rcx,0x4500; jmp 0x1426A0B70
constexpr std::uintptr_t kPlayerGetter = 0x283EC30;    // the player PlayPlayerVoice talks to
// Sound instances (the Wwise game objects: GraphSoundInstance, WwiseSimpleSoundInstance; run 20261118-054042):
// created by SoundManager 0x141DB61A0 (81 callers) and (re)attached by 0x141DCBC30, both synchronously inside the
// component call that plays the sound. The component is taken from a thread-local set by the hooks below.
constexpr std::uintptr_t kCreateInst = 0x1DB61A0, kSetupInst = 0x1DCBC30;
constexpr std::uintptr_t kPlaySound = 0x235A460, kPlaySound2 = 0x235AAC0, kPlaySound3 = 0x23493E0, kCustomInit = 0x2D84940;
constexpr std::uintptr_t kExecAction = 0x3A17E30;
// The player's per-frame update (this = the DSPlayerEntity; it runs his animation, the animation sound events,
// the voice manager update 0x1426C2040; run 20261118-054042 stacks). A thread-local flag marks calls made inside it.
constexpr std::uintptr_t kPlayerUpdate = 0x2730B30;
// A material / physics sound emitter's start (0x141AB3CA0, called by 0x141AB1190): creates GraphSoundInstances and
// sets their RTPCs (0x141DC6490). Run 20261118-055223: the creator of the RTPC-rich unknown posts (0x04A0FA03 ...).
constexpr std::uintptr_t kEmitterStart = 0x1AB3CA0;  // AK::SoundEngine::ExecuteActionOnEvent (by id)
constexpr std::uintptr_t kEntParent = 0x70, kEntFirstChild = 0x78, kEntSibling = 0x80, kEntComponents = 0x90, kEntPos = 0xC8,
                         kCompOwner = 0x48;
constexpr std::uintptr_t kObjPos = 0x20;  // a Decima Wwise game object's world position (doubles; 0x141DC13F0)

// ---------------------------------------------------------------- the hot ring (Wwise calls)
enum Kind : std::uint8_t { kPost = 1, kReg, kUnreg, kSwitch, kRtpc, kPos, kStop, kCreate, kSetup, kAction };
constexpr int kKinds = 11;
struct Rec {
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint64_t> w0{0}, w1{0}, w2{0}, w3{0}, w4{0};
};
constexpr std::uint32_t kRingBits = 16, kRingCap = 1u << kRingBits, kRingMask = kRingCap - 1;
Rec* g_ring = nullptr;
std::atomic<std::uint32_t> g_head{0};
std::atomic<bool> g_on{false};
std::atomic<std::uint32_t> g_calls[kKinds];  // per kind, every call (also before the ring is drained)

inline void Push(Kind k, std::uint32_t id, std::uint32_t aux, std::uint32_t aux2, std::uint64_t obj, std::uint64_t extra) noexcept {
    g_calls[k].fetch_add(1, std::memory_order_relaxed);
    if (!g_on.load(std::memory_order_relaxed)) return;
    const std::uint32_t i = g_head.fetch_add(1, std::memory_order_relaxed);
    Rec& r = g_ring[i & kRingMask];
    r.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    r.w0.store(static_cast<std::uint64_t>(k) | (static_cast<std::uint64_t>(id) << 32), std::memory_order_relaxed);
    r.w1.store(static_cast<std::uint64_t>(aux) | (static_cast<std::uint64_t>(aux2) << 32), std::memory_order_relaxed);
    r.w2.store(obj, std::memory_order_relaxed);
    r.w3.store(extra, std::memory_order_relaxed);
    r.w4.store(GetCurrentThreadId(), std::memory_order_relaxed);
    r.seq.store(i + 1, std::memory_order_release);
}

// ---------------------------------------------------------------- the detail ring (voice path, rare)
enum DKind : std::uint8_t { dEnqueue = 1, dTalk, dSentence, dAnim, dVoicePost, dCreate };
constexpr unsigned kDArgs = 8, kDData = 18, kDStack = 14;
struct DRec {
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint64_t> w[2 + kDArgs + kDData + kDStack];
};
constexpr std::uint32_t kDBits = 10, kDCap = 1u << kDBits, kDMask = kDCap - 1;
DRec* g_dring = nullptr;
std::atomic<std::uint32_t> g_dhead{0};
struct DTmp {
    std::uint64_t kind_tid = 0, qpc = 0;
    std::uint64_t a[kDArgs] = {};
    std::uint64_t d[kDData] = {};
    std::uint64_t s[kDStack] = {};
};
void DPush(const DTmp& t) noexcept {
    if (!g_on.load(std::memory_order_relaxed)) return;
    const std::uint32_t i = g_dhead.fetch_add(1, std::memory_order_relaxed);
    DRec& r = g_dring[i & kDMask];
    r.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    r.w[0].store(t.kind_tid, std::memory_order_relaxed);
    r.w[1].store(t.qpc, std::memory_order_relaxed);
    for (unsigned k = 0; k < kDArgs; ++k) r.w[2 + k].store(t.a[k], std::memory_order_relaxed);
    for (unsigned k = 0; k < kDData; ++k) r.w[2 + kDArgs + k].store(t.d[k], std::memory_order_relaxed);
    for (unsigned k = 0; k < kDStack; ++k) r.w[2 + kDArgs + kDData + k].store(t.s[k], std::memory_order_relaxed);
    r.seq.store(i + 1, std::memory_order_release);
}

std::uintptr_t g_base = 0, g_text_lo = 0, g_text_hi = 0;
std::atomic<std::uintptr_t> g_local_e{0};
std::atomic<std::uintptr_t> g_emitter_e{0};  // the partner voice's talking entity (partner_voice.cpp), for the voice post lines
std::atomic<bool> g_saying{false};  // our own test request (op 2) is in the enqueue
thread_local std::uintptr_t t_comp = 0;
thread_local bool t_emit = false;  // t_comp is the material emitter (set by H_EmitterStart), for the partner sounds
thread_local std::uint32_t t_in_player = 0;  // 1 while this thread runs the local player's update  // the sound-playing component this thread is inside (0: none known)
std::atomic<std::uint32_t> g_create_stacks{0}, g_reg_stacks{0};  // creations with no known component that got a stack (at most 600)

// A word on our own stack that points just behind a call instruction in ds.exe .text.
bool LooksLikeReturn(std::uintptr_t v) noexcept {
    if (v < g_text_lo + 8 || v >= g_text_hi) return false;
    const auto* c = reinterpret_cast<const std::uint8_t*>(v);  // ds.exe .text: always mapped, read-only
    if (c[-5] == 0xE8) return true;                                                           // call rel32
    if (c[-6] == 0xFF && (c[-5] == 0x15 || (c[-5] & 0xF8) == 0x90)) return true;              // call [rip+d32] / [reg+d32]
    if (c[-3] == 0xFF && (c[-2] & 0xF8) == 0x50) return true;                                 // call [reg+d8]
    if (c[-4] == 0xFF && c[-3] == 0x54 && c[-2] == 0x24) return true;                         // call [rsp+d8]
    if (c[-2] == 0xFF && (c[-1] & 0xF8) == 0xD0) return true;                                 // call reg
    if (c[-7] == 0xFF && c[-6] == 0x94 && c[-5] == 0x24) return true;                         // call [rsp+d32]
    return c[-2] == 0xFF && (c[-1] & 0xF8) == 0x10;                                           // call [reg]
}

// Our own stack only (between the current frame and the thread's stack base: always committed).
__declspec(noinline) void CaptureStack(std::uint64_t (&out)[kDStack], const void* from) noexcept {
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto hi = reinterpret_cast<std::uintptr_t>(tib->StackBase);
    auto p = reinterpret_cast<std::uintptr_t>(from);
    const std::uintptr_t end = hi < p + 0x3000 ? hi : p + 0x3000;
    unsigned n = 0;
    for (; p + 8 <= end && n < kDStack; p += 8) {
        const std::uintptr_t v = *reinterpret_cast<const std::uintptr_t*>(p);
        if (LooksLikeReturn(v)) out[n++] = v;
    }
}

std::uint64_t Qpc() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<std::uint64_t>(t.QuadPart);
}

// ---------------------------------------------------------------- detours
using PostEventFn = std::uint32_t (*)(std::uint32_t, std::uint64_t, std::uint32_t, void*, void*, std::uint32_t, void*, std::uint32_t);
using RegisterFn = int (*)(std::uint64_t, const char*);
using UnregisterFn = int (*)(std::uint64_t);
using SetSwitchFn = int (*)(std::uint32_t, std::uint32_t, std::uint64_t);
using SetRtpcFn = int (*)(std::uint32_t, float, std::uint64_t, std::int32_t, int, bool);
using SetPositionFn = int (*)(std::uint64_t, const void*);
using StopPlayingFn = void (*)(std::uint32_t, std::int32_t, int);
using EnqueueFn = std::uint64_t (*)(void*, void*, std::uint64_t, std::uint64_t, std::uint64_t);
using TalkFn = void (*)(void*, void*);
using PlaySentenceFn = std::uint64_t (*)(void*, void*, std::uint64_t, void*);
using AnimFn = void (*)(void*, void*);
using CreateInstFn = void* (*)(void*, void*, void*, std::uint32_t);
using SetupInstFn = void (*)(void*, void*, void*, std::uint32_t);
using Play6Fn = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using Play7Fn = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using CustomInitFn = void (*)(void*);
using PlayerUpdateFn = void (*)(void*, void*);
using EmitterStartFn = void (*)(void*);
using ExecActionFn = int (*)(std::uint32_t, int, std::uint64_t, std::int32_t, int, std::uint32_t);

PostEventFn o_post = nullptr;
RegisterFn o_reg = nullptr;
UnregisterFn o_unreg = nullptr;
SetSwitchFn o_switch = nullptr;
SetRtpcFn o_rtpc = nullptr;
SetPositionFn o_pos = nullptr;
StopPlayingFn o_stop = nullptr;
EnqueueFn o_enqueue = nullptr;
TalkFn o_talk = nullptr;
PlaySentenceFn o_sentence = nullptr;
AnimFn o_anim = nullptr;
CreateInstFn o_create = nullptr;
SetupInstFn o_setup = nullptr;
Play6Fn o_play1 = nullptr, o_play2 = nullptr;
Play7Fn o_play3 = nullptr;
CustomInitFn o_custom_init = nullptr;
ExecActionFn o_exec = nullptr;
PlayerUpdateFn o_player_update = nullptr;
EmitterStartFn o_emitter = nullptr;

std::uint32_t H_PostEvent(std::uint32_t ev, std::uint64_t obj, std::uint32_t flags, void* cb, void* cookie, std::uint32_t n_ext, void* ext,
                          std::uint32_t pid) {
    selfvoice::PrePost(ev, obj, n_ext);  // Sam's own voice silenced on this game (before the post), self_voice.cpp
    const std::uint32_t r = o_post(ev, obj, flags, cb, cookie, n_ext, ext, pid);
    if (psound::Capturing()) psound::HotPost(ev, obj, n_ext, r);
    Push(kPost, ev, r, (flags & 0x7FFFFFFFu) | (n_ext ? 0x80000000u : 0u), obj, reinterpret_cast<std::uint64_t>(_ReturnAddress()));
    if (n_ext && g_on.load(std::memory_order_relaxed)) {
        DTmp t;
        t.kind_tid = dVoicePost | (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32);
        t.qpc = Qpc();
        t.a[0] = ev, t.a[1] = obj, t.a[2] = flags, t.a[3] = n_ext, t.a[4] = r, t.a[5] = reinterpret_cast<std::uint64_t>(ext);
        CaptureStack(t.s, _AddressOfReturnAddress());
        DPush(t);
    }
    return r;
}
int H_Register(std::uint64_t obj, const char* name) {
    const int r = o_reg(obj, name);
    const std::uintptr_t comp = t_comp;  // a component's play path registers the instance it starts
    if (psound::Capturing()) psound::HotRegister(obj, comp, comp && t_emit);
    Push(kReg, t_in_player, static_cast<std::uint32_t>(r), 0, obj, comp);
    if (!comp && g_on.load(std::memory_order_relaxed) && g_reg_stacks.fetch_add(1, std::memory_order_relaxed) < 800) {
        DTmp t;
        t.kind_tid = dCreate | (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32);
        t.a[0] = obj, t.a[2] = 1;
        CaptureStack(t.s, _AddressOfReturnAddress());
        DPush(t);
    }
    return r;
}
int H_Unregister(std::uint64_t obj) {
    // Before the original: another thread may register the same pooled id right after it (the ring keeps the order).
    if (psound::Capturing()) psound::HotUnregister(obj);
    selfvoice::NoteUnregister(obj);
    const int r = o_unreg(obj);
    Push(kUnreg, 0, static_cast<std::uint32_t>(r), 0, obj, reinterpret_cast<std::uint64_t>(_ReturnAddress()));
    return r;
}
int H_SetSwitch(std::uint32_t group, std::uint32_t state, std::uint64_t obj) {
    const int r = o_switch(group, state, obj);
    Push(kSwitch, group, state, static_cast<std::uint32_t>(r), obj, reinterpret_cast<std::uint64_t>(_ReturnAddress()));
    return r;
}
int H_SetRtpc(std::uint32_t id, float value, std::uint64_t obj, std::int32_t ms, int curve, bool bypass) {
    const int r = o_rtpc(id, value, obj, ms, curve, bypass);
    if (psound::Capturing()) psound::HotRtpc(id, value, obj, ms);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    Push(kRtpc, id, bits, static_cast<std::uint32_t>(ms), obj, reinterpret_cast<std::uint64_t>(_ReturnAddress()));
    return r;
}
int H_SetPosition(std::uint64_t obj, const void* tr) {
    const int r = o_pos(obj, tr);
    Push(kPos, t_in_player, static_cast<std::uint32_t>(r), 0, obj, 0);
    return r;
}
void H_StopPlaying(std::uint32_t pid, std::int32_t ms, int curve) {
    o_stop(pid, ms, curve);
    if (psound::Capturing()) psound::HotStop(pid, ms);
    Push(kStop, pid, static_cast<std::uint32_t>(ms), 0, 0, reinterpret_cast<std::uint64_t>(_ReturnAddress()));
}
int H_ExecAction(std::uint32_t ev, int action, std::uint64_t obj, std::int32_t ms, int curve, std::uint32_t pid) {
    const int r = o_exec(ev, action, obj, ms, curve, pid);
    if (psound::Capturing()) psound::HotAction(ev, action, obj, ms, pid);
    Push(kAction, ev, static_cast<std::uint32_t>(action), pid, obj, reinterpret_cast<std::uint64_t>(_ReturnAddress()));
    return r;
}

// Instance creation / attachment: who (component, return address) made the game object.
void* H_CreateInst(void* mgr, void* p2, void* res, std::uint32_t flags) {
    void* inst = o_create(mgr, p2, res, flags);
    if (inst) {
        const auto ret = reinterpret_cast<std::uint64_t>(_ReturnAddress());
        Push(kCreate, static_cast<std::uint32_t>(ret), static_cast<std::uint32_t>(ret >> 32), t_in_player, reinterpret_cast<std::uint64_t>(inst), t_comp);
        if (!t_comp && g_on.load(std::memory_order_relaxed) && g_create_stacks.fetch_add(1, std::memory_order_relaxed) < 600) {
            DTmp t;
            t.kind_tid = dCreate | (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32);
            t.a[0] = reinterpret_cast<std::uint64_t>(inst), t.a[1] = reinterpret_cast<std::uint64_t>(res);
            CaptureStack(t.s, _AddressOfReturnAddress());
            DPush(t);
        }
    }
    return inst;
}
void H_SetupInst(void* mgr, void* inst, void* p3, std::uint32_t flags) {
    o_setup(mgr, inst, p3, flags);
    if (inst) {
        const auto ret = reinterpret_cast<std::uint64_t>(_ReturnAddress());
        Push(kSetup, static_cast<std::uint32_t>(ret), static_cast<std::uint32_t>(ret >> 32), t_in_player, reinterpret_cast<std::uint64_t>(inst), t_comp);
    }
}
// The component context: SoundComponent's play paths (this = the component).
std::uint64_t H_Play1(void* c, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6) {
    const std::uintptr_t prev = t_comp;
    const bool pe = t_emit;
    t_comp = reinterpret_cast<std::uintptr_t>(c), t_emit = false;
    const std::uint64_t r = o_play1(c, a2, a3, a4, a5, a6);
    t_comp = prev, t_emit = pe;
    return r;
}
std::uint64_t H_Play2(void* c, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6) {
    const std::uintptr_t prev = t_comp;
    const bool pe = t_emit;
    t_comp = reinterpret_cast<std::uintptr_t>(c), t_emit = false;
    const std::uint64_t r = o_play2(c, a2, a3, a4, a5, a6);
    t_comp = prev, t_emit = pe;
    return r;
}
std::uint64_t H_Play3(void* c, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7) {
    const std::uintptr_t prev = t_comp;
    const bool pe = t_emit;
    t_comp = reinterpret_cast<std::uintptr_t>(c), t_emit = false;
    const std::uint64_t r = o_play3(c, a2, a3, a4, a5, a6, a7);
    t_comp = prev, t_emit = pe;
    return r;
}
void H_PlayerUpdate(void* e, void* p2) {
    const std::uint32_t prev = t_in_player;
    t_in_player = reinterpret_cast<std::uintptr_t>(e) == g_local_e.load(std::memory_order_relaxed) ? 1u : 0u;
    o_player_update(e, p2);
    t_in_player = prev;
}
// Keeps an outer component context (an animation sound event of the player starts the footstep emitter inside
// OnAnimationSoundEvent when it is synchronous); otherwise the emitter itself is the context.
void H_EmitterStart(void* em) {
    const std::uintptr_t prev = t_comp;
    const bool pe = t_emit;
    if (!prev) t_comp = reinterpret_cast<std::uintptr_t>(em), t_emit = true;
    o_emitter(em);
    t_comp = prev, t_emit = pe;
}
void H_CustomInit(void* c) {
    const std::uintptr_t prev = t_comp;
    const bool pe = t_emit;
    t_comp = reinterpret_cast<std::uintptr_t>(c), t_emit = false;
    o_custom_init(c);
    t_comp = prev, t_emit = pe;
}

std::uint64_t H_Enqueue(void* mgr, void* req, std::uint64_t flag, std::uint64_t a4, std::uint64_t a5) {
    DTmp t;
    const bool on = g_on.load(std::memory_order_relaxed);
    if (on) {
        t.kind_tid = dEnqueue | (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32);
        t.qpc = Qpc();
        t.a[0] = reinterpret_cast<std::uint64_t>(mgr), t.a[1] = reinterpret_cast<std::uint64_t>(req), t.a[2] = flag & 0xFF, t.a[3] = a4,
        t.a[4] = a5, t.a[6] = g_saying.load(std::memory_order_relaxed) ? 1 : 0;
        mem::Read(reinterpret_cast<std::uintptr_t>(req), t.d, 0x18);  // {u32 hash, i32 index, u8, f32, f32, ...}
        CaptureStack(t.s, _AddressOfReturnAddress());
    }
    const std::uint64_t r = o_enqueue(mgr, req, flag, a4, a5);
    if (on) {
        t.a[5] = r & 0xFF;
        DPush(t);
    }
    return r;
}

void H_Talk(void* comp, void* msg) {
    // self_voice (Phase 1) shares this hook: it decides first whether the talk is dropped (one hook per target).
    const bool drop = selfvoice::FilterTalk(comp, msg);
    if (g_on.load(std::memory_order_relaxed)) {
        DTmp t;
        t.kind_tid = dTalk | (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32);
        t.qpc = Qpc();
        const auto c = reinterpret_cast<std::uintptr_t>(comp), m = reinterpret_cast<std::uintptr_t>(msg);
        t.a[0] = c, t.a[1] = m, t.a[2] = Q(c + kCompOwner), t.a[3] = Q(m), t.a[4] = drop ? 1 : 0;  // component, message, owner, vtable, muted
        mem::Read(m, t.d, 0x48);                                            // message +0x00..+0x47
        std::int32_t n = 0;
        if (Rd(m + 0x30, n) && n > 0) mem::Read(Q(m + 0x38), t.d + 9, static_cast<std::size_t>((std::min)(n, 3)) * 0x18);  // entries
        CaptureStack(t.s, _AddressOfReturnAddress());
        DPush(t);
    }
    if (drop) return;
    const std::uintptr_t prev = t_comp;
    t_comp = reinterpret_cast<std::uintptr_t>(comp);
    o_talk(comp, msg);
    t_comp = prev;
}

std::uint64_t H_PlaySentence(void* comp, void* sentence, std::uint64_t param, void* opt) {
    const std::uintptr_t prev = t_comp;
    t_comp = reinterpret_cast<std::uintptr_t>(comp);
    const std::uint64_t r = o_sentence(comp, sentence, param, opt);
    t_comp = prev;
    if (g_on.load(std::memory_order_relaxed)) {
        DTmp t;
        t.kind_tid = dSentence | (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32);
        t.qpc = Qpc();
        const auto c = reinterpret_cast<std::uintptr_t>(comp);
        t.a[0] = c, t.a[1] = reinterpret_cast<std::uint64_t>(sentence), t.a[2] = Q(c + kCompOwner), t.a[3] = param & 0xFFFFFFFFu,
        t.a[4] = reinterpret_cast<std::uint64_t>(opt), t.a[5] = r & 0xFF;
        CaptureStack(t.s, _AddressOfReturnAddress());
        DPush(t);
    }
    return r;
}

std::atomic<std::uint32_t> g_anim_other{0};
void H_Anim(void* comp, void* msg) {
    if (g_on.load(std::memory_order_relaxed)) {
        const auto c = reinterpret_cast<std::uintptr_t>(comp), m = reinterpret_cast<std::uintptr_t>(msg);
        const std::uintptr_t owner = Q(c + kCompOwner), local = g_local_e.load(std::memory_order_relaxed);
        if (owner && owner == local) {
            DTmp t;
            t.kind_tid = dAnim | (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32);
            t.qpc = Qpc();
            t.a[0] = c, t.a[1] = m, t.a[2] = owner;
            mem::Read(m, t.d, 0x30);  // +0x10 i32 index, +0x18 SoundResource*, +0x20 helper data, +0x28 u8 duration, +0x29 u8 started
            std::int32_t idx = 0;
            std::memcpy(&idx, reinterpret_cast<const std::uint8_t*>(t.d) + 0x10, 4);
            if (idx >= 2500 && idx < 2600) CaptureStack(t.s, _AddressOfReturnAddress());  // the voice ranges only
            else t.s[0] = reinterpret_cast<std::uint64_t>(_ReturnAddress());
            DPush(t);
        } else {
            g_anim_other.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const std::uintptr_t prev = t_comp;
    t_comp = reinterpret_cast<std::uintptr_t>(comp);
    o_anim(comp, msg);
    t_comp = prev;
}

// ---------------------------------------------------------------- install
struct HookDef {
    const char* name;
    const char* export_name;  // nullptr: an internal function (RVA only)
    std::uintptr_t rva;
    const char* sig;
    void* detour;
    void** original;
};
const HookDef kHooks[] = {
    {"PostEvent", "?PostEvent@SoundEngine@AK@@YAKK_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z", kPostEvent,
     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 4C 89 74 24 20 41 57 48 83", reinterpret_cast<void*>(&H_PostEvent),
     reinterpret_cast<void**>(&o_post)},
    {"RegisterGameObj", "?RegisterGameObj@SoundEngine@AK@@YA?AW4AKRESULT@@_K@Z", kRegister, "40 53 48 83 EC 20 48 8B D9 48 83 F9 E0 72 0B B8 1F 00 00 00",
     reinterpret_cast<void*>(&H_Register), reinterpret_cast<void**>(&o_reg)},
    {"UnregisterGameObj", "?UnregisterGameObj@SoundEngine@AK@@YA?AW4AKRESULT@@_K@Z", kUnregister,
     "40 53 48 83 EC 20 48 8B D9 48 83 F9 E0 72 0B B8 02 00 00 00", reinterpret_cast<void*>(&H_Unregister), reinterpret_cast<void**>(&o_unreg)},
    {"SetSwitch", "?SetSwitch@SoundEngine@AK@@YA?AW4AKRESULT@@KK_K@Z", kSetSwitch, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B F8 8B F2 8B D9 E8",
     reinterpret_cast<void*>(&H_SetSwitch), reinterpret_cast<void**>(&o_switch)},
    {"SetRTPCValue", "?SetRTPCValue@SoundEngine@AK@@YA?AW4AKRESULT@@KM_KJW4AkCurveInterpolation@@_N@Z", kSetRtpc,
     "48 83 EC 48 0F B6 44 24 78 88 44 24 30 8B 44 24 70 89 44 24 28 44 89 4C", reinterpret_cast<void*>(&H_SetRtpc),
     reinterpret_cast<void**>(&o_rtpc)},
    {"SetPosition", "?SetPosition@SoundEngine@AK@@YA?AW4AKRESULT@@_KAEBVAkTransform@@@Z", kSetPosition, "48 83 F9 E0 72 06 B8 02 00 00 00 C3 E9",
     reinterpret_cast<void*>(&H_SetPosition), reinterpret_cast<void**>(&o_pos)},
    {"StopPlayingID", "?StopPlayingID@SoundEngine@AK@@YAXKJW4AkCurveInterpolation@@@Z", kStopPlaying,
     "85 C9 74 5C 48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 8B F8 8B F2", reinterpret_cast<void*>(&H_StopPlaying),
     reinterpret_cast<void**>(&o_stop)},
    {"voice enqueue", nullptr, kEnqueue, "40 55 56 57 41 54 41 56 41 57 48 81 EC 98 00 00 00 48 8B 05", reinterpret_cast<void*>(&H_Enqueue),
     reinterpret_cast<void**>(&o_enqueue)},
    {"MsgDSStartTalk handler", nullptr, kTalk, "40 55 53 41 55 48 8D 6C 24 B9 48 81 EC F0 00 00 00 48 8B 05", reinterpret_cast<void*>(&H_Talk),
     reinterpret_cast<void**>(&o_talk)},
    {"PlaySentence", nullptr, kPlaySentence, "48 89 5C 24 18 55 56 57 48 81 EC B0 00 00 00 48 8B 05", reinterpret_cast<void*>(&H_PlaySentence),
     reinterpret_cast<void**>(&o_sentence)},
    {"OnAnimationSoundEvent", nullptr, kAnim, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC",
     reinterpret_cast<void*>(&H_Anim), reinterpret_cast<void**>(&o_anim)},
    {"ExecuteActionOnEvent", "?ExecuteActionOnEvent@SoundEngine@AK@@YA?AW4AKRESULT@@KW4AkActionOnEventType@12@_KJW4AkCurveInterpolation@@K@Z", kExecAction,
     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20", reinterpret_cast<void*>(&H_ExecAction),
     reinterpret_cast<void**>(&o_exec)},
    {"sound instance create", nullptr, kCreateInst, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 80 79 20 00",
     reinterpret_cast<void*>(&H_CreateInst), reinterpret_cast<void**>(&o_create)},
    {"sound instance setup", nullptr, kSetupInst, "48 85 D2 0F 84 37 02 00 00 48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30",
     reinterpret_cast<void*>(&H_SetupInst), reinterpret_cast<void**>(&o_setup)},
    {"SoundComponent play", nullptr, kPlaySound, "40 55 56 41 54 41 56 41 57 48 81 EC F0 00 00 00 48 8B 05", reinterpret_cast<void*>(&H_Play1),
     reinterpret_cast<void**>(&o_play1)},
    {"SoundComponent play 2", nullptr, kPlaySound2, "40 53 41 55 41 56 41 57 48 81 EC C8 00 00 00 48 8B 05", reinterpret_cast<void*>(&H_Play2),
     reinterpret_cast<void**>(&o_play2)},
    {"SoundComponent play 3", nullptr, kPlaySound3, "40 53 55 41 54 41 55 41 57 48 81 EC A0 00 00 00 48 8B 05", reinterpret_cast<void*>(&H_Play3),
     reinterpret_cast<void**>(&o_play3)},
    {"player update", nullptr, kPlayerUpdate, "48 8B C4 48 89 58 18 48 89 70 20 57 41 56 41 57 48 81 EC C0 01 00 00",
     reinterpret_cast<void*>(&H_PlayerUpdate), reinterpret_cast<void**>(&o_player_update)},
    {"sound emitter start", nullptr, kEmitterStart, "4C 8B DC 53 48 83 EC 60 48 8B 05", reinterpret_cast<void*>(&H_EmitterStart),
     reinterpret_cast<void**>(&o_emitter)},
    {"custom sound init", nullptr, kCustomInit, "48 89 5C 24 18 57 48 83 EC 20 48 8B F9 E8", reinterpret_cast<void*>(&H_CustomInit),
     reinterpret_cast<void**>(&o_custom_init)},
};
constexpr std::size_t kHookCount = sizeof(kHooks) / sizeof(kHooks[0]);
bool g_hooked[kHookCount] = {};
bool g_installed = false;

bool CallsTo(std::uintptr_t site, std::uint8_t op, std::uintptr_t target) {
    std::uint8_t b = 0;
    return Rd(site, b) && b == op && mem::RipTarget(site, 1, 5) == target;
}

// ---------------------------------------------------------------- game-thread state
struct EvStat {
    std::uint32_t posts = 0, player = 0, other = 0, global = 0, unknown = 0, ext = 0, failed = 0;
    std::uint64_t first_obj = 0;
    std::uint64_t callers[3] = {};
    std::map<std::uint32_t, std::uint32_t> sw_before, rtpc_before;  // switch groups / RTPC ids set on the object since its last post
    std::map<std::string, std::uint32_t> owners;                    // owner description -> posts
    std::uint32_t threads_seen = 0;
    DWORD tid0 = 0;
};
enum class Own : std::uint8_t { unknown, player, other, global, none };
struct ObjStat {
    std::uint32_t posts = 0, regs = 0, unregs = 0, switches = 0, rtpcs = 0, pos = 0;
    double first_ms = 0;
    bool live = false, analyzed = false, rescanned = false;
    Own own = Own::unknown;
    char owner[96] = "";
    const char* type = nullptr;
    float dist = -1;
    std::map<std::uint32_t, std::uint32_t> events;
    std::vector<std::uint32_t> pend_sw, pend_rtpc;
    std::map<std::uint32_t, std::uint32_t> last_switch;  // group -> state
    std::uintptr_t creator_comp = 0;  // the component that created / last attached it (0: none known)
    std::uint64_t creator_ret = 0;    // the return address of that create / attach call
    std::uint32_t creations = 0;
    std::uint32_t in_player_reg = 0, in_player_pos = 0, in_player_make = 0;  // calls made inside the local player's update
};
// What created instances (by the create / attach call's return address).
struct CreatorStat {
    std::uint32_t n = 0, with_comp = 0, player = 0;
    std::string sample_comp;
};
std::map<std::uint64_t, CreatorStat>* g_creators = nullptr;
std::map<std::string, std::uint32_t>* g_create_sigs = nullptr;  // stacks of creations with no known component
std::unordered_map<std::uintptr_t, std::string>* g_comp_labels = nullptr;  // component -> "Type of Owner" (cached)
std::uint32_t g_actions = 0, g_reg_with_comp = 0, g_near_logged = 0;
std::unordered_map<std::uint64_t, ObjStat>* g_objs = nullptr;
std::unordered_map<std::uint32_t, EvStat>* g_evs = nullptr;
std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint64_t>>* g_pids = nullptr;  // playing id -> (event, object)
std::unordered_map<DWORD, std::array<std::uint32_t, kKinds>>* g_threads = nullptr;
std::map<std::int32_t, std::uint32_t>* g_anim_idx = nullptr;
std::map<std::string, std::uint32_t>* g_talk_owners = nullptr;
std::vector<std::uint64_t>* g_analyze = nullptr;
std::uint32_t g_tail = 0, g_dtail = 0, g_dropped = 0, g_ddropped = 0, g_stops_known = 0, g_stops_unknown = 0;
std::uint32_t g_n_enqueue = 0, g_n_talk = 0, g_n_sentence = 0, g_n_anim = 0, g_n_voicepost = 0, g_n_say = 0;
double g_next_summary_ms = 0, g_next_family_ms = 0, g_next_reverse_ms = 0;
std::uint32_t g_last_sum[kKinds] = {};
DWORD g_game_tid = 0;

// The local player's family: him, his descendant entities (depth 3, gear and weapons), and their components.
struct Family {
    std::uintptr_t e = 0;
    double pos[3] = {};
    std::unordered_set<std::uintptr_t> ents, comps;
    std::unordered_map<std::uintptr_t, std::uintptr_t> comp_owner;
    std::vector<std::uintptr_t> sound_comps;  // the components the reverse scan reads (type names with Sound/Voice/Talk/Echo)
} g_fam;
std::unordered_map<std::uintptr_t, bool>* g_soundish = nullptr;  // component -> its type is sound-like (cached)
bool g_logged_comps = false;

const char* TypeOf(std::uintptr_t p) {
    const char* n = mem::RttiName(mem::RttiOf(p));
    return n ? n : "?";
}

void RefreshFamily(std::uintptr_t e) {
    g_fam.ents.clear(), g_fam.comps.clear(), g_fam.comp_owner.clear(), g_fam.sound_comps.clear();
    g_fam.e = e;
    if (!e) return;
    Rd(e + kEntPos, g_fam.pos);
    struct Node {
        std::uintptr_t e;
        int depth;
    };
    Node stack[256];
    int sp = 0;
    stack[sp++] = {e, 0};
    unsigned visited = 0;
    while (sp > 0 && visited < 320) {
        const Node nd = stack[--sp];
        ++visited;
        g_fam.ents.insert(nd.e);
        std::int32_t n = 0;
        if (Rd(nd.e + kEntComponents, n) && n > 0 && n <= 512) {
            std::uintptr_t list[512];
            if (mem::Read(Q(nd.e + kEntComponents + 8), list, static_cast<std::size_t>(n) * 8))
                for (std::int32_t i = 0; i < n; ++i)
                    if (list[i]) {
                        g_fam.comps.insert(list[i]), g_fam.comp_owner[list[i]] = nd.e;
                        auto it = g_soundish->find(list[i]);
                        if (it == g_soundish->end()) {
                            const char* tn = mem::RttiName(mem::RttiOf(list[i]));
                            const bool snd = tn && (std::strstr(tn, "Sound") || std::strstr(tn, "Voice") || std::strstr(tn, "Talk") ||
                                                    std::strstr(tn, "Echo"));
                            if (g_soundish->size() > 20000) g_soundish->clear();
                            it = g_soundish->emplace(list[i], snd).first;
                        }
                        if (it->second) g_fam.sound_comps.push_back(list[i]);
                    }
        }
        if (nd.depth < 3)
            for (std::uintptr_t c = Q(nd.e + kEntFirstChild), guard = 0; c && sp < 256 && guard < 64; c = Q(c + kEntSibling), ++guard)
                stack[sp++] = {c, nd.depth + 1};
    }
}

bool InFamilyChain(std::uintptr_t e, int* depth_out) {
    for (int d = 0; e && d < 6; ++d, e = Q(e + kEntParent))
        if (e == g_fam.e) {
            if (depth_out) *depth_out = d;
            return true;
        }
    return false;
}

void SetOwner(ObjStat& o, Own own, const char* fmt, ...) {
    o.own = own;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(o.owner, sizeof(o.owner), fmt, ap);
    va_end(ap);
}

// The creating component's label and whether it belongs to the player family (exact).
const std::string& CompLabel(std::uintptr_t comp, bool* player) {
    // The play paths run on the component's representation (SoundComponentRep, DSCustomSoundComponentRep ...):
    // rep +0x20 is the component, component +0x48 its entity (0x14235A460: [[this+0x20]+0x48]).
    const char* tn = TypeOf(comp);
    const std::size_t tl = std::strlen(tn);
    const bool rep_obj = tl > 3 && std::strcmp(tn + tl - 3, "Rep") == 0;
    const std::uintptr_t real = rep_obj ? Q(comp + 0x20) : comp;
    const std::uintptr_t owner = Q(real + kCompOwner);
    *player = g_fam.comps.count(real) != 0 || (owner && g_fam.ents.count(owner) != 0);
    auto it = g_comp_labels->find(comp);
    if (it != g_comp_labels->end()) return it->second;
    char b[128];
    std::snprintf(b, sizeof(b), "%s of %s", rep_obj ? TypeOf(real) : tn, !owner ? "-" : owner == g_fam.e ? "the player" : TypeOf(owner));
    if (g_comp_labels->size() > 20000) g_comp_labels->clear();
    return g_comp_labels->emplace(comp, b).first->second;
}

// Forward scan: the object's own words that point at the player family (directly, or a heap object
// whose +0x48 owner / +0x70 parent chain is in it). The first match wins (lowest offset).
void Analyze(std::uint64_t id, ObjStat& o) {
    o.analyzed = true;
    const auto a = static_cast<std::uintptr_t>(id);
    o.type = mem::RttiName(mem::RttiOf(a));
    double p[3] = {};
    if (o.type && Rd(a + kObjPos, p) && g_fam.e && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
        const double dx = p[0] - g_fam.pos[0], dy = p[1] - g_fam.pos[1], dz = p[2] - g_fam.pos[2];
        o.dist = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    if (!g_fam.e) return;
    std::uintptr_t w[0x58] = {};
    if (!mem::Read(a, w, sizeof(w)) && !mem::Read(a, w, 0x100)) return;
    for (unsigned i = 1; i < 0x58; ++i) {
        const std::uintptr_t q = w[i];
        if (!q || (q & 7) || q < 0x10000 || mem::InImage(q)) continue;
        if (g_fam.ents.count(q)) {
            int d = 0;
            InFamilyChain(q, &d);
            SetOwner(o, Own::player, "+0x%X = %s (%s)", i * 8, d == 0 ? "the player" : "player child", TypeOf(q));
            return;
        }
        if (g_fam.comps.count(q)) {
            const std::uintptr_t ow = g_fam.comp_owner[q];
            SetOwner(o, Own::player, "+0x%X = %s of %s", i * 8, TypeOf(q), ow == g_fam.e ? "the player" : TypeOf(ow));
            return;
        }
    }
    // one level deeper: a heap object with a vtable in ds.exe whose owner (+0x48) or parent chain is the family
    for (unsigned i = 1; i < 0x58; ++i) {
        const std::uintptr_t q = w[i];
        if (!q || (q & 7) || q < 0x10000 || mem::InImage(q) || !mem::InImage(Q(q))) continue;
        const std::uintptr_t ow = Q(q + kCompOwner);
        if (ow && g_fam.ents.count(ow)) {
            SetOwner(o, Own::player, "+0x%X -> %s, +0x48 = %s", i * 8, TypeOf(q), ow == g_fam.e ? "the player" : TypeOf(ow));
            return;
        }
    }
    // not the player's: remember what owns it, if the same pattern names an entity
    for (unsigned i = 1; i < 0x58; ++i) {
        const std::uintptr_t q = w[i];
        if (!q || (q & 7) || q < 0x10000 || mem::InImage(q) || !mem::InImage(Q(q))) continue;
        const char* tn = mem::RttiName(mem::RttiOf(q));
        if (tn && (std::strstr(tn, "Component") || std::strstr(tn, "Entity"))) {
            const std::uintptr_t ow = Q(q + kCompOwner);
            const char* on = ow ? mem::RttiName(mem::RttiOf(ow)) : nullptr;
            SetOwner(o, Own::other, "+0x%X = %s%s%s", i * 8, tn, on ? " of " : "", on ? on : "");
            return;
        }
    }
}

// Reverse scan (every 2 s): the family's components hold their playing sounds; a word (or a word of an
// object a word points at) that is a live game object names that object's owner exactly.
void ReverseScan() {
    if (!g_fam.e) return;
    unsigned found = 0;
    for (const std::uintptr_t c : g_fam.sound_comps) {
        std::uintptr_t w[0x40] = {};
        if (!mem::Read(c, w, sizeof(w))) continue;
        for (unsigned i = 1; i < 0x40; ++i) {
            const std::uintptr_t q = w[i];
            if (!q || (q & 7) || q < 0x10000 || mem::InImage(q)) continue;
            auto it = g_objs->find(q);
            if (it != g_objs->end() && it->second.own != Own::player) {
                SetOwner(it->second, Own::player, "%s of %s +0x%X", TypeOf(c), g_fam.comp_owner[c] == g_fam.e ? "the player" : TypeOf(g_fam.comp_owner[c]), i * 8);
                ++found;
                continue;
            }
            std::uintptr_t w2[0x40] = {};
            if (!mem::Read(q, w2, sizeof(w2))) continue;
            for (unsigned j = 0; j < 0x40; ++j) {
                const std::uintptr_t q2 = w2[j];
                if (!q2 || (q2 & 7)) continue;
                auto it2 = g_objs->find(q2);
                if (it2 != g_objs->end() && it2->second.own != Own::player) {
                    SetOwner(it2->second, Own::player, "%s of %s +0x%X -> +0x%X", TypeOf(c),
                             g_fam.comp_owner[c] == g_fam.e ? "the player" : TypeOf(g_fam.comp_owner[c]), i * 8, j * 8);
                    ++found;
                }
            }
        }
        // SoundComponent's playing table (0x14235A460): +0x30 slots of 0x18 bytes (cap +0x3C), slot +0x10 record, record +0x08 instance
        std::int32_t cap = 0;
        const std::uintptr_t arr = w[0x30 / 8];
        if (arr && Rd(c + 0x3C, cap) && cap > 0 && cap <= 1024) {
            for (std::int32_t s = 0; s < cap; ++s) {
                std::uint32_t key = 0;
                if (!Rd(arr + static_cast<std::uintptr_t>(s) * 0x18, key) || !key) continue;
                const std::uintptr_t rec = Q(arr + static_cast<std::uintptr_t>(s) * 0x18 + 0x10);
                const std::uintptr_t inst = rec ? Q(rec + 8) : 0;
                auto it = inst ? g_objs->find(inst) : g_objs->end();
                if (it != g_objs->end() && it->second.own != Own::player) {
                    SetOwner(it->second, Own::player, "%s of %s playing table", TypeOf(c),
                             g_fam.comp_owner[c] == g_fam.e ? "the player" : TypeOf(g_fam.comp_owner[c]));
                    ++found;
                }
            }
        }
    }
    if (found) Log("[audio] reverse scan: %u game object(s) newly tied to the player's components", found);
}

std::string Ds(std::uint64_t v) {
    char b[32];
    if (v >= g_base && v < g_base + 0x10000000ull) std::snprintf(b, sizeof(b), "ds+0x%llX", static_cast<unsigned long long>(v - g_base));
    else std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}
std::string StackText(const std::uint64_t* s, unsigned n) {
    std::string out;
    for (unsigned i = 0; i < n && s[i]; ++i) {
        if (!out.empty()) out += ' ';
        out += Ds(s[i]);
    }
    return out.empty() ? "-" : out;
}

// ---------------------------------------------------------------- situation names (docs/audio-research.md 3.1)
const char* const kSituations[] = {
    "ReceivedLike", "Jumping", "OutOfBreath", "Humming", "AlmostThere", "IdleBreathingWhenOverburdened", "Cold", "HoldBaggageNormal",
    "LandingFromJumpHigher", "SlipByRain", "IncreasinglyExhaustedBreathing", "MassagingShoulder", "JumpingDouble", "HoldBaggageFast",
    "HoldOnBig", "MovingBreathingWhenOverburdened", "BreathingWhenClimbingSteepSlope", "BreathingWhenWadingThroughWater", "Snore",
    "HummingToComfortBB", "StrongDamageBatting", "WeakDamageBatting", "StrongPunchKick", "UseRopeStart", "UseRopeEnd", "StartRidingCart",
    "CartImpact", "ShookOffHunter", "NearlyCaughtByHunters", "SteepSlope", "LookDown", "OnVehicle", "ShoelessWalk", "LookingAtBrokenShoes",
    "Drowning", "DrowningEnd", "Faint", "SleepColdPlace", "WeakDamageBattingOrz", "PrivateFistPumps", "PrivateLeanBack", "PrivateChoke",
    "PrivateLeanPoint", "PrivateRockOn", "PrivateNeckStretch", "PrivateWink", "PrivateLookAway", "PrivateMiddleFinger", "LeaveHiggs",
    "HiggsChoking", "HiggsChokingLoop", "PushBackHiggsAfterEarAttack"};
constexpr int kSituationCount = static_cast<int>(sizeof(kSituations) / sizeof(kSituations[0]));
const char* SituationName(std::uint32_t h) {
    for (const char* n : kSituations)
        if (SituationHash(n) == h) return n;
    return "?";
}

std::string Guid16(std::uintptr_t obj) {
    std::uint8_t g[16] = {};
    if (!obj || !mem::Read(obj + 8, g, 16)) return "-";
    char b[40];
    std::snprintf(b, sizeof(b), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", g[3], g[2], g[1], g[0], g[5], g[4],
                  g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return b;
}
const char* OwnerLabel(std::uintptr_t owner) {
    if (!owner) return "none";
    if (owner == g_fam.e) return "the local player";
    if (g_fam.ents.count(owner)) return "a player child";
    return "other";
}

// ---------------------------------------------------------------- drain
void OnRec(Kind k, std::uint32_t id, std::uint32_t aux, std::uint32_t aux2, std::uint64_t obj, std::uint64_t extra, DWORD tid, double now) {
    auto& th = (*g_threads)[tid];
    if (k < kKinds) ++th[k];
    if (k == kStop) {
        auto it = g_pids->find(id);
        if (it != g_pids->end()) ++g_stops_known, g_pids->erase(it);
        else ++g_stops_unknown;
        return;
    }
    const bool global = obj == 0 || obj >= 0xFFFFFFFFFFFFFFE0ull;
    ObjStat* o = nullptr;
    if (!global && g_objs->size() < 60000) {
        auto ins = g_objs->try_emplace(obj);
        o = &ins.first->second;
        if (ins.second) o->first_ms = now;
    }
    switch (k) {
    case kReg:
        if (o) {
            ++o->regs, o->live = true;
            if (id) ++o->in_player_reg;
            if (!o->type) o->type = mem::RttiName(mem::RttiOf(static_cast<std::uintptr_t>(obj)));
            if (extra) {
                ++g_reg_with_comp;
                bool pl = false;
                const std::string& label = CompLabel(static_cast<std::uintptr_t>(extra), &pl);
                o->creator_comp = static_cast<std::uintptr_t>(extra);
                SetOwner(*o, pl ? Own::player : Own::other, "%s", label.c_str());
                o->analyzed = true;
            }
        }
        break;
    case kUnreg:
        if (o) ++o->unregs, o->live = false;
        break;
    case kPos:
        if (o) {
            ++o->pos;
            if (id) ++o->in_player_pos;
        }
        break;
    case kSwitch:
        if (o) {
            ++o->switches;
            o->last_switch[id] = aux;
            if (o->pend_sw.size() < 16) o->pend_sw.push_back(id);
        }
        break;
    case kRtpc:
        if (o) {
            ++o->rtpcs;
            if (o->pend_rtpc.size() < 16) o->pend_rtpc.push_back(id);
        }
        break;
    case kCreate:
    case kSetup: {
        const std::uint64_t ret = static_cast<std::uint64_t>(id) | (static_cast<std::uint64_t>(aux) << 32);
        CreatorStat& cs = (*g_creators)[ret];
        ++cs.n;
        if (o) {
            ++o->creations;
            if (aux2) ++o->in_player_make;
            o->creator_comp = static_cast<std::uintptr_t>(extra), o->creator_ret = ret;
            if (!o->type) o->type = mem::RttiName(mem::RttiOf(static_cast<std::uintptr_t>(obj)));
        }
        if (extra) {
            ++cs.with_comp;
            bool pl = false;
            const std::string& label = CompLabel(static_cast<std::uintptr_t>(extra), &pl);
            if (pl) ++cs.player;
            if (cs.sample_comp.empty()) cs.sample_comp = label;
            if (o) {
                SetOwner(*o, pl ? Own::player : Own::other, "%s", label.c_str());
                o->analyzed = true;
            }
        } else if (o) {
            SetOwner(*o, Own::unknown, "made at %s", Ds(ret).c_str());
            o->analyzed = false;  // the forward scan may still find it
        }
        break;
    }
    case kAction:
        ++g_actions;
        break;
    case kPost: {
        if (o && !o->analyzed && g_analyze->size() < 4096) {
            o->analyzed = true;  // queued
            g_analyze->push_back(obj);
        }
        if (g_evs->size() >= 8192 && !g_evs->count(id)) break;
        EvStat& e = (*g_evs)[id];
        ++e.posts;
        if (!aux) ++e.failed;
        if (aux2 & 0x80000000u) ++e.ext;
        if (!e.first_obj) e.first_obj = obj;
        if (!e.tid0) e.tid0 = tid;
        else if (e.tid0 != tid) e.threads_seen = 1;
        for (auto& c : e.callers) {
            if (c == extra) break;
            if (!c) {
                c = extra;
                break;
            }
        }
        if (global) ++e.global;
        if (o) {
            if (o->own != Own::player && (o->in_player_reg || o->in_player_make || o->in_player_pos > 2)) {
                char was[sizeof(o->owner)];
                std::snprintf(was, sizeof(was), "%s", o->own == Own::other ? o->owner : "");
                SetOwner(*o, Own::player, "in the player's update (reg %u, made %u, pos %u)%s%s", o->in_player_reg, o->in_player_make, o->in_player_pos,
                         was[0] ? "; was " : "", was);
            }
            if (o->own != Own::player && g_fam.e && g_near_logged < 400) {
                double p[3] = {};
                if (Rd(static_cast<std::uintptr_t>(obj) + kObjPos, p) && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
                    const double dx = p[0] - g_fam.pos[0], dy = p[1] - g_fam.pos[1], dz = p[2] - g_fam.pos[2];
                    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (d < 3.0) {
                        ++g_near_logged;
                        Log("[audio] near post ev 0x%08X obj %s %s %s dist %.2f (rel %.2f %.2f %.2f)", id, Ds(obj).c_str(), o->type ? o->type : "?",
                            o->owner, d, dx, dy, dz);
                    }
                }
            }
            if (o->own == Own::player) ++e.player;
            else if (o->own == Own::other) ++e.other;
            else ++e.unknown;
            std::string key = o->own == Own::player  ? std::string("P ") + o->owner
                              : o->own == Own::other ? std::string("O ") + o->owner
                                                     : std::string("? ") + (o->type ? o->type : "?") + " " + o->owner;
            if (e.owners.size() < 6 || e.owners.count(key)) ++e.owners[key];
            ++o->posts;
            if (o->events.size() < 64) ++o->events[id];
            for (std::uint32_t g : o->pend_sw) ++e.sw_before[g];
            for (std::uint32_t r : o->pend_rtpc) ++e.rtpc_before[r];
            o->pend_sw.clear(), o->pend_rtpc.clear();
        }
        if (aux && g_pids->size() < 100000) (*g_pids)[aux] = {id, obj};
        break;
    }
    default:
        break;
    }
}

void OnDRec(const DTmp& t) {
    const auto k = static_cast<DKind>(t.kind_tid & 0xFF);
    const DWORD tid = static_cast<DWORD>(t.kind_tid >> 32);
    const char* thread = tid == g_game_tid ? "game" : "other";
    switch (k) {
    case dEnqueue: {
        ++g_n_enqueue;
        std::uint32_t hash = 0;
        std::int32_t index = 0;
        std::uint8_t b8 = 0;
        float f0 = 0, f1 = 0;
        const auto* d = reinterpret_cast<const std::uint8_t*>(t.d);
        std::memcpy(&hash, d, 4), std::memcpy(&index, d + 4, 4), b8 = d[8], std::memcpy(&f0, d + 0xC, 4), std::memcpy(&f1, d + 0x10, 4);
        const std::uintptr_t player = static_cast<std::uintptr_t>(t.a[0]) - 0x4500;
        Log("[audio] enqueue #%u %s hash 0x%08X (%s) index %d b8 %u f %.2f %.2f flag %llu a4 0x%llX a5 0x%llX -> %llu | mgr of %s | thread %lu (%s) | "
            "stack %s",
            g_n_enqueue, t.a[6] ? "(our test)" : "", hash, SituationName(hash), index, b8, static_cast<double>(f0), static_cast<double>(f1),
            static_cast<unsigned long long>(t.a[2]), static_cast<unsigned long long>(t.a[3]), static_cast<unsigned long long>(t.a[4]),
            static_cast<unsigned long long>(t.a[5]), OwnerLabel(player), tid, thread, StackText(t.s, kDStack).c_str());
        break;
    }
    case dTalk: {
        ++g_n_talk;
        const std::uintptr_t comp = t.a[0], owner = t.a[2];
        const auto* d = reinterpret_cast<const std::uint8_t*>(t.d);
        std::uint32_t m10 = 0, m28 = 0;
        std::int32_t n = 0;
        std::uint64_t m38 = 0;
        std::uint8_t m40 = 0;
        std::memcpy(&m10, d + 0x10, 4), std::memcpy(&m28, d + 0x28, 4), std::memcpy(&n, d + 0x30, 4), std::memcpy(&m38, d + 0x38, 8), m40 = d[0x40];
        char ent[3][200] = {};
        for (int i = 0; i < 3 && i < n; ++i) {
            const std::uint64_t* en = t.d + 9 + i * 3;
            float f[2] = {};
            std::memcpy(f, &en[2], 8);
            std::snprintf(ent[i], sizeof(ent[i]), "{%s %s %s %s f %.2f %.2f}", TypeOf(en[0]), Guid16(en[0]).c_str(), en[1] ? TypeOf(en[1]) : "-",
                          en[1] ? Guid16(en[1]).c_str() : "", static_cast<double>(f[0]), static_cast<double>(f[1]));
        }
        const char* ot = owner ? TypeOf(owner) : "-";
        char key[160];
        std::snprintf(key, sizeof(key), "%s %s on %s%s", TypeOf(comp), OwnerLabel(owner), ot, t.a[4] ? " (MUTED by self_voice)" : "");
        if (g_talk_owners->size() < 64) ++(*g_talk_owners)[key];
        Log("[audio] talk #%u%s comp %s (%s) owner %s (%s) | msg vt %s +0x10 0x%X +0x28 0x%X n %d +0x38 %s +0x40 %u | raw %016llX %016llX %016llX "
            "%016llX %016llX | %s %s %s | thread %lu (%s) | stack %s",
            g_n_talk, t.a[4] ? " MUTED" : "", Ds(comp).c_str(), TypeOf(comp), Ds(owner).c_str(), OwnerLabel(owner) == std::string("other") ? ot : OwnerLabel(owner),
            Ds(t.a[3]).c_str(), m10, m28, n, Ds(m38).c_str(), m40, static_cast<unsigned long long>(t.d[2]), static_cast<unsigned long long>(t.d[3]),
            static_cast<unsigned long long>(t.d[4]), static_cast<unsigned long long>(t.d[5]), static_cast<unsigned long long>(t.d[8]), ent[0],
            ent[1], ent[2], tid, thread, StackText(t.s, kDStack).c_str());
        break;
    }
    case dSentence:
        ++g_n_sentence;
        Log("[audio] PlaySentence #%u comp %s (%s) owner %s (%s) sentence %s %s param %llu opt %s -> %llu | thread %lu (%s) | stack %s", g_n_sentence,
            Ds(t.a[0]).c_str(), TypeOf(t.a[0]), Ds(t.a[2]).c_str(), OwnerLabel(t.a[2]), TypeOf(t.a[1]), Guid16(t.a[1]).c_str(),
            static_cast<unsigned long long>(t.a[3]), Ds(t.a[4]).c_str(), static_cast<unsigned long long>(t.a[5]), tid, thread,
            StackText(t.s, kDStack).c_str());
        break;
    case dAnim: {
        ++g_n_anim;
        const auto* d = reinterpret_cast<const std::uint8_t*>(t.d);
        std::int32_t idx = 0;
        std::uint64_t res = 0, helper = 0;
        std::memcpy(&idx, d + 0x10, 4), std::memcpy(&res, d + 0x18, 8), std::memcpy(&helper, d + 0x20, 8);
        if (g_anim_idx->size() < 1024) ++(*g_anim_idx)[idx];
        const bool voice = idx >= 2500 && idx < 2600;
        if (voice || (*g_anim_idx)[idx] == 1 || (idx >= 3100 && idx < 3200))
            Log("[audio] anim %s idx %d res %s %s dur %u started %u helper %s | thread %lu (%s) | %s %s", voice ? "VOICE" : "first", idx,
                res ? TypeOf(res) : "-", res ? Guid16(res).c_str() : "", d[0x28], d[0x29], Ds(helper).c_str(), tid, thread,
                voice ? "stack" : "ret", StackText(t.s, voice ? kDStack : 1).c_str());
        break;
    }
    case dCreate: {
        const std::string sig = std::string(t.a[2] ? "register: " : "create: ") + StackText(t.s, 9);
        if (g_create_sigs->size() < 300 || g_create_sigs->count(sig)) ++(*g_create_sigs)[sig];
        break;
    }
    case dVoicePost: {
        ++g_n_voicepost;
        // Where the voice instance is (the game object's world position, +0x20 doubles): its distance from the
        // local player and from the partner voice's talking entity (partner_voice.cpp), when there is one.
        char where[160] = "";
        double p[3] = {};
        const std::uintptr_t em = g_emitter_e.load(std::memory_order_relaxed);
        if (Rd(static_cast<std::uintptr_t>(t.a[1]) + kObjPos, p) && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
            const double dx = p[0] - g_fam.pos[0], dy = p[1] - g_fam.pos[1], dz = p[2] - g_fam.pos[2];
            int w = std::snprintf(where, sizeof(where), " at (%.2f, %.2f, %.2f), %.2f m from the local player", p[0], p[1], p[2],
                                  std::sqrt(dx * dx + dy * dy + dz * dz));
            double q[3] = {};
            if (em && w > 0 && Rd(em + kEntPos, q)) {
                const double ex = p[0] - q[0], ey = p[1] - q[1], ez = p[2] - q[2];
                std::snprintf(where + w, sizeof(where) - static_cast<std::size_t>(w), ", %.2f m from the partner voice entity %s", std::sqrt(ex * ex + ey * ey + ez * ez),
                              Ds(em).c_str());
            }
        }
        Log("[audio] voice post #%u event 0x%08X obj %s ext %llu -> pid %llu flags 0x%llX%s | thread %lu (%s) | stack %s", g_n_voicepost,
            static_cast<unsigned>(t.a[0]), Ds(t.a[1]).c_str(), static_cast<unsigned long long>(t.a[3]), static_cast<unsigned long long>(t.a[4]),
            static_cast<unsigned long long>(t.a[2]), where, tid, thread, StackText(t.s, kDStack).c_str());
        break;
    }
    }
}

void Drain(double now) {
    std::uint32_t head = g_head.load(std::memory_order_acquire);
    if (head - g_tail > kRingCap) {
        g_dropped += head - g_tail - kRingCap;
        g_tail = head - kRingCap;
    }
    while (g_tail != head) {
        Rec& r = g_ring[g_tail & kRingMask];
        const std::uint32_t s1 = r.seq.load(std::memory_order_acquire);
        if (s1 != g_tail + 1) {
            if (s1 == 0 || static_cast<std::int32_t>(s1 - (g_tail + 1)) < 0) break;  // being written: next tick
            ++g_dropped, ++g_tail;                                                    // overwritten
            continue;
        }
        const std::uint64_t w0 = r.w0.load(std::memory_order_relaxed), w1 = r.w1.load(std::memory_order_relaxed),
                            w2 = r.w2.load(std::memory_order_relaxed), w3 = r.w3.load(std::memory_order_relaxed),
                            w4 = r.w4.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (r.seq.load(std::memory_order_relaxed) != s1) {
            ++g_dropped, ++g_tail;
            continue;
        }
        ++g_tail;
        OnRec(static_cast<Kind>(w0 & 0xFF), static_cast<std::uint32_t>(w0 >> 32), static_cast<std::uint32_t>(w1),
              static_cast<std::uint32_t>(w1 >> 32), w2, w3, static_cast<DWORD>(w4), now);
    }
    std::uint32_t dhead = g_dhead.load(std::memory_order_acquire);
    if (dhead - g_dtail > kDCap) {
        g_ddropped += dhead - g_dtail - kDCap;
        g_dtail = dhead - kDCap;
    }
    unsigned logged = 0;
    while (g_dtail != dhead && logged < 64) {
        DRec& r = g_dring[g_dtail & kDMask];
        const std::uint32_t s1 = r.seq.load(std::memory_order_acquire);
        if (s1 != g_dtail + 1) {
            if (s1 == 0 || static_cast<std::int32_t>(s1 - (g_dtail + 1)) < 0) break;
            ++g_ddropped, ++g_dtail;
            continue;
        }
        DTmp t;
        t.kind_tid = r.w[0].load(std::memory_order_relaxed), t.qpc = r.w[1].load(std::memory_order_relaxed);
        for (unsigned k = 0; k < kDArgs; ++k) t.a[k] = r.w[2 + k].load(std::memory_order_relaxed);
        for (unsigned k = 0; k < kDData; ++k) t.d[k] = r.w[2 + kDArgs + k].load(std::memory_order_relaxed);
        for (unsigned k = 0; k < kDStack; ++k) t.s[k] = r.w[2 + kDArgs + kDData + k].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (r.seq.load(std::memory_order_relaxed) != s1) {
            ++g_ddropped, ++g_dtail;
            continue;
        }
        ++g_dtail, ++logged;
        OnDRec(t);
    }
}

void Summary(const char* why) {
    std::uint32_t c[kKinds];
    for (int k = 0; k < kKinds; ++k) c[k] = g_calls[k].load(std::memory_order_relaxed);
    std::uint32_t pl_objs = 0, other_objs = 0, unk_objs = 0, live = 0;
    for (const auto& kv : *g_objs) {
        if (kv.second.live) ++live;
        if (!kv.second.posts) continue;
        if (kv.second.own == Own::player) ++pl_objs;
        else if (kv.second.own == Own::other) ++other_objs;
        else ++unk_objs;
    }
    Log("[audio] %s: calls post %u reg %u unreg %u switch %u rtpc %u setpos %u stop %u action %u create %u attach %u (stops known %u unknown %u) | objects %zu (live %u; posting: "
        "player %u other %u unknown %u) events %zu | enqueue %u talk %u sentence %u anim(local) %u anim(other) %u voice-post %u | dropped %u/%u",
        why, c[kPost], c[kReg], c[kUnreg], c[kSwitch], c[kRtpc], c[kPos], c[kStop], c[kAction], c[kCreate], c[kSetup], g_stops_known, g_stops_unknown, g_objs->size(), live, pl_objs,
        other_objs, unk_objs, g_evs->size(), g_n_enqueue, g_n_talk, g_n_sentence, g_n_anim, g_anim_other.load(), g_n_voicepost, g_dropped,
        g_ddropped);
}

void Dump() {
    Summary("dump");
    // threads
    for (const auto& kv : *g_threads)
        Log("[audio] thread %lu%s: post %u reg %u unreg %u switch %u rtpc %u setpos %u stop %u action %u create %u attach %u", kv.first,
            kv.first == g_game_tid ? " (game)" : "", kv.second[kPost], kv.second[kReg], kv.second[kUnreg], kv.second[kSwitch], kv.second[kRtpc],
            kv.second[kPos], kv.second[kStop], kv.second[kAction], kv.second[kCreate], kv.second[kSetup]);
    // what created the instances
    for (const auto& kv : *g_creators)
        Log("[audio] creator %s: %u instance(s), %u with a component (%u the player's) e.g. %s", Ds(kv.first).c_str(), kv.second.n,
            kv.second.with_comp, kv.second.player, kv.second.sample_comp.empty() ? "-" : kv.second.sample_comp.c_str());
    Log("[audio] registrations inside a component's play path: %u", g_reg_with_comp);
    for (const auto& kv : *g_create_sigs) Log("[audio] no component known x%u: %s", kv.second, kv.first.c_str());
    std::vector<std::pair<std::uint32_t, const EvStat*>> evs;
    for (const auto& kv : *g_evs) evs.emplace_back(kv.first, &kv.second);
    std::sort(evs.begin(), evs.end(), [](const auto& a, const auto& b) { return a.second->posts > b.second->posts; });
    Log("[audio] events by posts (%zu): id posts player other unknown global ext failed | callers | switch groups before | rtpc before | owners", evs.size());
    unsigned n = 0;
    for (const auto& ev : evs) {
        const EvStat& e = *ev.second;
        if (n++ >= 400) break;
        std::string sw, rt, ow, cl;
        for (const auto& s : e.sw_before) {
            char b[32];
            std::snprintf(b, sizeof(b), "%s%08X:%u", sw.empty() ? "" : ",", s.first, s.second);
            if (sw.size() < 120) sw += b;
        }
        for (const auto& s : e.rtpc_before) {
            char b[32];
            std::snprintf(b, sizeof(b), "%s%08X:%u", rt.empty() ? "" : ",", s.first, s.second);
            if (rt.size() < 120) rt += b;
        }
        for (const auto& s : e.owners) {
            char b[160];
            std::snprintf(b, sizeof(b), "%s%s x%u", ow.empty() ? "" : "; ", s.first.c_str(), s.second);
            if (ow.size() < 400) ow += b;
        }
        for (std::uint64_t c : e.callers)
            if (c) cl += (cl.empty() ? "" : ",") + Ds(c);
        Log("[audio] ev 0x%08X %u %u %u %u %u %u %u | %s%s | sw %s | rtpc %s | %s", ev.first, e.posts, e.player, e.other, e.unknown, e.global, e.ext,
            e.failed, cl.c_str(), e.threads_seen ? " (several threads)" : "", sw.empty() ? "-" : sw.c_str(), rt.empty() ? "-" : rt.c_str(),
            ow.c_str());
    }
    // the player's objects
    unsigned np = 0;
    for (const auto& ko : *g_objs) {
        const ObjStat& o = ko.second;
        if (o.own != Own::player || np >= 300) continue;
        ++np;
        std::string evtxt, swtxt;
        for (const auto& ev : o.events) {
            char b[32];
            std::snprintf(b, sizeof(b), "%s%08X x%u", evtxt.empty() ? "" : ",", ev.first, ev.second);
            if (evtxt.size() < 300) evtxt += b;
        }
        for (const auto& s : o.last_switch) {
            char b[40];
            std::snprintf(b, sizeof(b), "%s%08X=%08X", swtxt.empty() ? "" : ",", s.first, s.second);
            if (swtxt.size() < 200) swtxt += b;
        }
        Log("[audio] player obj %s %s dist %.2f | %s | made at %s x%u | in player update reg %u made %u pos %u | posts %u regs %u unregs %u live %d sw %u rtpc %u pos %u | events %s | switches %s",
            Ds(ko.first).c_str(), o.type ? o.type : "?", static_cast<double>(o.dist), o.owner, Ds(o.creator_ret).c_str(), o.creations, o.in_player_reg, o.in_player_make, o.in_player_pos, o.posts, o.regs,
            o.unregs, o.live ? 1 : 0, o.switches, o.rtpcs, o.pos, evtxt.c_str(), swtxt.empty() ? "-" : swtxt.c_str());
    }
    // the other posting objects, grouped by owner text
    std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> others;  // owner -> (objects, posts)
    for (const auto& ko : *g_objs) {
        const ObjStat& o = ko.second;
        if (o.own == Own::player || !o.posts) continue;
        char key[160];
        std::snprintf(key, sizeof(key), "%s %s | %s", o.own == Own::other ? "other" : "unknown", o.type ? o.type : "?", o.owner[0] ? o.owner : "-");
        auto& p = others[key];
        ++p.first, p.second += o.posts;
    }
    for (const auto& kv : others) Log("[audio] non-player objects: %s: %u object(s), %u post(s)", kv.first.c_str(), kv.second.first, kv.second.second);
    std::string idx;
    for (const auto& kv : *g_anim_idx) {
        char b[24];
        std::snprintf(b, sizeof(b), "%s%d:%u", idx.empty() ? "" : " ", kv.first, kv.second);
        idx += b;
    }
    Log("[audio] local player's animation sound events by index: %s", idx.empty() ? "-" : idx.c_str());
    for (const auto& kv : *g_talk_owners) Log("[audio] talk requests: %s x%u", kv.first.c_str(), kv.second);
}

void LogComponents() {
    if (!g_fam.e) {
        Log("[audio] no local player");
        return;
    }
    std::int32_t n = 0;
    Rd(g_fam.e + kEntComponents, n);
    std::string line;
    unsigned shown = 0;
    if (n > 0 && n <= 512) {
        std::uintptr_t list[512];
        if (mem::Read(Q(g_fam.e + kEntComponents + 8), list, static_cast<std::size_t>(n) * 8))
            for (std::int32_t i = 0; i < n; ++i) {
                char b[96];
                std::snprintf(b, sizeof(b), "%s%s@%s", line.empty() ? "" : ", ", TypeOf(list[i]), Ds(list[i]).c_str());
                line += b;
                if (line.size() > 1500) {
                    Log("[audio] player %s components %u..: %s", TypeOf(g_fam.e), shown, line.c_str());
                    line.clear();
                    shown = static_cast<unsigned>(i + 1);
                }
            }
    }
    Log("[audio] player %s %s: %d components (%u..): %s; family %zu entities, %zu components", Ds(g_fam.e).c_str(), TypeOf(g_fam.e), n, shown,
        line.c_str(), g_fam.ents.size(), g_fam.comps.size());
    const char* want[] = {"DSTalkComponent", "TalkComponent", "VoiceComponent", "DSCustomSoundComponent", "SoundComponent", "NodeGraphComponent"};
    for (const char* w : want) {
        const std::uintptr_t c = partner::FindComponentByName(reinterpret_cast<game::Entity*>(g_fam.e), w);
        Log("[audio] player component deriving from %s: %s%s%s", w, c ? Ds(c).c_str() : "none", c ? " " : "", c ? TypeOf(c) : "");
    }
}

// ---------------------------------------------------------------- test situations (op 2)
bool g_say_checked = false, g_say_ok = false;
bool SayAvailable() {
    if (g_say_checked) return g_say_ok;
    g_say_checked = true;
    const std::uintptr_t b = g_base;
    g_say_ok = mem::CodeMatches(b + kToVoiceMgr, "48 81 C1 00 45 00 00 E9") && mem::RipTarget(b + kToVoiceMgr + 7, 1, 5) == b + 0x26A6330 &&
               mem::CodeMatches(b + 0x26A6330, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8D 99 C8 01 00 00") &&
               mem::CodeMatches(b + kVoiceCheck, "48 81 C1 00 45 00 00 E9") && mem::RipTarget(b + kVoiceCheck + 7, 1, 5) == b + 0x26A0B70 &&
               mem::CodeMatches(b + 0x26A0B70, "48 8B 49 28 E9") &&
               mem::CodeMatches(b + kPlayerGetter, "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 15 33 D2 E8") &&
               mem::CodeMatches(b + kPlayVoiceBody, "48 89 5C 24 10 57 48 83 EC 40 48 8B 05") && CallsTo(b + kPlayVoiceCall, 0xE8, b + kToVoiceMgr);
    if (!g_say_ok) Log("[audio] test situations unavailable: the PlayPlayerVoice path bytes differ");
    return g_say_ok;
}

struct VoiceRequest {  // PlayPlayerVoice's stack request (0x1426E98A0)
    std::uint32_t hash;
    std::int32_t index;
    std::uint8_t b8;
    std::uint8_t pad[3];
    float f0, f1;
};
static_assert(sizeof(VoiceRequest) == 0x14, "request layout");

std::uintptr_t CallPlayerGetter() {
    __try {
        return reinterpret_cast<std::uintptr_t (*)()>(g_base + kPlayerGetter)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// false: faulted. The game's own order: the check (0x142717F30), then the locked enqueue (0x1427352E0, flag 1).
bool CallSay(std::uintptr_t player, VoiceRequest* req, std::uint8_t* ok, std::uint8_t* queued) {
    __try {
        *ok = reinterpret_cast<std::uint8_t (*)(std::uintptr_t, VoiceRequest*)>(g_base + kVoiceCheck)(player, req);
        if (*ok) *queued = reinterpret_cast<std::uint8_t (*)(std::uintptr_t, VoiceRequest*, std::uint8_t)>(g_base + kToVoiceMgr)(player, req, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Say(int n, std::uintptr_t local, char* msg, std::size_t cap) {
    if (!g_base) g_base = mem::Base();  // also used without the probe (selfvoice's test op)
    if (n < 0 || n >= kSituationCount) {
        std::snprintf(msg, cap, "situation index %d out of 0..%d", n, kSituationCount - 1);
        return false;
    }
    if (!SayAvailable()) {
        std::snprintf(msg, cap, "unavailable (byte checks)");
        return false;
    }
    const std::uintptr_t player = CallPlayerGetter();
    if (!player || player != local) {
        std::snprintf(msg, cap, "the voice player %s is not the local player %s", Ds(player).c_str(), Ds(local).c_str());
        return false;
    }
    const char* tn = mem::RttiName(mem::RttiOf(player));
    if (!tn || std::strcmp(tn, "DSPlayerEntity") != 0) {
        std::snprintf(msg, cap, "the player is a %s, not a DSPlayerEntity", tn ? tn : "?");
        return false;
    }
    VoiceRequest req{SituationHash(kSituations[n]), -1, 0, {}, 0.0f, 0.0f};
    std::uint8_t ok = 0, queued = 0;
    g_saying.store(true);
    const bool fine = CallSay(player, &req, &ok, &queued);
    g_saying.store(false);
    if (!fine) {
        std::snprintf(msg, cap, "faulted");
        return false;
    }
    ++g_n_say;
    std::snprintf(msg, cap, "situation %d %s hash 0x%08X: check %u, queued %u", n, kSituations[n], req.hash, ok, queued);
    Log("[audio] test situation %s", msg);
    return true;
}

}  // namespace

std::uint32_t SituationHash(const char* name) noexcept {
    std::uint32_t c = 0;
    for (const char* p = name; p && *p; ++p) {
        c ^= static_cast<std::uint8_t>(*p);
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    }
    return c & 0x7FFFFFFFu;
}

bool Install() noexcept {
    if (g_installed) return true;
    g_base = mem::Base();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + static_cast<std::uintptr_t>(dos->e_lfanew));
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (std::memcmp(sec[i].Name, ".text", 6) == 0) g_text_lo = g_base + sec[i].VirtualAddress, g_text_hi = g_text_lo + sec[i].Misc.VirtualSize;
    if (!g_text_lo) {
        Log("[audio] probe not installed: no .text section");
        return false;
    }
    g_ring = new (std::nothrow) Rec[kRingCap];
    g_dring = new (std::nothrow) DRec[kDCap];
    g_objs = new (std::nothrow) std::unordered_map<std::uint64_t, ObjStat>();
    g_evs = new (std::nothrow) std::unordered_map<std::uint32_t, EvStat>();
    g_pids = new (std::nothrow) std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint64_t>>();
    g_threads = new (std::nothrow) std::unordered_map<DWORD, std::array<std::uint32_t, kKinds>>();
    g_anim_idx = new (std::nothrow) std::map<std::int32_t, std::uint32_t>();
    g_talk_owners = new (std::nothrow) std::map<std::string, std::uint32_t>();
    g_analyze = new (std::nothrow) std::vector<std::uint64_t>();
    g_soundish = new (std::nothrow) std::unordered_map<std::uintptr_t, bool>();
    g_creators = new (std::nothrow) std::map<std::uint64_t, CreatorStat>();
    g_create_sigs = new (std::nothrow) std::map<std::string, std::uint32_t>();
    g_comp_labels = new (std::nothrow) std::unordered_map<std::uintptr_t, std::string>();
    if (!g_ring || !g_dring || !g_objs || !g_evs || !g_pids || !g_threads || !g_anim_idx || !g_talk_owners || !g_analyze || !g_soundish || !g_creators || !g_create_sigs || !g_comp_labels) {
        Log("[audio] probe not installed: out of memory");
        return false;
    }
    // Call sites and tables that tie the internal targets to their meaning.
    const std::uintptr_t b = g_base;
    const bool internal_ok = CallsTo(b + kEnqueueCall, 0xE8, b + kEnqueue) && CallsTo(b + kTalkThunk, 0xE9, b + kTalk) &&
                             Q(b + kTalkTableEntry) == b + kTalkThunk && Q(b + kTalkVtable + 0x68) == b + kPlaySentence &&
                             CallsTo(b + kAnimThunk, 0xE9, b + kAnim) && CallsTo(b + 0x22D50BE, 0xE8, b + kCreateInst) &&
                             CallsTo(b + 0x235A943, 0xE8, b + kCreateInst) && CallsTo(b + 0x2D8497B, 0xE8, b + kCreateInst) &&
                             CallsTo(b + 0x235A7CD, 0xE8, b + kSetupInst);
    if (!internal_ok) Log("[audio] voice-path hooks skipped: a call site or table entry differs (enqueue / talk / PlaySentence / anim)");
    const HMODULE exe = GetModuleHandleW(nullptr);
    unsigned queued = 0;
    std::string ok, bad;
    for (std::size_t i = 0; i < kHookCount; ++i) {
        const HookDef& h = kHooks[i];
        const std::uintptr_t want = b + h.rva;
        const char* why = nullptr;
        if (h.export_name) {
            const auto got = reinterpret_cast<std::uintptr_t>(GetProcAddress(exe, h.export_name));
            if (!got) why = "export not found";
            else if (got != want) why = "export moved";
        } else if (!internal_ok) {
            why = "call sites differ";
        }
        if (!why && !mem::CodeMatches(want, h.sig)) why = "bytes differ";
        if (!why) {
            const MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(want), h.detour, h.original);
            const MH_STATUS q = s == MH_OK ? MH_QueueEnableHook(reinterpret_cast<void*>(want)) : s;
            if (q == MH_OK) {
                g_hooked[i] = true;
                ++queued;
            } else {
                if (s == MH_OK) MH_RemoveHook(reinterpret_cast<void*>(want));  // created but not queued: leave nothing behind
                why = MH_StatusToString(q);
            }
        }
        if (why) bad += std::string(bad.empty() ? "" : ", ") + h.name + " (" + why + ")";
        else ok += std::string(ok.empty() ? "" : ", ") + h.name;
    }
    const MH_STATUS applied = queued ? MH_ApplyQueued() : MH_OK;
    if (applied != MH_OK) {
        Log("[audio] probe not installed: MH_ApplyQueued %s", MH_StatusToString(applied));
        return false;
    }
    g_on.store(true);
    g_installed = true;
    Log("[audio] probe installed (logging only): %u/%zu hooks: %s%s%s", queued, kHookCount, ok.c_str(), bad.empty() ? "" : "; NOT: ", bad.c_str());
    return queued > 0;
}

bool Installed() noexcept { return g_installed; }
bool TalkHooked() noexcept {
    for (std::size_t i = 0; i < kHookCount; ++i)
        if (kHooks[i].rva == kTalk && !kHooks[i].export_name) return g_installed && g_hooked[i];
    return false;
}
bool Hooked(std::uintptr_t rva) noexcept {
    for (std::size_t i = 0; i < kHookCount; ++i)
        if (kHooks[i].rva == rva) return g_installed && g_hooked[i];
    return false;
}
bool SentenceHooked() noexcept {

    for (std::size_t i = 0; i < kHookCount; ++i)
        if (kHooks[i].rva == kPlaySentence && !kHooks[i].export_name) return g_installed && g_hooked[i];
    return false;
}
bool TestSituation(game::Entity* local, int n, char* msg, std::size_t cap) noexcept { return Say(n, reinterpret_cast<std::uintptr_t>(local), msg, cap); }
void NoteVoiceEmitter(std::uintptr_t entity) noexcept { g_emitter_e.store(entity, std::memory_order_relaxed); }

void Tick(game::Entity* local, double now_ms) noexcept {
    if (!g_installed) return;
    if (!g_game_tid) g_game_tid = GetCurrentThreadId();
    const auto e = reinterpret_cast<std::uintptr_t>(local);
    g_local_e.store(e, std::memory_order_relaxed);
    if (e != g_fam.e || now_ms >= g_next_family_ms) {
        const bool changed = e != g_fam.e;
        RefreshFamily(e);
        g_next_family_ms = now_ms + 2000;
        if (changed) {
            Log("[audio] local player %s (family %zu entities, %zu components)", Ds(e).c_str(), g_fam.ents.size(), g_fam.comps.size());
            if (e && !g_logged_comps) {
                g_logged_comps = true;
                LogComponents();
            }
        }
    } else if (e) {
        Rd(e + kEntPos, g_fam.pos);
    }
    Drain(now_ms);
    for (unsigned i = 0; i < 6 && !g_analyze->empty(); ++i) {
        const std::uint64_t id = g_analyze->back();
        g_analyze->pop_back();
        auto it = g_objs->find(id);
        if (it != g_objs->end()) Analyze(id, it->second);
    }
    if (now_ms >= g_next_reverse_ms) {
        g_next_reverse_ms = now_ms + 2000;
        ReverseScan();
    }
    if (now_ms >= g_next_summary_ms) {
        g_next_summary_ms = now_ms + 10000;
        std::uint32_t c[kKinds];
        bool moved = false;
        for (int k = 0; k < kKinds; ++k) c[k] = g_calls[k].load(std::memory_order_relaxed), moved |= c[k] != g_last_sum[k];
        if (moved) Summary("10 s");
        std::memcpy(g_last_sum, c, sizeof(c));
    }
}

bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    if (!g_installed) {
        std::snprintf(msg, cap, "audio probe off (session.ini audio_probe=1 at start)");
        return false;
    }
    switch (op) {
    case 0:
        Summary("summary");
        std::snprintf(msg, cap, "summary logged");
        return true;
    case 1:
        Dump();
        std::snprintf(msg, cap, "dump logged: %zu events, %zu objects", g_evs->size(), g_objs->size());
        return true;
    case 2:
        return Say(v[1] >= 0 && v[1] < 64 ? static_cast<int>(v[1]) : -1, g_fam.e, msg, cap);
    case 3:
        LogComponents();
        std::snprintf(msg, cap, "components logged");
        return true;
    case 4:
        g_objs->clear(), g_evs->clear(), g_pids->clear(), g_threads->clear(), g_anim_idx->clear(), g_talk_owners->clear(), g_analyze->clear();
        g_creators->clear(), g_create_sigs->clear();
        g_stops_known = g_stops_unknown = 0;
        std::snprintf(msg, cap, "counters reset");
        return true;
    default:
        std::snprintf(msg, cap, "unknown op %d (0 summary, 1 dump, 2 n say, 3 components, 4 reset)", op);
        return false;
    }
}

}  // namespace st::audioprobe

namespace st::game {
bool AudioProbeCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept { return audioprobe::DebugCommand(op, v, msg, cap); }
}  // namespace st::game
