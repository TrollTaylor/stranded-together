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
bool g_test_impair_allowed = false;  // hidden runs only (dllmain: automation::Enabled()): the test_net_* impairment
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
// Joint frames (STJ1): a small queue of their own (each ~1.3 KB decoded), own seq state.
struct RxJ {
    wire::JointFrame f;
    std::uint64_t ms;
    std::uint32_t generation;
};
constexpr unsigned kJQueue = 8;
RxJ g_jqueue[kJQueue];
unsigned g_jq_head = 0, g_jq_count = 0;
std::uint32_t g_last_rx_jseq = 0;
bool g_have_rx_jseq = false;

// Name tags (STN1). Our own name: written by the game thread (SetLocalName), read under g_tx
// by the senders. The schedule counter is per session (reset on a new session).
char g_local_name[wire::kMaxNameBytes + 1] = {};
std::uint8_t g_local_name_len = 0;
bool g_name_send = false;
std::uint32_t g_name_version = 0, g_name_sent_version = 0;
std::uint32_t g_name_sends = 0;  // pose datagrams sent this session (the STN1 schedule)
// The partner's name (under g_rx), cleared on a new session.
wire::NameFrame g_peer_name;
bool g_have_peer_name = false;
std::uint32_t g_peer_name_generation = 0;
// Gear layout frames (STC1): a small queue of their own, own seq state.
struct RxG {
    wire::GearFrame f;
    std::uint64_t ms;
    std::uint32_t generation;
};
constexpr unsigned kGQueue = 4;
RxG g_gqueue[kGQueue];
unsigned g_gq_head = 0, g_gq_count = 0;
std::uint32_t g_last_rx_gseq = 0;
bool g_have_rx_gseq = false;
// Gear resource entries (STR1): idempotent, so no sequence filter; newest wins when full.
struct RxR {
    wire::GearRes r;
    std::uint32_t generation;
};
constexpr unsigned kRQueue = 16;
RxR g_rqueue[kRQueue];
unsigned g_rq_head = 0, g_rq_count = 0;
// Gear look (STV1) and gear pose (STK1) frames: idempotent per item (the newest wins), no sequence
// filter; small queues, newest wins when full.
struct RxV {
    wire::GearLook l;
    std::uint32_t generation;
};
constexpr unsigned kVQueue = 16;
RxV g_vqueue[kVQueue];
unsigned g_vq_head = 0, g_vq_count = 0;
struct RxK {
    wire::GearPose p;
    std::uint32_t generation;
};
constexpr unsigned kKQueue = 8;  // four pose streams (round 29 O2: the Odradek arm and body too), drained every tick
RxK g_kqueue[kKQueue];
unsigned g_kq_head = 0, g_kq_count = 0;
// Vehicle frames (STM1, docs/real-partner-body.md section 22): a small queue of their own (each ~1.8 KB decoded),
// own "newer seq" filter.
struct RxM {
    wire::VehicleFrame f;
    std::uint32_t generation;
};
constexpr unsigned kMQueue = 8;
RxM g_mqueue[kMQueue];
unsigned g_mq_head = 0, g_mq_count = 0;
std::uint32_t g_last_rx_mseq = 0;
bool g_have_rx_mseq = false;
volatile LONG64 g_rx_vehicle = 0, g_tx_vehicle = 0;
// Driven / parked vehicle frames (STD1, vehicle_wire.hpp; the real shared vehicles' drive module): raw bytes (the magic and
// one of the two sizes checked here, the strict decoder runs on the game thread), oldest first; when full the oldest goes.
// No sequence filter: the receiver keeps its own per vehicle.
constexpr std::size_t kDriveMax = 172;  // vwire::kMaxDriveFrameSize
struct RxD {
    std::uint8_t bytes[kDriveMax];
    std::uint8_t size;
    std::uint32_t generation;
};
constexpr unsigned kDQueue = 64;
RxD g_dqueue[kDQueue];
unsigned g_dq_head = 0, g_dq_count = 0;
volatile LONG64 g_rx_drive = 0, g_tx_drive = 0, g_rx_drive_dropped = 0;
bool LooksLikeDriveFrame(const std::uint8_t* d, std::size_t n) noexcept {
    return d && (n == 124 || n == kDriveMax) && d[0] == 'S' && d[1] == 'T' && d[2] == 'D' && d[3] == '1';
}

// The reliable ordered channel (docs/link-protocol.md "Reliable channel"; under g_tx with the session):
// channel 2 (wire::kSpeechChannel) carries STS1. Reset on every new session.
lk::ReliableSender g_rel_tx[lk::kReliableChannels];
lk::ReliableReceiver g_rel_rx[lk::kReliableChannels];
// Received speech frames (under g_rx), in delivery order.
struct RxS {
    wire::Speech s;
    std::uint32_t generation;
};
constexpr unsigned kSQueue = 32;
RxS g_squeue[kSQueue];
unsigned g_sq_head = 0, g_sq_count = 0;
volatile LONG64 g_rx_speech = 0, g_tx_speech = 0, g_rx_speech_bad = 0;
// Received save transfer messages (STX1, channel 4; under g_save_lock, taken inside g_tx or alone, never the other
// way round), in delivery order. The sink (SetSaveSink) is called outside every lock.
struct RxSave {
    std::uint8_t bytes[lk::kMaxReliableMessage];
    std::uint16_t size;
    std::uint32_t generation;
};
constexpr unsigned kSaveQueue = 64;
SRWLOCK g_save_lock = SRWLOCK_INIT;
RxSave g_save_q[kSaveQueue];
unsigned g_save_head = 0, g_save_count = 0;
SaveSink g_save_sink = nullptr;
void* g_save_ctx = nullptr;
volatile long g_save_burst = 0;
volatile LONG64 g_rx_save = 0, g_tx_save = 0;
// Received vehicle events (SVM1, channel 3; vehicle_wire.hpp; under g_veh_lock, taken inside g_tx or alone, never the other
// way round), in delivery order, for PopVehicleEvent (game thread).
constexpr unsigned kVehQueue = 64;
SRWLOCK g_veh_lock = SRWLOCK_INIT;
RxSave g_veh_q[kVehQueue];
unsigned g_veh_head = 0, g_veh_count = 0;
volatile LONG64 g_rx_veh_events = 0, g_tx_veh_events = 0;
// Received world events (SWS1, channel 1; world_wire.hpp), the same way (under g_world_lock, taken inside g_tx or alone).
constexpr unsigned kWorldQueue = 64;
SRWLOCK g_world_lock = SRWLOCK_INIT;
RxSave g_world_q[kWorldQueue];
unsigned g_world_head = 0, g_world_count = 0;
volatile LONG64 g_rx_world_events = 0, g_tx_world_events = 0;
// Received assets (STT1, channel 5; texture_wire.hpp), the same way (under g_asset_lock, taken inside g_tx or alone).
constexpr unsigned kAssetQueue = 64;
SRWLOCK g_asset_lock = SRWLOCK_INIT;
RxSave g_asset_q[kAssetQueue];
unsigned g_asset_head = 0, g_asset_count = 0;
volatile LONG64 g_rx_assets = 0, g_tx_assets = 0;
// Received combat messages (SCB1, channel 6; docs/enemy-sync.md), the same way (under g_combat_lock, taken inside g_tx or alone).
constexpr unsigned kCombatQueue = 64;
SRWLOCK g_combat_lock = SRWLOCK_INIT;
RxSave g_combat_q[kCombatQueue];
unsigned g_combat_head = 0, g_combat_count = 0;
volatile LONG64 g_rx_combat = 0, g_tx_combat = 0;
// SOD1 (round 28 O3, odradek_sync.cpp): the channel 6 messages starting "SOD1", their own queue (under g_combat_lock).
constexpr unsigned kOdraQueue = 8;
RxSave g_odra_q[kOdraQueue];
unsigned g_odra_head = 0, g_odra_count = 0;
volatile LONG64 g_rx_odra = 0, g_odra_dropped = 0, g_odra_oversize = 0;
// Received BTS1 (round 28 lane B1, bt_sync.cpp): split out of the combat queue in DrainCombatLocked (SCB1's consumers would count
// them as bad), oldest dropped when full (counted). Under g_bt_lock (taken inside g_combat_lock).
constexpr unsigned kBtQueue = 32;
constexpr std::size_t kBtMax = 256;
struct RxBt {
    std::uint8_t bytes[kBtMax];
    std::uint16_t size;
    std::uint32_t generation;
};
SRWLOCK g_bt_lock = SRWLOCK_INIT;
RxBt g_bt_q[kBtQueue];
unsigned g_bt_head = 0, g_bt_count = 0;
volatile LONG64 g_rx_bt = 0, g_rx_bt_dropped = 0;
// The combat frames (SEH1 the beacon, SEN1 enemy states, SEP1 enemy poses, SFX1 shots; docs/enemy-sync.md): raw bytes per kind
// (the magic and the size bound checked here, the strict decoders run on the game thread), oldest first; when full the oldest
// goes (counted). Queues of 2 / 8 / 16 / 32 / 4 (SEH1, SEN1, SEP1, SFX1, SPV1). Under g_rx.
constexpr std::size_t kCombatFrameMax = 1024;
struct RxC {
    std::uint8_t bytes[kCombatFrameMax];
    std::uint16_t size;
    std::uint32_t generation;
};
constexpr unsigned kCKinds = 5, kCQueueMax = 32;
constexpr unsigned kCQueueCap[kCKinds] = {2, 8, 16, 32, 4};
RxC g_cqueue[kCKinds][kCQueueMax];
unsigned g_cq_head[kCKinds] = {}, g_cq_count[kCKinds] = {};
volatile LONG64 g_rx_cframes[kCKinds] = {}, g_tx_cframes[kCKinds] = {}, g_rx_cdropped[kCKinds] = {};
int CombatKindOf(const std::uint8_t* d, std::size_t n) noexcept {
    if (!d || n < 8 || n > kCombatFrameMax || d[0] != 'S') return -1;
    if (d[1] == 'E' && d[2] == 'H' && d[3] == '1') return n == 24 ? 0 : -1;
    if (d[1] == 'E' && d[2] == 'N' && d[3] == '1') return 1;
    if (d[1] == 'E' && d[2] == 'P' && d[3] == '1') return 2;
    if (d[1] == 'F' && d[2] == 'X' && d[3] == '1') return 3;
    if (d[1] == 'P' && d[2] == 'V' && d[3] == '1') return n == 32 ? 4 : -1;
    return -1;
}
// Hidden tests (TestBlackout): received datagrams are dropped until this NowMs().
volatile LONG64 g_blackout_until = 0, g_blackout_dropped = 0;

// Under g_tx: in-order channel 4 messages into the save queue while it has room (the rest stays in the receiver).
void DrainSaveLocked(std::uint64_t now, std::uint32_t generation) noexcept {
    AcquireSRWLockExclusive(&g_save_lock);
    std::size_t got = 0;
    while (g_save_count < kSaveQueue) {
        RxSave& slot = g_save_q[(g_save_head + g_save_count) % kSaveQueue];
        if (!g_rel_rx[lk::kSaveChannel].Drain(slot.bytes, sizeof(slot.bytes), got, now)) break;
        slot.size = static_cast<std::uint16_t>(got);
        slot.generation = generation;
        ++g_save_count;
        InterlockedIncrement64(&g_rx_save);
    }
    ReleaseSRWLockExclusive(&g_save_lock);
}

// Under g_tx: in-order channel 3 messages into the vehicle event queue while it has room (the rest stays in the receiver).
void DrainVehicleEventsLocked(std::uint64_t now, std::uint32_t generation) noexcept {
    AcquireSRWLockExclusive(&g_veh_lock);
    std::size_t got = 0;
    while (g_veh_count < kVehQueue) {
        RxSave& slot = g_veh_q[(g_veh_head + g_veh_count) % kVehQueue];
        if (!g_rel_rx[kVehicleEventChannel].Drain(slot.bytes, sizeof(slot.bytes), got, now)) break;
        slot.size = static_cast<std::uint16_t>(got);
        slot.generation = generation;
        ++g_veh_count;
        InterlockedIncrement64(&g_rx_veh_events);
    }
    ReleaseSRWLockExclusive(&g_veh_lock);
}

// Under g_tx: in-order channel 1 messages into the world event queue while it has room.
void DrainWorldEventsLocked(std::uint64_t now, std::uint32_t generation) noexcept {
    AcquireSRWLockExclusive(&g_world_lock);
    std::size_t got = 0;
    while (g_world_count < kWorldQueue) {
        RxSave& slot = g_world_q[(g_world_head + g_world_count) % kWorldQueue];
        if (!g_rel_rx[kWorldEventChannel].Drain(slot.bytes, sizeof(slot.bytes), got, now)) break;
        slot.size = static_cast<std::uint16_t>(got);
        slot.generation = generation;
        ++g_world_count;
        InterlockedIncrement64(&g_rx_world_events);
    }
    ReleaseSRWLockExclusive(&g_world_lock);
}

// Under g_tx: in-order channel 5 messages into the asset queue while it has room.
void DrainAssetsLocked(std::uint64_t now, std::uint32_t generation) noexcept {
    AcquireSRWLockExclusive(&g_asset_lock);
    std::size_t got = 0;
    while (g_asset_count < kAssetQueue) {
        RxSave& slot = g_asset_q[(g_asset_head + g_asset_count) % kAssetQueue];
        if (!g_rel_rx[lk::kAssetChannel].Drain(slot.bytes, sizeof(slot.bytes), got, now)) break;
        slot.size = static_cast<std::uint16_t>(got);
        slot.generation = generation;
        ++g_asset_count;
        InterlockedIncrement64(&g_rx_assets);
    }
    ReleaseSRWLockExclusive(&g_asset_lock);
}

// Under g_tx: in-order channel 6 messages into the combat queue while it has room.
void DrainCombatLocked(std::uint64_t now, std::uint32_t generation) noexcept {
    AcquireSRWLockExclusive(&g_combat_lock);
    std::size_t got = 0;
    while (g_combat_count < kCombatQueue) {
        RxSave& slot = g_combat_q[(g_combat_head + g_combat_count) % kCombatQueue];
        if (!g_rel_rx[lk::kCombatChannel].Drain(slot.bytes, sizeof(slot.bytes), got, now)) break;
        // Round 28: SOD1 (O3, the Odradek scan sync) and BTS1 (B1, the BT field sync) share channel 6 with SCB1 but go to queues
        // of their own (SCB1's consumers would count them as bad); the combat slot is not committed and is reused.
        if (got >= 4 && std::memcmp(slot.bytes, "SOD1", 4) == 0) {
            if (got > kOdradekMax) {  // larger than any SOD1 this build knows (a malformed or newer one): dropped, counted
                InterlockedIncrement64(&g_odra_dropped);
                InterlockedIncrement64(&g_odra_oversize);
                continue;
            }
            if (g_odra_count == kOdraQueue) {
                g_odra_head = (g_odra_head + 1) % kOdraQueue;
                --g_odra_count;
                InterlockedIncrement64(&g_odra_dropped);
            }
            RxSave& o = g_odra_q[(g_odra_head + g_odra_count) % kOdraQueue];
            std::memcpy(o.bytes, slot.bytes, got);
            o.size = static_cast<std::uint16_t>(got);
            o.generation = generation;
            ++g_odra_count;
            InterlockedIncrement64(&g_rx_odra);
            continue;
        }
        if (got >= 12 && got <= kBtMax && std::memcmp(slot.bytes, "BTS1", 4) == 0) {
            AcquireSRWLockExclusive(&g_bt_lock);
            if (g_bt_count == kBtQueue) {
                g_bt_head = (g_bt_head + 1) % kBtQueue;
                --g_bt_count;
                InterlockedIncrement64(&g_rx_bt_dropped);
            }
            RxBt& b = g_bt_q[(g_bt_head + g_bt_count) % kBtQueue];
            std::memcpy(b.bytes, slot.bytes, got);
            b.size = static_cast<std::uint16_t>(got);
            b.generation = generation;
            ++g_bt_count;
            ReleaseSRWLockExclusive(&g_bt_lock);
            InterlockedIncrement64(&g_rx_bt);
            continue;
        }
        slot.size = static_cast<std::uint16_t>(got);
        slot.generation = generation;
        ++g_combat_count;
        InterlockedIncrement64(&g_rx_combat);
    }
    ReleaseSRWLockExclusive(&g_combat_lock);
}

// Outside every lock: hand the queued save messages to the sink, if one is registered.
void DeliverSaves() noexcept {
    for (;;) {
        static thread_local RxSave item;  // transport thread
        SaveSink sink = nullptr;
        void* ctx = nullptr;
        AcquireSRWLockExclusive(&g_save_lock);
        if (g_save_sink && g_save_count) {
            sink = g_save_sink;
            ctx = g_save_ctx;
            item = g_save_q[g_save_head];
            g_save_head = (g_save_head + 1) % kSaveQueue;
            --g_save_count;
        }
        ReleaseSRWLockExclusive(&g_save_lock);
        if (!sink) return;
        sink(ctx, item.bytes, item.size, item.generation);
    }
}
// Received sound event frames (STA1, under g_rx): newest-seq filter (u16, wrap-safe), oldest dropped when full.
struct RxA {
    wire::SoundFrame f;
    std::uint32_t generation;
};
constexpr unsigned kAQueue = 16;
RxA g_aqueue[kAQueue];
unsigned g_aq_head = 0, g_aq_count = 0;
std::uint16_t g_last_rx_aseq = 0;
std::uint64_t g_rx_aseen = 0;  // bit k: seq (newest - k) was taken
bool g_have_rx_aseq = false;
volatile LONG64 g_rx_sound = 0, g_tx_sound = 0, g_tx_sound_alone = 0;

volatile LONG64 g_rx_frames = 0, g_tx_frames = 0;
volatile LONG64 g_rx_joints = 0, g_tx_joints = 0, g_tx_payload = 0;
volatile LONG64 g_rx_names = 0, g_tx_names = 0;
volatile LONG64 g_rx_gears = 0, g_tx_gears = 0;
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
    constexpr unsigned kMaxFrames = 32, kMaxEvents = 16, kMaxJoints = 2;
    wire::Pose poses[kMaxFrames];
    unsigned pose_count = 0;
    static thread_local wire::JointFrame joints[kMaxJoints];  // transport thread only
    unsigned joint_count = 0;
    lk::Event events[kMaxEvents];
    unsigned event_count = 0;
    const std::uint64_t now = NowMs();
    if (static_cast<LONG64>(now) < InterlockedCompareExchange64(&g_blackout_until, 0, 0)) {
        InterlockedIncrement64(&g_blackout_dropped);  // hidden tests: a link outage on this side
        return;
    }
    bool new_session = false;
    lab::Endpoint committed{};
    wire::NameFrame name;
    bool have_name = false;
    static thread_local wire::GearFrame gear;  // transport thread only
    bool have_gear = false;
    constexpr unsigned kMaxRes = 4;
    static thread_local wire::GearRes res[kMaxRes];
    unsigned res_count = 0;
    constexpr unsigned kMaxLooks = 4;
    static thread_local wire::GearLook looks[kMaxLooks];
    unsigned look_count = 0;
    static thread_local wire::GearPose gpose;
    bool have_gpose = false;
    constexpr unsigned kMaxSpeech = 16;
    static thread_local wire::Speech speech[kMaxSpeech];
    unsigned speech_count = 0;
    constexpr unsigned kMaxSound = 2;
    static thread_local wire::SoundFrame sound[kMaxSound];  // transport thread only
    unsigned sound_count = 0;
    static thread_local wire::VehicleFrame vehicle;  // transport thread only
    bool have_vehicle = false;
    constexpr unsigned kMaxDrive = 16;
    static thread_local RxD drive[kMaxDrive];  // transport thread only
    unsigned drive_count = 0;
    constexpr unsigned kMaxCombat = 8;
    static thread_local RxC combat[kMaxCombat];  // transport thread only
    static thread_local int combat_kind[kMaxCombat];
    unsigned combat_count = 0;

    AcquireSRWLockExclusive(&g_tx);
    const lk::Delivery d = g_session.OnDatagram(data, size, from, now);
    new_session = d.new_session;
    if (new_session) {
        g_name_sends = 0;  // the new partner gets our name with the next sends
        for (auto& t : g_rel_tx) t.Reset();
        for (auto& r : g_rel_rx) r.Reset();
    }
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
                } else if (joint_count < kMaxJoints && wire::DecodeJoints(m.data, m.size, joints[joint_count])) {
                    ++joint_count;
                } else if (wire::DecodeName(m.data, m.size, name)) {
                    have_name = true;  // the newest in the datagram wins
                } else if (wire::DecodeGear(m.data, m.size, gear)) {
                    have_gear = true;
                } else if (res_count < kMaxRes && wire::DecodeGearRes(m.data, m.size, res[res_count])) {
                    ++res_count;
                } else if (look_count < kMaxLooks && wire::DecodeGearLook(m.data, m.size, looks[look_count])) {
                    ++look_count;
                } else if (wire::DecodeGearPose(m.data, m.size, gpose)) {
                    have_gpose = true;
                } else if (sound_count < kMaxSound && wire::DecodeSound(m.data, m.size, sound[sound_count])) {
                    ++sound_count;
                } else if (wire::DecodeVehicle(m.data, m.size, vehicle)) {
                    have_vehicle = true;  // the newest in the datagram wins
                } else if (LooksLikeDriveFrame(m.data, m.size)) {
                    if (drive_count < kMaxDrive) {
                        std::memcpy(drive[drive_count].bytes, m.data, m.size);
                        drive[drive_count].size = static_cast<std::uint8_t>(m.size);
                        ++drive_count;
                    } else {
                        InterlockedIncrement64(&g_rx_drive_dropped);
                    }
                } else if (const int ck = CombatKindOf(m.data, m.size); ck >= 0) {
                    if (combat_count < kMaxCombat) {
                        std::memcpy(combat[combat_count].bytes, m.data, m.size);
                        combat[combat_count].size = static_cast<std::uint16_t>(m.size);
                        combat_kind[combat_count] = ck;
                        ++combat_count;
                    } else {
                        InterlockedIncrement64(&g_rx_cdropped[ck]);
                    }
                }
            } else if (m.type == static_cast<std::uint8_t>(lk::MsgType::reliable)) {
                lk::ReliableMsg r{};
                if (lk::DecodeReliable(m, r) && r.channel >= 1 && r.channel < lk::kReliableChannels)
                    g_rel_rx[r.channel].Accept(r.id, r.payload, r.size, now);
            } else if (m.type == static_cast<std::uint8_t>(lk::MsgType::relack)) {
                lk::RelAckMsg a{};
                if (lk::DecodeRelAck(m, a) && a.channel >= 1 && a.channel < lk::kReliableChannels)
                    g_rel_tx[a.channel].OnRelAck(a.next_expected, a.bits, a.free_slots, now);
            } else if (m.type == static_cast<std::uint8_t>(lk::MsgType::close)) {
                g_session.OnPeerClose(now);
            }
        }
        if (any_pose) g_session.jitter().OnPacket(static_cast<std::uint32_t>(now), d.send_ms, now);
    }
    // In-order reliable messages of the speech channel; anything that is not a valid STS1 is dropped
    // (it was delivered, so it is acknowledged either way).
    {
        std::uint8_t buf[lk::kMaxReliableMessage];
        std::size_t got = 0;
        // At most kMaxSpeech per pump: the rest stays in the receiver for the next one (never dropped unseen).
        while (speech_count < kMaxSpeech && g_rel_rx[wire::kSpeechChannel].Drain(buf, sizeof(buf), got, now)) {
            if (wire::DecodeSpeech(buf, got, speech[speech_count])) ++speech_count;
            else InterlockedIncrement64(&g_rx_speech_bad);
        }
        // Channel 4: the save transfer (STX1), queued for its reader. The generation is the session's (bumped below
        // under g_rx for a new one; this thread is its only writer).
        if (new_session) {
            AcquireSRWLockExclusive(&g_save_lock);
            g_save_count = 0;
            ReleaseSRWLockExclusive(&g_save_lock);
        }
        DrainSaveLocked(now, g_generation + (new_session ? 1u : 0u));
        // Channel 3: the vehicle events (SVM1, vehicle_share.cpp), queued for the game thread the same way.
        if (new_session) {
            AcquireSRWLockExclusive(&g_veh_lock);
            g_veh_count = 0;
            ReleaseSRWLockExclusive(&g_veh_lock);
        }
        DrainVehicleEventsLocked(now, g_generation + (new_session ? 1u : 0u));
        // Channel 1: the world events (SWS1, world_share.cpp), the same way.
        if (new_session) {
            AcquireSRWLockExclusive(&g_world_lock);
            g_world_count = 0;
            ReleaseSRWLockExclusive(&g_world_lock);
        }
        DrainWorldEventsLocked(now, g_generation + (new_session ? 1u : 0u));
        // Channel 5: the assets (STT1, texture_share.cpp), the same way.
        if (new_session) {
            AcquireSRWLockExclusive(&g_asset_lock);
            g_asset_count = 0;
            ReleaseSRWLockExclusive(&g_asset_lock);
        }
        DrainAssetsLocked(now, g_generation + (new_session ? 1u : 0u));
        // Channel 6: the combat messages (SCB1, the enemy rounds), the same way.
        if (new_session) {
            AcquireSRWLockExclusive(&g_combat_lock);
            g_combat_count = 0;
            g_odra_count = 0;  // round 28 O3: SOD1
            AcquireSRWLockExclusive(&g_bt_lock);  // round 28 B1: BTS1
            g_bt_count = 0;
            g_bt_head = 0;
            ReleaseSRWLockExclusive(&g_bt_lock);
            ReleaseSRWLockExclusive(&g_combat_lock);
        }
        DrainCombatLocked(now, g_generation + (new_session ? 1u : 0u));
        // Other channels are not used by this build: drained and dropped so their windows never fill.
        for (unsigned ch = 1; ch < lk::kReliableChannels; ++ch)
            if (ch != wire::kSpeechChannel && ch != lk::kSaveChannel && ch != kVehicleEventChannel && ch != kWorldEventChannel && ch != lk::kAssetChannel &&
                ch != lk::kCombatChannel)
                while (g_rel_rx[ch].Drain(buf, sizeof(buf), got, now)) {
                }
    }
    lk::Event e{};
    while (event_count < kMaxEvents && g_session.PopEvent(e)) events[event_count++] = e;
    InterlockedExchange(&g_state, static_cast<long>(g_session.state()));
    committed = g_session.peer();
    ReleaseSRWLockExclusive(&g_tx);

    if (g_use_steam && new_session && g_host && committed.address) g_steam.set_peer(lab::SteamIdOf(committed));

    if (pose_count || joint_count || have_name || have_gear || res_count || look_count || have_gpose || speech_count || sound_count || have_vehicle ||
        drive_count || combat_count || new_session) {
        AcquireSRWLockExclusive(&g_rx);
        if (new_session) {
            for (unsigned k = 0; k < kCKinds; ++k) g_cq_count[k] = 0;
            g_dq_count = 0;
            g_mq_count = 0;
            g_have_rx_mseq = false;
            g_aq_count = 0;
            g_have_rx_aseq = false;
            g_sq_count = 0;
            g_q_count = 0;
            g_have_rx_seq = false;
            g_jq_count = 0;
            g_have_rx_jseq = false;
            g_have_peer_name = false;
            g_gq_count = 0;
            g_have_rx_gseq = false;
            g_rq_count = 0;
            g_vq_count = 0;
            g_kq_count = 0;
            ++g_generation;
        }
        for (unsigned i = 0; i < sound_count; ++i) {
            const wire::SoundFrame& f = sound[i];
            // Duplicates (and frames older than 64 seqs) are dropped; a reordered frame is kept (its stops matter):
            // a bitmap of the seqs seen behind the newest one.
            if (g_have_rx_aseq) {
                const int diff = static_cast<std::int16_t>(static_cast<std::uint16_t>(f.seq - g_last_rx_aseq));
                if (diff > 0) {
                    g_rx_aseen = diff >= 64 ? 1ull : (g_rx_aseen << diff) | 1ull;
                    g_last_rx_aseq = f.seq;
                } else {
                    const int back = -diff;
                    if (back >= 64 || (g_rx_aseen >> back) & 1ull) continue;
                    g_rx_aseen |= 1ull << back;
                }
            } else {
                g_last_rx_aseq = f.seq;
                g_rx_aseen = 1;
            }
            g_have_rx_aseq = true;
            RxA& slot = g_aqueue[(g_aq_head + g_aq_count) % kAQueue];
            slot.f = f;
            slot.generation = g_generation;
            if (g_aq_count < kAQueue) ++g_aq_count;
            else g_aq_head = (g_aq_head + 1) % kAQueue;
            InterlockedIncrement64(&g_rx_sound);
        }
        for (unsigned i = 0; i < speech_count; ++i) {
            RxS& slot = g_squeue[(g_sq_head + g_sq_count) % kSQueue];
            slot.s = speech[i];
            slot.generation = g_generation;
            if (g_sq_count < kSQueue) ++g_sq_count;
            else g_sq_head = (g_sq_head + 1) % kSQueue;
            InterlockedIncrement64(&g_rx_speech);
        }
        for (unsigned i = 0; i < look_count; ++i) {
            RxV& slot = g_vqueue[(g_vq_head + g_vq_count) % kVQueue];
            slot.l = looks[i];
            slot.generation = g_generation;
            if (g_vq_count < kVQueue) ++g_vq_count;
            else g_vq_head = (g_vq_head + 1) % kVQueue;
        }
        if (have_vehicle && !(g_have_rx_mseq && static_cast<std::int32_t>(vehicle.seq - g_last_rx_mseq) <= 0)) {
            g_last_rx_mseq = vehicle.seq;
            g_have_rx_mseq = true;
            RxM& slot = g_mqueue[(g_mq_head + g_mq_count) % kMQueue];
            slot.f = vehicle;
            slot.generation = g_generation;
            if (g_mq_count < kMQueue) ++g_mq_count;
            else g_mq_head = (g_mq_head + 1) % kMQueue;
            InterlockedIncrement64(&g_rx_vehicle);
        }
        for (unsigned i = 0; i < combat_count; ++i) {
            const unsigned k = static_cast<unsigned>(combat_kind[i]), cap = kCQueueCap[k];
            RxC& slot = g_cqueue[k][(g_cq_head[k] + g_cq_count[k]) % cap];
            std::memcpy(slot.bytes, combat[i].bytes, combat[i].size);
            slot.size = combat[i].size;
            slot.generation = g_generation;
            if (g_cq_count[k] < cap) ++g_cq_count[k];
            else g_cq_head[k] = (g_cq_head[k] + 1) % cap, InterlockedIncrement64(&g_rx_cdropped[k]);
            InterlockedIncrement64(&g_rx_cframes[k]);
        }
        for (unsigned i = 0; i < drive_count; ++i) {
            RxD& slot = g_dqueue[(g_dq_head + g_dq_count) % kDQueue];
            slot = drive[i];
            slot.generation = g_generation;
            if (g_dq_count < kDQueue) ++g_dq_count;
            else g_dq_head = (g_dq_head + 1) % kDQueue, InterlockedIncrement64(&g_rx_drive_dropped);
            InterlockedIncrement64(&g_rx_drive);
        }
        if (have_gpose) {
            RxK& slot = g_kqueue[(g_kq_head + g_kq_count) % kKQueue];
            slot.p = gpose;
            slot.generation = g_generation;
            if (g_kq_count < kKQueue) ++g_kq_count;
            else g_kq_head = (g_kq_head + 1) % kKQueue;
        }
        for (unsigned i = 0; i < res_count; ++i) {
            RxR& slot = g_rqueue[(g_rq_head + g_rq_count) % kRQueue];
            slot.r = res[i];
            slot.generation = g_generation;
            if (g_rq_count < kRQueue) ++g_rq_count;
            else g_rq_head = (g_rq_head + 1) % kRQueue;
        }
        if (have_gear && !(g_have_rx_gseq && static_cast<std::int32_t>(gear.seq - g_last_rx_gseq) <= 0)) {
            g_last_rx_gseq = gear.seq;
            g_have_rx_gseq = true;
            RxG& slot = g_gqueue[(g_gq_head + g_gq_count) % kGQueue];
            slot.f = gear;
            slot.ms = now;
            slot.generation = g_generation;
            if (g_gq_count < kGQueue) ++g_gq_count;
            else g_gq_head = (g_gq_head + 1) % kGQueue;
            InterlockedIncrement64(&g_rx_gears);
        }
        if (have_name) {
            g_peer_name = name;
            g_have_peer_name = true;
            g_peer_name_generation = g_generation;
            InterlockedIncrement64(&g_rx_names);
        }
        for (unsigned i = 0; i < joint_count; ++i) {
            const wire::JointFrame& f = joints[i];
            if (g_have_rx_jseq && static_cast<std::int32_t>(f.seq - g_last_rx_jseq) <= 0) continue;
            g_last_rx_jseq = f.seq;
            g_have_rx_jseq = true;
            RxJ& slot = g_jqueue[(g_jq_head + g_jq_count) % kJQueue];
            slot.f = f;
            slot.ms = now;
            slot.generation = g_generation;
            if (g_jq_count < kJQueue) ++g_jq_count;
            else g_jq_head = (g_jq_head + 1) % kJQueue;
            InterlockedIncrement64(&g_rx_joints);
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
    DeliverSaves();
    LogEvents(events, event_count);
}

void OnIdle(void*) noexcept {
    constexpr unsigned kMaxEvents = 16;
    lk::Event events[kMaxEvents];
    unsigned event_count = 0;
    const std::uint64_t now = NowMs();
    AcquireSRWLockExclusive(&g_tx);
    // The reliable channel (as the link harness drives it): due first transmissions and retransmissions
    // (within the per-tick budget) and the receivers' acknowledgements, in small DATA packets of their own.
    if (g_session.CanSendData()) {
        std::size_t budget = lk::kRetransmitBudgetBytes;
        const std::uint32_t rto = g_session.rtt().rto_ms();
        // 4 packets per tick; up to 16 while the save transfer has data queued at the title / loading screen.
        const unsigned cap = lk::ReliablePacketsPerTick(!g_rel_tx[lk::kSaveChannel].idle(), InterlockedCompareExchange(&g_save_burst, 0, 0) != 0);
        for (unsigned packets = 0; packets < cap; ++packets) {
            const bool due = lk::ReliableDue(g_rel_tx, now, rto, budget);
            lk::RelAckState acks[lk::kReliableChannels];
            bool any_ack = false;
            for (unsigned ch = 0; ch < lk::kReliableChannels; ++ch) {
                acks[ch] = lk::RelAckState{};
                lk::ReliableReceiver& rx = g_rel_rx[ch];
                if (ch == 0 || !rx.dirty() || (!due && now - rx.dirty_since() < lk::kAckDelayMs)) continue;
                acks[ch].due = true;
                rx.AckState(acks[ch].next_expected, acks[ch].bits, acks[ch].free_slots);
                rx.ClearDirty();
                any_ack = true;
            }
            if (!due && !any_ack) break;
            lk::BodyWriter w(g_body, sizeof(g_body));
            lk::PackBody(w, nullptr, 0, acks, g_rel_tx, now, rto, budget);
            if (!w.size() || !g_session.SendData(g_body, w.size(), now)) break;
        }
    }
    g_session.OnTimer(now);
    // Save messages left in the channel while the queue was full: taken as soon as it has room again.
    DrainSaveLocked(now, g_generation);
    DrainAssetsLocked(now, g_generation);
    DrainCombatLocked(now, g_generation);
    lk::Event e{};
    while (event_count < kMaxEvents && g_session.PopEvent(e)) events[event_count++] = e;
    InterlockedExchange(&g_state, static_cast<long>(g_session.state()));
    ReleaseSRWLockExclusive(&g_tx);
    DeliverSaves();
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

void AllowTestImpairment(bool on) noexcept { g_test_impair_allowed = on; }

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
        if (c.test_net_latency_ms || c.test_net_jitter_ms || c.test_net_loss_permille) Log("[link] test_net_* ignored (the UDP link only)");
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
        const bool impair = c.test_net_latency_ms || c.test_net_jitter_ms || c.test_net_loss_permille;
        if (impair && g_test_impair_allowed) {
            // hidden tests only: this game's sends go through the transport's lab impairment (play test 3: the vehicle sync
            // over a simulated internet)
            t.impair.latency_ms = c.test_net_latency_ms;
            t.impair.jitter_ms = c.test_net_jitter_ms;
            t.impair.loss_permille = c.test_net_loss_permille;
            t.impair.burst_model = c.test_net_burst > 0;
            t.impair.burst = c.test_net_burst > 0 ? c.test_net_burst : 2;
            t.impair.seed = g_host ? 12345u : 54321u;
            Log("[link] TEST impairment on this game's sends: +%u ms, jitter 0..%u ms, loss %u per mille%s", c.test_net_latency_ms, c.test_net_jitter_ms,
                c.test_net_loss_permille, t.impair.burst_model ? " (bursts)" : "");
        } else if (impair) {
            Log("[link] test_net_* ignored (hidden runs only)");
        }
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

namespace {
// g_tx held. Adds our STN1 frame to a pose datagram when the schedule says so: the first 10
// datagrams of a session, every 30th after that (~1 s at send_hz=30), and the next one after
// the name changed. True when it was added (counted once the datagram is sent).
bool AddNameFrame(lk::BodyWriter& w) noexcept {
    if (!g_name_send) return false;
    const std::uint32_t n = g_name_sends;
    if (!(n < 10 || n % 30 == 0 || g_name_version != g_name_sent_version)) return false;
    wire::NameFrame f;
    f.flags = g_host ? wire::kNameFromHost : 0;
    f.len = g_local_name_len;
    std::memcpy(f.name, g_local_name, sizeof(f.name));
    std::uint8_t buf[wire::kMaxNameFrameSize];
    const std::size_t size = wire::EncodeName(f, buf, sizeof(buf));
    if (!size || !w.Add(lk::MsgType::frame, buf, size)) return false;  // no room: the next datagram tries again
    g_name_sent_version = g_name_version;
    return true;
}
}  // namespace

void SetLocalName(const char* utf8, bool send) noexcept {
    char clean[wire::kMaxNameBytes + 1];
    const std::size_t len = wire::SanitizeName(utf8 ? utf8 : "", utf8 ? std::strlen(utf8) : 0, clean, sizeof(clean));
    AcquireSRWLockExclusive(&g_tx);
    if (send != g_name_send || len != g_local_name_len || std::memcmp(clean, g_local_name, len) != 0) {
        std::memcpy(g_local_name, clean, len + 1);
        g_local_name_len = static_cast<std::uint8_t>(len);
        g_name_send = send;
        ++g_name_version;
    }
    ReleaseSRWLockExclusive(&g_tx);
}

bool PeerName(char* out, std::size_t cap, std::uint32_t* generation, bool* from_host) noexcept {
    if (!out || !cap) return false;
    out[0] = 0;
    bool ok = false;
    AcquireSRWLockShared(&g_rx);
    if (g_have_peer_name) {
        std::snprintf(out, cap, "%s", g_peer_name.name);
        if (generation) *generation = g_peer_name_generation;
        if (from_host) *from_host = (g_peer_name.flags & wire::kNameFromHost) != 0;
        ok = true;
    }
    ReleaseSRWLockShared(&g_rx);
    return ok;
}

namespace {
// g_tx held, a datagram sent: the STA1 frame that did not fit into it goes in one of its own.
void SendSoundAlone(const std::uint8_t* abuf, std::size_t an) noexcept {
    lk::BodyWriter w(g_body, sizeof(g_body));
    if (an && w.Add(lk::MsgType::frame, abuf, an) && g_session.SendData(g_body, w.size(), NowMs())) {
        InterlockedAdd64(&g_tx_payload, static_cast<LONG64>(w.size()));
        InterlockedIncrement64(&g_tx_sound);
        InterlockedIncrement64(&g_tx_sound_alone);
    }
}
}  // namespace

bool SendPose(const wire::Pose& pose, const wire::SoundFrame* sound) noexcept {
    std::uint8_t frame[wire::kPoseFrameSize];
    const std::size_t n = wire::EncodePose(pose, frame);
    std::uint8_t abuf[wire::kMaxSoundFrameSize];
    const std::size_t an = sound ? wire::EncodeSound(*sound, abuf, sizeof(abuf)) : 0;
    bool sent = false, with_name = false, with_sound = false;
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData()) {
        lk::BodyWriter w(g_body, sizeof(g_body));
        if (w.Add(lk::MsgType::frame, frame, n)) {
            with_sound = an && w.Add(lk::MsgType::frame, abuf, an);
            with_name = AddNameFrame(w);
            sent = g_session.SendData(g_body, w.size(), NowMs());
        }
        if (sent) {
            InterlockedAdd64(&g_tx_payload, static_cast<LONG64>(w.size()));
            ++g_name_sends;
            if (an && !with_sound) SendSoundAlone(abuf, an);
        }
    }
    ReleaseSRWLockExclusive(&g_tx);
    if (sent) InterlockedIncrement64(&g_tx_frames);
    if (sent && with_name) InterlockedIncrement64(&g_tx_names);
    if (sent && with_sound) InterlockedIncrement64(&g_tx_sound);
    return sent;
}

bool SendPoseAndJoints(const wire::Pose& pose, const wire::JointFrame& joints, const wire::SoundFrame* sound) noexcept {
    std::uint8_t frame[wire::kPoseFrameSize];
    const std::size_t n = wire::EncodePose(pose, frame);
    std::uint8_t jbuf[wire::kMaxJointFrameSize];
    const std::size_t jn = wire::EncodeJoints(joints, jbuf, sizeof(jbuf));
    std::uint8_t abuf[wire::kMaxSoundFrameSize];
    const std::size_t an = sound ? wire::EncodeSound(*sound, abuf, sizeof(abuf)) : 0;
    bool sent = false, with_joints = false, with_name = false, with_sound = false;
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData()) {
        lk::BodyWriter w(g_body, sizeof(g_body));
        if (w.Add(lk::MsgType::frame, frame, n)) {
            with_joints = jn && w.Add(lk::MsgType::frame, jbuf, jn);  // no room: the pose alone still goes
            with_sound = an && w.Add(lk::MsgType::frame, abuf, an);   // no room: a datagram of its own below
            with_name = AddNameFrame(w);
            sent = g_session.SendData(g_body, w.size(), NowMs());
            if (sent) {
                InterlockedAdd64(&g_tx_payload, static_cast<LONG64>(w.size()));
                ++g_name_sends;
                if (an && !with_sound) SendSoundAlone(abuf, an);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_tx);
    if (sent) InterlockedIncrement64(&g_tx_frames);
    if (sent && with_joints) InterlockedIncrement64(&g_tx_joints);
    if (sent && with_name) InterlockedIncrement64(&g_tx_names);
    if (sent && with_sound) InterlockedIncrement64(&g_tx_sound);
    return sent;
}

bool PopSound(wire::SoundFrame& out, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_rx);
    if (g_aq_count) {
        out = g_aqueue[g_aq_head].f;
        generation = g_aqueue[g_aq_head].generation;
        g_aq_head = (g_aq_head + 1) % kAQueue;
        --g_aq_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_rx);
    return ok;
}

bool SendGear(const wire::GearFrame& gear) noexcept {
    std::uint8_t buf[wire::kMaxGearFrameSize];
    const std::size_t n = wire::EncodeGear(gear, buf, sizeof(buf));
    if (!n) return false;
    bool sent = false;
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData()) {
        lk::BodyWriter w(g_body, sizeof(g_body));
        if (w.Add(lk::MsgType::frame, buf, n)) {
            sent = g_session.SendData(g_body, w.size(), NowMs());
            if (sent) InterlockedAdd64(&g_tx_payload, static_cast<LONG64>(w.size()));
        }
    }
    ReleaseSRWLockExclusive(&g_tx);
    if (sent) InterlockedIncrement64(&g_tx_gears);
    return sent;
}

bool SendGearRes(const wire::GearRes& res) noexcept {
    std::uint8_t buf[wire::kMaxGearResFrameSize];
    const std::size_t n = wire::EncodeGearRes(res, buf, sizeof(buf));
    if (!n) return false;
    bool sent = false;
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData()) {
        lk::BodyWriter w(g_body, sizeof(g_body));
        if (w.Add(lk::MsgType::frame, buf, n)) {
            sent = g_session.SendData(g_body, w.size(), NowMs());
            if (sent) InterlockedAdd64(&g_tx_payload, static_cast<LONG64>(w.size()));
        }
    }
    ReleaseSRWLockExclusive(&g_tx);
    return sent;
}

namespace {
bool SendFrameBytes(const std::uint8_t* buf, std::size_t n) noexcept {
    if (!n) return false;
    bool sent = false;
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData()) {
        lk::BodyWriter w(g_body, sizeof(g_body));
        if (w.Add(lk::MsgType::frame, buf, n)) {
            sent = g_session.SendData(g_body, w.size(), NowMs());
            if (sent) InterlockedAdd64(&g_tx_payload, static_cast<LONG64>(w.size()));
        }
    }
    ReleaseSRWLockExclusive(&g_tx);
    return sent;
}
}  // namespace

bool SendGearLook(const wire::GearLook& look) noexcept {
    std::uint8_t buf[wire::kMaxGearLookFrameSize];
    return SendFrameBytes(buf, wire::EncodeGearLook(look, buf, sizeof(buf)));
}

bool SendGearPose(const wire::GearPose& pose) noexcept {
    std::uint8_t buf[wire::kMaxGearPoseFrameSize];
    return SendFrameBytes(buf, wire::EncodeGearPose(pose, buf, sizeof(buf)));
}

bool SendVehicle(const wire::VehicleFrame& f) noexcept {
    std::uint8_t buf[wire::kMaxVehicleFrameSize];
    const bool sent = SendFrameBytes(buf, wire::EncodeVehicle(f, buf, sizeof(buf)));
    if (sent) InterlockedIncrement64(&g_tx_vehicle);
    return sent;
}

bool PopVehicle(wire::VehicleFrame& out, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_rx);
    if (g_mq_count) {
        out = g_mqueue[g_mq_head].f;
        generation = g_mqueue[g_mq_head].generation;
        g_mq_head = (g_mq_head + 1) % kMQueue;
        --g_mq_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_rx);
    return ok;
}

bool SendDrive(const std::uint8_t* bytes, std::size_t size) noexcept {
    if (!LooksLikeDriveFrame(bytes, size)) return false;
    const bool sent = SendFrameBytes(bytes, size);
    if (sent) InterlockedIncrement64(&g_tx_drive);
    return sent;
}

bool PopDrive(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_rx);
    if (g_dq_count && out && g_dqueue[g_dq_head].size <= cap) {
        const RxD& d = g_dqueue[g_dq_head];
        std::memcpy(out, d.bytes, d.size);
        size = d.size;
        generation = d.generation;
        g_dq_head = (g_dq_head + 1) % kDQueue;
        --g_dq_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_rx);
    return ok;
}

void DriveCounts(std::uint64_t* sent, std::uint64_t* received, std::uint64_t* dropped) noexcept {
    if (sent) *sent = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_tx_drive, 0, 0));
    if (received) *received = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_rx_drive, 0, 0));
    if (dropped) *dropped = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_rx_drive_dropped, 0, 0));
}

void VehicleCounts(std::uint64_t* sent, std::uint64_t* received) noexcept {
    if (sent) *sent = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_tx_vehicle, 0, 0));
    if (received) *received = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_rx_vehicle, 0, 0));
}

bool PopGearLook(wire::GearLook& out, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_rx);
    if (g_vq_count) {
        out = g_vqueue[g_vq_head].l;
        generation = g_vqueue[g_vq_head].generation;
        g_vq_head = (g_vq_head + 1) % kVQueue;
        --g_vq_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_rx);
    return ok;
}

bool PopGearPose(wire::GearPose& out, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_rx);
    if (g_kq_count) {
        out = g_kqueue[g_kq_head].p;
        generation = g_kqueue[g_kq_head].generation;
        g_kq_head = (g_kq_head + 1) % kKQueue;
        --g_kq_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_rx);
    return ok;
}

bool SendSpeech(const wire::Speech& s) noexcept {
    std::uint8_t buf[wire::kMaxSpeechFrameSize];
    const std::size_t n = wire::EncodeSpeech(s, buf, sizeof(buf));
    if (!n) return false;
    bool queued = false;
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData())
        queued = g_rel_tx[wire::kSpeechChannel].Queue(buf, n, lk::ReliableMode::append) == lk::ReliableResult::queued;
    ReleaseSRWLockExclusive(&g_tx);
    if (queued) {
        InterlockedIncrement64(&g_tx_speech);
        InterlockedAdd64(&g_tx_payload, static_cast<LONG64>(n));
    }
    return queued;  // the transport thread puts it on the wire within one idle tick (20 ms)
}

bool SendSave(const void* bytes, std::size_t size) noexcept {
    if (!bytes || size == 0 || size > lk::kMaxReliableMessage) return false;
    bool queued = false;
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData())
        queued = g_rel_tx[lk::kSaveChannel].Queue(bytes, size, lk::ReliableMode::append) == lk::ReliableResult::queued;
    ReleaseSRWLockExclusive(&g_tx);
    if (queued) InterlockedIncrement64(&g_tx_save);
    return queued;
}

unsigned SaveQueued() noexcept {
    AcquireSRWLockExclusive(&g_tx);
    const unsigned n = g_rel_tx[lk::kSaveChannel].queued();
    ReleaseSRWLockExclusive(&g_tx);
    return n;
}

void SetSaveSink(SaveSink sink, void* ctx) noexcept {
    AcquireSRWLockExclusive(&g_save_lock);
    g_save_sink = sink;
    g_save_ctx = ctx;
    ReleaseSRWLockExclusive(&g_save_lock);
}

bool PopSave(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_save_lock);
    if (g_save_count && out && g_save_q[g_save_head].size <= cap) {
        const RxSave& s = g_save_q[g_save_head];
        std::memcpy(out, s.bytes, s.size);
        size = s.size;
        generation = s.generation;
        g_save_head = (g_save_head + 1) % kSaveQueue;
        --g_save_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_save_lock);
    return ok;
}

bool SendVehicleEvent(const void* bytes, std::size_t size) noexcept {
    if (!bytes || size == 0 || size > lk::kMaxReliableMessage) return false;
    bool queued = false;
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData())
        queued = g_rel_tx[kVehicleEventChannel].Queue(bytes, size, lk::ReliableMode::append) == lk::ReliableResult::queued;
    ReleaseSRWLockExclusive(&g_tx);
    if (queued) {
        InterlockedIncrement64(&g_tx_veh_events);
        InterlockedAdd64(&g_tx_payload, static_cast<LONG64>(size));
    }
    return queued;
}

unsigned VehicleEventsQueued() noexcept {
    AcquireSRWLockExclusive(&g_tx);
    const unsigned n = g_rel_tx[kVehicleEventChannel].queued();
    ReleaseSRWLockExclusive(&g_tx);
    return n;
}

bool PopVehicleEvent(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_veh_lock);
    if (g_veh_count && out && g_veh_q[g_veh_head].size <= cap) {
        const RxSave& s = g_veh_q[g_veh_head];
        std::memcpy(out, s.bytes, s.size);
        size = s.size;
        generation = s.generation;
        g_veh_head = (g_veh_head + 1) % kVehQueue;
        --g_veh_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_veh_lock);
    return ok;
}

void VehicleEventCounts(std::uint64_t* sent, std::uint64_t* received) noexcept {
    if (sent) *sent = static_cast<std::uint64_t>(g_tx_veh_events);
    if (received) *received = static_cast<std::uint64_t>(g_rx_veh_events);
}

bool SendWorldEvent(const void* bytes, std::size_t size) noexcept {
    if (!bytes || size == 0 || size > lk::kMaxReliableMessage) return false;
    bool queued = false;
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData())
        queued = g_rel_tx[kWorldEventChannel].Queue(bytes, size, lk::ReliableMode::append) == lk::ReliableResult::queued;
    ReleaseSRWLockExclusive(&g_tx);
    if (queued) {
        InterlockedIncrement64(&g_tx_world_events);
        InterlockedAdd64(&g_tx_payload, static_cast<LONG64>(size));
    }
    return queued;
}

unsigned WorldEventsQueued() noexcept {
    AcquireSRWLockExclusive(&g_tx);
    const unsigned n = g_rel_tx[kWorldEventChannel].queued();
    ReleaseSRWLockExclusive(&g_tx);
    return n;
}

bool PopWorldEvent(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_world_lock);
    if (g_world_count && out && g_world_q[g_world_head].size <= cap) {
        const RxSave& s = g_world_q[g_world_head];
        std::memcpy(out, s.bytes, s.size);
        size = s.size;
        generation = s.generation;
        g_world_head = (g_world_head + 1) % kWorldQueue;
        --g_world_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_world_lock);
    return ok;
}

void WorldEventCounts(std::uint64_t* sent, std::uint64_t* received) noexcept {
    if (sent) *sent = static_cast<std::uint64_t>(g_tx_world_events);
    if (received) *received = static_cast<std::uint64_t>(g_rx_world_events);
}

bool SendCombat(const void* bytes, std::size_t size) noexcept {
    if (!bytes || size == 0 || size > lk::kMaxReliableMessage) return false;
    bool queued = false;
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData())
        queued = g_rel_tx[lk::kCombatChannel].Queue(bytes, size, lk::ReliableMode::append) == lk::ReliableResult::queued;
    ReleaseSRWLockExclusive(&g_tx);
    if (queued) {
        InterlockedIncrement64(&g_tx_combat);
        InterlockedAdd64(&g_tx_payload, static_cast<LONG64>(size));
    }
    return queued;
}

unsigned CombatQueued() noexcept {
    AcquireSRWLockExclusive(&g_tx);
    const unsigned n = g_rel_tx[lk::kCombatChannel].queued();
    ReleaseSRWLockExclusive(&g_tx);
    return n;
}

bool PopCombat(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_combat_lock);
    if (g_combat_count && out && g_combat_q[g_combat_head].size <= cap) {
        const RxSave& s = g_combat_q[g_combat_head];
        std::memcpy(out, s.bytes, s.size);
        size = s.size;
        generation = s.generation;
        g_combat_head = (g_combat_head + 1) % kCombatQueue;
        --g_combat_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_combat_lock);
    return ok;
}

void CombatChannelCounts(std::uint64_t* sent, std::uint64_t* received) noexcept {
    if (sent) *sent = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_tx_combat, 0, 0));
    if (received) *received = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_rx_combat, 0, 0));
}

bool PopOdradek(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_combat_lock);
    // a head larger than the caller's buffer is dropped (counted), never left to block the queue
    while (g_odra_count && out && g_odra_q[g_odra_head].size > cap) {
        g_odra_head = (g_odra_head + 1) % kOdraQueue;
        --g_odra_count;
        InterlockedIncrement64(&g_odra_dropped);
        InterlockedIncrement64(&g_odra_oversize);
    }
    if (g_odra_count && out) {
        const RxSave& s = g_odra_q[g_odra_head];
        std::memcpy(out, s.bytes, s.size);
        size = s.size;
        generation = s.generation;
        g_odra_head = (g_odra_head + 1) % kOdraQueue;
        --g_odra_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_combat_lock);
    return ok;
}

void OdradekCounts(std::uint64_t* received, std::uint64_t* dropped) noexcept {
    if (received) *received = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_rx_odra, 0, 0));
    if (dropped) *dropped = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_odra_dropped, 0, 0));
}

std::uint64_t OdradekOversize() noexcept { return static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_odra_oversize, 0, 0)); }

bool PopBtMessage(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_bt_lock);
    // a head larger than the caller's buffer is dropped (counted), never left to block the queue
    while (g_bt_count && out && g_bt_q[g_bt_head].size > cap) {
        g_bt_head = (g_bt_head + 1) % kBtQueue;
        --g_bt_count;
        InterlockedIncrement64(&g_rx_bt_dropped);
    }
    if (g_bt_count && out) {
        const RxBt& b = g_bt_q[g_bt_head];
        std::memcpy(out, b.bytes, b.size);
        size = b.size;
        generation = b.generation;
        g_bt_head = (g_bt_head + 1) % kBtQueue;
        --g_bt_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_bt_lock);
    return ok;
}

void BtMessageCounts(std::uint64_t* received, std::uint64_t* dropped) noexcept {
    if (received) *received = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_rx_bt, 0, 0));
    if (dropped) *dropped = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_rx_bt_dropped, 0, 0));
}

bool SendCombatFrame(const std::uint8_t* bytes, std::size_t size) noexcept {
    const int k = CombatKindOf(bytes, size);
    if (k < 0) return false;
    const bool sent = SendFrameBytes(bytes, size);
    if (sent) InterlockedIncrement64(&g_tx_cframes[k]);
    return sent;
}

bool PopCombatFrame(CombatFrame kind, std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept {
    const unsigned k = static_cast<unsigned>(kind);
    if (k >= kCKinds) return false;
    bool ok = false;
    AcquireSRWLockExclusive(&g_rx);
    if (g_cq_count[k] && out && g_cqueue[k][g_cq_head[k]].size <= cap) {
        const RxC& c = g_cqueue[k][g_cq_head[k]];
        std::memcpy(out, c.bytes, c.size);
        size = c.size;
        generation = c.generation;
        g_cq_head[k] = (g_cq_head[k] + 1) % kCQueueCap[k];
        --g_cq_count[k];
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_rx);
    return ok;
}

void CombatCounts(CombatFrame kind, std::uint64_t* sent, std::uint64_t* received, std::uint64_t* dropped) noexcept {
    const unsigned k = static_cast<unsigned>(kind);
    if (k >= kCKinds) return;
    if (sent) *sent = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_tx_cframes[k], 0, 0));
    if (received) *received = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_rx_cframes[k], 0, 0));
    if (dropped) *dropped = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_rx_cdropped[k], 0, 0));
}

void SetSaveBurst(bool title_or_loading) noexcept { InterlockedExchange(&g_save_burst, title_or_loading ? 1 : 0); }

bool SendAsset(const void* bytes, std::size_t size) noexcept {
    if (!bytes || size == 0 || size > lk::kMaxReliableMessage) return false;
    bool queued = false;
    AcquireSRWLockExclusive(&g_tx);
    if (g_session.CanSendData())
        queued = g_rel_tx[lk::kAssetChannel].Queue(bytes, size, lk::ReliableMode::append) == lk::ReliableResult::queued;
    ReleaseSRWLockExclusive(&g_tx);
    if (queued) {
        InterlockedIncrement64(&g_tx_assets);
        InterlockedAdd64(&g_tx_payload, static_cast<LONG64>(size));
    }
    return queued;
}

unsigned AssetsQueued() noexcept {
    AcquireSRWLockExclusive(&g_tx);
    const unsigned n = g_rel_tx[lk::kAssetChannel].queued();
    ReleaseSRWLockExclusive(&g_tx);
    return n;
}

bool SaveBusy() noexcept {
    AcquireSRWLockExclusive(&g_tx);
    const bool busy = !g_rel_tx[lk::kSaveChannel].idle();
    ReleaseSRWLockExclusive(&g_tx);
    return busy;
}

bool PopAsset(std::uint8_t* out, std::size_t cap, std::size_t& size, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_asset_lock);
    if (g_asset_count && out && g_asset_q[g_asset_head].size <= cap) {
        const RxSave& s = g_asset_q[g_asset_head];
        std::memcpy(out, s.bytes, s.size);
        size = s.size;
        generation = s.generation;
        g_asset_head = (g_asset_head + 1) % kAssetQueue;
        --g_asset_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_asset_lock);
    return ok;
}

void AssetCounts(std::uint64_t* sent, std::uint64_t* received) noexcept {
    if (sent) *sent = static_cast<std::uint64_t>(g_tx_assets);
    if (received) *received = static_cast<std::uint64_t>(g_rx_assets);
}

void TestBlackout(std::uint32_t ms) noexcept {
    InterlockedExchange64(&g_blackout_until, ms ? static_cast<LONG64>(NowMs() + ms) : 0);
    Log("[link] test blackout: %s", ms ? "every received datagram is dropped from now on" : "ended");
}

std::uint64_t TestBlackoutDropped() noexcept { return static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_blackout_dropped, 0, 0)); }

bool PopSpeech(wire::Speech& out, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_rx);
    if (g_sq_count) {
        out = g_squeue[g_sq_head].s;
        generation = g_squeue[g_sq_head].generation;
        g_sq_head = (g_sq_head + 1) % kSQueue;
        --g_sq_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_rx);
    return ok;
}

std::uint32_t Generation() noexcept {
    AcquireSRWLockExclusive(&g_rx);
    const std::uint32_t g = g_generation;
    ReleaseSRWLockExclusive(&g_rx);
    return g;
}

bool PopGearRes(wire::GearRes& out, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_rx);
    if (g_rq_count) {
        out = g_rqueue[g_rq_head].r;
        generation = g_rqueue[g_rq_head].generation;
        g_rq_head = (g_rq_head + 1) % kRQueue;
        --g_rq_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_rx);
    return ok;
}

bool PopGear(wire::GearFrame& out, std::uint64_t& receive_ms, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_rx);
    if (g_gq_count) {
        out = g_gqueue[g_gq_head].f;
        receive_ms = g_gqueue[g_gq_head].ms;
        generation = g_gqueue[g_gq_head].generation;
        g_gq_head = (g_gq_head + 1) % kGQueue;
        --g_gq_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_rx);
    return ok;
}

bool PopJoints(wire::JointFrame& out, std::uint64_t& receive_ms, std::uint32_t& generation) noexcept {
    bool ok = false;
    AcquireSRWLockExclusive(&g_rx);
    if (g_jq_count) {
        out = g_jqueue[g_jq_head].f;
        receive_ms = g_jqueue[g_jq_head].ms;
        generation = g_jqueue[g_jq_head].generation;
        g_jq_head = (g_jq_head + 1) % kJQueue;
        --g_jq_count;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_rx);
    return ok;
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
    i.rx_joint_frames = static_cast<std::uint64_t>(g_rx_joints);
    i.tx_joint_frames = static_cast<std::uint64_t>(g_tx_joints);
    i.tx_payload_bytes = static_cast<std::uint64_t>(g_tx_payload);
    i.rx_name_frames = static_cast<std::uint64_t>(g_rx_names);
    i.tx_name_frames = static_cast<std::uint64_t>(g_tx_names);
    i.rx_gear_frames = static_cast<std::uint64_t>(g_rx_gears);
    i.tx_gear_frames = static_cast<std::uint64_t>(g_tx_gears);
    i.rx_speech = static_cast<std::uint64_t>(g_rx_speech);
    i.rx_save = static_cast<std::uint64_t>(g_rx_save);
    i.tx_save = static_cast<std::uint64_t>(g_tx_save);
    i.tx_speech = static_cast<std::uint64_t>(g_tx_speech);
    i.rx_sound = static_cast<std::uint64_t>(g_rx_sound);
    i.tx_sound = static_cast<std::uint64_t>(g_tx_sound);
    i.tx_sound_alone = static_cast<std::uint64_t>(g_tx_sound_alone);

    AcquireSRWLockShared(&g_tx);
    for (const auto& t : g_rel_tx) i.rel_retransmits += t.retransmits();
    ReleaseSRWLockShared(&g_tx);
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
