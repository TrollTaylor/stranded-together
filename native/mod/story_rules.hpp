// Story tools (docs/dev-tools.md "Story tools"), the pure part: the episodes, the mission states, the episode hash the game
// keeps in its CurrentEpisodeHashFact, the summary line, the [Slot] text of a save, and which session may load a story
// point. No game memory, no Windows: unit-tested in tests/story_rules_tests.cpp. The game side is story_state.cpp.
//
// The rule (a hard one): reading the story state is allowed everywhere the dev tools are on. Loading a story point
// replaces the world a save keeps, so it runs only in a game whose saves are sandboxed (the launcher's Local test with a
// test save, a hidden run), never in a session that plays on a real save, and never on a host-held save session's guest
// (the host's game holds the world: the host loads, M1 brings the guest along).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "dev_rules.hpp"

namespace st::storyrules {

// ---------------------------------------------------------------- episodes (ds/mission/system/mission_goals.core Episodes[])
inline constexpr int kEpisodeCount = 16;  // 0 = the prologue, 1..15
inline const char* EpisodeName(int i) noexcept {
    static const char* const kNames[kEpisodeCount] = {
        "Prologue: Porter", "Bridget",  "Amelie",   "Fragile",        "Unger",   "Mama",      "Deadman", "Clifford",
        "Heartman",         "Higgs",    "Die-Hardman", "Clifford Unger", "Bridges", "Sam Strand", "Lou",   "Tomorrow is in Your Hands",
    };
    return i >= 0 && i < kEpisodeCount ? kNames[i] : "?";
}
// "Prologue" / "Episode 2" (the short label of the page and the log).
inline void EpisodeLabel(int i, char* out, std::size_t cap) noexcept {
    if (!out || !cap) return;
    if (i == 0) std::snprintf(out, cap, "Prologue");
    else if (i > 0 && i < kEpisodeCount) std::snprintf(out, cap, "Episode %d", i);
    else std::snprintf(out, cap, "episode unknown");
}

// The value the game writes into CurrentEpisodeHashFact for an episode resource (SetEpisode 0x14335F3A0, read back by the
// getter 0x14333C820): the SSE4.2 crc32 instruction (CRC-32C, reflected 0x82F63B78) over the GUID's two little-endian
// qwords, start value 0, no final inversion.
inline std::uint32_t Crc32cByte(std::uint32_t crc, std::uint8_t b) noexcept {
    crc ^= b;
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    return crc;
}
inline std::uint32_t Crc32cU64(std::uint32_t crc, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) crc = Crc32cByte(crc, static_cast<std::uint8_t>(v >> (8 * i)));
    return crc;
}
inline std::uint32_t EpisodeHash(std::uint64_t guid_lo, std::uint64_t guid_hi) noexcept { return Crc32cU64(Crc32cU64(0, guid_lo), guid_hi); }

// The episode from the two facts: the index fact when it holds 0..15; -2 (SetEpisode / ClearEpisode leave it so) or
// anything else: the hash fact matched against the Episodes array's hashes. -1 when neither says.
inline int ResolveEpisode(std::int32_t index_fact, std::uint32_t hash_fact, const std::uint32_t* hashes, int count) noexcept {
    if (index_fact >= 0 && index_fact < kEpisodeCount) return index_fact;
    if (!hash_fact || !hashes) return -1;
    for (int i = 0; i < count && i < kEpisodeCount; ++i)
        if (hashes[i] == hash_fact) return i;
    return -1;
}

// ---------------------------------------------------------------- missions (DSMissionInfo +0x2C, EDSMissionState)
inline const char* MissionStateName(std::uint16_t s) noexcept {
    switch (s) {
        case 0: return "invalid";
        case 1: return "available";
        case 2: return "ready";
        case 4: return "in progress";
        case 8: return "failed";
        case 16: return "done";
        case 32: return "cancelled";
        case 64: return "not available";
        case 128: return "returned";
        case 256: return "used";
        case 512: return "missing";
        case 1024: return "consigned";
        case 2048: return "complete (automation)";
        default: return "?";
    }
}
inline constexpr std::uint16_t kStateProgress = 4, kStateSuccess = 16;
// DSMissionInfo +0x2A: EDSMissionOrderState, the order's story progress (CountCompletedMissions 0x1433CBD90 counts == 4).
// +0x2C above is the current attempt's state (Progress while the order is being played).
inline constexpr std::uint8_t kOrderNotStarted = 1, kOrderNotCompleted = 2, kOrderCompleted = 4;
inline const char* OrderStateName(std::uint8_t s) noexcept {
    switch (s) {
        case 0: return "none";
        case kOrderNotStarted: return "not started";
        case kOrderNotCompleted: return "started";
        case kOrderCompleted: return "completed";
        default: return "?";
    }
}

// What future story sync can compare: one per game, read by story_state.cpp. `crc` folds every (MissionId, order state,
// state) of the game's main/sub list in its order, so two games at the same story point have the same value.
struct Summary {
    bool valid = false;
    int episode = -1;                 // 0 prologue .. 15; -1 unknown
    std::int32_t index_fact = -3;     // CurrentEpisodeIndexFact as read (-2: the hash fact decides)
    std::uint32_t hash_fact = 0;      // CurrentEpisodeHashFact as read
    int main_total = 0, main_done = 0, sub_total = 0, sub_done = 0;  // done = order state completed
    int main_started = 0;             // main orders started, not completed (order state 2)
    int available = 0;                // missions in state available / ready (+0x2C 1 / 2)
    int highest_done_order = 0;       // the highest Order No. of a completed main order
    static constexpr int kMaxProgress = 6;
    int progress_count = 0;           // missions in progress (main and sub)
    int progress_order[kMaxProgress] = {};
    std::uint64_t progress_id[kMaxProgress] = {};
    bool progress_main[kMaxProgress] = {};
    std::uint32_t crc = 0;
};
inline std::uint32_t FoldState(std::uint32_t crc, std::uint64_t id, std::uint8_t order_state, std::uint16_t state) noexcept {
    return Crc32cU64(Crc32cU64(crc, id), (static_cast<std::uint64_t>(order_state) << 16) | state);
}
inline void AddProgress(Summary& s, std::uint64_t id, int order, bool main) noexcept {
    if (s.progress_count >= Summary::kMaxProgress) return;
    s.progress_id[s.progress_count] = id;
    s.progress_order[s.progress_count] = order;
    s.progress_main[s.progress_count] = main;
    ++s.progress_count;
}
inline bool SameStory(const Summary& a, const Summary& b) noexcept {
    return a.valid == b.valid && a.episode == b.episode && a.crc == b.crc && a.main_done == b.main_done && a.sub_done == b.sub_done &&
           a.main_started == b.main_started && a.progress_count == b.progress_count;
}

// One line: Episode 2 "Amelie" | in progress: Order 5 (main, id 60) | main done 4/48 (highest Order 4, 1 started), sub done 0/34 |
// crc 0x...
inline std::size_t FormatSummary(const Summary& s, char* out, std::size_t cap) noexcept {
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!s.valid) {
        std::snprintf(out, cap, "no story state (not in a world)");
        return std::strlen(out);
    }
    char ep[24];
    EpisodeLabel(s.episode, ep, sizeof(ep));
    char prog[200] = {};
    std::size_t p = 0;
    for (int i = 0; i < s.progress_count && p + 1 < sizeof(prog); ++i) {
        const int k = std::snprintf(prog + p, sizeof(prog) - p, "%sOrder %d (%s, id %llu)", i ? ", " : "", s.progress_order[i],
                                    s.progress_main[i] ? "main" : "sub", static_cast<unsigned long long>(s.progress_id[i]));
        if (k < 0) break;
        p += static_cast<std::size_t>(k) < sizeof(prog) - p ? static_cast<std::size_t>(k) : sizeof(prog) - p - 1;
    }
    std::snprintf(out, cap, "%s \"%s\" | in progress: %s | main done %d/%d (highest Order %d, %d started), sub done %d/%d | crc 0x%08X", ep,
                  EpisodeName(s.episode), s.progress_count ? prog : "none", s.main_done, s.main_total, s.highest_done_order, s.main_started,
                  s.sub_done, s.sub_total, s.crc);
    return std::strlen(out);
}

// ---------------------------------------------------------------- story points (a save library, docs/dev-tools.md)
// tools/testenv/Story-Library.ps1 -Pack writes library entry NN as manualsave(10 + NN) of a seed folder (testenv-saves\
// story-pack): the hidden runs' -SeedSaves and the launcher's Local test test save. M1's guest uses manualsave5 / 6.
inline constexpr int kPackFirstSlot = 10, kPackSlots = 10;
inline constexpr int kMaxManualSlot = 22;  // the game's manual slots: manualsave0 .. manualsave22
inline bool ValidLoadSlot(int slot) noexcept { return slot >= 0 && slot <= kMaxManualSlot; }

inline constexpr const char* kLoadRealReason =
    "refused: this game plays on its real save; loading a story point would replace your progress at the next autosave (use the "
    "launcher's Local test with a test save, or a hidden run)";
inline constexpr const char* kLoadGuestReason =
    "refused on the guest: the host's game holds the world (load the story point on the host; its load comes to you)";

inline devrules::Verdict DecideLoad(const devrules::Session& s) noexcept {
    devrules::Verdict v;
    if (!s.enabled) return v.why = devrules::kOffReason, v;
    if (s.role == 2 && s.host_save) return v.why = kLoadGuestReason, v;
    if (!s.saves_redirected) return v.why = kLoadRealReason, v;
    v.route = s.role == 1 && s.host_save ? devrules::Route::local : devrules::Route::local_only;
    return v;
}

// ---------------------------------------------------------------- the [Slot] text of a checkpoint.dat
// Three 8-byte decimal sizes (ini, PNG, data), then the ini ("Key = Value" lines, values URL-encoded: %22, %0A).
struct SlotText {
    char title[128] = {};
    char detail[128] = {};
};
// %XX and '+' decoded; control characters (the %0A line breaks) become " / " between non-empty parts.
inline void UrlDecodeLine(const char* in, std::size_t n, char* out, std::size_t cap) noexcept {
    if (!out || !cap) return;
    std::size_t o = 0;
    bool pending_sep = false;
    auto hex = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (std::size_t i = 0; i < n && in[i]; ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '%' && i + 2 < n && hex(in[i + 1]) >= 0 && hex(in[i + 2]) >= 0) {
            c = static_cast<unsigned char>(hex(in[i + 1]) * 16 + hex(in[i + 2]));
            i += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (c < 0x20) {
            pending_sep = o > 0;
            continue;
        }
        if (c > 0x7E) c = '?';
        if (pending_sep) {
            for (const char* s = " / "; *s && o + 1 < cap; ++s) out[o++] = *s;
            pending_sep = false;
        }
        if (o + 1 < cap) out[o++] = static_cast<char>(c);
    }
    out[o] = 0;
}
// false when the bytes are not a checkpoint.dat header (the sizes, the "[Slot]" line).
inline bool ParseSlotText(const std::uint8_t* data, std::size_t size, SlotText* out) noexcept {
    if (!data || !out || size < 24 + 6) return false;
    *out = SlotText{};
    std::size_t ini = 0;
    for (int i = 0; i < 8 && data[i] >= '0' && data[i] <= '9'; ++i) ini = ini * 10 + (data[i] - '0');
    if (ini < 6 || ini > 64 * 1024) return false;
    const char* p = reinterpret_cast<const char*>(data + 24);
    const std::size_t n = ini < size - 24 ? ini : size - 24;
    if (n < 6 || std::memcmp(p, "[Slot]", 6) != 0) return false;
    for (std::size_t i = 0; i < n;) {
        std::size_t e = i;
        while (e < n && p[e] != '\n' && p[e] != '\r') ++e;
        const char* line = p + i;
        const std::size_t len = e - i;
        const char* eq = static_cast<const char*>(std::memchr(line, '=', len));
        if (eq) {
            std::size_t klen = static_cast<std::size_t>(eq - line);
            while (klen && line[klen - 1] == ' ') --klen;
            const char* val = eq + 1;
            while (val < line + len && *val == ' ') ++val;
            const std::size_t vlen = static_cast<std::size_t>(line + len - val);
            if (klen == 5 && std::memcmp(line, "Title", 5) == 0) UrlDecodeLine(val, vlen, out->title, sizeof(out->title));
            else if (klen == 6 && std::memcmp(line, "Detail", 6) == 0) UrlDecodeLine(val, vlen, out->detail, sizeof(out->detail));
        }
        i = e + 1;
    }
    return out->title[0] != 0;
}

}  // namespace st::storyrules
