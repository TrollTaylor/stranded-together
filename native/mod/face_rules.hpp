// Fragile's face (docs/real-partner-body.md section 36), the pure part: the wire ids of Sam's facial clips, the choice
// of the clip Sam's facial manager wants, and when a body's face is given a clip. No game memory, no Windows:
// unit-tested in tests/face_rules_tests.cpp.
//
// Sam's face is a facial rig driven by his DSPlayerFacialComponent (a DSFacialRigManagerComponent on the player). Each
// frame it picks a state (idle, tired, ...), a Morpheme event clip (rest, sleep, emotions, ...), a private-room gesture
// (the override) or a photo-mode clip, and plays it through DSFacialRigComponent::PlayFacial (0x1422CC710). A demo body
// (her self view, the partner body) has the rig but no manager: nothing plays and her face holds its default pose. Both
// heads use one rig skeleton (skeleton_face_r) with their own mapping, so Sam's clips play on her face as they are.
//
// The manager (DSFacialRigManagerComponent, 0x70 bytes) keeps:
//   +0x30 its resource (DSPlayerFacialComponentResource: +0x20 the DSFacialManagerResource, +0x28 / +0x30 the 18
//         photo-mode clips), +0x48 its entity, +0x50 the override clip, +0x5C photo mode on, +0x5E / +0x5F the
//         override's blink / saccade, +0x60 the event clip's key (CRC-32C of the clip pointer), +0x64 the state index,
//         +0x68 flags (1 an event clip plays, 2 the override plays).
// The DSFacialManagerResource: AnimationEventInfo count +0x20 / data +0x28 (0x28 bytes each: clip +8, start +0x10,
// blend +0x14, blink +0x18, saccade +0x19, EventIndex +0x20); AnimationInfo count +0x30 / data +0x38 (0x18 bytes each:
// clip +0, start +8, blend +0xC, saccade +0x10, blink +0x11). (The blink / saccade names follow PlayFacial's 5th and
// 6th arguments, which set the rig's +0x573 bits 0x10 and 0x08: 0x1422D1510.)
#pragma once
#include <cstdint>

namespace st::facerules {

// ---------------------------------------------------------------- the wire id of a clip (STJ1 face block)
// The sender maps the clip to its place in its own Sam's tables, the receiver plays the clip at that place in its own
// Sam's (the same game data on both sides).
inline constexpr std::uint8_t kStateBase = 0, kStateSlots = 16;   // AnimationInfo 0..15 (the game has 12)
inline constexpr std::uint8_t kEventBase = 16, kEventSlots = 96;  // AnimationEventInfo 0..95 -> 16..111 (83)
inline constexpr std::uint8_t kPhotoBase = 112, kPhotoSlots = 32; // the photo-mode clips 0..31 -> 112..143 (18)
inline constexpr std::uint8_t kIdNone = 0xFE;     // Sam's face plays nothing
inline constexpr std::uint8_t kIdUnknown = 0xFF;  // a clip in none of the tables (a private-room gesture): keep the last
enum class Table : std::uint8_t { state, event, photo, none, unknown };

inline const char* TableName(Table t) {
    return t == Table::state ? "state" : t == Table::event ? "event" : t == Table::photo ? "photo" : t == Table::none ? "none" : "unknown";
}

inline std::uint8_t MakeId(Table t, unsigned index) {
    if (t == Table::state) return index < kStateSlots ? static_cast<std::uint8_t>(kStateBase + index) : kIdUnknown;
    if (t == Table::event) return index < kEventSlots ? static_cast<std::uint8_t>(kEventBase + index) : kIdUnknown;
    if (t == Table::photo) return index < kPhotoSlots ? static_cast<std::uint8_t>(kPhotoBase + index) : kIdUnknown;
    return t == Table::none ? kIdNone : kIdUnknown;
}

inline Table SplitId(std::uint8_t id, unsigned* index) {
    unsigned i = 0;
    Table t = Table::unknown;
    if (id < kEventBase) t = Table::state, i = id - kStateBase;
    else if (id < kPhotoBase) t = Table::event, i = id - kEventBase;
    else if (id < kPhotoBase + kPhotoSlots) t = Table::photo, i = id - kPhotoBase;
    else if (id == kIdNone) t = Table::none;
    if (index) *index = i;
    return t;
}

// The id a receiver plays: an unknown clip (or a reserved id) keeps the last one.
inline std::uint8_t KeepId(std::uint8_t last, std::uint8_t rx) {
    unsigned i = 0;
    return SplitId(rx, &i) == Table::unknown ? last : rx;
}

// The face block's flags byte: PlayFacial's needBlink / needSaccade.
inline constexpr std::uint8_t kFlagBlink = 1, kFlagSaccade = 2, kFlagsKnown = 3;
inline std::uint8_t MakeFlags(bool blink, bool saccade) {
    return static_cast<std::uint8_t>((blink ? kFlagBlink : 0) | (saccade ? kFlagSaccade : 0));
}
// The rig's own +0x573 bits (0x10 blink, 0x08 saccade) as the block's flags.
inline std::uint8_t FlagsFromRig(std::uint8_t rig) { return MakeFlags((rig & 0x10) != 0, (rig & 0x08) != 0); }

// ---------------------------------------------------------------- the clip Sam's manager wants
inline constexpr std::uint8_t kMgrEvent = 1, kMgrOverride = 2;
enum class Source : std::uint8_t { state, event, override_, rig };
inline const char* SourceName(Source s) {
    return s == Source::state ? "state" : s == Source::event ? "event" : s == Source::override_ ? "override" : "rig";
}

// Photo mode plays its clip past the manager (0x1422D2E20, only the saved flag +0x5C): then the rig's own slot says
// what plays. Else the override (a private-room gesture, 0x1422D2F10), the event clip (0x1422D2A50) or the state
// (0x1422D2D70), in the order the game's Apply / Restore (0x1422D2C00) checks them.
inline Source PickSource(std::uint8_t mgr_flags, bool photo_saved) {
    if (photo_saved) return Source::rig;
    if (mgr_flags & kMgrOverride) return Source::override_;
    if (mgr_flags & kMgrEvent) return Source::event;
    return Source::state;
}

// The state index the game uses (0x1422D2D70 clamps a negative one to 0); -1 out of the table.
inline int StateIndex(int index, int count) {
    if (index < 0) index = 0;
    return index < count ? index : -1;
}

// CRC-32C of a 64-bit value with no pre / post inversion: the SSE4.2 `crc32 r64, m64` the game keys its event clips
// with (0x1422D2AA2, init 0).
inline std::uint32_t Crc32cU64(std::uint32_t crc, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        crc ^= static_cast<std::uint8_t>(v >> (8 * i));
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
    return crc;
}

// ---------------------------------------------------------------- when a body's face gets a clip
// A face is given the clip once per change (the game itself calls PlayFacial only on a change: a call restarts the
// clip and its blend), again when the body or its showing changes, and again (at most kMaxTries times per clip) when
// the clip did not land in the body's rig within kRetryMs (the rig was not ready, or the entity at that address is
// another one now). Calls are kMinGapMs apart at least, so a flapping manager cannot flood the rig.
inline constexpr double kMinGapMs = 50;
inline constexpr double kRetryMs = 1000;
inline constexpr int kMaxTries = 3;

struct Played {
    std::uintptr_t body = 0;   // the rig component given the clip
    std::uint32_t epoch = 0;   // the body's showing (a new show replays)
    std::uintptr_t clip = 0;
    std::uint8_t flags = 0;
    double at_ms = -1e18;
    int tries = 0;
    bool took = false;         // the clip was seen in the body's rig after the call
};

enum class Act : std::uint8_t { none, play, retry, wait };

// took_now: the body's rig holds `clip` now.
inline Act Decide(const Played& p, std::uintptr_t body, std::uint32_t epoch, std::uintptr_t clip, std::uint8_t flags, bool took_now, double now_ms) {
    if (!body || !clip) return Act::none;
    const bool changed = body != p.body || epoch != p.epoch || clip != p.clip || flags != p.flags;
    if (changed) return now_ms - p.at_ms < kMinGapMs ? Act::wait : Act::play;
    if (p.took || took_now) return Act::none;
    if (p.tries >= kMaxTries) return Act::none;
    return now_ms - p.at_ms >= kRetryMs ? Act::retry : Act::none;
}

// After a call (ok: it returned).
inline void NotePlayed(Played& p, std::uintptr_t body, std::uint32_t epoch, std::uintptr_t clip, std::uint8_t flags, double now_ms, bool retry) {
    p.tries = retry ? p.tries + 1 : 1;
    p.body = body, p.epoch = epoch, p.clip = clip, p.flags = flags, p.at_ms = now_ms;
    p.took = false;
}

// The first time the clip is seen in the body's rig after a call: true (log it once).
inline bool NoteTook(Played& p, bool took_now) {
    if (p.took || !took_now || !p.clip) return false;
    p.took = true;
    return true;
}

}  // namespace st::facerules
