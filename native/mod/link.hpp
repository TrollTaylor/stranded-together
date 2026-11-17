// The co-op link: DDT's link v2 session (authenticated handshake, HMAC,
// keepalives, RTT) over either Steam Networking Messages or plain UDP.
// Carries pose frames (pose_wire) as unreliable FRAME messages.
//
// Threads: the transport thread drives the session (datagrams + timers); the
// game thread sends poses and drains received ones. One SRW lock guards the
// session; the received-pose queue has its own lock.
#pragma once
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
};

bool Start(const Config& config) noexcept;  // game thread, once
bool Running() noexcept;
bool SendPose(const wire::Pose& pose) noexcept;  // game thread; false when not connected
// The pose plus a joint frame (STJ1) in the same datagram (one FRAME message each).
// Only a same-build peer ever receives it: the link runs in play mode, where a HELLO from
// another build is rejected (build_mismatch) before any FRAME is exchanged.
bool SendPoseAndJoints(const wire::Pose& pose, const wire::JointFrame& joints) noexcept;
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
// Millisecond clock from QueryPerformanceCounter (shared by link and sync).
std::uint64_t NowMs() noexcept;
double NowMsPrecise() noexcept;
Info GetInfo() noexcept;
// Host only: "DST1-...@<id>" once Steam knows our id; false otherwise.
bool BuildJoinCode(char* out, std::size_t capacity) noexcept;
void Shutdown() noexcept;  // process exit: one CLOSE, best effort

}  // namespace st::link
