#include "self_voice.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "audio_probe.hpp"
#include "body_choice.hpp"
#include "game_actions.hpp"
#include "log.hpp"
#include "menu_figure.hpp"
#include "partner_source.hpp"
#include "partner_voice.hpp"
#include "real_body.hpp"

namespace st::selfvoice {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// Evidence: docs/audio-research.md sections 3.2 C, 10, 12.1-12.2 (decompiles; Phase 0 runs 20261118-054042 ...).
constexpr std::uintptr_t kTalk = 0x22D4F70;            // DSTalkComponent MsgDSStartTalk handler body
constexpr std::uintptr_t kTalkThunk = 0x22D6AA0;       // jmp kTalk (its only reference)
constexpr std::uintptr_t kTalkTableEntry = 0x4AE8568;  // DSTalkComponent's handler table: MsgDSStartTalk -> kTalkThunk
constexpr const char* kTalkSig = "40 55 53 41 55 48 8D 6C 24 B9 48 81 EC F0 00 00 00 48 8B 05";
constexpr std::uintptr_t kCompOwner = 0x48;            // Component -> owner entity
constexpr std::uintptr_t kMsgStop = 0x40;              // MsgDSStartTalk: u8, 0 = start
// The cargo menu figure's voice (DSPlayerEntity_sExportedPlayAkukanVoice): 0x142757D40(int list) reads the
// subspace work object [0x147BE79B0], takes its figure ([w+0x30]-0x20, 0x14269C560) and calls 0x14273BB10(figure,
// list), which picks a random sentence of list `list` from the figure's resource and sends MsgDSStartTalk
// (vtable 0x143CF3A50, +0x10 = 1.5, entry delay 0, 0.1) to the figure itself.
constexpr std::uintptr_t kAkukanVoice = 0x2757D40;
constexpr const char* kAkukanVoiceSig = "40 53 48 83 EC 20 8B D9 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 74 0F";
constexpr std::uintptr_t kAkukanVoiceCall = 0x2757D63;  // jmp kAkukanSay (tail call)
constexpr std::uintptr_t kAkukanSay = 0x273BB10;
constexpr const char* kAkukanSaySig = "4C 8B DC 55 53 49 8D 6B A1 48 81 EC B8 00 00 00 48 8B 05";
constexpr std::uintptr_t kWork = 0x7BE79B0, kFigureOf = 0x269C560;

using TalkFn = void (*)(void*, void*);
TalkFn o_talk = nullptr;

std::atomic<bool> g_mute{false};   // the setting in force (debug op 1 changes it while armed)
std::atomic<bool> g_armed{false};  // a talk hook asks FilterTalk (set on the bootstrap thread, read by job threads)
std::atomic<bool> g_capture{false};  // partner voice (voice_sync=1): the local player's talks are copied for STS1
bool g_resolved_mute = false;      // the setting resolved to mute at start
bool g_own_hook = false;           // ours (not the probe's)
Mode g_mode = Mode::sam;
const char* g_character = "sam";
std::atomic<std::uintptr_t> g_local{0}, g_figure{0};
std::atomic<std::uint32_t> g_drop_local{0}, g_drop_menu{0}, g_pass_local{0}, g_pass_menu{0}, g_pass_other_branch{0}, g_seen{0};

// ---------------------------------------------------------------- the talk capture (partner voice)
// Multi-producer (job threads), one consumer (the game thread): a seqlock per slot, as the audio probe's
// rings. A slot is 15 words: w0 = n | muted << 8 | truncated << 9, w1 = QPC, w2..w5 the SentenceResource
// pointers, w6..w13 the four 16-byte GUIDs, w14 the first entry's delay (f32 bits).
constexpr std::uint32_t kCapBits = 6, kCapCap = 1u << kCapBits, kCapMask = kCapCap - 1;
constexpr unsigned kCapWords = 15;
struct CapRec {
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint64_t> w[kCapWords];
};
CapRec g_cap[kCapCap];
std::atomic<std::uint32_t> g_cap_head{0};
std::uint32_t g_cap_tail = 0;  // game thread
std::atomic<std::uint32_t> g_cap_unreadable{0};
std::uint32_t g_cap_dropped = 0;  // game thread: overwritten before they were drained

constexpr std::uintptr_t kMsgCount = 0x30, kMsgEntries = 0x38, kEntrySize = 0x18, kEntryDelay = 0x10;

void Capture(std::uintptr_t msg, bool muted) noexcept {
    std::int32_t n = 0;
    std::uintptr_t arr = 0;
    if (!Rd(msg + kMsgCount, n) || n <= 0 || n > 64 || !Rd(msg + kMsgEntries, arr) || !arr) {
        g_cap_unreadable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint64_t w[kCapWords] = {};
    const unsigned take = n > static_cast<std::int32_t>(kCaptureMax) ? kCaptureMax : static_cast<unsigned>(n);
    unsigned got = 0;
    for (unsigned i = 0; i < take; ++i) {
        std::uintptr_t sentence = 0;
        std::uint8_t guid[16] = {};
        if (!Rd(arr + i * kEntrySize, sentence) || !sentence || !mem::Read(sentence + 8, guid, 16)) continue;
        w[2 + got] = sentence;
        std::memcpy(&w[6 + got * 2], guid, 16);
        ++got;
    }
    if (!got) {
        g_cap_unreadable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float delay = 0;
    Rd(arr + kEntryDelay, delay);
    std::uint32_t dbits = 0;
    std::memcpy(&dbits, &delay, 4);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    w[0] = got | (muted ? 0x100u : 0u) | (static_cast<unsigned>(n) > got ? 0x200u : 0u);
    w[1] = static_cast<std::uint64_t>(t.QuadPart);
    w[14] = dbits;
    const std::uint32_t i = g_cap_head.fetch_add(1, std::memory_order_relaxed);
    CapRec& r = g_cap[i & kCapMask];
    r.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (unsigned k = 0; k < kCapWords; ++k) r.w[k].store(w[k], std::memory_order_relaxed);
    r.seq.store(i + 1, std::memory_order_release);
}

void H_Talk(void* comp, void* msg) {
    if (FilterTalk(comp, msg)) return;
    o_talk(comp, msg);
}

bool CallsTo(std::uintptr_t site, std::uint8_t op, std::uintptr_t target) {
    std::uint8_t b = 0;
    return Rd(site, b) && b == op && mem::RipTarget(site, 1, 5) == target;
}

bool TalkSitesOk(char* why, std::size_t cap) {
    const std::uintptr_t b = mem::Base();
    if (!CallsTo(b + kTalkThunk, 0xE9, b + kTalk)) return std::snprintf(why, cap, "the handler thunk does not jump to the handler"), false;
    if (Q(b + kTalkTableEntry) != b + kTalkThunk) return std::snprintf(why, cap, "the handler table entry does not hold the thunk"), false;
    if (!mem::CodeMatches(b + kTalk, kTalkSig)) return std::snprintf(why, cap, "the handler's bytes differ"), false;
    return true;
}

// ---------------------------------------------------------------- the cargo menu figure's voice (debug op 2)
struct AkukanSites {
    bool done = false, ok = false;
    char why[160] = "not checked";
} g_ak;

bool AkukanOk() {
    if (g_ak.done) return g_ak.ok;
    g_ak.done = true;
    const std::uintptr_t b = mem::Base();
    if (!mem::CodeMatches(b + kAkukanVoice, kAkukanVoiceSig) || mem::RipTarget(b + kAkukanVoice + 8, 3, 7) != b + kWork ||
        !CallsTo(b + kAkukanVoice + 0xF, 0xE8, b + kFigureOf) || !CallsTo(b + kAkukanVoiceCall, 0xE9, b + kAkukanSay) ||
        !mem::CodeMatches(b + kAkukanSay, kAkukanSaySig))
        std::snprintf(g_ak.why, sizeof(g_ak.why), "PlayAkukanVoice (ds.exe+0x%llX) or its callee does not match", static_cast<unsigned long long>(kAkukanVoice));
    else
        g_ak.ok = true;
    Log("[voice] cargo menu figure voice sites %s", g_ak.ok ? "verified (PlayAkukanVoice -> 0x14273BB10)" : g_ak.why);
    return g_ak.ok;
}

// What 0x14273BB10 reads before it builds the message, fault-free: the figure's resource (its StreamingRef at
// e+0x58: [[ref]] non-null with bit 59 of [ref]+8 set, resource = [[[ref]]+0x20]), the list count (+0x118), the
// list (+0x120)[n] and its sentence count (+0x20) and array (+0x28). The function itself checks n < count.
bool AkukanList(std::uintptr_t e, int n, std::int32_t* count, std::int32_t* sentences) {
    *count = 0, *sentences = 0;
    const std::uintptr_t ref = Q(e + 0x58);
    std::uint64_t flags = 0;
    const std::uintptr_t obj = ref ? Q(ref) : 0;
    if (!obj || !Rd(ref + 8, flags) || static_cast<std::int8_t>(static_cast<std::uint8_t>(flags >> 52)) >= 0) return false;
    const std::uintptr_t res = Q(obj + 0x20);
    if (!res || !Rd(res + 0x118, *count) || *count <= 0 || *count > 4096 || n < 0 || n >= *count) return false;
    const std::uintptr_t list = Q(res + 0x120);
    const std::uintptr_t entry = list ? Q(list + static_cast<std::uintptr_t>(n) * 8) : 0;
    if (!entry || !Rd(entry + 0x20, *sentences) || *sentences <= 0 || *sentences > 4096) return false;
    const std::uintptr_t arr = Q(entry + 0x28);
    return arr && Q(arr) != 0;
}

using AkukanVoiceFn = void (*)(int);
bool CallAkukanVoice(int n) {
    __try {
        reinterpret_cast<AkukanVoiceFn>(mem::Base() + kAkukanVoice)(n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- silencing Sam's voice in Wwise (the talk runs)
// With a PostEvent hook in (partner_sound's or the probe's; they call PrePost before the original), a muted local
// talk is not dropped: the game runs it (so whatever follows it, BB's answer to a dialogue line, still comes), and the
// one external-source post that plays its voice gets SetGameObjectOutputBusVolume(object, listener 0, 0) first (a
// queued Wwise message, the same thread, so it applies before the post). Without that hook the talk is dropped as
// before. Which post is the local player's voice: the local DSTalkComponent's playing instance ([[comp+0x40]+0x90],
// read fault-free: "exact"), or, failing that, a talk token (a local talk let through in the last kTokenMs) and the
// post's object position (+0x20) within kNearM of the local player's head and nearer to it than to the partner's
// talking entity, and not one of the other talkers' preset events (BB, the cuff device).
constexpr std::uintptr_t kSetBusVolume = 0x3A1EEF0;  // export SetGameObjectOutputBusVolume(_K emitter, _K listener, float)
constexpr const char* kSetBusVolumeSig = "48 89 5C 24 08 57 48 83 EC 30 0F 29 74 24 20 48 8B DA 0F 57 F6 48 8B F9";
constexpr const char* kSetBusVolumeName = "?SetGameObjectOutputBusVolume@SoundEngine@AK@@YA?AW4AKRESULT@@_K0M@Z";
constexpr std::uintptr_t kTalkPlaying = 0x40, kPlayingInstance = 0x90, kObjPos = 0x20;
constexpr double kTokenMs = 3000.0, kNearM = 3.0, kHeadM = 1.55;
constexpr std::uint32_t kOtherTalkerEvents[] = {0x8118407Cu, 0xD44C68CFu, 0xE5BEA679u};  // BB, the cuff device, a child's VoiceComponent
using BusVolumeFn = int (*)(std::uint64_t, std::uint64_t, float);
BusVolumeFn g_bus_volume = nullptr;
bool g_want_post = false;            // mute resolved at start and the export checked: silencing wanted
std::atomic<bool> g_silence{false};  // a PostEvent hook calls PrePost: silence instead of drop
std::atomic<std::uintptr_t> g_local_comp{0};
std::atomic<std::int32_t> g_tokens{0};
std::atomic<std::int64_t> g_token_until{0};
std::atomic<std::uint64_t> g_pos_bits[3], g_echo_bits[3];
std::atomic<bool> g_pos_ok{false}, g_echo_ok{false};
constexpr unsigned kMutedSlots = 16;
std::atomic<std::uint64_t> g_muted_obj[kMutedSlots];
std::atomic<std::uint32_t> g_muted_n{0};
std::atomic<std::uint32_t> g_sil_talks{0}, g_sil_posts{0}, g_sil_exact{0}, g_sil_token{0}, g_sil_fail{0}, g_sil_restored{0}, g_sil_unmatched{0},
    g_sil_other{0};
std::atomic<std::uint64_t> g_last_unmatched{0};  // ev | obj low bits, for the log
std::int64_t g_qpf = 1;

std::int64_t QpcNow() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
void StoreVec(std::atomic<std::uint64_t> (&dst)[3], const double (&v)[3]) noexcept {
    for (int k = 0; k < 3; ++k) {
        std::uint64_t b = 0;
        std::memcpy(&b, &v[k], 8);
        dst[k].store(b, std::memory_order_relaxed);
    }
}
void LoadVec(const std::atomic<std::uint64_t> (&src)[3], double (&v)[3]) noexcept {
    for (int k = 0; k < 3; ++k) {
        const std::uint64_t b = src[k].load(std::memory_order_relaxed);
        std::memcpy(&v[k], &b, 8);
    }
}
double Dist3(const double (&a)[3], const double (&b)[3]) noexcept {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
int CallBusVolume(std::uint64_t obj, float v) noexcept {
    __try {
        return g_bus_volume(obj, 0, v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool MutedHas(std::uint64_t obj, bool remove) noexcept {
    for (std::atomic<std::uint64_t>& s : g_muted_obj) {
        std::uint64_t cur = s.load(std::memory_order_acquire);
        if (cur != obj) continue;
        if (remove && s.compare_exchange_strong(cur, 0, std::memory_order_acq_rel)) g_muted_n.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}
void MutedAdd(std::uint64_t obj) noexcept {
    if (MutedHas(obj, false)) return;
    for (std::atomic<std::uint64_t>& s : g_muted_obj) {
        std::uint64_t zero = 0;
        if (s.compare_exchange_strong(zero, obj, std::memory_order_acq_rel)) {
            g_muted_n.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // full (16 objects muted and never reused or unregistered): the oldest-looking slot is reused; its object keeps
    // volume 0 until Wwise forgets it at its unregistration (pooled voice instances are re-registered per sound)
    g_muted_obj[obj % kMutedSlots].store(obj, std::memory_order_release);
}

std::uint32_t g_last[6] = {};
double g_next_log_ms = 0;

void LogCounts(const char* why) {
    Log("[voice] %s: self_voice=%s (%s, character '%s'): local player talks muted %u (dropped %u, silenced in Wwise %u) passed %u; menu figure talks "
        "muted %u passed %u; other-branch messages passed %u; talk messages seen %u | voice posts silenced %u (exact %u, token %u), volume call failed "
        "%u, restored %u, near the player but not matched %u, other talkers kept %u",
        why, ModeName(g_mode),
        !g_armed.load() ? "not armed" : !g_mute.load() ? "armed, mute off" : g_silence.load() ? "muting: the talk runs, its voice is silenced in Wwise" : "muting: the talk is dropped",
        g_character, g_drop_local.load() + g_sil_talks.load(), g_drop_local.load(), g_sil_talks.load(), g_pass_local.load(), g_drop_menu.load(),
        g_pass_menu.load(), g_pass_other_branch.load(), g_seen.load(), g_sil_posts.load(), g_sil_exact.load(), g_sil_token.load(), g_sil_fail.load(),
        g_sil_restored.load(), g_sil_unmatched.load(), g_sil_other.load());
}

}  // namespace

bool FilterTalk(void* comp, void* msg) noexcept {
    if (!g_armed.load(std::memory_order_relaxed)) return false;
    g_seen.fetch_add(1, std::memory_order_relaxed);
    const std::uintptr_t local = g_local.load(std::memory_order_relaxed), fig = g_figure.load(std::memory_order_relaxed);
    if (!local && !fig) return false;
    std::uintptr_t owner = 0;
    if (!Rd(reinterpret_cast<std::uintptr_t>(comp) + kCompOwner, owner) || !owner) return false;
    const bool is_local = owner == local, is_fig = fig && owner == fig;
    if (!is_local && !is_fig) return false;
    std::uint8_t stop = 1;
    if (!Rd(reinterpret_cast<std::uintptr_t>(msg) + kMsgStop, stop) || stop != 0) {
        g_pass_other_branch.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bool mute = g_mute.load(std::memory_order_relaxed);
    // The partner voice copy: the local player's own talks only (never the menu figure), muted or not.
    if (is_local && g_capture.load(std::memory_order_relaxed)) Capture(reinterpret_cast<std::uintptr_t>(msg), mute);
    if (!mute) {
        (is_local ? g_pass_local : g_pass_menu).fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (is_local && g_silence.load(std::memory_order_relaxed)) {
        // The talk runs (what the game chains to it still happens); its voice post is silenced in PrePost. Its
        // subtitle (a spoken line) is dropped once by the subtitle hook.
        g_local_comp.store(reinterpret_cast<std::uintptr_t>(comp), std::memory_order_relaxed);
        g_token_until.store(QpcNow() + static_cast<std::int64_t>(kTokenMs * static_cast<double>(g_qpf) / 1000.0), std::memory_order_relaxed);
        if (g_tokens.fetch_add(1, std::memory_order_relaxed) >= 4) g_tokens.store(4, std::memory_order_relaxed);
        std::int32_t n = 0;
        std::uintptr_t arr = 0;
        if (Rd(reinterpret_cast<std::uintptr_t>(msg) + kMsgCount, n) && n > 0 && n <= 4 && Rd(reinterpret_cast<std::uintptr_t>(msg) + kMsgEntries, arr) && arr)
            for (std::int32_t i = 0; i < n; ++i) {
                std::uintptr_t sentence = 0, text = 0;
                if (Rd(arr + static_cast<std::uintptr_t>(i) * kEntrySize, sentence) && sentence && Rd(sentence + 0x40, text) && text)
                    pvoice::ArmSubtitleText(text, true);
            }
        g_sil_talks.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    (is_local ? g_drop_local : g_drop_menu).fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PrePost(std::uint32_t ev, std::uint64_t obj, std::uint32_t n_ext) noexcept {
    if (!g_silence.load(std::memory_order_relaxed) || !obj || obj >= 0xFFFFFFFFFFFFFFE0ull) return;
    const bool mute = g_mute.load(std::memory_order_relaxed);
    const bool was_muted = g_muted_n.load(std::memory_order_relaxed) && MutedHas(obj, false);
    bool silence = false;
    if (n_ext && mute) {
        bool other = false;
        for (std::uint32_t o : kOtherTalkerEvents) other |= o == ev;
        // exact: the local talk component's playing instance is this object
        const std::uintptr_t comp = g_local_comp.load(std::memory_order_relaxed);
        std::uintptr_t playing = 0, inst = 0;
        const bool exact = !other && comp && Rd(comp + kTalkPlaying, playing) && playing && Rd(playing + kPlayingInstance, inst) && inst == obj;
        // else: a token and the position
        double p[3] = {}, head[3] = {}, echo[3] = {};
        bool close_by = false, nearer = true;
        if (!exact && !other && g_pos_ok.load(std::memory_order_relaxed) && mem::Read(static_cast<std::uintptr_t>(obj) + kObjPos, p, sizeof(p))) {
            LoadVec(g_pos_bits, head);
            head[2] += kHeadM;
            const double dl = Dist3(p, head);
            close_by = std::isfinite(dl) && dl <= kNearM;
            if (close_by && g_echo_ok.load(std::memory_order_relaxed)) {
                LoadVec(g_echo_bits, echo);
                nearer = dl < Dist3(p, echo);
            }
        }
        const bool token = g_tokens.load(std::memory_order_relaxed) > 0 && QpcNow() < g_token_until.load(std::memory_order_relaxed);
        if (exact || (token && close_by && nearer)) {
            silence = true;
            if (g_tokens.fetch_sub(1, std::memory_order_relaxed) <= 0) g_tokens.store(0, std::memory_order_relaxed);
            (exact ? g_sil_exact : g_sil_token).fetch_add(1, std::memory_order_relaxed);
        } else if (other) {
            g_sil_other.fetch_add(1, std::memory_order_relaxed);
        } else if (close_by && nearer) {
            g_sil_unmatched.fetch_add(1, std::memory_order_relaxed);
            g_last_unmatched.store(static_cast<std::uint64_t>(ev) | (obj << 32), std::memory_order_relaxed);
        }
    }
    if (silence) {
        if (CallBusVolume(obj, 0.0f) == 1) {
            g_sil_posts.fetch_add(1, std::memory_order_relaxed);
            MutedAdd(obj);
        } else {
            g_sil_fail.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (was_muted) {
        // a muted object reused for another sound: back to full volume first
        if (CallBusVolume(obj, 1.0f) == 1) g_sil_restored.fetch_add(1, std::memory_order_relaxed);
        MutedHas(obj, true);
    }
}

void NoteUnregister(std::uint64_t obj) noexcept {
    // Wwise forgets the object's volume with it
    if (g_muted_n.load(std::memory_order_relaxed)) MutedHas(obj, true);
}

bool WantsPostHook() noexcept { return g_want_post; }

void PostHookReady(bool ready) noexcept {
    if (!g_want_post) return;
    g_silence.store(ready);
    Log("[voice] Sam's muted voice: %s", ready ? "the talk runs and its voice post is silenced in Wwise (SetGameObjectOutputBusVolume 0; what the game chains to "
                                                   "the line, BB's answer, still comes; its subtitle is dropped)"
                                                 : "no PostEvent hook: the talk is dropped instead (BB does not answer a muted dialogue line)");
}

void NoteEcho(game::Entity* echo) noexcept {
    game::WorldTransform w{};
    if (echo && game::TryGetTransform(echo, &w)) {
        const double v[3] = {w.pos.x, w.pos.y, w.pos.z};
        StoreVec(g_echo_bits, v);
        g_echo_ok.store(true, std::memory_order_relaxed);
    } else {
        g_echo_ok.store(false, std::memory_order_relaxed);
    }
}

bool PopTalk(TalkCapture* out) noexcept {
    for (;;) {
        const std::uint32_t head = g_cap_head.load(std::memory_order_acquire);
        if (head - g_cap_tail > kCapCap) {
            g_cap_dropped += head - g_cap_tail - kCapCap;
            g_cap_tail = head - kCapCap;
        }
        if (g_cap_tail == head) return false;
        const CapRec& r = g_cap[g_cap_tail & kCapMask];
        if (r.seq.load(std::memory_order_acquire) != g_cap_tail + 1) return false;  // still being written: next tick
        std::uint64_t w[kCapWords];
        for (unsigned k = 0; k < kCapWords; ++k) w[k] = r.w[k].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (r.seq.load(std::memory_order_relaxed) != g_cap_tail + 1) {  // overwritten while read
            ++g_cap_tail, ++g_cap_dropped;
            continue;
        }
        ++g_cap_tail;
        TalkCapture c;
        c.n = static_cast<std::uint8_t>(w[0] & 0xFF);
        c.muted = (w[0] & 0x100) != 0;
        c.truncated = (w[0] & 0x200) != 0;
        c.qpc = w[1];
        if (c.n == 0 || c.n > kCaptureMax) continue;
        for (unsigned i = 0; i < c.n; ++i) {
            c.sentence[i] = static_cast<std::uintptr_t>(w[2 + i]);
            std::memcpy(c.guid[i], &w[6 + i * 2], 16);
        }
        const std::uint32_t dbits = static_cast<std::uint32_t>(w[14]);
        std::memcpy(&c.delay, &dbits, 4);
        *out = c;
        return true;
    }
}

void CaptureCounters(std::uint32_t* unreadable, std::uint32_t* dropped) noexcept {
    *unreadable = g_cap_unreadable.load(std::memory_order_relaxed);
    *dropped = g_cap_dropped;
}

const char* Character() noexcept { return g_character; }

bool Install(Mode mode, bool is_host, const std::string& my_character, bool capture) noexcept {
    g_mode = mode;
    const bodies::OwnChoice own = bodies::ChooseOwn(is_host, my_character.c_str());
    g_character = own.body ? own.body->key : "sam";
    const bool mute = ResolveMute(mode, g_character);
    g_resolved_mute = mute;
    if (!mute && !capture) {
        Log("[voice] self_voice=%s, character '%s': Sam's voice plays as usual (nothing hooked)", ModeName(mode), g_character);
        return false;
    }
    // The probe's hook (when it is in) already replaced the handler's first bytes; it checked the same bytes, the
    // thunk and the table entry before hooking. Otherwise they are checked here.
    const bool probe = audioprobe::TalkHooked();
    char why[160] = {};
    if (!probe && !TalkSitesOk(why, sizeof(why))) {
        Log("[voice] self_voice=%s, character '%s': NOT %s: %s", ModeName(mode), g_character, mute ? "muted (and no partner voice copy)" : "hooked (no partner voice copy)", why);
        return false;
    }
    if (mute) {
        // Silencing in Wwise (the talk runs) needs this export and a PostEvent hook (PostHookReady says, after psound::Install).
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpf = f.QuadPart > 0 ? f.QuadPart : 1;
        const auto got = reinterpret_cast<std::uintptr_t>(GetProcAddress(GetModuleHandleW(nullptr), kSetBusVolumeName));
        if (got && got == mem::Base() + kSetBusVolume && mem::CodeMatches(got, kSetBusVolumeSig)) {
            g_bus_volume = reinterpret_cast<BusVolumeFn>(got);
            g_want_post = true;
        } else {
            Log("[voice] SetGameObjectOutputBusVolume (ds.exe+0x%llX) %s: a muted talk is dropped", static_cast<unsigned long long>(kSetBusVolume),
                !got ? "not exported" : got != mem::Base() + kSetBusVolume ? "moved" : "bytes differ");
        }
    }
    g_mute.store(mute);  // before arming: an armed filter never counts a talk as passed because of the order
    g_capture.store(capture);
    if (probe) {
        // One hook per target: the probe's detour asks FilterTalk before it runs the handler.
        g_armed.store(true);
    } else {
        const MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(mem::Base() + kTalk), reinterpret_cast<void*>(&H_Talk), reinterpret_cast<void**>(&o_talk));
        const MH_STATUS e = s == MH_OK ? MH_EnableHook(reinterpret_cast<void*>(mem::Base() + kTalk)) : s;
        if (e != MH_OK) {
            if (s == MH_OK) MH_RemoveHook(reinterpret_cast<void*>(mem::Base() + kTalk));
            g_mute.store(false);
            g_capture.store(false);
            Log("[voice] self_voice=%s, character '%s': NOT hooked: the talk hook failed (%s)", ModeName(mode), g_character, MH_StatusToString(e));
            return false;
        }
        g_own_hook = true;
        g_armed.store(true);
    }
    if (mute)
        Log("[voice] self_voice=%s, character '%s': Sam's voice is muted for this player (talk handler ds.exe+0x%llX, %s; the cargo menu's figure "
            "too while it shows the chosen character)%s",
            ModeName(mode), g_character, static_cast<unsigned long long>(kTalk), g_own_hook ? "own hook" : "through the audio probe's hook",
            capture ? "; the local player's talks are copied for the partner voice" : "");
    else
        Log("[voice] self_voice=%s, character '%s': Sam's voice plays as usual; the talk handler (ds.exe+0x%llX, %s) copies the local player's talks for "
            "the partner voice",
            ModeName(mode), g_character, static_cast<unsigned long long>(kTalk), g_own_hook ? "own hook" : "through the audio probe's hook");
    return true;
}

bool Armed() noexcept { return g_armed.load() && g_mute.load(); }
bool Capturing() noexcept { return g_armed.load() && g_capture.load(); }

void Tick(game::Entity* local, double now_ms) noexcept {
    g_local.store(reinterpret_cast<std::uintptr_t>(local), std::memory_order_relaxed);  // (also for the test op 3 when not armed)
    if (!g_armed.load(std::memory_order_relaxed)) return;
    if (g_silence.load(std::memory_order_relaxed)) {
        game::WorldTransform w{};
        if (local && game::TryGetTransform(local, &w)) {
            const double v[3] = {w.pos.x, w.pos.y, w.pos.z};
            StoreVec(g_pos_bits, v);
            g_pos_ok.store(true, std::memory_order_relaxed);
        } else {
            g_pos_ok.store(false, std::memory_order_relaxed);
        }
        const std::uint64_t um = g_last_unmatched.exchange(0, std::memory_order_relaxed);
        if (um)
            Log("[voice] an external-source post (event 0x%08X, object ...%08X) near the local player was not matched to a muted talk: not silenced",
                static_cast<unsigned>(um & 0xFFFFFFFFu), static_cast<unsigned>(um >> 32));
    }
    const std::uintptr_t fig = realbody::MenuSelfFigure();
    const std::uintptr_t was = g_figure.exchange(fig, std::memory_order_relaxed);
    if (fig != was)
        Log("[voice] cargo menu figure %p %s", reinterpret_cast<void*>(fig ? fig : was),
            fig ? "shows the chosen character: its voice is muted too" : "is Sam again (or the menu closed): its voice is not muted");
    if (now_ms < g_next_log_ms) return;
    g_next_log_ms = now_ms + 1000;
    const std::uint32_t now[6] = {g_drop_local.load() + g_sil_talks.load(), g_drop_menu.load(), g_pass_local.load(), g_pass_menu.load(), g_pass_other_branch.load(),
                                  g_sil_posts.load() + g_sil_unmatched.load() * 7u + g_sil_restored.load() * 13u + g_sil_fail.load() * 17u};
    bool changed = false;
    for (int i = 0; i < 6; ++i) changed |= now[i] != g_last[i];
    if (!changed) return;
    for (int i = 0; i < 6; ++i) g_last[i] = now[i];
    LogCounts("counts");
}

bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    auto say = [&](const char* fmt, auto... a) { std::snprintf(msg, cap, fmt, a...); };
    switch (op) {
    case 0:
        LogCounts("status");
        say("self_voice=%s (%s): muted %u local / %u menu, passed %u / %u, voice posts silenced %u", ModeName(g_mode),
            !g_armed.load() ? "not armed" : g_mute.load() ? "muting" : "mute off", g_drop_local.load() + g_sil_talks.load(), g_drop_menu.load(),
            g_pass_local.load(), g_pass_menu.load(), g_sil_posts.load());
        return true;
    case 1: {
        if (!g_armed.load() || !g_resolved_mute) {
            say("not armed: self_voice resolved to sam at start (nothing muted)");
            return false;
        }
        const bool on = v[1] != 0.0;
        g_mute.store(on);
        LogCounts(on ? "mute on (debug)" : "mute off (debug)");
        say("Sam's voice %s", on ? "muted" : "plays (mute off)");
        return true;
    }
    case 3: {
        // a test situation through the local player's voice queue (the probe's list; works without the probe)
        const int n = v[1] >= 0 && v[1] < 64 ? static_cast<int>(v[1]) : -1;
        return audioprobe::TestSituation(reinterpret_cast<game::Entity*>(g_local.load()), n, msg, cap);
    }
    case 2: {
        const int n = v[1] >= 0 && v[1] < 64 ? static_cast<int>(v[1]) : -1;
        if (!AkukanOk()) {
            say("%s", g_ak.why);
            return false;
        }
        menufig::View view;
        if (!menufig::Read(&view) || !view.e || !menufig::Open(view)) {
            say("the cargo menu's figure is not shown (state %d)", view.state);
            return false;
        }
        std::int32_t count = 0, sentences = 0;
        if (!AkukanList(view.e, n, &count, &sentences)) {
            say("figure %p: line list %d is not there (%d lists)", reinterpret_cast<void*>(view.e), n, count);
            return false;
        }
        const bool muted = g_armed.load() && g_mute.load() && g_figure.load() == view.e;  // as FilterTalk will decide
        if (!CallAkukanVoice(n)) {
            say("PlayAkukanVoice(%d) faulted", n);
            return false;
        }
        Log("[voice] asked the cargo menu figure %p for line list %d of %d (%d sentences): the game's PlayAkukanVoice; figure %s", reinterpret_cast<void*>(view.e),
            n, count, sentences, muted ? "muted" : "not muted");
        say("figure %p: list %d of %d (%d sentences) asked", reinterpret_cast<void*>(view.e), n, count, sentences);
        return true;
    }
    default:
        say("ops: 0 log, 1 <0/1> mute off / on, 2 <n> the menu figure says line list n, 3 <n> test situation n (voice queue)");
        return false;
    }
}

}  // namespace st::selfvoice

namespace st::game {
bool SelfVoiceCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept { return selfvoice::DebugCommand(op, v, msg, cap); }
}  // namespace st::game
