// Story tools' pure part (native/mod/story_rules.hpp): the episode names and labels, the episode hash (CRC-32C as the game's
// crc32 instruction computes it), the episode from the two facts, the mission states, the summary line and its state fold,
// the story-point load rule for every session kind, and the [Slot] text of a checkpoint.dat. No game.
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

#include "story_rules.hpp"

using namespace st::storyrules;
using st::devrules::Route;
using st::devrules::Session;

namespace {
int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

Session S(bool enabled, bool redirected, int role, bool host_save) {
    Session s;
    s.enabled = enabled;
    s.saves_redirected = redirected;
    s.role = role;
    s.host_save = host_save;
    return s;
}

void TestEpisodes() {
    CHECK(std::strcmp(EpisodeName(0), "Prologue: Porter") == 0);
    CHECK(std::strcmp(EpisodeName(2), "Amelie") == 0);
    CHECK(std::strcmp(EpisodeName(15), "Tomorrow is in Your Hands") == 0);
    CHECK(std::strcmp(EpisodeName(16), "?") == 0 && std::strcmp(EpisodeName(-1), "?") == 0);
    char b[24];
    EpisodeLabel(0, b, sizeof(b));
    CHECK(std::strcmp(b, "Prologue") == 0);
    EpisodeLabel(9, b, sizeof(b));
    CHECK(std::strcmp(b, "Episode 9") == 0);
    EpisodeLabel(-1, b, sizeof(b));
    CHECK(std::strcmp(b, "episode unknown") == 0);
}

void TestCrc() {
    // the CRC-32C check value: "123456789" with the usual start ~0 and final inversion is 0xE3069283
    std::uint32_t c = 0xFFFFFFFFu;
    for (const char* p = "123456789"; *p; ++p) c = Crc32cByte(c, static_cast<std::uint8_t>(*p));
    CHECK((c ^ 0xFFFFFFFFu) == 0xE3069283u);
    // the qword step is the eight bytes little-endian (as the crc32 r64 instruction)
    const std::uint64_t v = 0x0807060504030201ull;
    std::uint32_t b = 0x12345678u;
    for (int i = 1; i <= 8; ++i) b = Crc32cByte(b, static_cast<std::uint8_t>(i));
    CHECK(Crc32cU64(0x12345678u, v) == b);
    CHECK(Crc32cU64(0, 0) == 0);  // start 0, no inversion: zero stays zero
    CHECK(EpisodeHash(1, 2) == Crc32cU64(Crc32cU64(0, 1), 2));
    CHECK(EpisodeHash(1, 2) != EpisodeHash(2, 1));
}

void TestResolve() {
    const std::uint32_t h[4] = {0x11u, 0x22u, 0x33u, 0x44u};
    CHECK(ResolveEpisode(3, 0x11u, h, 4) == 3);    // the index fact wins when it holds 0..15
    CHECK(ResolveEpisode(0, 0, h, 4) == 0);
    CHECK(ResolveEpisode(-2, 0x33u, h, 4) == 2);   // -2: SetEpisode / ClearEpisode left it; the hash decides
    CHECK(ResolveEpisode(-2, 0x99u, h, 4) == -1);  // an unknown hash
    CHECK(ResolveEpisode(-2, 0, h, 4) == -1);      // ClearEpisode: hash 0
    CHECK(ResolveEpisode(16, 0x44u, h, 4) == 3);   // out of range index: the hash
    CHECK(ResolveEpisode(-2, 0x44u, nullptr, 4) == -1);
    CHECK(ResolveEpisode(-2, 0x44u, h, 3) == -1);  // only the first `count` hashes count
}

void TestStates() {
    CHECK(std::strcmp(MissionStateName(4), "in progress") == 0);
    CHECK(std::strcmp(MissionStateName(16), "done") == 0);
    CHECK(std::strcmp(MissionStateName(64), "not available") == 0);
    CHECK(std::strcmp(MissionStateName(3), "?") == 0);
    static_assert(kStateProgress == 4 && kStateSuccess == 16, "EDSMissionState Progress / Success");
}

void TestSummary() {
    Summary s;
    char line[320];
    FormatSummary(s, line, sizeof(line));
    CHECK(std::strcmp(line, "no story state (not in a world)") == 0);
    s.valid = true;
    s.episode = 2;
    s.main_total = 48, s.main_done = 4, s.sub_total = 34, s.sub_done = 1, s.highest_done_order = 4, s.main_started = 1;
    AddProgress(s, 60, 5, true);
    AddProgress(s, 65, 6, false);
    s.crc = 0xABCDEF01u;
    FormatSummary(s, line, sizeof(line));
    CHECK(std::strcmp(line, "Episode 2 \"Amelie\" | in progress: Order 5 (main, id 60), Order 6 (sub, id 65) | main done 4/48 (highest Order 4, "
                            "1 started), sub done 1/34 | crc 0xABCDEF01") == 0);
    char small[16];
    CHECK(FormatSummary(s, small, sizeof(small)) == 15);  // truncated, terminated
    for (int i = 0; i < 10; ++i) AddProgress(s, 100 + i, 10 + i, true);
    CHECK(s.progress_count == Summary::kMaxProgress);
    Summary t = s;
    CHECK(SameStory(s, t));
    t.crc ^= 1;
    CHECK(!SameStory(s, t));
    t = s;
    t.main_started = 2;
    CHECK(!SameStory(s, t));
    // the fold: order and values matter (the order state and the attempt's state both)
    CHECK(FoldState(FoldState(0, 10, 4, 16), 20, 2, 4) != FoldState(FoldState(0, 20, 2, 4), 10, 4, 16));
    CHECK(FoldState(0, 10, 4, 16) != FoldState(0, 10, 4, 4));
    CHECK(FoldState(0, 10, 4, 0) != FoldState(0, 10, 2, 0));
    CHECK(std::strcmp(OrderStateName(kOrderCompleted), "completed") == 0 && std::strcmp(OrderStateName(kOrderNotCompleted), "started") == 0);
    CHECK(std::strcmp(OrderStateName(kOrderNotStarted), "not started") == 0 && std::strcmp(OrderStateName(9), "?") == 0);
    Summary p;
    p.valid = true;
    p.episode = 0;
    FormatSummary(p, line, sizeof(line));
    CHECK(std::strncmp(line, "Prologue \"Prologue: Porter\" | in progress: none |", 49) == 0);
}

void TestLoadRule() {
    // the dev tools off: refused with the launcher hint
    CHECK(!DecideLoad(S(false, true, 0, false)).ok());
    CHECK(std::strcmp(DecideLoad(S(false, true, 0, false)).why, st::devrules::kOffReason) == 0);
    // a real save (solo, the online host with host_save=1, a host_save=0 host or guest): refused with the reason
    for (const Session& s : {S(true, false, 0, false), S(true, false, 1, true), S(true, false, 1, false), S(true, false, 2, false)}) {
        const auto v = DecideLoad(s);
        CHECK(!v.ok() && std::strcmp(v.why, kLoadRealReason) == 0);
    }
    // the host-held save session's guest: never (its world is the host's), even sandboxed
    CHECK(std::strcmp(DecideLoad(S(true, true, 2, true)).why, kLoadGuestReason) == 0);
    CHECK(std::strcmp(DecideLoad(S(true, false, 2, true)).why, kLoadGuestReason) == 0);
    // sandboxed: solo / a host_save=0 pair (this world only), the sandboxed host of a host-held save session (the guest follows)
    CHECK(DecideLoad(S(true, true, 0, false)).route == Route::local_only);
    CHECK(DecideLoad(S(true, true, 2, false)).route == Route::local_only);
    CHECK(DecideLoad(S(true, true, 1, false)).route == Route::local_only);
    CHECK(DecideLoad(S(true, true, 1, true)).route == Route::local);
    CHECK(std::strstr(kLoadRealReason, "real save") != nullptr);
    // slots
    CHECK(ValidLoadSlot(0) && ValidLoadSlot(10) && ValidLoadSlot(22));
    CHECK(!ValidLoadSlot(-1) && !ValidLoadSlot(23));
    static_assert(kPackFirstSlot + kPackSlots - 1 <= kMaxManualSlot, "the library's slots are manual slots");
    static_assert(kPackFirstSlot > 6, "never M1's guest slots 5 / 6");
}

std::string Checkpoint(const std::string& ini, bool pad_garbage) {
    char sizes[24] = {};
    std::snprintf(sizes, 8, "%zu", ini.size());
    if (pad_garbage) sizes[5] = 'x';  // seen in real saves: bytes after the digits are not always NUL
    std::snprintf(sizes + 8, 8, "%d", 4);
    std::snprintf(sizes + 16, 8, "%d", 8);
    return std::string(sizes, 24) + ini + "\x89PNG" + std::string("\x1D\x1E\x39\xC0rest", 8);
}

void TestSlotText() {
    const std::string ini =
        "[Slot]\r\nTitle                           = Order In Progress: No. 5 %22Rare Metal Delivery%22 - 02:34:08\r\n"
        "Detail                          = Head west!%0A%0AWaystation West of Capital Knot City\r\nUserParam = #004356D3\r\n"
        "ModificationTime                = 63787658560000000\r\n";
    const std::string f = Checkpoint(ini, false);
    SlotText t;
    CHECK(ParseSlotText(reinterpret_cast<const std::uint8_t*>(f.data()), f.size(), &t));
    CHECK(std::strcmp(t.title, "Order In Progress: No. 5 \"Rare Metal Delivery\" - 02:34:08") == 0);
    CHECK(std::strcmp(t.detail, "Head west! / Waystation West of Capital Knot City") == 0);
    const std::string g = Checkpoint(ini, true);
    CHECK(ParseSlotText(reinterpret_cast<const std::uint8_t*>(g.data()), g.size(), &t));
    // a leading %0A gives no separator; '+' is a space; a bad escape stays literal
    const std::string h = Checkpoint("[Slot]\nTitle = a+b%zz\nDetail = %0A%0ACapital Knot City\n", false);
    CHECK(ParseSlotText(reinterpret_cast<const std::uint8_t*>(h.data()), h.size(), &t));
    CHECK(std::strcmp(t.title, "a b%zz") == 0);
    CHECK(std::strcmp(t.detail, "Capital Knot City") == 0);
    // not a checkpoint: no sizes, no [Slot], too short, no title
    const std::string bad1 = "garbage that is long enough to be checked for a header";
    CHECK(!ParseSlotText(reinterpret_cast<const std::uint8_t*>(bad1.data()), bad1.size(), &t));
    const std::string bad2 = Checkpoint("[Nope]\nTitle = x\n", false);
    CHECK(!ParseSlotText(reinterpret_cast<const std::uint8_t*>(bad2.data()), bad2.size(), &t));
    const std::string bad3 = Checkpoint("[Slot]\nDetail = x\n", false);
    CHECK(!ParseSlotText(reinterpret_cast<const std::uint8_t*>(bad3.data()), bad3.size(), &t));
    CHECK(!ParseSlotText(nullptr, 100, &t));
    CHECK(!ParseSlotText(reinterpret_cast<const std::uint8_t*>(f.data()), 20, &t));
    // a header cut short (only the first bytes read): the title comes out (cut where the bytes end)
    CHECK(ParseSlotText(reinterpret_cast<const std::uint8_t*>(f.data()), 24 + 100, &t));
    // long values are cut, terminated
    const std::string longt = Checkpoint("[Slot]\nTitle = " + std::string(400, 'A') + "\n", false);
    CHECK(ParseSlotText(reinterpret_cast<const std::uint8_t*>(longt.data()), longt.size(), &t));
    CHECK(std::strlen(t.title) == sizeof(t.title) - 1);
}

}  // namespace

int main() {
    TestEpisodes();
    TestCrc();
    TestResolve();
    TestStates();
    TestSummary();
    TestLoadRule();
    TestSlotText();
    std::printf("story_rules_tests: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
