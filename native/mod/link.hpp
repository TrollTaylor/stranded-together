// The co-op link: DDT's link v2 session (authenticated handshake, HMAC,
// keepalives, RTT) over either Steam Networking Messages or plain UDP.
// Carries pose frames (pose_wire) as unreliable FRAME messages, and speech frames (STS1) on the
// session's reliable ordered channel (link_protocol.hpp ReliableSender / ReliableReceiver).
//
// Threads: the transport thread drives the session (datagrams + timers); the
// game thread sends poses and drains received ones. One SRW lock guards the
// session; the received-pose queue has its own lock.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "pose_wire.hpp"

namespace st::link {

struct Info {
    const char* state = "off";     // LinkStateName or "off"/"error"
    const char* transport = "";    // "steam" | "udp"
    const char* route = "";        // "relay" | "direct" | ""
    char peer[48] = {};
    std::uint32_t rtt_ms = 0;
    bool connected = false;
    char reason[96] = {};
    std::uint64_t rx_frames = 0, tx_frames = 0;
    // Joint frames (STJ1) sent / accepted, and the link body bytes sent with the poses (the
    // FRAME messages; the link header and encryption come on top): the real body's bandwidth.
    std::uint64_t rx_joint_frames = 0, tx_joint_frames = 0, tx_payload_bytes = 0;
    // Player name frames (STN1, name tags) sent / accepted.
    std::uint64_t rx_name_frames = 0, tx_name_frames = 0;
    // Gear layout frames (STC1, backpack and cargo) sent / accepted.
    std::uint64_t rx_gear_frames = 0, tx_gear_frames = 0;
    // Speech frames (STS1, partner voice) queued on / delivered by the reliable channel, and the
    // reliable channel's retransmissions (all channels).
    std::uint64_t rx_speech = 0, tx_speech = 0, rel_retransmits = 0;
    // Sound event frames (STA1, partner sounds) sent (in a pose datagram or, without room, one of their own) /
    // accepted, and those that went alone.
    std::uint64_t rx_sound = 0, tx_sound = 0, tx_sound_alone = 0;
    // Save transfer messages (STX1, channel 4) queued / delivered.
    std::uint64_t rx_save = 0, tx_save = 0;
};

bool Start(const Config& config) noexcept;  // game thread, once
// Hidden runs only (dllmain sets it from automation::Enabled() before Start): session.ini test_net_* impair the UDP link.
void AllowTestImpairment(bool on) noexcept;
bool Running() noexcept;
// `sound` (optional): an STA1 frame (partner sounds, docs/audio-research.md section 16) added to the same
// datagram; when it does not fit there it goes in a datagram of its own right after.
bool SendPose(const wire::Pose& pose, const wire::SoundFrame* sound = nullptr) noexcept;  // game thread; false when not connected
// The pose plus a joint frame (STJ1) in the same datagram (one FRAME message each).
// Only a same-build peer ever receives it: the link runs in play mode, where a HELLO from
// another build is rejected (build_mismatch) before any FRAME is exchanged.
bool SendPoseAndJoints(const wire::Pose& pose, const wire::JointFrame& joints, const wire::SoundFrame* sound = nullptr) noexcept;
// Received sound event frames (STA1), oldest first (a queue of 16; the oldest is dropped when full), with their
// own "newer seq" filter (u16, wrap-safe) and the generation counter.
bool PopSound(wire::SoundFrame& out, std::uint32_t& generation) noexcept;
// Received joint frames, oldest first, with their own "newer seq" filter (the seq is
// shared with the STP1 frame of the same datagram) and the same generation counter.
bool PopJoints(wire::JointFrame& out, std::uint64_t& receive_ms, std::uint32_t& generation) noexcept;
// Oldest-first; receive_ms is NowMs() at arrival; session changes (a new
// handshake: rejoin, different guest) bump `generation` so the caller can
// reset its interpolation state.
bool PopPose(wire::Pose& out, std::uint64_t& receive_ms, std::uint32_t& generation) noexcept;
// Name tags (docs/real-partner-body.md section 10). Game thread: the name the partner should
// show above our head (UTF-8; sanitized here; "" with send = false stops sending). The link adds
// a STN1 frame to the pose datagrams: the first 10 of every session, then every 30th, and the
// next one after a change.
void SetLocalName(const char* utf8, bool send) noexcept;
// The partner's name from its newest STN1 frame of the current session (sanitized). False when
// none arrived this session. generation = the link session it came in (as PopPose's).
bool PeerName(char* out, std::size_t cap, std::uint32_t* generation, bool* from_host) noexcept;
// Gear (docs/real-partner-body.md section 12). Game thread: our backpack / cargo layout (STC1) in a
// datagram of its own (one FRAME message). False when not connected or it does not encode.
bool SendGear(const wire::GearFrame& gear) noexcept;
// Received gear frames, oldest first (a small queue), with their own "newer seq" filter and the
// same generation counter as PopPose.
bool PopGear(wire::GearFrame& out, std::uint64_t& receive_ms, std::uint32_t& generation) noexcept;
// One entry of our gear resource table (STR1) in a datagram of its own; received ones, oldest first
// (a queue of 16, no sequence filter: an entry is idempotent), with the generation counter.
bool SendGearRes(const wire::GearRes& res) noexcept;
bool PopGearRes(wire::GearRes& out, std::uint32_t& generation) noexcept;
// The look of one item of our gear (STV1: its art-parts variations and shader variables) and the
// skinned pose of one item (STK1: the backpack's joints), each in a datagram of its own; received
// ones, oldest first (queues of 16 / 4, no sequence filter: the newest per item wins), with the
// generation counter (docs/real-partner-body.md section 13).
bool SendGearLook(const wire::GearLook& look) noexcept;
bool PopGearLook(wire::GearLook& out, std::uint32_t& generation) noexcept;
bool SendGearPose(const wire::GearPose& pose) noexcept;
bool PopGearPose(wire::GearPose& out, std::uint32_t& generation) noexcept;
// The vehicle we ride or have just left (STM1, docs/real-partner-body.md section 22) in a datagram of its own;
// received ones, oldest first (a queue of 8, newer-seq filter), with the generation counter. Counts both ways.
bool SendVehicle(const wire::VehicleFrame& f) noexcept;
bool PopVehicle(wire::VehicleFrame& out, std::uint32_t& generation) noexcept;
void VehicleCounts(std::uint64_t* sent, std::uint64_t* received) noexcept;
// The real shared vehicles' driven / parked vehicle frames (STD1, vehicle_wire.hpp DriveFrame; docs/real-partner-body.md
// section 23.11): one FRAME message in a datagram of its own (only "STD1" with one of its two sizes is sent); received
// ones as raw bytes, oldest first (a queue of 64, the oldest dropped when full; no sequence filter: the drive module keeps
// one per vehicle), with the generation counter. The game thread decodes them (vwire::DecodeDrive).
bool SendDrive(const std::uint8_t* bytes, std::size_t size) noexcept;
bool PopDrive(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept;
void DriveCounts(std::uint64_t* sent, std::uint64_t* received, std::uint64_t* dropped) noexcept;
// Partner voice (docs/audio-research.md section 15). Game thread: one STS1 frame queued on the link's
// reliable ordered channel (wire::kSpeechChannel; resent until acknowledged, in order). False when not
// connected, the frame does not encode, or the channel's window (32 unacknowledged messages) is full.
bool SendSpeech(const wire::Speech& s) noexcept;
// Received speech frames in order (a queue of 32; the oldest is dropped when full), with the
// generation counter. Delivered at most once per link session by the reliable channel.
bool PopSpeech(wire::Speech& out, std::uint32_t& generation) noexcept;
// The current link session counter (the `generation` the Pop* calls report; bumped by each new
// handshake). Game thread: tells the sender that the partner's tables were cleared.
std::uint32_t Generation() noexcept;
// ---- Save transfer (STX1, save_xfer.hpp; docs/coop-session.md): the reliable ordered channel 4.
// Any thread: queues one STX1 message (1..1024 bytes; resent until acknowledged, in order). False when not connected,
// too large, or the channel's window (32 unacknowledged messages) is full: keep the message and try again later.
bool SendSave(const void* bytes, std::size_t size) noexcept;
// Channel 4 messages queued and not yet acknowledged (0..32): the sender's flow control.
unsigned SaveQueued() noexcept;
// The received channel 4 messages, in order, exactly once per link session. With a sink registered they are handed
// to it on the transport thread, outside the link's locks, as they complete (keep it short: decode and queue, no
// file I/O); without one they wait in a queue of 64 for PopSave (any thread). Only while the queue has room are
// messages taken from the channel, so a slow reader slows the sender (the channel's window) instead of losing data.
// A new link session clears the queue; `generation` is the session (as PopPose's).
using SaveSink = void (*)(void* ctx, const std::uint8_t* bytes, std::size_t size, std::uint32_t generation);
void SetSaveSink(SaveSink sink, void* ctx) noexcept;   // nullptr: back to the queue
bool PopSave(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept;
// The sender's phase for the burst cap (link_protocol.hpp ReliablePacketsPerTick): true while this game is at the
// title or on a loading screen. Default false (4 packets per tick).
void SetSaveBurst(bool title_or_loading) noexcept;
// ---- Vehicle events (SVM1, vehicle_wire.hpp; docs/real-partner-body.md section 23): the reliable ordered channel 3.
inline constexpr unsigned kVehicleEventChannel = 3;
// Any thread: queues one message (1..1024 bytes; resent until acknowledged, in order). Append only: the channel mixes
// message types, so a "latest" replacement could drop another type's message. False when not connected, too large, or the
// channel's window (32 unacknowledged messages) is full: keep it and try again later.
bool SendVehicleEvent(const void* bytes, std::size_t size) noexcept;
// Channel 3 messages queued and not yet acknowledged (0..32): the sender's flow control.
unsigned VehicleEventsQueued() noexcept;
// The received channel 3 messages, in order, exactly once per link session (a queue of 64; taken from the channel only
// while it has room, so a slow reader slows the sender instead of losing data). A new link session clears the queue;
// `generation` is the session (as PopPose's). Any thread (vehicle_share pops on the game thread).
bool PopVehicleEvent(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept;
void VehicleEventCounts(std::uint64_t* sent, std::uint64_t* received) noexcept;
// ---- World events (SWS1, world_wire.hpp; docs/world-sync.md): the reliable ordered channel 1 ("world"), the same contract
// as the vehicle events above (append only, a receive queue of 64, cleared by a new link session).
inline constexpr unsigned kWorldEventChannel = 1;
// ---- Assets (STT1, texture_wire.hpp; docs/textures.md): the reliable ordered channel 5, the same contract as the world events
// (append only, 1..1024 bytes, a receive queue of 64, cleared by a new link session). The sender paces itself.
bool SendAsset(const void* bytes, std::size_t size) noexcept;
unsigned AssetsQueued() noexcept;
bool SaveBusy() noexcept;  // the save channel (4) has data queued: the assets wait
bool PopAsset(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept;
void AssetCounts(std::uint64_t* sent, std::uint64_t* received) noexcept;
bool SendWorldEvent(const void* bytes, std::size_t size) noexcept;
unsigned WorldEventsQueued() noexcept;
bool PopWorldEvent(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept;
void WorldEventCounts(std::uint64_t* sent, std::uint64_t* received) noexcept;
// ---- Combat (docs/enemy-sync.md): the reliable ordered channel 6 (SCB1, the enemy rounds), the same contract as the world
// events (append only, 1..1024 bytes, a receive queue of 64, cleared by a new link session). Nothing may be queued on it before
// the partner's SEH1 beacon says it has the part (an older lab-mode peer drops channel 6 unacknowledged).
bool SendCombat(const void* bytes, std::size_t size) noexcept;
unsigned CombatQueued() noexcept;
bool PopCombat(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept;
void CombatChannelCounts(std::uint64_t* sent, std::uint64_t* received) noexcept;
// The combat frames, unreliable: SEH1 (the beacon, 24 B), SEN1 (enemy states), SEP1 (enemy poses), SFX1 (shots), SPV1 (the
// player's own visibility, 32 B; round 27 M2), each one FRAME
// message in a datagram of its own; received ones as raw bytes per kind (the magic and the size checked here; the strict
// decoders run on the game thread), oldest first, in queues of 2 / 8 / 16 / 32 / 4 (the oldest dropped when full, counted), with
// the generation counter. A new link session clears them.
enum class CombatFrame : std::uint8_t { beacon = 0, enemies = 1, poses = 2, shots = 3, visibility = 4 };
bool SendCombatFrame(const std::uint8_t* bytes, std::size_t size) noexcept;
bool PopCombatFrame(CombatFrame kind, std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept;
void CombatCounts(CombatFrame kind, std::uint64_t* sent, std::uint64_t* received, std::uint64_t* dropped) noexcept;
// Hidden tests only (the co-op link-loss scenario; coop_session.cpp allows it only in automation runs): every datagram
// this game receives is dropped for `ms` (0: ends now), so its session times out and reconnects as after a real outage.
void TestBlackout(std::uint32_t ms) noexcept;
std::uint64_t TestBlackoutDropped() noexcept;
// Millisecond clock from QueryPerformanceCounter (shared by link and sync).
std::uint64_t NowMs() noexcept;
double NowMsPrecise() noexcept;
Info GetInfo() noexcept;
// Host only: "DST1-...@<id>" once Steam knows our id; false otherwise.
bool BuildJoinCode(char* out, std::size_t capacity) noexcept;
void Shutdown() noexcept;  // process exit: one CLOSE, best effort

}  // namespace st::link
