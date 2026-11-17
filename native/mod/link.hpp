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
};

bool Start(const Config& config) noexcept;  // game thread, once
bool Running() noexcept;
bool SendPose(const wire::Pose& pose) noexcept;  // game thread; false when not connected
// The pose plus a joint frame (STJ1) in the same datagram (one FRAME message each).
// Peers before v0.4 ignore the second message (DecodePose rejects its size and magic).
bool SendPoseAndJoints(const wire::Pose& pose, const wire::JointFrame& joints) noexcept;
// Received joint frames, oldest first, with their own "newer seq" filter (the seq is
// shared with the STP1 frame of the same datagram) and the same generation counter.
bool PopJoints(wire::JointFrame& out, std::uint64_t& receive_ms, std::uint32_t& generation) noexcept;
// Oldest-first; receive_ms is NowMs() at arrival; session changes (a new
// handshake: rejoin, different guest) bump `generation` so the caller can
// reset its interpolation state.
bool PopPose(wire::Pose& out, std::uint64_t& receive_ms, std::uint32_t& generation) noexcept;
// Millisecond clock from QueryPerformanceCounter (shared by link and sync).
std::uint64_t NowMs() noexcept;
double NowMsPrecise() noexcept;
Info GetInfo() noexcept;
// Host only: "DST1-...@<id>" once Steam knows our id; false otherwise.
bool BuildJoinCode(char* out, std::size_t capacity) noexcept;
void Shutdown() noexcept;  // process exit: one CLOSE, best effort

}  // namespace st::link
