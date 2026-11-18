#include "partner_voice.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "MinHook.h"
#include "audio_probe.hpp"
#include "game_actions.hpp"
#include "link.hpp"
#include "log.hpp"
#include "partner_source.hpp"
#include "real_body.hpp"
#include "self_voice.hpp"

namespace st::pvoice {
namespace {

namespace mem = partner::mem;
using mem::Q;
using mem::Rd;

// ---------------------------------------------------------------- ds.exe (RVAs at base 0x140000000)
// Evidence: docs/audio-research.md sections 3, 6.1, 10, 15 (decompiles of 0x1422D3FE0, 0x1422D5A40, 0x1422D6230,
// 0x14361A6C0, 0x1430C09F0, 0x1426C2040; the RTTI catalog).
constexpr std::uintptr_t kPlaySentence = 0x22D3FE0;  // DSTalkComponent::PlaySentence(comp, SentenceResource*, i32, Params*)
constexpr const char* kPlaySentenceSig = "48 89 5C 24 18 55 56 57 48 81 EC B0 00 00 00 48 8B 05";
constexpr std::uintptr_t kTalkVtable = 0x3CF65E0;    // DSTalkComponent's vtable; +0x68 = PlaySentence
constexpr std::uintptr_t kRttiTalk = 0x4AE8400;      // DSTalkComponent
constexpr std::uintptr_t kRttiSentence = 0x4ADC440;  // SentenceResource
constexpr std::uintptr_t kRttiGroup = 0x4BE0370;     // DSSituationSentenceGroupResource
constexpr std::uintptr_t kVoiceMgr = 0x4500, kMgrGroup = 0x28;  // DSPlayerEntity + 0x4500 = voice manager; +0x28 the resident group
constexpr std::uintptr_t kGroupSituations = 0x20;  // Array<Ref<DSSentenceSituationResource>> {i32 count, i32 cap, T* data}
constexpr std::uintptr_t kSituationSentences = 0x20;  // Array<Ref<DSPlayerSentenceResource>>
constexpr std::uintptr_t kPlayerSentenceRef = 0x20;   // DSPlayerSentenceResource.SentenceResource
constexpr std::uintptr_t kSentenceText = 0x40;        // SentenceResource.Text (Ref<LocalizedTextResource>)
constexpr std::uintptr_t kSentenceShowSubtitles = 0x28;
constexpr std::uintptr_t kCompOwner = 0x48;
// The subtitle request (vtable slot at 0x143F15240): (ui, LocalizedTextResource* text, speaker, const u8[16]*, u32, u32)
// builds MsgShowSubtitle (vtable 0x143F15258) and hands it to the subtitles HUD ([ui+0x610]).
constexpr std::uintptr_t kSubtitle = 0x361A6C0;
constexpr const char* kSubtitleSig = "48 83 EC 68 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58 48 83 B9 10 06 00 00 00 4C 8B D1 0F 84";
constexpr std::uintptr_t kSubtitleSlot = 0x3F15240, kSubtitleMsgLea = 0x361A701, kShowSubtitleVtable = 0x3F15258;

Config g_cfg;
bool g_enabled = false;
bool g_subtitles = false;  // voice_subtitles=1: keep the partner's subtitles
bool g_is_host = false;

// ---------------------------------------------------------------- counters (game thread unless atomic)
std::uint32_t g_tx_seq = 0;
std::uint32_t g_talks = 0, g_sent_lines = 0, g_sent_muted = 0, g_send_fail = 0;
std::uint32_t g_rx = 0, g_rx_dup = 0, g_rx_muted = 0, g_rx_played = 0, g_rx_refused = 0, g_rx_unresolved = 0, g_rx_gated = 0, g_rx_no_emitter = 0,
              g_rx_far = 0, g_rx_fault = 0;
bool g_have_rx_seq = false;
std::uint32_t g_last_rx_seq = 0;
bool g_force_sam = false;  // debug op 2
double g_next_track_ms = 0, g_next_counts_ms = 0;
std::uint32_t g_logged_counts_hash = 0;

// ---------------------------------------------------------------- GUID -> SentenceResource (the resident group)
struct Entry {
    std::uint8_t guid[16];
    std::uintptr_t sentence;
    bool text;
};
std::vector<Entry> g_map;
std::uintptr_t g_map_group = 0, g_map_local = 0;
unsigned g_map_situations = 0, g_map_refs = 0, g_map_spoken = 0, g_map_bad = 0;
double g_map_retry_ms = 0;

bool GuidLess(const Entry& a, const Entry& b) { return std::memcmp(a.guid, b.guid, 16) < 0; }

void GuidText(const std::uint8_t* g, char (&out)[37]) {
    std::snprintf(out, sizeof(out), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
                  g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

std::uintptr_t GroupOf(std::uintptr_t local) { return local ? Q(local + kVoiceMgr + kMgrGroup) : 0; }

// Walks the group once (fault-free; arrays read whole). False with why when the group is not the expected type.
bool BuildMap(std::uintptr_t local, const char** why) {
    g_map.clear();
    g_map_group = 0, g_map_local = local, g_map_situations = 0, g_map_refs = 0, g_map_spoken = 0, g_map_bad = 0;
    const std::uintptr_t base = mem::Base();
    const std::uintptr_t group = GroupOf(local);
    if (!group) return *why = "the local player has no resident sentence group (voice manager +0x28)", false;
    if (mem::RttiOf(group) != base + kRttiGroup) return *why = "voice manager +0x28 is not a DSSituationSentenceGroupResource", false;
    std::int32_t n = 0;
    if (!Rd(group + kGroupSituations, n) || n <= 0 || n > 4096) return *why = "the group's situation count is unreadable", false;
    std::vector<std::uintptr_t> sits(static_cast<std::size_t>(n));
    if (!mem::Read(Q(group + kGroupSituations + 8), sits.data(), sits.size() * 8)) return *why = "the group's situation array is unreadable", false;
    std::vector<std::uintptr_t> refs;
    for (std::uintptr_t s : sits) {
        std::int32_t m = 0;
        if (!s || !Rd(s + kSituationSentences, m) || m <= 0 || m > 4096) continue;
        refs.resize(static_cast<std::size_t>(m));
        if (!mem::Read(Q(s + kSituationSentences + 8), refs.data(), refs.size() * 8)) continue;
        ++g_map_situations;
        for (std::uintptr_t ps : refs) {
            const std::uintptr_t sentence = ps ? Q(ps + kPlayerSentenceRef) : 0;
            Entry e{};
            if (!sentence || !mem::Read(sentence + 8, e.guid, 16)) continue;
            e.sentence = sentence;
            ++g_map_refs;
            g_map.push_back(e);
        }
    }
    std::sort(g_map.begin(), g_map.end(), GuidLess);
    g_map.erase(std::unique(g_map.begin(), g_map.end(), [](const Entry& a, const Entry& b) { return std::memcmp(a.guid, b.guid, 16) == 0; }), g_map.end());
    // Every distinct sentence must be a SentenceResource; one that is not is dropped (counted).
    std::vector<Entry> ok;
    ok.reserve(g_map.size());
    for (Entry& e : g_map) {
        if (mem::RttiOf(e.sentence) != base + kRttiSentence) {
            ++g_map_bad;
            continue;
        }
        e.text = Q(e.sentence + kSentenceText) != 0;
        g_map_spoken += e.text ? 1u : 0u;
        ok.push_back(e);
    }
    g_map.swap(ok);
    if (g_map.empty()) return *why = "no SentenceResource in the group", false;
    g_map_group = group;
    return true;
}

// The map for the local player's current group (rebuilt when the group changes).
bool MapReady(std::uintptr_t local, double now) {
    if (!local) return false;
    const std::uintptr_t group = GroupOf(local);
    if (group && group == g_map_group && !g_map.empty()) return true;
    if (now < g_map_retry_ms) return false;
    const char* why = "";
    if (!BuildMap(local, &why)) {
        g_map_retry_ms = now + 5000;
        Log("[pvoice] sentence map: %s (retry in 5 s)", why);
        return false;
    }
    Log("[pvoice] sentence map: %zu sentences (%u spoken, %zu non-verbal) from %u situations, %u references, of the resident group %p%s",
        g_map.size(), g_map_spoken, g_map.size() - g_map_spoken, g_map_situations, g_map_refs, reinterpret_cast<void*>(group),
        g_map_bad ? " (some were not SentenceResources: dropped)" : "");
    return true;
}

const Entry* Resolve(const std::uint8_t (&guid)[16]) {
    Entry key{};
    std::memcpy(key.guid, guid, 16);
    auto it = std::lower_bound(g_map.begin(), g_map.end(), key, GuidLess);
    if (it == g_map.end() || std::memcmp(it->guid, guid, 16) != 0) return nullptr;
    return &*it;
}

// ---------------------------------------------------------------- the talking entity's DSTalkComponent
std::uintptr_t g_talk_entity = 0, g_talk_comp = 0;
bool g_play_checked = false, g_play_ok = false;

bool PlaySentenceOk() {
    if (g_play_checked) return g_play_ok;
    g_play_checked = true;
    const std::uintptr_t b = mem::Base();
    // With the audio probe on, its hook replaced the first bytes (it checked the same bytes first).
    g_play_ok = Q(b + kTalkVtable + 0x68) == b + kPlaySentence && (audioprobe::SentenceHooked() || mem::CodeMatches(b + kPlaySentence, kPlaySentenceSig));
    Log("[pvoice] DSTalkComponent::PlaySentence (ds.exe+0x%llX, vtable +0x68) %s", static_cast<unsigned long long>(kPlaySentence),
        g_play_ok ? (audioprobe::SentenceHooked() ? "verified (through the audio probe's hook)" : "verified") : "does NOT match: the partner voice plays nothing");
    return g_play_ok;
}

std::uintptr_t TalkComponentOf(game::Entity* e) {
    const auto a = reinterpret_cast<std::uintptr_t>(e);
    if (!a) return 0;
    if (a == g_talk_entity && g_talk_comp && Q(g_talk_comp + kCompOwner) == a) return g_talk_comp;
    g_talk_entity = a, g_talk_comp = 0;
    const std::uintptr_t b = mem::Base();
    const std::uintptr_t c = partner::FindComponentByName(e, "DSTalkComponent");
    if (!c || mem::RttiOf(c) != b + kRttiTalk || Q(c) != b + kTalkVtable || Q(c + kCompOwner) != a) {
        Log("[pvoice] talking entity %p: no usable DSTalkComponent (found %p)", e, reinterpret_cast<void*>(c));
        partner::DescribeEntity(e, "partner voice entity");
        return 0;
    }
    g_talk_comp = c;
    Log("[pvoice] talking entity %p: DSTalkComponent %p", e, reinterpret_cast<void*>(c));
    partner::DescribeEntity(e, "partner voice entity");
    return c;
}

using PlayFn = bool (*)(void*, void*, std::int32_t, void*);
bool CallPlay(std::uintptr_t comp, std::uintptr_t sentence, bool* accepted) {
    __try {
        *accepted = reinterpret_cast<PlayFn>(mem::Base() + kPlaySentence)(reinterpret_cast<void*>(comp), reinterpret_cast<void*>(sentence), 0, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- the subtitle filter (any thread)
using SubtitleFn = void (*)(void*, void*, void*, void*, std::uint32_t, std::uint32_t);
SubtitleFn o_subtitle = nullptr;
bool g_sub_hooked = false;
struct SubSlot {
    std::atomic<std::uintptr_t> text{0};
    std::atomic<std::int64_t> until{0};  // QPC
    std::atomic<bool> self{false};       // the local player's own muted line (self_voice), not the partner's
};
constexpr unsigned kSubSlots = 8;
SubSlot g_sub[kSubSlots];
std::atomic<unsigned> g_sub_next{0};  // armed from the game thread (partner lines) and the talk handler's job threads (self)
std::atomic<std::uint32_t> g_sub_calls{0}, g_sub_dropped{0}, g_sub_kept{0}, g_sub_self_dropped{0};
// The last subtitle requests, for the log (seqlock per slot, as the probe's rings).
constexpr std::uint32_t kSubLogCap = 32;
struct SubRec {
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint64_t> text{0}, speaker{0}, flags{0};
};
SubRec g_sublog[kSubLogCap];
std::atomic<std::uint32_t> g_sublog_head{0};
std::uint32_t g_sublog_tail = 0;
std::int64_t g_qpf = 1;

std::int64_t QpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void SubLog(const void* text, const void* speaker, std::uint64_t flags) {
    const std::uint32_t i = g_sublog_head.fetch_add(1, std::memory_order_relaxed);
    SubRec& r = g_sublog[i % kSubLogCap];
    r.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    r.text.store(reinterpret_cast<std::uint64_t>(text), std::memory_order_relaxed);
    r.speaker.store(reinterpret_cast<std::uint64_t>(speaker), std::memory_order_relaxed);
    r.flags.store(flags, std::memory_order_relaxed);
    r.seq.store(i + 1, std::memory_order_release);
}

int Filter(const void* text) noexcept;  // 0 show, 1 drop (a partner line), 2 drop (the local player's own muted line)

std::int64_t QpcNowSafe() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Any thread. Returns the slot (for Disarm), or -1.
int Arm(std::uintptr_t text, double window_ms, bool self) noexcept {
    if (!text) return -1;  // a non-verbal line has no subtitle
    const unsigned i = g_sub_next.fetch_add(1, std::memory_order_relaxed) % kSubSlots;
    SubSlot& s = g_sub[i];
    s.text.store(0, std::memory_order_relaxed);
    s.self.store(self, std::memory_order_relaxed);
    s.until.store(QpcNowSafe() + static_cast<std::int64_t>(window_ms * static_cast<double>(g_qpf) / 1000.0), std::memory_order_relaxed);
    s.text.store(text, std::memory_order_release);
    return static_cast<int>(i);
}
void Disarm(int slot, std::uintptr_t text) noexcept {
    if (slot < 0) return;
    std::uintptr_t cur = text;
    g_sub[slot].text.compare_exchange_strong(cur, 0, std::memory_order_acq_rel);
}

void H_Subtitle(void* ui, void* text, void* speaker, void* p4, std::uint32_t p5, std::uint32_t p6) {
    const int r = Filter(text);
    const bool drop = r != 0;
    SubLog(text, speaker, static_cast<std::uint64_t>(r) | (static_cast<std::uint64_t>(p6) << 32));
    if (drop) return;
    o_subtitle(ui, text, speaker, p4, p5, p6);
}

// The partner's line: its subtitle request came 0-31 ms after PlaySentence in every run (section 15.4).
int ArmSubtitle(std::uintptr_t sentence, std::uintptr_t* text_out) {
    const std::uintptr_t text = Q(sentence + kSentenceText);
    *text_out = text;
    return Arm(text, kSubtitleWindowMs, false);
}

void DrainSubLog() {
    for (;;) {
        const std::uint32_t head = g_sublog_head.load(std::memory_order_acquire);
        if (head - g_sublog_tail > kSubLogCap) g_sublog_tail = head - kSubLogCap;
        if (g_sublog_tail == head) return;
        const SubRec& r = g_sublog[g_sublog_tail % kSubLogCap];
        if (r.seq.load(std::memory_order_acquire) != g_sublog_tail + 1) return;
        const std::uint64_t text = r.text.load(std::memory_order_relaxed), speaker = r.speaker.load(std::memory_order_relaxed),
                            flags = r.flags.load(std::memory_order_relaxed);
        ++g_sublog_tail;
        const std::uint32_t dbits = static_cast<std::uint32_t>(flags >> 32);
        float d = 0;
        std::memcpy(&d, &dbits, 4);
        const char* tt = text ? partner::TypeName(reinterpret_cast<const void*>(text)) : nullptr;
        const char* st = speaker ? partner::TypeName(reinterpret_cast<const void*>(speaker)) : nullptr;
        Log("[pvoice] subtitle request: text %p (%s) speaker %p (%s) last arg %.2f -> %s", reinterpret_cast<void*>(text), tt ? tt : "?",
            reinterpret_cast<void*>(speaker), st ? st : "-", static_cast<double>(d),
            (flags & 2) ? "DROPPED (the local player's own muted line, self_voice)" : (flags & 1) ? "DROPPED (a partner line, voice_subtitles=0)" : "shown");
    }
}

void InstallSubtitleHook() {
    const std::uintptr_t b = mem::Base();
    const char* why = nullptr;
    if (Q(b + kSubtitleSlot) != b + kSubtitle) why = "its vtable slot does not hold it";
    else if (!mem::CodeMatches(b + kSubtitle, kSubtitleSig)) why = "its bytes differ";
    else if (mem::RipTarget(b + kSubtitleMsgLea, 3, 7) != b + kShowSubtitleVtable) why = "it does not build MsgShowSubtitle";
    if (!why) {
        const MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(b + kSubtitle), reinterpret_cast<void*>(&H_Subtitle), reinterpret_cast<void**>(&o_subtitle));
        const MH_STATUS e = s == MH_OK ? MH_EnableHook(reinterpret_cast<void*>(b + kSubtitle)) : s;
        if (e != MH_OK) {
            if (s == MH_OK) MH_RemoveHook(reinterpret_cast<void*>(b + kSubtitle));
            why = MH_StatusToString(e);
        }
    }
    g_sub_hooked = !why;
    if (why)
        Log("[pvoice] subtitle hook (ds.exe+0x%llX) NOT installed: %s; the partner's lines may show subtitles", static_cast<unsigned long long>(kSubtitle), why);
    else
        Log("[pvoice] subtitle hook installed (ds.exe+0x%llX): the partner's subtitles are %s", static_cast<unsigned long long>(kSubtitle),
            g_subtitles ? "kept (voice_subtitles=1; counted only)" : "dropped (voice_subtitles=0)");
}

// ---------------------------------------------------------------- gating
// nullptr = the local game is being played; else why not.
const char* Gate(game::Entity* local) {
    static char why[96];
    if (!local) return "no local player";
    std::uint64_t flags = 0;
    if (!partner::ReadFlags(local, &flags)) return "the local player is unreadable";
    if (flags & game::kEntityFlagSleeping) return "the local player is asleep (loading, cutscene, menu)";
    std::uint64_t state = 0;
    std::uint32_t load = 0;
    if (!game::ReadStopGates(&state, &load)) return "no game state yet";
    if (load) {
        std::snprintf(why, sizeof(why), "loading / suspended (load mask 0x%X)", load);
        return why;
    }
    if (state & game::kStoppingGameEventMask) {
        char d[64];
        game::DescribeGameStateFlags(state & game::kStoppingGameEventMask, d, sizeof(d));
        std::snprintf(why, sizeof(why), "paused / menu / cutscene (%s)", d);
        return why;
    }
    return nullptr;
}

double Dist(const double (&a)[3], const double (&b)[3]) {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// The newest Frame (OnSpeech runs between ticks).
Frame g_frame;

bool Play(const Entry& en, const char* what, std::uint32_t seq, double now) {
    (void)now;
    char gt[37];
    GuidText(en.guid, gt);
    game::Entity* em = g_frame.emitter;
    if (const char* why = Gate(g_frame.local)) {
        ++g_rx_gated;
        Log("[pvoice] %s seq %u sentence %s (%s): not played: %s", what, seq, gt, en.text ? "spoken" : "non-verbal", why);
        return false;
    }
    if (!em) {
        ++g_rx_no_emitter;
        Log("[pvoice] %s seq %u sentence %s: not played: no talking entity (no partner body yet)", what, seq, gt);
        return false;
    }
    game::WorldTransform ew{}, lw{};
    if (!game::TryGetTransform(em, &ew) || !game::TryGetTransform(g_frame.local, &lw)) {
        ++g_rx_fault;
        Log("[pvoice] %s seq %u: not played: a transform is unreadable", what, seq);
        return false;
    }
    const double ep[3] = {ew.pos.x, ew.pos.y, ew.pos.z}, lp[3] = {lw.pos.x, lw.pos.y, lw.pos.z};
    const double d = Dist(ep, lp);
    if (!(d <= kMaxDistanceM)) {
        ++g_rx_far;
        Log("[pvoice] %s seq %u sentence %s: not played: the partner is %.0f m away (over %.0f m)", what, seq, gt, d, kMaxDistanceM);
        return false;
    }
    const std::uintptr_t comp = TalkComponentOf(em);
    if (!comp || !PlaySentenceOk()) {
        ++g_rx_no_emitter;
        return false;
    }
    if (mem::RttiOf(en.sentence) != mem::Base() + kRttiSentence) {  // the group was replaced under us: rebuild next time
        g_map_group = 0;
        ++g_rx_unresolved;
        Log("[pvoice] %s seq %u sentence %s: not played: the resolved sentence is no longer a SentenceResource (map rebuilt)", what, seq, gt);
        return false;
    }
    std::uint8_t show = 0;
    Rd(en.sentence + kSentenceShowSubtitles, show);
    std::uintptr_t text = 0;
    const int slot = g_sub_hooked ? ArmSubtitle(en.sentence, &text) : -1;  // voice_subtitles=1: only counted ("kept")
    bool accepted = false;
    const bool called = CallPlay(comp, en.sentence, &accepted);
    if (!called || !accepted) Disarm(slot, text);  // a refused line asks for no subtitle: the local player's own must not be eaten
    if (!called) {
        ++g_rx_fault;
        g_play_ok = false;  // never again this session
        Log("[pvoice] %s seq %u: PlaySentence FAULTED (comp %p, sentence %p): the partner voice is off for this session", what, seq, reinterpret_cast<void*>(comp),
            reinterpret_cast<void*>(en.sentence));
        return false;
    }
    (accepted ? g_rx_played : g_rx_refused)++;
    std::uint64_t eflags = 0;
    partner::ReadFlags(em, &eflags);
    Log("[pvoice] %s seq %u sentence %s (%s, ShowSubtitles %u) -> PlaySentence(comp %p, sentence %p) = %d | talking entity %p at (%.2f, %.2f, %.2f) "
        "flags 0x%llX, %.2f m from the local player",
        what, seq, gt, en.text ? "spoken" : "non-verbal", show, reinterpret_cast<void*>(comp), reinterpret_cast<void*>(en.sentence), accepted ? 1 : 0, em,
        ew.pos.x, ew.pos.y, ew.pos.z, static_cast<unsigned long long>(eflags), d);
    return accepted;
}

void LogCounts(const char* why) {
    std::uint32_t unreadable = 0, dropped = 0;
    selfvoice::CaptureCounters(&unreadable, &dropped);
    const link::Info li = link::GetInfo();
    Log("[pvoice] %s: sent %u talks (%u Sam lines, %u muted, %u not sent: no link) | received %u (%u duplicate, %u muted by the partner): played %u, "
        "refused by the game %u, unresolved %u, skipped %u (paused / menu / cutscene / loading), %u (no talking entity), %u (too far), faults %u | "
        "subtitles: %u requests, %u partner lines dropped, %u kept, %u own muted lines dropped | link: %llu speech frames queued, %llu delivered, %llu "
        "reliable resends | capture: %u unreadable, %u overwritten",
        why, g_talks, g_sent_lines, g_sent_muted, g_send_fail, g_rx, g_rx_dup, g_rx_muted, g_rx_played, g_rx_refused, g_rx_unresolved, g_rx_gated,
        g_rx_no_emitter, g_rx_far, g_rx_fault, g_sub_calls.load(), g_sub_dropped.load(), g_sub_kept.load(), g_sub_self_dropped.load(),
        static_cast<unsigned long long>(li.tx_speech),
        static_cast<unsigned long long>(li.rx_speech), static_cast<unsigned long long>(li.rel_retransmits), unreadable, dropped);
}

// Received frames wait for the next Tick (which has the newest talking entity and local player).
constexpr unsigned kPendingCap = 32;
constexpr unsigned kPlaysPerTick = 2;  // received lines handled per tick (a burst after a stall plays over a few ticks)
wire::Speech g_pending[kPendingCap];
unsigned g_pending_n = 0;
std::uint32_t g_pending_dropped = 0;

}  // namespace

// ================================================================== public
void Install(const Config& cfg) noexcept {
    g_cfg = cfg;
    g_enabled = cfg.voice_sync;
    g_subtitles = cfg.voice_subtitles;
    g_is_host = cfg.role == Role::host;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart > 0 ? f.QuadPart : 1;
    if (!g_enabled) {
        // self_voice silencing (the talk runs): Sam's own subtitle must still be dropped on a non-Sam player's game
        if (selfvoice::WantsPostHook()) InstallSubtitleHook();
        return;
    }
    Log("[pvoice] voice_sync=1: the local player's talks go to the partner (STS1, reliable), the partner's are played on their body's head; "
        "voice_subtitles=%d",
        g_subtitles ? 1 : 0);
    InstallSubtitleHook();
}

bool Enabled() noexcept { return g_enabled; }

bool EmitterTransform(game::Entity* body, const game::WorldTransform& root, game::WorldTransform* out) noexcept {
    *out = root;
    double head[3] = {};
    if (body && realbody::HeadWorld(body, head)) {
        out->pos.x = head[0], out->pos.y = head[1], out->pos.z = head[2];
        return true;
    }
    out->pos.z += kHeadFallbackM;
    return false;
}

const char* LocalGate(game::Entity* local) noexcept { return Gate(local); }

namespace {
int Filter(const void* text) noexcept {
    g_sub_calls.fetch_add(1, std::memory_order_relaxed);
    const auto t = reinterpret_cast<std::uintptr_t>(text);
    if (!t) return 0;
    const std::int64_t now = QpcNow();
    for (SubSlot& s : g_sub) {
        std::uintptr_t cur = s.text.load(std::memory_order_acquire);
        if (cur != t || now >= s.until.load(std::memory_order_relaxed)) continue;
        const bool self = s.self.load(std::memory_order_relaxed);
        if (!self && g_subtitles) {
            g_sub_kept.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        if (!s.text.compare_exchange_strong(cur, 0, std::memory_order_acq_rel)) continue;  // one-shot: another thread took it
        (self ? g_sub_self_dropped : g_sub_dropped).fetch_add(1, std::memory_order_relaxed);
        return self ? 2 : 1;
    }
    return 0;
}
}  // namespace

bool FilterSubtitle(const void* text) noexcept { return Filter(text) != 0; }

void ArmSubtitleText(std::uintptr_t text, bool self) noexcept { Arm(text, self ? kSelfSubtitleWindowMs : kSubtitleWindowMs, self); }

void NewSession() noexcept {
    g_have_rx_seq = false;  // our own seq keeps counting (the partner's filter starts again on its side)
    g_pending_n = 0;
}

namespace {
void Handle(const wire::Speech& s, double now) {
    ++g_rx;
    if (g_have_rx_seq && static_cast<std::int32_t>(s.seq - g_last_rx_seq) <= 0) {
        ++g_rx_dup;
        Log("[pvoice] received seq %u again (last %u): ignored", s.seq, g_last_rx_seq);
        return;
    }
    g_have_rx_seq = true, g_last_rx_seq = s.seq;
    if (s.kind == static_cast<std::uint8_t>(wire::SpeechKind::muted)) {
        ++g_rx_muted;
        Log("[pvoice] received seq %u kind 1 (the partner's character is not Sam: their Sam line was not sent; nothing to play)%s", s.seq,
            (s.flags & wire::kSpeechFromHost) ? " from the host" : "");
        return;
    }
    const std::uintptr_t local = reinterpret_cast<std::uintptr_t>(g_frame.local);
    if (!MapReady(local, now)) {
        ++g_rx_unresolved;
        Log("[pvoice] received seq %u kind %u (%u sentences): not played: no sentence map yet", s.seq, s.kind, s.n);
        return;
    }
    // The first sentence that resolves is played (the game's own talks carry one: 83 of 83 in Phase 0).
    for (unsigned i = 0; i < s.n; ++i) {
        const Entry* en = Resolve(s.guid[i]);
        char gt[37];
        GuidText(s.guid[i], gt);
        if (!en) {
            ++g_rx_unresolved;
            Log("[pvoice] received seq %u kind %u sentence %u/%u %s: not among this game's resident Sam sentences (refused)", s.seq, s.kind, i + 1, s.n, gt);
            continue;
        }
        Log("[pvoice] received seq %u kind %u sentence %u/%u %s: resolved to SentenceResource %p (%s)", s.seq, s.kind, i + 1, s.n, gt,
            reinterpret_cast<void*>(en->sentence), en->text ? "spoken" : "non-verbal");
        Play(*en, "partner line", s.seq, now);
        return;
    }
}
}  // namespace

void OnSpeech(const wire::Speech& s, double now) noexcept {
    (void)now;
    if (!g_enabled) return;
    if (g_pending_n == kPendingCap) {
        ++g_pending_dropped;
        Log("[pvoice] received seq %u dropped: %u frames already wait for the next tick", s.seq, kPendingCap);
        return;
    }
    g_pending[g_pending_n++] = s;
}

void Tick(const Frame& f, double now) noexcept {
    if (!g_enabled) {
        if (g_sub_hooked) DrainSubLog();  // self_voice silencing alone: the subtitle hook's lines
        return;
    }
    g_frame = f;
    audioprobe::NoteVoiceEmitter(reinterpret_cast<std::uintptr_t>(f.emitter));
    selfvoice::NoteEcho(f.emitter);
    DrainSubLog();

    // ---- send: one STS1 per captured talk of the local player
    selfvoice::TalkCapture c;
    while (selfvoice::PopTalk(&c)) {
        ++g_talks;
        wire::Speech s;
        s.seq = ++g_tx_seq;
        s.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now));
        s.flags = g_is_host ? wire::kSpeechFromHost : 0;
        const char* character = selfvoice::Character();
        const bool sam = selfvoice::IsSamCharacter(character) || g_force_sam;
        if (sam) {
            s.kind = static_cast<std::uint8_t>(wire::SpeechKind::played);
            s.n = c.n;
            for (unsigned i = 0; i < c.n; ++i) std::memcpy(s.guid[i], c.guid[i], 16);
        } else {
            s.kind = static_cast<std::uint8_t>(wire::SpeechKind::muted);
            s.n = 0;
        }
        bool sent = false;
        if (f.net) {
            sent = link::SendSpeech(s);
        } else {  // role echo: our own line comes back through the wire format to the echo partner
            std::uint8_t buf[wire::kMaxSpeechFrameSize];
            wire::Speech back;
            const std::size_t n = wire::EncodeSpeech(s, buf, sizeof(buf));
            sent = n && wire::DecodeSpeech(buf, n, back);
            if (sent) OnSpeech(back, now);
        }
        char gt[37] = "-";
        if (c.n) GuidText(c.guid[0], gt);
        if (!sent) {
            ++g_send_fail;
            Log("[pvoice] talk #%u (sentence %s%s) NOT sent: %s", g_talks, gt, c.muted ? ", muted here" : "",
                !f.net ? "encode failed" : link::GetInfo().connected ? "the reliable channel's window is full" : "the link is not connected");
            continue;
        }
        if (sam) {
            ++g_sent_lines;
            Log("[pvoice] sent seq %u kind 0: %u sentence(s), first %s (%p)%s%s%s", s.seq, s.n, gt, reinterpret_cast<void*>(c.sentence[0]),
                c.muted ? "; muted on this game" : "", c.truncated ? "; more entries than 4 (cut)" : "", g_force_sam && !selfvoice::IsSamCharacter(character) ? " (test: sent as Sam)" : "");
        } else {
            ++g_sent_muted;
            Log("[pvoice] sent seq %u kind 1: this player's character '%s' is not Sam, so Sam's line (%s) was not sent%s", s.seq, character, gt,
                c.muted ? " (muted here too)" : "");
        }
    }

    // ---- play what arrived (in order; at most kPlaysPerTick a tick, the rest waits: the frames are reliable, never dropped here)
    const unsigned take = g_pending_n < kPlaysPerTick ? g_pending_n : kPlaysPerTick;
    for (unsigned i = 0; i < take; ++i) Handle(g_pending[i], now);
    for (unsigned i = take; i < g_pending_n; ++i) g_pending[i - take] = g_pending[i];
    g_pending_n -= take;

    // ---- the talking entity follows the partner's head (player_sync places it): logged every 60 s
    if (f.emitter && now >= g_next_track_ms) {
        g_next_track_ms = now + 60000;
        game::WorldTransform ew{}, lw{};
        double head[3] = {};
        const bool e_ok = game::TryGetTransform(f.emitter, &ew);
        const bool h_ok = f.body && realbody::HeadWorld(f.body, head);
        const bool l_ok = f.local && game::TryGetTransform(f.local, &lw);
        const double ep[3] = {ew.pos.x, ew.pos.y, ew.pos.z}, lp[3] = {lw.pos.x, lw.pos.y, lw.pos.z};
        char hb[96] = "no readable head joint";
        if (h_ok) std::snprintf(hb, sizeof(hb), "partner head (%.2f, %.2f, %.2f), %.3f m apart", head[0], head[1], head[2], e_ok ? Dist(ep, head) : -1.0);
        Log("[pvoice] talking entity %p at (%.2f, %.2f, %.2f) | %s | the local player %.2f m away", f.emitter, ew.pos.x, ew.pos.y, ew.pos.z, hb,
            e_ok && l_ok ? Dist(ep, lp) : -1.0);
    }
    if (now >= g_next_counts_ms) {
        g_next_counts_ms = now + 10000;
        const std::uint32_t h = g_talks * 31u + g_rx * 17u + g_rx_played * 7u + g_sub_calls.load() * 3u + g_send_fail;
        if (h != g_logged_counts_hash) {
            g_logged_counts_hash = h;
            LogCounts("counts");
        }
    }
}

bool DebugCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept {
    auto say = [&](const char* fmt, auto... a) { std::snprintf(msg, cap, fmt, a...); };
    if (!g_enabled) {
        say("voice_sync=0: the partner voice is off");
        return false;
    }
    switch (op) {
        case 0:
            LogCounts("status");
            say("sent %u (%u lines, %u muted), received %u, played %u", g_talks, g_sent_lines, g_sent_muted, g_rx, g_rx_played);
            return true;
        case 1: {
            const std::uintptr_t local = reinterpret_cast<std::uintptr_t>(g_frame.local);
            if (!MapReady(local, 1e300)) {
                say("no sentence map (no local player?)");
                return false;
            }
            const double a = v[1];
            const std::size_t n = (std::isfinite(a) && a >= 0 && a < 1e9 ? static_cast<std::size_t>(a) : 0) % g_map.size();
            const bool ok = Play(g_map[n], "test line", 0, 0);
            // The action did what it was asked (tried, with the receiver's checks): a refusal is a result, not a failure.
            say("sentence %zu of %zu: %s", n, g_map.size(), ok ? "PlaySentence accepted it" : "not played (the log says why)");
            return true;
        }
        case 2:
            g_force_sam = v[1] != 0.0;
            Log("[pvoice] test: Sam's lines are %s", g_force_sam ? "sent even though this player is not Sam" : "sent only while this player is Sam");
            say("send as Sam: %s", g_force_sam ? "on (test)" : "off");
            return true;
        case 3: {
            g_map_group = 0, g_map_retry_ms = 0;
            const bool ok = MapReady(reinterpret_cast<std::uintptr_t>(g_frame.local), 1e300);
            say("sentence map: %zu sentences", ok ? g_map.size() : static_cast<std::size_t>(0));
            return ok;
        }
        default:
            say("ops: 0 log, 1 <n> play sentence n here, 2 <0/1> test: send as Sam, 3 rebuild the map");
            return false;
    }
}

}  // namespace st::pvoice

namespace st::game {
bool VoiceSyncCommand(int op, const double (&v)[3], char* msg, std::size_t cap) noexcept { return pvoice::DebugCommand(op, v, msg, cap); }
}  // namespace st::game
