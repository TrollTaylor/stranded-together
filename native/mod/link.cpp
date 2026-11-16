#include "link.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>

#include "log.hpp"
#include "steam_owner.hpp"
#include "stcoop/link_crypto.hpp"
#include "stcoop/link_protocol.hpp"
#include "stcoop/link_session.hpp"
#include "stcoop/steam_join_code.hpp"
#include "stcoop/steam_net_callbacks.hpp"
#include "stcoop/steam_net_dispatch.hpp"
#include "stcoop/steam_transport.hpp"
#include "stcoop/udp_transport.hpp"

#ifndef ST_BUILD_ID
#define ST_BUILD_ID "dev"
#endif

namespace st::link {
namespace {

namespace lk = stcoop::link;
namespace lab = stcoop::lab;

constexpr std::uint32_t kIdleMs = 20;

SRWLOCK g_tx = SRWLOCK_INIT;  // session
SRWLOCK g_rx = SRWLOCK_INIT;  // received pose queue
lk::Session g_session;
std::uint8_t g_body[lk::kLinkMaxBody];

lab::UdpTransport g_udp;
lab::SteamTransport g_steam;
bool g_use_steam = false;
bool g_host = false;
volatile long g_running = 0;
volatile long g_failed = 0;
char g_fail_reason[96] = {};
SRWLOCK g_reason_lock = SRWLOCK_INIT;
void SetReason(const char* text) noexcept {
    AcquireSRWLockExclusive(&g_reason_lock);
    std::snprintf(g_fail_reason, sizeof(g_fail_reason), "%s", text);
    ReleaseSRWLockExclusive(&g_reason_lock);
}

char g_secret[lk::kSecretChars] = {};
bool g_secret_valid = false;

// Received pose queue (latest 64).
struct Rx {
    wire::Pose pose;
    std::uint64_t ms;
    std::uint32_t generation;
};
std::uint32_t g_generation = 0;
Rx g_queue[64];
unsigned g_q_head = 0, g_q_count = 0;
std::uint32_t g_last_rx_seq = 0;
bool g_have_rx_seq = false;

volatile LONG64 g_rx_frames = 0, g_tx_frames = 0;
volatile long g_state = static_cast<long>(lk::LinkState::off);


void SteamLog(const char* line) noexcept {
    // Transport lines end with \r\n.
    char buf[512];
    std::strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char* p = buf; *p; ++p)
        if (*p == '\r' || *p == '\n') *p = 0;
    Log("[net] %s", buf);
}

bool Transmit(void*, const lab::Endpoint& to, const std::uint8_t* data, std::size_t size) noexcept {
    const lab::WireChunk chunk{data, size};
    return g_use_steam ? g_steam.transmit_to(to, &chunk, 1, true) : g_udp.transmit_to(to, &chunk, 1, true);
}

bool Random(void*, std::uint8_t* out, std::size_t size) noexcept {
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

void FormatEndpoint(const lab::Endpoint& e, char* out, std::size_t cap) noexcept {
    if (g_use_steam) {
        std::snprintf(out, cap, "steam:%llu", static_cast<unsigned long long>(lab::SteamIdOf(e)));
    } else {
        std::snprintf(out, cap, "%u.%u.%u.%u:%u", (e.address >> 24) & 255, (e.address >> 16) & 255,
                      (e.address >> 8) & 255, e.address & 255, e.port);
    }
}

void LogEvents(const lk::Event* events, unsigned count) noexcept {
    for (unsigned i = 0; i < count; ++i) {
        const lk::Event& e = events[i];
        char peer[48];
        FormatEndpoint(e.peer, peer, sizeof(peer));
        switch (e.kind) {
            case lk::EventKind::connected:
            case lk::EventKind::reconnected:
                Log("[link] CONNECTED session=%u peer=%s rtt=%ums remote_build=%s", e.session, peer, e.rtt_ms,
                    e.remote_build);
                break;
            case lk::EventKind::lost:
                Log("[link] lost peer=%s after %ums of silence", peer, e.gap_ms);
                break;
            case lk::EventKind::rejected:
                Log("[link] rejected: %s (verified=%d) - do both players run the same mod build and code?",
                    lk::RejectReasonName(e.reason), e.verified ? 1 : 0);
                { char r_[96]; std::snprintf(r_, sizeof(r_), "rejected: %s", lk::RejectReasonName(e.reason)); SetReason(r_); }
                break;
            case lk::EventKind::warn_build:
                Log("[link] warning: peer build '%s' differs from ours '%s'", e.remote_build, ST_BUILD_ID);
                break;
            case lk::EventKind::handshake:
                Log("[link] handshake %s peer=%s", lk::HandshakeStepName(e.step), peer);
                break;
            case lk::EventKind::connecting:
                Log("[link] connecting to %s (attempt %u)", peer, e.attempt);
                break;
            case lk::EventKind::reconnecting:
                Log("[link] reconnecting (attempt %u)", e.attempt);
                break;
            case lk::EventKind::listening:
                Log("[link] listening for the guest");
                break;
            default:
                Log("[link] event %d", static_cast<int>(e.kind));
                break;
        }
    }
}

void OnDatagram(void*, const std::uint8_t* data, std::size_t size, const lab::Endpoint& from) noexcept {
    constexpr unsigned kMaxFrames = 32, kMaxEvents = 16;
    wire::Pose poses[kMaxFrames];
    unsigned pose_count = 0;
    lk::Event events[kMaxEvents];
    unsigned event_count = 0;
    const std::uint64_t now = NowMs();
    bool new_session = false;
    lab::Endpoint committed{};

    AcquireSRWLockExclusive(&g_tx);
    const lk::Delivery d = g_session.OnDatagram(data, size, from, now);
    new_session = d.new_session;
    unsigned messages = 0;
    if (d.data && lk::ValidateBody(d.body, d.size, messages)) {
        lk::MessageReader reader(d.body, d.size);
        lk::Message m{};
        bool any_pose = false;
        while (reader.Next(m)) {
            if (m.type == static_cast<std::uint8_t>(lk::MsgType::frame)) {
                wire::Pose p;
                if (pose_count < kMaxFrames && wire::DecodePose(m.data, m.size, p)) {
                    poses[pose_count++] = p;
                    any_pose = true;
                }
            } else if (m.type == static_cast<std::uint8_t>(lk::MsgType::close)) {
                g_session.OnPeerClose(now);
            }
        }
        if (any_pose) g_session.jitter().OnPacket(static_cast<std::uint32_t>(now), d.send_ms, now);
    }
    lk::Event e{};
    while (event_count < kMaxEvents && g_session.PopEvent(e)) events[event_count++] = e;
    InterlockedExchange(&g_state, static_cast<long>(g_session.state()));
    committed = g_session.peer();
    ReleaseSRWLockExclusive(&g_tx);

    if (g_use_steam && new_session && g_host && committed.address) g_steam.set_peer(lab::SteamIdOf(committed));

    if (pose_count || new_session) {
        AcquireSRWLockExclusive(&g_rx);
        if (new_session) {
            g_q_count = 0;
            g_have_rx_seq = false;
            ++g_generation;
        }
        for (unsigned i = 0; i < pose_count; ++i) {
            const wire::Pose& p = poses[i];
            // Drop stale/duplicate frames (unsigned wrap-safe "newer").
            if (g_have_rx_seq && static_cast<std::int32_t>(p.seq - g_last_rx_seq) <= 0) continue;
            g_last_rx_seq = p.seq;
            g_have_rx_seq = true;
            const unsigned slot = (g_q_head + g_q_count) % 64;
            g_queue[slot] = Rx{p, now, g_generation};
            if (g_q_count < 64) ++g_q_count;
            else g_q_head = (g_q_head + 1) % 64;
            InterlockedIncrement64(&g_rx_frames);
        }
        ReleaseSRWLockExclusive(&g_rx);
    }
    LogEvents(events, event_count);
}

void OnIdle(void*) noexcept {
    constexpr unsigned kMaxEvents = 16;
    lk::Event events[kMaxEvents];
    unsigned event_count = 0;
    const std::uint64_t now = NowMs();
    AcquireSRWLockExclusive(&g_tx);
    g_session.OnTimer(now);
    lk::Event e{};
    while (event_count < kMaxEvents && g_session.PopEvent(e)) events[event_count++] = e;
    InterlockedExchange(&g_state, static_cast<long>(g_session.state()));
    ReleaseSRWLockExclusive(&g_tx);
    LogEvents(events, event_count);
}

void SteamReady(void*) noexcept { Log("[net] steam transport ready (self=%llu)", static_cast<unsigned long long>(steam::SelfId())); }
void SteamFailed(void*, const char* reason) noexcept {
    { char r_[96]; std::snprintf(r_, sizeof(r_), "steam: %s", reason ? reason : "failed"); SetReason(r_); }
    InterlockedExchange(&g_failed, 1);
    Log("[net] steam transport failed: %s", reason ? reason : "?");
}
void SteamShutdown(void*) noexcept {
    // Best-effort goodbye so the peer frees its slot at once.
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData()) {
        lk::BodyWriter w(g_body, sizeof(g_body));
        if (w.Add(lk::MsgType::close, nullptr, 0)) g_session.SendData(g_body, w.size(), NowMs());
    }
    ReleaseSRWLockExclusive(&g_tx);
}

bool ParseIpv4(const std::string& text, std::uint32_t& out) noexcept {
    IN_ADDR a{};
    if (InetPtonA(AF_INET, text.c_str(), &a) != 1) return false;
    out = ntohl(a.S_un.S_addr);
    return true;
}

}  // namespace

double NowMsPrecise() noexcept {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

std::uint64_t NowMs() noexcept { return static_cast<std::uint64_t>(NowMsPrecise()); }

bool Start(const Config& c) noexcept {
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return true;
    g_host = c.role == Role::host;
    g_use_steam = c.transport == Transport::steam;

    // Secret: from the join code; the UDP dev path may fall back to the lab secret.
    lk::SecretStatus ss = lk::ParseSecretText(c.secret.data(), c.secret.size(), g_secret);
    if (ss != lk::SecretStatus::ok) {
        if (!g_use_steam) {
            std::memcpy(g_secret, lk::kLabSecret, lk::kSecretChars);
            Log("[link] no valid secret in session.ini (%s): using the built-in LAN test secret", lk::SecretStatusName(ss));
        } else {
            { char r_[96]; std::snprintf(r_, sizeof(r_), "secret %s", lk::SecretStatusName(ss)); SetReason(r_); }
            Log("[link] disabled: session.ini secret is %s (paste the full DST1 code in the launcher)",
                lk::SecretStatusName(ss));
            InterlockedExchange(&g_failed, 1);
            return false;
        }
    }
    g_secret_valid = true;

    lk::SessionConfig sc{};
    sc.role = g_host ? lk::Role::host : lk::Role::guest;
    sc.play = true;
    std::strncpy(sc.build, ST_BUILD_ID, lk::kBuildSize);
    lk::DerivePsk(g_secret, sc.psk);

    if (g_use_steam) {
        if (!g_host && !stcoop::steam::is_individual_steamid(c.peer_steam_id)) {
            { char r_[96]; std::snprintf(r_, sizeof(r_), "no host SteamID in session.ini"); SetReason(r_); }
            Log("[link] disabled: guest needs peer_steamid (from the host's DST1 code)");
            InterlockedExchange(&g_failed, 1);
            return false;
        }
        sc.exact_filter = true;
        sc.peer = g_host ? lab::Endpoint{} : lab::SteamEndpoint(c.peer_steam_id);
        AcquireSRWLockExclusive(&g_tx);
        g_session.Start(sc, lk::SessionIo{nullptr, &Transmit, &Random}, NowMs());
        ReleaseSRWLockExclusive(&g_tx);

        lab::SteamTransportConfig t{};
        t.role = g_host ? lab::SteamRole::host : lab::SteamRole::guest;
        t.leg = lab::SteamLeg::messages;
        t.host_id = g_host ? 0 : c.peer_steam_id;
        t.idle_interval_ms = kIdleMs;
        t.accepting = true;
        t.warm_relay = true;
        t.resolve = &steam::Resolve;
        t.pop_request = g_host ? &stcoop::steam_cb::PopSessionRequest : nullptr;
        t.dispatch = stcoop::steam_dispatch::InstallBits(stcoop::steam_dispatch::Variant::both, g_host);
        t.log = &SteamLog;
        // self_id is learned on the transport thread through Resolve; the
        // guest filter only needs host_id.
        lab::SteamSink sink{};
        sink.on_datagram = &OnDatagram;
        sink.on_idle = &OnIdle;
        sink.on_ready = &SteamReady;
        sink.on_failed = &SteamFailed;
        sink.on_shutdown = &SteamShutdown;
        if (!g_steam.start(t, sink)) {
            { char r_[96]; std::snprintf(r_, sizeof(r_), "steam transport did not start"); SetReason(r_); }
            Log("[link] steam transport failed to start (error %d)", static_cast<int>(g_steam.error()));
            InterlockedExchange(&g_failed, 1);
            return false;
        }
        Log("[link] steam %s started; peer=%llu", g_host ? "host" : "guest",
            static_cast<unsigned long long>(c.peer_steam_id));
    } else {
        std::uint32_t peer_addr = 0x7F000001;
        if (!ParseIpv4(c.udp_peer, peer_addr)) Log("[link] udp_peer '%s' is not IPv4; using 127.0.0.1", c.udp_peer.c_str());
        sc.exact_filter = false;
        sc.peer = lab::Endpoint{peer_addr, c.udp_port_peer};
        AcquireSRWLockExclusive(&g_tx);
        g_session.Start(sc, lk::SessionIo{nullptr, &Transmit, &Random}, NowMs());
        ReleaseSRWLockExclusive(&g_tx);

        lab::TransportConfig t{};
        t.listen_port = c.udp_port_local;
        t.peer_port = g_host ? 0 : c.udp_port_peer;
        t.idle_interval_ms = kIdleMs;
        t.bind_address = 0;  // any
        t.peer_address = peer_addr;
        t.any_source = true;
        t.allow_ephemeral = false;
        lab::DatagramSink sink{};
        sink.on_datagram = &OnDatagram;
        sink.on_idle = &OnIdle;
        if (!g_udp.start(t, sink)) {
            { char r_[96]; std::snprintf(r_, sizeof(r_), "udp bind failed on port %u", c.udp_port_local); SetReason(r_); }
            Log("[link] UDP transport failed to start on port %u (error %d)", c.udp_port_local,
                static_cast<int>(g_udp.error()));
            InterlockedExchange(&g_failed, 1);
            return false;
        }
        Log("[link] udp %s started: local port %u, peer %s:%u", g_host ? "host" : "guest", c.udp_port_local,
            c.udp_peer.c_str(), c.udp_port_peer);
    }
    lk::SecureZero(&sc.psk, sizeof(sc.psk));
    return true;
}

bool Running() noexcept { return g_running != 0 && g_failed == 0; }

bool SendPose(const wire::Pose& pose) noexcept {
    std::uint8_t frame[wire::kPoseFrameSize];
    const std::size_t n = wire::EncodePose(pose, frame);
    bool sent = false;
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData()) {
        lk::BodyWriter w(g_body, sizeof(g_body));
        if (w.Add(lk::MsgType::frame, frame, n)) sent = g_session.SendData(g_body, w.size(), NowMs());
    }
    ReleaseSRWLockExclusive(&g_tx);
    if (sent) InterlockedIncrement64(&g_tx_frames);
    return sent;
}

bool PopPose(wire::Pose& out, std::uint64_t& receive_ms, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_rx);
    if (g_q_count) {
        out = g_queue[g_q_head].pose;
        receive_ms = g_queue[g_q_head].ms;
        generation = g_queue[g_q_head].generation;
        g_q_head = (g_q_head + 1) % 64;
        --g_q_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_rx);
    return ok;
}

Info GetInfo() noexcept {
    Info i;
    i.transport = g_use_steam ? "steam" : "udp";
    if (!g_running) {
        i.state = "off";
        return i;
    }
    AcquireSRWLockShared(&g_tx);
    const lk::LinkState s = g_session.state();
    i.connected = s == lk::LinkState::connected;
    i.rtt_ms = g_session.rtt().have() ? g_session.rtt().srtt_ms() : 0;
    const lab::Endpoint peer = g_session.peer();
    const bool has_peer = g_session.has_peer();
    ReleaseSRWLockShared(&g_tx);
    i.state = g_failed ? "error" : lk::LinkStateName(s);
    if (has_peer) FormatEndpoint(peer, i.peer, sizeof(i.peer));
    if (g_use_steam) {
        const auto r = g_steam.route();
        i.route = r == stcoop::steam_net::Route::relay ? "relay" : r == stcoop::steam_net::Route::direct ? "direct" : "";
    }
    AcquireSRWLockShared(&g_reason_lock);
    std::strncpy(i.reason, g_fail_reason, sizeof(i.reason) - 1);
    ReleaseSRWLockShared(&g_reason_lock);
    i.rx_frames = static_cast<std::uint64_t>(g_rx_frames);
    i.tx_frames = static_cast<std::uint64_t>(g_tx_frames);
    return i;
}

bool BuildJoinCode(char* out, std::size_t capacity) noexcept {
    if (!g_host || !g_secret_valid || !g_use_steam) return false;
    const std::uint64_t self = steam::SelfId();
    if (!self) return false;
    return stcoop::steam_code::Format(g_secret, self, out, capacity);
}

void Shutdown() noexcept {
    if (!g_running) return;
    if (g_use_steam) g_steam.request_shutdown();
    else SteamShutdown(nullptr);
}

}  // namespace st::link
