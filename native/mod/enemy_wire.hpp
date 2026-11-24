// Enemy sync P1a (docs/enemy-sync.md 8): the two unreliable enemy frames, host -> guest. The pure part: their codecs and strict
// decoders. No game, no link: unit-tested in tests/enemy_wire_tests.cpp (st_enemy_wire_tests).
//
// SEN1, the enemies' places and states (all little-endian; one FRAME message in a datagram of its own):
//   header 20 B: 'SEN1' | u8 version 1 | u8 n (1..32) | u16 flags (bit0: the last frame of a full pass; 0 in P1a) | u32 seq |
//                u32 sender_ms (the sender's tick clock) | u32 gen_tag (0)
//   entry 28 B:
//      0 u32 actor_id (an enemy id: enemy::IsHumanEnemyId)
//      4 u8  owner_epoch (1)
//      5 u8  state: bit0 realized on the sender, bit1 dead, bits2-4 defeat (0 in P1a), bit5 the sender sends a pose (SEP1),
//                   bit6 hidden on the sender, bit7 a real sequence holds it on the sender
//      6 u8  phase (0xFF in P1a)
//      7 u8  flags: bit0 moving (|v| > 0.2 m/s), bit1 owner is the guest (0 in P1a)
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
enum EntryFlag : std::uint8_t { kMoving = 1, kOwnerGuest = 2 };

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
// Strict on the header (magic, version, slot 0..7, the reserved word 0, an enemy id) and the STJ1 bytes (wire::DecodeJoints).
// A frame with another layout hash is decoded with foreign_layout set.
bool DecodeEnemyPose(const std::uint8_t* d, std::size_t n, EnemyPose& out) noexcept;

}  // namespace st::ewire
