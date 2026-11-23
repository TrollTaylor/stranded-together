// Fragile's face, the pure part (native/mod/face_rules.hpp): the wire ids of Sam's facial clips, the manager's choice of
// clip, the event key (CRC-32C of the clip pointer, as the game's crc32 instruction) and when a body's face gets a clip.
// No game.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <nmmintrin.h>

#include "face_rules.hpp"

using namespace st::facerules;

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

void TestIds() {
    unsigned i = 99;
    CHECK(MakeId(Table::state, 0) == 0 && MakeId(Table::state, 11) == 11 && MakeId(Table::state, 15) == 15);
    CHECK(MakeId(Table::state, 16) == kIdUnknown);
    CHECK(MakeId(Table::event, 0) == 16 && MakeId(Table::event, 82) == 98 && MakeId(Table::event, 95) == 111);
    CHECK(MakeId(Table::event, 96) == kIdUnknown);
    CHECK(MakeId(Table::photo, 0) == 112 && MakeId(Table::photo, 17) == 129 && MakeId(Table::photo, 31) == 143);
    CHECK(MakeId(Table::photo, 32) == kIdUnknown);
    CHECK(MakeId(Table::none, 0) == kIdNone && MakeId(Table::unknown, 0) == kIdUnknown);
    // every id of every table splits back to its table and index
    for (unsigned k = 0; k < kStateSlots; ++k) CHECK(SplitId(MakeId(Table::state, k), &i) == Table::state && i == k);
    for (unsigned k = 0; k < kEventSlots; ++k) CHECK(SplitId(MakeId(Table::event, k), &i) == Table::event && i == k);
    for (unsigned k = 0; k < kPhotoSlots; ++k) CHECK(SplitId(MakeId(Table::photo, k), &i) == Table::photo && i == k);
    CHECK(SplitId(kIdNone, &i) == Table::none);
    CHECK(SplitId(kIdUnknown, &i) == Table::unknown);
    CHECK(SplitId(144, &i) == Table::unknown && SplitId(200, &i) == Table::unknown && SplitId(0xFD, nullptr) == Table::unknown);
    // the receiver keeps the last id for an unknown one (a private-room gesture) or a reserved one
    CHECK(KeepId(5, kIdUnknown) == 5);
    CHECK(KeepId(5, 200) == 5);
    CHECK(KeepId(5, 17) == 17);
    CHECK(KeepId(5, kIdNone) == kIdNone);
    CHECK(KeepId(kIdNone, kIdUnknown) == kIdNone);
    CHECK(std::strcmp(TableName(Table::event), "event") == 0 && std::strcmp(TableName(Table::unknown), "unknown") == 0);
}

void TestFlags() {
    CHECK(MakeFlags(false, false) == 0 && MakeFlags(true, false) == kFlagBlink && MakeFlags(false, true) == kFlagSaccade);
    CHECK(MakeFlags(true, true) == kFlagsKnown);
    // the rig's +0x573: 0x10 blink, 0x08 saccade (other bits ignored)
    CHECK(FlagsFromRig(0x10) == kFlagBlink && FlagsFromRig(0x08) == kFlagSaccade && FlagsFromRig(0x18) == 3);
    CHECK(FlagsFromRig(0xE7) == 0 && FlagsFromRig(0xFF) == 3);
}

void TestSource() {
    CHECK(PickSource(0, false) == Source::state);
    CHECK(PickSource(kMgrEvent, false) == Source::event);
    CHECK(PickSource(kMgrOverride, false) == Source::override_);
    CHECK(PickSource(kMgrOverride | kMgrEvent, false) == Source::override_);  // the override wins (Apply checks it first)
    CHECK(PickSource(0, true) == Source::rig);                                  // photo mode plays past the manager
    CHECK(PickSource(kMgrEvent | kMgrOverride, true) == Source::rig);
    CHECK(StateIndex(0, 12) == 0 && StateIndex(11, 12) == 11);
    CHECK(StateIndex(-3, 12) == 0);  // the game clamps a negative index to 0
    CHECK(StateIndex(12, 12) == -1 && StateIndex(400, 12) == -1);
    CHECK(StateIndex(0, 0) == -1);
    CHECK(std::strcmp(SourceName(Source::override_), "override") == 0);
}

void TestCrc() {
    // the game's key: `crc32 r9, qword [clip]` with r9 = 0 (SSE4.2, CRC-32C, no inversion)
    const std::uint64_t v[] = {0, 1, 0x0000020A12345678ull, 0xFFFFFFFFFFFFFFFFull, 0x00007FF6A1B2C3D0ull, 0x8000000000000001ull};
    for (std::uint64_t x : v) CHECK(Crc32cU64(0, x) == static_cast<std::uint32_t>(_mm_crc32_u64(0, x)));
    CHECK(Crc32cU64(0x12345678u, 42) == static_cast<std::uint32_t>(_mm_crc32_u64(0x12345678u, 42)));
    CHECK(Crc32cU64(0, 0) == 0);
    CHECK(Crc32cU64(0, 0x1000) != Crc32cU64(0, 0x1008));
}

void TestDecide() {
    Played p;
    const std::uintptr_t body = 0x1000, clip = 0x2000, clip2 = 0x3000;
    // nothing to drive
    CHECK(Decide(p, 0, 1, clip, 3, false, 0) == Act::none);
    CHECK(Decide(p, body, 1, 0, 3, false, 0) == Act::none);
    // first time: play
    CHECK(Decide(p, body, 1, clip, 3, false, 0) == Act::play);
    NotePlayed(p, body, 1, clip, 3, 0, false);
    CHECK(p.tries == 1 && !p.took && p.clip == clip);
    // the same clip: nothing, and no retry before 1 s
    CHECK(Decide(p, body, 1, clip, 3, false, 500) == Act::none);
    // it never landed: retried after 1 s, at most 3 calls per clip
    CHECK(Decide(p, body, 1, clip, 3, false, 1000) == Act::retry);
    NotePlayed(p, body, 1, clip, 3, 1000, true);
    CHECK(p.tries == 2);
    CHECK(Decide(p, body, 1, clip, 3, false, 1500) == Act::none);
    CHECK(Decide(p, body, 1, clip, 3, false, 2000) == Act::retry);
    NotePlayed(p, body, 1, clip, 3, 2000, true);
    CHECK(p.tries == 3);
    CHECK(Decide(p, body, 1, clip, 3, false, 9000) == Act::none);  // given up for this clip
    // a new clip plays again (tries reset)
    CHECK(Decide(p, body, 1, clip2, 3, false, 9000) == Act::play);
    NotePlayed(p, body, 1, clip2, 3, 9000, false);
    CHECK(p.tries == 1);
    // it landed: noted once, never retried
    CHECK(NoteTook(p, true));
    CHECK(!NoteTook(p, true));
    CHECK(p.took);
    CHECK(Decide(p, body, 1, clip2, 3, false, 20000) == Act::none);
    // the flags changed (blink off): play
    CHECK(Decide(p, body, 1, clip2, 1, true, 20000) == Act::play);
    // a new showing of the body, or another body: play
    CHECK(Decide(p, body, 2, clip2, 3, true, 20000) == Act::play);
    CHECK(Decide(p, 0x1100, 1, clip2, 3, true, 20000) == Act::play);
    // a change within 50 ms of the last call waits (a flapping manager cannot flood the rig)
    NotePlayed(p, body, 2, clip2, 3, 30000, false);
    CHECK(Decide(p, body, 2, clip, 3, false, 30020) == Act::wait);
    CHECK(Decide(p, body, 2, clip, 3, false, 30050) == Act::play);
    // already in the rig before we noticed: no retry
    Played q;
    NotePlayed(q, body, 1, clip, 0, 0, false);
    CHECK(Decide(q, body, 1, clip, 0, true, 5000) == Act::none);
    CHECK(!NoteTook(q, false));
    // NoteTook without a call
    Played r;
    CHECK(!NoteTook(r, true));
}
}  // namespace

int main() {
    TestIds();
    TestFlags();
    TestSource();
    TestCrc();
    TestDecide();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
