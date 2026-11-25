// Enemy sync P1a (docs/enemy-sync.md 8): the two unreliable enemy frames, host -> guest. The pure part: their codecs and strict
// decoders. No game, no link: unit-tested in tests/enemy_wire_tests.cpp (st_enemy_wire_tests).
//
// SEN1, the enemies' places and states (all little-endian; one FRAME message in a datagram of its own):
//   header 20 B: 'SEN1' | u8 version 1 | u8 n (1..32) | u16 flags (bit0: the last frame of a full pass; 0 in P1a) | u32 seq |
//                u32 sender_ms (the sender's tick clock) | u32 gen_tag (0)
//   entry 28 B:
//      0 u32 actor_id (an enemy id: enemy::IsHumanEnemyId)
//      4 u8  owner_epoch (1)
//      5 u8  state: bit0 realized on the sender, bit1 dead, bits2-4 the defeat (P2: erules::Defeat, the OUTCOME's; 0 none), bit5 the sender sends a pose (SEP1),
//                   bit6 hidden on the sender, bit7 a real sequence holds it on the sender
//      6 u8  phase (0xFF in P1a)
//      7 u8  flags: bit0 moving (|v| > 0.2 m/s), bit1 owner is the guest (0 in P1a), bit2 damaged, bit3 in a vehicle (round 23 B; bits
//                   4-7 refused)
//      8 i32 pos[3] mm (the entity's place when realized, else the actor's +0x28)
//     20 u16 yaw (2^16 a turn, atan2 of the forward row, as shotw::YawOf)
//     22 i16 vel[3] cm/s (clamped to +-327 m/s)
//
// SEP1, one enemy's pose (one frame per datagram): 16 + the STJ1 bytes (wire::EncodeJoints; 77 joints, layout BE39C83F, root and
// pelvis translations, no outfit, no face):
//   'SEP1' | u8 version 1 | u8 slot (0..7) | u16 0 | u32 actor_id | u32 sender_ms | STJ1
#pragma once

#include <cstddef>
#include <cstdint>

#include "pose_wire.hpp"

namespace st::ewire {

inline constexpr std::uint8_t kVersion = 1;

// ---------------------------------------------------------------- SEN1
inline constexpr std::size_t kSenHeader = 20, kSenEntry = 28;
inline constexpr unsigned kSenMax = 32;
inline constexpr std::size_t kSenMaxSize = kSenHeader + kSenMax * kSenEntry;  // 916 B
inline constexpr double kMaxCoordM = 2.0e6;                                    // +-2000 km
inline constexpr double kMaxSpeedMs = 327.0;

enum StateBit : std::uint8_t { kRealized = 1, kDead = 2, kPosed = 0x20, kHidden = 0x40, kSeqHeld = 0x80 };
inline constexpr unsigned kDefeatShift = 2;
inline constexpr std::uint8_t kDefeatMask = 0x1C;  // bits 2-4
inline constexpr std::uint8_t DefeatBits(std::uint8_t defeat) noexcept { return static_cast<std::uint8_t>((defeat & 7u) << kDefeatShift); }
inline constexpr std::uint8_t DefeatOfState(std::uint8_t state) noexcept { return static_cast<std::uint8_t>((state & kDefeatMask) >> kDefeatShift); }
// A SEN1 state byte as an erules::Defeat: dead (4) by kDead, else its defeat bits (round 23: the replay flush, the tie-up watch).
inline constexpr std::uint8_t SenDefeat(std::uint8_t state) noexcept { return (state & kDead) ? std::uint8_t{4} : DefeatOfState(state); }
enum EntryFlag : std::uint8_t { kMoving = 1, kOwnerGuest = 2, kDamaged = 4, kInVehicle = 8 };
// Round 23 B: kInVehicle, the sender's copy sits in a vehicle (the parent chain or the ride fields; erules::VehicleTest), sent by
// both games for their own enemies. An older decoder ignores it. The decoder refuses any flag bit above it (0xF0).
inline constexpr std::uint8_t kEntryFlagsKnown = 0x0F;
// P1b (docs/enemy-sync.md 10): the header flags. bit1 the sender is the guest (her owned enemies), bit2 the sender is in a
// cutscene. gen_tag: the host's combat generation (the guest echoes the newest she saw). The entry's owner_epoch is the low byte
// of the owner epoch, its phase the owner's +0x110 (0..5, 0xFF unknown), flags kOwnerGuest on her entries, kDamaged a hit since
// the last owner change.
enum FrameFlag : std::uint16_t { kFrameLast = 1, kFromGuest = 2, kSenderCutscene = 4 };

struct Enemy {
    std::uint32_t actor_id = 0;
    std::uint8_t owner_epoch = 1;
    std::uint8_t state = 0;
    std::uint8_t phase = 0xFF;
    std::uint8_t flags = 0;
    double pos[3] = {};  // m, world
    float yaw = 0;       // rad, [-pi, pi)
    float vel[3] = {};   // m/s
};
struct EnemyFrame {
    std::uint16_t flags = 0;
    std::uint32_t seq = 0, sender_ms = 0, gen_tag = 0;
    std::uint8_t n = 0;
    Enemy e[kSenMax];
};
// 0 when it does not encode (n out of range, an entry that would not decode).
std::size_t EncodeEnemies(const EnemyFrame& f, std::uint8_t* out, std::size_t cap) noexcept;
// Strict: the magic, the version, n in 1..32, the exact size, every entry (an enemy actor id, |pos| < 2000 km). One bad entry
// rejects the frame.
bool DecodeEnemies(const std::uint8_t* d, std::size_t n, EnemyFrame& out) noexcept;

// The yaw on the wire: 2^16 a turn.
std::uint16_t YawToU16(float yaw) noexcept;
float YawFromU16(std::uint16_t u) noexcept;

// ---------------------------------------------------------------- SEP1
inline constexpr std::size_t kSepHeader = 16;
inline constexpr unsigned kSepSlots = 8;
inline constexpr std::size_t kSepMaxSize = kSepHeader + wire::kMaxJointFrameSize;
struct EnemyPose {
    std::uint8_t slot = 0;
    std::uint32_t actor_id = 0;
    std::uint32_t sender_ms = 0;
    wire::JointFrame joints;
    bool foreign_layout = false;  // decoded, but not the shared human layout (the receiver refuses it)
};
// 0 when it does not encode (slot out of range, not an enemy id, the joints do not encode).
std::size_t EncodeEnemyPose(const EnemyPose& p, std::uint8_t* out, std::size_t cap) noexcept;

// ---------------------------------------------------------------- SCB1 (P2, round 17; docs/enemy-sync.md 9)
// The reliable combat channel 6 (link::SendCombat, 1..1024 B, in order, exactly once a link session). Little-endian.
//   header 16 B: 'SCB1' | u8 type | u8 version 1 | u8 n | u8 0 | u32 guids_fp (the sender's enemy guids fingerprint: the world
//                both games share; a mismatch is refused `world`) | u32 seq (the sender's message count)
//   type 4 HIT (guest -> host), n 1..12, 80 B each:
//      0 u32 key (an enemy actor id)   4 u32 hit_seq   8 u16 attack (1..0x18D, never 0x180)   10 u8 kind (1 bullet, 2 sphere / job,
//      3 direct, 4 test, 5 restrain: round 23, below)   11 u8 flags (kHit*)   12 u32 serial (P+0x08)   16 u8 b12E (P+0x0E)   17 u8 b180 (P+0x60)   18 u8 depth
//      (in the +0x70 chain)   19 u8 0   20 u32 v130 (P+0x10)   24 u32 v184 (P+0x64, the power's f32 bits)   28 u32 v188 (P+0x68)
//      32 u32 part (DI+0x68; 0xFFFFFFFF none)   36 f32 amount (DI+0x78)   40 f32 mult (DI+0x48)   44 f32 a20 (DI+0x20)
//      48 f32 point_local[3] m (each |..| <= 8: the mirror's yaw frame)   60 i16 normal_local[3] q15   66 i16 impulse_local[3]
//      x100 (clamped)   72 u32 t_ms (the sender's tick clock)   76 u16 her Sam's distance to the point, dm   78 u16 0
//   type 5 HITRES (host -> guest), n 1..64, 12 B: u32 key | u32 hit_seq | u8 result (erules::HitResult, 0..kHitResultMax) | u8
//      owner mode (0 credit, 1 body, 2 none, 3 sam) | u8 defeat now | u8 0
//   type 6 OUTCOME (host -> guest), n 1..32, 24 B: u32 key | u8 defeat (erules::Defeat) | u8 cause (erules::Cause) | u16 ver |
//      u8 state (+0xEA0) | u8 downbits ((+0xEA4 >> 19) & 0x3F) | u8 phase (0xFF) | u8 epoch (the host's outcome epoch, 1..255:
//      a new one each host world or link session, its versions start again; the guest drops her table on a change) | f32 health |
//      f32 stamina | u32 the last applied hit_seq (0 none)
//   type 7 PHASE: reserved (never sent in P2; refused)
//   type 8 RESYNC (guest -> host), n 0: the host answers with an OUTCOME for every enemy whose defeat is not none.
// P1b (round 17, docs/enemy-sync.md 10): ownership by proximity. The header's u32 stays the guids fingerprint (the world check);
// the host's combat generation travels in each entry.
//   type 3 OWN (host -> guest), n 1..16, 40 B: u32 gen (the host's combat generation) | u32 id | u16 epoch | u8 owner (1 host,
//      2 guest) | u8 reason (erules::OwnReason 1..12, never 6) | u8 phase (0..5, 0xFF unknown) | u8 flags (bit0 damaged since the
//      last change, bit1 terminal, bit2 has place) | u16 0 | i32 pos[3] mm | u16 yaw | i16 vel[3] cm/s | f32 health (NaN: not
//      carried)
//   type 9 OWNACK (guest -> host), the same layout: owner = the owner she now applies, reason 6 (ack), the u16 at +14 = result
//      (0 taken, 1 released, 2 refused) << 8 | the refusal code (erules::OwnRefuse); place / yaw / vel / phase her copy's after it
//   type 10 OWNRESET (host -> guest), n 1, 4 B: u32 the new generation (everything is the host's).
inline constexpr std::size_t kScbHeader = 16, kHitSize = 80, kHitResSize = 12, kOutcomeSize = 24, kOwnSize = 40, kOwnResetSize = 4;
inline constexpr unsigned kHitMax = 12, kHitResMax = 64, kOutcomeMax = 32, kOwnMax = 16;
inline constexpr std::size_t kScbMaxSize = kScbHeader + kHitMax * kHitSize;  // 976 B
enum ScbType : std::uint8_t { kScbOwn = 3, kScbHit = 4, kScbHitRes = 5, kScbOutcome = 6, kScbPhase = 7, kScbResync = 8, kScbOwnAck = 9, kScbOwnReset = 10 };
// kKindRestrain (5, round 23, docs/enemy-sync.md 14): "the acting game's copy was tied up (or knocked out) by its own Sam": no damage
// carried; b180 = the outcome setter's k (0 faint, 2 restrained), depth = the signal (1 the copy's own +0xEA4 read, 2 the fallback:
// strand weapon 41 + attack 0x15C at contact, 3 a hidden-run test), amount 0; byte 19 stays the zero pad. Sent only to a partner
// whose beacon carries shotw::kPartRestrain (an older decoder refuses kind 5 and the whole message with it).
enum HitKind : std::uint8_t { kKindBullet = 1, kKindJob = 2, kKindDirect = 3, kKindTest = 4, kKindRestrain = 5 };
enum RestrainSrc : std::uint8_t { kRestrainNative = 1, kRestrainFallback = 2, kRestrainTest = 3 };
enum HitFlag : std::uint8_t {
    kHitAttackerSam = 1, kHitOurWeapon = 2, kHitOurVehicle = 4, kHitOnPart = 8, kHitLethal = 16, kHitPartValid = 32, kHitNormalValid = 64, kHitImpulseValid = 128
};
inline constexpr double kHitPointMaxM = 8.0, kImpulseMax = 327.0;
inline constexpr std::uint8_t kHitResultMax = 13;  // erules::HitResult's last (down, round 23: only ever an answer to kind 5; enemy_combat.cpp static_asserts it)
struct ScbHeader {
    std::uint8_t type = 0;
    std::uint8_t n = 0;
    std::uint32_t guids_fp = 0, seq = 0;
};
struct Hit {
    std::uint32_t key = 0, hit_seq = 0;
    std::uint16_t attack = 0;
    std::uint8_t kind = 0, flags = 0;
    std::uint32_t serial = 0;
    std::uint8_t b12E = 0, b180 = 0, depth = 0;
    std::uint32_t v130 = 0, v184 = 0, v188 = 0;
    std::uint32_t part = 0xFFFFFFFFu;
    float amount = 0, mult = 1, a20 = 1;
    float point[3] = {};    // m, the mirror's yaw frame
    float normal[3] = {};   // unit (q15 on the wire)
    float impulse[3] = {};  // x100 on the wire, clamped to +-327
    std::uint32_t t_ms = 0;
    float her_dist_m = 0;   // dm on the wire (clamped to 6553.5 m)
};
struct HitRes {
    std::uint32_t key = 0, hit_seq = 0;
    std::uint8_t result = 0, owner = 0, defeat = 0;
};
struct Outcome {
    std::uint32_t key = 0;
    std::uint8_t defeat = 0, cause = 0;
    std::uint16_t ver = 0;
    std::uint8_t state = 0, downbits = 0, phase = 0xFF;
    std::uint8_t epoch = 0;  // the host's outcome epoch (1..255; 0 an old sender: never compared)
    float health = 0, stamina = 0;
    std::uint32_t last_seq = 0;
};
enum OwnFlag : std::uint8_t { kOwnDamaged = 1, kOwnTerminal = 2, kOwnHasPlace = 4 };
enum OwnResult : std::uint8_t { kOwnTaken = 0, kOwnReleased = 1, kOwnRefused = 2 };
struct Own {
    std::uint32_t gen = 0, id = 0;
    std::uint16_t epoch = 0;
    std::uint8_t owner = 1, reason = 1, phase = 0xFF, flags = 0;
    std::uint8_t result = 0, refusal = 0;  // OWNACK only (0 on an OWN)
    double pos[3] = {};  // m, world
    float yaw = 0;       // rad
    float vel[3] = {};   // m/s
    float health = 0;    // NaN: not carried
};
// The encoders return the size, 0 when it does not encode (n out of range, an entry that would not decode).
std::size_t EncodeHits(const ScbHeader& h, const Hit* hits, unsigned n, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeHitRes(const ScbHeader& h, const HitRes* r, unsigned n, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeOutcomes(const ScbHeader& h, const Outcome* o, unsigned n, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeResync(const ScbHeader& h, std::uint8_t* out, std::size_t cap) noexcept;
// type kScbOwn or kScbOwnAck (an OWN carries result / refusal 0).
std::size_t EncodeOwns(const ScbHeader& h, std::uint8_t type, const Own* o, unsigned n, std::uint8_t* out, std::size_t cap) noexcept;
std::size_t EncodeOwnReset(const ScbHeader& h, std::uint32_t gen, std::uint8_t* out, std::size_t cap) noexcept;
// Strict: the magic, a known type (4, 5, 6, 8), the version, n in its bounds, byte 7 zero, the exact size. `type`, `n`, the
// fingerprint and seq into h.
bool DecodeScbHeader(const std::uint8_t* d, std::size_t n, ScbHeader& h) noexcept;
// Strict per entry (an enemy key, AttackOk, a known kind, finite values, |point| <= 8 m, the reserved bytes zero, a known result /
// owner / defeat / cause). One bad entry rejects the message. `out` holds the type's maximum.
bool DecodeHits(const std::uint8_t* d, std::size_t n, ScbHeader& h, Hit (&out)[kHitMax]) noexcept;
bool DecodeHitRes(const std::uint8_t* d, std::size_t n, ScbHeader& h, HitRes (&out)[kHitResMax]) noexcept;
bool DecodeOutcomes(const std::uint8_t* d, std::size_t n, ScbHeader& h, Outcome (&out)[kOutcomeMax]) noexcept;
// Strict: an enemy id, owner 1 / 2, an OWN's reason 1..12 but 6 with the result word 0, an OWNACK's reason 6 with result 0..2 and
// refusal 0..9, phase 0..5 or 0xFF, the flags' high bits zero, a finite place within 2000 km (health may be NaN).
bool DecodeOwns(const std::uint8_t* d, std::size_t n, ScbHeader& h, Own (&out)[kOwnMax]) noexcept;
bool DecodeOwnReset(const std::uint8_t* d, std::size_t n, ScbHeader& h, std::uint32_t* gen) noexcept;
// Strict on the header (magic, version, slot 0..7, the reserved word 0, an enemy id) and the STJ1 bytes (wire::DecodeJoints).
// A frame with another layout hash is decoded with foreign_layout set.
bool DecodeEnemyPose(const std::uint8_t* d, std::size_t n, EnemyPose& out) noexcept;

}  // namespace st::ewire
